#include "gfx/ui_events.h"
/* Set while a self-test pushes its own keys through the live rings: they are
 * not anyone's input, so no application is woken for them -- a woken focused
 * app would poll the test's keys away from it. */
static volatile int g_selftest_quiet;
#include "include/kstring.h"
#if defined(__x86_64__)
#include "power/policy.h"
#define INPUT_ACTIVITY() power_activity()
#else
#define INPUT_ACTIVITY() ((void)0)
#endif
#include "include/spinlock.h"
#include "drivers/input/keyboard.h"
/* PS/2 is an x86 device, and this file is now TWO things: the PS/2 controller
 * driver, and the keyboard POLICY every input source shares -- the char ring,
 * the key-event ring, modifier tracking, layouts, the Ctrl-C route and the
 * grab. Only the first half is x86, and only the first half is behind the
 * guards below. ARM64.md §2.6 said virtio-input "replaces" the PS/2 keyboard;
 * it replaces the HARDWARE half, and duplicating the other 300 lines to say so
 * would have been the real mistake -- two ring buffers, two grab rules, two
 * places for a stuck-modifier bug to live. See keyboard_inject_event(). */
#if defined(__x86_64__)
#include "arch/x86_64/irq/irq.h"
#include "drivers/input/i8042.h"
#include "include/kprintf.h"
#include "include/io.h"
#endif
#include "drivers/char/serial.h"
#include "include/arch_irq.h"   /* arch_cpu_idle() -- see keyboard_getchar() */
#if !defined(__x86_64__)
#include "drivers/input/virtio_input.h"   /* the LEDs live on its status queue */
#endif
#include "process/process.h"
#include "include/errno.h"


#include <stdint.h>


#define KBD_DATA_PORT 0x60
#define KBD_STATUS_PORT 0x64

/* Readers blocked on an empty buffer. Zero-init is sufficient: struct
 * wait_queue is just { struct thread *head; }, and head == NULL IS the
 * empty state -- no sentinel to construct. */
static struct wait_queue kbd_wait_queue;   /* for keyboard_deliver() to wait on */

// Scancode to ASCII translaation - US QWERTY, Set 1
// Indexed by scancode (0-0x7F for "pressed" code)
// 0 = unmapped / special key

static const uint32_t scan_to_cp_us[128] = {
    0,   0x1B, '1', '2', '3', '4', '5', '6', '7', '8',   // 0x00-0x09
    '9', '0', '-', '=', '\b','\t','q', 'w', 'e', 'r',    // 0x0A-0x13
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,     // 0x14-0x1D (1D=LeftCtrl)
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',    // 0x1E-0x27
    '\'','`',  0,  '\\','z', 'x', 'c', 'v', 'b', 'n',    // 0x28-0x31 (2A=LeftShift)
    'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,      // 0x32-0x3B (36=RightShift, 38=LeftAlt)
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x3C-0x45 (F-keys, NumLock, ScrollLock)
    0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,      // 0x46-0x4F (keypad)
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x50-0x59
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x5A-0x63
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x64-0x6D
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x6E-0x77
    0,   0,   0,   0,   0,   0,   0,   0,                // 0x78-0x7F
};

/* SHIFTED layer, US QWERTY. Existed as a gap until the pipeline shell made
 * it absurd: a shell whose core operator is '|' on an OS whose keyboard
 * could not TYPE '|' (shift produced the unshifted char -- no uppercase, no
 * !@#$, no quotes-vs-apostrophe either). Same indexing as scan_to_cp_us. */
static const uint32_t scan_to_cp_shift[128] = {
    0,   0x1B, '!', '@', '#', '$', '%', '^', '&', '*',   // 0x00-0x09
    '(', ')', '_', '+', '\b','\t','Q', 'W', 'E', 'R',    // 0x0A-0x13
    'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,     // 0x14-0x1D
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',    // 0x1E-0x27
    '"', '~',  0,  '|', 'Z', 'X', 'C', 'V', 'B', 'N',    // 0x28-0x31
    'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,      // 0x32-0x3B
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x3C-0x45
    0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,      // 0x46-0x4F (keypad)
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x50-0x59
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x5A-0x63
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x64-0x6D
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x6E-0x77
    0,   0,   0,   0,   0,   0,   0,   0,                // 0x78-0x7F
};


/* DVORAK, and why this layout and not AZERTY.
 *
 * A layout is just these two tables -- scancode->key is hardware, key->char is
 * policy -- so this is what "support keymaps" actually costs. Dvorak was chosen
 * to PROVE that, because it is pure ASCII.
 *
 * AZERTY USED TO BE REFUSED HERE, and the refusal was right at the time: French
 * needs é è ç à ù, the char stream was 7-bit, and an "AZERTY" built on it could
 * only have been QWERTY-with-letters-moved and silent holes where the accents
 * belong -- a worse lie than saying no. The stream carries UTF-8 now and the
 * tables carry codepoints, so the real layout ships: see scan_to_cp_azerty. */
static const uint32_t scan_to_cp_dvorak[128] = {
    0,   0x1B, '1', '2', '3', '4', '5', '6', '7', '8',   // 0x00-0x09
    '9', '0', '[', ']', '\b','\t','\'',',', '.', 'p',    // 0x0A-0x13
    'y', 'f', 'g', 'c', 'r', 'l', '/', '=', '\n', 0,     // 0x14-0x1D
    'a', 'o', 'e', 'u', 'i', 'd', 'h', 't', 'n', 's',    // 0x1E-0x27
    '-', '`',  0,  '\\',';', 'q', 'j', 'k', 'x', 'b',    // 0x28-0x31
    'm', 'w', 'v', 'z', 0,   '*', 0,   ' ', 0,   0,      // 0x32-0x3B
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x3C-0x45
    0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,      // 0x46-0x4F
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x50-0x59
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x5A-0x63
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x64-0x6D
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x6E-0x77
    0,   0,   0,   0,   0,   0,   0,   0,                // 0x78-0x7F
};
static const uint32_t scan_to_cp_dvorak_shift[128] = {
    0,   0x1B, '!', '@', '#', '$', '%', '^', '&', '*',   // 0x00-0x09
    '(', ')', '{', '}', '\b','\t','"', '<', '>', 'P',    // 0x0A-0x13
    'Y', 'F', 'G', 'C', 'R', 'L', '?', '+', '\n', 0,     // 0x14-0x1D
    'A', 'O', 'E', 'U', 'I', 'D', 'H', 'T', 'N', 'S',    // 0x1E-0x27
    '_', '~',  0,  '|', ':', 'Q', 'J', 'K', 'X', 'B',    // 0x28-0x31
    'M', 'W', 'V', 'Z', 0,   '*', 0,   ' ', 0,   0,      // 0x32-0x3B
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x3C-0x45
    0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,      // 0x46-0x4F
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x50-0x59
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x5A-0x63
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x64-0x6D
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,      // 0x6E-0x77
    0,   0,   0,   0,   0,   0,   0,   0,                // 0x78-0x7F
};

/* Live modifier state (EKM_*). File-scope, not a handler static, because it is
 * now a QUERY (keyboard_mods) and it stamps every key event. */
static volatile uint8_t g_mods;

/* Defined below (they need the tables above and the 8042 helpers below); the
 * IRQ handler sits between the two and has to reach both. */
static spinlock_t input_state_lock=SPINLOCK_INIT;
static void kbd_set_leds(void);
static const struct keymap *g_layout;

/* The key-event ring. Separate from the char ring on purpose -- see keyboard.h.
 * Overflow DROPS THE NEWEST rather than overwriting the oldest: losing the tail
 * of a burst is recoverable, but silently eating a key-UP would strand a
 * modifier "held" forever, and a stuck Ctrl is far worse than a dropped key. */
#define KBD_EVENT_RING 64
static struct key_event ev_ring[KBD_EVENT_RING];
static volatile uint32_t ev_head, ev_tail;

static void kbd_event_push(uint16_t code, int pressed) {
    INPUT_ACTIVITY();
    uint32_t nh = (ev_head + 1) % KBD_EVENT_RING;
    if (nh == ev_tail) return;             /* full: drop the newest (see above) */
    ev_ring[ev_head].code    = code;
    ev_ring[ev_head].mods    = g_mods;
    ev_ring[ev_head].pressed = (uint8_t)(pressed ? 1 : 0);
    ev_head = nh;
}

// Circular buffer for storing incoming characters from the keyboard
#define KBD_BUFFER_SIZE 128
static char kbd_buffer[KBD_BUFFER_SIZE];
static uint8_t kbd_buffer_mods[KBD_BUFFER_SIZE];
static uint16_t kbd_buffer_code[KBD_BUFFER_SIZE];   /* which key (see keyboard_poll_key) */
static uint8_t kbd_buffer_flags[KBD_BUFFER_SIZE];   /* KBF_* */
static volatile uint32_t buf_head = 0;   // write index (IRQ writes here)
static volatile uint32_t buf_tail = 0;   // read index (kernel reads here)

/* The key being delivered right now, stamped on the FIRST byte it produces and
 * cleared there, so a multi-byte character's later bytes carry no key. Set
 * only around a delivery (deliver_key*), never left behind for a later byte. */
static uint16_t g_key_code;
static uint8_t g_key_flags;

// Push a character into the circular buffer (called from IRQ handler or USB HID)
static void buffer_push_entry(char c, uint8_t flags) {
    uint32_t next_head = (buf_head + 1) % KBD_BUFFER_SIZE;
    if (next_head == buf_tail) {
        return;
    
    }
    kbd_buffer[buf_head] = c;
    kbd_buffer_mods[buf_head] = g_mods;
    kbd_buffer_code[buf_head] = g_key_code;
    kbd_buffer_flags[buf_head] = (uint8_t)(g_key_flags | flags);
    g_key_code = 0; g_key_flags = 0;
    buf_head = next_head;
}
static void buffer_push(char c) { buffer_push_entry(c, 0); }

