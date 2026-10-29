/* ratelimit.h — pacing for --in-rate / --out-rate (simulates a slow device). */
#ifndef RATELIMIT_H
#define RATELIMIT_H

#include <stddef.h>
#include <stdint.h>

/* Owned by one thread (reader or writer); no locking. 1 MB = 1,000,000 bytes. */
typedef struct ratelimit {
    double   ns_per_byte;   // 0 = unlimited
    uint64_t next_ns;       // CLOCK_MONOTONIC time the next transfer may start
} ratelimit_t;

void ratelimit_init(ratelimit_t *rl, double mb_per_s);   // mb_per_s 0 = unlimited
void ratelimit_wait(ratelimit_t *rl, size_t bytes);      // call before moving `bytes`; sleeps as needed

#endif
