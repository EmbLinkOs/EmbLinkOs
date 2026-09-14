/* kernel/lib/crashlog.c -- why the machine stopped, readable after it starts
 * again.
 *
 * docs/PILLARS.md phase 3: "A problem on the real machine cannot be diagnosed
 * after the fact." Until now a kernel fault printed a register dump to a
 * serial port and halted. On a developer's desk that is enough. On the target
 * machine there is no serial cable, the screen holds whatever was on it, and
 * the next thing that happens is somebody pressing the power button -- after
 * which the only evidence of what went wrong is gone.
 *
 * kernel/acpi/erst.c already provides somewhere durable that does not involve
 * this operating system's own storage stack. This is what puts something in
 * it.
 *
 * THE PANIC PATH IS THE WHOLE DIFFICULTY, and every decision here comes from
 * it. By the time this runs the machine is known to be broken, one core is
 * halted mid-fault, and whatever invariant failed may be the one this code
 * would like to rely on. So:
 *
 *   - No allocation. The record is a static buffer sized at compile time.
 *   - No locks taken. The caller already holds the panic lock, which is what
 *     keeps two faulting cores from interleaving; nothing else is acquired,
 *     because a lock another core holds forever is a crash report that hangs
 *     instead of being written.
 *   - No page tables touched. erst.c maps its registers once at boot for
 *     exactly this reason.
 *   - Bounded everywhere. A loop that does not finish is worse here than a
 *     record that is short.
 *
 * WHAT GOES IN IT IS NOT JUST THE REGISTERS. A register dump says WHERE the
 * machine stopped and almost never why. The last few kilobytes of kernel log
 * -- which kernel/lib/klog.c has been keeping all along and which used to be
 * thrown away -- is the part that says what it was doing.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
/* WHERE A CRASH RECORD GOES IS A PLATFORM QUESTION, and only one platform in
 * this tree has an answer. ERST is an ACPI mechanism described by a table the
 * firmware writes; the aarch64 machine here boots from a device tree and has
 * no such table and no equivalent, so a fault there still prints to the
 * serial port and is still lost when the power goes. Said plainly rather than
 * left as a link error or a silent no-op: the gap is the platform's, and it
 * is in docs/TODO.md. */
#if defined(__x86_64__)
#include "acpi/erst.h"
#define CRASHLOG_HAVE_STORE 1
#else
#define CRASHLOG_HAVE_STORE 0
static inline bool erst_present(void) { return false; }
static inline uint64_t erst_record_count(void) { return 0; }
static inline uint64_t erst_first_record(void) { return ~0ULL; }
static inline uint64_t erst_next_record(void) { return ~0ULL; }
static inline int erst_read(uint64_t id, void *o, uint32_t c) { (void)id; (void)o; (void)c; return -1; }
static inline int erst_write(uint64_t id, const void *d, uint32_t l) { (void)id; (void)d; (void)l; return -1; }
static inline int erst_clear(uint64_t id) { (void)id; return -1; }
#define ERST_NO_RECORD (~0ULL)
#endif
#include "lib/klog.h"
#include "lib/ksym.h"
#include "lib/crashlog.h"

/* ONE SLOT, OVERWRITTEN. The platform's error store is small and the panic
 * path is not the place to manage a ring of records: choosing which old crash
 * to evict is bookkeeping, and bookkeeping on a broken machine is how a
 * report gets lost. The LAST crash is what a person needs; keeping a history
 * is in docs/TODO.md. */
#define CRASH_RECORD_ID 0x454D424B43520001ULL   /* "EMBKCR" and a slot number */

#define CPER_HEADER 128
#define CRASH_MAGIC "EMBLINK-CRASH1"
#define CRASH_LOG_BYTES 3072
/* How much of it the boot message shows. The rest is one command away. */
#define CRASH_BOOT_EXCERPT 640
#define CRASH_TOTAL (CPER_HEADER + (int)sizeof(struct crash_payload))

/* Our own payload, after the CPER header the platform insists on. Fixed
 * layout with no pointers: it is read back by a different boot of a kernel
 * that may not be the same build. */
struct crash_payload {
    char     magic[16];
    uint32_t version;
    uint32_t vector;
    uint64_t error_code;
    uint64_t rip, rsp, rbp, rflags, cr2, cr3;
    uint64_t cs, ss;
    uint32_t cpu;
    uint32_t pid;
    uint64_t uptime_ms;
    char     symbol[96];      /* the faulting address, already symbolised */
    char     log[CRASH_LOG_BYTES];
} __attribute__((packed));

static uint8_t g_record[CPER_HEADER + sizeof(struct crash_payload)];
static bool g_have_previous;
static struct crash_payload g_previous;

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void crashlog_capture(const struct crash_state *st) {
    if (!erst_present() || !st) return;

    /* Build the record in place. memset of a static buffer and a few stores;
     * nothing here can fail or wait. */
    memset(g_record, 0, sizeof g_record);
    uint8_t *h = g_record;
    h[0] = 'C'; h[1] = 'P'; h[2] = 'E'; h[3] = 'R';
    h[4] = 0x00; h[5] = 0x01;                          /* revision 1.0 */
    h[6] = h[7] = h[8] = h[9] = 0xFF;                  /* signature end */
    h[10] = 0; h[11] = 0;                              /* no CPER sections  */
    h[12] = 1;                                         /* severity: fatal   */
    put32(h + 20, (uint32_t)sizeof g_record);          /* record length     */
    for (int i = 0; i < 8; i++)
        h[96 + i] = (uint8_t)(CRASH_RECORD_ID >> (i * 8));

    struct crash_payload *p = (struct crash_payload *)(g_record + CPER_HEADER);
    memcpy(p->magic, CRASH_MAGIC, sizeof CRASH_MAGIC);
    p->version = 1;
    p->vector = st->vector;
    p->error_code = st->error_code;
    p->rip = st->rip; p->rsp = st->rsp; p->rbp = st->rbp;
    p->rflags = st->rflags; p->cr2 = st->cr2; p->cr3 = st->cr3;
    p->cs = st->cs; p->ss = st->ss;
    p->cpu = st->cpu;
    p->pid = st->pid;
    p->uptime_ms = st->uptime_ms;

    /* SYMBOLISE NOW, NOT LATER. The next boot may be a different build, and
     * an address without the image it came from is a number. ksym reads a
     * table already in memory and allocates nothing. */
    if (ksym_ready())
        ksym_symbolize(st->rip, p->symbol, sizeof p->symbol);
    else
        p->symbol[0] = '\0';

    klog_tail(p->log, sizeof p->log);

    /* And out. erst_write is register writes and a memcpy into a buffer
     * mapped at boot; it takes no lock and allocates nothing. */
    erst_write(CRASH_RECORD_ID, g_record, (uint32_t)sizeof g_record);
}

