/* pipeline.h — the two run modes. Both return a process exit code. */
#ifndef PIPELINE_H
#define PIPELINE_H

#include "common.h"

int pipeline_run(const config_t *cfg);   // --mode=pipeline (Phat, W2-01)
int baseline_run(const config_t *cfg);   // --mode=blocking (Vy, W1-10)

#endif
