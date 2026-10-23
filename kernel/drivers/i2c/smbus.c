/* kernel/drivers/i2c/smbus.c -- the two-wire bus everything small hangs off.
 *
 * WHAT IS ON IT, on a real machine: the battery (a Smart Battery answers
 * SMBus commands directly), the memory modules' SPD EEPROMs, the temperature
 * sensors, and -- through a monitor's DDC lines -- the EDID block that says
 * what the display actually is and what it can do.
 *
 * WHAT IS NOT ON IT, said plainly so this is not mistaken for more than it is:
 * a modern laptop's TOUCHPAD is I2C-HID, and it hangs off an Intel LPSS or
 * AMD designware I2C controller, not the chipset's SMBus. QEMU emulates no
 * such controller, so that driver cannot be written or tested here -- only on
 * the target machine. This is the other half of the I2C story and it is the
 * half that CAN be built today.
 *
 * TWO CONTROLLERS, one register layout. Intel has shipped essentially the same
 * SMBus host since the PIIX4: a status byte, a control byte, a command byte,
 * an address byte, two data bytes and a block window. The difference between
 * the 1997 part and the one in a current chipset is where the I/O base is kept
 * in configuration space, which is why the probe below is a two-entry table
 * rather than two drivers.
 *
 * THE PROTOCOL FIELD IS THE WHOLE INTERFACE. SMBus is not a byte stream: the
 * controller runs a complete transaction -- address, command, direction,
 * length -- from one write to the control register, and which transaction it
 * runs is three bits. Getting those wrong does not produce corrupt data; it
 * produces a different transaction on a bus shared with a battery.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "drivers/bus/pci.h"
#include "drivers/i2c/smbus.h"

#if defined(__x86_64__)
#include "include/io.h"

/* Registers, relative to the I/O base. */
#define SMB_HSTSTS  0x00
#define SMB_HSTCNT  0x02
#define SMB_HSTCMD  0x03
#define SMB_HSTADD  0x04
#define SMB_HSTDAT0 0x05
#define SMB_HSTDAT1 0x06
#define SMB_BLKDAT  0x07

/* Status bits. HOST_BUSY means a transaction is running; INTR means one
 * finished. The three error bits are separate on purpose -- a device that did
 * not answer (DEV_ERR) and a bus that is electrically broken (BUS_ERR) are
 * different problems and only one of them is worth retrying. */
#define STS_HOST_BUSY (1u << 0)
#define STS_INTR      (1u << 1)
#define STS_DEV_ERR   (1u << 2)
#define STS_BUS_ERR   (1u << 3)
#define STS_FAILED    (1u << 4)
#define STS_ALL_ERR   (STS_DEV_ERR | STS_BUS_ERR | STS_FAILED)

#define CNT_START     (1u << 6)
#define CNT_PROTO(p)  (((p) & 0x7) << 2)

/* The protocols, by their numbers in the control register. */
#define PROTO_QUICK     0
#define PROTO_BYTE      1
#define PROTO_BYTE_DATA 2
#define PROTO_WORD_DATA 3
#define PROTO_BLOCK     5

static uint16_t g_base;
static bool     g_up;
static const char *g_name = "none";

static bool smbus_wait(void) {
    /* Bounded. A bus with a device holding SDA low never clears HOST_BUSY, and
     * a driver that spins forever there takes the boot with it. */
    for (int i = 0; i < 500000; i++) {
        uint8_t s = inb((uint16_t)(g_base + SMB_HSTSTS));
        if (s & STS_ALL_ERR) {
            outb((uint16_t)(g_base + SMB_HSTSTS), s);   /* write-1-to-clear */
            return false;
        }
        if (!(s & STS_HOST_BUSY) && (s & STS_INTR)) {
            outb((uint16_t)(g_base + SMB_HSTSTS), s);
            return true;
        }
        for (volatile int d = 0; d < 20; d++) { }
    }
    return false;
}