/* Character readers never see a key that has no character. */
static void drop_key_only(void) {
    while (buf_head != buf_tail && (kbd_buffer_flags[buf_tail] & KBF_KEYONLY))
        buf_tail = (buf_tail + 1) % KBD_BUFFER_SIZE;
}

// pop a character from the circular buffer, return 0 if buffer is empty (called from keyboard_getchar)
static int buffer_pop(char *c) {
    drop_key_only();
    if (buf_head == buf_tail) {
        return 0;
    }
    *c = kbd_buffer[buf_tail];
    buf_tail = (buf_tail + 1) % KBD_BUFFER_SIZE;
    return 1;
}

/* Every push site (PS/2 IRQ, USB HID) MUST go through here so the push and the
 * wake are one atomic step -- a bare buffer_push() lands the char but never
 * wakes a reader blocked in keyboard_getchar_blocking(), which is a lost
 * wakeup (the read hangs forever). Defined below; forward-declared so the IRQ
 * handler can reach it. */
static void keyboard_deliver(char c);
static void keyboard_deliver_key_only(uint16_t code, uint8_t flags);
static void deliver_key(uint16_t code, uint8_t flags, char c) {
    g_key_code = code; g_key_flags = flags;
    keyboard_deliver(c);
    g_key_code = 0; g_key_flags = 0;
}

/* The pending system shortcut, decided in IRQ context and performed by the
 * kernel's main loop -- see SYSKEY_* in keyboard.h. */
static volatile int g_syskey;
static volatile unsigned g_shortcut_modifier;
void keyboard_set_shortcut_modifier(unsigned mode){g_shortcut_modifier=mode;}
int keyboard_take_syskey(void) { int k = g_syskey; g_syskey = SYSKEY_NONE; return k; }
void        kbd_translate(uint8_t make, int pressed);   /* the testable seam */
static void keyboard_deliver_cp(uint32_t cp);   /* one codepoint, as UTF-8 */
static void deliver_key_cp(uint16_t code, uint8_t flags, uint32_t cp) {
    g_key_code = code; g_key_flags = flags;
    keyboard_deliver_cp(cp);
    g_key_code = 0; g_key_flags = 0;
}
static uint32_t kbd_compose(uint32_t mark, uint32_t base);
static uint32_t g_dead;                          /* pending dead-key mark, or 0 */


/* The EK_* navigation codes now live in keyboard.h -- they are a contract
 * shared with the USB HID driver (usb/xhci.c) and userland (EMBK_KEY_*), not
 * a PS/2 private. Keeping them here is what left the USB path with no
 * navigation keys at all. */

/* Set a modifier bit from a make/break, and emit the event for the modifier KEY
 * itself. `bit` is the EKM_* to hold while down. Shift/Ctrl/Alt/GUI are LEVELS:
 * each has two physical keys, so we cannot just clear the bit on a break -- we
 * track the two sides and clear only when BOTH are up, or releasing one Shift
 * while the other is held would wrongly un-shift. */
static uint8_t g_side;                       /* which physical modifier keys are down */
#define SIDE_LSHIFT 0x01
#define SIDE_RSHIFT 0x02
#define SIDE_LCTRL  0x04
#define SIDE_RCTRL  0x08
#define SIDE_LALT   0x10
#define SIDE_RALT   0x20
#define SIDE_LGUI   0x40
#define SIDE_RGUI   0x80

static void mod_update(uint8_t side_bit, uint8_t pair_mask, uint8_t ekm,
                       uint16_t code, int pressed)
{
    if (pressed) g_side |= side_bit; else g_side &= (uint8_t)~side_bit;
    if (g_side & pair_mask) g_mods |= ekm; else g_mods &= (uint8_t)~ekm;
    kbd_event_push(code, pressed);
}

/* The locks. A latch, not a level: it flips on the MAKE and ignores the break
 * entirely (holding Caps Lock down does not "hold" anything), which is exactly
 * how it differs from Shift. The LED is the only feedback the user gets. */
static void lock_toggle(uint8_t ekm, uint16_t code, int pressed,int leds)
{
    if (pressed) { g_mods ^= ekm; if(leds)kbd_set_leds(); }
    kbd_event_push(code, pressed);
}

/* Set-1 decoding is logic, not hardware: the 8042 interrupt below feeds it on
 * x86, and every HID keyboard (USB, I2C) feeds it on both architectures, so a
 * USB keyboard on a Raspberry Pi gets the same layouts, modifiers and key
 * identities as a laptop's PS/2 one. */
