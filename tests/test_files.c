#include <assert.h>
#include <fcntl.h>
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

static bool stdin_nonblocking(void)
{
    int flags = fcntl(STDIN_FILENO, F_GETFL);

    assert(flags >= 0);
    return (flags & O_NONBLOCK) != 0;
}

/* Puts fd on stdin; returns a dup of the old stdin to hand to restore_stdin. */
static int replace_stdin(int fd)
{
    int saved = dup(STDIN_FILENO);

    assert(saved >= 0);
    assert(dup2(fd, STDIN_FILENO) == STDIN_FILENO);
    close(fd);
    return saved;
}

static void restore_stdin(int saved)
{
    assert(dup2(saved, STDIN_FILENO) == STDIN_FILENO);
    close(saved);
}

static void test_stdin_pipe_is_nonblocking_while_open(void)
{
    config_t cfg = make_config();
    files_t f;
    int p[2], saved;

    cfg.input = "-";
    assert(pipe(p) == 0);
    saved = replace_stdin(p[0]);
    assert(!stdin_nonblocking());
    assert(files_open(&cfg, &f) == 0);
    assert(f.in == STDIN_FILENO && stdin_nonblocking());
    assert(files_close(&cfg, &f, true) == 0);
    assert(!stdin_nonblocking());                  // original flags restored
    assert(fcntl(STDIN_FILENO, F_GETFD) >= 0);     // stdin is not closed

    /* Already nonblocking before the run: stays that way. */
    assert(fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK) == 0);
    assert(files_open(&cfg, &f) == 0);
    assert(files_close(&cfg, &f, true) == 0);
    assert(stdin_nonblocking());

    /* Restored on the failure path too. */
    assert(fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) & ~O_NONBLOCK) == 0);
    cfg.manifest = "/nonexistent/dir/manifest";
    assert(files_open(&cfg, &f) == -1);
    assert(!stdin_nonblocking());

    /* --stdin-blocking: the pipe is left blocking and there is nothing to restore. */
    cfg = make_config();
    cfg.input = "-";
    cfg.stdin_blocking = true;
    assert(files_open(&cfg, &f) == 0);
    assert(!stdin_nonblocking() && f.in_flags == -1);
    assert(files_close(&cfg, &f, true) == 0);
    assert(!stdin_nonblocking());

    restore_stdin(saved);
    close(p[1]);
}

static void test_stdin_file_is_left_alone(void)
{
    config_t cfg = make_config();
    files_t f;
    int fd, saved;

    write_text(in_path, "input");
    fd = open(in_path, O_RDONLY);
    assert(fd >= 0);
    saved = replace_stdin(fd);
    cfg.input = "-";
    assert(files_open(&cfg, &f) == 0);
    assert(!stdin_nonblocking() && f.in_flags == -1);
    assert(files_close(&cfg, &f, true) == 0);
    restore_stdin(saved);
}

/* --io=mmap: blocks point into the mapping (no copy), the last one is short, then EOF. */
static void test_mmap_read(void)
{
    config_t cfg = make_config();
    files_t f;
    unsigned char own[4096];
    record_t r = { .buf = own, .cap = sizeof own };

    cfg.io = IO_MMAP;
    write_text(in_path, "0123456789");
    assert(files_open(&cfg, &f) == 0);
    assert(f.map != NULL && f.map_len == 10);
    assert(files_read(&cfg, &f, &r, 0, 4) == 4 && r.buf == f.map && memcmp(r.buf, "0123", 4) == 0);
    assert(files_read(&cfg, &f, &r, 8, 4) == 2 && r.buf == f.map + 8 && memcmp(r.buf, "89", 2) == 0);
    assert(files_read(&cfg, &f, &r, 10, 4) == 0);
    assert(files_close(&cfg, &f, true) == 0 && f.map == NULL);

    write_text(in_path, "");   // an empty file cannot be mapped; it just has no blocks
    assert(files_open(&cfg, &f) == 0 && f.map == NULL);
    assert(files_read(&cfg, &f, &r, 0, 4) == 0);
    assert(files_close(&cfg, &f, true) == 0);

    cfg.input = "/dev/zero";   // not a regular file
    assert(files_open(&cfg, &f) == -1);
    assert(f.in == -1 && f.out == -1 && f.manifest == NULL);
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
    test_stdin_pipe_is_nonblocking_while_open();
    test_stdin_file_is_left_alone();
    test_mmap_read();

    unlink(in_path);
    unlink(out_path);
    unlink(man_path);
    rmdir(dir);
    return 0;
}
