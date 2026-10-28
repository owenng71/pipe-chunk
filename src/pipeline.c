/* pipeline.c — --mode=pipeline: reader, N workers, writer, supervisor. Not implemented yet. */
#include <stdio.h>
#include "pipeline.h"

int pipeline_run(const config_t *cfg) { (void)cfg; fprintf(stderr, "pipe-chunk: --mode=pipeline not implemented yet\n"); return 1; }
