/* files.c — input/output/manifest handling shared by both run modes. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files.h"

void files_report(const char *what, const char *path)
{
    fprintf(stderr, "pipe-chunk: %s %s: %s\n", what, path, strerror(errno));
}

static bool from_stdin(const config_t *cfg)
{
    return strcmp(cfg->input, "-") == 0;
}

int files_open(const config_t *cfg, files_t *f)
{
    struct stat in_st, out_st;

    *f = (files_t){ .in = -1, .out = -1, .manifest = NULL };
    f->in = from_stdin(cfg) ? STDIN_FILENO : open(cfg->input, O_RDONLY);
    if (f->in < 0) {
        files_report("cannot open input", cfg->input);
        goto fail;
    }
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
    /* EINVAL from fsync: the fd cannot be synced (e.g. /dev/null), which is not a copy failure. */
    if (ok && f->out >= 0 && fsync(f->out) < 0 && errno != EINVAL) {
        files_report("cannot sync", cfg->output);
        ok = false;
    }
    if (ok && f->manifest != NULL &&
        (fflush(f->manifest) != 0 || (fsync(fileno(f->manifest)) < 0 && errno != EINVAL))) {
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
    if (f->in >= 0 && !from_stdin(cfg))
        close(f->in);
    *f = (files_t){ .in = -1, .out = -1, .manifest = NULL };
    return ok ? 0 : -1;
}
