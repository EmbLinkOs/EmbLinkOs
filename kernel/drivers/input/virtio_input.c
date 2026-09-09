#include "drivers/input/virtio_input.h"
#include "drivers/bus/virtio_pci.h"
#include "drivers/input/keyboard.h"
#include "drivers/input/mouse.h"
#include "mm/pmm.h"
#include "include/kprintf.h"
#include "include/kstring.h"

/* virtio-input -- the keyboard and the pointer on a machine with no PS/2
 * controller. docs/ARM64.md phase A7; docs/TODO.md said "virtio-input is the
 * one that matters first: without it the ARM machine has no keyboard".
 *
 * WHAT THIS FILE IS NOT: a second keyboard driver. The char ring, the key-event
 * ring, modifier tracking, layouts, the Ctrl-C route and the grab all live in
 * drivers/input/keyboard.c and are shared -- that file's PS/2 half is now
 * behind an #if and its POLICY half is not. This file is a TRANSLATION TABLE
 * plus a queue: Linux evdev codes in, keyboard_inject_event() out. The same
 * goes for the pointer, which lands in mouse_set_absolute(), an entry point
 * that already existed for USB tablets.
 *
 * TWO DEVICES, ONE DRIVER. QEMU exposes the keyboard and the tablet as separate
 * PCI functions with the SAME device id (0x1052), distinguishable only by
 * asking each what it is: select ID_DEVIDS/ID_NAME in the device config and
 * read the answer. We probe by capability instead of by name -- a device that
 * reports EV_KEY with keyboard keycodes IS a keyboard, whatever it calls
 * itself -- because the names are QEMU's and could change.
 *
 * POLLED, like virtio-blk, and for the same reason plus one: there is no MSI on
 * this machine (arch_pci_msi_message() returns false), so an interrupt-driven
 * version would use a level-triggered INTx line shared with everything else on
 * the bus, and the event queue is drained from the compositor tick anyway --
 * which is exactly the cadence input needs.
 */

#define VIRTIO_INPUT_DEVID   0x1052
#define VI_MAX_DEVS          4
#define VI_QSIZE             64        /* eventq depth: a burst of keystrokes  */

/* Device-config selects (virtio 1.1, 5.8.4). */
#define VI_CFG_SELECT        0x00
#define VI_CFG_SUBSEL        0x01
#define VI_CFG_SIZE          0x02
#define VI_CFG_PAYLOAD       0x08

#define VI_SEL_ID_NAME       0x01
#define VI_SEL_EV_BITS       0x11

/* Linux evdev event types + the codes we translate. These are the ABI the
 * device speaks; they are Linux's numbers, not ours. */
#define EV_SYN  0x00
#define EV_KEY  0x01
#define EV_REL  0x02
#define EV_ABS  0x03

#define REL_WHEEL 0x08
#define ABS_X     0x00
#define ABS_Y     0x01

#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed));

struct vring_avail_q { uint16_t flags; uint16_t idx; uint16_t ring[VI_QSIZE]; }
    __attribute__((packed));
struct vring_used_q  { uint16_t flags; uint16_t idx; struct vring_used_elem ring[VI_QSIZE]; }
    __attribute__((packed));

/* One ring set per device. Static, not kmalloc'd: a descriptor addresses
 * PHYSICAL memory and KV2P() only works on the kernel image window, not on the
 * heap -- the same constraint virtio_blk.c records for its rings. */
struct vi_dev {
    struct virtio_pci_dev vd;
    struct vring_desc     desc[VI_QSIZE]  __attribute__((aligned(16)));
    struct vring_avail_q  avail           __attribute__((aligned(2)));
    struct vring_used_q   used            __attribute__((aligned(4)));
    struct virtio_input_event ev[VI_QSIZE];
    uint16_t qsize;
    uint16_t notify_off;
    uint16_t last_used;
    bool     is_keyboard;
    bool     up;
};

static struct vi_dev g_vi[VI_MAX_DEVS];
static uint32_t      g_vi_count;

/* The tablet reports ABS_X/ABS_Y in a device-defined range; QEMU uses 0..32767.
 * We read the real maximum out of the config rather than assuming, because a
 * wrong range does not fail -- it silently squashes the cursor into a corner. */
static int32_t g_abs_max_x = 32767, g_abs_max_y = 32767;
static int32_t g_last_x, g_last_y;
static uint32_t g_buttons;

/* Counters, for the boot self-test. "The device came up" and "events actually
 * arrive" are different claims, and only the second one is worth making --
 * a queue that is armed but never filled looks identical to a working one
 * until someone presses a key. */
