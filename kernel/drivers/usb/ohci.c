// OHCI (Open Host Controller Interface, USB 1.0/1.1) driver.
//
// Memory-mapped controller; work is described with Endpoint Descriptors (ED)
// holding chains of Transfer Descriptors (TD). Control/bulk transfers run
// synchronously through a scratch ED on the control/bulk list; interrupt-IN
// endpoints get a persistent ED chained into the HCCA periodic table and are
// polled from usb_core_poll().
//
// Data toggles are forced per-TD from the core's per-endpoint state, so the
// ED toggle-carry mechanism is never relied upon.

#include "drivers/usb/ohci.h"
#include "drivers/usb/usb_core.h"

#include "include/kprintf.h"
#include "drivers/timer/timer.h"
#include "include/kstring.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "drivers/timer/pit.h"

// Operational registers.
#define OHCI_REVISION        0x00
#define OHCI_CONTROL         0x04
#define OHCI_CMDSTATUS       0x08
#define OHCI_INTSTATUS       0x0C
#define OHCI_INTDISABLE      0x14
#define OHCI_HCCA            0x18
#define OHCI_CTRL_HEAD_ED    0x20
#define OHCI_CTRL_CURR_ED    0x24
#define OHCI_BULK_HEAD_ED    0x28
#define OHCI_BULK_CURR_ED    0x2C
#define OHCI_FMINTERVAL      0x34
#define OHCI_PERIODICSTART   0x40
#define OHCI_RHDESCRIPTORA   0x48
#define OHCI_RHSTATUS        0x50
#define OHCI_RHPORTSTATUS    0x54

#define OHCI_CTRL_PLE  (1 << 2)
#define OHCI_CTRL_CLE  (1 << 4)
#define OHCI_CTRL_BLE  (1 << 5)
#define OHCI_CTRL_HCFS_OPER (2 << 6)

#define OHCI_CS_HCR (1 << 0)
#define OHCI_CS_CLF (1 << 1)
#define OHCI_CS_BLF (1 << 2)

#define OHCI_INT_WDH (1 << 1)

#define OHCI_PORT_CCS  (1 << 0)
#define OHCI_PORT_PES  (1 << 1)
#define OHCI_PORT_PRS  (1 << 4)
#define OHCI_PORT_LSDA (1 << 9)
#define OHCI_PORT_CSC  (1 << 16)
#define OHCI_PORT_PESC (1 << 17)
#define OHCI_PORT_PRSC (1 << 20)

// ED dword0 fields.
#define OHCI_ED_SPEED_LOW (1 << 13)
#define OHCI_ED_SKIP      (1 << 14)

// TD dword0 fields.
#define OHCI_TD_ROUNDING  (1 << 18)
#define OHCI_TD_DP_SETUP  (0U << 19)
#define OHCI_TD_DP_OUT    (1U << 19)
#define OHCI_TD_DP_IN     (2U << 19)
#define OHCI_TD_DI_NONE   (7U << 21)
#define OHCI_TD_T_DATA0   (2U << 24)
#define OHCI_TD_T_DATA1   (3U << 24)
#define OHCI_TD_CC_MASK   (0xFU << 28)
#define OHCI_TD_CC_NOTACCESSED (0xFU << 28)

#define OHCI_CC_NOERROR 0
#define OHCI_CC_STALL   4

#define OHCI_MAX_HC   2
#define OHCI_NUM_TD   72
#define OHCI_NUM_INTR 4

struct ohci_ed {
    uint32_t flags;
    uint32_t tail;
    uint32_t head;
    uint32_t next;
} __attribute__((aligned(16)));

struct ohci_td {
    uint32_t flags;
    uint32_t cbp;
    uint32_t next;
    uint32_t be;
} __attribute__((aligned(16)));

struct ohci_hcca {
    uint32_t int_table[32];
    uint16_t frame_number;
    uint16_t pad1;
    uint32_t done_head;
    uint8_t  reserved[116];
} __attribute__((aligned(256)));

struct ohci_intr_slot {
    bool active;
    struct usb_device *dev;
    uint8_t ep_addr;
    uint16_t len;
    struct ohci_td td;
    struct ohci_td tail;
    uint8_t buf[64] __attribute__((aligned(16)));
};

struct ohci_hc {
    bool used;
    volatile uint8_t *mmio;
    uint32_t nports;