static int interface_key(uint16_t key, uint32_t base, uint32_t cp);   /* below */
static void keyboard_scancode(uint8_t sc,int *prefix,int leds) {
    int extended = *prefix;     /* the previous byte was the 0xE0 prefix */

    if (sc == 0xE0) { *prefix = 1; return; }   /* prefix: next byte is extended */
    if (sc == 0xE1) { return; }                 /* Pause/Break's 6-byte burst: not decoded */

    int     pressed = !(sc & 0x80);             /* break codes are make | 0x80 */
    uint8_t make    = sc & 0x7F;

    if (extended) {
        *prefix = 0;
        /* An 0xE0-prefixed 0x2A/0xAA is a FAKE shift: set 1 pads it around the
         * nav keys so a shift-unaware BIOS still sees sane codes. It must never
         * touch real shift state, and it is not a key -- drop it silently. */
        if (make == 0x2A || make == 0x36) return;

        switch (make) {
            case 0x1D: mod_update(SIDE_RCTRL, SIDE_LCTRL|SIDE_RCTRL, EKM_CTRL, EKC_RCTRL, pressed); return;
            case 0x38: mod_update(SIDE_RALT,  SIDE_LALT|SIDE_RALT,   EKM_ALT,  EKC_RALT,  pressed); return;
            case 0x5B: mod_update(SIDE_LGUI,  SIDE_LGUI|SIDE_RGUI,   EKM_GUI,  EKC_LWIN,  pressed); return;
            case 0x5C: mod_update(SIDE_RGUI,  SIDE_LGUI|SIDE_RGUI,   EKM_GUI,  EKC_RWIN,  pressed); return;
            case 0x5D: kbd_event_push(EKC_MENU, pressed);
                       if (pressed) keyboard_deliver_key_only(EKC_MENU, 0);
                       return;
            default: break;
        }

        /* The real nav cluster (the keypad's twins are handled below).
         *
         * WITH SHIFT HELD these four deliver a SELECTION command instead of a
         * movement. The decision belongs here because this is where the
         * modifier state is known at the instant the key went down -- the char
         * stream carries no modifiers, so Shift+Left and Left are otherwise
         * literally the same byte and no reader downstream could ever tell
         * them apart. See EK_SEL_* in keyboard.h. */
        int sel = (g_mods & EKM_SHIFT) != 0;
        uint16_t kc = 0; char ch = 0;
        switch (make) {
            case 0x4B: kc = EKC_LEFT;  ch = sel ? (char)EK_SEL_LEFT  : (char)EK_LEFT;  break;
            case 0x4D: kc = EKC_RIGHT; ch = sel ? (char)EK_SEL_RIGHT : (char)EK_RIGHT; break;
            case 0x48: kc = EKC_UP;    ch = EK_UP;    break;
            case 0x50: kc = EKC_DOWN;  ch = EK_DOWN;  break;
            case 0x47: kc = EKC_HOME;  ch = sel ? (char)EK_SEL_HOME  : (char)EK_HOME;  break;
            case 0x4F: kc = EKC_END;   ch = sel ? (char)EK_SEL_END   : (char)EK_END;   break;
            case 0x53: kc = EKC_DEL;   ch = EK_DEL;   break;
            case 0x49: kc = EKC_PGUP;  ch = EK_PGUP;  break;
            case 0x51: kc = EKC_PGDN;  ch = EK_PGDN;  break;
            case 0x52: kc = EKC_INS;   ch = 0;        break;  /* no char: C0 is full */
            case 0x1C: kc = '\n';      ch = '\n';     break;  /* keypad Enter */
            case 0x35: kc = '/';       ch = '/';      break;  /* keypad / */
            default: return;
        }
        kbd_event_push(kc, pressed);
        uint8_t pad = (make == 0x1C || make == 0x35) ? KBF_KEYPAD : 0;
        /* the system's own first (GUI+Up), as every other key is */
        if (pressed && ch && interface_key(kc, 0, (uint32_t)(unsigned char)ch)) return;
        if (pressed && ch) deliver_key(kc, pad, ch);
        else if (pressed) keyboard_deliver_key_only(kc, 0);
        return;
    }

    /* --- modifiers and locks (the breaks we must NOT ignore) --- */
    switch (make) {
        case 0x2A: mod_update(SIDE_LSHIFT, SIDE_LSHIFT|SIDE_RSHIFT, EKM_SHIFT, EKC_LSHIFT, pressed); return;
        case 0x36: mod_update(SIDE_RSHIFT, SIDE_LSHIFT|SIDE_RSHIFT, EKM_SHIFT, EKC_RSHIFT, pressed); return;
        case 0x1D: mod_update(SIDE_LCTRL,  SIDE_LCTRL|SIDE_RCTRL,   EKM_CTRL,  EKC_LCTRL,  pressed); return;
        case 0x38: mod_update(SIDE_LALT,   SIDE_LALT|SIDE_RALT,     EKM_ALT,   EKC_LALT,   pressed); return;
        case 0x3A: lock_toggle(EKM_CAPS,   EKC_CAPS,   pressed,leds); return;
        case 0x45: lock_toggle(EKM_NUM,    EKC_NUM,    pressed,leds); return;
        case 0x46: lock_toggle(EKM_SCROLL, EKC_SCROLL, pressed,leds); return;
        default: break;
    }

    /* --- F1-F12: event-only. There is no byte for them (C0 is Ctrl+letter's),
     * and inventing an escape sequence would force every reader to become a
     * state machine. They are exactly what the event stream is for. --- */
    {
        uint16_t f = make >= 0x3B && make <= 0x44 ? (uint16_t)(EKC_F1 + (make - 0x3B))
                   : make == 0x57 ? EKC_F11 : make == 0x58 ? EKC_F12 : 0;
        if (f) {
            kbd_event_push(f, pressed);
            if (pressed) keyboard_deliver_key_only(f, 0);    /* in order with the text */
            return;
        }
    }

    /* --- the keypad, whose codes COLLIDE with the nav cluster's ---
     * A non-extended 0x47 is keypad-7-or-Home; the real Home arrived above with
     * an 0xE0. Num Lock picks which, and that is the whole of what Num Lock is. */
    if (make >= 0x47 && make <= 0x53) {
        static const char pad_num[] = "789-456+1230.";        /* 0x47..0x53 */
        static const uint16_t pad_nav[] = {
            EKC_HOME, EKC_UP, EKC_PGUP, 0, EKC_LEFT, 0, EKC_RIGHT,
            0, EKC_END, EKC_DOWN, EKC_PGDN, EKC_INS, EKC_DEL };
        static const char pad_nav_ch[] = {
            EK_HOME, EK_UP, EK_PGUP, '-', EK_LEFT, 0, EK_RIGHT,
            '+', EK_END, EK_DOWN, EK_PGDN, 0, EK_DEL };
        int i = make - 0x47;
        if (g_mods & EKM_NUM) {
            char c = pad_num[i];
            if (!c) return;
            kbd_event_push((uint16_t)c, pressed);
            if (pressed) deliver_key((uint16_t)c, KBF_KEYPAD, c);
        } else {
            if (!pad_nav[i]) return;
            kbd_event_push(pad_nav[i], pressed);
            if (pressed && pad_nav_ch[i]) deliver_key(pad_nav[i], KBF_KEYPAD, pad_nav_ch[i]);
            else if (pressed) keyboard_deliver_key_only(pad_nav[i], KBF_KEYPAD);
        }
        return;
    }

    /* --- ordinary keys --- */
    kbd_translate(make, pressed);
}
static uint16_t hid_scancode(uint16_t usage) {
    static const uint16_t scans[128] = {
        [4]=0x1e,[5]=0x30,[6]=0x2e,[7]=0x20,[8]=0x12,[9]=0x21,[10]=0x22,
        [11]=0x23,[12]=0x17,[13]=0x24,[14]=0x25,[15]=0x26,[16]=0x32,
        [17]=0x31,[18]=0x18,[19]=0x19,[20]=0x10,[21]=0x13,[22]=0x1f,
        [23]=0x14,[24]=0x16,[25]=0x2f,[26]=0x11,[27]=0x2d,[28]=0x15,[29]=0x2c,
        [30]=2,[31]=3,[32]=4,[33]=5,[34]=6,[35]=7,[36]=8,[37]=9,[38]=10,[39]=11,
        [40]=0x1c,[41]=1,[42]=0x0e,[43]=0x0f,[44]=0x39,[45]=0x0c,[46]=0x0d,
        [47]=0x1a,[48]=0x1b,[49]=0x2b,[50]=0x2b,[51]=0x27,[52]=0x28,
        [53]=0x29,[54]=0x33,[55]=0x34,[56]=0x35,[57]=0x3a,
        [58]=0x3b,[59]=0x3c,[60]=0x3d,[61]=0x3e,[62]=0x3f,[63]=0x40,
        [64]=0x41,[65]=0x42,[66]=0x43,[67]=0x44,[68]=0x57,[69]=0x58,
        [71]=0x46,[73]=0xe052,[74]=0xe047,[75]=0xe049,[76]=0xe053,
        [77]=0xe04f,[78]=0xe051,[79]=0xe04d,[80]=0xe04b,[81]=0xe050,[82]=0xe048,
        [83]=0x45,[84]=0xe035,[85]=0x37,[86]=0x4a,[87]=0x4e,[88]=0xe01c,
        [89]=0x4f,[90]=0x50,[91]=0x51,[92]=0x4b,[93]=0x4c,[94]=0x4d,
        [95]=0x47,[96]=0x48,[97]=0x49,[98]=0x52,[99]=0x53,[100]=0x56,[101]=0xe05d
    };
    static const uint16_t mods[8]={0x1d,0x2a,0x38,0xe05b,0xe01d,0x36,0xe038,0xe05c};
    uint16_t sc=usage>=0xe0 && usage<=0xe7?mods[usage-0xe0]:(usage<128?scans[usage]:0);
    return sc;
}

static void hid_usage_inner(uint16_t usage, int down);
static void hid_boot_report_inner(uint8_t prev[8], const uint8_t *report, unsigned length, int locked) {
    void (*usage_fn)(uint16_t, int) = locked ? hid_usage_inner : keyboard_inject_hid_usage;
    uint8_t now[8] = {0};
    for (unsigned i = 0; i < 8 && i < length; i++) now[i] = report[i];
    if (now[2] == 1) return;                  /* phantom state (too many keys): says nothing */
    for (unsigned bit = 0; bit < 8; bit++)    /* modifiers first, so a chord's key sees them */
        if (((prev[0] ^ now[0]) >> bit) & 1) usage_fn((uint16_t)(0xE0 + bit), (now[0] >> bit) & 1);
    for (unsigned pass = 0; pass < 2; pass++) /* releases, then presses */
        for (unsigned i = 2; i < 8; i++) {
            uint8_t usage = pass ? now[i] : prev[i];
            if (usage < 4) continue;
            int elsewhere = 0;
            for (unsigned j = 2; j < 8; j++) elsewhere |= (pass ? prev[j] : now[j]) == usage;
            if (!elsewhere) usage_fn(usage, (int)pass);
        }
    for (unsigned i = 0; i < 8; i++) prev[i] = now[i];
}
void keyboard_hid_boot_report(uint8_t prev[8], const uint8_t *report, unsigned length) {
    hid_boot_report_inner(prev, report, length, 0);
}

static void hid_usage_inner(uint16_t usage,int down) {
    uint16_t scan=hid_scancode(usage);if(!scan)return;
    int prefix=0;
    if(scan&0xff00)keyboard_scancode(0xe0,&prefix,0);
    keyboard_scancode((scan&127)|(down?0:128),&prefix,0);
}
void keyboard_inject_hid_usage(uint16_t usage,int down) {
    spin_lock(&input_state_lock);hid_usage_inner(usage,down);spin_unlock(&input_state_lock);
}

/* THE INTERFACE'S KEYS, decided the same for every keyboard: the system
 * shortcuts (recorded for the kernel loop and swallowed) and the GUI editing
 * commands (delivered as their byte). `base` is the key's unshifted character,
 * `cp` what it types now. 1 if the key was one of these and is done.
 *
 * Its own function because it was written inline in kbd_translate -- the PS/2
 * path -- and a virtio keyboard never passes through there: on arm64 GUI+Tab,
 * GUI+W, GUI+Space and the rest typed a tab, a w and a space. Caller holds
 * input_state_lock. */
int compositor_overview_shown(void);   /* gfx/compositor.h; a flag read */
static int interface_key(uint16_t key, uint32_t base, uint32_t cp) {
    /* Esc while the overview is up leaves it: the overview has the keys
     * (no window is in front to take them) */
    if (cp == 0x1b && compositor_overview_shown()) { g_syskey = SYSKEY_OVERVIEW; return 1; }
    /* GUI + letter -> an editing command. The interface's modifier, kept
     * separate from the terminal's: Ctrl+C is an INTERRUPT in this OS and
     * always will be, so the clipboard cannot live there. See EK_SEL_ALL and
     * friends in keyboard.h for the whole argument.
     *
     * Delivered as a RAW BYTE, not through keyboard_deliver_cp: these are byte
     * values, not codepoints, and encoding 0xFD as UTF-8 would turn one command
     * into the two bytes of ý. Unclaimed GUI+letter combinations fall through
     * and type their letter, so the key is not a hole. */
    if(g_shortcut_modifier?((g_mods&(EKM_CTRL|EKM_ALT))==(EKM_CTRL|EKM_ALT)):(g_mods&EKM_GUI)) {
        char system=base;
        if(system>='A' && system<='Z')system+=32;
        if(system=='\t'){g_syskey=SYSKEY_NEXT_WINDOW;return 1;}
        if(system=='w'){g_syskey=SYSKEY_CLOSE_WINDOW;return 1;}
        if(system=='q'){g_syskey=SYSKEY_QUIT_APP;return 1;}
        if(system=='m'){g_syskey=SYSKEY_MINIMIZE;return 1;}
        if(key==EKC_UP){g_syskey=SYSKEY_OVERVIEW;return 1;}
        if(system==' '){g_syskey=SYSKEY_LAUNCHER;return 1;}
        if(system=='['){g_syskey=SYSKEY_SPACE_PREVIOUS;return 1;}
        if(system==']'){g_syskey=SYSKEY_SPACE_NEXT;return 1;}
        if(system>='1' && system<='6') {g_syskey=((g_mods&EKM_SHIFT)?SYSKEY_MOVE_SPACE_1:SYSKEY_SPACE_1)+system-'1';return 1;}
        /* GUI+K: THE KERNEL LOG, ON SCREEN. The one question a machine with no
         * serial port and no working mouse can still be asked. */
        if(system=='k'){g_syskey=SYSKEY_KERNEL_LOG;return 1;}
        /* GUI+Shift+P: what composing costs, on the screen (comp_stats.h). */
        if(system=='p' && (g_mods&EKM_SHIFT)){g_syskey=SYSKEY_PERF_HUD;return 1;}
    }
    if (g_mods & EKM_GUI) {
        uint32_t low = (cp >= 'A' && cp <= 'Z') ? cp + 0x20 : cp;
        /* SYSTEM shortcuts first, and they are swallowed: see SYSKEY_* in
         * keyboard.h for why an application must not be able to see -- and so
         * decline -- the window switcher. Recorded, not performed: this is IRQ
         * context and the compositor is not safe to call from one. */



        char cmd = 0;
        switch (low) {
            case 'a': cmd = (char)EK_SEL_ALL; break;
            case 'c': cmd = (char)EK_COPY;    break;
            case 'x': cmd = (char)EK_CUT;     break;
            case 'v': cmd = (char)EK_PASTE;   break;
            /* UNDO is GUI+Z and REDO is GUI+Shift+Z -- the shift is read here
             * because the char stream cannot carry it, so a reader downstream
             * could never tell the two apart. GUI+Y is the other spelling of
             * redo and costs one line to honour. */
            case 'z': cmd = (g_mods & EKM_SHIFT) ? (char)EK_REDO : (char)EK_UNDO; break;
            case 'y': cmd = (char)EK_REDO;    break;
            default: break;
        }
        if (cmd) { deliver_key(key, 0, cmd); return 1; }
    }
    return 0;
}

