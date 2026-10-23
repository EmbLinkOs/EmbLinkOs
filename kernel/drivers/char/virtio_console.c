/* kernel/drivers/char/virtio_console.c -- a serial port that is not a 16550.
 *
 * Every byte this kernel has ever logged went out of a 1987 UART, one at a
 * time, through an I/O port. That works and it is slow: each byte is a VM exit
 * on an emulator and a busy-wait on real hardware, and a boot that prints a
 * few thousand lines spends real time doing it.
 *
 * virtio-console is the same idea with a queue instead of a port. It matters
 * for two reasons beyond speed: a machine with no serial header (which is most
 * laptops) has nowhere for the 16550 to be, and a host can attach the console
 * to a file or socket without pretending to be a UART.
 *
 * QUEUE 0 IS RECEIVE AND QUEUE 1 IS TRANSMIT, in that order, and they are not
 * interchangeable -- the device reads from one and writes to the other, so
 * getting them the wrong way round produces a console that accepts everything
 * and says nothing.
 */
#include <stdint.h>
#include <stddef.h>
#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "drivers/bus/virtio_pci.h"
#include "drivers/char/virtio_console.h"
#include "mm/pmm.h"

#define VIRTIO_CONSOLE_DEVID_M 0x1043   /* 0x1040 + device type 3 */
#define VIRTIO_CONSOLE_DEVID_T 0x1003

#define VC_QSIZE 8
#define VC_BUF   512

static struct virtio_pci_dev g_vd;
static bool g_up;

static struct vring_desc g_tdesc[VC_QSIZE] __attribute__((aligned(16)));
static struct { uint16_t flags, idx; uint16_t ring[VC_QSIZE]; uint16_t ue; }
    __attribute__((packed, aligned(2))) g_tavail;
static struct { uint16_t flags, idx; struct vring_used_elem ring[VC_QSIZE]; uint16_t ae; }
    __attribute__((packed, aligned(4))) g_tused;
static uint8_t g_tbuf[VC_BUF] __attribute__((aligned(64)));
static uint16_t g_tnotify, g_tqsize, g_tlast;
static uint64_t g_bytes;

static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

bool virtio_console_init(void) {
    if (g_up) return true;
    const struct pci_device *pci = virtio_pci_find(VIRTIO_CONSOLE_DEVID_M, 0);
    if (!pci) pci = virtio_pci_find(VIRTIO_CONSOLE_DEVID_T, 0);
    if (!pci) return false;
    if (!virtio_pci_attach(&g_vd, pci, "virtio-console", 0, 0)) return false;

    memset(g_tdesc, 0, sizeof g_tdesc);
    memset((void *)&g_tavail, 0, sizeof g_tavail);
    memset((void *)&g_tused, 0, sizeof g_tused);
    g_tavail.flags = VRING_AVAIL_F_NO_INTERRUPT;

    /* QUEUE 1 IS TRANSMIT. Queue 0 is receive and is left unconfigured: this
     * kernel takes its console input from the keyboard and the 16550, and a
     * receive queue with no buffers posted simply never delivers -- which is
     * correct, rather than a device waiting on one we never service. */
    g_tqsize = virtio_pci_setup_queue(&g_vd, 1, VC_QSIZE, g_tdesc, &g_tavail,
                                      &g_tused, &g_tnotify);
    if (!g_tqsize) { kprintf("virtio-console: no transmit queue\n"); return false; }
    virtio_pci_driver_ok(&g_vd);
    g_tlast = g_tused.idx;
    g_up = true;
    kprintf("virtio-console: ready\n");
    return true;
}

void virtio_console_write(const char *s, uint32_t len) {
    if (!g_up || !s || !len) return;
    while (len) {
        uint32_t n = len < VC_BUF ? len : VC_BUF;
        memcpy(g_tbuf, s, n);
        g_tdesc[0].addr = dma(g_tbuf);
        g_tdesc[0].len = n;
        g_tdesc[0].flags = 0;              /* device READS it */
        g_tdesc[0].next = 0;
        g_tavail.ring[g_tavail.idx % g_tqsize] = 0;
        __sync_synchronize();
        g_tavail.idx++;
        __sync_synchronize();
        virtio_pci_notify(&g_vd, g_tnotify, 1);
        for (int spin = 0; spin < 2000000; spin++) {
            if (g_tused.idx != g_tlast) { g_tlast++; break; }
            __asm__ volatile("" ::: "memory");
        }
        g_bytes += n;
        s += n; len -= n;
    }
}

bool virtio_console_present(void) { return g_up; }
uint64_t virtio_console_bytes(void) { return g_bytes; }
