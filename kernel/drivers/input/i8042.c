/* kernel/drivers/input/i8042.c -- the PS/2 controller (see i8042.h).
 *
 * The bring-up below is the sequence the chip documents, in the order it
 * documents it, and every step says what it found. That reporting is not
 * decoration: the machine this was written for has no serial port, so the
 * boot log on its EFI partition is the only way to learn whether its keyboard
 * is a PS/2 device at all -- and whether its touchpad is the second port or
 * something on an I2C bus this kernel has yet to meet.
 */
#include "drivers/input/i8042.h"

#if defined(__x86_64__)

#include "include/io.h"
#include "include/kprintf.h"
#include "acpi/acpi.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define ST_OBF      0x01   /* output buffer full: a byte is waiting at 0x60   */
#define ST_IBF      0x02   /* input buffer full: the controller is still busy */
#define ST_AUX      0x20   /* the waiting byte came from port 2               */

#define CFG_IRQ1    0x01   /* port 1 interrupt (IRQ1)                         */
#define CFG_IRQ12   0x02   /* port 2 interrupt (IRQ12)                        */
#define CFG_KBD_OFF 0x10   /* 1 = port 1 clock DISABLED                       */
#define CFG_AUX_OFF 0x20   /* 1 = port 2 clock DISABLED                       */
#define CFG_XLATE   0x40   /* set 2 -> set 1 translation                      */

#define CMD_READ_CFG   0x20
#define CMD_WRITE_CFG  0x60
#define CMD_AUX_OFF    0xA7
#define CMD_AUX_ON     0xA8
#define CMD_AUX_TEST   0xA9
#define CMD_SELFTEST   0xAA
#define CMD_KBD_TEST   0xAB
#define CMD_KBD_OFF    0xAD
#define CMD_KBD_ON     0xAE
#define CMD_TO_AUX     0xD4

/* Spin budgets. The units are status-register reads, which are I/O port reads
 * and therefore ~1 us each on any real chipset -- so 100k is a tenth of a
 * second, and a device that has not answered by then is not going to. Long
 * enough for a real keyboard's 0xFF reset (which takes ~600 ms on some), which
 * is why RESET gets its own, larger budget. */
#define SPIN_NORMAL   100000u
#define SPIN_RESET   2000000u

static bool g_done;
static bool g_present;
static bool g_have_kbd;
static bool g_have_aux;
static uint8_t g_cfg;

bool i8042_have_kbd(void) { return g_have_kbd; }
bool i8042_have_aux(void) { return g_have_aux; }

static bool wait_write(void) {
    for (uint32_t i = 0; i < SPIN_NORMAL; i++)
        if (!(inb(PS2_STATUS) & ST_IBF)) return true;
    return false;
}

int i8042_read(uint32_t spins) {
    for (uint32_t i = 0; i < spins; i++)
        if (inb(PS2_STATUS) & ST_OBF) return (int)inb(PS2_DATA);
    return -1;
}

uint32_t i8042_flush(void) {
    uint32_t n = 0;
    /* Bounded: a controller that reports OBF forever (which a missing one
     * does -- 0xFF on every port read) must not spin here for ever. */
    while ((inb(PS2_STATUS) & ST_OBF) && n < 64u) { (void)inb(PS2_DATA); n++; }
    return n;
}

bool i8042_cmd(uint8_t cmd) {
    if (!wait_write()) return false;
    outb(PS2_CMD, cmd);
    return true;
}

bool i8042_cmd_data(uint8_t cmd, uint8_t data) {
    if (!i8042_cmd(cmd)) return false;
    if (!wait_write()) return false;
    outb(PS2_DATA, data);
    return true;
}

int i8042_cmd_read(uint8_t cmd) {
    if (!i8042_cmd(cmd)) return -1;
    return i8042_read(SPIN_NORMAL);
}

/* One byte to a device, ACK consumed. A 0xFE means RESEND and is retried; the
 * count is small because a device that needs three goes is broken anyway. */
static bool device_write(bool aux, uint8_t byte, uint32_t spins) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (aux && !i8042_cmd(CMD_TO_AUX)) return false;
        if (!wait_write()) return false;
        outb(PS2_DATA, byte);
        int r = i8042_read(spins);
        if (r == 0xFA) return true;     /* ACK */
        if (r != 0xFE) return false;    /* not a RESEND: it is not listening */
    }
    return false;
}

