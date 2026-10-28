/* pipeline.h — the two run modes. Both return a process exit code. */
#ifndef PIPELINE_H
#define PIPELINE_H

#include "common.h"

int pipeline_run(const config_t *cfg);   // --mode=pipeline
int baseline_run(const config_t *cfg);   // --mode=blocking

#endif