static uint32_t g_key_events, g_ptr_events;

/* --- evdev keycode -> this OS's key code + unshifted ASCII ------------------
 * Only the keys a keyboard actually has. Everything absent maps to 0 and is
 * dropped, which is the honest answer for a key we cannot name -- injecting a
 * wrong code would be worse than injecting none. */
static uint16_t vi_translate(uint16_t code, char *ascii) {
    *ascii = 0;
    /* evdev's number row and letters are contiguous, so the common cases are
     * arithmetic rather than table lookups. KEY_1..KEY_9 = 2..10, KEY_0 = 11. */
    if (code >= 2 && code <= 10) { *ascii = (char)('1' + (code - 2)); return (uint16_t)*ascii; }
    if (code == 11)              { *ascii = '0'; return '0'; }

    static const char row_q[] = "qwertyuiop";
    static const char row_a[] = "asdfghjkl";
    static const char row_z[] = "zxcvbnm";
    if (code >= 16 && code <= 25) { *ascii = row_q[code - 16]; return (uint16_t)*ascii; }
    if (code >= 30 && code <= 38) { *ascii = row_a[code - 30]; return (uint16_t)*ascii; }
    if (code >= 44 && code <= 50) { *ascii = row_z[code - 44]; return (uint16_t)*ascii; }

    switch (code) {
    case 1:   *ascii = 0x1B; return 0x1B;              /* Esc        */
    case 12:  *ascii = '-';  return '-';
    case 13:  *ascii = '=';  return '=';
    case 14:  *ascii = '\b'; return '\b';               /* Backspace  */
    case 15:  *ascii = '\t'; return '\t';               /* Tab        */
    case 26:  *ascii = '[';  return '[';
    case 27:  *ascii = ']';  return ']';
    case 28:  *ascii = '\n'; return '\n';               /* Enter      */
    case 39:  *ascii = ';';  return ';';
    case 40:  *ascii = '\''; return '\'';
    case 41:  *ascii = '`';  return '`';
    case 43:  *ascii = '\\'; return '\\';
    case 51:  *ascii = ',';  return ',';
    case 52:  *ascii = '.';  return '.';
    case 53:  *ascii = '/';  return '/';
    case 57:  *ascii = ' ';  return ' ';                /* Space      */

    case 29:  return EKC_LCTRL;
    case 42:  return EKC_LSHIFT;
    case 54:  return EKC_RSHIFT;
    case 56:  return EKC_LALT;
    case 58:  return EKC_CAPS;
    case 97:  return EKC_RCTRL;
    case 100: return EKC_RALT;
    case 125: return EKC_LWIN;
    case 126: return EKC_RWIN;
    case 127: return EKC_MENU;

    case 59: return EKC_F1;  case 60: return EKC_F2;  case 61: return EKC_F3;
    case 62: return EKC_F4;  case 63: return EKC_F5;  case 64: return EKC_F6;
    case 65: return EKC_F7;  case 66: return EKC_F8;  case 67: return EKC_F9;
    case 68: return EKC_F10; case 87: return EKC_F11; case 88: return EKC_F12;

    case 69: return EKC_NUM;
    case 70: return EKC_SCROLL;

    /* Navigation. These carry BOTH an event code and a private control byte:
     * the char stream is how the shell's history recall and the terminal's
     * scrollback read them (keyboard.h's three-way contract). */
    case 102: *ascii = EK_HOME;  return EKC_HOME;
    case 103: *ascii = EK_UP;    return EKC_UP;
    case 104: *ascii = EK_PGUP;  return EKC_PGUP;
    case 105: *ascii = EK_LEFT;  return EKC_LEFT;
    case 106: *ascii = EK_RIGHT; return EKC_RIGHT;
    case 107: *ascii = EK_END;   return EKC_END;
    case 108: *ascii = EK_DOWN;  return EKC_DOWN;
    case 109: *ascii = EK_PGDN;  return EKC_PGDN;
    case 110: return EKC_INS;
    case 111: *ascii = EK_DEL;   return EKC_DEL;

    default: return 0;
    }
}

/* Shift a printable key's ASCII the way the PS/2 path's shift table does. Only
 * the US layout, and deliberately so: keyboard.c owns layouts for the scancode
 * world, and wiring evdev into that table is a separate change (docs/TODO.md). */
