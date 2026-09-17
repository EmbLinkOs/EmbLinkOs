#include "drivers/timer/timer.h"
#include "arch/x86_64/irq/lapic.h"   /* timer_sched_ticks() */
#include "drivers/timer/hpet.h"
#include "drivers/timer/pit.h"
#include "arch/x86_64/irq/irq.h"
#include "include/kprintf.h"
#include "mm/kheap.h"
#include "acpi/acpi.h"
#include "include/io.h"
#include <stdint.h>

static volatile uint64_t ticks = 0;
volatile int heap_stress_enable = 0;

/* ---- raw TSC ---- */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- TSC calibration state ---- */
static uint64_t tsc_freq_hz  = 0;   /* TSC ticks per second (0 = not calibrated) */
static uint64_t tsc_base     = 0;   /* TSC at calibration point                  */
static bool     tsc_calibrated = false;
uint64_t time_get_ns(void);

/* ---- IRQ 0 handler (PIT / 18.2 Hz) ---- */
static void timer_handler(void)
{
    ticks++;
}

void timer_init(void)
{
    kprintf("=== Timer init ===\n");
    irq_register(0, timer_handler);
    kprintf("Timer initialized. IRQ0 handler registered.\n");
}

uint64_t timer_get_ticks(void)
{
    return ticks;
}

/* ---- TSC calibration ----
 *
 * HOW FAST IS THIS PROCESSOR'S CLOCK? Every delay, every timeout, every
 * sleep and the scheduler's tick all hang off the answer, so getting it wrong
 * is not a precision problem: a TSC believed to run at 2 MHz makes a
 * three-second USB timeout expire in six milliseconds.
 *
 * It used to be answered ONLY by timing ten milliseconds of HPET or 8254 PIT.
 * That is exactly the pair a recent laptop does not have: firmware commonly
 * hides the HPET, and Intel platforms clock-gate the PIT, which then never
 * finishes counting. Measured in QEMU with both switched off: "TSC: frequency
 * ~2 MHz", and a boot that only survived because the emulated port happened
 * to answer. On real silicon that wait does not end.
 *
 * So, in order of how much each can be trusted:
 *
 *   1. a hypervisor's timing leaf, which states the frequency outright;
 *   2. CPUID 0x15, where the processor states the TSC/crystal ratio and --
 *      on most parts -- the crystal (or 0x16 lets us derive it);
 *   3. measured against the HPET, then the ACPI PM timer, then the PIT --
 *      each measurement bounded, and each result thrown away unless it lands
 *      between 100 MHz and 20 GHz;
 *   4. CPUID 0x16's nominal frequency on its own;
 *   5. and, failing everything, 2 GHz and a warning that says so. */

#define TSC_MIN_HZ   100000000ULL
#define TSC_MAX_HZ 20000000000ULL

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static bool plausible(uint64_t hz) { return hz >= TSC_MIN_HZ && hz <= TSC_MAX_HZ; }

static uint64_t tsc_from_cpuid(const char **how) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t max_leaf = a;

    cpuid(1, 0, &a, &b, &c, &d);
    if (c & (1u << 31)) {                     /* running under a hypervisor */
        uint32_t hmax;
        cpuid(0x40000000, 0, &hmax, &b, &c, &d);
        if (hmax >= 0x40000010) {
            cpuid(0x40000010, 0, &a, &b, &c, &d);
            uint64_t hz = (uint64_t)a * 1000ULL;          /* EAX is kHz */
            if (plausible(hz)) { *how = "hypervisor timing leaf"; return hz; }
        }
    }

    if (max_leaf >= 0x15) {
        uint32_t den, num, crystal, x;
        cpuid(0x15, 0, &den, &num, &crystal, &x);
        if (den && num) {
            if (crystal) {
                uint64_t hz = (uint64_t)crystal * num / den;
                if (plausible(hz)) { *how = "CPUID 0x15 (crystal x ratio)"; return hz; }
            }
            if (max_leaf >= 0x16) {
                uint32_t mhz;
                cpuid(0x16, 0, &mhz, &b, &c, &d);
                if (mhz) {
                    uint64_t hz = (uint64_t)mhz * 1000000ULL;
                    if (plausible(hz)) { *how = "CPUID 0x15 ratio with 0x16's frequency"; return hz; }
                }
            }
        }
    }
    return 0;
}

static uint64_t tsc_nominal_cpuid(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a < 0x16) return 0;
    cpuid(0x16, 0, &a, &b, &c, &d);
    uint64_t hz = (uint64_t)a * 1000000ULL;
    return plausible(hz) ? hz : 0;
}

/* Time `window_ms` of the ACPI PM timer. Masked to 24 bits whatever the
 * counter's width: a 32-bit counter's low 24 bits wrap exactly like a 24-bit
 * one, and a 24-bit counter wraps every 4.7 s, far longer than the window. */
static uint64_t tsc_measure_pmtimer(uint16_t port, uint32_t window_ms) {
    const uint32_t want = (uint32_t)(3579545ULL * window_ms / 1000ULL);
    uint32_t p0 = inl(port) & 0xFFFFFF;
    /* Wait for the counter to MOVE first: an absent timer reads a constant,
     * and measuring against a constant is an infinite loop by another name. */
    uint32_t spins = 0;
    while (((inl(port) & 0xFFFFFF) == p0)) {
        if (++spins > 5000000) return 0;
    }
    p0 = inl(port) & 0xFFFFFF;
    uint64_t t0 = rdtsc();
    for (uint64_t guard = 0; guard < 200000000ULL; guard++) {
        uint32_t p = inl(port) & 0xFFFFFF;
        uint32_t elapsed = (p - p0) & 0xFFFFFF;
        if (elapsed >= want) {
            uint64_t t1 = rdtsc();
            return (t1 - t0) * 3579545ULL / elapsed;
        }
    }
    return 0;
}