static bool smbus_begin(uint8_t addr, bool read, uint8_t cmd, uint8_t proto) {
    if (!g_up) return false;

    /* Clear whatever the last transaction left. A latched status bit makes the
     * NEXT transaction look finished before it starts. */
    outb((uint16_t)(g_base + SMB_HSTSTS), 0xFF);
    for (int i = 0; i < 10000; i++)
        if (!(inb((uint16_t)(g_base + SMB_HSTSTS)) & STS_HOST_BUSY)) break;

    /* THE ADDRESS IS SHIFTED. SMBus addresses are 7 bits and the register's
     * low bit is the direction -- writing an unshifted address talks to a
     * completely different device, and on a bus with a battery on it that is
     * not a harmless mistake. */
    outb((uint16_t)(g_base + SMB_HSTADD), (uint8_t)((addr << 1) | (read ? 1 : 0)));
    outb((uint16_t)(g_base + SMB_HSTCMD), cmd);
    return true;
}

bool smbus_read_byte_data(uint8_t addr, uint8_t cmd, uint8_t *out) {
    if (!smbus_begin(addr, true, cmd, PROTO_BYTE_DATA)) return false;
    outb((uint16_t)(g_base + SMB_HSTCNT), CNT_START | CNT_PROTO(PROTO_BYTE_DATA));
    if (!smbus_wait()) return false;
    *out = inb((uint16_t)(g_base + SMB_HSTDAT0));
    return true;
}

bool smbus_write_byte_data(uint8_t addr, uint8_t cmd, uint8_t val) {
    if (!smbus_begin(addr, false, cmd, PROTO_BYTE_DATA)) return false;
    outb((uint16_t)(g_base + SMB_HSTDAT0), val);
    outb((uint16_t)(g_base + SMB_HSTCNT), CNT_START | CNT_PROTO(PROTO_BYTE_DATA));
    return smbus_wait();
}

bool smbus_read_word_data(uint8_t addr, uint8_t cmd, uint16_t *out) {
    if (!smbus_begin(addr, true, cmd, PROTO_WORD_DATA)) return false;
    outb((uint16_t)(g_base + SMB_HSTCNT), CNT_START | CNT_PROTO(PROTO_WORD_DATA));
    if (!smbus_wait()) return false;
    *out = (uint16_t)(inb((uint16_t)(g_base + SMB_HSTDAT0)) |
                      (inb((uint16_t)(g_base + SMB_HSTDAT1)) << 8));
    return true;
}

/* Is anything at this address? A quick-write addresses the device and sends
 * nothing -- the only probe that cannot have a side effect on a device that
 * happens to be there, which matters when the thing you might be poking is a
 * battery controller. */
bool smbus_probe(uint8_t addr) {
    if (!smbus_begin(addr, false, 0, PROTO_QUICK)) return false;
    outb((uint16_t)(g_base + SMB_HSTCNT), CNT_START | CNT_PROTO(PROTO_QUICK));
    return smbus_wait();
}

/* ---- bringing it up ------------------------------------------------------
 *
 * Two chipsets, differing only in where the I/O base lives. */
struct smbus_host {
    uint16_t vendor, device;
    uint8_t  base_reg;     /* PCI config offset of the I/O base       */
    uint8_t  enable_reg;   /* host configuration register             */
    uint8_t  enable_bit;
    const char *name;
};

static const struct smbus_host g_hosts[] = {
    /* PIIX4's SMBus shares a function with its power management, so the id is
     * the ACPI one and the base is at a vendor-specific offset rather than in
     * a BAR. */
    { 0x8086, 0x7113, 0x90, 0xD2, 0x01, "PIIX4" },
    /* ICH9 and everything since: its own function, base in BAR4. */
    { 0x8086, 0x2930, 0x20, 0x40, 0x01, "ICH9" },
};

bool smbus_init(void) {
    if (g_up) return true;
    uint32_t n = pci_devices_count();

    for (unsigned h = 0; h < sizeof g_hosts / sizeof g_hosts[0]; h++) {
        const struct smbus_host *hd = &g_hosts[h];
        for (uint32_t i = 0; i < n; i++) {
            const struct pci_device *d = pci_get_device(i);
            if (!d || d->vendor_id != hd->vendor || d->device_id != hd->device)
                continue;

            uint32_t base = pci_read32(d->bus, d->device, d->function, hd->base_reg);
            /* Bit 0 marks an I/O resource on both; the address is the rest. */
            uint16_t io = (uint16_t)(base & 0xFFFE);
            if (!io) continue;

            /* Turn the host on. Reading the enable back matters: a chipset
             * that refuses leaves a base address that looks perfectly valid
             * and answers nothing. */
            uint8_t cfg = pci_read8(d->bus, d->device, d->function, hd->enable_reg);
            if (!(cfg & hd->enable_bit)) {
                pci_write16(d->bus, d->device, d->function, hd->enable_reg,
                            (uint16_t)(cfg | hd->enable_bit));
                cfg = pci_read8(d->bus, d->device, d->function, hd->enable_reg);
                if (!(cfg & hd->enable_bit)) {
                    kprintf("smbus: %s host refused to enable\n", hd->name);
                    continue;
                }
            }

            g_base = io;
            g_name = hd->name;
            g_up = true;
            kprintf("smbus: %s host at I/O 0x%x\n", hd->name, (unsigned)io);
            return true;
        }
    }
    return false;
}

