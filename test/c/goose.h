/* A stand-in for the goose.h that the Goose compiler supplies: just what the C sources use. */
#ifndef GOOSE_STUB_H
#define GOOSE_STUB_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *data; size_t len, cap; } gs_buf;
typedef gs_buf *gs_rref;

static inline void gs_bld_append(gs_rref b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        while (cap < b->len + n) cap *= 2;
        b->data = realloc(b->data, cap);
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

#endif
