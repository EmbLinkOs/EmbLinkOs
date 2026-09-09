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

uint64_t rtc_now_unix(void) {
    pl031_probe();
    if (!pl031)
        return 0;
    return *(volatile uint32_t *)(pl031 + RTC_DR);
}

uint64_t rtc_now_ns(void) {
    pl031_probe();
    if (!pl031)
        return 0;

    /* Whole seconds from the RTC, sub-second from the monotonic counter. The
     * PL031 has one-second resolution, and a filesystem that stamps a hundred
     * files in the same second with the same nanosecond value cannot order
     * them. */
    uint64_t now_ns = time_get_ns();
    return (base_unix * 1000000000ULL) + (now_ns - base_ns);
}