    struct ohci_hcca hcca;
    struct ohci_ed ed_work;
    struct ohci_ed ed_intr[OHCI_NUM_INTR];
    struct ohci_td td[OHCI_NUM_TD];
    struct ohci_td td_tail;                 // shared dummy tail for ed_work
    uint8_t setup_buf[8] __attribute__((aligned(16)));
    uint8_t bounce[4096] __attribute__((aligned(64)));

    struct ohci_intr_slot intr[OHCI_NUM_INTR];
};

static struct ohci_hc g_ohci[OHCI_MAX_HC];

static inline uint32_t ohci_dma(const volatile void *p) {
    return (uint32_t)KV2P((uint64_t)(uintptr_t)p);
}

static inline uint32_t ohci_read(struct ohci_hc *hc, uint32_t off) {
    return *(volatile uint32_t *)(hc->mmio + off);
}

static inline void ohci_write(struct ohci_hc *hc, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(hc->mmio + off) = v;
}

static inline uint16_t ohci_ep_mps(struct usb_device *dev, uint8_t ep_addr) {
    for (uint32_t i = 0; i < dev->num_eps; i++) {
        if (dev->eps[i].addr == ep_addr) return dev->eps[i].mps;
    }
    return (dev->speed == USB_SPEED_LOW) ? 8 : 64;
}

static void ohci_fill_td(struct ohci_td *td, uint32_t dp, uint32_t toggle_t,
                         const void *buf, uint32_t len, uint32_t next_phys) {
    td->flags = OHCI_TD_CC_NOTACCESSED | OHCI_TD_DI_NONE | OHCI_TD_ROUNDING |
                dp | toggle_t;
    if (len) {
        uint32_t phys = ohci_dma(buf);
        td->cbp = phys;
        td->be = phys + len - 1;
    } else {
        td->cbp = 0;
        td->be = 0;
    }
    td->next = next_phys;
}

static inline uint8_t ohci_td_cc(const struct ohci_td *td) {
    return (uint8_t)(((volatile const struct ohci_td *)td)->flags >> 28);
}

// Bytes actually moved by a retired TD that described `size` bytes.
static uint32_t ohci_td_done_bytes(const struct ohci_td *td, uint32_t size) {
    uint32_t cbp = ((volatile const struct ohci_td *)td)->cbp;
    if (cbp == 0) return size;                    // fully consumed
    return size - (td->be - cbp + 1);
}

// Run the TD chain hanging off ed_work on the given list (control or bulk).
// Returns -1 on success, -2 on timeout, else index of the first failed TD.
static int ohci_run_ed(struct ohci_hc *hc, bool control,
                       uint32_t ntd, uint32_t timeout_ms) {
    uint32_t tail_phys = ohci_dma(&hc->td_tail);

    if (control) {
        ohci_write(hc, OHCI_CTRL_HEAD_ED, ohci_dma(&hc->ed_work));
        ohci_write(hc, OHCI_CTRL_CURR_ED, 0);
        ohci_write(hc, OHCI_CONTROL,
                   ohci_read(hc, OHCI_CONTROL) | OHCI_CTRL_CLE);
        ohci_write(hc, OHCI_CMDSTATUS, OHCI_CS_CLF);
    } else {
        ohci_write(hc, OHCI_BULK_HEAD_ED, ohci_dma(&hc->ed_work));
        ohci_write(hc, OHCI_BULK_CURR_ED, 0);
        ohci_write(hc, OHCI_CONTROL,
                   ohci_read(hc, OHCI_CONTROL) | OHCI_CTRL_BLE);
        ohci_write(hc, OHCI_CMDSTATUS, OHCI_CS_BLF);
    }

    /* A DEADLINE OFF THE CLOCK, NOT A SPIN COUNT.
     *
     * This waited `timeout_ms * 20000` iterations of a tight loop, which is
     * not a timeout -- it is a guess about how fast the processor is, and it
     * is wrong by whatever ratio this machine differs from the one it was
     * calibrated on. Here it was wrong in the direction that matters: a
     * 64-descriptor transfer got 34 of them done before the count ran out,
     * so a stick enumerated perfectly and every read of it failed. The
     * controller was fine and the driver simply stopped waiting.
     *
     * A millisecond is a millisecond on every machine, and it is also the
     * unit the hardware works in: one USB frame. */
    int result = -2;
    uint64_t deadline = timer_uptime_ms() + timeout_ms;
    while (timer_uptime_ms() <= deadline) {
        uint32_t head = ((volatile struct ohci_ed *)&hc->ed_work)->head;
        if (head & 1) {                       // halted (error/stall)
            for (uint32_t t = 0; t < ntd; t++) {
                uint8_t cc = ohci_td_cc(&hc->td[t]);
                if (cc != OHCI_CC_NOERROR && cc != 0xF) {
                    result = (int)t;
                    break;
                }
            }
            if (result == -2) result = 0;
            break;
        }
        if ((head & ~0xFU) == tail_phys) {    // all TDs retired
            result = -1;
            break;
        }
    }

    // Detach the list again and ack the writeback-done-head interrupt.
    if (control) {
        ohci_write(hc, OHCI_CONTROL,
                   ohci_read(hc, OHCI_CONTROL) & ~(uint32_t)OHCI_CTRL_CLE);
        ohci_write(hc, OHCI_CTRL_HEAD_ED, 0);
        ohci_write(hc, OHCI_CTRL_CURR_ED, 0);
    } else {
        ohci_write(hc, OHCI_CONTROL,
                   ohci_read(hc, OHCI_CONTROL) & ~(uint32_t)OHCI_CTRL_BLE);
        ohci_write(hc, OHCI_BULK_HEAD_ED, 0);
        ohci_write(hc, OHCI_BULK_CURR_ED, 0);
    }
    hc->hcca.done_head = 0;
    ohci_write(hc, OHCI_INTSTATUS, OHCI_INT_WDH);
    return result;
}

