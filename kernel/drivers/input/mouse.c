#if defined(__x86_64__)
#include "power/policy.h"
#define POINTER_ACTIVITY() power_activity()
#else
#define POINTER_ACTIVITY() ((void)0)
#endif
#include "include/spinlock.h"
/* kernel/drivers/input/mouse.c -- PS/2 mouse driver (see mouse.h).
 *
 * The 8042 controller delivers mouse data on IRQ12. Each movement is a 3-byte
 * packet: [flags, dx, dy]. We accumulate a clamped absolute cursor position and
 * a button bitmask that ring-3 polls via sys_ui_input. */

static spinlock_t pointer_state_lock=SPINLOCK_INIT;
#include "drivers/input/mouse.h"
/* Same split as keyboard.c: the PS/2 aux-device driver is x86, the CURSOR STATE
 * (position, clamp, buttons, wheel) is shared. mouse_set_absolute() below was
 * already the arch-neutral way in -- it was written for a USB tablet -- and
 * virtio-input's tablet uses it unchanged, which is why this file needed no new
 * entry point the way keyboard.c did. */
#if defined(__x86_64__)
#include "arch/x86_64/irq/irq.h"
#include "include/io.h"
#include "drivers/input/i8042.h"
#endif
#include "drivers/char/serial.h"
#include "include/kprintf.h"
#include "drivers/timer/timer.h"   /* timer_uptime_ms: a press knows its own time */

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

/* status register bits */
#define PS2_ST_OUTPUT_FULL 0x01   /* byte available to read from 0x60 */
#define PS2_ST_INPUT_FULL  0x02   /* controller busy; wait before writing */
#define PS2_ST_FROM_MOUSE  0x20   /* the pending byte is from the aux (mouse) port */

/* packet byte-0 flags */
#define PKT_LEFT     0x01
#define PKT_RIGHT    0x02
#define PKT_MIDDLE   0x04
#define PKT_ALWAYS1  0x08   /* validity/sync bit -- always set on a real byte 0 */
#define PKT_X_SIGN   0x10
#define PKT_Y_SIGN   0x20

static volatile int32_t  g_x, g_y;
/* When the pointer first moved since the compositor last drew it (ns, 0 = it
 * has not): the start of the motion-to-screen latency comp_stats reports. The
 * OLDEST unshown motion, so a burst of packets is timed from its first. */
static volatile uint64_t g_motion_ns;
static inline void note_motion(int32_t nx, int32_t ny) {
    if ((nx != g_x || ny != g_y) && !g_motion_ns) g_motion_ns = time_get_ns();
}
/* Who to tell that the pointer changed: the compositor's frame thread. Called
 * after the lock is let go -- it takes the scheduler's. */
static void (*volatile g_notify)(void);
void mouse_set_notify(void (*fn)(void)) { g_notify = fn; }
static void notify(void) {
    void (*fn)(void) = g_notify;
    if (fn) fn();
}

uint64_t mouse_take_motion_ns(void) {
    uint64_t t = g_motion_ns;
    g_motion_ns = 0;
    return t;
}
static volatile uint32_t g_buttons;
static volatile int32_t  g_wheel;        /* accumulated scroll notches (IntelliMouse Z) */

/* LEFT-BUTTON PRESSES THAT HAVE HAPPENED, counted as the packets arrive.
 *
 * The button STATE alone cannot answer "how many times was this clicked".
 * compositor_pointer_tick() used to find a press by diffing mouse_get_state()
 * against the previous tick, which works only while clicks are slower than the
 * tick: a complete down-up-down-up between two ticks shows the same state at
 * both ends and yields ZERO edges, so the whole gesture vanished before any of
 * the system could see it. That is what made double-click unreliable and fast
 * double-clicks impossible -- measured, clicks 130 ms apart survived only
 * sometimes and 50 ms apart never.
 *
 * A COUNT cannot miss one. The IRQ handler already sees every packet, so it
 * does the counting here; the compositor takes the total and latches that many
 * clicks. Incremented in the two places a button state is set -- the PS/2
 * packet path and mouse_set_absolute (the tablet, which is how aarch64 gets a
 * pointer at all) -- so both arches are fixed by the same lines. */
