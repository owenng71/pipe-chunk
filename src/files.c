/* files.c — input/output/manifest handling shared by both run modes. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files.h"
#include "io.h"

void files_report(const char *what, const char *path)
{
    fprintf(stderr, "pipe-chunk: %s %s: %s\n", what, path, strerror(errno));
}

static bool from_stdin(const config_t *cfg)
{
    return strcmp(cfg->input, "-") == 0;
}

/*
 * A pipe or socket on stdin becomes nonblocking, so a slow producer shows up as EAGAIN + poll.
 * --stdin-blocking skips this (read sleeps in the kernel instead), for comparing the two.
 * A terminal is left alone: it shares its open file description with the shell, which would stay
 * nonblocking if the hard stop's _exit skipped files_close. Regular files ignore O_NONBLOCK.
 */
static int make_stdin_nonblocking(files_t *f)
{
    struct stat st;
    int flags;

    if (fstat(f->in, &st) < 0)
        return -1;
    if (!S_ISFIFO(st.st_mode) && !S_ISSOCK(st.st_mode))
        return 0;
    flags = fcntl(f->in, F_GETFL);
    if (flags < 0)
        return -1;
    if (flags & O_NONBLOCK)
        return 0;   // already nonblocking: nothing to restore
    if (fcntl(f->in, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    f->in_flags = flags;
    return 0;
}

/*
 * --io=mmap: map the whole input once, read-only. Only a regular file named by path has a fixed
 * size to map; an empty one cannot be mapped (EINVAL) and simply has no blocks.
 * limit: if another program truncates the input during the run, touching the lost pages raises
 * SIGBUS and the process dies; read() would just see an early EOF.
 */
static int map_input(const config_t *cfg, files_t *f)
{
    struct stat st;

    if (from_stdin(cfg)) {   // even a redirected file: its read position need not be 0
        fprintf(stderr, "pipe-chunk: --io=mmap cannot read stdin; give the input file's path\n");
        return -1;
    }
    if (fstat(f->in, &st) < 0) {
        files_report("cannot stat input", cfg->input);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "pipe-chunk: --io=mmap needs a regular file as input: %s\n", cfg->input);
        return -1;
    }
    if (st.st_size == 0)
        return 0;
    f->map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, f->in, 0);
    if (f->map == MAP_FAILED) {
        f->map = NULL;
        files_report("cannot mmap input", cfg->input);
        return -1;
    }
    f->map_len = (size_t)st.st_size;
    return 0;
}

int files_open(const config_t *cfg, files_t *f)
{
    struct stat in_st, out_st;

    *f = (files_t){ .in = -1, .out = -1, .manifest = NULL, .in_flags = -1 };
    f->in = from_stdin(cfg) ? STDIN_FILENO : open(cfg->input, O_RDONLY);
    if (f->in < 0) {
        files_report("cannot open input", cfg->input);
        goto fail;
    }
    if (from_stdin(cfg) && !cfg->stdin_blocking && make_stdin_nonblocking(f) < 0) {
        files_report("cannot make nonblocking", "stdin");
        goto fail;
    }
    if (cfg->io == IO_MMAP && map_input(cfg, f) < 0)
        goto fail;
    f->out = open(cfg->output, O_WRONLY | O_CREAT, 0644);
    if (f->out < 0) {
        files_report("cannot open output", cfg->output);
        goto fail;
    }
    /* Truncating first would destroy the input if both names point to the same file. */
    if (fstat(f->in, &in_st) == 0 && fstat(f->out, &out_st) == 0 && S_ISREG(in_st.st_mode) &&
        in_st.st_dev == out_st.st_dev && in_st.st_ino == out_st.st_ino) {
        fprintf(stderr, "pipe-chunk: input and output are the same file\n");
        goto fail;
    }
    if (ftruncate(f->out, 0) < 0 && errno != EINVAL) {   // EINVAL: not a regular file (e.g. /dev/null)
        files_report("cannot truncate output", cfg->output);
        goto fail;
    }
    f->manifest = fopen(cfg->manifest, "w");
    if (f->manifest == NULL) {
        files_report("cannot open manifest", cfg->manifest);
        goto fail;
    }
    return 0;

fail:
    (void)files_close(cfg, f, false);
    return -1;
}

ssize_t files_read(const config_t *cfg, files_t *f, record_t *r, off_t off, size_t n)
{
    size_t left;

    if (cfg->io != IO_MMAP)
        return read_full(f->in, r->buf, n);
    left = (size_t)off < f->map_len ? f->map_len - (size_t)off : 0;
    if (n > left)
        n = left;   // EOF
    if (n > 0)
        r->buf = f->map + off;   // no copy: the worker hashing it is the first to touch (fault in) the pages
    return (ssize_t)n;
}

int files_write_manifest(const config_t *cfg, files_t *f, const record_t *r)
{
    static const char digits[] = "0123456789abcdef";
    char hex[2 * sizeof r->sha + 1];

    for (size_t i = 0; i < sizeof r->sha; i++) {
        hex[2 * i] = digits[r->sha[i] >> 4];
        hex[2 * i + 1] = digits[r->sha[i] & 0xf];
    }
    hex[2 * sizeof r->sha] = '\0';
    if (fprintf(f->manifest, "%" PRIu64 ",%jd,%zu,%s\n", r->seq, (intmax_t)r->offset, r->len, hex) < 0) {
        files_report("write error on", cfg->manifest);
        return -1;
    }
    return 0;
}

int files_close(const config_t *cfg, files_t *f, bool ok)
{
    /* Flush before the slow output fsync: the signal thread is gone, so a Ctrl+C there would drop the buffer. */
    if (ok && f->manifest != NULL && fflush(f->manifest) != 0) {
        files_report("write error on", cfg->manifest);
        ok = false;
    }
    /* EINVAL from fsync: the fd cannot be synced (e.g. /dev/null), which is not a copy failure. */
    if (ok && f->out >= 0 && fsync(f->out) < 0 && errno != EINVAL) {
        files_report("cannot sync", cfg->output);
        ok = false;
    }
    if (ok && f->manifest != NULL && fsync(fileno(f->manifest)) < 0 && errno != EINVAL) {
        files_report("cannot sync", cfg->manifest);
        ok = false;
    }
    if (f->manifest != NULL && fclose(f->manifest) != 0 && ok) {
        files_report("cannot close", cfg->manifest);
        ok = false;
    }
    if (f->out >= 0 && close(f->out) < 0 && ok) {
        files_report("cannot close", cfg->output);
        ok = false;
    }
    // limit: the hard stop's _exit(130) skips this, leaving a stdin pipe nonblocking (only we read it)
    if (f->in_flags >= 0)
        (void)fcntl(f->in, F_SETFL, f->in_flags);   // best effort: the copy itself is already done
    if (f->map != NULL)
        (void)munmap(f->map, f->map_len);   // every stage has stopped: no record points into it any more
    if (f->in >= 0 && !from_stdin(cfg))
        close(f->in);
    *f = (files_t){ .in = -1, .out = -1, .manifest = NULL, .in_flags = -1 };
    return ok ? 0 : -1;
}
