/* pool.c - fixed buffer (tray) pool with on-demand unbounded trays. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"

enum { UNBOUNDED_INDEX_INITIAL_CAPACITY = 64, MAX_CHUNKS = 48 };

typedef struct tray {
    record_t rec;                 /* first member: record_t * and tray_t * share an address */
    unsigned char *original_buf;  /* pool-owned payload; rec.buf may be repointed by a caller */
    bool out;
    struct tray *owner_next;
    struct tray *free_next;
} tray_t;

/* Positive-count pools retain the existing contiguous tray/payload allocation. */
typedef struct chunk {
    tray_t *trays;
    unsigned char *data;
    size_t n;
} chunk_t;

struct pool {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;

    chunk_t chunks[MAX_CHUNKS];
    size_t nchunks;
    tray_t **free_list;           /* bounded-mode free stack */
    size_t nfree;
    size_t total;

    tray_t *owners;               /* unbounded-mode lifetime registry */
    tray_t *free_head;            /* unbounded-mode available tray list */
    tray_t **index;               /* unbounded-mode open-addressed membership table */
    size_t index_capacity;
    size_t index_count;

    size_t bufsize;
    bool unbounded;
    bool closed;
};

/* Adds a bounded pool chunk and pushes its trays onto the free stack. */
static int pool_add_chunk(pool_t *pool, size_t n)
{
    chunk_t *chunk = &pool->chunks[pool->nchunks];
    tray_t **free_list;

    if (pool->nchunks == MAX_CHUNKS || n > SIZE_MAX / pool->bufsize ||
        n > SIZE_MAX / sizeof(*chunk->trays) ||
        n > SIZE_MAX / sizeof(*free_list) ||
        pool->total > SIZE_MAX / sizeof(*free_list) - n) {
        errno = ENOMEM;
        return -1;
    }

    free_list = realloc(pool->free_list, (pool->total + n) * sizeof(*free_list));
    if (free_list == NULL)
        return -1;
    pool->free_list = free_list;

    chunk->trays = calloc(n, sizeof(*chunk->trays));
    chunk->data = malloc(n * pool->bufsize);
    if (chunk->trays == NULL || chunk->data == NULL) {
        free(chunk->trays);
        free(chunk->data);
        chunk->trays = NULL;
        chunk->data = NULL;
        errno = ENOMEM;
        return -1;
    }

    chunk->n = n;
    for (size_t i = 0; i < n; i++) {
        chunk->trays[i].original_buf = chunk->data + i * pool->bufsize;
        pool->free_list[pool->nfree++] = &chunk->trays[i];
    }
    pool->nchunks++;
    pool->total += n;
    return 0;
}

static chunk_t *pool_chunk_of(pool_t *pool, const record_t *record)
{
    uintptr_t address = (uintptr_t)record;

    for (size_t k = 0; k < pool->nchunks; k++) {
        chunk_t *chunk = &pool->chunks[k];
        uintptr_t base = (uintptr_t)chunk->trays;

        if (address >= base && (address - base) % sizeof(tray_t) == 0 &&
            (address - base) / sizeof(tray_t) < chunk->n)
            return chunk;
    }
    return NULL;
}

/* A 64-bit integer mixer gives a well-distributed slot for aligned tray addresses. */
static size_t pool_index_hash(const record_t *record)
{
    uint64_t value = (uint64_t)(uintptr_t)record;

    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return (size_t)value;
}

/* Allocate and populate a replacement membership table before publishing it. */
static int pool_grow_index(pool_t *pool)
{
    size_t capacity;
    tray_t **index;

    if (pool->index_capacity == 0) {
        capacity = UNBOUNDED_INDEX_INITIAL_CAPACITY;
    } else {
        if (pool->index_capacity > SIZE_MAX / 2) {
            errno = ENOMEM;
            return -1;
        }
        capacity = pool->index_capacity * 2;
    }
    if (capacity > SIZE_MAX / sizeof(*index)) {
        errno = ENOMEM;
        return -1;
    }

    index = calloc(capacity, sizeof(*index));
    if (index == NULL) {
        errno = ENOMEM;
        return -1;
    }

    for (tray_t *tray = pool->owners; tray != NULL; tray = tray->owner_next) {
        size_t slot = pool_index_hash(&tray->rec) & (capacity - 1);

        while (index[slot] != NULL)
            slot = (slot + 1) & (capacity - 1);
        index[slot] = tray;
    }

    free(pool->index);
    pool->index = index;
    pool->index_capacity = capacity;
    return 0;
}

