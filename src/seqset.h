/* seqset.h — growable bitmap of block sequence numbers: the writer's "already written" set. */
#ifndef SEQSET_H
#define SEQSET_H

#include "common.h"

/* Zero-initialized = empty. Not thread-safe: one owner (the writer). */
typedef struct seqset {
    uint8_t *bits;
    size_t   bytes;
} seqset_t;

/* Adds seq. Returns 1 if it was already present, 0 if newly added, -1 with errno ENOMEM. */
int seqset_add(seqset_t *s, uint64_t seq);

/* Frees the bitmap and leaves s empty. */
void seqset_free(seqset_t *s);

#endif
