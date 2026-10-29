/* cancel.h — Ctrl+C handling: a sigwait thread turns the first SIGINT/SIGTERM into a graceful drain, the second into a hard stop. */
#ifndef CANCEL_H
#define CANCEL_H

#include <stdio.h>

#include "common.h"

typedef struct cancel cancel_t;

/*
 * Blocks SIGINT, SIGTERM and SIGUSR1 (used by cancel_stop) in the calling thread, so threads it
 * creates later inherit the mask, and starts the signal thread. On the first SIGINT/SIGTERM,
 * drain(arg) runs in the signal thread (if drain is not NULL); it must not block. On the second,
 * the thread flushes `manifest` (if not NULL) and ends the process with _exit(130). Returns NULL
 * with errno on failure, with the mask restored.
 */
cancel_t *cancel_start(void (*drain)(void *arg), void *arg, FILE *manifest);

/* True once the first signal has arrived. NULL c = false. */
bool cancel_requested(cancel_t *c);

/*
 * Stops and joins the signal thread, then restores the caller's signal mask. A signal that arrives
 * after the stop gets its default action. Call before closing `manifest`. NULL is a no-op.
 */
void cancel_stop(cancel_t *c);

#endif
