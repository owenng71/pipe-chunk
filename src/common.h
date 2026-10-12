/* common.h — types shared by every module (record + config). Frozen after W1-03. */
#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MAX_ATTEMPTS 3

/* One block in transit ("tray"). Owned by exactly one stage at a time — see Overview §5.2. */
typedef struct record {
    uint64_t       seq;          // block number 0..N-1
    off_t          offset;       // byte position in input/output
    size_t         len;          // bytes actually in buf (last block may be shorter)
    size_t         cap;          // buffer capacity (= block size)
    unsigned char *buf;          // the data
    unsigned char  sha[32];      // SHA-256 of buf[0..len)
    uint64_t       t_read_ns;    // timestamp when read (for latency)
    int            attempts;     // how many times a worker tried this record
} record_t;

typedef enum { MODE_PIPELINE, MODE_BLOCKING } run_mode_t;
typedef enum { IO_RW, IO_MMAP } io_kind_t;

typedef struct config {
    const char *input;           // path, or "-" for stdin (nonblocking path)
    const char *output;
    const char *manifest;        // default OUTPUT.manifest
    const char *metrics;         // CSV prefix, NULL = no CSV
    size_t      block_size;
    size_t      queue_cap;
    int         workers;
    run_mode_t  mode;
    io_kind_t   io;
    bool        unbounded;
    double      in_rate, out_rate;            // MB/s, 0 = unlimited
    double      fault_short, fault_eintr, fault_eagain, fail_worker; // probabilities 0..1
    unsigned    fault_delay_us;
    unsigned long seed;
} config_t;

#endif