static void pool_index_insert(pool_t *pool, tray_t *tray)
{
    size_t slot = pool_index_hash(&tray->rec) & (pool->index_capacity - 1);

    while (pool->index[slot] != NULL)
        slot = (slot + 1) & (pool->index_capacity - 1);
    pool->index[slot] = tray;
    pool->index_count++;
}

static tray_t *pool_index_find(pool_t *pool, const record_t *record)
{
    size_t slot;

    if (pool->index_capacity == 0)
        return NULL;

    slot = pool_index_hash(record) & (pool->index_capacity - 1);
    for (size_t probes = 0; probes < pool->index_capacity; probes++) {
        tray_t *tray = pool->index[slot];

        if (tray == NULL)
            return NULL;
        if (&tray->rec == record)
            return tray;
        slot = (slot + 1) & (pool->index_capacity - 1);
    }
    return NULL;
}

static void pool_free_memory(pool_t *pool)
{
    if (pool->unbounded) {
        tray_t *tray = pool->owners;

        while (tray != NULL) {
            tray_t *next = tray->owner_next;

            free(tray->original_buf);
            free(tray);
            tray = next;
        }
    } else {
        for (size_t k = 0; k < pool->nchunks; k++) {
            free(pool->chunks[k].data);
            free(pool->chunks[k].trays);
        }
    }
    free(pool->index);
    free(pool->free_list);
    free(pool);
}

pool_t *pool_create(size_t nbufs, size_t bufsize)
{
    pool_t *pool;
    int saved_errno = errno;
    int rc;

    if (bufsize == 0) {
        errno = EINVAL;
        return NULL;
    }
    if (nbufs > SIZE_MAX / bufsize || nbufs > SIZE_MAX / sizeof(tray_t)) {
        errno = EOVERFLOW;
        return NULL;
    }

    pool = calloc(1, sizeof(*pool));
    if (pool == NULL)
        return NULL;
    pool->bufsize = bufsize;
    pool->unbounded = nbufs == 0;

    if (!pool->unbounded && pool_add_chunk(pool, nbufs) < 0) {
        int allocation_errno = errno;

        pool_free_memory(pool);
        errno = allocation_errno;
        return NULL;
    }

    rc = pthread_mutex_init(&pool->mutex, NULL);
    if (rc != 0) {
        pool_free_memory(pool);
        errno = rc;
        return NULL;
    }
    rc = pthread_cond_init(&pool->not_empty, NULL);
    if (rc != 0) {
        (void)pthread_mutex_destroy(&pool->mutex);
        pool_free_memory(pool);
        errno = rc;
        return NULL;
    }

    errno = saved_errno;
    return pool;
}

