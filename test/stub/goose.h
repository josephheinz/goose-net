/* Test stub for the parts of goose.h that src/loop.c uses: a builder over a malloc'd buffer. */
#ifndef STUB_GOOSE_H
#define STUB_GOOSE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *data; size_t len, cap; } gs_buf;
typedef gs_buf *gs_rref;

static inline void gs_bld_append(gs_rref b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 64;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
#endif
