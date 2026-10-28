/* io.c — read_full / pwrite_full: the only place that calls read/write. Not implemented yet. */
#include <errno.h>
#include "io.h"

ssize_t read_full(int fd, void *buf, size_t n) { (void)fd; (void)buf; (void)n; errno = ENOSYS; return -1; }
int pwrite_full(int fd, const void *buf, size_t n, off_t off) { (void)fd; (void)buf; (void)n; (void)off; errno = ENOSYS; return -1; }