// ---------------------------------------------------------------------------
// usb_hcd_ops
// ---------------------------------------------------------------------------

static int ohci_control_xfer(struct usb_device *dev,
                             const struct usb_setup_packet *setup, void *data) {
    struct ohci_hc *hc = (struct ohci_hc *)dev->hc;
    bool dir_in = (setup->bmRequestType & 0x80) != 0;
    uint32_t wlen = setup->wLength;
    uint16_t mps = dev->ep0_mps ? dev->ep0_mps : 8;

    if (wlen > sizeof(hc->bounce)) return USB_ERR_IO;
    memcpy(hc->setup_buf, setup, 8);
    if (wlen && !dir_in && data) memcpy(hc->bounce, data, wlen);

    /* One descriptor per page-pair, as in the bulk path -- not one per packet.
     * A control transfer is usually small enough that the difference does not
     * show, which is exactly why the same mistake could sit here unnoticed:
     * a descriptor read big enough to need thirty of them would have hit the
     * same controller limit that made every bulk read fail. */
    uint32_t ndata = wlen ? (wlen + 4095) / 4096 : 0;
    uint32_t ntd = 2 + ndata;
    if (ntd > OHCI_NUM_TD) return USB_ERR_IO;

    uint32_t tail_phys = ohci_dma(&hc->td_tail);

    // SETUP (DATA0)
    ohci_fill_td(&hc->td[0], OHCI_TD_DP_SETUP, OHCI_TD_T_DATA0,
                 hc->setup_buf, 8, 0);

    /* Data stage. Its first packet is DATA1 by definition; the controller
     * alternates from there, within a descriptor and across them, so only the
     * first has its toggle forced. */
    uint32_t idx = 1, off = 0;
    while (off < wlen) {
        uint32_t chunk = wlen - off;
        if (chunk > 4096) chunk = 4096;
        ohci_fill_td(&hc->td[idx],
                     dir_in ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT,
                     idx == 1 ? OHCI_TD_T_DATA1 : 0,
                     hc->bounce + off, chunk, 0);
        off += chunk;
        idx++;
    }

    // Status stage: OUT only after an IN data stage; IN otherwise. DATA1.
    ohci_fill_td(&hc->td[idx],
                 (dir_in && wlen) ? OHCI_TD_DP_OUT : OHCI_TD_DP_IN,
                 OHCI_TD_T_DATA1, NULL, 0, tail_phys);

    for (uint32_t i = 0; i < idx; i++) {
        hc->td[i].next = ohci_dma(&hc->td[i + 1]);
    }

    hc->ed_work.flags = ((uint32_t)dev->addr & 0x7F) |
                        ((uint32_t)mps << 16) |
                        (dev->speed == USB_SPEED_LOW ? OHCI_ED_SPEED_LOW : 0);
    hc->ed_work.tail = tail_phys;
    hc->ed_work.head = ohci_dma(&hc->td[0]);
    hc->ed_work.next = 0;

    int rc = ohci_run_ed(hc, true, idx + 1, 500);
    if (rc == -2) return USB_ERR_TIMEOUT;
    if (rc >= 0) {
        return (ohci_td_cc(&hc->td[rc]) == OHCI_CC_STALL)
            ? USB_ERR_STALL : USB_ERR_IO;
    }

    uint32_t got = 0, sz_off = 0;
    for (uint32_t i = 1; i < idx; i++) {
        uint32_t chunk = wlen - sz_off;
        if (chunk > 4096) chunk = 4096;
        got += ohci_td_done_bytes(&hc->td[i], chunk);
        sz_off += chunk;
    }
    if (dir_in && data && got) {
        memcpy(data, hc->bounce, got > wlen ? wlen : got);
    }
    return (int)got;
}

