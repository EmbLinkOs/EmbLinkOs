#ifndef __KEYBOARD__H__
#define __KEYBOARD__H__

#include <stdint.h>

/* ---- the non-ASCII key codes this OS delivers ---------------------------
 * Navigation keys reach userspace as PRIVATE single-byte control codes, not
 * ANSI escape sequences -- so a reader is a byte compare, never an escape
 * state machine (the shell's Up/Down history recall and the terminal's
 * PgUp/PgDn scrollback both rely on that).
 *
 * A THREE-WAY CONTRACT, kept here so all of it shares ONE definition:
 *   - the PS/2 driver   (keyboard.c, extended 0xE0 scancodes)
 *   - the USB HID driver (usb/xhci.c, HID usages 0x4A-0x52)
 *   - userland          (EMBK_KEY_* in user/lib/embk.h -- mirror any change)
 * These were file-local to keyboard.c, which is exactly why the USB path
 * silently had NO navigation keys at all: xhci.c couldn't name them. */
#define EK_HOME  0x02
#define EK_END   0x05
#define EK_PGUP  0x0E   /* NOT 0x03/0x04: those stay free for a future Ctrl-C/D */
#define EK_PGDN  0x0F
#define EK_LEFT  0x11
#define EK_RIGHT 0x12
#define EK_UP    0x13
#define EK_DOWN  0x14
#define EK_DEL   0x7F

/* ---- the EDITING commands -----------------------------------------------
 *
 * The other half of a text field: selecting, and the clipboard. These are not
 * characters and there was nowhere to put them -- C0 is Ctrl+letter's and the
 * navigation codes' -- so they go ABOVE the character range instead. That is
 * not an arbitrary free corner: THIRTEEN byte values can never occur in valid
 * UTF-8 at any position -- 0xC0, 0xC1 and 0xF5..0xFF -- because the lead bytes
 * stop at 0xF4 (U+10FFFF encodes as F4 8F BF BF) and 0xC0/0xC1 would be
 * overlong forms of ASCII. Carrying commands there is safe in a stream that is
 * otherwise text.
 *
 * The first eight commands took 0xF8..0xFF and this comment then said "eight
 * values, eight commands, which is either elegant or a warning". It was the
 * warning: undo and redo arrived and needed two more. They are 0xF6/0xF7, and
 * 0xF5, 0xC0 and 0xC1 remain.
 *
 * IN THE SAME STREAM ON PURPOSE. A separate command channel would lose the
 * ORDER: a paste between two typed characters has to land between them, and
 * two queues cannot promise that. One queue can, and does, for free.
 *
 * WHY THE GUI KEY AND NOT CTRL, which is the question that decided all of
 * this. Ctrl+C cannot be copy in this operating system: 0x03 is intercepted in
 * keyboard_deliver() and routed at the console's interrupt target, so in a text
 * field it would cancel whatever the terminal is running rather than reach the
 * field. That is CORRECT -- ^C is an interruption here (docs/INTERRUPTION.md)
 * and must stay one. So the rule this OS adopts is a division of the two
 * modifiers: CTRL BELONGS TO THE TERMINAL -- interrupt, job control, the C0
 * codes -- AND THE GUI KEY BELONGS TO THE INTERFACE. It is the same separation
 * macOS makes between Ctrl and Cmd, and for the same reason.
 *
 * The driver decides these, not userspace, because the driver is where the
 * modifier state is known at the instant the key went down -- exactly as it
 * already decides that Ctrl+letter is a C0 code. Mirrored by EMBK_KEY_* in
 * user/lib/embk.h and UI_KEY_* in ui/kit/kit.h; all three must agree. */
