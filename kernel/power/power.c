#include "drivers/storage/nvme.h"
#include "block/block.h"
#include "power/power.h"
#include "include/errno.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/spinlock.h"
#include "drivers/timer/timer.h"
#include "mm/vm_object.h"
#include "process/process.h"
#include "include/percpu.h"     /* this_cpu()->cpu_index */

/* See power/power.h. */

#define POWER_MAX_CPUS 64

/* Per-core idle accounting. Deliberately NOT under a lock: each core writes
 * only its own slot, from its own idle thread, and a reader that catches a
 * counter mid-update is off by one sample of a millisecond clock. A lock here
 * would be taken on the hottest possible path -- every entry into and exit
 * from idle -- to protect a number whose whole purpose is to be approximate. */
struct cpu_idle {
    uint64_t enter_ms;
    uint64_t idle_ms;
    uint64_t entries;
    uint64_t first_ms;      /* when this core started counting */
    uint64_t timer_irqs;
    bool     in_idle;
};
static struct cpu_idle g_idle[POWER_MAX_CPUS];

static const struct power_supply_driver *g_supply = NULL;
static const char *g_last_error = "";

void power_supply_register(const struct power_supply_driver *drv) {
    if (!drv || !drv->poll)
        return;
    /* First one wins. A second registration means two drivers each believe
     * they own the battery, which is a platform bug worth naming rather than
     * resolving by silently preferring one. */
    if (g_supply) {
        kprintf("power: %s ignored -- %s already owns the supply\n",
                drv->name, g_supply->name);
        return;
    }
    g_supply = drv;
    kprintf("power: supply driver '%s' registered\n", drv->name);
}

bool power_supply_get(struct power_supply_state *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof *out);
    if (!g_supply)
        return false;              /* no battery on this machine -- a fact */
    if (g_supply->poll(out) != EMBK_OK)
        return false;
    return true;
}

/* ---- idle residency ------------------------------------------------------ */

void power_idle_enter(void) {
    uint32_t c = this_cpu()->cpu_index;
    if (c >= POWER_MAX_CPUS)
        return;
    struct cpu_idle *s = &g_idle[c];
    if (!s->first_ms)
        s->first_ms = timer_uptime_ms();
    s->enter_ms = timer_uptime_ms();
    s->in_idle = true;
    s->entries++;

    /* The dispatched thread stops being charged for CPU here -- it is about to
     * do nothing. See sched_account_pause(). */
    sched_account_pause();
}

void power_idle_exit(void) {
    uint32_t c = this_cpu()->cpu_index;
    if (c >= POWER_MAX_CPUS)
        return;
    struct cpu_idle *s = &g_idle[c];
    if (!s->in_idle)
        return;
    uint64_t now = timer_uptime_ms();
    if (now > s->enter_ms)
        s->idle_ms += now - s->enter_ms;
    s->in_idle = false;

    sched_account_resume();
}

void power_timer_tick(void) {
    uint32_t c = this_cpu()->cpu_index;
    if (c < POWER_MAX_CPUS)
        g_idle[c].timer_irqs++;
}

bool power_cpu_stats(uint32_t cpu, struct power_cpu_stats *out) {
    if (!out || cpu >= POWER_MAX_CPUS)
        return false;
    struct cpu_idle *s = &g_idle[cpu];
    if (!s->first_ms)
        return false;              /* this core never idled: not online */

    uint64_t now = timer_uptime_ms();
    out->idle_ms = s->idle_ms;
    /* A core sitting in idle RIGHT NOW has time not yet added to the total.
     * Leaving it out makes a fully idle machine read 0%, which is the exact
     * opposite of the truth and the first thing anyone would notice. */
    if (s->in_idle && now > s->enter_ms)
        out->idle_ms += now - s->enter_ms;
    out->uptime_ms = now > s->first_ms ? now - s->first_ms : 0;
    out->idle_entries = s->entries;
    out->timer_irqs = s->timer_irqs;
    return true;
}

uint32_t power_idle_percent(void) {
    uint64_t idle = 0, total = 0;
    for (uint32_t c = 0; c < POWER_MAX_CPUS; c++) {
        struct power_cpu_stats s;
        if (!power_cpu_stats(c, &s))
            continue;
        idle += s.idle_ms;
        total += s.uptime_ms;
    }
    if (!total)
        return 0;
    if (idle > total)
        idle = total;              /* clock skew between cores, not a bug */
    return (uint32_t)((idle * 100) / total);
}

/* ---- transitions --------------------------------------------------------- */

const char *power_last_error(void) { return g_last_error; }

int power_transition(enum power_transition what) {
    if (what == POWER_SUSPEND) {
        /* Refused BY NAME. Suspend-to-RAM needs every driver to save and
         * restore its own state, and a driver that silently does not is a
         * machine that wakes up with a dead disk. Pretending to suspend by
         * halting would be the worst of both. */
        g_last_error = "suspend-to-RAM is not implemented";
        return -EMBK_ENOSYS;
    }

    /* THE ORDERLY PART, and it is the reason this is one function rather than
     * a call to the firmware at each site. write() now returns before the
     * bytes reach the device (mm/vm_object.h); a shutdown that skipped this
     * would lose every recently written file, and would make the page cache a
     * bug rather than a feature. */
    uint64_t pages = vmo_writeback_all();
    kprintf("power: flushed %llu dirty page(s) before %s\n",
            (unsigned long long)pages,
            what == POWER_REBOOT ? "reboot" : "power off");

    /* THE DEVICES' OWN CACHES, which the line above does not reach. Writeback
     * moves dirty pages to the disk driver; a drive with a volatile write cache
     * reports those writes complete while they are still in its RAM. A FLUSH
     * to every block device is what makes them durable -- the same barrier the
     * filesystem uses at commit, applied once more to everything, because the
     * power is about to stop being there to finish anything. */
    int nflushed = 0;
    for (uint32_t i = 0; i < embk_block_count(); i++) {
        struct embk_block_device *d = embk_block_get(i);
        if (d && d->flush && embk_block_flush(d) == EMBK_OK)
            nflushed++;
    }
    kprintf("power: %d block device cache(s) flushed\n", nflushed);

    /* Then tell NVMe drives the power is going, so they close out cleanly
     * rather than count an unsafe shutdown and recover on the next boot. */
    nvme_shutdown_all();

    int rc = (what == POWER_REBOOT) ? arch_power_reboot() : arch_power_off();

    /* STILL HERE, so the machine is still running -- and its NVMe drives have
     * just been told it is not. Bring them back before anything touches a
     * file: the alternative is a live system whose disk answers every request
     * with an error, which is worse than the power-off failing at all. */
    nvme_restart_all();

    /* Reaching here means the firmware did not take the machine. Say so: "the
     * kernel could not power off this hardware" is actionable, and a silent
     * hang is the same symptom as a crash. */
    g_last_error = (what == POWER_REBOOT)
                     ? "no reboot mechanism this platform answers to"
                     : "no power-off mechanism this platform answers to";
    return rc < 0 ? rc : -EMBK_ENODEV;
}

void power_init(void) {
    /* Deliberately NOT memset. The secondaries have been halting in ap_main
     * since before this ran, and their counts are real; zeroing here would
     * throw away the only measurement of the window between a core coming up
     * and the rest of the kernel being ready. The statics start at zero on
     * their own. */
    kprintf("power: idle accounting armed; supply: %s\n",
            g_supply ? g_supply->name : "none reported (no battery on this machine)");
}
