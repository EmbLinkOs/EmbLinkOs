#ifndef __TIMER_H__
#define __TIMER_H__

#include <stdint.h>

/* Install the timer IRQ handler (IRQ0 / PIT keeps running at BIOS default). */
void timer_init(void);

#if defined(__aarch64__)
/* Bring THIS core's timer up: enable its PPI in its own redistributor and arm
 * its comparator. The generic timer's control and compare registers are banked
 * per core, so a secondary must do this for itself -- core 0 cannot do it on
 * its behalf. Global setup (frequency, the device-tree interrupt, the handler)
 * belongs to timer_init() and is done once. */
void timer_init_this_cpu(void);
void timer_arm_this_cpu(void);
#endif

/* Get the raw LAPIC 100-Hz tick count (used by the heartbeat loop). */
uint64_t timer_get_ticks(void);

/* Ticks of the timer that PREEMPTS -- the one the scheduler is driven by, and
 * the clock every scheduling timestamp (born_tick, exit_tick) is measured in.
 *
 * Not the same thing as timer_get_ticks() and the difference is not cosmetic:
 * on x86 those are two different devices. The legacy PIT drives IRQ0 and
 * timer_get_ticks(); the LOCAL APIC timer drives preemption and is per-CPU. A
 * scheduler timestamp taken from the wrong one is wrong by however much the
 * two have drifted. On aarch64 there is one generic timer doing both jobs, so
 * the two are equal -- which is exactly why this needed to be a named seam
 * rather than an assumption that they always are. */
uint64_t timer_sched_ticks(void);

/* Busy-wait for `ms` milliseconds. For device bring-up sequences that have to
 * observe a spec'd settle time before the scheduler exists (or while holding a
 * lock that forbids sleeping). Was `pit_delay_ms` at every call site, which
 * named an x86 device in code that only wanted a delay. */
void timer_delay_ms(uint32_t ms);
/* Monotonic milliseconds since boot (HPET where available, coarse ticks
 * otherwise). The clock both the ring-3 UI animator and the compositor's own
 * window motion run on. */
uint64_t timer_uptime_ms(void);

/*
 * TSC-based high-resolution time.
 *
 * tsc_calibrate() measures the TSC frequency against the HPET (preferred)
 * or the PIT (fallback) over a ~10 ms window.  Must be called once, after
 * hpet_init() and lapic_timer_init().  Subsequent calls are no-ops.
 *
 * time_get_ns() / time_get_us() return nanoseconds / microseconds elapsed
 * since tsc_calibrate() was called.  Both are lock-free; they execute a
 * single rdtsc instruction in the hot path.
 *
 * tsc_read() exposes the raw 64-bit TSC value for callers that need it
 * (e.g., the LAPIC timer can stamp the TSC at every tick for profiling).
 */
void     tsc_calibrate(void);
uint64_t tsc_read(void);
uint64_t time_get_ns(void);
uint64_t time_get_us(void);

/* Return the calibrated TSC frequency in Hz (0 before calibration). */
uint64_t tsc_get_freq_hz(void);

#endif /* __TIMER_H__ */