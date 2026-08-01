/* Git pkt-line framing -- see pktline.h. */
#include "pktline.h"
#include <string.h>

static int hexval(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int pktline_next(const uint8_t **cur, const uint8_t *end,
                 const uint8_t **data, size_t *dlen) {
    const uint8_t *p = *cur;
    if (p + 4 > end) { *cur = end; return PKT_ERR; }

    int h0 = hexval(p[0]), h1 = hexval(p[1]), h2 = hexval(p[2]), h3 = hexval(p[3]);
    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) { *cur = end; return PKT_ERR; }
    unsigned n = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);

    if (n == 0) { *cur = p + 4; return PKT_FLUSH; }
    if (n == 1) { *cur = p + 4; return PKT_DELIM; }
    if (n < 4 || p + n > end) { *cur = end; return PKT_ERR; }

    *data = p + 4;
    *dlen = n - 4;
    *cur = p + n;
    return PKT_DATA;
}

static void put_hex4(uint8_t *out, unsigned n) {
    static const char hx[] = "0123456789abcdef";
    out[0] = hx[(n >> 12) & 0xf];
    out[1] = hx[(n >> 8) & 0xf];
    out[2] = hx[(n >> 4) & 0xf];
    out[3] = hx[n & 0xf];
}

size_t pktline_write(uint8_t *out, const void *data, size_t dlen) {
    unsigned total = (unsigned)dlen + 4;
    put_hex4(out, total);
    memcpy(out + 4, data, dlen);
    return dlen + 4;
}

size_t pktline_flush(uint8_t *out) {
    memcpy(out, "0000", 4);
    return 4;
}
