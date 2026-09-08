#include "drivers/timer/timer.h"
#include "arch/aarch64/irq/gicv3.h"
#include "arch/aarch64/boot/fdt.h"
#include "arch/aarch64/sched/bringup.h"
#include "include/kprintf.h"

/* The ARM generic timer -- docs/ARM64.md phase A3.
 *
 * Implements kernel/drivers/timer/timer.h, the same header the x86 side does,
 * so shared code that asks for uptime or a monotonic clock does not care which
 * machine it is on.
 *
 * Three x86 devices collapse into one here. x86 needs the PIT to have a tick
 * at all, the HPET for a decent monotonic clock, and the TSC for cheap
 * high-resolution time -- and tsc_calibrate() exists because the TSC's
 * frequency is not knowable, only measurable, so the kernel spends 10 ms at
 * boot timing one clock against another. The ARM generic timer is one device
 * that does all three, and CNTFRQ_EL0 STATES its frequency. There is nothing
 * to calibrate, which is why tsc_calibrate() below is a no-op rather than a
 * stub: the measurement is genuinely unnecessary, not merely unwritten. */

#define TICK_HZ 100     /* matches the x86 LAPIC tick, so shared code that
                         * reasons in ticks means the same thing on both */

static uint64_t timer_freq;       /* Hz, from CNTFRQ_EL0                */
static uint64_t reload;           /* counter units per tick             */
static uint64_t ticks;
static uint64_t boot_count;       /* CNTVCT at init, so uptime starts 0 */
static uint32_t timer_intid;

/* The VIRTUAL timer (CNTV_*), not the physical one. Under HVF or KVM the
 * hypervisor owns the physical timer and a guest is expected to use the
 * virtual one; boot.S already zeroed CNTVOFF_EL2, so the two read the same
 * value here and choosing the virtual costs nothing under TCG either. */
static inline uint64_t cntvct(void) {
    uint64_t v;
    /* isb before the read: CNTVCT_EL0 is not ordered against other
     * instructions, so without it the compiler's chosen position and the
     * CPU's execution order can differ by an arbitrary amount. That is
     * invisible until someone times a short interval and gets a negative
     * delta. */
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}

static void timer_tick(uint32_t intid) {
    (void)intid;

    /* Re-arm first. CNTV_TVAL is a DOWN counter that fired at zero and keeps
     * going negative; the interrupt stays asserted until it is reloaded, so
     * doing this after the scheduler call below would mean an interrupt that
     * re-fires the instant we return -- a livelock that looks like the timer
     * running impossibly fast. */
    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"(reload));

    ticks++;
    bringup_sched_tick();
}

void timer_init(void) {
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(timer_freq));

    if (timer_freq == 0) {
        kprintf("timer: CNTFRQ_EL0 is zero -- firmware did not program it\n");
        return;
    }

    reload = timer_freq / TICK_HZ;
    boot_count = cntvct();

    /* Which interrupt line the timer is on comes from the device tree, not
     * from a table of remembered numbers. `virt` lists four in /timer --
     * secure physical, non-secure physical, VIRTUAL, hypervisor -- in that
     * order, so index 2 is the one that matches CNTV_* above. Hardcoding
     * "PPI 11" would work today and break on the next machine. */
    fdt_node_t node = fdt_find_compatible("arm,armv8-timer");
    if (node == FDT_NONE)
        node = fdt_find_compatible("arm,armv7-timer");

    uint32_t type = GIC_TYPE_PPI, num = 11, flags = 0;
    if (node != FDT_NONE && fdt_interrupt(node, 2, &type, &num, &flags)) {
        kprintf("timer: device tree says virtual timer is %s %d\n",
                type == GIC_TYPE_PPI ? "PPI" : "SPI", (int)num);
    } else {
        kprintf("timer: no usable /timer node; assuming PPI 11 (the ARM binding's virtual timer)\n");
    }
    timer_intid = gic_intid(type, num);

    gic_register(timer_intid, timer_tick, "generic timer");

    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"(reload));
    __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"((uint64_t)1));  /* ENABLE, unmasked */
    __asm__ volatile("isb" ::: "memory");

    kprintf("timer: %d Hz counter, %d Hz tick (reload %d), INTID %d\n",
            (int)timer_freq, TICK_HZ, (int)reload, (int)timer_intid);
}

uint64_t timer_get_ticks(void) { return ticks; }

uint64_t timer_uptime_ms(void) {
    if (!timer_freq)
        return 0;
    /* From the counter, not from the tick count: this stays right across a
     * missed or delayed interrupt, which a ticks*10 would not. */
    return ((cntvct() - boot_count) * 1000ULL) / timer_freq;
}

/* --- the timer.h high-resolution clock ------------------------------------
 * Named for x86's TSC because that is what the shared header calls them.
 * docs/TODO.md records the rename. */

void tsc_calibrate(void) {
    /* Nothing to do: CNTFRQ_EL0 is architecturally required to hold the
     * counter frequency. See the file comment -- this is an absence, not a
     * gap. */
}

uint64_t tsc_read(void) { return cntvct(); }

uint64_t tsc_get_freq_hz(void) { return timer_freq; }

uint64_t time_get_ns(void) {
    if (!timer_freq)
        return 0;
    uint64_t d = cntvct() - boot_count;
    /* Split the multiply so a 62.5 MHz counter does not overflow 64 bits after
     * a few minutes: d * 1e9 wraps at ~18 seconds of counts at 1 GHz. */
    return (d / timer_freq) * 1000000000ULL +
           ((d % timer_freq) * 1000000000ULL) / timer_freq;
}

uint64_t time_get_us(void) { return time_get_ns() / 1000ULL; }