/* THE TRANSLATION, AS A FUNCTION YOU CAN CALL. Split out of the interrupt
 * handler so a selftest can drive a layout with make codes and read what comes
 * out of the char stream -- there is no other way to test a keymap without an
 * 8042 and a pair of hands. Everything above this point is hardware decoding
 * (prefixes, break codes, modifiers); everything below is policy. */
void kbd_translate(uint8_t make, int pressed) {
    uint32_t base = g_layout->normal[make];
    if (!base) { if (pressed) { } return; }   /* unmapped: no event, no char */

    /* The event carries the UNSHIFTED key -- "which key", not "what character".
     * The char stream below answers the character question, and duplicating it
     * here would just be a second, worse answer. A dead key reports the mark it
     * carries (´ is EKC-less, so its codepoint is the honest answer to "which
     * key"), and a key above the BMP reports 0 rather than a truncated lie. */
    uint32_t evcp = KEY_BARE(base);
    uint16_t key = (uint16_t)(evcp <= 0xFFFF ? evcp : 0);
    uint8_t pad = make == 0x37 ? KBF_KEYPAD : 0;  /* keypad *; the rest arrive above */
    kbd_event_push(key, pressed);
    if (!pressed) return;                     /* releases produce no text */

    /* THREE LEVELS. AltGr is the right Alt key specifically -- tracked as a
     * SIDE, not as EKM_ALT, because Alt+f is a shortcut and AltGr+f is a
     * character, and a layout that cannot tell them apart cannot type € or @
     * on any European keyboard. */
    uint32_t cp;
    if ((g_side & SIDE_RALT) && g_layout->altgr) cp = g_layout->altgr[make];
    else if (g_mods & EKM_SHIFT)                 cp = g_layout->shift[make];
    else                                         cp = base;
    if (!cp) return;

    /* A DEAD KEY produces nothing yet: it waits for the next key and composes
     * with it (^ then e -> ê). Pressing it twice, or following it with a key it
     * cannot combine with, emits the mark itself and then the other character
     * -- which is what every desktop does and what makes ^ still typeable. */
    if (KEY_IS_DEAD(cp)) {
        uint32_t mark = KEY_BARE(cp);
        if (g_dead == mark) {                 /* the same mark twice: type it once */
            keyboard_deliver_cp(mark);
            g_dead = 0;
        } else {
            if (g_dead) keyboard_deliver_cp(g_dead);   /* a different one: flush it */
            g_dead = mark;
        }
        return;
    }
    if (g_dead) {
        uint32_t composed = kbd_compose(g_dead, cp);
        uint32_t mark = g_dead;
        g_dead = 0;
        if (composed) { deliver_key_cp(key, 0, composed); return; }
        keyboard_deliver_cp(mark);            /* no such pairing: mark, then key */
    }

    /* Caps Lock: LETTERS ONLY, and it XORs with Shift rather than adding to it
     * (Caps+Shift+a is 'a', not 'A'). Applying it to the whole shift table --
     * the obvious implementation -- would make Caps Lock type '!' for '1',
     * which no keyboard on earth does.
     *
     * Latin-1 letters case the same way, one block up: à<->À, é<->É, ù<->Ù. The
     * rule is +/-0x20 across U+00C0..U+00FE, with the two multiplication and
     * division signs punched out of the middle -- they sit inside a letter
     * range and are not letters. */
    if (g_mods & EKM_CAPS) {
        if      (cp >= 'a'    && cp <= 'z')    cp = cp - 'a' + 'A';
        else if (cp >= 'A'    && cp <= 'Z')    cp = cp - 'A' + 'a';
        else if (cp >= 0x00E0 && cp <= 0x00FE && cp != 0x00F7) cp -= 0x20;
        else if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) cp += 0x20;
    }

    /* Ctrl + letter -> the C0 control code (Ctrl-C = 0x03, Ctrl-D = 0x04, ...).
     * This is what actually MAKES ^C possible: without it the driver produced a
     * plain 'c' and keyboard_deliver()'s 0x03 branch could never fire. Gate on a
     * real letter so Ctrl+digit / Ctrl+symbol pass through unchanged rather than
     * becoming stray control bytes. */
    if ((g_mods & EKM_CTRL) && cp >= 'a' && cp <= 'z') cp = cp & 0x1f;
    else if ((g_mods & EKM_CTRL) && cp >= 'A' && cp <= 'Z') cp = cp & 0x1f;

    if (interface_key(key, base, cp)) return;

    deliver_key_cp(key, pad, cp);
}



#if defined(__x86_64__)
static void keyboard_handler(void) {
    static int prefix;
    uint8_t sc=inb(KBD_DATA_PORT);
    spin_lock(&input_state_lock);keyboard_scancode(sc,&prefix,1);spin_unlock(&input_state_lock);
}

/* --- 8042 controller ------------------------------------------------------
 * We used to rely on whatever the BIOS left behind. That worked, but it is a
 * bet: it assumes the device is in scancode set 2 AND that controller
 * translation is on (which is what makes set 2 arrive here looking like the
 * set 1 our tables index). Nothing verified either.
 *
 * THE CONFIG BYTE HAS ONE OWNER NOW: i8042.c. This file used to do its own
 * read-modify-write of it and so did mouse.c, each touching "only its own
 * bits" so the two would compose in either order. That reasoning was sound and
 * still cost a laptop its keyboard, because it skipped the step before the
 * read: emptying the output buffer. UEFI drives the keyboard until the moment
 * it hands over, and a scancode left in the buffer is what the read returns --
 * so the config byte written back is a keystroke. i8042_init() quiets both
 * ports, flushes, self-tests and tests each port before anything believes a
 * byte it read; this file asks it for the keyboard and gets a yes or a no.
 *
 * The old note said a self-test would "reset state the mouse depends on" and
 * that the mouse worked, so leave it. On QEMU it did. The machine that matters
 * is the one where it did not.
 *
 * 🪤 THE PAIRING TRAP: scancode set and translation are ONE decision, not two.
 * Set the device to set 1 while translation is on and the controller
 * translates set-1 bytes as if they were set-2 -- garbage, and a dead keyboard.
 * We pick the standard PC pairing (device in SET 2 + translation ON) and state
 * it, because it is what our set-1 tables require. */
/* Send a byte to the KEYBOARD (not the controller) and eat its ACK. The
 * retry-on-RESEND and the bounded wait live in i8042.c now, with the rest of
 * the controller: one owner, one config byte, one flush. */
static int kbd_cmd(uint8_t byte) { return i8042_kbd_write(byte) ? 1 : 0; }

/* The three lock LEDs. `0xED` then a bitmap: bit0 Scroll, bit1 Num, bit2 Caps.
 * Caps Lock without its LED is a latch the user cannot see -- the light IS the
 * feature's only feedback. */
static void kbd_set_leds(void) {
    uint8_t bits = 0;
    if (g_mods & EKM_SCROLL) bits |= 0x01;
    if (g_mods & EKM_NUM)    bits |= 0x02;
    if (g_mods & EKM_CAPS)   bits |= 0x04;
    if (kbd_cmd(0xED)) kbd_cmd(bits);
}

