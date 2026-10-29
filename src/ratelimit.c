/* ratelimit.c — pacing for --in-rate / --out-rate (simulates a slow device). */
#include <errno.h>
#include <time.h>

#include "ratelimit.h"

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void ratelimit_init(ratelimit_t *rl, double mb_per_s)
{
    rl->ns_per_byte = mb_per_s > 0 ? 1e9 / (mb_per_s * 1e6) : 0;
    rl->next_ns = 0;
}

void ratelimit_wait(ratelimit_t *rl, size_t bytes)
{
    int saved_errno = errno;
    uint64_t now;

    if (rl->ns_per_byte == 0)
        return;
    now = now_ns();
    if (rl->next_ns < now) {
        rl->next_ns = now;   // idle time does not build up a burst allowance
    } else {
        struct timespec until = { (time_t)(rl->next_ns / 1000000000u), (long)(rl->next_ns % 1000000000u) };

        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) == EINTR)
            ;
    }
    rl->next_ns += (uint64_t)(rl->ns_per_byte * (double)bytes);
    errno = saved_errno;
}