static int ohci_bulk_xfer(struct usb_device *dev, uint8_t ep_addr,
                          void *data, uint32_t len) {
    struct ohci_hc *hc = (struct ohci_hc *)dev->hc;
    bool dir_in = (ep_addr & 0x80) != 0;
    uint16_t mps = ohci_ep_mps(dev, ep_addr);

    if (len > sizeof(hc->bounce)) return USB_ERR_IO;
    if (!dir_in && data && len) memcpy(hc->bounce, data, len);

    /* ONE DESCRIPTOR FOR THE WHOLE TRANSFER, NOT ONE PER PACKET.
     *
     * This built a TD for every max-packet: sixty-four of them for a 4 KiB
     * read. That is legal and it is not how the hardware is meant to be
     * driven -- a single OHCI descriptor carries up to two pages and the
     * controller does the packetising itself, one frame instead of sixty-four.
     *
     * It was also WRONG, and quietly. Past about thirty descriptors on one
     * endpoint the controller stopped and raised UnrecoverableError, so a
     * stick enumerated perfectly (INQUIRY and READ CAPACITY are one
     * descriptor each) and every actual read of it failed -- and with the
     * controller then halted, every command after it timed out too, which
     * made it look like the device had gone away.
     *
     * THE TOGGLE IS WHY IT WAS WRITTEN THIS WAY. The file's own header says
     * toggles are "forced per-TD from the core's per-endpoint state, so the
     * ED toggle-carry mechanism is never relied upon" -- and forcing one per
     * TD means one TD per packet. It does not have to: the T field sets the
     * toggle for the descriptor's FIRST packet and the controller alternates
     * correctly through the rest of it. So the toggle is still forced, once,
     * and what the core believes afterwards is computed from the bytes that
     * actually moved. */
    uint32_t tail_phys = ohci_dma(&hc->td_tail);
    uint8_t toggle = usb_toggle_get(dev, ep_addr);
    uint32_t idx = 0, off = 0;

    do {
        /* A descriptor's buffer may cross ONE page boundary and no more, so
         * 4096 bytes starting anywhere is always expressible. */
        uint32_t chunk = len - off;
        if (chunk > 4096) chunk = 4096;
        ohci_fill_td(&hc->td[idx],
                     dir_in ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT,
                     /* Force the toggle on the FIRST descriptor only; after
                      * that the controller's own carry is correct and
                      * overriding it would restart the sequence. */
                     idx == 0 ? (toggle ? OHCI_TD_T_DATA1 : OHCI_TD_T_DATA0) : 0,
                     hc->bounce + off, chunk, 0);
        off += chunk;
        idx++;
    } while (off < len && idx < OHCI_NUM_TD);
    if (off < len) return USB_ERR_IO;

    for (uint32_t i = 0; i + 1 < idx; i++) {
        hc->td[i].next = ohci_dma(&hc->td[i + 1]);
    }
    hc->td[idx - 1].next = tail_phys;

    hc->ed_work.flags = ((uint32_t)dev->addr & 0x7F) |
                        (((uint32_t)ep_addr & 0x0F) << 7) |
                        ((uint32_t)mps << 16) |
                        (dev->speed == USB_SPEED_LOW ? OHCI_ED_SPEED_LOW : 0);
    hc->ed_work.tail = tail_phys;
    hc->ed_work.head = ohci_dma(&hc->td[0]);
    hc->ed_work.next = 0;

    int rc = ohci_run_ed(hc, false, idx, 2000);
    if (rc == -2) {
        kprintf("OHCI: bulk ep %02x, %u byte(s) in %u TD(s): timed out\n",
                ep_addr, len, idx);
        return USB_ERR_TIMEOUT;
    }
    if (rc >= 0) {
        /* SAY WHICH WAY IT FAILED. The condition code is the only thing the
         * controller tells you about a failed transfer, and returning a bare
         * USB_ERR_IO throws it away -- which left "the stick enumerates and
         * its filesystem cannot be read" with nothing to go on at all. */
        uint8_t cc = ohci_td_cc(&hc->td[rc]);
        kprintf("OHCI: bulk ep %02x, %u byte(s): TD %d failed, condition %u\n",
                ep_addr, len, rc, cc);
        usb_toggle_set(dev, ep_addr, usb_toggle_get(dev, ep_addr) ^ (rc & 1));
        return (cc == OHCI_CC_STALL) ? USB_ERR_STALL : USB_ERR_IO;
    }

    uint32_t got = 0, sz_off = 0;
    for (uint32_t i = 0; i < idx; i++) {
        uint32_t chunk = len - sz_off;
        if (chunk > 4096) chunk = 4096;
        got += ohci_td_done_bytes(&hc->td[i], chunk);
        sz_off += chunk;
    }

    /* WHAT THE TOGGLE IS NOW depends on how many packets actually went, not
     * on how many were asked for: a device that answers short ends the
     * sequence early, and the next transfer on this endpoint has to agree
     * with it or the device ignores everything that follows. A zero-length
     * transfer is still one packet. */
    {
        uint32_t pkts = got ? (got + mps - 1) / mps : 1;
        usb_toggle_set(dev, ep_addr, (uint8_t)(toggle ^ (pkts & 1)));
    }
    if (got > len) got = len;
    if (dir_in && data && got) memcpy(data, hc->bounce, got);
    return (int)got;
}