#define EK_SEL_LEFT  0xF8   /* Shift+Left   */
#define EK_SEL_RIGHT 0xF9   /* Shift+Right  */
#define EK_SEL_HOME  0xFA   /* Shift+Home   */
#define EK_SEL_END   0xFB   /* Shift+End    */
#define EK_SEL_ALL   0xFC   /* GUI+A        */
#define EK_COPY      0xFD   /* GUI+C        */
#define EK_CUT       0xFE   /* GUI+X        */
#define EK_PASTE     0xFF   /* GUI+V        */
#define EK_UNDO      0xF6   /* GUI+Z        */
#define EK_REDO      0xF7   /* GUI+Shift+Z, and GUI+Y */

/* ---- SYSTEM shortcuts ----------------------------------------------------
 *
 * These never reach an application, and that is the whole point of them. GUI+
 * Tab switches windows and GUI+W closes one: an app that could see them could
 * also decline them, and a machine where the window switcher works only in the
 * applications that bothered to implement it is a machine with no window
 * switcher. Same reasoning as ^C, which the driver consumes rather than
 * delivering (docs/INTERRUPTION.md).
 *
 * They are NOT acted on here. This runs in the keyboard IRQ and the compositor
 * takes a plain spinlock and repaints -- so the driver only records what was
 * asked, and the kernel's main loop performs it in schedulable context, the
 * same arrangement compositor_pointer_tick() already uses. */
enum {
    SYSKEY_NONE = 0,
    SYSKEY_NEXT_WINDOW,      /* GUI+Tab */
    SYSKEY_CLOSE_WINDOW,     /* GUI+W   */
};
/* Take the pending system shortcut, if any, and clear it. */
int keyboard_take_syskey(void);

/* ---- the KEY EVENT stream -----------------------------------------------
 * The char stream above answers "what did the user TYPE". It cannot answer
 * "what key is DOWN", and it never will -- not from lack of effort, but
 * because it is out of room and inherently ambiguous:
 *
 *   - C0 is already spoken for. Ctrl+letter occupies 0x01-0x1A, so Up (0x13)
 *     and Ctrl+S are THE SAME BYTE. That ambiguity is real today.
 *   - F1-F12, Alt, the GUI/Menu keys and every key RELEASE have no byte to be.
 *
 * So key events are a SECOND, parallel stream rather than an escape encoding
 * bolted onto the first. A text reader keeps doing byte compares and is
 * completely unaffected; anything that needs real key state (a game, a
 * hold-to-repeat UI, a chord like Alt+F4) polls events instead. Neither stream
 * is a lossy re-encoding of the other, which is the whole point.
 *
 * Keycodes are NOT ASCII: printable keys carry their UNSHIFTED ASCII (so 'a'
 * is 'a' whether or not Shift is down -- the char stream already told you it
 * was 'A'), and everything else lives at >= 0x100 where nothing collides. */
#define EKC_F1     0x101
#define EKC_F2     0x102
#define EKC_F3     0x103
#define EKC_F4     0x104
#define EKC_F5     0x105
#define EKC_F6     0x106
#define EKC_F7     0x107
#define EKC_F8     0x108
#define EKC_F9     0x109
#define EKC_F10    0x10A
#define EKC_F11    0x10B
#define EKC_F12    0x10C
#define EKC_INS    0x110
#define EKC_LWIN   0x111
#define EKC_RWIN   0x112
#define EKC_MENU   0x113
#define EKC_LEFT   0x120
#define EKC_RIGHT  0x121
#define EKC_UP     0x122
#define EKC_DOWN   0x123
#define EKC_HOME   0x124
#define EKC_END    0x125
#define EKC_PGUP   0x126
#define EKC_PGDN   0x127
#define EKC_DEL    0x128
#define EKC_LSHIFT 0x130
#define EKC_RSHIFT 0x131
#define EKC_LCTRL  0x132
#define EKC_RCTRL  0x133
#define EKC_LALT   0x134
#define EKC_RALT   0x135
#define EKC_CAPS   0x140
#define EKC_NUM    0x141
#define EKC_SCROLL 0x142

