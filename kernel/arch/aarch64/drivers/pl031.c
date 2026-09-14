#include "drivers/timer/rtc.h"
#include "drivers/timer/timer.h"
#include "arch/aarch64/boot/fdt.h"
#include "mm/vmm.h"
#include "include/kprintf.h"

/* The ARM PrimeCell PL031 real-time clock -- drivers/timer/rtc.h on aarch64.
 *
 * A real driver rather than a stub, because QEMU `virt` really has one
 * (`/pl031@9010000` in the device tree) and the filesystem stamps every inode
 * with a wall-clock time. A machine whose files are all dated 1970 is a
 * machine whose backups sort wrongly.
 *
 * It is also almost the simplest possible device: one 32-bit register holding
 * seconds since the Unix epoch. x86 needs the CMOS RTC's BCD fields, a
 * century register and an update-in-progress dance to read the equivalent. */

#define RTC_DR 0x00     /* data: seconds since 1970, read-only */

static volatile uint8_t *pl031;
static bool tried;

/* Seconds and boot-relative nanoseconds are captured together so the two
 * cannot drift: rtc_now_ns() interpolates between whole seconds using the
 * monotonic counter, which the PL031 alone cannot provide. */
static uint64_t base_unix;
static uint64_t base_ns;

static void pl031_probe(void) {
    if (tried)
        return;
    tried = true;

    fdt_node_t n = fdt_find_compatible("arm,pl031");
    if (n == FDT_NONE) {
        kprintf("rtc: no PL031 in the device tree; wall clock unavailable\n");
        return;
    }

    uint64_t base = 0, size = 0;
    if (!fdt_reg(n, 0, &base, &size))
        return;

    uint64_t va = vmm_map_mmio(base, size ? size : 0x1000);
    if (!va)
        return;

    pl031 = (volatile uint8_t *)(uintptr_t)va;
    base_unix = *(volatile uint32_t *)(pl031 + RTC_DR);
    base_ns   = time_get_ns();

    kprintf("rtc: PL031 at %p, unix time %d\n",
            (void *)(uintptr_t)base, (int)base_unix);
}

/* HOW FAR THE HARDWARE CLOCK IS WRONG, as told to us by NTP or by a person.
 * The same design as x86's CMOS driver, and for the same reason: the offset is
 * one addition and cannot corrupt a clock, where writing the chip back can.
 * See kernel/drivers/timer/rtc.c for the argument in full.
 *
 * It matters more here, not less. The PL031 in QEMU `virt` has no battery
 * behind it and boards without one read zero -- 1970 -- so on aarch64 the
 * offset is frequently the only thing standing between the filesystem and a
 * tree of files dated the first of January 1970. */
static int64_t g_clock_offset;

static uint64_t pl031_read_hardware(void) {
    pl031_probe();
    if (!pl031)
        return 0;
    return *(volatile uint32_t *)(pl031 + RTC_DR);
}

uint64_t rtc_now_unix(void) {
    int64_t adjusted = (int64_t)pl031_read_hardware() + g_clock_offset;
    return adjusted < 0 ? 0 : (uint64_t)adjusted;
}

void rtc_set_unix(uint64_t unix_seconds) {
    g_clock_offset = (int64_t)unix_seconds - (int64_t)pl031_read_hardware();
}

int64_t rtc_offset(void) { return g_clock_offset; }

uint64_t rtc_hardware_unix(void) { return pl031_read_hardware(); }

uint64_t rtc_now_ns(void) {
    pl031_probe();
    if (!pl031)
        return 0;

    /* Whole seconds from the RTC, sub-second from the monotonic counter. The
     * PL031 has one-second resolution, and a filesystem that stamps a hundred
     * files in the same second with the same nanosecond value cannot order
     * them. */
    uint64_t now_ns = time_get_ns();
    int64_t  base = (int64_t)base_unix + g_clock_offset;
    if (base < 0) base = 0;
    return ((uint64_t)base * 1000000000ULL) + (now_ns - base_ns);
}
