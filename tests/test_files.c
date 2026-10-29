#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "files.h"

static char dir[] = "/tmp/test_files_XXXXXX";
static char in_path[64], out_path[64], man_path[64];

static config_t make_config(void)
{
    return (config_t){ .input = in_path, .output = out_path, .manifest = man_path, .block_size = 4096 };
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    assert(f != NULL);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static void test_manifest_line_format(void)
{
    config_t cfg = make_config();
    files_t f;
    record_t r = { .seq = 42, .offset = 172032, .len = 17 };
    char line[128];
    FILE *man;

    write_text(in_path, "input");
    write_text(out_path, "stale output that must be truncated");
    for (size_t i = 0; i < sizeof r.sha; i++)
        r.sha[i] = (unsigned char)(i * 8 + 1);
    assert(files_open(&cfg, &f) == 0);
    assert(files_write_manifest(&cfg, &f, &r) == 0);
    assert(files_close(&cfg, &f, true) == 0);
    assert(f.in == -1 && f.out == -1 && f.manifest == NULL);

    man = fopen(man_path, "r");
    assert(man != NULL);
    assert(fgets(line, sizeof line, man) != NULL);
    assert(strcmp(line, "42,172032,17,"
                        "0109111921293139414951596169717981899199a1a9b1b9c1c9d1d9e1e9f1f9\n") == 0);
    assert(fgets(line, sizeof line, man) == NULL);
    assert(fclose(man) == 0);
    assert(access(out_path, F_OK) == 0);
    {
        FILE *out = fopen(out_path, "r");

        assert(out != NULL && fgetc(out) == EOF);   // truncated
        assert(fclose(out) == 0);
    }
}

static void test_open_failures_close_everything(void)
{
    config_t cfg = make_config();
    files_t f;

    cfg.output = in_path;   // same file
    assert(files_open(&cfg, &f) == -1);
    assert(f.in == -1 && f.out == -1 && f.manifest == NULL);

    cfg = make_config();
    cfg.manifest = "/nonexistent/dir/manifest";
    assert(files_open(&cfg, &f) == -1);
    assert(f.in == -1 && f.out == -1 && f.manifest == NULL);

    cfg = make_config();
    cfg.input = "/nonexistent/input";
    assert(files_open(&cfg, &f) == -1);
}

static void test_dev_null_output_is_allowed(void)
{
    config_t cfg = make_config();
    files_t f;

    cfg.output = "/dev/null";   // cannot be truncated or synced; that is not an error
    assert(files_open(&cfg, &f) == 0);
    assert(files_close(&cfg, &f, true) == 0);
}

int main(void)
{
    assert(mkdtemp(dir) != NULL);
    snprintf(in_path, sizeof in_path, "%s/in", dir);
    snprintf(out_path, sizeof out_path, "%s/out", dir);
    snprintf(man_path, sizeof man_path, "%s/man", dir);

    test_manifest_line_format();
    test_open_failures_close_everything();
    test_dev_null_output_is_allowed();

    unlink(in_path);
    unlink(out_path);
    unlink(man_path);
    rmdir(dir);
    return 0;
}