void keyboard_init(void) {
    serial_write_string("\n=== Keyboard init ===\n");
    g_mods = 0;                              /* locks start OFF, whatever feeds the keys */

    /* The controller first, and only once: it tests its own ports and says
     * what is on them. A machine with no 8042 -- which is a thing a laptop can
     * be -- stops here rather than configuring a chip that is not there. */
    if (!i8042_init() || !i8042_have_kbd()) {
        kprintf("Keyboard: no PS/2 keyboard on this machine; USB and "
                "virtio-input feed the same key handling\n");
        return;
    }

    irq_register(1, keyboard_handler);
    i8042_irq_enable(true, false);           /* IRQ1 on, handler now in place */

    /* Scancode set 2 EXPLICITLY -- the other half of the translation pairing.
     * Best-effort: QEMU and most real controllers ACK; one that does not is
     * almost certainly already in set 2 (that being the power-on default), and
     * refusing to boot over it would be worse than proceeding. Say so either
     * way rather than pretending we configured something we did not. */
    if (kbd_cmd(0xF0) && kbd_cmd(0x02))
        serial_write_string("Keyboard: scancode set 2 + translation (set 1 seen)\n");
    else
        serial_write_string("Keyboard: set-2 select not ACKed; assuming power-on default\n");

    kbd_cmd(0xF4);                           /* enable scanning */

    kbd_set_leds();                          /* and the LEDs agree with g_mods */

    serial_write_string("Keyboard registered on IRQ 1\n");
}

#else  /* !__x86_64__ ---------------------------------------------------------
 * No PS/2 controller, so no scancode decode, no IRQ1 and no LED command. The
 * POLICY above is untouched and fully live; it is fed by keyboard_inject_event()
 * instead of by an interrupt.
 *
 * The LEDs go out over virtio-input's STATUS queue instead of a PS/2 0xED
 * command -- same moment, same trigger, different wire. */
static void kbd_set_leds(void) {
    virtio_input_set_leds(g_mods);
}

void keyboard_init(void) {
    g_mods = 0;
}
#endif /* __x86_64__ */

/* The layout, the shift table, Caps Lock and Ctrl -- applied ONCE, here, so
 * every input source composes characters the same way.
 *
 * `make` is an AT SET-1 make code, which is also, and not by coincidence, a
 * Linux evdev keycode: evdev's numbering for the main key block was taken from
 * set 1, so KEY_ESC is 1, KEY_Q is 16 and KEY_A is 30 exactly as the scancodes
 * are. That is what lets virtio-input index the SAME layout tables instead of
 * carrying a second copy of them -- and what makes `keyboard_set_layout
 * dvorak` apply to a virtio keyboard without another line of code.
 *
 * Returns 0 for a key that produces no character in this layout.
 */
uint32_t keyboard_compose_cp(uint8_t make, uint8_t mods) {
    if (make >= 128 || !g_layout)
        return 0;

    /* The same three levels the PS/2 path uses, so a virtio keyboard types
     * exactly what a PS/2 one does on the same layout -- AltGr included. */
    uint32_t ascii;
    if ((g_side & SIDE_RALT) && g_layout->altgr) ascii = g_layout->altgr[make];
    else if (mods & EKM_SHIFT)                   ascii = g_layout->shift[make];
    else                                         ascii = g_layout->normal[make];
    if (!ascii)
        return 0;

    /* A dead key composes with the NEXT key here exactly as it does on the
     * PS/2 path -- same pending mark, because it is the same person typing on
     * one keyboard and the state cannot live in two places. */
    if (KEY_IS_DEAD(ascii)) {
        uint32_t mark = KEY_BARE(ascii);
        if (g_dead == mark) { g_dead = 0; return mark; }
        if (g_dead) { uint32_t flush = g_dead; g_dead = mark; return flush; }
        g_dead = mark;
        return 0;
    }
    if (g_dead) {
        uint32_t composed = kbd_compose(g_dead, ascii);
        uint32_t mark = g_dead;
        g_dead = 0;
        if (composed) return composed;
        keyboard_deliver_cp(mark);          /* no pairing: the mark, then the key */
    }

    /* Caps Lock: LETTERS ONLY, and it XORs with Shift rather than adding to it
     * (Caps+Shift+a is 'a', not 'A'). Applying it to the whole shift table --
     * the obvious implementation -- would make Caps Lock type '!' for '1',
     * which no keyboard on earth does. */
    if (mods & EKM_CAPS) {
        if      (ascii >= 'a'    && ascii <= 'z')    ascii = ascii - 'a' + 'A';
        else if (ascii >= 'A'    && ascii <= 'Z')    ascii = ascii - 'A' + 'a';
        else if (ascii >= 0x00E0 && ascii <= 0x00FE && ascii != 0x00F7) ascii -= 0x20;
        else if (ascii >= 0x00C0 && ascii <= 0x00DE && ascii != 0x00D7) ascii += 0x20;
    }

    /* Ctrl + letter -> the C0 control code. Gated on a real letter so
     * Ctrl+digit / Ctrl+symbol pass through unchanged rather than becoming
     * stray control bytes. */
    if ((mods & EKM_CTRL) && ((ascii >= 'a' && ascii <= 'z') ||
                              (ascii >= 'A' && ascii <= 'Z')))
        ascii = ascii & 0x1f;

    return ascii;
}

/* The unshifted key IDENTITY for an event code -- "which key", not "what
 * character". Always the base layout, never the shift table, so an event says
 * the same thing whether or not Shift was held. */
uint16_t keyboard_keycode_of(uint8_t make) {
    if (make >= 128 || !g_layout)
        return 0;
    /* The BARE codepoint: a dead key is still "which key", and its identity is
     * the mark it carries. Above the BMP there is no 16-bit answer, so say 0
     * rather than a truncated one -- the same choice kbd_translate makes. */
    uint32_t cp = KEY_BARE(g_layout->normal[make]);
    return (uint16_t)(cp <= 0xFFFF ? cp : 0);
}

/* ---- the injection seam --------------------------------------------------
 * ONE decoded key, from whatever hardware decoded it, into the two streams.
 *
 * This is what makes a second keyboard driver a translation table rather than a
 * second keyboard driver. virtio-input (arch/aarch64/drivers/virtio_input.c)
 * turns Linux evdev codes into EKC_* / ASCII and calls this; the PS/2 handler
 * above reaches the same rings by the same rules, and USB HID's
 * keyboard_inject_char() is the char-only special case of it.
 *
 * `ascii` is 0 for a key with no character (F5, the arrows already carry their
 * EK_* through it, a modifier). It is delivered only on the MAKE: a key
 * RELEASE has never produced a character on any of these paths, and emitting
 * one would double every keystroke. */
static void inject_event_inner(uint16_t code, int pressed, char ascii) {
    switch (code) {
    case EKC_LSHIFT: mod_update(SIDE_LSHIFT, SIDE_LSHIFT | SIDE_RSHIFT,
                                EKM_SHIFT, code, pressed); return;
    case EKC_RSHIFT: mod_update(SIDE_RSHIFT, SIDE_LSHIFT | SIDE_RSHIFT,
                                EKM_SHIFT, code, pressed); return;
    case EKC_LCTRL:  mod_update(SIDE_LCTRL,  SIDE_LCTRL  | SIDE_RCTRL,
                                EKM_CTRL,  code, pressed); return;
    case EKC_RCTRL:  mod_update(SIDE_RCTRL,  SIDE_LCTRL  | SIDE_RCTRL,
                                EKM_CTRL,  code, pressed); return;
    case EKC_LALT:   mod_update(SIDE_LALT,   SIDE_LALT   | SIDE_RALT,
                                EKM_ALT,   code, pressed); return;
    case EKC_RALT:   mod_update(SIDE_RALT,   SIDE_LALT   | SIDE_RALT,
                                EKM_ALT,   code, pressed); return;
    case EKC_LWIN:   mod_update(SIDE_LGUI,   SIDE_LGUI   | SIDE_RGUI,
                                EKM_GUI,   code, pressed); return;
    case EKC_RWIN:   mod_update(SIDE_RGUI,   SIDE_LGUI   | SIDE_RGUI,
                                EKM_GUI,   code, pressed); return;
    case EKC_CAPS:   lock_toggle(EKM_CAPS,   code, pressed,0); return;
    case EKC_NUM:    lock_toggle(EKM_NUM,    code, pressed,0); return;
    case EKC_SCROLL: lock_toggle(EKM_SCROLL, code, pressed,0); return;
    default: break;
    }

    kbd_event_push(code, pressed);
    if (pressed && ascii)
        deliver_key(code, 0, ascii);
    else if (pressed && ((code >= EKC_F1 && code <= EKC_F12) || code == EKC_INS || code == EKC_MENU))
        keyboard_deliver_key_only(code, 0);
}
void keyboard_inject_event(uint16_t code,int pressed,char ascii) {
    spin_lock(&input_state_lock);inject_event_inner(code,pressed,ascii);spin_unlock(&input_state_lock);
}

/* --- AZERTY (French) ------------------------------------------------------
 *
 * THE LAYOUT THIS DRIVER USED TO REFUSE TO SHIP. Its comment above the Dvorak
 * table said, correctly, that an AZERTY in a 7-bit char stream "could only be
 * QWERTY-with-letters-moved and silent holes where the accents belong -- a
 * worse lie than saying no". The stream is UTF-8 now, so here is the real one.
 *
 * What makes it French rather than QWERTY rearranged:
 *   * é è ç à ù are on the NUMBER ROW, unshifted, where a French keyboard puts
 *     them -- they are not dead-key compositions and never were;
 *   * the digits are the SHIFTED level of that row (French types 1 with Shift);
 *   * ^ and ¨ live on one key as DEAD keys, which is how â ê î ô û and ë ï ü
 *     are typed;
 *   * AltGr is a real third level: @ # { } [ ] | \ € ~ are only reachable
 *     there, and nothing else on the keyboard can produce them.
 *
 * Set-1 make codes, same indexing as every other table here. */