bool i8042_kbd_write(uint8_t byte) { return device_write(false, byte, SPIN_NORMAL); }
bool i8042_aux_write(uint8_t byte) { return device_write(true,  byte, SPIN_NORMAL); }

void i8042_irq_enable(bool kbd, bool aux) {
    if (!g_present) return;
    if (kbd && g_have_kbd) g_cfg |= CFG_IRQ1;
    if (aux && g_have_aux) g_cfg |= CFG_IRQ12;
    i8042_cmd_data(CMD_WRITE_CFG, g_cfg);
}

/* Reset a device and read back its identity. Returns the number of identity
 * bytes read (0..2) and fills `id`, or -1 if the device never answered.
 *
 * The reset reply is 0xFA (ACK) then 0xAA (self-test passed); a keyboard adds
 * nothing more, a mouse adds its device id 0x00. Then 0xF2 asks properly: a
 * keyboard says 0xAB 0x83, a mouse says one byte (0x00 plain, 0x03 with a
 * wheel, 0x04 five-button). Naming it in the log is the point -- it is how a
 * person reading EMBLINK.LOG on another computer finds out what their laptop
 * actually has. */
static int device_identify(bool aux, uint8_t id[2]) {
    if (!device_write(aux, 0xFF, SPIN_RESET)) return -1;
    int st = i8042_read(SPIN_RESET);
    if (st != 0xAA) return -1;                 /* no self-test pass: nothing there */
    (void)i8042_read(SPIN_NORMAL);             /* a mouse's trailing 0x00, if any  */

    if (!device_write(aux, 0xF5, SPIN_NORMAL)) return 0;  /* disable scanning first */
    if (!device_write(aux, 0xF2, SPIN_NORMAL)) return 0;
    int n = 0;
    int b = i8042_read(SPIN_NORMAL);
    if (b >= 0) { id[n++] = (uint8_t)b; b = i8042_read(SPIN_NORMAL); }
    if (b >= 0 && n < 2) { id[n++] = (uint8_t)b; }
    return n;
}

static const char *name_for(bool aux, const uint8_t *id, int n) {
    if (aux) {
        if (n < 1) return "PS/2 pointing device";
        switch (id[0]) {
            case 0x00: return "standard PS/2 mouse or touchpad";
            case 0x03: return "wheel mouse";
            case 0x04: return "five-button wheel mouse";
            default:   return "unknown pointing device";
        }
    }
    if (n >= 2 && id[0] == 0xAB) return "MF2 keyboard";
    if (n >= 1) return "keyboard";
    return "device that did not identify itself";
}

