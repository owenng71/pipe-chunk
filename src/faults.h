/* faults.h — fault injection for io.c (--fault-*, --seed): fake short counts, EINTR, EAGAIN, delays. */
#ifndef FAULTS_H
#define FAULTS_H

#include "common.h"

typedef enum { FAULT_READ, FAULT_WRITE } fault_dir_t;

/*
 * Sets the process-wide fault settings from cfg and restarts the random streams. Call before any
 * thread does I/O. Each direction has its own stream seeded from cfg->seed, so a run with one reader
 * and one writer makes the same decisions every time, whatever the thread scheduling.
 */
void faults_init(const config_t *cfg);

/*
 * Called by io.c before each read/pwrite of *n bytes. May sleep up to fault_delay_us. Returns 0 to go
 * ahead (*n may be lowered to force a short count), or -1 with errno EINTR or EAGAIN to fake a failed call.
 */
int faults_before_io(fault_dir_t dir, size_t *n);

#endif