static struct ohci_intr_slot *ohci_find_slot(struct ohci_hc *hc,
                                             struct usb_device *dev,
                                             uint8_t ep_addr, bool alloc) {
    for (uint32_t i = 0; i < OHCI_NUM_INTR; i++) {
        if (hc->intr[i].dev == dev && hc->intr[i].ep_addr == ep_addr) {
            return &hc->intr[i];
        }
    }
    if (!alloc) return NULL;
    for (uint32_t i = 0; i < OHCI_NUM_INTR; i++) {
        if (!hc->intr[i].dev) {
            hc->intr[i].dev = dev;
            hc->intr[i].ep_addr = ep_addr;
            return &hc->intr[i];
        }
    }
    return NULL;
}

static int ohci_int_submit(struct usb_device *dev, uint8_t ep_addr,
                           uint32_t len) {
    struct ohci_hc *hc = (struct ohci_hc *)dev->hc;
    struct ohci_intr_slot *slot = ohci_find_slot(hc, dev, ep_addr, true);
    if (!slot || len > sizeof(slot->buf)) return USB_ERR_IO;
    if (slot->active) return 0;

    uint32_t si = (uint32_t)(slot - hc->intr);
    struct ohci_ed *ed = &hc->ed_intr[si];

    slot->len = (uint16_t)len;
    ohci_fill_td(&slot->td, OHCI_TD_DP_IN,
                 usb_toggle_get(dev, ep_addr) ? OHCI_TD_T_DATA1
                                              : OHCI_TD_T_DATA0,
                 slot->buf, len, ohci_dma(&slot->tail));

    ed->flags = ((uint32_t)dev->addr & 0x7F) |
                (((uint32_t)ep_addr & 0x0F) << 7) |
                ((uint32_t)ohci_ep_mps(dev, ep_addr) << 16) |
                (dev->speed == USB_SPEED_LOW ? OHCI_ED_SPEED_LOW : 0);
    ed->tail = ohci_dma(&slot->tail);
    __sync_synchronize();
    ed->head = ohci_dma(&slot->td);    // arms the transfer
    slot->active = true;
    return 0;
}