static char vi_shift(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    switch (c) {
    case '1': return '!'; case '2': return '@'; case '3': return '#';
    case '4': return '$'; case '5': return '%'; case '6': return '^';
    case '7': return '&'; case '8': return '*'; case '9': return '(';
    case '0': return ')'; case '-': return '_'; case '=': return '+';
    case '[': return '{'; case ']': return '}'; case '\\': return '|';
    case ';': return ':'; case '\'': return '"'; case '`': return '~';
    case ',': return '<'; case '.': return '>'; case '/': return '?';
    default: return c;
    }
}

/* Read one device-config field. select/subsel latch, then size says how many
 * payload bytes are valid. */
static uint8_t vi_cfg(struct vi_dev *d, uint8_t sel, uint8_t subsel,
                      uint8_t *out, uint8_t max)
{
    vp_w8(d->vd.devcfg, VI_CFG_SELECT, sel);
    vp_w8(d->vd.devcfg, VI_CFG_SUBSEL, subsel);
    uint8_t size = vp_r8(d->vd.devcfg, VI_CFG_SIZE);
    if (size > max) size = max;
    for (uint8_t i = 0; i < size; i++)
        out[i] = vp_r8(d->vd.devcfg, VI_CFG_PAYLOAD + i);
    return size;
}

/* Hand every buffer to the device. Each event is ONE device-writable
 * descriptor -- virtio-input has no request/response shape, the device just
 * fills buffers as things happen. */
static void vi_arm_all(struct vi_dev *d) {
    for (uint16_t i = 0; i < d->qsize; i++) {
        d->desc[i].addr  = KV2P((uint64_t)(uintptr_t)&d->ev[i]);
        d->desc[i].len   = sizeof(struct virtio_input_event);
        d->desc[i].flags = VRING_DESC_F_WRITE;
        d->desc[i].next  = 0;
        d->avail.ring[i] = i;
    }
    __asm__ volatile("" ::: "memory");
    d->avail.idx = d->qsize;
    virtio_pci_notify(&d->vd, d->notify_off, 0);
}

static void vi_handle(const struct virtio_input_event *e) {
    switch (e->type) {
    case EV_KEY: {
        if (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == BTN_MIDDLE) {
            uint32_t bit = (e->code == BTN_LEFT)  ? MOUSE_BTN_LEFT :
                           (e->code == BTN_RIGHT) ? MOUSE_BTN_RIGHT
                                                  : MOUSE_BTN_MIDDLE;
            if (e->value) g_buttons |= bit; else g_buttons &= ~bit;
            mouse_set_absolute(g_last_x, g_last_y, g_abs_max_x, g_buttons, 0);
            return;
        }
        /* value 2 is auto-repeat; treat it as another press, which is what a
         * held key should look like to a text reader. */
        if (e->value > 2) return;
        g_key_events++;
        char ascii = 0;
        uint16_t code = vi_translate(e->code, &ascii);
        if (!code) return;
        if (ascii) {
            uint8_t m = keyboard_mods();
            /* Caps Lock affects letters only; Shift affects everything. The
             * two XOR for letters, which is why a shifted letter under Caps is
             * lower case -- the behaviour every other keyboard has. */
            bool upper = (m & EKM_SHIFT) != 0;
            if ((m & EKM_CAPS) && ascii >= 'a' && ascii <= 'z')
                upper = !upper;
            if (upper) ascii = vi_shift(ascii);
            /* Ctrl+letter becomes the control code, exactly as the PS/2 path
             * produces it -- user/lib depends on that (a printable byte is
             * never treated as a shortcut; see Note++'s paste bug). */
            if ((m & EKM_CTRL) && ((ascii >= 'a' && ascii <= 'z') ||
                                   (ascii >= 'A' && ascii <= 'Z')))
                ascii = (char)(ascii & 0x1F);
        }
        keyboard_inject_event(code, e->value != 0, ascii);
        return;
    }
    case EV_ABS:
        g_ptr_events++;
        if (e->code == ABS_X) g_last_x = (int32_t)e->value;
        else if (e->code == ABS_Y) g_last_y = (int32_t)e->value;
        return;
    case EV_REL:
        if (e->code == REL_WHEEL)
            mouse_set_absolute(g_last_x, g_last_y, g_abs_max_x, g_buttons,
                               (int32_t)e->value);
        return;
    case EV_SYN:
        /* The frame is complete: publish the accumulated position. Doing it
         * here rather than per-axis is what stops the cursor from tracking
         * through an intermediate (newX, oldY) point on every move. */
        mouse_set_absolute(g_last_x, g_last_y, g_abs_max_x, g_buttons, 0);
        return;
    default:
        return;
    }
}

