/* kernel/drivers/crypto/virtio_crypto.c -- ciphers done somewhere else.
 *
 * This kernel already has AES: kernel/crypto/aes.c, used by EMBKFS's
 * encryption. It is a correct, portable, software implementation, and on a
 * machine with no AES instructions it is also the slowest part of writing to
 * an encrypted disk. virtio-crypto is the host offering to do that work --
 * with its own hardware, or its own instructions, or its own library.
 *
 * TWO QUEUES AND TWO LIFETIMES. A key is not sent with each block. It is
 * installed once on the CONTROL queue, which answers with a SESSION ID, and
 * every later operation on the DATA queue names that session instead of
 * carrying the key. That is the whole shape of the device, and it is why a
 * driver that only ever sends data requests gets nothing back: there is
 * nothing to do them with.
 *
 * THE DATA QUEUES COME FIRST AND THE CONTROL QUEUE IS AFTER THEM. Its index
 * is not fixed -- it is `max_dataqueues`, out of configuration space. Assuming
 * queue 1 works on a device with one data queue and silently addresses a data
 * queue on a device with two.
 *
 * WHAT IS NOT DONE HERE: this offloads a cipher and nothing above it decides
 * to use it yet. EMBKFS still encrypts in software. Wiring a filesystem's
 * crypto to a device that may or may not be present is a policy change, and
 * it belongs in a commit about the filesystem, not about the driver.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "drivers/bus/virtio_pci.h"
#include "drivers/crypto/virtio_crypto.h"
#include "mm/pmm.h"

#define VIRTIO_CRYPTO_DEVID 0x1054   /* 0x1040 + device type 20 */

/* Configuration space. */
#define VC_CFG_STATUS          0
#define VC_CFG_MAX_DATAQUEUES  4
#define VC_CFG_CRYPTO_SERVICES 8
#define VC_CFG_CIPHER_ALGO_L   12
#define VC_CFG_MAX_CIPHER_KEY  40

#define VC_SERVICE_CIPHER 0
#define VC_OPCODE(service, op) (((uint32_t)(service) << 8) | (uint32_t)(op))
#define VC_CIPHER_ENCRYPT        VC_OPCODE(VC_SERVICE_CIPHER, 0x00)
#define VC_CIPHER_DECRYPT        VC_OPCODE(VC_SERVICE_CIPHER, 0x01)
#define VC_CIPHER_CREATE_SESSION VC_OPCODE(VC_SERVICE_CIPHER, 0x02)
#define VC_CIPHER_DESTROY_SESSION VC_OPCODE(VC_SERVICE_CIPHER, 0x03)

#define VC_ALGO_AES_CBC 3

#define VC_OP_ENCRYPT 1
#define VC_OP_DECRYPT 2
#define VC_SYM_OP_CIPHER 1
#define VC_STATUS_OK 0

#define VC_QSIZE 8
#define VC_MAX_DATA 4096

struct vc_ctrl_header {
    uint32_t opcode;
    uint32_t algo;
    uint32_t flag;
    uint32_t reserved;
} __attribute__((packed));

struct vc_cipher_session_para {
    uint32_t algo;
    uint32_t keylen;
    uint32_t op;
    uint32_t padding;
} __attribute__((packed));

/* The layouts below are padded to the sizes the specification fixes. The
 * padding is not slack: the device reads the fields after it at those exact
 * offsets, so a struct that is merely "big enough" puts op_type somewhere the
 * device is not looking. */
struct vc_ctrl_req {
    struct vc_ctrl_header header;
    union {
        struct {
            struct vc_cipher_session_para para;
            uint8_t padding[32];
            uint32_t op_type;
            uint32_t padding2;
        } sym_create;
        struct {
            uint64_t session_id;
            uint8_t  padding[48];
        } destroy;
        uint8_t padding[56];
    } u;
} __attribute__((packed));

struct vc_session_input {
    uint64_t session_id;
    uint32_t status;
    uint32_t padding;
} __attribute__((packed));

