/* Ciphers done somewhere else. A key is installed once as a SESSION and every
 * later operation names it; the control queue lives after all the data
 * queues. See the .c. */
#ifndef _EMBK_VIRTIO_CRYPTO_H_
#define _EMBK_VIRTIO_CRYPTO_H_
#include <stdint.h>
#include "include/types.h"
bool     virtio_crypto_init(void);
bool     virtio_crypto_present(void);
uint32_t virtio_crypto_ops(void);
/* One AES-CBC operation. `len` must be a whole number of 16-byte blocks. */
int virtio_crypto_aes_cbc(bool encrypt, const uint8_t *key, uint32_t keylen,
                          const uint8_t iv[16], const void *src, void *dst,
                          uint32_t len);
#endif
