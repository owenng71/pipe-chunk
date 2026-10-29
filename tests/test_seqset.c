#include <assert.h>
#include "seqset.h"

/* The writer's duplicate check: a seq delivered twice is reported, neighbours stay separate. */
static void test_duplicates(void)
{
    seqset_t s = { 0 };

    assert(seqset_add(&s, 0) == 0);
    assert(seqset_add(&s, 0) == 1);
    assert(seqset_add(&s, 7) == 0 && seqset_add(&s, 8) == 0);
    assert(seqset_add(&s, 7) == 1 && seqset_add(&s, 8) == 1);
    assert(seqset_add(&s, 1) == 0);
    seqset_free(&s);
    assert(s.bits == NULL && s.bytes == 0);
}

/* A seq past the current bitmap grows it and keeps the seqs already added. */
static void test_growth(void)
{
    seqset_t s = { 0 };

    assert(seqset_add(&s, 3) == 0);
    assert(seqset_add(&s, 8191) == 0 && s.bytes == 1024);   // last bit of the first 1 KiB
    assert(seqset_add(&s, 8192) == 0 && s.bytes == 2048);
    assert(seqset_add(&s, 1000000) == 0 && s.bytes >= 1000000 / 8 + 1);
    assert(seqset_add(&s, 3) == 1 && seqset_add(&s, 8191) == 1);
    assert(seqset_add(&s, 8192) == 1 && seqset_add(&s, 1000000) == 1);
    assert(seqset_add(&s, 999999) == 0);
    seqset_free(&s);
}

int main(void)
{
    test_duplicates();
    test_growth();
    return 0;
}
