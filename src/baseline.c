/* baseline.c — --mode=blocking: single-threaded read → SHA-256 → pwrite → manifest. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/sha.h>

#include "io.h"
#include "pipeline.h"

static void sha_hex(const unsigned char sha[SHA256_DIGEST_LENGTH], char hex[2 * SHA256_DIGEST_LENGTH + 1])
{
    static const char digits[] = "0123456789abcdef";

    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        hex[2 * i] = digits[sha[i] >> 4];
        hex[2 * i + 1] = digits[sha[i] & 0xf];
    }
    hex[2 * SHA256_DIGEST_LENGTH] = '\0';
}

static void report(const char *what, const char *path)
{
    fprintf(stderr, "pipe-chunk: %s %s: %s\n", what, path, strerror(errno));
}

int baseline_run(const config_t *cfg)
{
    bool from_stdin = strcmp(cfg->input, "-") == 0;
    int in = -1, out = -1;
    FILE *man = NULL;
    unsigned char *buf = NULL;
    unsigned char sha[SHA256_DIGEST_LENGTH];
    char hex[2 * SHA256_DIGEST_LENGTH + 1];
    struct stat in_st, out_st;
    uint64_t seq = 0;
    off_t off = 0;
    int status = 1;

    in = from_stdin ? STDIN_FILENO : open(cfg->input, O_RDONLY);
    if (in < 0) {
        report("cannot open input", cfg->input);
        goto done;
    }
    out = open(cfg->output, O_WRONLY | O_CREAT, 0644);
    if (out < 0) {
        report("cannot open output", cfg->output);
        goto done;
    }
    /* Truncating first would destroy the input if both names point to the same file. */
    if (fstat(in, &in_st) == 0 && fstat(out, &out_st) == 0 && S_ISREG(in_st.st_mode) &&
        in_st.st_dev == out_st.st_dev && in_st.st_ino == out_st.st_ino) {
        fprintf(stderr, "pipe-chunk: input and output are the same file\n");
        goto done;
    }
    if (ftruncate(out, 0) < 0) {
        report("cannot truncate output", cfg->output);
        goto done;
    }
    man = fopen(cfg->manifest, "w");
    if (man == NULL) {
        report("cannot open manifest", cfg->manifest);
        goto done;
    }
    buf = malloc(cfg->block_size);
    if (buf == NULL) {
        report("cannot allocate block buffer for", cfg->input);
        goto done;
    }

    for (;;) {
        ssize_t n = read_full(in, buf, cfg->block_size);

        if (n < 0) {
            report("read error on", cfg->input);
            goto done;
        }
        if (n == 0)
            break;
        SHA256(buf, (size_t)n, sha);
        if (pwrite_full(out, buf, (size_t)n, off) < 0) {
            report("write error on", cfg->output);
            goto done;
        }
        sha_hex(sha, hex);
        if (fprintf(man, "%" PRIu64 ",%jd,%zd,%s\n", seq, (intmax_t)off, n, hex) < 0) {
            report("write error on", cfg->manifest);
            goto done;
        }
        seq++;
        off += n;
        if ((size_t)n < cfg->block_size)
            break;   // read_full is short only at EOF
    }

    /* EINVAL: the fd cannot be synced (e.g. /dev/null), which is not a copy failure. */
    if (fsync(out) < 0 && errno != EINVAL) {
        report("cannot sync", cfg->output);
        goto done;
    }
    if (fflush(man) != 0 || (fsync(fileno(man)) < 0 && errno != EINVAL)) {
        report("cannot sync", cfg->manifest);
        goto done;
    }
    status = 0;

done:
    free(buf);
    if (man != NULL && fclose(man) != 0 && status == 0) {
        report("cannot close", cfg->manifest);
        status = 1;
    }
    if (out >= 0 && close(out) < 0 && status == 0) {
        report("cannot close", cfg->output);
        status = 1;
    }
    if (in >= 0 && !from_stdin)
        close(in);
    return status;
}
