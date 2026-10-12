/* main.c — CLI parsing (Overview §7) and mode selection. */
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pipeline.h"

static void usage(FILE *f)
{
    fputs("usage: pipe-chunk [options] INPUT OUTPUT\n"
          "  -b, --block-size=BYTES     block size, K/M/G suffix ok (default 65536)\n"
          "  -q, --queue=N              capacity of Q1 and Q2 (default 16)\n"
          "  -w, --workers=N            worker threads (default 4)\n"
          "      --mode=blocking|pipeline\n"
          "      --io=rw|mmap\n"
          "      --unbounded            disable backpressure (memory-growth demo)\n"
          "      --in-rate=MB/s         throttle reader\n"
          "      --out-rate=MB/s        throttle writer\n"
          "      --fault-short=P        prob. of short read/write\n"
          "      --fault-eintr=P        prob. of fake EINTR\n"
          "      --fault-eagain=P       prob. of fake EAGAIN\n"
          "      --fault-delay-us=US    random extra delay per I/O\n"
          "      --fail-worker=P        prob. a worker dies mid-record\n"
          "      --seed=N               RNG seed\n"
          "      --manifest=FILE        default OUTPUT.manifest\n"
          "      --metrics=PREFIX       writes PREFIX.summary.csv and PREFIX.occupancy.csv\n"
          "INPUT may be '-' for stdin.\n", f);
}

static void die_usage(const char *opt, const char *val)
{
    fprintf(stderr, "pipe-chunk: invalid value '%s' for %s\n", val, opt);
    usage(stderr);
    exit(2);
}

/* Unsigned integer with optional K/M/G (binary) suffix. */
static unsigned long long parse_size(const char *opt, const char *s)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s || *s == '-') die_usage(opt, s);
    switch (*end) {
    case 'K': case 'k': v <<= 10; end++; break;
    case 'M': case 'm': v <<= 20; end++; break;
    case 'G': case 'g': v <<= 30; end++; break;
    }
    if (*end) die_usage(opt, s);
    return v;
}

static double parse_double(const char *opt, const char *s, double lo, double hi)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || end == s || *end || v < lo || v > hi) die_usage(opt, s);
    return v;
}

enum { O_MODE = 256, O_IO, O_UNBOUNDED, O_IN_RATE, O_OUT_RATE, O_F_SHORT, O_F_EINTR,
       O_F_EAGAIN, O_F_DELAY, O_FAIL_WORKER, O_SEED, O_MANIFEST, O_METRICS, O_HELP };

int main(int argc, char **argv)
{
    config_t cfg = { .block_size = 65536, .queue_cap = 16, .workers = 4,
                     .mode = MODE_PIPELINE, .io = IO_RW, .seed = 1 };
    static const struct option opts[] = {
        { "block-size",     required_argument, 0, 'b' },
        { "queue",          required_argument, 0, 'q' },
        { "workers",        required_argument, 0, 'w' },
        { "mode",           required_argument, 0, O_MODE },
        { "io",             required_argument, 0, O_IO },
        { "unbounded",      no_argument,       0, O_UNBOUNDED },
        { "in-rate",        required_argument, 0, O_IN_RATE },
        { "out-rate",       required_argument, 0, O_OUT_RATE },
        { "fault-short",    required_argument, 0, O_F_SHORT },
        { "fault-eintr",    required_argument, 0, O_F_EINTR },
        { "fault-eagain",   required_argument, 0, O_F_EAGAIN },
        { "fault-delay-us", required_argument, 0, O_F_DELAY },
        { "fail-worker",    required_argument, 0, O_FAIL_WORKER },
        { "seed",           required_argument, 0, O_SEED },
        { "manifest",       required_argument, 0, O_MANIFEST },
        { "metrics",        required_argument, 0, O_METRICS },
        { "help",           no_argument,       0, O_HELP },
        { 0, 0, 0, 0 }
    };
    const char *name = NULL;   // long option name for error messages
    int c, idx;

    while ((c = getopt_long(argc, argv, "b:q:w:", opts, &idx)) != -1) {
        name = c == 'b' ? "--block-size" : c == 'q' ? "--queue" : c == 'w' ? "--workers"
             : c >= 256 ? opts[idx].name : "?";
        switch (c) {
        case 'b': cfg.block_size = parse_size(name, optarg);
                  if (cfg.block_size == 0 || cfg.block_size > (1ULL << 30)) die_usage(name, optarg);
                  break;
        case 'q': cfg.queue_cap = parse_size(name, optarg);
                  if (cfg.queue_cap == 0) die_usage(name, optarg);
                  break;
        case 'w': { unsigned long long w = parse_size(name, optarg);
                    if (w == 0 || w > 1024) die_usage(name, optarg);
                    cfg.workers = (int)w; } break;
        case O_MODE:
            if (!strcmp(optarg, "blocking")) cfg.mode = MODE_BLOCKING;
            else if (!strcmp(optarg, "pipeline")) cfg.mode = MODE_PIPELINE;
            else die_usage(name, optarg);
            break;
        case O_IO:
            if (!strcmp(optarg, "rw")) cfg.io = IO_RW;
            else if (!strcmp(optarg, "mmap")) cfg.io = IO_MMAP;
            else die_usage(name, optarg);
            break;
        case O_UNBOUNDED:   cfg.unbounded = true; break;
        case O_IN_RATE:     cfg.in_rate = parse_double(name, optarg, 0, 1e9); break;
        case O_OUT_RATE:    cfg.out_rate = parse_double(name, optarg, 0, 1e9); break;
        case O_F_SHORT:     cfg.fault_short = parse_double(name, optarg, 0, 1); break;
        case O_F_EINTR:     cfg.fault_eintr = parse_double(name, optarg, 0, 1); break;
        case O_F_EAGAIN:    cfg.fault_eagain = parse_double(name, optarg, 0, 1); break;
        case O_F_DELAY:     cfg.fault_delay_us = (unsigned)parse_size(name, optarg); break;
        case O_FAIL_WORKER: cfg.fail_worker = parse_double(name, optarg, 0, 1); break;
        case O_SEED:        cfg.seed = (unsigned long)parse_size(name, optarg); break;
        case O_MANIFEST:    cfg.manifest = optarg; break;
        case O_METRICS:     cfg.metrics = optarg; break;
        case O_HELP:        usage(stdout); return 0;
        default:            usage(stderr); return 2;
        }
    }
    if (argc - optind != 2) { usage(stderr); return 2; }
    cfg.input = argv[optind];
    cfg.output = argv[optind + 1];

    static char manifest_buf[4096];
    if (!cfg.manifest) {
        if ((size_t)snprintf(manifest_buf, sizeof manifest_buf, "%s.manifest", cfg.output) >= sizeof manifest_buf) {
            fprintf(stderr, "pipe-chunk: output path too long\n");
            return 2;
        }
        cfg.manifest = manifest_buf;
    }

    return cfg.mode == MODE_BLOCKING ? baseline_run(&cfg) : pipeline_run(&cfg);
}