static int ohci_int_poll(struct usb_device *dev, uint8_t ep_addr,
                         void *buf, uint32_t len) {
    struct ohci_hc *hc = (struct ohci_hc *)dev->hc;
    struct ohci_intr_slot *slot = ohci_find_slot(hc, dev, ep_addr, false);
    if (!slot || !slot->active) return USB_ERR_IO;

    uint32_t si = (uint32_t)(slot - hc->intr);
    volatile struct ohci_ed *ed = (volatile struct ohci_ed *)&hc->ed_intr[si];

    uint32_t head = ed->head;
    bool halted = (head & 1) != 0;
    bool retired = ((head & ~0xFU) == ohci_dma(&slot->tail));
    if (!halted && !retired) return USB_ERR_PENDING;

    slot->active = false;
    hc->hcca.done_head = 0;
    ohci_write(hc, OHCI_INTSTATUS, OHCI_INT_WDH);

    if (halted) {
        ed->head = ed->tail;   // clear the halt for the next submit
        uint8_t cc = ohci_td_cc(&slot->td);
        return (cc == OHCI_CC_STALL) ? USB_ERR_STALL : USB_ERR_IO;
    }

    usb_toggle_set(dev, ep_addr, usb_toggle_get(dev, ep_addr) ^ 1);
    uint32_t got = ohci_td_done_bytes(&slot->td, slot->len);
    if (got > len) got = len;
    if (got) memcpy(buf, slot->buf, got);
    return (int)got;
}

static const struct usb_hcd_ops g_ohci_ops = {
    .control = ohci_control_xfer,
    .bulk = ohci_bulk_xfer,
    .int_submit = ohci_int_submit,
    .int_poll = ohci_int_poll,
};

// ---------------------------------------------------------------------------
// Controller bring-up
// ---------------------------------------------------------------------------

