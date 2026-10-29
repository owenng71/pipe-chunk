/* baseline.c — --mode=blocking: single-threaded read → SHA-256 → pwrite → manifest. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <openssl/sha.h>

#include "cancel.h"
#include "faults.h"
#include "files.h"
#include "io.h"
#include "metrics.h"
#include "pipeline.h"
#include "ratelimit.h"

int baseline_run(const config_t *cfg)
{
    files_t f;
    metrics_t *m;
    cancel_t *cancel = NULL;
    metrics_summary_t summary = { 0 };
    ratelimit_t in_rate, out_rate;
    record_t r = { .cap = cfg->block_size };
    uint64_t start = 0, end;
    bool ok = false, interrupted;

    faults_init(cfg);
    if (files_open(cfg, &f) < 0)
        return 1;
    m = metrics_create(true, cfg->metrics);
    r.buf = malloc(cfg->block_size);
    if (m == NULL || r.buf == NULL || metrics_thread_register(m) < 0) {
        files_report("cannot set up run for", cfg->input);
        goto done;
    }
    cancel = cancel_start(NULL, NULL, f.manifest);
    if (cancel == NULL) {
        files_report("cannot start signal thread for", cfg->input);
        goto done;
    }
    ratelimit_init(&in_rate, cfg->in_rate);
    ratelimit_init(&out_rate, cfg->out_rate);
    (void)metrics_now_ns(&start);

    for (;;) {
        ssize_t n;
        uint64_t now;

        if (cancel_requested(cancel))
            break;   // Ctrl+C: finish here, every listed block is complete
        n = read_full(f.in, r.buf, cfg->block_size);
        if (n < 0) {
            files_report("read error on", cfg->input);
            goto done;
        }
        if (n == 0)
            break;
        ratelimit_wait(&in_rate, (size_t)n);
        (void)metrics_now_ns(&r.t_read_ns);
        r.len = (size_t)n;
        SHA256(r.buf, r.len, r.sha);
        ratelimit_wait(&out_rate, r.len);
        if (pwrite_full(f.out, r.buf, r.len, r.offset) < 0) {
            files_report("write error on", cfg->output);
            goto done;
        }
        if (files_write_manifest(cfg, &f, &r) < 0)
            goto done;
        (void)metrics_now_ns(&now);
        (void)metrics_record_block(m, r.t_read_ns, now);
        summary.bytes += r.len;
        summary.blocks++;
        r.seq++;
        r.offset += n;
        if (r.len < cfg->block_size)
            break;   // read_full is short only at EOF
    }
    ok = true;

done:
    interrupted = cancel_requested(cancel);
    cancel_stop(cancel);   // before the manifest it may flush is closed
    ok = files_close(cfg, &f, ok) == 0 && ok;
    if (ok && interrupted)
        fprintf(stderr, "pipe-chunk: interrupted: %" PRIu64 " blocks written and listed in the manifest\n", summary.blocks);
    free(r.buf);
    if (m != NULL) {
        metrics_thread_unregister();
        if (ok) {
            (void)metrics_now_ns(&end);
            summary.wall_ns = end - start;
            if (metrics_merge(m, &summary) == 0)
                (void)metrics_write_csv(m, &summary);
            metrics_print_summary(&summary);
        }
        metrics_destroy(m);
    }
    return !ok ? 1 : interrupted ? 130 : 0;
}
