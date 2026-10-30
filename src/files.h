/* files.h — input/output/manifest handling shared by both run modes. */
#ifndef FILES_H
#define FILES_H

#include <stdio.h>

#include "common.h"

typedef struct files {
    int   in;          // input fd (stdin for "-"); used only by the reading stage
    int   out;         // output fd; used only by the writing stage
    FILE *manifest;    // used only by the writing stage
    int   in_flags;    // stdin's file-status flags before files_open set O_NONBLOCK; -1 = untouched
} files_t;

/*
 * Opens cfg's input, output (truncated) and manifest. Refuses input == output. A stdin pipe or
 * socket is switched to O_NONBLOCK (read_full then waits in poll); files_close restores it.
 * 0, or -1 after printing why.
 */
int  files_open(const config_t *cfg, files_t *f);

/* Appends "seq,offset,len,sha256hex" for r. 0, or -1 after printing why. */
int  files_write_manifest(const config_t *cfg, files_t *f, const record_t *r);

/* If ok, syncs output and manifest; always closes everything and restores stdin's flags. Returns 0 only if ok and every step worked. */
int  files_close(const config_t *cfg, files_t *f, bool ok);

/* Prints "pipe-chunk: WHAT PATH: strerror(errno)". */
void files_report(const char *what, const char *path);

#endif
