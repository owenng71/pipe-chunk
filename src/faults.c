/* faults.c — fault injection for io.c: decides, never does I/O itself. */
#include <errno.h>
#include <time.h>

#include "faults.h"

/* Written only by faults_init, before the threads that read them start. */
static double p_short, p_eintr, p_eagain;
static unsigned delay_us;
static bool enabled;
static unsigned long seed;
static unsigned generation;   // bumped by faults_init so every thread reseeds

/* Per thread, one xorshift64* stream per direction. */
static _Thread_local uint64_t state[2];
static _Thread_local unsigned state_gen;

static uint64_t splitmix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15u;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9u;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebu;
    return x ^ (x >> 31);
}

static uint64_t next(fault_dir_t dir)
{
    uint64_t x;

    if (state_gen != generation) {
        for (int d = 0; d < 2; d++)
            state[d] = splitmix64((uint64_t)seed * 2 + (uint64_t)d) | 1;   // xorshift state must not be 0
        state_gen = generation;
    }
    x = state[dir];
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state[dir] = x;
    return x * 0x2545f4914f6cdd1du;
}

/* Uniform in [0, 1). */
static double chance(fault_dir_t dir)
{
    return (double)(next(dir) >> 11) * 0x1.0p-53;
}

void faults_init(const config_t *cfg)
{
    p_short = cfg->fault_short;
    p_eintr = cfg->fault_eintr;
    p_eagain = cfg->fault_eagain;
    delay_us = cfg->fault_delay_us;
    seed = cfg->seed;
    enabled = p_short > 0 || p_eintr > 0 || p_eagain > 0 || delay_us > 0;
    generation++;
}

int faults_before_io(fault_dir_t dir, size_t *n)
{
    if (!enabled)
        return 0;
    if (delay_us > 0) {
        uint64_t ns = next(dir) % ((uint64_t)delay_us * 1000 + 1);
        struct timespec ts = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) };
        int saved_errno = errno;

        while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
            ;
        errno = saved_errno;
    }
    if (p_eintr > 0 && chance(dir) < p_eintr) {
        errno = EINTR;
        return -1;
    }
    if (p_eagain > 0 && chance(dir) < p_eagain) {
        errno = EAGAIN;
        return -1;
    }
    if (p_short > 0 && *n > 1 && chance(dir) < p_short)
        *n = 1 + (size_t)(next(dir) % (*n - 1));   // 1 .. n-1 bytes
    return 0;
}
