/* io.c — read_full / pwrite_full: the only place that calls read/write. */
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>

#include "io.h"

/* Sleep until fd is ready for `events`. Returns 0, or -1 with errno set. */
static int wait_ready(int fd, short events)
{
    struct pollfd pfd = { .fd = fd, .events = events };

    for (;;) {
        int rc = poll(&pfd, 1, -1);

        if (rc > 0)
            return 0;   // ready, or POLLERR/POLLHUP: the next call reports it
        if (rc < 0 && errno != EINTR)
            return -1;
    }
}

ssize_t read_full(int fd, void *buf, size_t n)
{
    int saved_errno = errno;
    unsigned char *p = buf;
    size_t got = 0;

    if (n > SSIZE_MAX) {
        errno = EINVAL;
        return -1;
    }

    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);

        if (r > 0) {
            got += (size_t)r;
        } else if (r == 0) {
            break;                                  // EOF
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (wait_ready(fd, POLLIN) < 0)
                return -1;
        } else {
            return -1;
        }
    }

    errno = saved_errno;
    return (ssize_t)got;
}

int pwrite_full(int fd, const void *buf, size_t n, off_t off)
{
    int saved_errno = errno;
    const unsigned char *p = buf;
    size_t done = 0;

    if (off < 0 || n > SSIZE_MAX) {
        errno = EINVAL;
        return -1;
    }

    while (done < n) {
        ssize_t w = pwrite(fd, p + done, n - done, off + (off_t)done);

        if (w > 0) {
            done += (size_t)w;
        } else if (w == 0) {
            errno = EIO;                            // no progress: avoid spinning forever
            return -1;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (wait_ready(fd, POLLOUT) < 0)
                return -1;
        } else {
            return -1;
        }
    }

    errno = saved_errno;
    return 0;
}
