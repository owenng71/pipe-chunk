/* pool.c — fixed buffer (tray) pool. Not implemented yet. */
#include <errno.h>
#include "pool.h"

pool_t *pool_create(size_t nbufs, size_t bufsize) { (void)nbufs; (void)bufsize; errno = ENOSYS; return NULL; }
record_t *pool_get(pool_t *p) { (void)p; return NULL; }
void pool_put(pool_t *p, record_t *r) { (void)p; (void)r; }
void pool_close(pool_t *p) { (void)p; }
void pool_destroy(pool_t *p) { (void)p; }
