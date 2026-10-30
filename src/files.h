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
    unsigned char *map;  // --io=mmap: whole input, read-only (PROT_READ); NULL for rw or an empty file
    size_t map_len;
} files_t;

/*
 * Opens cfg's input, output (truncated) and manifest. Refuses input == output. A stdin pipe or
 * socket is switched to O_NONBLOCK (read_full then waits in poll); files_close restores it.
 * With --io=mmap the input must be a regular file and is mapped whole. 0, or -1 after printing why.
 */
int  files_open(const config_t *cfg, files_t *f);

/*
 * Gets the n bytes at input offset off into r: rw reads them into r->buf; mmap points r->buf into
 * the mapping (no copy; pool_get restores the tray's own buffer). Bytes got, < n only at EOF; -1 on error.
 */
ssize_t files_read(const config_t *cfg, files_t *f, record_t *r, off_t off, size_t n);

/* Appends "seq,offset,len,sha256hex" for r. 0, or -1 after printing why. */
int  files_write_manifest(const config_t *cfg, files_t *f, const record_t *r);

/* If ok, syncs output and manifest; always unmaps, closes everything and restores stdin's flags. Returns 0 only if ok and every step worked. */
int  files_close(const config_t *cfg, files_t *f, bool ok);

/* Prints "pipe-chunk: WHAT PATH: strerror(errno)". */
void files_report(const char *what, const char *path);

#endif
