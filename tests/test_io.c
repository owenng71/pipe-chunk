#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "io.h"

/*
 * Scripted syscalls. This file defines read() and pwrite(), so io.c's calls bind here instead of
 * libc. Each call consumes the next scripted step; with no steps left it is the real syscall.
 */
enum { STEP_SHORT, STEP_FAIL, STEP_ZERO };
typedef struct { int kind; size_t max; int err; } step_t;

static step_t script[16];
static size_t script_len, script_pos;
static unsigned read_calls, pwrite_calls;

static void set_script(const step_t *steps, size_t n)
{
    assert(n <= sizeof script / sizeof script[0]);
    memcpy(script, steps, n * sizeof *steps);
    script_len = n;
    script_pos = 0;
    read_calls = 0;
    pwrite_calls = 0;
}

/* Applies the next step. Returns 1 when the call must return `*ret` without a syscall. */
static int next_step(size_t *n, ssize_t *ret)
{
    step_t s;

    if (script_pos == script_len)
        return 0;
    s = script[script_pos++];
    if (s.kind == STEP_FAIL) {
        errno = s.err;
        *ret = -1;
        return 1;
    }
    if (s.kind == STEP_ZERO) {
        *ret = 0;
        return 1;
    }
    if (*n > s.max)
        *n = s.max;
    return 0;
}

ssize_t read(int fd, void *buf, size_t n)
{
    ssize_t ret;

    read_calls++;
    if (next_step(&n, &ret))
        return ret;
    return syscall(SYS_read, fd, buf, n);
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    ssize_t ret;

    pwrite_calls++;
    if (next_step(&n, &ret))
        return ret;
    return syscall(SYS_pwrite64, fd, buf, n, off);
}

static void fill_pattern(unsigned char *buf, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        buf[i] = (unsigned char)(i * 31u + seed);
}

static void write_all(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;

    while (n > 0) {
        ssize_t w = write(fd, p, n);

        assert(w > 0);
        p += w;
        n -= (size_t)w;
    }
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    while (nanosleep(&ts, &ts) != 0)
        assert(errno == EINTR);
}

static int temp_file(void)
{
    char path[] = "/tmp/test_io_XXXXXX";
    int fd = mkstemp(path);

    assert(fd >= 0);
    assert(unlink(path) == 0);
    return fd;
}

static void test_read_short_counts(void)
{
    unsigned char in[100], out[100];
    int p[2];
    step_t steps[] = { { STEP_SHORT, 1, 0 }, { STEP_SHORT, 7, 0 }, { STEP_SHORT, 30, 0 } };

    fill_pattern(in, sizeof in, 1);
    assert(pipe(p) == 0);
    write_all(p[1], in, sizeof in);
    set_script(steps, 3);
    assert(read_full(p[0], out, sizeof out) == (ssize_t)sizeof out);
    assert(memcmp(in, out, sizeof in) == 0);
    assert(read_calls == 4);
    close(p[0]);
    close(p[1]);
}

static void test_read_retries_eintr_and_eagain_and_keeps_errno(void)
{
    unsigned char in[64], out[64];
    int p[2];
    step_t steps[] = { { STEP_FAIL, 0, EINTR }, { STEP_SHORT, 10, 0 }, { STEP_FAIL, 0, EAGAIN },
                       { STEP_FAIL, 0, EINTR } };

    fill_pattern(in, sizeof in, 2);
    assert(pipe(p) == 0);
    write_all(p[1], in, sizeof in);
    set_script(steps, 4);
    errno = ENOTTY;
    assert(read_full(p[0], out, sizeof out) == (ssize_t)sizeof out);
    assert(errno == ENOTTY);
    assert(memcmp(in, out, sizeof in) == 0);
    close(p[0]);
    close(p[1]);
}

static void test_read_eof(void)
{
    unsigned char in[10], out[100];
    int p[2];

    fill_pattern(in, sizeof in, 3);
    assert(pipe(p) == 0);
    write_all(p[1], in, sizeof in);
    close(p[1]);
    set_script(NULL, 0);
    assert(read_full(p[0], out, sizeof out) == (ssize_t)sizeof in);
    assert(memcmp(in, out, sizeof in) == 0);
    assert(read_full(p[0], out, sizeof out) == 0);
    close(p[0]);
}

static void test_read_errors(void)
{
    unsigned char buf[8];
    int p[2];
    step_t eio[] = { { STEP_SHORT, 3, 0 }, { STEP_FAIL, 0, EIO } };

    set_script(NULL, 0);
    errno = 0;
    assert(read_full(-1, buf, sizeof buf) == -1);
    assert(errno == EBADF);

    assert(pipe(p) == 0);
    write_all(p[1], "abcdefgh", 8);
    set_script(eio, 2);
    assert(read_full(p[0], buf, sizeof buf) == -1);
    assert(errno == EIO);

    set_script(NULL, 0);
    assert(read_full(p[0], buf, 0) == 0);
    assert(read_calls == 0);
    close(p[0]);
    close(p[1]);
}

typedef struct {
    int fd;
    const unsigned char *data;
    size_t len;
    pthread_t target;   // thread to interrupt with SIGUSR1, if signals > 0
    int signals;
} feeder_t;

