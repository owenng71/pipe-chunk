#include <assert.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "cancel.h"

static atomic_int drains;
static void *drain_arg;

static void count_drain(void *arg)
{
    drain_arg = arg;
    atomic_fetch_add(&drains, 1);
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };

    nanosleep(&ts, NULL);
}

/* Waits up to 5 s for the signal thread to see the first signal. */
static void wait_requested(cancel_t *c)
{
    for (int i = 0; i < 500 && !cancel_requested(c); i++)
        sleep_ms(10);
    assert(cancel_requested(c));
}

static bool blocked(int sig)
{
    sigset_t cur;

    assert(pthread_sigmask(SIG_BLOCK, NULL, &cur) == 0);
    return sigismember(&cur, sig);
}

static void test_start_stop_restores_mask(void)
{
    cancel_t *c;

    assert(!blocked(SIGINT) && !blocked(SIGTERM) && !blocked(SIGUSR1));
    c = cancel_start(count_drain, NULL, NULL);
    assert(c != NULL);
    assert(blocked(SIGINT) && blocked(SIGTERM) && blocked(SIGUSR1));
    assert(!cancel_requested(c));
    cancel_stop(c);
    assert(!blocked(SIGINT) && !blocked(SIGTERM) && !blocked(SIGUSR1));
    assert(atomic_load(&drains) == 0);
    assert(!cancel_requested(NULL));
    cancel_stop(NULL);
}

/* The first SIGINT or SIGTERM runs drain once, with its argument, and does not end the process. */
static void test_first_signal_drains(int sig)
{
    int token;
    cancel_t *c;

    atomic_store(&drains, 0);
    c = cancel_start(count_drain, &token, NULL);
    assert(c != NULL);
    assert(kill(getpid(), sig) == 0);
    wait_requested(c);
    sleep_ms(20);
    assert(atomic_load(&drains) == 1 && drain_arg == &token);
    cancel_stop(c);
    assert(!blocked(sig));
}

/* The second signal flushes the manifest's buffered lines and exits with 130. Runs in a child. */
static void test_second_signal_hard_stops(void)
{
    char path[] = "/tmp/test_cancel_XXXXXX";
    int fd = mkstemp(path), status;
    char line[64] = "";
    pid_t pid;
    FILE *f;

    assert(fd >= 0);
    close(fd);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        FILE *man = fopen(path, "w");
        cancel_t *c = cancel_start(count_drain, NULL, man);

        if (man == NULL || c == NULL || fputs("0,0,1,abc\n", man) < 0)   // stays in the stdio buffer
            _exit(1);
        kill(getpid(), SIGINT);
        wait_requested(c);   // signals do not queue: two quick kills could merge into one
        kill(getpid(), SIGINT);
        sleep_ms(5000);
        _exit(2);   // not reached if the hard stop works
    }
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 130);
    f = fopen(path, "r");
    assert(f != NULL && fgets(line, sizeof line, f) != NULL);
    assert(strcmp(line, "0,0,1,abc\n") == 0);
    fclose(f);
    unlink(path);
}

/* A NULL drain is allowed: the caller polls cancel_requested instead. */
static void test_null_drain(void)
{
    cancel_t *c = cancel_start(NULL, NULL, NULL);

    assert(c != NULL);
    assert(kill(getpid(), SIGTERM) == 0);
    wait_requested(c);
    cancel_stop(c);
}

int main(void)
{
    test_start_stop_restores_mask();
    test_first_signal_drains(SIGINT);
    test_first_signal_drains(SIGTERM);
    test_second_signal_hard_stops();
    test_null_drain();
    return 0;
}
