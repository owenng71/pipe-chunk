#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "pool.h"

struct pool {
    record_t *trays;
    unsigned char **original_buffers;
    size_t *free_indices;
    bool *checked_out;
    size_t nbufs;
    size_t bufsize;
    size_t nfree;
    bool closed;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
};

static void pool_free_partial(pool_t *pool, size_t buffers_created,
                              bool mutex_initialized, bool cond_initialized)
{
    int saved_errno = errno;

    if (pool != NULL) {
        if (pool->original_buffers != NULL) {
            for (size_t i = 0; i < buffers_created; i++)
                free(pool->original_buffers[i]);
        }
        if (cond_initialized)
            (void)pthread_cond_destroy(&pool->not_empty);
        if (mutex_initialized)
            (void)pthread_mutex_destroy(&pool->mutex);
        free(pool->checked_out);
        free(pool->free_indices);
        free(pool->original_buffers);
        free(pool->trays);
        free(pool);
    }
    errno = saved_errno;
}

pool_t *pool_create(size_t nbufs, size_t bufsize)
{
    int caller_errno = errno;
    size_t buffers_created = 0;
    bool mutex_initialized = false;
    bool cond_initialized = false;
    pool_t *pool;
    int rc;

    // limit: 0 = unbounded arrives with W2-09
    if (nbufs == 0) {
        errno = EINVAL;
        return NULL;
    }
    if (bufsize == 0) {
        errno = EINVAL;
        return NULL;
    }
    if (nbufs > SIZE_MAX / sizeof(record_t) ||
        nbufs > SIZE_MAX / sizeof(unsigned char *) ||
        nbufs > SIZE_MAX / sizeof(size_t) ||
        nbufs > SIZE_MAX / sizeof(bool) ||
        nbufs > SIZE_MAX / bufsize) {
        errno = EOVERFLOW;
        return NULL;
    }

    pool = calloc(1, sizeof *pool);
    if (pool == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    pool->nbufs = nbufs;
    pool->bufsize = bufsize;
    pool->trays = calloc(nbufs, sizeof *pool->trays);
    pool->original_buffers = calloc(nbufs, sizeof *pool->original_buffers);
    pool->free_indices = calloc(nbufs, sizeof *pool->free_indices);
    pool->checked_out = calloc(nbufs, sizeof *pool->checked_out);
    if (pool->trays == NULL || pool->original_buffers == NULL ||
        pool->free_indices == NULL || pool->checked_out == NULL) {
        pool_free_partial(pool, buffers_created, mutex_initialized,
                          cond_initialized);
        errno = ENOMEM;
        return NULL;
    }

    for (size_t i = 0; i < nbufs; i++) {
        pool->original_buffers[i] = calloc(1, bufsize);
        if (pool->original_buffers[i] == NULL) {
            pool_free_partial(pool, buffers_created, mutex_initialized,
                              cond_initialized);
            errno = ENOMEM;
            return NULL;
        }
        buffers_created++;
        pool->trays[i].buf = pool->original_buffers[i];
        pool->trays[i].cap = bufsize;
        pool->free_indices[i] = i;
    }

    rc = pthread_mutex_init(&pool->mutex, NULL);
    if (rc != 0) {
        pool_free_partial(pool, buffers_created, mutex_initialized,
                          cond_initialized);
        errno = rc;
        return NULL;
    }
    mutex_initialized = true;

    rc = pthread_cond_init(&pool->not_empty, NULL);
    if (rc != 0) {
        pool_free_partial(pool, buffers_created, mutex_initialized,
                          cond_initialized);
        errno = rc;
        return NULL;
    }
    cond_initialized = true;
    pool->nfree = nbufs;
    errno = caller_errno;
    return pool;
}

record_t *pool_get(pool_t *pool)
{
    int caller_errno = errno;
    int rc;
    size_t index;
    record_t *tray;

    if (pool == NULL) {
        errno = EINVAL;
        return NULL;
    }
    rc = pthread_mutex_lock(&pool->mutex);
    if (rc != 0) {
        errno = rc;
        return NULL;
    }
    while (!pool->closed && pool->nfree == 0) {
        rc = pthread_cond_wait(&pool->not_empty, &pool->mutex);
        if (rc != 0) {
            int unlock_rc = pthread_mutex_unlock(&pool->mutex);
            (void)unlock_rc;
            errno = rc;
            return NULL;
        }
    }
    if (pool->closed) {
        rc = pthread_mutex_unlock(&pool->mutex);
        if (rc != 0) {
            errno = rc;
            return NULL;
        }
        errno = caller_errno;
        return NULL;
    }

    index = pool->free_indices[--pool->nfree];
    pool->checked_out[index] = true;
    tray = &pool->trays[index];
    tray->seq = 0;
    tray->offset = 0;
    tray->len = 0;
    memset(tray->sha, 0, sizeof tray->sha);
    tray->t_read_ns = 0;
    tray->attempts = 0;
    tray->buf = pool->original_buffers[index];
    tray->cap = pool->bufsize;

    rc = pthread_mutex_unlock(&pool->mutex);
    if (rc != 0) {
        errno = rc;
        return NULL;
    }
    errno = caller_errno;
    return tray;
}

void pool_put(pool_t *pool, record_t *tray)
{
    int caller_errno = errno;
    uintptr_t base;
    uintptr_t address;
    uintptr_t offset;
    size_t span;
    size_t index;
    int rc;

    if (pool == NULL || tray == NULL) {
        errno = EINVAL;
        return;
    }
    base = (uintptr_t)(void *)pool->trays;
    address = (uintptr_t)(void *)tray;
    span = pool->nbufs * sizeof *pool->trays;
    if (address < base) {
        errno = EINVAL;
        return;
    }
    offset = address - base;
    if (offset >= span || offset % sizeof *pool->trays != 0) {
        errno = EINVAL;
        return;
    }
    index = (size_t)(offset / sizeof *pool->trays);

    rc = pthread_mutex_lock(&pool->mutex);
    if (rc != 0) {
        errno = rc;
        return;
    }
    if (!pool->checked_out[index]) {
        (void)pthread_mutex_unlock(&pool->mutex);
        errno = EINVAL;
        return;
    }

    pool->checked_out[index] = false;
    pool->free_indices[pool->nfree++] = index;
    if (!pool->closed) {
        rc = pthread_cond_signal(&pool->not_empty);
        if (rc != 0) {
            (void)pthread_mutex_unlock(&pool->mutex);
            errno = rc;
            return;
        }
    }
    rc = pthread_mutex_unlock(&pool->mutex);
    if (rc != 0) {
        errno = rc;
        return;
    }
    errno = caller_errno;
}

void pool_close(pool_t *pool)
{
    int caller_errno = errno;
    int rc;

    if (pool == NULL) {
        errno = EINVAL;
        return;
    }
    rc = pthread_mutex_lock(&pool->mutex);
    if (rc != 0) {
        errno = rc;
        return;
    }
    pool->closed = true;
    rc = pthread_cond_broadcast(&pool->not_empty);
    if (rc != 0) {
        (void)pthread_mutex_unlock(&pool->mutex);
        errno = rc;
        return;
    }
    rc = pthread_mutex_unlock(&pool->mutex);
    if (rc != 0) {
        errno = rc;
        return;
    }
    errno = caller_errno;
}

void pool_destroy(pool_t *pool)
{
    int caller_errno = errno;
    int error = 0;
    int rc;

    if (pool == NULL)
        return;

    rc = pthread_cond_destroy(&pool->not_empty);
    if (rc != 0)
        error = rc;
    rc = pthread_mutex_destroy(&pool->mutex);
    if (rc != 0 && error == 0)
        error = rc;
    for (size_t i = 0; i < pool->nbufs; i++)
        free(pool->original_buffers[i]);
    free(pool->checked_out);
    free(pool->free_indices);
    free(pool->original_buffers);
    free(pool->trays);
    free(pool);

    errno = error != 0 ? error : caller_errno;
}