static const uint32_t scan_to_cp_azerty[128] = {
    /* THE NUMBER ROW IS THE LAYOUT. Set-1 make 0x02 is the key labelled 1 on a
     * US board, and on a French one that key is '&' -- the accented letters sit
     * along this row and the digits are its shifted level. Getting this row
     * off by one make code (which the first draft did) puts é where & belongs
     * and every test of it disagrees by exactly one key. */
    0,   0x1B, 0x0026,0x00E9,0x0022,0x0027,0x0028,0x002D,0x00E8,0x005F,  // 0x00-0x09  & é " ' ( - è _
    0x00E7,0x00E0,0x0029,0x003D,'\b','\t', 'a', 'z', 'e', 'r',            // 0x0A-0x13  ç à ) =
    't', 'y', 'u', 'i', 'o', 'p', KEY_DEAD(0x005E), 0x0024, '\n', 0,     // 0x14-0x1D  ^dead $
    'q', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 'm',                    // 0x1E-0x27
    0x00F9,0x00B2, 0,  '*', 'w', 'x', 'c', 'v', 'b', 'n',                // 0x28-0x31  ù ²
    0x002C,0x003B,0x003A,0x0021, 0, '*', 0,  ' ', 0,   0,                // 0x32-0x3B  , ; : !
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x3C-0x45
    0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,                      // 0x46-0x4F
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x50-0x59
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x5A-0x63
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x64-0x6D
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x6E-0x77
    0,   0,   0,   0,   0,   0,   0,   0,                                // 0x78-0x7F
};
static const uint32_t scan_to_cp_azerty_shift[128] = {
    0,   0x1B, '1', '2', '3', '4', '5', '6', '7', '8',                   // 0x00-0x09  the DIGITS
    '9', '0', 0x00B0,'+', '\b','\t', 'A', 'Z', 'E', 'R',                  // 0x0A-0x13  ° +
    'T', 'Y', 'U', 'I', 'O', 'P', KEY_DEAD(0x00A8), 0x00A3, '\n', 0,     // 0x14-0x1D  ¨dead £
    'Q', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'M',                    // 0x1E-0x27
    '%', 0,   0,  0x00B5,'W', 'X', 'C', 'V', 'B', 'N',                   // 0x28-0x31  % µ
    '?', '.', '/', 0x00A7, 0, '*', 0,  ' ', 0,   0,                      // 0x32-0x3B  ? . / §
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x3C-0x45
    0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,                      // 0x46-0x4F
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x50-0x59
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x5A-0x63
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x64-0x6D
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x6E-0x77
    0,   0,   0,   0,   0,   0,   0,   0,                                // 0x78-0x7F
};
/* The third level. Everything a programmer needs that the first two levels of a
 * French keyboard do not have: without this row you cannot type @ in an email
 * address, { } in C, [ ] in an array, | in a shell pipeline, or \\ anywhere. */
static const uint32_t scan_to_cp_azerty_altgr[128] = {
    0,   0,   0,   '~', '#', '{', '[', '|', 0x0060,'\\',                  // 0x00-0x09  ~ # { [ | ` and backslash
    '^', '@', ']', '}', 0,   0,   0,   0,   0x20AC,0,                    // 0x0A-0x13  ^ @ ] }  € on E
    0,   0,   0,   0,   0,   0,   0,   0x00A4,0,   0,                    // 0x14-0x1D  ¤
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x1E-0x27
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x28-0x31
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x32-0x3B
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x3C-0x45
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x46-0x4F
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x50-0x59
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x5A-0x63
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x64-0x6D
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,                      // 0x6E-0x77
    0,   0,   0,   0,   0,   0,   0,   0,                                // 0x78-0x7F
};

/* --- Unicode: composition and UTF-8 ---------------------------------------
 *
 * WHAT THE CHAR STREAM CARRIES, AND WHY IT CHANGED. It used to be 7-bit ASCII,
 * one byte per key, and that made French unshippable: this driver's own
 * comment said so, and refused to offer AZERTY rather than ship a layout with
 * silent holes where é è ç à ù belong. The stream is UTF-8 now. ASCII is
 * byte-for-byte what it always was -- every existing reader (the tty, the
 * shell, embk_key_poll, ui_input_char) keeps working unchanged -- and anything
 * above 0x7F arrives as the 2-4 bytes the renderer already knows how to draw
 * (ui/backend/font.c's font_utf8_decode has decoded UTF-8 all along; it was
 * only the INPUT half of the OS that could not spell). */

/* The pairings a dead key can make. Deliberately the Latin-1 set and no more:
 * every one of these is a character the shipped font can draw and a French,
 * Spanish or German keyboard can reach. An entry that cannot be rendered would
 * be a hole of exactly the kind this whole change exists to remove. */
static const struct { uint16_t mark, base, out; } g_compose[] = {
    /* acute */
    { 0x00B4, 'a', 0x00E1 }, { 0x00B4, 'e', 0x00E9 }, { 0x00B4, 'i', 0x00ED },
    { 0x00B4, 'o', 0x00F3 }, { 0x00B4, 'u', 0x00FA }, { 0x00B4, 'y', 0x00FD },
    { 0x00B4, 'A', 0x00C1 }, { 0x00B4, 'E', 0x00C9 }, { 0x00B4, 'I', 0x00CD },
    { 0x00B4, 'O', 0x00D3 }, { 0x00B4, 'U', 0x00DA },
    /* grave */
    { 0x0060, 'a', 0x00E0 }, { 0x0060, 'e', 0x00E8 }, { 0x0060, 'i', 0x00EC },
    { 0x0060, 'o', 0x00F2 }, { 0x0060, 'u', 0x00F9 },
    { 0x0060, 'A', 0x00C0 }, { 0x0060, 'E', 0x00C8 }, { 0x0060, 'I', 0x00CC },
    { 0x0060, 'O', 0x00D2 }, { 0x0060, 'U', 0x00D9 },
    /* circumflex -- the one AZERTY puts on its own key */
    { 0x005E, 'a', 0x00E2 }, { 0x005E, 'e', 0x00EA }, { 0x005E, 'i', 0x00EE },
    { 0x005E, 'o', 0x00F4 }, { 0x005E, 'u', 0x00FB },
    { 0x005E, 'A', 0x00C2 }, { 0x005E, 'E', 0x00CA }, { 0x005E, 'I', 0x00CE },
    { 0x005E, 'O', 0x00D4 }, { 0x005E, 'U', 0x00DB },
    /* diaeresis -- AZERTY's shifted circumflex */
    { 0x00A8, 'a', 0x00E4 }, { 0x00A8, 'e', 0x00EB }, { 0x00A8, 'i', 0x00EF },
    { 0x00A8, 'o', 0x00F6 }, { 0x00A8, 'u', 0x00FC }, { 0x00A8, 'y', 0x00FF },
    { 0x00A8, 'A', 0x00C4 }, { 0x00A8, 'E', 0x00CB }, { 0x00A8, 'I', 0x00CF },
    { 0x00A8, 'O', 0x00D6 }, { 0x00A8, 'U', 0x00DC },
    /* tilde */
    { 0x007E, 'n', 0x00F1 }, { 0x007E, 'a', 0x00E3 }, { 0x007E, 'o', 0x00F5 },
    { 0x007E, 'N', 0x00D1 }, { 0x007E, 'A', 0x00C3 }, { 0x007E, 'O', 0x00D5 },
    /* cedilla */
    { 0x00B8, 'c', 0x00E7 }, { 0x00B8, 'C', 0x00C7 },
};

static uint32_t kbd_compose(uint32_t mark, uint32_t base) {
    for (unsigned i = 0; i < sizeof g_compose / sizeof g_compose[0]; i++)
        if (g_compose[i].mark == mark && g_compose[i].base == base)
            return g_compose[i].out;
    return 0;                                  /* no such pairing */
}

/* One codepoint -> UTF-8 -> the byte ring the whole system already reads. */
static void keyboard_deliver_cp(uint32_t cp) {
    if (cp < 0x80) { keyboard_deliver((char)cp); return; }
    if (cp < 0x800) {
        keyboard_deliver((char)(0xC0 | (cp >> 6)));
        keyboard_deliver((char)(0x80 | (cp & 0x3F)));
        return;
    }
    if (cp < 0x10000) {
        keyboard_deliver((char)(0xE0 | (cp >> 12)));
        keyboard_deliver((char)(0x80 | ((cp >> 6) & 0x3F)));
        keyboard_deliver((char)(0x80 | (cp & 0x3F)));
        return;
    }
    keyboard_deliver((char)(0xF0 | (cp >> 18)));
    keyboard_deliver((char)(0x80 | ((cp >> 12) & 0x3F)));
    keyboard_deliver((char)(0x80 | ((cp >> 6) & 0x3F)));
    keyboard_deliver((char)(0x80 | (cp & 0x3F)));
}

/* Force modifier state, FOR THE KEYMAP SELFTEST ONLY. A test that cannot hold
 * Shift or AltGr can only check a third of any layout -- and the third level is
 * exactly where the characters a French keyboard needs for code live. */
void kbd_mods_force(uint8_t mods)  { g_mods = mods; }
void kbd_altgr_force(int on)       { if (on) g_side |= SIDE_RALT; else g_side &= (uint8_t)~SIDE_RALT; }

/* A key from a keyboard that is not the PS/2 one. Same event ring, same char
 * stream, same UTF-8 -- so a virtio keyboard and a PS/2 keyboard on the same
 * layout produce identical bytes, which is the only way `keyboard_set_layout`
 * can mean one thing on a machine that has both. */