void tsc_calibrate(void)
{
    if (tsc_calibrated)
        return;

    const uint32_t window_ms = 10;
    const char *how = 0;
    uint64_t hz = tsc_from_cpuid(&how);

    if (!hz && hpet_available()) {
        uint64_t t0 = rdtsc();
        hpet_delay_ms(window_ms);
        uint64_t t1 = rdtsc();
        uint64_t m = (t1 - t0) * (1000 / window_ms);
        if (plausible(m)) { hz = m; how = "measured against the HPET"; }
    }

    if (!hz) {
        const struct acpi_power_info *pw = acpi_power_info();
        if (pw && pw->pm_tmr_port) {
            /* A longer window than the HPET's: this counter is read through a
             * port, and the read itself is part of what is being timed. */
            uint64_t m = tsc_measure_pmtimer(pw->pm_tmr_port, 50);
            if (plausible(m)) { hz = m; how = "measured against the ACPI PM timer"; }
        }
    }

    if (!hz) {
        uint64_t t0 = rdtsc();
        bool ok = pit_delay_ms_checked(window_ms);
        uint64_t t1 = rdtsc();
        uint64_t m = (t1 - t0) * (1000 / window_ms);
        if (ok && plausible(m)) { hz = m; how = "measured against the 8254 PIT"; }
    }

    if (!hz) {
        hz = tsc_nominal_cpuid();
        if (hz) how = "CPUID 0x16 nominal frequency (nothing could be measured)";
    }

    if (!hz) {
        hz = 2000000000ULL;
        how = "GUESSED -- no clock on this machine could be read; every timeout is approximate";
    }

    tsc_freq_hz = hz;
    tsc_base    = rdtsc();
    tsc_calibrated = true;

    kprintf("TSC: %lu MHz, %s\n", (unsigned long)(tsc_freq_hz / 1000000ULL), how);
}

uint64_t tsc_read(void)
{
    return rdtsc();
}

uint64_t tsc_get_freq_hz(void)
{
    return tsc_freq_hz;
}

/* time_get_ns / time_get_us use the formula:
 *   elapsed_ticks = rdtsc() - tsc_base
 *   ns = elapsed_ticks * 1_000_000_000 / tsc_freq_hz
 *
 * To avoid 128-bit division, we split the multiplication:
 *   ns = (elapsed_ticks / tsc_freq_hz) * 1e9
 *      + (elapsed_ticks % tsc_freq_hz) * 1e9 / tsc_freq_hz
 *
 * For practical TSC frequencies (1–5 GHz) and elapsed times up to a few
 * hours, the first term dominates and the division stays in 64 bits. */

uint64_t time_get_ns(void)
{
    if (!tsc_calibrated || tsc_freq_hz == 0)
        return 0;

    uint64_t elapsed = rdtsc() - tsc_base;
    /* integer: elapsed * 1e9 / freq */
    uint64_t sec     = elapsed / tsc_freq_hz;
    uint64_t rem     = elapsed % tsc_freq_hz;
    return sec * 1000000000ULL + rem * 1000000000ULL / tsc_freq_hz;
}

uint64_t time_get_us(void)
{
    if (!tsc_calibrated || tsc_freq_hz == 0)
        return 0;

    uint64_t elapsed = rdtsc() - tsc_base;
    uint64_t sec     = elapsed / tsc_freq_hz;
    uint64_t rem     = elapsed % tsc_freq_hz;
    return sec * 1000000ULL + rem * 1000000ULL / tsc_freq_hz;
}
/* See timer.h. Kept here rather than in the syscall layer so the compositor can
 * time its window animations without reaching into ring-3 plumbing. */
uint64_t timer_uptime_ms(void) {
    if (hpet_available()) {
        uint64_t pf = hpet_period_fs();              /* femtoseconds per tick */
        if (pf) {
            uint64_t tpms = 1000000000000ULL / pf;   /* ticks per millisecond */
            if (tpms == 0) tpms = 1;
            return hpet_read_counter() / tpms;
        }
    }
    /* NO HPET: THE TSC, not the PIT's interrupt count. That count was the
     * fallback, and it was wrong twice over on the machines that reach it: it
     * counts IRQ 0 at the PIT's power-on 18.2 Hz while being returned as
     * milliseconds, and IRQ 0 is masked at the PIC anyway -- so on a laptop
     * with no HPET, uptime was ZERO for the life of the machine. Measured: the
     * panic screen of the emulated no-HPET laptop said "uptime ms 0x0", and
     * every one of the forty-six deadlines built on this function could wait
     * forever. The TSC is calibrated before anything that asks this. */
    if (tsc_calibrated && tsc_freq_hz)
        return time_get_ns() / 1000000ULL;
    return timer_get_ticks();
}

/* timer.h's scheduler clock. On x86 that is the LOCAL APIC timer, not the PIT
 * counter above -- see the header for why the distinction matters. */
uint64_t timer_sched_ticks(void) {
    return lapic_timer_get_ticks();
}

/* timer.h's portable delay. On x86 the PIT is the thing that can busy-wait a
 * known interval without an interrupt. */
void timer_delay_ms(uint32_t ms) {
    pit_delay_ms(ms);
}