record_t *pool_get(pool_t *pool)
{
    int saved_errno = errno;
    tray_t *tray;
    int rc;

    if (pool == NULL) {
        errno = EINVAL;
        return NULL;
    }

    rc = pthread_mutex_lock(&pool->mutex);
    if (rc != 0) {
        errno = rc;
        return NULL;
    }

    if (pool->closed) {
        (void)pthread_mutex_unlock(&pool->mutex);
        errno = saved_errno;
        return NULL;
    }

    if (pool->unbounded) {
        if (pool->free_head != NULL) {
            tray = pool->free_head;
            pool->free_head = tray->free_next;
            tray->free_next = NULL;
            tray->out = true;
        } else {
            tray = calloc(1, sizeof(*tray));
            if (tray == NULL) {
                (void)pthread_mutex_unlock(&pool->mutex);
                errno = ENOMEM;
                return NULL;
            }
            tray->original_buf = malloc(pool->bufsize);
            if (tray->original_buf == NULL) {
                free(tray);
                (void)pthread_mutex_unlock(&pool->mutex);
                errno = ENOMEM;
                return NULL;
            }

            if ((pool->index_capacity == 0 ||
                 pool->index_count >= pool->index_capacity / 2) &&
                pool_grow_index(pool) < 0) {
                int allocation_errno = errno;

                free(tray->original_buf);
                free(tray);
                (void)pthread_mutex_unlock(&pool->mutex);
                errno = allocation_errno;
                return NULL;
            }

            tray->owner_next = pool->owners;
            pool->owners = tray;
            pool_index_insert(pool, tray);
            tray->out = true;
        }

        (void)pthread_mutex_unlock(&pool->mutex);
        memset(&tray->rec, 0, sizeof(tray->rec));
        tray->rec.buf = tray->original_buf;
        tray->rec.cap = pool->bufsize;
        errno = saved_errno;
        return &tray->rec;
    }

    while (pool->nfree == 0 && !pool->closed) {
        rc = pthread_cond_wait(&pool->not_empty, &pool->mutex);
        if (rc != 0) {
            (void)pthread_mutex_unlock(&pool->mutex);
            errno = rc;
            return NULL;
        }
    }

    if (pool->closed) {
        (void)pthread_mutex_unlock(&pool->mutex);
        errno = saved_errno;
        return NULL;
    }

    tray = pool->free_list[--pool->nfree];
    tray->out = true;
    (void)pthread_mutex_unlock(&pool->mutex);

    memset(&tray->rec, 0, sizeof(tray->rec));
    tray->rec.buf = tray->original_buf;
    tray->rec.cap = pool->bufsize;
    errno = saved_errno;
    return &tray->rec;
}

void pool_put(pool_t *pool, record_t *record)
{
    int saved_errno = errno;
    tray_t *tray;
    int rc;

    if (pool == NULL || record == NULL) {
        errno = EINVAL;
        return;
    }

    rc = pthread_mutex_lock(&pool->mutex);
    if (rc != 0) {
        errno = rc;
        return;
    }

    if (pool->unbounded) {
        tray = pool_index_find(pool, record);
        if (tray == NULL || !tray->out) {
            (void)pthread_mutex_unlock(&pool->mutex);
            errno = EINVAL;
            return;
        }
        tray->out = false;
        tray->free_next = pool->free_head;
        pool->free_head = tray;
    } else {
        if (pool_chunk_of(pool, record) == NULL) {
            (void)pthread_mutex_unlock(&pool->mutex);
            errno = EINVAL;
            return;
        }
        tray = (tray_t *)record;
        if (!tray->out) {
            (void)pthread_mutex_unlock(&pool->mutex);
            errno = EINVAL;
            return;
        }
        tray->out = false;
        pool->free_list[pool->nfree++] = tray;
    }

    (void)pthread_cond_signal(&pool->not_empty);
    (void)pthread_mutex_unlock(&pool->mutex);
    errno = saved_errno;
}

void pool_close(pool_t *pool)
{
    int saved_errno = errno;
    int rc;

    if (pool == NULL) {
        errno = saved_errno;
        return;
    }

    rc = pthread_mutex_lock(&pool->mutex);
    if (rc != 0) {
        errno = rc;
        return;
    }
    pool->closed = true;
    (void)pthread_cond_broadcast(&pool->not_empty);
    (void)pthread_mutex_unlock(&pool->mutex);
    errno = saved_errno;
}

void pool_destroy(pool_t *pool)
{
    int saved_errno = errno;
    int rc;

    if (pool == NULL) {
        errno = saved_errno;
        return;
    }

    rc = pthread_cond_destroy(&pool->not_empty);
    if (rc != 0) {
        errno = rc;
        return;
    }
    rc = pthread_mutex_destroy(&pool->mutex);
    if (rc != 0) {
        errno = rc;
        return;
    }

    pool_free_memory(pool);
    errno = saved_errno;
}
