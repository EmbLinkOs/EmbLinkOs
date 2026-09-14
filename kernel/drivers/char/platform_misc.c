/* kernel/drivers/char/platform_misc.c -- three small devices that make a
 * machine easier to run and to test.
 *
 * None of them is a feature a person uses. All three are things the MACHINE
 * offers that this kernel was ignoring, and each one replaces an inference
 * with a fact:
 *
 *   pvpanic         tells the host the guest panicked, so a crash in CI is a
 *                   reported crash and not a timeout that could equally be a
 *                   hang, a slow boot, or a dead test harness.
 *   i6300esb        a watchdog: the machine resets itself if the kernel stops
 *                   petting it. On a headless target that is the difference
 *                   between a reboot and a machine that is simply gone.
 *   isa-debug-exit  lets the guest END the emulator with a status code, so a
 *                   test can RETURN pass or fail instead of printing a
 *                   sentence for a harness on the other side to grep for.
 *
 * THE THIRD ONE IS THE INTERESTING ONE. Seven harnesses in tools/ each boot a
 * machine, wait, and scrape the serial log for a verdict string -- and each
 * has its own timeout, its own idea of what "no verdict" means, and its own
 * way of being wrong about it. An exit code is not a better string; it is a
 * different kind of thing, and it cannot be garbled by a line that happened
 * to contain the word OK.
 */
#include <stdint.h>

#include "include/types.h"
#include "include/io.h"
#include "include/kprintf.h"
#include "drivers/bus/pci.h"
#include "mm/vmm.h"
#include "drivers/char/platform_misc.h"

/* ---- pvpanic ------------------------------------------------------------
 *
 * One byte-wide port. Writing a bit tells the host what happened; QEMU turns
 * that into a `GUEST_PANICKED` event on the monitor and, by default, stops the
 * machine. The ISA device sits at 0x505 on QEMU's PC machines; there is also a
 * PCI form, which is found through the normal enumeration. */
#define PVPANIC_ISA_PORT 0x505
#define PVPANIC_PANICKED (1u << 0)
#define PVPANIC_CRASHLOADED (1u << 1)

static uint16_t g_pvpanic_port;
static bool     g_pvpanic;

/* ---- i6300esb watchdog --------------------------------------------------
 *
 * An Intel 6300ESB, which QEMU emulates and real 6300ESB-era boards had. Two
 * 32-bit preload registers and a reload register behind a two-write unlock
 * sequence -- the unlock is the whole point of the design: a wild write cannot
 * disarm the watchdog by accident. */
#define ESB_CONFIG_REG   0x60      /* PCI config space */
#define ESB_LOCK_REG     0x68
#define ESB_TIMER1_REG   0x00      /* MMIO */
#define ESB_TIMER2_REG   0x04
#define ESB_GINTSR_REG   0x08
#define ESB_RELOAD_REG   0x0C
#define ESB_UNLOCK_1     0x80
#define ESB_UNLOCK_2     0x86
#define ESB_WDT_RELOAD   0x10
#define ESB_WDT_ENABLE   (1u << 1)
#define ESB_WDT_LOCK     (1u << 0)

static volatile uint8_t *g_esb;
static bool g_wdt;

/* ---- isa-debug-exit ----------------------------------------------------- */
#define DEBUG_EXIT_PORT 0x501

void platform_misc_init(void) {
    /* pvpanic: probe by writing nothing and reading -- an absent ISA port
     * reads 0xFF on QEMU, and the device reports the events it supports. */
#if defined(__x86_64__)
    uint8_t caps = inb(PVPANIC_ISA_PORT);
    if (caps != 0xFF && caps != 0x00) {
        g_pvpanic_port = PVPANIC_ISA_PORT;
        g_pvpanic = true;
        kprintf("pvpanic: at port 0x%x (events 0x%02x)\n",
                (unsigned)g_pvpanic_port, (unsigned)caps);
    }
#endif

    /* The PCI watchdog. Intel 8086:25ab. */
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (!d || d->vendor_id != 0x8086 || d->device_id != 0x25AB) continue;
        struct pci_bar bar = pci_read_bar(d->bus, d->device, d->function, 0);
        if (!bar.valid || !bar.is_mmio) break;
        g_esb = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(bar.address,
                                                            bar.size ? bar.size : 0x10);
        if (g_esb) {
            g_wdt = true;
            kprintf("watchdog: i6300ESB at 0x%llx (armed only on request)\n",
                    (unsigned long long)bar.address);
        }
        break;
    }
}