void keyboard_inject_cp(uint16_t code, int pressed, uint32_t cp) {
    spin_lock(&input_state_lock);
    if (cp) kbd_event_push(code, pressed);      /* the event half */
    else inject_event_inner(code, pressed, 0);  /* (a modifier, or a key with no text) */
    /* the interface's keys first, as on PS/2; a printable key's code is its
     * unshifted character (keyboard.h), which is the `base` they are read by */
    if (pressed && cp && !interface_key(code, code < 0x80 ? code : 0, cp))
        deliver_key_cp(code, 0, cp);            /* the text half, as UTF-8 */
    spin_unlock(&input_state_lock);
}

/* --- layouts -------------------------------------------------------------- */
static const struct keymap g_layouts[] = {
    { "us",     scan_to_cp_us,        scan_to_cp_shift,        NULL },
    { "dvorak", scan_to_cp_dvorak, scan_to_cp_dvorak_shift, NULL },
    { "azerty", scan_to_cp_azerty,    scan_to_cp_azerty_shift,    scan_to_cp_azerty_altgr },
};
static const struct keymap *g_layout = &g_layouts[0];

int keyboard_set_layout(const char *name) {
    for (unsigned i = 0; i < sizeof g_layouts / sizeof g_layouts[0]; i++) {
        const char *a = g_layouts[i].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) { g_layout = &g_layouts[i]; return 0; }
    }
    return -1;      /* unknown: keep the current layout, and SAY so (see the header) */
}
const char *keyboard_layout(void) { return g_layout->name; }

uint8_t keyboard_mods(void) { return g_mods; }

int keyboard_event_pop(struct key_event *ev) {
    if (ev_head == ev_tail) return 0;
    *ev = ev_ring[ev_tail];
    ev_tail = (ev_tail + 1) % KBD_EVENT_RING;
    return 1;
}


char keyboard_getchar(void) {
    char c;
    // spin until a character is available in the buffer
    while (!buffer_pop(&c)) {
        /* Idle until the next interrupt. Through the HAL rather than a bare
         * `hlt`: the instruction is x86's spelling (aarch64's is `wfi`), and
         * this file is compiled for both now. docs/TODO.md asked for exactly
         * this conversion, "as part of making its file build". */
        arch_cpu_idle();
    }
    
    return c;
}


uint32_t keyboard_poll_input(void) {
    char c;uint32_t value=0;
    sched_lock();
    drop_key_only();
    if(buf_head!=buf_tail) {
        uint8_t mods=kbd_buffer_mods[buf_tail];
        if(buffer_pop(&c))value=(uint8_t)c|((uint32_t)mods<<8);
    }
    sched_unlock();return value;
}
int keyboard_input_snapshot_selftest(void) {
    /* Preserve live input and modifier state; do not inject test keys into a
     * desktop. Match the producer's input-state -> scheduler lock order. */
    spin_lock(&input_state_lock);sched_lock();
    char bytes[KBD_BUFFER_SIZE];uint8_t mods[KBD_BUFFER_SIZE];
    memcpy(bytes,kbd_buffer,sizeof bytes);memcpy(mods,kbd_buffer_mods,sizeof mods);
    uint32_t head=buf_head,tail=buf_tail;uint8_t held=g_mods;
    buf_head=buf_tail=0;g_mods=EKM_CTRL;buffer_push(0x13);
    g_mods=0;buffer_push(EK_UP);
    int ok=kbd_buffer_mods[0]==EKM_CTRL && kbd_buffer_mods[1]==0;
    char c;ok=ok && buffer_pop(&c) && c==0x13 && buf_tail==1;
    ok=ok && buffer_pop(&c) && c==EK_UP && buf_head==buf_tail;
    memcpy(kbd_buffer,bytes,sizeof bytes);memcpy(kbd_buffer_mods,mods,sizeof mods);
    buf_head=head;buf_tail=tail;g_mods=held;
    sched_unlock();spin_unlock(&input_state_lock);return ok;
}
/* `test kbd ring`: the key behind each byte survives the queue, keys with no
 * character queue in order with the text, and character readers still see
 * exactly the bytes they always saw. Live input, both rings and the modifier
 * state are saved first and put back after; nothing reaches a window. */
int keyboard_key_ring_selftest(void) {
    static char bytes[KBD_BUFFER_SIZE]; static uint8_t mods[KBD_BUFFER_SIZE], flags[KBD_BUFFER_SIZE];
    static uint16_t codes[KBD_BUFFER_SIZE]; static struct key_event events[KBD_EVENT_RING];
    g_selftest_quiet = 1;
    spin_lock(&input_state_lock);sched_lock();
    memcpy(bytes,kbd_buffer,sizeof bytes);memcpy(mods,kbd_buffer_mods,sizeof mods);
    memcpy(codes,kbd_buffer_code,sizeof codes);memcpy(flags,kbd_buffer_flags,sizeof flags);
    memcpy(events,ev_ring,sizeof events);
    uint32_t head=buf_head,tail=buf_tail,eh=ev_head,et=ev_tail;uint8_t held=g_mods,side=g_side;
    sched_unlock();
    static const struct { uint8_t byte, mods; uint16_t code; uint8_t flags; } want[] = {
        { 0x13, EKM_CTRL, 's', 0 },             /* Ctrl+S */
        { 0x13, EKM_CTRL, EKC_UP, 0 },          /* Ctrl+Up: the same byte, a different key */
        { 0x13, 0, EKC_UP, 0 },                 /* Up */
        { 0, 0, EKC_F1 + 4, KBF_KEYONLY },      /* F5: no byte, in order */
        { 0xC3, 0, 'e', 0 }, { 0xA9, 0, 0, 0 }, /* e-acute: two bytes, one key */
        { 0, 0, EKC_INS, KBF_KEYONLY },         /* Insert */
        { 0x08, 0, '\b', 0 },                   /* Backspace, not Ctrl+H */
        { 0x08, EKM_CTRL, 'h', 0 },             /* Ctrl+H */
    };
    int ok=1;
    for (int pass=0;pass<2;pass++) {
        buf_head=buf_tail=0;
        g_mods=EKM_CTRL;g_side=SIDE_LCTRL;inject_event_inner('s',1,0x13);inject_event_inner(EKC_UP,1,EK_UP);
        g_mods=0;g_side=0;inject_event_inner(EKC_UP,1,EK_UP);inject_event_inner(EKC_F1+4,1,0);
        deliver_key_cp('e',0,0xE9);inject_event_inner(EKC_INS,1,0);inject_event_inner('\b',1,'\b');
        g_mods=EKM_CTRL;g_side=SIDE_LCTRL;inject_event_inner('h',1,0x08);g_mods=0;g_side=0;
        for (unsigned i=0;i<sizeof want/sizeof want[0];i++) {
            if (pass==0) {
                uint64_t k=keyboard_poll_key();
                ok=ok && (k>>40&1) && (uint8_t)k==want[i].byte && (uint8_t)(k>>8)==want[i].mods &&
                   (uint16_t)(k>>16)==want[i].code && (uint8_t)(k>>32)==want[i].flags;
            } else if (!(want[i].flags&KBF_KEYONLY)) {
                uint32_t v=keyboard_poll_input();       /* the legacy reader: bytes only */
                ok=ok && v==(want[i].byte|(uint32_t)want[i].mods<<8);
            }
        }
        ok=ok && (pass ? keyboard_poll_input()==0 : keyboard_poll_key()==0);
    }
    buf_head=buf_tail=0;inject_event_inner(EKC_F1,1,0);
    ok=ok && !keyboard_has_char();                    /* a lone F1 is not a character */
    /* A USB boot keyboard's reports, decoded like every other keyboard: its
     * Ctrl is real, Up is Up, Shift shifts, F5 arrives, and a report with the
     * keys gone releases them -- modifiers included. Layout-independent keys
     * only (the live layout is the user's): s/a are the same key on us, azerty. */
    static const uint8_t reports[][8] = {
        { 0x01, 0, 0x16 }, { 0 },                     /* LCtrl+s: 0x13 */
        { 0, 0, 0x52 }, { 0 },                        /* Up: 0x13, from Up */
        { 0x02, 0, 0x04 }, { 0x02 }, { 0 },           /* LShift+a: A, Shift let go after */
        { 0, 0, 0x3E }, { 0 },                        /* F5: no byte */
    };
    static const struct { uint8_t byte, mods; uint16_t code; uint8_t flags; } usb[] = {
        { 0x13, EKM_CTRL, 's', 0 }, { 0x13, 0, EKC_UP, 0 }, { 'A', EKM_SHIFT, 'a', 0 }, { 0, 0, EKC_F1 + 4, KBF_KEYONLY },
    };
    uint8_t prev[8] = {0};const char *saved_layout = keyboard_layout();
    keyboard_set_layout("us");buf_head=buf_tail=0;
    for (unsigned i=0;i<sizeof reports/sizeof reports[0];i++) hid_boot_report_inner(prev, reports[i], 8, 1);
    for (unsigned i=0;i<sizeof usb/sizeof usb[0];i++) {
        uint64_t k=keyboard_poll_key();
        ok=ok && (uint8_t)k==usb[i].byte && (uint8_t)(k>>8)==usb[i].mods &&
           (uint16_t)(k>>16)==usb[i].code && (uint8_t)(k>>32)==usb[i].flags;
    }
    ok=ok && keyboard_poll_key()==0 && !(g_mods&(EKM_CTRL|EKM_SHIFT));
    keyboard_set_layout(saved_layout);
    sched_lock();
    memcpy(kbd_buffer,bytes,sizeof bytes);memcpy(kbd_buffer_mods,mods,sizeof mods);
    memcpy(kbd_buffer_code,codes,sizeof codes);memcpy(kbd_buffer_flags,flags,sizeof flags);
    memcpy(ev_ring,events,sizeof events);
    buf_head=head;buf_tail=tail;ev_head=eh;ev_tail=et;g_mods=held;g_side=side;
    sched_unlock();spin_unlock(&input_state_lock);
    g_selftest_quiet = 0;
    return ok;
}

