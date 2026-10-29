#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <openssl/sha.h>
#include "pipeline.h"

static char dir[] = "/tmp/test_baseline_XXXXXX";
static char in_path[64], out_path[64], man_path[64];

static config_t make_config(const char *input, size_t block_size)
{
    return (config_t){ .input = input, .output = out_path, .manifest = man_path,
                       .block_size = block_size, .queue_cap = 16, .workers = 1,
                       .mode = MODE_BLOCKING, .io = IO_RW, .seed = 1 };
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

/* Output must equal the input, and the manifest must list each block once, in order, with its SHA-256. */
static void check_copy(const unsigned char *data, size_t size, size_t block_size)
{
    size_t out_size, expected_blocks = (size + block_size - 1) / block_size;
    unsigned char *out = slurp(out_path, &out_size);
    FILE *man = fopen(man_path, "r");
    uint64_t seq;
    intmax_t off;
    size_t len, blocks = 0;
    char hex[65], want[65];

    assert(out_size == size);
    assert(memcmp(out, data, size) == 0);
    assert(man != NULL);
    while (fscanf(man, "%" SCNu64 ",%jd,%zu,%64s\n", &seq, &off, &len, hex) == 4) {
        unsigned char sha[SHA256_DIGEST_LENGTH];

        assert(seq == blocks);
        assert((size_t)off == blocks * block_size);
        assert(len == (size - (size_t)off < block_size ? size - (size_t)off : block_size));
        SHA256(data + off, len, sha);
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
            sprintf(want + 2 * i, "%02x", sha[i]);
        assert(strcmp(hex, want) == 0);
        blocks++;
    }
    assert(feof(man));
    assert(blocks == expected_blocks);
    assert(fclose(man) == 0);
    free(out);
}

static void copy_and_check(size_t size, size_t block_size)
{
    unsigned char *data = make_input(size);
    config_t cfg = make_config(in_path, block_size);

    assert(baseline_run(&cfg) == 0);
    check_copy(data, size, block_size);
    free(data);
}

static void test_sizes(void)
{
    copy_and_check(0, 4096);              // empty input: empty output and manifest
    copy_and_check(1, 4096);              // one short block
    copy_and_check(4 * 4096, 4096);       // exact multiple: no empty trailing block
    copy_and_check(3 * 4096 + 17, 4096);  // short last block
    copy_and_check(100000, 1);            // one byte per block
    copy_and_check(300000, 1 << 20);      // block larger than the file
}

/* Short counts, fake EINTR/EAGAIN and delays on every read and write: the copy must be exact. */
static void test_injected_faults(void)
{
    size_t size = 3 * 65536 + 1234;
    unsigned char *data = make_input(size);
    config_t cfg = make_config(in_path, 4096);

    cfg.fault_short = 0.5;
    cfg.fault_eintr = 0.3;
    cfg.fault_eagain = 0.3;
    cfg.fault_delay_us = 5;
    assert(baseline_run(&cfg) == 0);
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
    copy_and_check(1000, 256);   // stale tail must be gone
}

static void test_stdin_input(void)
{
    size_t size = 70000;
    unsigned char *data = make_input(size);
    config_t cfg = make_config("-", 4096);
    int saved = dup(STDIN_FILENO), fd = open(in_path, O_RDONLY);

    assert(saved >= 0 && fd >= 0);
    assert(dup2(fd, STDIN_FILENO) == STDIN_FILENO);
    close(fd);
    assert(baseline_run(&cfg) == 0);
    assert(dup2(saved, STDIN_FILENO) == STDIN_FILENO);
    close(saved);
    check_copy(data, size, 4096);
    free(data);
}

static void test_errors(void)
{
    size_t size;
    unsigned char *data = make_input(5000), *after;
    config_t cfg = make_config("/nonexistent/input", 4096);

    assert(baseline_run(&cfg) == 1);

    cfg = make_config(in_path, 4096);
    cfg.output = in_path;   // same file: must refuse instead of truncating the input
    assert(baseline_run(&cfg) == 1);
    after = slurp(in_path, &size);
    assert(size == 5000 && memcmp(after, data, size) == 0);

    cfg = make_config(in_path, 4096);
    cfg.output = "/nonexistent/output";
    assert(baseline_run(&cfg) == 1);
    free(after);
    free(data);
}

int main(void)
{
    assert(mkdtemp(dir) != NULL);
    snprintf(in_path, sizeof in_path, "%s/in.bin", dir);
    snprintf(out_path, sizeof out_path, "%s/out.bin", dir);
    snprintf(man_path, sizeof man_path, "%s/out.manifest", dir);

    test_sizes();
    test_injected_faults();
    test_overwrites_longer_existing_output();
    test_stdin_input();
    test_errors();

    unlink(in_path);
    unlink(out_path);
    unlink(man_path);
    rmdir(dir);
    return 0;
}