/* Modifier bits, as seen AT THE MOMENT the event was generated. */
#define EKM_SHIFT  0x01
#define EKM_CTRL   0x02
#define EKM_ALT    0x04
#define EKM_GUI    0x08   /* either Windows/Command key */
#define EKM_CAPS   0x10   /* the LATCH, not the key */
#define EKM_NUM    0x20
#define EKM_SCROLL 0x40

struct key_event {
    uint16_t code;      /* unshifted ASCII, or an EKC_* */
    uint8_t  mods;      /* EKM_* bitmap at event time */
    uint8_t  pressed;   /* 1 = make, 0 = break */
};

/* Pop one event. Returns 1 and fills *ev, or 0 when the queue is empty.
 * Non-blocking by design: an event consumer is a UI frame loop or a game, and
 * both already have a clock -- neither wants to block a thread on a keypress. */
int keyboard_event_pop(struct key_event *ev);

/* The live modifier bitmap (EKM_*). Answers "is Shift down RIGHT NOW", which
 * the event stream cannot if you missed the make. */
uint8_t keyboard_mods(void);

/* ---- keyboard layouts ---------------------------------------------------
 * A layout is the two ASCII tables, nothing more: the scancode->key mapping is
 * hardware, and the key->character mapping is policy. Swapping tables is the
 * whole of "support AZERTY" for a set-1 PC keyboard.
 * keyboard_set_layout() returns 0 on success, -1 for an unknown name (it does
 * NOT fall back to US silently -- a typo'd layout name that quietly keeps
 * QWERTY is worse than a refusal). */
/* A layout is three tables of CODEPOINTS -- not characters.
 *
 * It was `const char *` until French made that untenable: é è ç à ù are on the
 * number row of a real AZERTY keyboard, and none of them exists in 7 bits. The
 * tables are Unicode scalar values now, the char stream carries UTF-8, and a
 * pure-ASCII layout is unaffected -- 'a' is as valid a uint32_t initialiser as
 * a char one, so the US and Dvorak tables did not change a single entry.
 *
 * A DEAD KEY is an entry wrapped in KEY_DEAD(): it produces no character of its
 * own and instead combines with the next key (^ then e -> ê). AltGr is the
 * third level every non-US layout needs for @ # { } [ ] | \ €. */
#define KEY_DEAD_BIT     0x80000000u
#define KEY_DEAD(cp)     (KEY_DEAD_BIT | (uint32_t)(cp))
#define KEY_IS_DEAD(v)   (((v) & KEY_DEAD_BIT) != 0)
#define KEY_BARE(v)      ((v) & ~KEY_DEAD_BIT)

struct keymap {
    const char *name;
    const uint32_t *normal;   /* [128], indexed by set-1 make code */
    const uint32_t *shift;    /* [128] */
    const uint32_t *altgr;    /* [128], or NULL if the layout has no third level */
};
/* Translate one set-1 MAKE code through the current layout, exactly as the
 * interrupt handler does (dead keys, AltGr, Caps, Ctrl, UTF-8 delivery). Public
 * for the keymap selftest; nothing else should need it. */
void        kbd_translate(uint8_t make, int pressed);
/* Inject a key from a NON-PS/2 keyboard (virtio-input): the event code, and
 * the codepoint it typed, which is encoded to UTF-8 into the char stream. */
void        keyboard_inject_cp(uint16_t code, int pressed, uint32_t cp);
void        kbd_mods_force(uint8_t mods);   /* selftest: hold Shift/Caps/Ctrl  */
void        kbd_altgr_force(int on);        /* selftest: hold AltGr (right Alt) */
int         keyboard_set_layout(const char *name);
const char *keyboard_layout(void);

// Initialise the keyboard driver, register IRQ1 handler and unmask IRQ1 at PIC
void keyboard_init(void);

// Blocking read: returns the next ASCII character available
// Returns 0 if no character is available (e.g. non-ASCII key pressed)
char keyboard_getchar(void);

// Non-blocking read: returns 1 if the next ASCII character is available, otherwise returns 0
int keyboard_has_char(void);

