/* queue.h — bounded blocking queue. */
#ifndef QUEUE_H
#define QUEUE_H

#include "common.h"

typedef struct bq bq_t;

bq_t  *bq_create(size_t capacity);          // capacity 0 = unbounded
int    bq_push(bq_t *q, record_t *r);       // blocks while full; -1 if closed
int    bq_pop(bq_t *q, record_t **out);     // blocks while empty; 1 = got one, 0 = closed and empty
void   bq_close(bq_t *q);                   // wake everyone
size_t bq_size(bq_t *q);
void   bq_destroy(bq_t *q);

#endif
