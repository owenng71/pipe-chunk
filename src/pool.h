/* pool.h — fixed buffer (tray) pool. */
#ifndef POOL_H
#define POOL_H

#include "common.h"

typedef struct pool pool_t;

pool_t   *pool_create(size_t nbufs, size_t bufsize);   // nbufs 0 = unbounded (malloc on demand)
record_t *pool_get(pool_t *p);                          // blocks while empty; NULL if closed
void      pool_put(pool_t *p, record_t *r);
void      pool_close(pool_t *p);
void      pool_destroy(pool_t *p);                      // frees every tray (needed for ASan)

#endif
