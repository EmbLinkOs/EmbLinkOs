/* user/web/jpeg.h -- a baseline JPEG decoder.
 *
 * PNG was the format a document needs; JPEG is the format the web actually
 * has. It is also a genuinely different problem: PNG is a filter and a
 * DEFLATE stream we already owned, while JPEG is Huffman coding, a frequency
 * transform and a chroma-subsampled colour space -- four algorithms with
 * nothing shared with anything else in this OS.
 *
 * BASELINE sequential (SOF0) only. Progressive JPEG is a different decoder
 * wearing the same file extension -- it delivers coefficients across multiple
 * scans and needs the whole coefficient buffer live between them -- and is
 * REFUSED rather than half-decoded, for the same reason interlaced PNG is.
 *
 * Output is BGRA premultiplied, matching png.h, so imgcache does not care
 * which decoder produced a picture.
 */
#ifndef _EMBLINK_WEB_JPEG_H_
#define _EMBLINK_WEB_JPEG_H_

#include <stdint.h>
#include <stddef.h>

enum {
    JPG_OK       =  0,
    JPG_ENOTJPG  = -1,   /* no SOI, or no frame we recognise   */
    JPG_EUNSUP   = -2,   /* progressive, arithmetic, CMYK, ... */
    JPG_ETOOBIG  = -3,   /* would not fit the caller's buffers */
    JPG_EDATA    = -4,   /* corrupt entropy-coded data         */
};

/* Is this a JPEG, and how big? Reads the frame header only. */
int jpeg_probe(const uint8_t *src, size_t len, uint32_t *out_w, uint32_t *out_h);

/* Decode to BGRA premultiplied. `scratch` holds the component planes:
 * w*h*3 + 4096 bytes is always enough (three 8-bit planes at full size). */
int jpeg_decode(const uint8_t *src, size_t len,
                uint32_t *dst, size_t dst_cap,
                uint8_t *scratch, size_t scratch_cap,
                uint32_t *out_w, uint32_t *out_h);

#endif /* _EMBLINK_WEB_JPEG_H_ */
