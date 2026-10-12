/* queue.c — STUB, Son implements in W1-08. */
#include <errno.h>
#include "queue.h"

bq_t *bq_create(size_t capacity) { (void)capacity; errno = ENOSYS; return NULL; }
int bq_push(bq_t *q, record_t *r) { (void)q; (void)r; errno = ENOSYS; return -1; }
int bq_pop(bq_t *q, record_t **out) { (void)q; *out = NULL; return 0; }
void bq_close(bq_t *q) { (void)q; }
size_t bq_size(bq_t *q) { (void)q; return 0; }
void bq_destroy(bq_t *q) { (void)q; }