/* AND WHEN EACH HAPPENED. A count alone is not enough: the caller would have
 * to stamp them with the time it NOTICED, and two presses noticed on different
 * ticks then look as far apart as the ticks are -- which on a busy kernel loop
 * is further apart than any double-click window. The press's own time is known
 * here and nowhere else, so it is taken here.
 *
 * Eight deep: the question is only ever "how many clicks in one gesture", and
 * a burst longer than that is not a gesture anyone is making deliberately. */
#define MOUSE_PRESS_Q 8
static volatile uint32_t g_press_t[MOUSE_PRESS_Q];
static volatile uint8_t  g_press_head, g_press_n;

/* Apply a new button mask, recording the DOWN transition of the left button.
 * Both device paths go through here so neither can forget to. */
static void buttons_set(uint32_t nb) {
    uint32_t ob = g_buttons;
    if ((nb & MOUSE_BTN_LEFT) && !(ob & MOUSE_BTN_LEFT)) {
        if (g_press_n < MOUSE_PRESS_Q) {
            uint8_t slot = (uint8_t)((g_press_head + g_press_n) % MOUSE_PRESS_Q);
            g_press_t[slot] = (uint32_t)timer_uptime_ms();
            g_press_n++;
        }
    }
    g_buttons = nb;
}

/* Take the oldest un-taken press and the millisecond it happened. Returns 1 if
 * there was one, 0 if not. */
int mouse_take_press_inner(uint32_t *when_ms) {
    if (!g_press_n) return 0;
    if (when_ms) *when_ms = g_press_t[g_press_head];
    g_press_head = (uint8_t)((g_press_head + 1) % MOUSE_PRESS_Q);
    g_press_n--;
    return 1;
}
int mouse_take_press(uint32_t *when_ms) {
    spin_lock(&pointer_state_lock);
    int result=mouse_take_press_inner(when_ms);
    spin_unlock(&pointer_state_lock);return result;
}

void mouse_discard_presses(void) {
    spin_lock(&pointer_state_lock);g_press_head=g_press_n=0;spin_unlock(&pointer_state_lock);
}

static int32_t  g_w = 1024, g_h = 768;

static uint8_t  g_pkt[4];
static int      g_cycle;
static int      g_pkt_len = 3;           /* 4 once IntelliMouse (wheel) is negotiated */

void mouse_get_state_inner(int32_t *x, int32_t *y, uint32_t *buttons) {
    if (x) *x = g_x;
    if (y) *y = g_y;
    if (buttons) *buttons = g_buttons;
}
void mouse_get_state(int32_t *x,int32_t *y,uint32_t *buttons) {
    spin_lock(&pointer_state_lock);
    mouse_get_state_inner(x,y,buttons);
    spin_unlock(&pointer_state_lock);
}


/* Return and clear the scroll delta accumulated since the last call (notches;
 * +up / -down). ring-3 polls this each frame via sys_ui_input. */
int32_t mouse_take_wheel_inner(void) {
    int32_t w = g_wheel; g_wheel = 0; return w;
}
int32_t mouse_take_wheel(void) {
    spin_lock(&pointer_state_lock);
    int32_t result=mouse_take_wheel_inner();
    spin_unlock(&pointer_state_lock);return result;
}


#if defined(__x86_64__)
/* One byte to the device on port 2, ACK consumed. The 0xD4 prefix, the bounded
 * waits and the RESEND retry live in i8042.c, which owns the controller. */
static bool mouse_command(uint8_t cmd) { return i8042_aux_write(cmd); }

/* Apply the accumulated packet's motion + buttons (bytes 0..2, common to both
 * the 3- and 4-byte formats). */
