#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/sha.h>
#include "pipeline.h"

static char dir[] = "/tmp/test_pipeline_XXXXXX";
static char in_path[64], out_path[64], man_path[64], metrics_prefix[64];

static config_t make_config(const char *input, size_t block_size, size_t queue_cap, int workers)
{
    return (config_t){ .input = input, .output = out_path, .manifest = man_path,
                       .block_size = block_size, .queue_cap = queue_cap, .workers = workers,
                       .mode = MODE_PIPELINE, .io = IO_RW, .seed = 1 };
}

static unsigned char *make_input(size_t size)
{
    unsigned char *data = malloc(size ? size : 1);
    FILE *f = fopen(in_path, "wb");

    assert(data != NULL && f != NULL);
    for (size_t i = 0; i < size; i++)
        data[i] = (unsigned char)(i * 131u + (i >> 9));
    assert(fwrite(data, 1, size, f) == size);
    assert(fclose(f) == 0);
    return data;
}

static unsigned char *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long n;

    assert(f != NULL);
    assert(fseek(f, 0, SEEK_END) == 0);
    n = ftell(f);
    assert(n >= 0);
    rewind(f);
    data = malloc((size_t)n + 1);
    assert(data != NULL);
    assert(fread(data, 1, (size_t)n, f) == (size_t)n);
    assert(fclose(f) == 0);
    *size = (size_t)n;
    return data;
}

/* Output must equal the input; the manifest (any order) must list every block exactly once with its SHA-256. */
static void check_copy(const unsigned char *data, size_t size, size_t block_size)
{
    size_t out_size, nblocks = (size + block_size - 1) / block_size, lines = 0;
    unsigned char *out = slurp(out_path, &out_size);
    bool *seen = calloc(nblocks + 1, sizeof(*seen));
    FILE *man = fopen(man_path, "r");
    uint64_t seq;
    intmax_t off;
    size_t len;
    char hex[65], want[65];

    assert(out_size == size);
    assert(memcmp(out, data, size) == 0);
    assert(man != NULL && seen != NULL);
    while (fscanf(man, "%" SCNu64 ",%jd,%zu,%64s\n", &seq, &off, &len, hex) == 4) {
        unsigned char sha[SHA256_DIGEST_LENGTH];

        assert(seq < nblocks && !seen[seq]);
        seen[seq] = true;
        assert((size_t)off == seq * block_size);
        assert(len == (size - (size_t)off < block_size ? size - (size_t)off : block_size));
        SHA256(data + off, len, sha);
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
            sprintf(want + 2 * i, "%02x", sha[i]);
        assert(strcmp(hex, want) == 0);
        lines++;
    }
    assert(feof(man));
    assert(lines == nblocks);
    assert(fclose(man) == 0);
    free(seen);
    free(out);
}

static void copy_and_check(size_t size, size_t block_size, size_t queue_cap, int workers, bool unbounded)
{
    unsigned char *data = make_input(size);
    config_t cfg = make_config(in_path, block_size, queue_cap, workers);

    cfg.unbounded = unbounded;
    assert(pipeline_run(&cfg) == 0);
    check_copy(data, size, block_size);
    free(data);
}

static void test_configurations(void)
{
    static const int workers[] = { 1, 2, 4, 8 };
    static const size_t queues[] = { 1, 4, 16 };

    for (size_t w = 0; w < sizeof workers / sizeof workers[0]; w++)
        for (size_t q = 0; q < sizeof queues / sizeof queues[0]; q++)
            copy_and_check(3 * 65536 + 4096 * 50 + 17, 4096, queues[q], workers[w], false);
    copy_and_check(0, 4096, 4, 4, false);            // empty input
    copy_and_check(1, 4096, 4, 4, false);            // one short block
    copy_and_check(8 * 4096, 4096, 2, 3, false);     // exact multiple
    copy_and_check(50000, 1, 1, 4, false);           // one byte per block: many tiny records
    copy_and_check(300000, 1 << 20, 4, 4, false);    // block larger than the file
    copy_and_check(500000, 4096, 1, 4, true);        // --unbounded
}

