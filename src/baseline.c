/* baseline.c — --mode=blocking: single-threaded read → SHA-256 → pwrite → manifest. Not implemented yet. */
#include <stdio.h>
#include "pipeline.h"

int baseline_run(const config_t *cfg) { (void)cfg; fprintf(stderr, "pipe-chunk: --mode=blocking not implemented yet\n"); return 1; }
