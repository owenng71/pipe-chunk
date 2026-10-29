/* pool.c — fixed buffer (tray) pool; nbufs 0 = unbounded (allocates on demand, never blocks). */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"

enum { UNBOUNDED_FIRST_CHUNK = 64, MAX_CHUNKS = 48 };

typedef struct tray {
    record_t rec;   // first member: a record_t * handed out is also a tray *
    bool out;       // checked out (catches a double put)
} tray_t;

/* A run of trays with one allocation backing their buffers. Bounded pools have exactly one. */
typedef struct chunk {
    tray_t *trays;
    unsigned char *data;   // trays[i].rec.buf starts at data + i * bufsize
    size_t n;
} chunk_t;

struct pool {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;   // a tray was returned or the pool was closed
    chunk_t chunks[MAX_CHUNKS];
    size_t nchunks;
    tray_t **free_list;         // stack of free trays, room for every tray
    size_t nfree;
    size_t total;               // trays in all chunks
    size_t bufsize;
    bool unbounded;
    bool closed;
};

/* Adds a chunk of n trays and pushes them on the free list. Mutex held (or not yet shared). */
static int pool_add_chunk(pool_t *pool, size_t n)
{
    chunk_t *c = &pool->chunks[pool->nchunks];
    tray_t **free_list;

    if (pool->nchunks == MAX_CHUNKS || n > SIZE_MAX / pool->bufsize ||
        n > SIZE_MAX / sizeof(*c->trays) || pool->total > SIZE_MAX / sizeof(*free_list) - n) {
        errno = ENOMEM;
        return -1;
    }
    free_list = realloc(pool->free_list, (pool->total + n) * sizeof(*free_list));
    if (free_list == NULL)
        return -1;
    pool->free_list = free_list;
    c->trays = calloc(n, sizeof(*c->trays));
    c->data = malloc(n * pool->bufsize);
    if (c->trays == NULL || c->data == NULL) {
        free(c->trays);
        free(c->data);
        errno = ENOMEM;
        return -1;
    }
    c->n = n;
    for (size_t i = 0; i < n; i++)
        pool->free_list[pool->nfree++] = &c->trays[i];
    pool->nchunks++;
    pool->total += n;
    return 0;
}

static chunk_t *pool_chunk_of(pool_t *pool, const record_t *r)
{
    uintptr_t addr = (uintptr_t)r;

    for (size_t k = 0; k < pool->nchunks; k++) {
        chunk_t *c = &pool->chunks[k];
        uintptr_t base = (uintptr_t)c->trays;

        if (addr >= base && (addr - base) % sizeof(tray_t) == 0 && (addr - base) / sizeof(tray_t) < c->n)
            return c;
    }
    return NULL;
}

static void pool_free_memory(pool_t *pool)
{
    for (size_t k = 0; k < pool->nchunks; k++) {
        free(pool->chunks[k].data);
        free(pool->chunks[k].trays);
    }
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
    if (pool_add_chunk(pool, pool->unbounded ? UNBOUNDED_FIRST_CHUNK : nbufs) < 0) {
        rc = errno;
        pool_free_memory(pool);
        errno = rc;
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
    chunk_t *c;
    tray_t *t;
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

    if (pool->nfree == 0 && pool->unbounded && !pool->closed &&
        pool_add_chunk(pool, pool->total) < 0) {   // doubles the pool
        rc = errno;
        (void)pthread_mutex_unlock(&pool->mutex);
        errno = rc;
        return NULL;
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

    t = pool->free_list[--pool->nfree];
    t->out = true;
    c = pool_chunk_of(pool, &t->rec);
    (void)pthread_mutex_unlock(&pool->mutex);

    /* The tray is ours now: hand it out clean, with its own buffer even if a user repointed buf. */
    memset(&t->rec, 0, sizeof(t->rec));
    t->rec.buf = c->data + (size_t)(t - c->trays) * pool->bufsize;
    t->rec.cap = pool->bufsize;
    errno = saved_errno;
    return &t->rec;
}

void pool_put(pool_t *pool, record_t *r)
{
    int saved_errno = errno;
    tray_t *t = (tray_t *)r;
    int rc;

    if (pool == NULL || r == NULL) {
        errno = EINVAL;
        return;
    }

    rc = pthread_mutex_lock(&pool->mutex);
    if (rc != 0) {
        errno = rc;
        return;
    }
    if (pool_chunk_of(pool, r) == NULL || !t->out) {
        (void)pthread_mutex_unlock(&pool->mutex);
        errno = EINVAL;   // foreign tray, or a double put that would give one tray two owners
        return;
    }
    t->out = false;
    pool->free_list[pool->nfree++] = t;
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