/* One port, probed -- see uhci.c on why this is split out of the scan. */
static void ohci_port_probe(struct ohci_hc *hc, uint32_t p) {
    {
        uint32_t reg = OHCI_RHPORTSTATUS + p * 4;
        uint32_t sc = ohci_read(hc, reg);
        if (!(sc & OHCI_PORT_CCS)) return;

        ohci_write(hc, reg, OHCI_PORT_PRS);
        for (int i = 0; i < 500; i++) {
            if (ohci_read(hc, reg) & OHCI_PORT_PRSC) break;
            pit_delay_ms(1);
        }
        ohci_write(hc, reg, OHCI_PORT_PRSC | OHCI_PORT_CSC | OHCI_PORT_PESC);
        pit_delay_ms(10);

        sc = ohci_read(hc, reg);
        if (!(sc & OHCI_PORT_PES)) {
            kprintf("OHCI: port %u failed to enable (sc=%x)\n",
                    (unsigned int)(p + 1), (unsigned int)sc);
            return;
        }

        uint8_t speed = (sc & OHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
        kprintf("OHCI: port %u connected (%s speed)\n",
                (unsigned int)(p + 1),
                speed == USB_SPEED_LOW ? "low" : "full");

        struct usb_device *dev = usb_alloc_device(&g_ohci_ops, hc, NULL, speed);
        if (!dev) return;
        dev->port = (uint8_t)(p + 1);
        if (usb_enumerate(dev) != 0) {
            usb_free_device(dev);
        }
    }
}

static void ohci_port_scan(struct ohci_hc *hc) {
    for (uint32_t p = 0; p < hc->nports; p++) ohci_port_probe(hc, p);
}

void ohci_rescan(void *hcv) {
    struct ohci_hc *hc = hcv;
    for (uint32_t p = 0; p < hc->nports; p++) {
        uint32_t reg = OHCI_RHPORTSTATUS + p * 4;
        uint32_t sc = ohci_read(hc, reg);
        if (sc & OHCI_PORT_CSC) ohci_write(hc, reg, OHCI_PORT_CSC);

        bool connected = (sc & OHCI_PORT_CCS) != 0;
        struct usb_device *known = usb_device_on_port(hc, (uint8_t)(p + 1));
        if (!connected) {
            usb_port_clear_dead(hc, (uint8_t)(p + 1));
            if (known) usb_device_gone(known);
            continue;
        }
        if (known || usb_port_is_dead(hc, (uint8_t)(p + 1))) continue;

        kprintf("OHCI: port %u -- something was plugged in\n",
                (unsigned int)(p + 1));
        ohci_port_probe(hc, p);
        if (!usb_device_on_port(hc, (uint8_t)(p + 1)))
            usb_port_mark_dead(hc, (uint8_t)(p + 1));
    }
}

bool ohci_init_controller(struct usb_controller *ctrl) {
    if (!ctrl->bar0.valid || !ctrl->bar0.is_mmio) {
        kprintf("OHCI: no usable MMIO BAR0\n");
        return false;
    }

    struct ohci_hc *hc = NULL;
    for (uint32_t i = 0; i < OHCI_MAX_HC; i++) {
        if (!g_ohci[i].used) { hc = &g_ohci[i]; break; }
    }
    if (!hc) {
        kprintf("OHCI: controller table full\n");
        return false;
    }
    memset(hc, 0, sizeof(*hc));
    hc->used = true;

    uint64_t virt = vmm_map_mmio(ctrl->bar0.address, ctrl->bar0.size);
    if (!virt) {
        kprintf("OHCI: failed to map MMIO\n");
        hc->used = false;
        return false;
    }
    hc->mmio = (volatile uint8_t *)virt;
    ctrl->mmio_virt = virt;
    ctrl->mmio_size = ctrl->bar0.size;

    uint32_t rev = ohci_read(hc, OHCI_REVISION) & 0xFF;
    kprintf("OHCI: revision %u.%u\n",
            (unsigned int)(rev >> 4), (unsigned int)(rev & 0xF));

    // Software reset, then restore the frame interval and go operational.
    ohci_write(hc, OHCI_CMDSTATUS, OHCI_CS_HCR);
    for (int i = 0; i < 100; i++) {
        if (!(ohci_read(hc, OHCI_CMDSTATUS) & OHCI_CS_HCR)) break;
        pit_delay_ms(1);
    }

    // Empty periodic table: interrupt ED chain filled in below.
    for (uint32_t i = 0; i < OHCI_NUM_INTR; i++) {
        struct ohci_intr_slot *slot = &hc->intr[i];
        struct ohci_ed *ed = &hc->ed_intr[i];
        ed->flags = OHCI_ED_SKIP;   // becomes real on first int_submit
        ed->head = ed->tail = ohci_dma(&slot->tail);
        ed->next = (i + 1 < OHCI_NUM_INTR) ? ohci_dma(&hc->ed_intr[i + 1]) : 0;
    }
    for (uint32_t i = 0; i < 32; i++) {
        hc->hcca.int_table[i] = ohci_dma(&hc->ed_intr[0]);
    }

    ohci_write(hc, OHCI_HCCA, ohci_dma(&hc->hcca));
    ohci_write(hc, OHCI_CTRL_HEAD_ED, 0);
    ohci_write(hc, OHCI_BULK_HEAD_ED, 0);
    ohci_write(hc, OHCI_INTDISABLE, 0xC000007F);
    ohci_write(hc, OHCI_FMINTERVAL, 0x27782EDF);   // FSMPS | FI (defaults)
    ohci_write(hc, OHCI_PERIODICSTART, 0x3E67);    // 90% of the frame
    ohci_write(hc, OHCI_CONTROL, OHCI_CTRL_HCFS_OPER | OHCI_CTRL_PLE | 3);

    // Power the root hub ports and wait the mandated settle time.
    ohci_write(hc, OHCI_RHSTATUS, 1U << 16);       // SetGlobalPower
    uint32_t rhda = ohci_read(hc, OHCI_RHDESCRIPTORA);
    uint32_t potpgt = (rhda >> 24) & 0xFF;
    pit_delay_ms(potpgt ? potpgt * 2 : 50);

    hc->nports = rhda & 0xFF;
    if (hc->nports > 15) hc->nports = 15;
    ctrl->max_ports = (uint8_t)hc->nports;
    ctrl->hc = hc;
    ctrl->rescan = ohci_rescan;
    kprintf("OHCI: running, %u root ports\n", (unsigned int)hc->nports);

    // Interrupt EDs live on the periodic list; a skipped ED with SKIP set
    // must still be unskipped when first armed.
    for (uint32_t i = 0; i < OHCI_NUM_INTR; i++) {
        hc->ed_intr[i].flags &= ~(uint32_t)OHCI_ED_SKIP;
    }

    ohci_port_scan(hc);
    return true;
}