#else  /* !x86 */

/* No I/O ports on this architecture, and no MMIO I2C controller in the
 * machines this kernel boots on. Declared so callers need no #ifdef. */
bool smbus_init(void) { return false; }
bool smbus_probe(uint8_t a) { (void)a; return false; }
bool smbus_read_byte_data(uint8_t a, uint8_t c, uint8_t *o) { (void)a;(void)c;(void)o; return false; }
bool smbus_write_byte_data(uint8_t a, uint8_t c, uint8_t v) { (void)a;(void)c;(void)v; return false; }
bool smbus_read_word_data(uint8_t a, uint8_t c, uint16_t *o) { (void)a;(void)c;(void)o; return false; }
static const char *g_name = "none";
static bool g_up;

#endif

bool smbus_present(void) { return g_up; }
const char *smbus_host_name(void) { return g_name; }

/* ---- EDID over DDC -------------------------------------------------------
 *
 * A monitor answers at I2C address 0x50 with a 128-byte block describing
 * itself: who made it, its physical size, and every mode it can actually
 * display. It is how an OS knows to pick 2560x1600 rather than guessing, and
 * how it knows the panel is 13 inches rather than 27.
 *
 * Read a byte at a time with byte-data transactions. SMBus block reads cap at
 * 32 bytes and not every host implements them; 128 single-byte reads is slower
 * and works everywhere, and this happens once. */
bool smbus_read_edid(uint8_t *out128) {
    return smbus_read_edid_at(EDID_I2C_ADDR, out128);
}

bool smbus_read_edid_at(uint8_t addr, uint8_t *out128) {
    if (!smbus_present()) return false;
    for (int i = 0; i < 128; i++)
        if (!smbus_read_byte_data(addr, (uint8_t)i, &out128[i]))
            return false;

    /* THE HEADER IS FIXED and is the only way to tell a real EDID from a bus
     * that answered with whatever was floating. */
    static const uint8_t magic[8] = { 0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00 };
    if (memcmp(out128, magic, 8) != 0) return false;

    /* And the whole block sums to zero, mod 256. */
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++) sum = (uint8_t)(sum + out128[i]);
    return sum == 0;
}

/* Manufacturer + product, decoded. The three-letter code is five bits per
 * letter packed big-endian into two bytes, which is why it looks like nothing
 * until it is unpacked. */
void smbus_edid_identity(const uint8_t *edid, char vendor[4], uint16_t *product,
                         uint32_t *serial) {
    uint16_t m = (uint16_t)((edid[8] << 8) | edid[9]);
    vendor[0] = (char)('A' - 1 + ((m >> 10) & 0x1F));
    vendor[1] = (char)('A' - 1 + ((m >> 5) & 0x1F));
    vendor[2] = (char)('A' - 1 + (m & 0x1F));
    vendor[3] = 0;
    if (product) *product = (uint16_t)(edid[10] | (edid[11] << 8));
    if (serial)  *serial  = (uint32_t)edid[12] | ((uint32_t)edid[13] << 8) |
                            ((uint32_t)edid[14] << 16) | ((uint32_t)edid[15] << 24);
}

/* The preferred timing -- the panel's native mode, which is the first detailed
 * descriptor and the one a display actually wants to be driven at. */
bool smbus_edid_preferred_mode(const uint8_t *edid, uint32_t *w, uint32_t *h) {
    const uint8_t *d = edid + 54;
    uint32_t pixclk = (uint32_t)(d[0] | (d[1] << 8));
    if (pixclk == 0) return false;          /* not a timing descriptor */
    *w = (uint32_t)d[2] | (((uint32_t)d[4] & 0xF0) << 4);
    *h = (uint32_t)d[5] | (((uint32_t)d[7] & 0xF0) << 4);
    return *w != 0 && *h != 0;
}
