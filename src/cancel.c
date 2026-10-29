/* cancel.c — signal thread for graceful drain (first SIGINT/SIGTERM) and hard stop (second). */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

#include "cancel.h"

struct cancel {
    pthread_t thread;
    sigset_t set;          // SIGINT, SIGTERM, and SIGUSR1 to wake the thread for cancel_stop
    sigset_t old_mask;     // caller's mask, restored by cancel_stop
    void (*drain)(void *arg);
    void *arg;
    FILE *manifest;
    atomic_bool requested;
    atomic_bool stopping;
};

static void *signal_main(void *arg)
{
    cancel_t *c = arg;
    int sig;

    for (;;) {
        if (sigwait(&c->set, &sig) != 0)
            continue;
        if (sig == SIGUSR1) {
            if (atomic_load(&c->stopping))
                break;
            continue;   // limit: a SIGUSR1 from outside is ignored while a run is active
        }
        if (!atomic_exchange(&c->requested, true)) {
            if (c->drain != NULL)
                c->drain(c->arg);
        } else {
            /*
             * Hard stop: the one place a module ends the process (Overview 5.6). stdio locks the
             * stream, so the flush sees whole manifest lines only; each line is written after its
             * block's pwrite, so the manifest lists exactly the blocks that are complete.
             */
            if (c->manifest != NULL)
                (void)fflush(c->manifest);
            _exit(130);
        }
    }
    return NULL;
}

cancel_t *cancel_start(void (*drain)(void *arg), void *arg, FILE *manifest)
{
    cancel_t *c = calloc(1, sizeof(*c));
    int rc;

    if (c == NULL)
        return NULL;
    c->drain = drain;
    c->arg = arg;
    c->manifest = manifest;
    atomic_init(&c->requested, false);
    atomic_init(&c->stopping, false);
    sigemptyset(&c->set);
    sigaddset(&c->set, SIGINT);
    sigaddset(&c->set, SIGTERM);
    sigaddset(&c->set, SIGUSR1);
    rc = pthread_sigmask(SIG_BLOCK, &c->set, &c->old_mask);
    if (rc == 0) {
        rc = pthread_create(&c->thread, NULL, signal_main, c);
        if (rc != 0)
            (void)pthread_sigmask(SIG_SETMASK, &c->old_mask, NULL);
    }
    if (rc != 0) {
        free(c);
        errno = rc;
        return NULL;
    }
    return c;
}

bool cancel_requested(cancel_t *c)
{
    return c != NULL && atomic_load(&c->requested);
}

void cancel_stop(cancel_t *c)
{
    int saved_errno = errno;

    if (c == NULL)
        return;
    /* Not pthread_cancel: AddressSanitizer's runtime cannot unwind a cancelled thread. */
    atomic_store(&c->stopping, true);
    (void)pthread_kill(c->thread, SIGUSR1);   // to this thread only; it has SIGUSR1 blocked, so sigwait takes it
    (void)pthread_join(c->thread, NULL);
    (void)pthread_sigmask(SIG_SETMASK, &c->old_mask, NULL);
    free(c);
    errno = saved_errno;
}