struct vc_op_header {
    uint32_t opcode;
    uint32_t algo;
    uint64_t session_id;
    uint32_t flag;
    uint32_t padding;
} __attribute__((packed));

struct vc_data_req {
    struct vc_op_header header;
    union {
        struct {
            struct { uint32_t iv_len, src_len, dst_len, padding; } para;
            uint8_t padding[24];
            uint32_t op_type;
            uint32_t padding2;
        } sym;
        uint8_t padding[48];
    } u;
} __attribute__((packed));

static struct virtio_pci_dev g_vd;
static bool g_up;
static uint32_t g_data_queues;

/* The control queue. */
static struct vring_desc g_cdesc[VC_QSIZE] __attribute__((aligned(16)));
static struct { uint16_t flags, idx; uint16_t ring[VC_QSIZE]; uint16_t ue; }
    __attribute__((packed, aligned(2))) g_cavail;
static struct { uint16_t flags, idx; struct vring_used_elem ring[VC_QSIZE]; uint16_t ae; }
    __attribute__((packed, aligned(4))) g_cused;
static uint16_t g_cnotify, g_cqsize, g_clast;

/* Data queue 0. */
static struct vring_desc g_ddesc[VC_QSIZE] __attribute__((aligned(16)));
static struct { uint16_t flags, idx; uint16_t ring[VC_QSIZE]; uint16_t ue; }
    __attribute__((packed, aligned(2))) g_davail;
static struct { uint16_t flags, idx; struct vring_used_elem ring[VC_QSIZE]; uint16_t ae; }
    __attribute__((packed, aligned(4))) g_dused;
static uint16_t g_dnotify, g_dqsize, g_dlast;

static struct vc_ctrl_req      g_ctrl  __attribute__((aligned(64)));
static struct vc_session_input g_sess  __attribute__((aligned(64)));
static struct vc_data_req      g_req   __attribute__((aligned(64)));
static uint8_t g_key[64]  __attribute__((aligned(64)));
static uint8_t g_iv[16]   __attribute__((aligned(64)));
static uint8_t g_src[VC_MAX_DATA] __attribute__((aligned(64)));
static uint8_t g_dst[VC_MAX_DATA] __attribute__((aligned(64)));
static uint8_t g_status   __attribute__((aligned(64)));
static uint32_t g_ops;

static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

static bool ring_wait(volatile uint16_t *used_idx, uint16_t *last) {
    for (int spin = 0; spin < 20000000; spin++) {
        if (*used_idx != *last) { (*last)++; return true; }
        __asm__ volatile("" ::: "memory");
    }
    return false;
}

/* Install a key and get back a session id. The key travels as its own
 * device-readable descriptor, not inside the request. */
static bool vc_create_session(const uint8_t *key, uint32_t keylen, bool encrypt,
                              uint64_t *out_session) {
    if (keylen > sizeof g_key) return false;
    memset(&g_ctrl, 0, sizeof g_ctrl);
    g_ctrl.header.opcode = VC_CIPHER_CREATE_SESSION;
    g_ctrl.header.algo = VC_ALGO_AES_CBC;
    g_ctrl.u.sym_create.para.algo = VC_ALGO_AES_CBC;
    g_ctrl.u.sym_create.para.keylen = keylen;
    g_ctrl.u.sym_create.para.op = encrypt ? VC_OP_ENCRYPT : VC_OP_DECRYPT;
    g_ctrl.u.sym_create.op_type = VC_SYM_OP_CIPHER;
    memcpy(g_key, key, keylen);
    memset(&g_sess, 0, sizeof g_sess);
    g_sess.status = 0xFFFFFFFFu;

    g_cdesc[0].addr = dma(&g_ctrl); g_cdesc[0].len = sizeof g_ctrl;
    g_cdesc[0].flags = VRING_DESC_F_NEXT; g_cdesc[0].next = 1;
    g_cdesc[1].addr = dma(g_key); g_cdesc[1].len = keylen;
    g_cdesc[1].flags = VRING_DESC_F_NEXT; g_cdesc[1].next = 2;
    g_cdesc[2].addr = dma(&g_sess); g_cdesc[2].len = sizeof g_sess;
    g_cdesc[2].flags = VRING_DESC_F_WRITE; g_cdesc[2].next = 0;

    g_cavail.ring[g_cavail.idx % g_cqsize] = 0;
    __sync_synchronize();
    g_cavail.idx++;
    __sync_synchronize();
    virtio_pci_notify(&g_vd, g_cnotify, (uint16_t)g_data_queues);

    if (!ring_wait(&g_cused.idx, &g_clast)) {
        kprintf("virtio-crypto: the device never answered a session request\n");
        return false;
    }
    if (g_sess.status != VC_STATUS_OK) {
        kprintf("virtio-crypto: session refused (status %u)\n", g_sess.status);
        return false;
    }
    *out_session = g_sess.session_id;
    return true;
}