void virtio_input_poll(void) {
    for (uint32_t i = 0; i < g_vi_count; i++) {
        struct vi_dev *d = &g_vi[i];
        if (!d->up) continue;

        while (d->last_used != d->used.idx) {
            __asm__ volatile("" ::: "memory");
            uint16_t slot = (uint16_t)(d->last_used % d->qsize);
            uint32_t id   = d->used.ring[slot].id;
            if (id < d->qsize)
                vi_handle(&d->ev[id]);

            /* Recycle the buffer immediately: the queue is the only place
             * events can land, and a buffer left un-armed is an event the
             * device has nowhere to put. */
            uint16_t a = (uint16_t)(d->avail.idx % d->qsize);
            d->avail.ring[a] = (uint16_t)id;
            __asm__ volatile("" ::: "memory");
            d->avail.idx++;
            d->last_used++;
        }
        virtio_pci_notify(&d->vd, d->notify_off, 0);
    }
}

void virtio_input_init(void) {
    kprintf("\n=== virtio-input ===\n");

    for (uint32_t idx = 0; idx < VI_MAX_DEVS; idx++) {
        const struct pci_device *pci = virtio_pci_find(VIRTIO_INPUT_DEVID, idx);
        if (!pci)
            break;

        struct vi_dev *d = &g_vi[g_vi_count];
        memset(d, 0, sizeof(*d));

        if (!virtio_pci_attach(&d->vd, pci, "virtio-input"))
            continue;

        /* WHAT IS THIS DEVICE? Ask what event types it can produce. A device
         * with EV_KEY bit 1 (KEY_ESC) set is a keyboard; one that reports
         * EV_ABS is a tablet. Probing by capability rather than by the name
         * string, which belongs to QEMU and is not part of any contract. */
        uint8_t bits[128];
        uint8_t n = vi_cfg(d, VI_SEL_EV_BITS, EV_KEY, bits, sizeof bits);
        d->is_keyboard = (n > 0) && (bits[0] & 0x02);   /* KEY_ESC == 1 */

        uint8_t nabs = vi_cfg(d, VI_SEL_EV_BITS, EV_ABS, bits, sizeof bits);
        bool is_tablet = (nabs > 0) && (bits[0] & 0x03);  /* ABS_X | ABS_Y */

        /* The tablet's real coordinate range, read rather than assumed. */
        if (is_tablet) {
            uint8_t ai[24];
            if (vi_cfg(d, 0x12 /* ID_ABS_INFO */, ABS_X, ai, sizeof ai) >= 8)
                g_abs_max_x = (int32_t)((uint32_t)ai[4] | ((uint32_t)ai[5] << 8) |
                                        ((uint32_t)ai[6] << 16) | ((uint32_t)ai[7] << 24));
            if (vi_cfg(d, 0x12, ABS_Y, ai, sizeof ai) >= 8)
                g_abs_max_y = (int32_t)((uint32_t)ai[4] | ((uint32_t)ai[5] << 8) |
                                        ((uint32_t)ai[6] << 16) | ((uint32_t)ai[7] << 24));
            if (g_abs_max_x <= 0) g_abs_max_x = 32767;
            if (g_abs_max_y <= 0) g_abs_max_y = 32767;
        }

        /* Queue 0 is the event queue (device -> driver). Queue 1 is status
         * (LEDs, rumble) and is deliberately not set up: nothing drives it. */
        d->qsize = virtio_pci_setup_queue(&d->vd, 0, VI_QSIZE, d->desc,
                                          &d->avail, &d->used, &d->notify_off);
        if (d->qsize == 0) {
            kprintf("virtio-input: device %u has no event queue\n", idx);
            continue;
        }

        /* Polled: tell the device not to raise its INTx line. Without this a
         * level-triggered interrupt nothing acknowledges wedges the machine --
         * the exact failure the PCI routing self-test caused once already
         * (docs/TODO.md, "a diagnostic must not arm anything"). */
        d->avail.flags = VRING_AVAIL_F_NO_INTERRUPT;

        virtio_pci_driver_ok(&d->vd);
        vi_arm_all(d);
        d->up = true;
        g_vi_count++;

        kprintf("virtio-input: %u:%u.%u is a %s, queue %u, polled\n",
                pci->bus, pci->device, pci->function,
                d->is_keyboard ? "keyboard" : (is_tablet ? "tablet" : "device"),
                d->qsize);
    }

    if (g_vi_count == 0)
        kprintf("virtio-input: no device\n");
}

bool virtio_input_present(void) { return g_vi_count > 0; }

void virtio_input_stats(uint32_t *keys, uint32_t *pointer) {
    if (keys)    *keys    = g_key_events;
    if (pointer) *pointer = g_ptr_events;
}
