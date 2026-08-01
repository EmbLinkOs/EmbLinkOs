#ifndef EMBK_UNZIP_H
#define EMBK_UNZIP_H
/* Minimal in-memory ZIP reader -- enough to extract a Python wheel (a ZIP of
 * STORED or DEFLATE entries). Iterates the central directory and hands each
 * file's decompressed bytes to a callback. No writing, no CRC check (the TLS
 * layer already authenticated the download). */
#include <stdint.h>
#include <stddef.h>

/* Called once per file entry with its name and fully-decompressed contents.
 * Return 0 to continue, non-zero to stop with that error. `data` is valid only
 * for the duration of the call. */
typedef int (*unzip_cb)(void *ctx, const char *name, size_t name_len,
                        const uint8_t *data, size_t data_len);

/* Walk the ZIP in `zip`[0..zip_len). Returns 0 on success, <0 on a malformed
 * archive, or the callback's non-zero return. */
int unzip_iter(const uint8_t *zip, size_t zip_len, unzip_cb cb, void *ctx);

#endif /* EMBK_UNZIP_H */