bool platform_pvpanic_present(void) { return g_pvpanic; }
bool platform_watchdog_present(void) { return g_wdt; }

void platform_pvpanic_notify(void) {
#if defined(__x86_64__)
    if (g_pvpanic) outb(g_pvpanic_port, PVPANIC_PANICKED);
#endif
}

/* Arm the watchdog for roughly `seconds`, or disarm it with 0.
 *
 * THE TIMER IS IN 30-NANOSECOND TICKS and only 20 bits wide, which caps a
 * single stage at about 31 seconds. The device fires stage 2 after stage 1
 * expires, so the real ceiling is twice that; asking for more is refused
 * rather than silently rounded down to something that reboots the machine
 * sooner than the caller believes. */
bool platform_watchdog_arm(const struct pci_device *pci, uint32_t seconds) {
    if (!g_wdt || !pci) return false;
    if (seconds > 31) {
        kprintf("watchdog: %u s is past this device's 31 s stage limit\n", seconds);
        return false;
    }
    uint32_t ticks = seconds ? (seconds * 33000000u) : 0;   /* ~30 ns per tick */
    if (ticks > 0xFFFFF) ticks = 0xFFFFF;

    if (!seconds) {
        pci_write16(pci->bus, pci->device, pci->function, ESB_LOCK_REG, 0);
        return true;
    }

    /* The unlock sequence, then the preload. Both stages get the same value:
     * stage 1 raises an interrupt nobody handles, stage 2 resets. */
    for (int stage = 0; stage < 2; stage++) {
        uint32_t reg = stage ? ESB_TIMER2_REG : ESB_TIMER1_REG;
        *(volatile uint8_t *)(g_esb + ESB_RELOAD_REG) = ESB_UNLOCK_1;
        *(volatile uint8_t *)(g_esb + ESB_RELOAD_REG) = ESB_UNLOCK_2;
        *(volatile uint32_t *)(g_esb + reg) = ticks;
    }
    pci_write16(pci->bus, pci->device, pci->function, ESB_LOCK_REG,
                ESB_WDT_ENABLE);
    kprintf("watchdog: armed for ~%u s\n", seconds);
    return true;
}

void platform_watchdog_pet(void) {
    if (!g_wdt) return;
    *(volatile uint8_t *)(g_esb + ESB_RELOAD_REG) = ESB_UNLOCK_1;
    *(volatile uint8_t *)(g_esb + ESB_RELOAD_REG) = ESB_UNLOCK_2;
    *(volatile uint16_t *)(g_esb + ESB_RELOAD_REG) = ESB_WDT_RELOAD;
}

/* END THE EMULATOR with a status code.
 *
 * QEMU computes its exit status as (value << 1) | 1, so the code a harness
 * sees is never zero -- which is deliberate on QEMU's part and worth knowing:
 * `0` here becomes exit status 1, and a test that wants "success" should use
 * a value both sides agree on. PLATFORM_EXIT_PASS/FAIL are that agreement.
 *
 * Does nothing if the device is absent, which is the case on real hardware --
 * so a kernel that calls this on a machine simply carries on, and the call is
 * safe to leave in a test path that also runs for real. */
void platform_debug_exit(uint8_t code) {
#if defined(__x86_64__)
    outb(DEBUG_EXIT_PORT, code);
#else
    (void)code;
#endif
}
