/* queue.c - bounded blocking queue (Son, W1-08). */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "queue.h"

struct bq {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    record_t **items;
    size_t capacity;
    size_t head;
    size_t count;
    bool closed;
};

bq_t *bq_create(size_t capacity)
{
    bq_t *queue;
    int saved_errno = errno;
    int rc;

    if (capacity == 0) {
        // limit: 0 = unbounded arrives with W2-09
        errno = EINVAL;
        return NULL;
    }
    if (capacity > SIZE_MAX / sizeof(*queue->items)) {
        errno = EOVERFLOW;
        return NULL;
    }

    queue = calloc(1, sizeof(*queue));
    if (queue == NULL)
        return NULL;

    queue->items = calloc(capacity, sizeof(*queue->items));
    if (queue->items == NULL) {
        int allocation_errno = errno;

        free(queue);
        errno = allocation_errno;
        return NULL;
    }

    rc = pthread_mutex_init(&queue->mutex, NULL);
    if (rc != 0) {
        free(queue->items);
        free(queue);
        errno = rc;
        return NULL;
    }

    rc = pthread_cond_init(&queue->not_empty, NULL);
    if (rc != 0) {
        (void)pthread_mutex_destroy(&queue->mutex);
        free(queue->items);
        free(queue);
        errno = rc;
        return NULL;
    }

    rc = pthread_cond_init(&queue->not_full, NULL);
    if (rc != 0) {
        (void)pthread_cond_destroy(&queue->not_empty);
        (void)pthread_mutex_destroy(&queue->mutex);
        free(queue->items);
        free(queue);
        errno = rc;
        return NULL;
    }

    queue->capacity = capacity;
    errno = saved_errno;
    return queue;
}

int bq_push(bq_t *queue, record_t *record)
{
    int saved_errno = errno;
    int rc;

    if (queue == NULL || record == NULL) {
        errno = EINVAL;
        return -1;
    }

    rc = pthread_mutex_lock(&queue->mutex);
    if (rc != 0) {
        errno = rc;
        return -1;
    }

    while (queue->count == queue->capacity && !queue->closed) {
        rc = pthread_cond_wait(&queue->not_full, &queue->mutex);
        if (rc != 0) {
            (void)pthread_mutex_unlock(&queue->mutex);
            errno = rc;
            return -1;
        }
    }

    if (queue->closed) {
        (void)pthread_mutex_unlock(&queue->mutex);
        errno = saved_errno;
        return -1;
    }

    queue->items[(queue->head + queue->count) % queue->capacity] = record;
    queue->count++;
    (void)pthread_cond_signal(&queue->not_empty);
    (void)pthread_mutex_unlock(&queue->mutex);
    errno = saved_errno;
    return 0;
}

int bq_pop(bq_t *queue, record_t **out)
{
    int saved_errno = errno;
    int rc;

    if (out != NULL)
        *out = NULL;
    if (queue == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    rc = pthread_mutex_lock(&queue->mutex);
    if (rc != 0) {
        errno = rc;
        return -1;
    }

    while (queue->count == 0 && !queue->closed) {
        rc = pthread_cond_wait(&queue->not_empty, &queue->mutex);
        if (rc != 0) {
            (void)pthread_mutex_unlock(&queue->mutex);
            errno = rc;
            return -1;
        }
    }

    if (queue->count == 0) {
        (void)pthread_mutex_unlock(&queue->mutex);
        errno = saved_errno;
        return 0;
    }

    *out = queue->items[queue->head];
    queue->items[queue->head] = NULL;
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    (void)pthread_cond_signal(&queue->not_full);
    (void)pthread_mutex_unlock(&queue->mutex);
    errno = saved_errno;
    return 1;
}

void bq_close(bq_t *queue)
{
    int saved_errno = errno;
    int rc;

    if (queue == NULL) {
        errno = saved_errno;
        return;
    }

    rc = pthread_mutex_lock(&queue->mutex);
    if (rc != 0) {
        errno = rc;
        return;
    }

    queue->closed = true;
    (void)pthread_cond_broadcast(&queue->not_empty);
    (void)pthread_cond_broadcast(&queue->not_full);
    (void)pthread_mutex_unlock(&queue->mutex);
    errno = saved_errno;
}

size_t bq_size(bq_t *queue)
{
    int saved_errno = errno;
    size_t count;
    int rc;

    if (queue == NULL) {
        errno = EINVAL;
        return 0;
    }

    rc = pthread_mutex_lock(&queue->mutex);
    if (rc != 0) {
        errno = rc;
        return 0;
    }
    count = queue->count;
    (void)pthread_mutex_unlock(&queue->mutex);
    errno = saved_errno;
    return count;
}

void bq_destroy(bq_t *queue)
{
    int saved_errno = errno;
    int rc;

    if (queue == NULL) {
        errno = saved_errno;
        return;
    }

    rc = pthread_cond_destroy(&queue->not_empty);
    if (rc != 0) {
        errno = rc;
        return;
    }
    rc = pthread_cond_destroy(&queue->not_full);
    if (rc != 0) {
        errno = rc;
        return;
    }
    rc = pthread_mutex_destroy(&queue->mutex);
    if (rc != 0) {
        errno = rc;
        return;
    }

    free(queue->items);
    free(queue);
    errno = saved_errno;
}