bool i8042_init(void) {
    if (g_done) return g_present;
    g_done = true;

    /* DOES THIS MACHINE EVEN HAVE ONE? The firmware has an opinion (the FADT's
     * IA-PC boot flags) and it is worth reading out loud, but it is NOT the
     * authority: QEMU's tables say "no 8042" on a machine that plainly has
     * one, and real firmware has been wrong in both directions. So the flag
     * is a hint and the controller is asked directly -- a missing one reads
     * 0xFF on every port, which fails the first bounded wait below and stands
     * the driver down within a fraction of a second. Linux does the same:
     * a firmware "absent" still probes; only a platform-level "absent" is
     * believed without asking. */
    bool known = false;
    bool claimed = acpi_i8042_present(&known);
    if (known && !claimed)
        kprintf("i8042: the firmware says there is no PS/2 controller (FADT IA-PC "
                "boot flags) -- asking the hardware anyway, firmware has been wrong\n");

    /* Quiet BOTH ports before anything else. Whatever the firmware's own
     * keyboard driver was doing, it stops here. */
    i8042_cmd(CMD_KBD_OFF);
    i8042_cmd(CMD_AUX_OFF);

    /* AND EMPTY THE BUFFER. This is the step whose absence cost a laptop its
     * keyboard: the next thing done is a read-modify-write of the config byte,
     * and a leftover scancode read in its place gets written back as config. */
    uint32_t stale = i8042_flush();

    int cfg = i8042_cmd_read(CMD_READ_CFG);
    if (cfg < 0 || cfg == 0xFF) {        /* 0xFF is an empty bus, not a config */
        kprintf("i8042: no answer from the controller -- there is no PS/2 here\n");
        return false;
    }
    g_cfg = (uint8_t)cfg;

    /* Interrupts and translation OFF while testing: a byte arriving mid-test
     * would go to a handler that is not ready, and translation changes what
     * the identify bytes look like. */
    uint8_t test_cfg = (uint8_t)(g_cfg & ~(CFG_IRQ1 | CFG_IRQ12 | CFG_XLATE));
    i8042_cmd_data(CMD_WRITE_CFG, test_cfg);

    int st = i8042_cmd_read(CMD_SELFTEST);
    if (st != 0x55) {
        kprintf("i8042: controller self-test answered 0x%x, not 0x55 -- "
                "standing down\n", (unsigned)(st < 0 ? 0xFFu : (unsigned)st));
        return false;
    }
    /* Some controllers reset themselves in the self-test. Put the config back. */
    i8042_cmd_data(CMD_WRITE_CFG, test_cfg);

    /* IS THERE A SECOND PORT? Enabling it and reading the config back is the
     * documented way to ask: a controller with one port leaves the aux clock
     * bit set because there is no clock to enable. */
    bool dual = false;
    i8042_cmd(CMD_AUX_ON);
    int back = i8042_cmd_read(CMD_READ_CFG);
    if (back >= 0 && !((uint8_t)back & CFG_AUX_OFF)) dual = true;
    i8042_cmd(CMD_AUX_OFF);

    int t1 = i8042_cmd_read(CMD_KBD_TEST);
    int t2 = dual ? i8042_cmd_read(CMD_AUX_TEST) : -1;

    g_present = true;
    kprintf("i8042: controller ok (self-test 0x55%s%s), port 1 test 0x%x%s\n",
            stale ? ", " : "", stale ? "buffer had stale bytes" : "",
            (unsigned)(t1 < 0 ? 0xFFu : (unsigned)t1),
            dual ? ", second port present" : ", single port");

    /* Bring up each port that passed, and ask what is on it. */
    uint8_t id[2] = {0, 0};
    if (t1 == 0) {
        i8042_cmd(CMD_KBD_ON);
        int n = device_identify(false, id);
        if (n >= 0) {
            g_have_kbd = true;
            kprintf("i8042: port 1 holds a %s (id %x %x)\n",
                    name_for(false, id, n), (unsigned)id[0], (unsigned)id[1]);
        } else {
            kprintf("i8042: port 1 tested good but nothing answered a reset\n");
        }
    }
    if (dual && t2 == 0) {
        i8042_cmd(CMD_AUX_ON);
        int n = device_identify(true, id);
        if (n >= 0) {
            g_have_aux = true;
            kprintf("i8042: port 2 holds a %s (id %x)\n",
                    name_for(true, id, n), (unsigned)id[0]);
        } else {
            kprintf("i8042: port 2 tested good but nothing answered a reset -- "
                    "this laptop's touchpad is somewhere else (I2C, most likely)\n");
        }
    } else if (!dual) {
        kprintf("i8042: no second port -- the pointing device is not PS/2 here\n");
    }

    /* Translation back on: the scancode tables in keyboard.c are set 1, and
     * the keyboard is left in set 2, which is what the controller translates
     * from. Interrupts stay off until the drivers say their handlers are up. */
    g_cfg = (uint8_t)(test_cfg | CFG_XLATE);
    g_cfg &= (uint8_t)~CFG_KBD_OFF;
    if (g_have_aux) g_cfg &= (uint8_t)~CFG_AUX_OFF;
    i8042_cmd_data(CMD_WRITE_CFG, g_cfg);
    return true;
}

#else  /* !__x86_64__ -- no 8042 on this architecture at all. */

bool i8042_init(void) { return false; }
bool i8042_have_kbd(void) { return false; }
bool i8042_have_aux(void) { return false; }
int  i8042_read(uint32_t spins) { (void)spins; return -1; }
uint32_t i8042_flush(void) { return 0; }
bool i8042_cmd(uint8_t cmd) { (void)cmd; return false; }
bool i8042_cmd_data(uint8_t cmd, uint8_t data) { (void)cmd; (void)data; return false; }
int  i8042_cmd_read(uint8_t cmd) { (void)cmd; return -1; }
bool i8042_kbd_write(uint8_t byte) { (void)byte; return false; }
bool i8042_aux_write(uint8_t byte) { (void)byte; return false; }
void i8042_irq_enable(bool kbd, bool aux) { (void)kbd; (void)aux; }

#endif
