#ifndef EMBK_INFLATE_H
#define EMBK_INFLATE_H
/* DEFLATE decompression (RFC 1951) -- a from-scratch inflate, no zlib. Written
 * for the native package installer (unzip a wheel), but reusable anywhere the OS
 * meets DEFLATE-compressed data (ZIP entries use raw DEFLATE, method 8). Decodes
 * a raw DEFLATE stream (no zlib/gzip wrapper) into a caller-provided buffer. */
#include <stdint.h>
#include <stddef.h>

/* Inflate `src`[0..src_len) into `dst`[0..dst_cap). On success returns 0 and sets
 * *out_len to the produced size; returns <0 on a malformed stream or if the
 * output would exceed dst_cap. */
int inflate_raw(const uint8_t *src, size_t src_len,
                uint8_t *dst, size_t dst_cap, size_t *out_len);

#endif /* EMBK_INFLATE_H */
