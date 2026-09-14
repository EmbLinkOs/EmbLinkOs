/* kernel/drivers/char/virtio_rng.c -- entropy from the machine, on a machine
 * whose CPU will not give you any.
 *
 * WHY THIS MATTERS MORE ON ARM THAN ON x86. The kernel's CSPRNG is seeded from
 * whatever the architecture offers: on x86 that is RDSEED, which is a real
 * hardware entropy source and enough on its own. On aarch64 there is no
 * equivalent this kernel uses -- the pool is seeded from timing jitter and
 * boot-time values, which is a defensible fallback and is nobody's idea of a
 * good one. Every key the machine generates afterwards rests on it.
 *
 * virtio-rng is the hypervisor handing the guest bytes from the HOST's entropy
 * source. It is not a substitute for real hardware on a physical machine -- a
 * physical machine has no virtio bus -- but it is exactly the right thing in a
 * VM, and a VM is where this OS runs today.
 *
 * THE DEVICE IS AS SIMPLE AS VIRTIO GETS: one queue, no configuration space,
 * no negotiated features. Put a buffer in, get random bytes back. Everything
 * below that is not the virtqueue dance is bookkeeping.
 *
 * ENTROPY IS ADDED, NEVER TRUSTED ALONE. random_add_entropy() folds these
 * bytes into the DRBG state rather than replacing it, so a hypervisor that
 * feeds the guest predictable bytes cannot make the pool WORSE than it was --
 * which is the only safe way to accept entropy from something you do not
 * control.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "drivers/bus/virtio_pci.h"
#include "lib/random.h"
#include "drivers/char/virtio_rng.h"
#include "mm/pmm.h"

/* TWO IDS, because QEMU's `virtio-rng-pci` is TRANSITIONAL by default and a
 * transitional device presents the LEGACY id -- 0x1005 -- while still offering
 * the modern capability layout this driver speaks. Matching only the modern id
 * finds nothing on the command line anybody actually types. virtio-blk carries
 * the same pair for the same reason. */
#define VIRTIO_RNG_DEVID_M  0x1044   /* 0x1040 + device type 4 (entropy)    */
#define VIRTIO_RNG_DEVID_T  0x1005   /* transitional / legacy               */

#define RNG_QSIZE   8
#define RNG_CHUNK   64             /* bytes asked for per request */

static struct virtio_pci_dev g_vd;
static bool g_up;

/* The ring, in .bss so KV2P() is exact -- see virtio_blk.c on why this cannot
 * come from the heap. */
static struct vring_desc  g_desc[RNG_QSIZE] __attribute__((aligned(16)));
static struct {
    uint16_t flags, idx;
    uint16_t ring[RNG_QSIZE];
    uint16_t used_event;
} __attribute__((packed, aligned(2))) g_avail;
static struct {
    uint16_t flags, idx;
    struct vring_used_elem ring[RNG_QSIZE];
    uint16_t avail_event;
} __attribute__((packed, aligned(4))) g_used;

static uint8_t  g_buf[RNG_CHUNK] __attribute__((aligned(64)));
static uint16_t g_notify_off;
static uint16_t g_qsize;
static uint16_t g_last_used;
static uint64_t g_bytes;

static inline uint64_t dma(const volatile void *p) {
    return KV2P((uint64_t)(uintptr_t)p);
}

bool virtio_rng_init(void) {
    const struct pci_device *pci = virtio_pci_find(VIRTIO_RNG_DEVID_M, 0);
    if (!pci) pci = virtio_pci_find(VIRTIO_RNG_DEVID_T, 0);
    if (!pci) return false;

    if (!virtio_pci_attach(&g_vd, pci, "virtio-rng", 0, 0)) return false;

    memset(g_desc, 0, sizeof g_desc);
    memset((void *)&g_avail, 0, sizeof g_avail);
    memset((void *)&g_used, 0, sizeof g_used);
    /* Polled, so the device must not raise its INTx line: a level-triggered
     * interrupt nothing acknowledges wedges the machine the moment interrupts
     * are enabled. The same trap virtio-blk documents. */
    g_avail.flags = VRING_AVAIL_F_NO_INTERRUPT;

    g_qsize = virtio_pci_setup_queue(&g_vd, 0, RNG_QSIZE, g_desc, &g_avail,
                                     &g_used, &g_notify_off);
    if (!g_qsize) {
        kprintf("virtio-rng: no request queue\n");
        return false;
    }
    virtio_pci_driver_ok(&g_vd);

    g_last_used = g_used.idx;
    g_up = true;
    kprintf("virtio-rng: ready (queue of %u)\n", (unsigned)g_qsize);
    return true;
}

/* Ask the device for `len` bytes and fold them into the pool. Returns how many
 * arrived, which may legitimately be fewer than asked: the specification lets
 * the device return a short buffer when its own source is running dry, and a
 * driver that treats that as an error stops collecting entropy exactly when
 * entropy is scarce. */
uint32_t virtio_rng_harvest(uint32_t len) { return virtio_rng_read(0, len); }

/* As above, and hand the caller a copy. `out` may be NULL.
 *
 * THE COPY EXISTS SO THE BYTES CAN BE JUDGED. A device that returns 64 zeroes
 * satisfies every check in the virtqueue dance -- the descriptor completed,
 * the length was right -- and contributes nothing. `test rng` looks at what
 * actually arrived, which is the only way to tell a working entropy source
 * from a well-behaved one that is empty. */
uint32_t virtio_rng_read(void *out, uint32_t len) {
    if (!g_up) return 0;
    if (len == 0 || len > RNG_CHUNK) len = RNG_CHUNK;

    memset(g_buf, 0, len);

    g_desc[0].addr  = dma(g_buf);
    g_desc[0].len   = len;
    g_desc[0].flags = VRING_DESC_F_WRITE;   /* the DEVICE writes into it */
    g_desc[0].next  = 0;

    g_avail.ring[g_avail.idx % g_qsize] = 0;
    __sync_synchronize();
    g_avail.idx++;
    __sync_synchronize();
    virtio_pci_notify(&g_vd, g_notify_off, 0);

    /* Bounded. A device that never completes must leave the boot running with
     * a weaker pool, not stop it. */
    uint32_t got = 0;
    for (int spin = 0; spin < 2000000; spin++) {
        if (g_used.idx != g_last_used) {
            struct vring_used_elem *e = &g_used.ring[g_last_used % g_qsize];
            got = e->len > len ? len : e->len;
            g_last_used++;
            break;
        }
        __asm__ volatile("" ::: "memory");
    }
    if (!got) return 0;

    random_add_entropy(g_buf, got);
    if (out) memcpy(out, g_buf, got);
    g_bytes += got;
    return got;
}

bool virtio_rng_present(void) { return g_up; }
uint64_t virtio_rng_bytes(void) { return g_bytes; }
