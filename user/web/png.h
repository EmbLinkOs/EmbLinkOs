/* user/web/png.h -- a PNG decoder, on top of our own DEFLATE.
 *
 * PNG because it is the format a document actually needs: lossless, alpha,
 * and the one every diagram, logo and screenshot on a documentation site is
 * written in. The heavy half already existed -- `inflate_raw` was written for
 * the package installer -- so this is chunk parsing, a zlib wrapper, and the
 * unfilter step, which is the part that is genuinely PNG's own.
 *
 * Output is BGRA premultiplied: exactly what the scene's image leaf wants, so
 * the decode lands in a buffer the compositor can blit with no conversion
 * pass. Straight alpha would look right and cost a second walk of every pixel.
 *
 * Bounded like everything downstream of a socket: the caller supplies the
 * pixel buffer AND a scratch buffer, and a picture that does not fit is
 * REFUSED rather than truncated -- a half-decoded image is a corrupt one, and
 * unlike a truncated document it cannot be usefully shown.
 */
#ifndef _EMBLINK_WEB_PNG_H_
#define _EMBLINK_WEB_PNG_H_

#include <stdint.h>
#include <stddef.h>

enum {
    PNG_OK = 0,
    PNG_ENOTPNG   = -1,   /* signature or chunk structure wrong        */
    PNG_EUNSUP    = -2,   /* interlaced, or a bit depth we do not do   */
    PNG_ETOOBIG   = -3,   /* would not fit the caller's buffers        */
    PNG_EDATA     = -4,   /* corrupt compressed data or filter byte    */
};

/* Read just the header. Cheap, and lets a caller reject a picture on its
 * DIMENSIONS before allocating anything for it. */
int png_probe(const uint8_t *src, size_t len, uint32_t *out_w, uint32_t *out_h);

/* Decode into `dst` as BGRA8888-premultiplied, `dst_cap` in BYTES.
 * `scratch`/`scratch_cap` hold the inflated raw scanlines: that needs
 * h * (1 + w * channels) bytes, so (w*h*4 + h + 64) is always enough.
 * On success writes the size into *out_w/*out_h and returns PNG_OK. */
int png_decode(const uint8_t *src, size_t len,
               uint32_t *dst, size_t dst_cap,
               uint8_t *scratch, size_t scratch_cap,
               uint32_t *out_w, uint32_t *out_h);

#endif /* _EMBLINK_WEB_PNG_H_ */