static void apply_motion(void) {
    POINTER_ACTIVITY();
    int32_t dx = g_pkt[1];
    int32_t dy = g_pkt[2];
    if (g_pkt[0] & PKT_X_SIGN) dx |= ~0xFF;   /* sign-extend 8->32 */
    if (g_pkt[0] & PKT_Y_SIGN) dy |= ~0xFF;
    int32_t nx = g_x + dx;
    int32_t ny = g_y - dy;                     /* PS/2 y is up-positive; screen is down */
    if (nx < 0) nx = 0; if (nx >= g_w) nx = g_w - 1;
    if (ny < 0) ny = 0; if (ny >= g_h) ny = g_h - 1;
    note_motion(nx, ny);
    g_x = nx; g_y = ny;
    buttons_set(g_pkt[0] & (PKT_LEFT | PKT_RIGHT | PKT_MIDDLE));
}

#endif /* __x86_64__ */

/* Absolute pointer update (USB tablet, virtio-input tablet): map [0,range] ->
 * screen and set
 * cursor directly. Same clamped g_x/g_y the compositor reads; whichever device
 * (PS/2 or tablet) produces events wins. Aligned 32-bit stores are atomic on
 * x86, so this races harmlessly with the IRQ12 PS/2 writer. */
void mouse_set_absolute_inner(int32_t x, int32_t y, int32_t range,
                        uint32_t buttons, int32_t wheel) {
    if (range <= 0) range = 1;
    int32_t nx = (int32_t)(((int64_t)x * (g_w - 1)) / range);
    int32_t ny = (int32_t)(((int64_t)y * (g_h - 1)) / range);
    if (nx < 0) nx = 0; if (nx >= g_w) nx = g_w - 1;
    if (ny < 0) ny = 0; if (ny >= g_h) ny = g_h - 1;
    if(g_x!=nx || g_y!=ny || wheel || buttons!=g_buttons)POINTER_ACTIVITY();
    note_motion(nx, ny);
    g_x = nx; g_y = ny;
    buttons_set(buttons & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT | MOUSE_BTN_MIDDLE));
    if (wheel) g_wheel += wheel;
}
void mouse_set_absolute(int32_t x,int32_t y,int32_t range,uint32_t buttons,int32_t wheel) {
    spin_lock(&pointer_state_lock);
    mouse_set_absolute_inner(x,y,range,buttons,wheel);
    spin_unlock(&pointer_state_lock);
    notify();
}


void mouse_move_relative_inner(int32_t dx, int32_t dy, uint32_t buttons, int32_t wheel) {
    int32_t nx = g_x + dx;
    int32_t ny = g_y + dy;                     /* screen convention: +y is DOWN */
    if (nx < 0) nx = 0; if (nx >= g_w) nx = g_w - 1;
    if (ny < 0) ny = 0; if (ny >= g_h) ny = g_h - 1;
    if(g_x!=nx || g_y!=ny || wheel || buttons!=g_buttons)POINTER_ACTIVITY();
    note_motion(nx, ny);
    g_x = nx; g_y = ny;
    buttons_set(buttons & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT | MOUSE_BTN_MIDDLE));
    if (wheel) g_wheel += wheel;
}
void mouse_move_relative(int32_t dx,int32_t dy,uint32_t buttons,int32_t wheel) {
    spin_lock(&pointer_state_lock);
    mouse_move_relative_inner(dx,dy,buttons,wheel);
    spin_unlock(&pointer_state_lock);
    notify();
}


#if defined(__x86_64__)
static void mouse_handler_inner(void) {
    /* On IRQ12 the pending byte is mouse data; read it directly. (Some 8042
     * emulations don't set the AUX status bit reliably, so we don't gate on it.) */
    uint8_t b = inb(PS2_DATA);

    switch (g_cycle) {
        case 0:
            if (!(b & PKT_ALWAYS1)) return;  /* out of sync -- wait for a real byte 0 */
            g_pkt[0] = b; g_cycle = 1;
            break;
        case 1:
            g_pkt[1] = b; g_cycle = 2;
            break;
        case 2:
            g_pkt[2] = b;
            if (g_pkt_len == 3) { apply_motion(); g_cycle = 0; }
            else g_cycle = 3;               /* IntelliMouse: one more byte (Z/wheel) */
            break;
        case 3: {
            g_pkt[3] = b; g_cycle = 0;
            apply_motion();
            int32_t z = g_pkt[3] & 0x0F;    /* 4-bit two's-complement scroll delta */
            if (z & 0x08) z |= ~0x0F;
            /* IntelliMouse Z is positive down; the shared pointer ABI and
             * HID/virtio transports use positive up. Normalize at this seam. */
            g_wheel -= z;
            break;
        }
    }
}
static void mouse_handler(void) {
    spin_lock(&pointer_state_lock);
    mouse_handler_inner();
    spin_unlock(&pointer_state_lock);
    notify();
}