static void vc_destroy_session(uint64_t session) {
    memset(&g_ctrl, 0, sizeof g_ctrl);
    g_ctrl.header.opcode = VC_CIPHER_DESTROY_SESSION;
    g_ctrl.header.algo = VC_ALGO_AES_CBC;
    g_ctrl.u.destroy.session_id = session;
    g_status = 0xFF;

    g_cdesc[0].addr = dma(&g_ctrl); g_cdesc[0].len = sizeof g_ctrl;
    g_cdesc[0].flags = VRING_DESC_F_NEXT; g_cdesc[0].next = 1;
    g_cdesc[1].addr = dma(&g_status); g_cdesc[1].len = 1;
    g_cdesc[1].flags = VRING_DESC_F_WRITE; g_cdesc[1].next = 0;

    g_cavail.ring[g_cavail.idx % g_cqsize] = 0;
    __sync_synchronize();
    g_cavail.idx++;
    __sync_synchronize();
    virtio_pci_notify(&g_vd, g_cnotify, (uint16_t)g_data_queues);
    ring_wait(&g_cused.idx, &g_clast);
}

/* One AES-CBC operation over a whole number of blocks. */
int virtio_crypto_aes_cbc(bool encrypt, const uint8_t *key, uint32_t keylen,
                          const uint8_t iv[16], const void *src, void *dst,
                          uint32_t len) {
    if (!g_up) return -EMBK_ENODEV;
    if (!len || (len & 15) || len > VC_MAX_DATA) return -EMBK_EINVAL;

    uint64_t session = 0;
    if (!vc_create_session(key, keylen, encrypt, &session)) return -EMBK_EIO;

    memcpy(g_iv, iv, 16);
    memcpy(g_src, src, len);
    memset(g_dst, 0, len);
    memset(&g_req, 0, sizeof g_req);
    g_req.header.opcode = encrypt ? VC_CIPHER_ENCRYPT : VC_CIPHER_DECRYPT;
    g_req.header.algo = VC_ALGO_AES_CBC;
    g_req.header.session_id = session;
    g_req.header.flag = 1;                    /* session mode */
    g_req.u.sym.para.iv_len = 16;
    g_req.u.sym.para.src_len = len;
    g_req.u.sym.para.dst_len = len;
    g_req.u.sym.op_type = VC_SYM_OP_CIPHER;
    g_status = 0xFF;

    /* The chain is fixed by the specification: request, iv and source are
     * device-READABLE in that order; destination and status are
     * device-WRITABLE in that order. */
    g_ddesc[0].addr = dma(&g_req); g_ddesc[0].len = sizeof g_req;
    g_ddesc[0].flags = VRING_DESC_F_NEXT; g_ddesc[0].next = 1;
    g_ddesc[1].addr = dma(g_iv); g_ddesc[1].len = 16;
    g_ddesc[1].flags = VRING_DESC_F_NEXT; g_ddesc[1].next = 2;
    g_ddesc[2].addr = dma(g_src); g_ddesc[2].len = len;
    g_ddesc[2].flags = VRING_DESC_F_NEXT; g_ddesc[2].next = 3;
    g_ddesc[3].addr = dma(g_dst); g_ddesc[3].len = len;
    g_ddesc[3].flags = VRING_DESC_F_WRITE | VRING_DESC_F_NEXT; g_ddesc[3].next = 4;
    g_ddesc[4].addr = dma(&g_status); g_ddesc[4].len = 1;
    g_ddesc[4].flags = VRING_DESC_F_WRITE; g_ddesc[4].next = 0;

    g_davail.ring[g_davail.idx % g_dqsize] = 0;
    __sync_synchronize();
    g_davail.idx++;
    __sync_synchronize();
    virtio_pci_notify(&g_vd, g_dnotify, 0);

    bool ok = ring_wait(&g_dused.idx, &g_dlast);
    if (ok && g_status == VC_STATUS_OK) {
        memcpy(dst, g_dst, len);
        g_ops++;
    }
    vc_destroy_session(session);

    if (!ok) {
        kprintf("virtio-crypto: the device never answered a cipher request\n");
        return -EMBK_EIO;
    }
    if (g_status != VC_STATUS_OK) {
        kprintf("virtio-crypto: cipher returned status %u\n", g_status);
        return -EMBK_EIO;
    }
    return EMBK_OK;
}

