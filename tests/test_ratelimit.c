#include <assert.h>
#include <errno.h>
#include <time.h>
#include "ratelimit.h"

static double now_s(void)
{
    struct timespec ts;

    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void test_unlimited_never_sleeps(void)
{
    ratelimit_t rl;
    double start = now_s();

    ratelimit_init(&rl, 0);
    for (int i = 0; i < 1000; i++)
        ratelimit_wait(&rl, 1000000);
    assert(now_s() - start < 0.05);
}

static void test_paces_to_rate(void)
{
    ratelimit_t rl;
    double start, elapsed;

    ratelimit_init(&rl, 10);   // 10 MB/s: each 1 MB costs 100 ms
    start = now_s();
    errno = ENOTTY;
    for (int i = 0; i < 6; i++)
        ratelimit_wait(&rl, 1000000);
    elapsed = now_s() - start;
    assert(errno == ENOTTY);
    assert(elapsed >= 0.49);   // the first MB goes at once, the next five wait 100 ms each
    assert(elapsed < 2.0);
}

static void test_idle_time_gives_no_burst(void)
{
    ratelimit_t rl;
    struct timespec idle = { 0, 300000000L };
    double start;

    ratelimit_init(&rl, 10);
    ratelimit_wait(&rl, 1000000);
    while (nanosleep(&idle, &idle) != 0)
        ;
    start = now_s();
    ratelimit_wait(&rl, 1000000);   // idle longer than the debt: goes at once
    ratelimit_wait(&rl, 1000000);   // but the 300 ms idle is not banked
    assert(now_s() - start >= 0.09);
}

int main(void)
{
    test_unlimited_never_sleeps();
    test_paces_to_rate();
    test_idle_time_gives_no_burst();
    return 0;
}
