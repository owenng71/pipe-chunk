/* io.h — every short/interruptible syscall goes through here (short counts, EINTR, EAGAIN). */
#ifndef IO_H
#define IO_H

#include "common.h"

ssize_t read_full(int fd, void *buf, size_t n);                  // loops on short/EINTR/EAGAIN; <n only at EOF; -1 on error
int     pwrite_full(int fd, const void *buf, size_t n, off_t off); // 0 ok, -1 error

#endif
