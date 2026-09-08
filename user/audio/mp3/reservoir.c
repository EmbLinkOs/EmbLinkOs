/* user/audio/mp3/reservoir.c -- see reservoir.h. */
#include <string.h>

#include "reservoir.h"

/* How much history to keep after compaction. A frame can reach back 511 bytes,
 * so anything older can never be referenced again. */
#define KEEP 1024

void mp3_res_reset(Mp3Reservoir *r)
{
    r->len = 0;
}

long mp3_res_add(Mp3Reservoir *r, const uint8_t *data, int len,
                 int main_data_begin)
{
    if (len < 0) return -1;

    /* Compact BEFORE computing the start offset, never after: the offset is an
     * index into this buffer, and shifting the bytes afterwards would leave it
     * pointing at whatever moved into its place. */
    if (r->len + (size_t)len > MP3_RES_CAP) {
        size_t keep = r->len < KEEP ? r->len : KEEP;
        memmove(r->buf, r->buf + (r->len - keep), keep);
        r->len = keep;
    }

    /* Where this frame's granules begin, relative to everything received so
     * far -- which is the END of the buffer as it stands, before appending. */
    long start = (long)r->len - (long)main_data_begin;

    if ((size_t)len <= MP3_RES_CAP - r->len) {
        memcpy(r->buf + r->len, data, (size_t)len);
        r->len += (size_t)len;
    }

    /* Cold reservoir: this frame refers to bytes from frames we never saw.
     * The data is appended regardless, so the NEXT frame can still be decoded
     * -- refusing to buffer would make one cold frame poison the whole file. */
    if (start < 0) return -1;
    return start * 8;
}