/* ---- the other side: reading it back on the next boot ------------------- */

bool crashlog_load_previous(void) {
    if (!erst_present()) return false;
    if (erst_record_count() == 0) return false;

    static uint8_t buf[CPER_HEADER + sizeof(struct crash_payload)];

    /* WALK THE RECORDS; DO NOT ASK FOR OURS BY NAME.
     *
     * Reading a specific identifier that is not there is a FAILED OPERATION,
     * and a failed operation leaves the platform's enumeration pointing
     * somewhere its next caller did not choose. That is not hypothetical: it
     * broke `make test-erst`, which ran afterwards and was told the store was
     * empty while the record sat in it -- a bug in this function that
     * appeared in a different one.
     *
     * Walking is also the only correct way to look. The store is shared with
     * whatever else writes to it -- the firmware, another operating system
     * that had this machine before -- so ours is found by its magic, not by
     * assuming an identifier is free. And the walk ends by handing back the
     * invalid identifier, which leaves the platform ready for the next
     * caller rather than mid-enumeration. */
    for (uint64_t id = erst_first_record(), guard = 0;
         id != ERST_NO_RECORD && guard < 64;
         id = erst_next_record(), guard++) {
        memset(buf, 0, sizeof buf);
        int n = erst_read(id, buf, sizeof buf);
        if (n < (int)(CPER_HEADER + sizeof(struct crash_payload))) continue;

        struct crash_payload *p = (struct crash_payload *)(buf + CPER_HEADER);
        if (memcmp(p->magic, CRASH_MAGIC, sizeof CRASH_MAGIC) != 0) continue;

        g_previous = *p;
        /* Whatever the record said, make sure the strings this kernel is
         * about to print end. A record written by a different build is data,
         * not trust. */
        g_previous.symbol[sizeof g_previous.symbol - 1] = '\0';
        g_previous.log[sizeof g_previous.log - 1] = '\0';
        g_have_previous = true;
        return true;
    }
    return false;
}

void crashlog_report(void) {
    if (!crashlog_load_previous()) return;

    kprintf("\n=== THE LAST BOOT ENDED IN A KERNEL FAULT ===\n");
    kprintf("crash: vector %u, error code %llx, after %llu ms of uptime\n",
            (unsigned)g_previous.vector,
            (unsigned long long)g_previous.error_code,
            (unsigned long long)g_previous.uptime_ms);
    kprintf("crash: RIP %llx  %s\n", (unsigned long long)g_previous.rip,
            g_previous.symbol[0] ? g_previous.symbol : "(not symbolised)");
    kprintf("crash: RSP %llx RBP %llx RFLAGS %llx CS %llx\n",
            (unsigned long long)g_previous.rsp,
            (unsigned long long)g_previous.rbp,
            (unsigned long long)g_previous.rflags,
            (unsigned long long)g_previous.cs);
    if (g_previous.vector == 14)
        kprintf("crash: CR2 %llx (the address it tried to touch)\n",
                (unsigned long long)g_previous.cr2);
    kprintf("crash: on cpu %u, pid %u\n", (unsigned)g_previous.cpu,
            (unsigned)g_previous.pid);
    /* THE TAIL AT BOOT, THE WHOLE THING ON REQUEST. Three kilobytes through a
     * serial port and a framebuffer console that scrolls by memmoving the
     * whole screen is seconds of every boot, every boot, forever -- for
     * something that is only interesting the first time somebody looks. The
     * last few lines are what says what it was doing; `test crashlog` prints
     * the rest. */
    uint32_t len = (uint32_t)strlen(g_previous.log);
    const char *tail = g_previous.log;
    if (len > CRASH_BOOT_EXCERPT) {
        tail = g_previous.log + (len - CRASH_BOOT_EXCERPT);
        /* Start at a line boundary, so the excerpt does not open mid-word. */
        while (*tail && *tail != '\n') tail++;
        if (*tail == '\n') tail++;
    }
    kprintf("crash: the last of the kernel log before it (%u bytes kept, "
            "`test crashlog` for all of it):\n--- 8< ---\n%s\n--- >8 ---\n",
            (unsigned)len, tail);
    kprintf("=== end of the previous crash ===\n\n");
}

bool crashlog_have_previous(void) { return g_have_previous; }

const char *crashlog_previous_symbol(void) {
    return g_have_previous ? g_previous.symbol : NULL;
}
uint32_t crashlog_previous_vector(void) {
    return g_have_previous ? g_previous.vector : 0;
}
const char *crashlog_previous_log(void) {
    return g_have_previous ? g_previous.log : NULL;
}

int crashlog_clear(void) {
    g_have_previous = false;
    return erst_clear(CRASH_RECORD_ID);
}
