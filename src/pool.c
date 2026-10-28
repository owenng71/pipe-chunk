/* pool.c — fixed buffer (tray) pool. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"

struct pool {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;   // a tray was returned or the pool was closed
    record_t *trays;            // nbufs trays; trays[i].buf starts at data + i * bufsize
    unsigned char *data;        // one allocation backing every tray buffer
    record_t **free_list;       // stack of free trays
    bool *out;                  // out[i]: trays[i] is checked out (catches double put)
    size_t nbufs;
    size_t bufsize;
    size_t nfree;
    bool closed;
};

static void pool_free_memory(pool_t *pool)
{
    free(pool->out);
    free(pool->free_list);
    free(pool->data);
    free(pool->trays);
    free(pool);
}

pool_t *pool_create(size_t nbufs, size_t bufsize)
{
    pool_t *pool;
    int saved_errno = errno;
    int rc;

    if (nbufs == 0 || bufsize == 0) {
        // limit: nbufs 0 = unbounded (--unbounded) is not implemented yet
        errno = EINVAL;
        return NULL;
    }
    if (nbufs > SIZE_MAX / bufsize || nbufs > SIZE_MAX / sizeof(*pool->trays)) {
        errno = EOVERFLOW;
        return NULL;
    }

    pool = calloc(1, sizeof(*pool));
    if (pool == NULL)
        return NULL;
    pool->trays = calloc(nbufs, sizeof(*pool->trays));
    pool->data = malloc(nbufs * bufsize);
    pool->free_list = calloc(nbufs, sizeof(*pool->free_list));
    pool->out = calloc(nbufs, sizeof(*pool->out));
    if (pool->trays == NULL || pool->data == NULL || pool->free_list == NULL || pool->out == NULL) {
        pool_free_memory(pool);
        errno = ENOMEM;
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

    for (size_t i = 0; i < nbufs; i++)
        pool->free_list[i] = &pool->trays[i];
    pool->nbufs = nbufs;
    pool->bufsize = bufsize;
    pool->nfree = nbufs;
    errno = saved_errno;
    return pool;
}

record_t *pool_get(pool_t *pool)
{
    int saved_errno = errno;
    record_t *r;
    size_t i;
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

    r = pool->free_list[--pool->nfree];
    i = (size_t)(r - pool->trays);
    pool->out[i] = true;
    (void)pthread_mutex_unlock(&pool->mutex);

    /* The tray is ours now: hand it out clean, with its own buffer even if a user repointed buf. */
    memset(r, 0, sizeof(*r));
    r->buf = pool->data + i * pool->bufsize;
    r->cap = pool->bufsize;
    errno = saved_errno;
    return r;
}

void pool_put(pool_t *pool, record_t *r)
{
    int saved_errno = errno;
    size_t i;
    int rc;

    if (pool == NULL || r == NULL) {
        errno = EINVAL;
        return;
    }
    uintptr_t base = (uintptr_t)pool->trays, addr = (uintptr_t)r;

    if (addr < base || (addr - base) % sizeof(*r) != 0 || (addr - base) / sizeof(*r) >= pool->nbufs) {
        errno = EINVAL;   // not one of this pool's trays
        return;
    }
    i = (addr - base) / sizeof(*r);

    rc = pthread_mutex_lock(&pool->mutex);
    if (rc != 0) {
        errno = rc;
        return;
    }
    if (!pool->out[i]) {
        (void)pthread_mutex_unlock(&pool->mutex);
        errno = EINVAL;   // already free: a double put would hand one tray to two owners
        return;
    }
    pool->out[i] = false;
    pool->free_list[pool->nfree++] = r;
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