int keyboard_has_char(void) {
    drop_key_only();
    return buf_head != buf_tail;
}

uint64_t keyboard_poll_key(void) {
    uint64_t value = 0;
    sched_lock();
    if (buf_head != buf_tail) {
        uint32_t t = buf_tail;
        value = (uint64_t)(uint8_t)kbd_buffer[t] | (uint64_t)kbd_buffer_mods[t] << 8 |
                (uint64_t)kbd_buffer_code[t] << 16 | (uint64_t)kbd_buffer_flags[t] << 32 | 1ull << 40;
        buf_tail = (t + 1) % KBD_BUFFER_SIZE;
    }
    sched_unlock();
    return value;
}

/* A key with no character, queued in order with the text and woken like it. */
static void keyboard_deliver_key_only(uint16_t code, uint8_t flags) {
    INPUT_ACTIVITY();
    sched_lock();
    g_key_code = code; g_key_flags = 0;
    buffer_push_entry(0, (uint8_t)(KBF_KEYONLY | flags));
    wait_queue_wake_one(&kbd_wait_queue);
    sched_unlock();
    if (!g_selftest_quiet) ui_event_post();   /* an application waiting for input (ui_events.h) */
}

/* Every push site goes through here. The lock makes {push -> wake} atomic
 * against a reader's {check-empty -> block}, closing the lost-wakeup window.
 * Safe from IRQ context because of the kernel's standing invariant:
 * g_sched_lock is only ever held with interrupts OFF, so an IRQ physically
 * cannot land while a thread holds it -- there is no self-deadlock path.
 * The critical section is deliberately tiny (one ring push + one state
 * flip); the wake only marks the thread READY, it does not switch to it. */
/* Where ^C goes. See docs/INTERRUPTION.md.
 *
 * ONE slot, because there is ONE console. This is NOT an inferred "foreground
 * process": no session, no process group, no walking a tree. A process that
 * holds the console DELEGATES its interrupts to a child it holds a handle for
 * (sys_console_interrupt_route), exactly as it delegates fds via file-actions
 * and the environment via envp. 0 = nobody routed anything, and then ^C is just
 * a byte -- a true and predictable statement about a system where no one asked
 * to be interrupted.
 *
 * Single-user concession, stated plainly: the slot is global and last-writer-
 * wins, the same class of concession as embk_proc_kill's ambient authority. */
static volatile uint32_t g_console_int_target;   /* pid, or 0 for none */

/* WHO DELEGATED, so ^Z has somewhere to report back to.
 *
 * ^C needs only a target: interrupt it, and the shell finds out because its
 * wait returns when the child dies or gives up. ^Z is different -- the child
 * does not die, it FREEZES, and a parent blocked waiting for it would wait
 * forever for an exit that is never coming. So the router is remembered too,
 * and suspending wakes it.
 *
 * Recorded at the same moment as the target and by the same call, so the two
 * cannot disagree about who routed what. */
static volatile uint32_t g_console_int_router;   /* pid that routed, or 0 */

void keyboard_set_interrupt_target(uint32_t pid) { g_console_int_target = pid; }
void keyboard_set_interrupt_router(uint32_t pid) { g_console_int_router = pid; }
uint32_t keyboard_get_interrupt_target(void) { return g_console_int_target; }

static void keyboard_deliver(char c) {
    INPUT_ACTIVITY();
    /* ^C: cancel the routed target instead of delivering a byte.
     *
     * DELIBERATELY BEFORE sched_lock(): process_cancel() takes g_sched_lock
     * itself, and this runs in IRQ context -- taking it twice would self-
     * deadlock. Safe to take it here at all only because of the standing
     * invariant (see below): g_sched_lock is never held with interrupts on, so
     * an IRQ cannot land while some thread holds it, and it is free right now. */
    if (c == 0x03) {
        uint32_t target = g_console_int_target;
        if (target) {
            /* An interrupt FIRST, if the target asked to catch them. That is
             * the difference between "stop what you are doing" and "stop
             * permanently": a shell running a script wants the first and
             * cannot survive the second, because cancellation is sticky by
             * design. A target that has not opted in is cancelled exactly as
             * before, so every routed child behaves as it always did. */
            if (process_raise_interrupt(target) == 1)
                return;
            process_cancel(target);
            return;         /* consumed: ^C is an interruption, not input */
        }
        /* Nobody routed: fall through and hand ^C over as an ordinary byte
         * rather than silently swallowing a keystroke. */
    }

    /* ^Z: STOP the routed target rather than delivering a byte.
     *
     * The other half of "stop what you are doing". ^C says do not finish this;
     * ^Z says finish it later. It needs the kernel to be able to freeze a
     * process rather than only interrupt or cancel one -- which it has always
     * been able to do (process_suspend, built for the debugger) and has never
     * had a way to ask for from a keyboard.
     *
     * WAKING THE ROUTER IS NOT A COURTESY, IT IS THE WHOLE MECHANISM. The
     * child does not exit, so the shell's process_wait() would block forever
     * on an exit that is not coming. process_wake_child_waiters() makes it
     * re-check, and process_wait() now answers -EMBK_ESTOPPED, which is how a
     * shell gets its prompt back and can say "Stopped".
     *
     * Same lock reasoning as ^C above: both calls take g_sched_lock
     * themselves and this runs in IRQ context, so neither may be made with it
     * already held -- and by the standing invariant it never is. */
    if (c == 0x1A) {
        uint32_t target = g_console_int_target;
        uint32_t router = g_console_int_router;
        /* Suspending the SHELL ITSELF is not a job control operation, it is a
         * way to wedge the console with one keystroke. A session owner that
         * routed ^C at its own prompt (handle 0) is refused here and ^Z falls
         * through as an ordinary byte. */
        if (target && target != router) {
            if (process_suspend(target) > 0) {
                if (router) process_wake_child_waiters(router);
                return;     /* consumed */
            }
        }
        /* Nobody routed, or the target could not be frozen (every thread of it
         * is pinned): hand ^Z over as a byte rather than pretending. */
    }

    sched_lock();
    buffer_push(c);   /* the kernel shell will read it */
    wait_queue_wake_one(&kbd_wait_queue);
    sched_unlock();
    if (!g_selftest_quiet) ui_event_post();   /* an application waiting for input (ui_events.h) */
}

/* --- keyboard grab -------------------------------------------------------
 * A ring-3 UI app (e.g. uidemo) grabs the keyboard so the kernel shell stops
 * draining it; both otherwise poll the same buffer and split keystrokes. The
 * grab is auto-released when the grabbing process is reaped (see process.c). */
static volatile int      g_kbd_grabbed;
static volatile uint32_t g_kbd_grabber_pid;

void keyboard_set_grab(int grab, uint32_t pid) {
    g_kbd_grabbed = grab ? 1 : 0;
    g_kbd_grabber_pid = grab ? pid : 0;
}
int keyboard_is_grabbed(void) { return g_kbd_grabbed; }
void keyboard_release_grab_pid(uint32_t pid) {
    if (g_kbd_grabbed && g_kbd_grabber_pid == pid) { g_kbd_grabbed = 0; g_kbd_grabber_pid = 0; }
}

/* Blocking read for FD_BACKING_CONSOLE (fd 0). Deliberately NOT
 * keyboard_getchar() -- that one hlt-spins the CPU without ever yielding,
 * which is correct for the kernel shell (pre-scheduler, nothing else to run)
 * but would wedge us here: a shell blocked on read(0) would halt the core,
 * so the child it just spawned would never get scheduled to run. */
char keyboard_getchar_blocking(void) {
    char c;
    sched_lock();
    while (drop_key_only(), buf_head == buf_tail) {            /* re-check UNDER the lock */
        sched_block_current_locked(&kbd_wait_queue);           /* block until a key is pushed */
        sched_lock();                                          /* re-acquire the lock to re-check the condition */
    }
    buffer_pop(&c);                                          /* still locked -> atomic with the check */
    sched_unlock();

    return c;
}

/* Like keyboard_getchar_blocking(), but honours cancellation (docs/INTERRUPTION.md).
 * Returns EMBK_OK with *out set, or -EMBK_ECANCELED if this process was cancelled
 * while (or before) waiting. The plain version stays for callers with no process
 * context -- the kernel shell -- where cancellation has no meaning.
 *
 * Ordering matches the pipe path: a buffered key is a real result and is
 * returned even if the process is also cancelled; only BLOCKING is refused. */
int keyboard_getchar_blocking_cancelable(char *out) {
    sched_lock();
    while (drop_key_only(), buf_head == buf_tail) {
        if (current_process && current_process->cancelled) {
            sched_unlock();
            return -EMBK_ECANCELED;
        }
        sched_block_current_locked(&kbd_wait_queue);
        sched_lock();
    }
    buffer_pop(out);
    sched_unlock();
    return EMBK_OK;
}

// Inject a character as if it came from a keyboard press.
// Called by the USB HID driver for keys received over xHCI.
void keyboard_inject_char(char c) {
    spin_lock(&input_state_lock);keyboard_deliver(c);spin_unlock(&input_state_lock);
}
void keyboard_flush_input(void) {
    spin_lock(&input_state_lock);sched_lock();
    buf_head=buf_tail=ev_head=ev_tail=0;
    sched_unlock();spin_unlock(&input_state_lock);
}
