/* pool.c — buffer (tray) pool. Bounded: nbufs trays in one allocation; pool_get blocks while none is
 * free. Unbounded (nbufs 0): each tray is malloc'd on demand with its buffer; pool_get never blocks. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"

enum { INDEX_FIRST_CAP = 64 };

typedef struct tray {
    record_t rec;            // first member: a record_t * handed out is also a tray *
    unsigned char *buf;      // the tray's own buffer; rec.buf may be repointed by its owner
    struct tray *next_free;
    bool out;                // checked out (catches a double put)
} tray_t;

struct pool {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;   // a tray was returned or the pool was closed
    tray_t *free;               // stack of free trays, linked through next_free
    tray_t *trays;              // bounded: all ntrays trays
    unsigned char *data;        // bounded: trays[i].buf = data + i * bufsize
    tray_t **index;             // unbounded: open-addressed set of every tray, for put checks and destroy
    size_t index_cap;           // unbounded: power of two, at least twice ntrays
    size_t ntrays;
    size_t bufsize;
    bool unbounded;
    bool closed;
};

/* splitmix64 finalizer: spreads aligned heap addresses over the whole table. */
static size_t pool_hash(const record_t *r)
{
    uint64_t x = (uint64_t)(uintptr_t)r;

    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return (size_t)x;
}

/* Slot holding r, or the empty slot where it would go. The table is at most half full, so this ends. */
static size_t pool_index_slot(const pool_t *pool, const record_t *r)
{
    size_t mask = pool->index_cap - 1;
    size_t slot = pool_hash(r) & mask;

    while (pool->index[slot] != NULL && &pool->index[slot]->rec != r)
        slot = (slot + 1) & mask;
    return slot;
}

/* Doubles the index and rehashes every tray into it. Mutex held. */
static int pool_grow_index(pool_t *pool)
{
    tray_t **old = pool->index;
    size_t old_cap = pool->index_cap;
    size_t cap = old_cap == 0 ? INDEX_FIRST_CAP : old_cap * 2;

    if (old_cap > SIZE_MAX / 2 / sizeof(*old)) {
        errno = ENOMEM;
        return -1;
    }
    pool->index = calloc(cap, sizeof(*old));
    if (pool->index == NULL) {
        pool->index = old;
        errno = ENOMEM;
        return -1;
    }
    pool->index_cap = cap;
    for (size_t k = 0; k < old_cap; k++) {
        if (old[k] != NULL)
            pool->index[pool_index_slot(pool, &old[k]->rec)] = old[k];
    }
    free(old);
    return 0;
}

/* Unbounded: mallocs one tray with its buffer right behind it and pushes it on the free stack. Mutex held. */
static int pool_grow(pool_t *pool)
{
    tray_t *t;

    if (pool->ntrays >= pool->index_cap / 2 && pool_grow_index(pool) < 0)
        return -1;
    t = malloc(sizeof(*t) + pool->bufsize);   // size checked in pool_create
    if (t == NULL) {
        errno = ENOMEM;
        return -1;
    }
    t->buf = (unsigned char *)(t + 1);
    t->out = false;
    pool->index[pool_index_slot(pool, &t->rec)] = t;
    pool->ntrays++;
    t->next_free = pool->free;
    pool->free = t;
    return 0;
}

/* The tray behind r, or NULL if r is not one of this pool's trays. Mutex held. */
static tray_t *pool_find(const pool_t *pool, const record_t *r)
{
    uintptr_t addr = (uintptr_t)r;
    uintptr_t base = (uintptr_t)pool->trays;

    if (pool->unbounded)
        return pool->index_cap == 0 ? NULL : pool->index[pool_index_slot(pool, r)];
    if (addr >= base && (addr - base) % sizeof(tray_t) == 0 && (addr - base) / sizeof(tray_t) < pool->ntrays)
        return (tray_t *)r;
    return NULL;
}

static void pool_free_memory(pool_t *pool)
{
    for (size_t k = 0; k < pool->index_cap; k++)
        free(pool->index[k]);   // unbounded trays, checked out or not
    free(pool->index);
    free(pool->trays);
    free(pool->data);
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
    if (nbufs > SIZE_MAX / bufsize || nbufs > SIZE_MAX / sizeof(tray_t) || bufsize > SIZE_MAX - sizeof(tray_t)) {
        errno = EOVERFLOW;
        return NULL;
    }

    pool = calloc(1, sizeof(*pool));
    if (pool == NULL)
        return NULL;
    pool->bufsize = bufsize;
    pool->unbounded = nbufs == 0;
    if (!pool->unbounded) {
        pool->trays = calloc(nbufs, sizeof(*pool->trays));
        pool->data = malloc(nbufs * bufsize);
        if (pool->trays == NULL || pool->data == NULL) {
            pool_free_memory(pool);
            errno = ENOMEM;
            return NULL;
        }
        pool->ntrays = nbufs;
        for (size_t i = nbufs; i-- > 0;) {
            pool->trays[i].buf = pool->data + i * bufsize;
            pool->trays[i].next_free = pool->free;
            pool->free = &pool->trays[i];
        }
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

    if (pool->free == NULL && pool->unbounded && !pool->closed && pool_grow(pool) < 0) {
        rc = errno;
        (void)pthread_mutex_unlock(&pool->mutex);
        errno = rc;
        return NULL;
    }
    while (pool->free == NULL && !pool->closed) {
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

    t = pool->free;
    pool->free = t->next_free;
    t->out = true;
    (void)pthread_mutex_unlock(&pool->mutex);

    /* The tray is ours now: hand it out clean, with its own buffer even if a user repointed buf. */
    memset(&t->rec, 0, sizeof(t->rec));
    t->rec.buf = t->buf;
    t->rec.cap = pool->bufsize;
    errno = saved_errno;
    return &t->rec;
}

void pool_put(pool_t *pool, record_t *r)
{
    int saved_errno = errno;
    tray_t *t;
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
    t = pool_find(pool, r);
    if (t == NULL || !t->out) {
        (void)pthread_mutex_unlock(&pool->mutex);
        errno = EINVAL;   // foreign tray, or a double put that would give one tray two owners
        return;
    }
    t->out = false;
    t->next_free = pool->free;
    pool->free = t;
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