#endif /* __x86_64__ */

void mouse_init(uint32_t screen_w, uint32_t screen_h) {
    serial_write_string("\n=== Mouse init ===\n");
    g_w = (int32_t)screen_w; g_h = (int32_t)screen_h;
    g_x = g_w / 2; g_y = g_h / 2;
    g_cycle = 0; g_buttons = 0; g_press_head = g_press_n = 0;

#if !defined(__x86_64__)
    /* The clamp bounds and the centred cursor ARE the whole of mouse_init()
     * where there is no PS/2 controller to bring up. Everything below this
     * point talks to the 8042. */
    serial_write_string("Mouse: no PS/2 controller here; virtio-input drives the cursor\n");
    return;
#else

    /* THE CONTROLLER FIRST, and it decides whether there is anything here at
     * all. A laptop touchpad that speaks PS/2 is on port 2 and this drives it;
     * one on an I2C bus is not, and the honest thing is to say so and leave
     * the cursor to whatever else can move it, rather than spending four
     * unanswered commands finding out the hard way on every boot. */
    if (!i8042_init() || !i8042_have_aux()) {
        kprintf("Mouse: no PS/2 pointing device; a USB or virtio pointer moves "
                "the cursor instead\n");
        return;
    }

    mouse_command(0xF6);   /* set defaults */

    /* IntelliMouse magic knock: sample rates 200,100,80 then GET_DEVICE_ID; a
     * reply of 0x03 means the mouse now emits a 4th byte carrying the wheel.
     * A laptop touchpad in its default PS/2 mode answers this too -- most of
     * them report 0x00 and send plain 3-byte packets, which is exactly what
     * the parser below expects, and is why a touchpad works here at all. */
    mouse_command(0xF3); mouse_command(200);
    mouse_command(0xF3); mouse_command(100);
    mouse_command(0xF3); mouse_command(80);
    int id = -1;
    if (mouse_command(0xF2)) { id = i8042_read(100000); }
    if (id == 0x03) { g_pkt_len = 4; kprintf("Mouse: IntelliMouse wheel enabled\n"); }

    /* A touchpad reports far more often than a mouse needs to, and every
     * report is an interrupt. 100/s is the usual compromise and is what the
     * cursor code can consume; the default is 100 already on most devices,
     * but a device left at 200 by the firmware halves the interrupt load for
     * one command. */
    mouse_command(0xF3); mouse_command(100);

    mouse_command(0xF4);   /* enable data reporting */

    irq_register(12, mouse_handler);
    i8042_irq_enable(false, true);          /* IRQ12 on, handler now in place */
    kprintf("Mouse: PS/2 pointing device ready on IRQ 12 (%u-byte packets)\n",
            (unsigned)g_pkt_len);
#endif /* __x86_64__ */
}

void mouse_warp(int32_t x,int32_t y) {
    spin_lock(&pointer_state_lock);
    if(x<0)x=0;if(y<0)y=0;if(x>=g_w)x=g_w-1;if(y>=g_h)y=g_h-1;
    g_x=x;g_y=y;
    spin_unlock(&pointer_state_lock);
}
void mouse_set_bounds(uint32_t w,uint32_t h) {
    if(!w || !h)return;
    spin_lock(&pointer_state_lock);
    g_w=(int32_t)w;g_h=(int32_t)h;
    if(g_x>=g_w)g_x=g_w-1;if(g_y>=g_h)g_y=g_h-1;
    g_press_head=g_press_n=0;
    spin_unlock(&pointer_state_lock);
}