/* Waits, optionally interrupts the reader, then writes `data` in three pieces and closes. */
static void *feeder(void *arg)
{
    feeder_t *f = arg;
    size_t third = f->len / 3;

    sleep_ms(50);
    for (int i = 0; i < f->signals; i++) {
        assert(pthread_kill(f->target, SIGUSR1) == 0);
        sleep_ms(20);
    }
    write_all(f->fd, f->data, third);
    sleep_ms(20);
    write_all(f->fd, f->data + third, third);
    sleep_ms(20);
    write_all(f->fd, f->data + 2 * third, f->len - 2 * third);
    close(f->fd);
    return NULL;
}

static void test_read_real_nonblocking_pipe(void)
{
    static unsigned char in[3000], out[3000];
    int p[2];
    pthread_t t;
    feeder_t f;

    fill_pattern(in, sizeof in, 4);
    assert(pipe(p) == 0);
    assert(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0);
    f = (feeder_t){ .fd = p[1], .data = in, .len = sizeof in };
    set_script(NULL, 0);
    assert(pthread_create(&t, NULL, feeder, &f) == 0);
    assert(read_full(p[0], out, sizeof out) == (ssize_t)sizeof out);
    assert(pthread_join(t, NULL) == 0);
    assert(memcmp(in, out, sizeof in) == 0);
    assert(read_full(p[0], out, sizeof out) == 0);
    close(p[0]);
}

static atomic_int signals_seen;

static void on_sigusr1(int sig)
{
    (void)sig;
    atomic_fetch_add(&signals_seen, 1);
}

static void test_read_real_signal_interrupts(void)
{
    static unsigned char in[3000], out[3000];
    struct sigaction sa, old;
    int p[2];
    pthread_t t;
    feeder_t f;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigusr1;   // no SA_RESTART: a blocked read() fails with EINTR
    assert(sigemptyset(&sa.sa_mask) == 0);
    assert(sigaction(SIGUSR1, &sa, &old) == 0);

    fill_pattern(in, sizeof in, 5);
    assert(pipe(p) == 0);
    f = (feeder_t){ .fd = p[1], .data = in, .len = sizeof in, .target = pthread_self(), .signals = 3 };
    set_script(NULL, 0);
    assert(pthread_create(&t, NULL, feeder, &f) == 0);
    assert(read_full(p[0], out, sizeof out) == (ssize_t)sizeof out);
    assert(pthread_join(t, NULL) == 0);
    assert(memcmp(in, out, sizeof in) == 0);
    assert(atomic_load(&signals_seen) >= 1);   // pending standard signals may merge (TSan defers them)
    close(p[0]);
    assert(sigaction(SIGUSR1, &old, NULL) == 0);
}

static void test_pwrite_short_interrupted_at_offset(void)
{
    static unsigned char in[5000], back[4096 + 5000];
    int fd = temp_file();
    step_t steps[] = { { STEP_SHORT, 3, 0 }, { STEP_FAIL, 0, EINTR }, { STEP_SHORT, 500, 0 },
                       { STEP_FAIL, 0, EAGAIN }, { STEP_SHORT, 1, 0 } };

    fill_pattern(in, sizeof in, 6);
    set_script(steps, 5);
    errno = ENOTTY;
    assert(pwrite_full(fd, in, sizeof in, 4096) == 0);
    assert(errno == ENOTTY);
    assert(pwrite_calls == 6);
    assert(pread(fd, back, sizeof back, 0) == (ssize_t)sizeof back);
    for (size_t i = 0; i < 4096; i++)
        assert(back[i] == 0);
    assert(memcmp(back + 4096, in, sizeof in) == 0);

    /* Rewriting the same block at the same offset is idempotent. */
    set_script(NULL, 0);
    assert(pwrite_full(fd, in, sizeof in, 4096) == 0);
    assert(pread(fd, back, sizeof back, 0) == (ssize_t)sizeof back);
    assert(memcmp(back + 4096, in, sizeof in) == 0);
    close(fd);
}

static void test_pwrite_errors(void)
{
    unsigned char buf[16] = { 0 };
    int fd = temp_file();
    int p[2];
    step_t zero[] = { { STEP_SHORT, 4, 0 }, { STEP_ZERO, 0, 0 } };
    step_t enospc[] = { { STEP_FAIL, 0, ENOSPC } };

    set_script(zero, 2);
    assert(pwrite_full(fd, buf, sizeof buf, 0) == -1);
    assert(errno == EIO);

    set_script(enospc, 1);
    assert(pwrite_full(fd, buf, sizeof buf, 0) == -1);
    assert(errno == ENOSPC);

    set_script(NULL, 0);
    assert(pwrite_full(-1, buf, sizeof buf, 0) == -1);
    assert(errno == EBADF);
    assert(pwrite_full(fd, buf, sizeof buf, -1) == -1);
    assert(errno == EINVAL);
    set_script(NULL, 0);
    assert(pwrite_full(fd, buf, 0, 0) == 0);
    assert(pwrite_calls == 0);

    assert(pipe(p) == 0);
    assert(pwrite_full(p[1], buf, sizeof buf, 0) == -1);
    assert(errno == ESPIPE);
    close(p[0]);
    close(p[1]);
    close(fd);
}

int main(void)
{
    test_read_short_counts();
    test_read_retries_eintr_and_eagain_and_keeps_errno();
    test_read_eof();
    test_read_errors();
    test_read_real_nonblocking_pipe();
    test_read_real_signal_interrupts();
    test_pwrite_short_interrupted_at_offset();
    test_pwrite_errors();
    return 0;
}
