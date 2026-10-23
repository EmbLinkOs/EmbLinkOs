#include "drivers/timer/pit.h"
#include "include/io.h"
#include "include/types.h"
#include "drivers/timer/timer.h"

#define PIT_FREQUENCY 1193182  // PIT input clock frequency in Hz

// One one-shot cycle can count at most 0xFFFF ticks before the 16-bit
// counter wraps, which bounds a single cycle to ~54.9ms at this frequency.
#define PIT_MAX_COUNT       0xFFFFu
#define PIT_MAX_CYCLE_US    ((uint64_t)PIT_MAX_COUNT * 1000000ULL / PIT_FREQUENCY)

// PIT channel 2 is gated by bit 0 of port 0x61. We will use channel 2 in mode 0 (one-shot) to implement a microsecond delay function.
// can be read at bit 5 of port 0x61 (0 = ready, 1 = counting)

// Run one one-shot cycle of exactly `count` PIT ticks (count must be <=
// PIT_MAX_COUNT -- not re-checked here, only pit_delay_us below calls this
// and it already clamps every cycle to that bound).
static bool g_pit_dead;      /* it never finished a cycle: stop asking it */

static inline uint64_t pit_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void pit_oneshot_ticks(uint16_t count)
{
    // Enable channel 2 gate by setting bit 0 of port 0x61
    uint8_t port61 = inb(0x61);
    port61 |= (port61 & ~0x02) | 0x01; // Set bit 0 to enable gate
    outb(0x61, port61);

    // Command byte: channel 2, access mode lobyte/hibyte, mode 0 (one-shot), binary counting
    outb(0x43, 0xB0);
    outb(0x42, count & 0xFF);         // low byte
    outb(0x42, (count >> 8) & 0xFF);  // high byte

    // Restart the counter by toggling bit 0 of port 0x61
    port61 = inb(0x61) & ~0x01; // Clear bit 0 to disable gate
    outb(0x61, port61);
    port61 |= 0x01; // Set bit 0 to enable gate and start counting
    outb(0x61, port61);

    // Wait for the PIT to finish counting by polling bit 5 of port 0x61 --
    // BOUNDED. Recent Intel platforms clock-gate the 8254; its output then
    // never rises and this loop, unbounded, was the end of the boot. A cycle
    // is at most 55 ms, which is well inside the bound on any real machine.
    for (uint32_t spin = 0; !(inb(0x61) & 0x20); spin++) {
        if (spin > 3000000u) { g_pit_dead = true; return; }
    }
}

// Busywait for approximately `us` microseconds, looping one-shot cycles as
// needed. A single PIT one-shot cycle can only count up to 0xFFFF ticks
// (~54.9ms at this frequency, PIT_MAX_CYCLE_US above) -- `pit_delay_ms`
// used to silently CLAMP any request past that to one 0xFFFF-tick cycle
// instead of looping, so `pit_delay_ms(500)` actually delayed ~55ms, not
// 500ms, for every single caller (found while writing a selftest that
// needed a real multi-hundred-ms delay and got one that was 4x too short).
// This is the fix: run as many full-length cycles as fit, then one final
// shorter cycle for the remainder.
static void pit_delay_us(uint64_t us)
{
    while (us > 0) {
        uint64_t chunk_us = (us > PIT_MAX_CYCLE_US) ? PIT_MAX_CYCLE_US : us;
        uint32_t count = (uint32_t)(chunk_us * PIT_FREQUENCY / 1000000ULL);
        if (count == 0) count = 1;         // a nonzero remainder must still count at least 1 tick
        if (count > PIT_MAX_COUNT) count = PIT_MAX_COUNT;
        pit_oneshot_ticks((uint16_t)count);
        us -= chunk_us;
    }
}

/* ONCE THE TSC'S FREQUENCY IS KNOWN, IT DOES THE WAITING. This function has
 * forty-odd callers across the USB, SMP and storage code, all of which wanted
 * "busy-wait this long" and none of which cared that it was the PIT. A TSC
 * busy-wait cannot hang, is exact, and works on the machines whose PIT is
 * gated off -- so every one of those callers is fixed here instead of in
 * forty places. Before calibration the PIT is still used, bounded. */
static void delay_us(uint64_t us)
{
    uint64_t hz = tsc_get_freq_hz();
    if (hz) {
        uint64_t ticks = us * (hz / 1000000ULL);
        uint64_t t0 = pit_rdtsc();
        while (pit_rdtsc() - t0 < ticks)
            __asm__ volatile ("pause" ::: "memory");
        return;
    }
    if (g_pit_dead) return;
    pit_delay_us(us);
}

void pit_delay_ms(uint32_t ms)
{
    delay_us((uint64_t)ms * 1000ULL);
}

/* The PIT itself, for calibration: false if it never finished a cycle, so a
 * zero-length "10 ms" is not mistaken for a measurement. */
bool pit_delay_ms_checked(uint32_t ms)
{
    if (g_pit_dead) return false;
    pit_delay_us((uint64_t)ms * 1000ULL);
    return !g_pit_dead;
}
