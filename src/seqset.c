/* seqset.c — growable bitmap of block sequence numbers. */
#include <stdlib.h>
#include <string.h>

#include "seqset.h"

int seqset_add(seqset_t *s, uint64_t seq)
{
    size_t byte = (size_t)(seq / 8);
    uint8_t bit = (uint8_t)(1u << (seq % 8));

    if (byte >= s->bytes) {
        size_t size = s->bytes ? s->bytes : 1024;
        uint8_t *b;

        while (size <= byte)
            size *= 2;
        b = realloc(s->bits, size);
        if (b == NULL)
            return -1;
        memset(b + s->bytes, 0, size - s->bytes);
        s->bits = b;
        s->bytes = size;
    }
    if (s->bits[byte] & bit)
        return 1;
    s->bits[byte] |= bit;
    return 0;
}

void seqset_free(seqset_t *s)
{
    free(s->bits);
    *s = (seqset_t){ 0 };
}