/* Short counts, fake EINTR/EAGAIN and delays on the reader's and writer's I/O: the copy must be exact. */
static void test_injected_faults(void)
{
    size_t size = 3 * 65536 + 1234;
    unsigned char *data = make_input(size);
    config_t cfg = make_config(in_path, 4096, 2, 4);

    cfg.fault_short = 0.5;
    cfg.fault_eintr = 0.3;
    cfg.fault_eagain = 0.3;
    cfg.fault_delay_us = 5;
    assert(pipeline_run(&cfg) == 0);
    check_copy(data, size, 4096);
    free(data);
}

static void test_overwrites_longer_existing_output(void)
{
    FILE *f = fopen(out_path, "wb");
    unsigned char junk[50000];

    assert(f != NULL);
    memset(junk, 0xee, sizeof junk);
    assert(fwrite(junk, 1, sizeof junk, f) == sizeof junk);
    assert(fclose(f) == 0);
    copy_and_check(1000, 256, 4, 4, false);
}

static void test_stdin_input(void)
{
    size_t size = 70000;
    unsigned char *data = make_input(size);
    config_t cfg = make_config("-", 4096, 4, 4);
    int saved = dup(STDIN_FILENO), fd = open(in_path, O_RDONLY);

    assert(saved >= 0 && fd >= 0);
    assert(dup2(fd, STDIN_FILENO) == STDIN_FILENO);
    close(fd);
    assert(pipeline_run(&cfg) == 0);
    assert(dup2(saved, STDIN_FILENO) == STDIN_FILENO);
    close(saved);
    check_copy(data, size, 4096);
    free(data);
}

static void test_metrics_csv(void)
{
    char path[96], line[256];
    unsigned char *data = make_input(100000);
    config_t cfg = make_config(in_path, 4096, 4, 4);
    FILE *f;

    cfg.metrics = metrics_prefix;
    assert(pipeline_run(&cfg) == 0);
    snprintf(path, sizeof path, "%s.summary.csv", metrics_prefix);
    f = fopen(path, "r");
    assert(f != NULL);
    assert(fgets(line, sizeof line, f) != NULL);
    assert(fgets(line, sizeof line, f) != NULL);
    assert(strncmp(line, "100000,25,", 10) == 0);   // bytes, blocks
    assert(fclose(f) == 0);
    unlink(path);
    snprintf(path, sizeof path, "%s.occupancy.csv", metrics_prefix);
    assert(access(path, R_OK) == 0);
    unlink(path);
    free(data);
}

static void test_errors_stop_cleanly(void)
{
    size_t size;
    unsigned char *data = make_input(5000), *after;
    config_t cfg = make_config("/nonexistent/input", 4096, 4, 4);

    assert(pipeline_run(&cfg) == 1);

    cfg = make_config(in_path, 4096, 4, 4);
    cfg.output = in_path;   // same file: must refuse instead of truncating the input
    assert(pipeline_run(&cfg) == 1);
    after = slurp(in_path, &size);
    assert(size == 5000 && memcmp(after, data, size) == 0);
    free(after);

    cfg = make_config(dir, 4096, 4, 4);   // a directory opens, then read() fails: reader error
    assert(pipeline_run(&cfg) == 1);

    if (access("/dev/full", W_OK) == 0) {   // every write fails with ENOSPC: writer error
        free(data);
        data = make_input(2000000);
        cfg = make_config(in_path, 4096, 2, 4);
        cfg.output = "/dev/full";
        assert(pipeline_run(&cfg) == 1);
    }
    free(data);
}

int main(void)
{
    assert(mkdtemp(dir) != NULL);
    snprintf(in_path, sizeof in_path, "%s/in.bin", dir);
    snprintf(out_path, sizeof out_path, "%s/out.bin", dir);
    snprintf(man_path, sizeof man_path, "%s/out.manifest", dir);
    snprintf(metrics_prefix, sizeof metrics_prefix, "%s/m", dir);

    test_configurations();
    test_injected_faults();
    test_overwrites_longer_existing_output();
    test_stdin_input();
    test_metrics_csv();
    test_errors_stop_cleanly();

    unlink(in_path);
    unlink(out_path);
    unlink(man_path);
    rmdir(dir);
    return 0;
}
