/* kernel/lib/klog.c -- the last few pages of kernel output, kept.
 *
 * Every line this kernel has ever printed went to a serial port and, if the
 * framebuffer console was up, to a screen. Both are gone the moment the
 * machine stops: a person looking at a crashed laptop has no serial cable and
 * the screen is showing whatever was there before.
 *
 * So kprintf tees into a ring here, and the crash record (kernel/lib/crashlog.c)
 * carries the tail of it into storage that survives a power cycle. THE
 * REGISTER DUMP ALMOST NEVER TELLS YOU WHY -- it tells you where. What was
 * being printed in the moments before is what tells you why, and it is the
 * part that has always been thrown away.
 *
 * THERE IS NO LOCK, AND THAT IS DELIBERATE. This is written from kprintf,
 * which already holds its own lock for a whole line, and READ from the panic
 * path, where taking a lock somebody else might be holding is how a crash
 * report turns into a hang. A byte torn between two cores costs one garbled
 * character in a log; a deadlock while reporting a crash costs the report.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "lib/klog.h"

/* Eight kilobytes: roughly the last hundred lines, which is far enough back
 * to cover a driver's bring-up, and small enough to fit a crash record
 * alongside whatever else the platform's error store is holding. */
/* SIXTY-FOUR KILOBYTES, because the interesting part of a boot is the
 * BEGINNING. Eight kilobytes held the last few seconds, which is the right
 * size for a crash record and the wrong one for the question a real machine
 * asks: "what happened during USB enumeration", four hundred lines earlier.
 * On a laptop, with no serial port, this ring IS the boot log -- GUI+K puts it
 * back on the screen (kernel/main.c). The cost is .bss, and the kernel's fit
 * against its load address is measured at every build. */
#define KLOG_SIZE 65536

static char g_ring[KLOG_SIZE];
static uint32_t g_head;      /* where the next character goes */
static uint64_t g_total;     /* how many have ever been written */

void klog_putc(char c) {
    g_ring[g_head] = c;
    g_head = (g_head + 1) % KLOG_SIZE;
    g_total++;
}

/* Copy out the most recent `cap` bytes, oldest first, NUL-terminated.
 * Returns how many bytes of text (excluding the terminator) were written. */
uint32_t klog_tail(char *out, uint32_t cap) {
    if (!out || cap < 2) return 0;
    uint32_t have = g_total < KLOG_SIZE ? (uint32_t)g_total : KLOG_SIZE;
    uint32_t want = cap - 1;
    if (want > have) want = have;

    /* START `want` BYTES BACK FROM THE HEAD, wrapping. Reading forward from
     * the ring's index 0 would hand back the oldest surviving bytes in
     * whatever rotation the ring happens to be in, which reads as a log that
     * jumps backwards in the middle. */
    uint32_t start = (g_head + KLOG_SIZE - want) % KLOG_SIZE;
    for (uint32_t i = 0; i < want; i++)
        out[i] = g_ring[(start + i) % KLOG_SIZE];
    out[want] = '\0';
    return want;
}

uint64_t klog_bytes(void) { return g_total; }

/* ONE BYTE, COUNTED BACK FROM THE NEWEST: n == 1 is the last byte written,
 * n == klog_live() the oldest still held. 0 outside that range.
 *
 * Exists so the on-screen log viewer (GUI+K, kernel/main.c) can walk backwards
 * through a 64 KB ring looking for line breaks WITHOUT a second 64 KB buffer
 * to copy it into -- the kernel image has to fit under the address the
 * firmware loads it at, and a spare copy of the log is a sixteenth of the
 * budget. */
char klog_peek_back(uint32_t n) {
    uint32_t have = g_total < KLOG_SIZE ? (uint32_t)g_total : KLOG_SIZE;
    if (n == 0 || n > have) return 0;
    uint32_t idx = (g_head + KLOG_SIZE - n) % KLOG_SIZE;
    return g_ring[idx];
}

uint32_t klog_live(void) {
    return g_total < KLOG_SIZE ? (uint32_t)g_total : KLOG_SIZE;
}