/* Ctrl-C routing — docs/INTERRUPTION.md.
 *
 * `pid` receives console interrupts: a ^C cancels it (process_cancel) instead of
 * being delivered as a byte. 0 clears the route, and then ^C is just input.
 *
 * There is ONE slot because there is ONE console. This is a DELEGATION, not an
 * inferred "foreground process" — EmbLink has no session or process group, and
 * a process that never routes is simply never interrupted. Set via
 * sys_console_interrupt_route(), which only accepts a HANDLE the caller holds,
 * so nobody can route interrupts at a process they were never given. */
void keyboard_set_interrupt_target(uint32_t pid);

/* sys_console_interrupt_route's two non-handle values. SELF is -2, not 0: 0 is
 * a valid spawn handle (a process's first child), and meaning "self" by it
 * routed a shell's first child's ^C to the shell. */
#define EMBK_INTR_ROUTE_SELF  (-2)
#define EMBK_INTR_ROUTE_NONE  (-1)
/* Who did the routing. ^C needs only the target; ^Z also has to wake the
 * parent, because a frozen child never exits and a parent waiting for one
 * would wait forever. Set by the same call that sets the target. */
void keyboard_set_interrupt_router(uint32_t pid);
uint32_t keyboard_get_interrupt_target(void);

/* Cancellation-aware console read (docs/INTERRUPTION.md). EMBK_OK + *out, or
 * -EMBK_ECANCELED if the calling process was cancelled while waiting. */
int keyboard_getchar_blocking_cancelable(char *out);

// Inject a character into the keyboard buffer (used by USB HID driver).
void keyboard_inject_char(char c);

/* Deliver one DECODED key from any hardware -- the seam every input driver that
 * is not PS/2 comes in through (virtio-input today, and where USB HID belongs).
 * `code` is an unshifted ASCII or an EKC_*; `ascii` is the character the key
 * produces, or 0 for keys that produce none, and is delivered only on a make.
 * Modifiers and locks are recognised by their EKC_* and update the live
 * modifier bitmap, so a caller never has to track Shift itself. */
void keyboard_inject_event(uint16_t code, int pressed, char ascii);

/* Compose the CHARACTER an AT set-1 make code produces under `mods`, through
 * the CURRENT layout -- shift table, Caps Lock and Ctrl all applied. Returns 0
 * for a key with no character.
 *
 * A Linux evdev keycode is an AT set-1 make code for the main key block (evdev
 * took its numbering from set 1), so a virtio-input driver can call this
 * directly and inherits `keyboard_set_layout` for free. */
/* Apply the CURRENT layout to a make code: shift, AltGr, dead keys, Caps and
 * Ctrl, returning a Unicode codepoint (0 = this key types nothing, which is
 * also what a dead key returns while it waits for the next one).
 *
 * Returns a CODEPOINT, not a char, because a layout can produce é and a char
 * cannot hold it -- the virtio path truncated exactly that into a lone 0xE9,
 * which is not even valid UTF-8. Deliver it with keyboard_inject_cp(). */
uint32_t keyboard_compose_cp(uint8_t make, uint8_t mods);

/* The UNSHIFTED key identity for the same code -- what an event carries. */
uint16_t keyboard_keycode_of(uint8_t make);

// Keyboard grab: while grabbed, the kernel shell stops draining the buffer so a
// ring-3 UI app has exclusive keystrokes. Auto-released when the grabber exits.
void keyboard_set_grab(int grab, uint32_t pid);
int  keyboard_is_grabbed(void);
void keyboard_release_grab_pid(uint32_t pid);

/* Blocking read that YIELDS (wait-queue backed) rather than halting the CPU.
 * Use this from any context where a scheduler exists; keyboard_getchar()'s
 * hlt-spin is only correct pre-scheduler. */
char keyboard_getchar_blocking(void);

#endif /* __KEYBOARD__H__ */