bool virtio_crypto_init(void) {
    if (g_up) return true;
    const struct pci_device *pci = virtio_pci_find(VIRTIO_CRYPTO_DEVID, 0);
    if (!pci) return false;
    if (!virtio_pci_attach(&g_vd, pci, "virtio-crypto", 0, 0)) return false;
    if (!g_vd.devcfg) {
        kprintf("virtio-crypto: no config window\n");
        return false;
    }

    g_data_queues = vp_r32(g_vd.devcfg, VC_CFG_MAX_DATAQUEUES);
    if (!g_data_queues || g_data_queues > 64) g_data_queues = 1;
    uint32_t services = vp_r32(g_vd.devcfg, VC_CFG_CRYPTO_SERVICES);
    uint32_t ciphers  = vp_r32(g_vd.devcfg, VC_CFG_CIPHER_ALGO_L);
    uint32_t maxkey   = vp_r32(g_vd.devcfg, VC_CFG_MAX_CIPHER_KEY);

    if (!(services & (1u << VC_SERVICE_CIPHER))) {
        kprintf("virtio-crypto: the device offers no cipher service\n");
        return false;
    }
    if (!(ciphers & (1u << VC_ALGO_AES_CBC))) {
        kprintf("virtio-crypto: the device does not do AES-CBC\n");
        return false;
    }

    memset(g_cdesc, 0, sizeof g_cdesc);
    memset((void *)&g_cavail, 0, sizeof g_cavail);
    memset((void *)&g_cused, 0, sizeof g_cused);
    memset(g_ddesc, 0, sizeof g_ddesc);
    memset((void *)&g_davail, 0, sizeof g_davail);
    memset((void *)&g_dused, 0, sizeof g_dused);
    g_cavail.flags = VRING_AVAIL_F_NO_INTERRUPT;
    g_davail.flags = VRING_AVAIL_F_NO_INTERRUPT;

    g_dqsize = virtio_pci_setup_queue(&g_vd, 0, VC_QSIZE, g_ddesc, &g_davail,
                                      &g_dused, &g_dnotify);
    /* THE CONTROL QUEUE IS AFTER ALL THE DATA QUEUES. */
    g_cqsize = virtio_pci_setup_queue(&g_vd, (uint16_t)g_data_queues, VC_QSIZE,
                                      g_cdesc, &g_cavail, &g_cused, &g_cnotify);
    if (!g_dqsize || !g_cqsize) {
        kprintf("virtio-crypto: could not set up both queues\n");
        return false;
    }
    virtio_pci_driver_ok(&g_vd);
    g_dlast = g_dused.idx;
    g_clast = g_cused.idx;
    g_up = true;

    kprintf("virtio-crypto: ready, %u data queue(s), AES-CBC, keys to %u bytes\n",
            g_data_queues, maxkey);
    return true;
}

bool virtio_crypto_present(void) { return g_up; }
uint32_t virtio_crypto_ops(void) { return g_ops; }
