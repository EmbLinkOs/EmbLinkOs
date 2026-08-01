#ifndef EMBK_PKTLINE_H
#define EMBK_PKTLINE_H
/* Git pkt-line framing (Documentation/technical/protocol-common.txt): a stream
 * of length-prefixed chunks. Each chunk starts with 4 hex digits giving the
 * TOTAL length including those 4 bytes; 0000 is a "flush" marker and 0001 a
 * "delim" marker (both carry no data). */
#include <stddef.h>
#include <stdint.h>

enum pktline_kind {
    PKT_DATA  = 1,   /* a normal packet; data and dlen point into the buffer */
    PKT_FLUSH = 0,   /* 0000 */
    PKT_DELIM = 2,   /* 0001 */
    PKT_ERR   = -1,  /* truncated / malformed / end of buffer */
};

/* Decode the next pkt-line at *cur (bounded by end). On PKT_DATA, sets *data and
 * *dlen to the payload (may include a trailing '\n'). Always advances *cur past
 * the packet it consumed. */
int pktline_next(const uint8_t **cur, const uint8_t *end,
                 const uint8_t **data, size_t *dlen);

/* Write a pkt-line for `data` (dlen bytes) into out (>= dlen+4). Returns bytes
 * written (dlen+4). dlen must be <= 65516. */
size_t pktline_write(uint8_t *out, const void *data, size_t dlen);

/* Write a flush-pkt ("0000") into out (>= 4). Returns 4. */
size_t pktline_flush(uint8_t *out);

#endif /* EMBK_PKTLINE_H */
