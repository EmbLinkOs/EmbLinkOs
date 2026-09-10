#ifndef _POWER_H_
#define _POWER_H_

#include <stdint.h>
#include "include/types.h"

/* =====================================================================
 * POWER -- what the machine is running on, what it is spending, and how
 * it stops.
 *
 * A kernel that cannot turn the machine off is not finished, and one that
 * cannot say where its energy is going cannot be made to use less. Those are
 * two different problems and this file names both.
 *
 * WHY THIS IS A SUBSYSTEM AND NOT A FUNCTION. "Shut down" is the smallest
 * part of it. The parts that matter over a machine's life are:
 *
 *   - IDLE RESIDENCY. A core that is halted costs almost nothing; a core
 *     spinning in a scheduler loop costs everything. The difference is
 *     invisible unless it is measured, and unmeasured it silently regresses
 *     the first time somebody adds a poll loop. This kernel measures it per
 *     core, always, from the idle thread itself.
 *   - THE SOURCE. Running on a battery is not the same machine as running on
 *     mains: it justifies different scheduling, different device states,
 *     different writeback intervals. Nothing can act on that until something
 *     reports it, so this defines how a driver reports it.
 *   - TRANSITIONS. Off, reboot, and (eventually) suspend are one mechanism
 *     with one set of orderly steps -- flush the page cache, quiesce devices,
 *     then hand off to firmware -- not three ad-hoc code paths.
 *
 * WHAT IS HONEST ABOUT THE STATE OF IT. Shutdown and reboot are real on both
 * architectures. Idle residency is real and measured. A battery driver can
 * register and nothing currently does, because the hardware this runs on does
 * not have one -- power_source_present() answers false, which is a fact about
 * the machine rather than a stub. Suspend-to-RAM is not built; it is refused
 * by name rather than silently doing nothing.
 * ===================================================================== */

/* ---- where the energy comes from ---------------------------------------- */

enum power_source {
    POWER_SOURCE_UNKNOWN = 0,   /* nothing has reported                     */
    POWER_SOURCE_AC,            /* mains / external supply                  */
    POWER_SOURCE_BATTERY,       /* running down a battery                   */
};

struct power_supply_state {
    enum power_source source;
    bool     battery_present;
    uint8_t  percent;           /* 0-100, only meaningful with a battery    */
    bool     charging;
    int32_t  rate_mw;           /* + charging, - discharging, 0 unknown     */
    uint32_t design_mwh;        /* 0 = unknown                              */
    uint32_t remaining_mwh;     /* 0 = unknown                              */
};

/* A power-supply DRIVER. One function: report the current state. Registering
 * is how a platform says "this machine has a battery and here is how to read
 * it" -- ACPI's _BST on a laptop, an I2C fuel gauge on a board, a hypervisor
 * channel on a VM. The kernel above never learns which. */
struct power_supply_driver {
    const char *name;
    int (*poll)(struct power_supply_state *out);   /* 0, or -EMBK_* */
};

void power_supply_register(const struct power_supply_driver *drv);

/* The current state. Returns false when nothing has registered -- which is a
 * true statement about a machine with no battery, not a missing feature. */
bool power_supply_get(struct power_supply_state *out);

/* ---- what the machine is spending --------------------------------------- */

/* Called by the idle thread around its halt, on every core. These two are the
 * measurement: everything else about idle policy is derived from them. */
void power_idle_enter(void);
void power_idle_exit(void);

struct power_cpu_stats {
    uint64_t idle_ms;           /* time this core spent halted              */
    uint64_t uptime_ms;         /* time since this core started counting    */
    uint64_t idle_entries;      /* how many times it went to sleep          */
};

/* Per-core residency. `cpu` is the core index; false if it has none. */
bool power_cpu_stats(uint32_t cpu, struct power_cpu_stats *out);

/* System-wide idle percentage over all cores, 0-100. The one number to watch:
 * if it falls without the workload changing, something started polling. */
uint32_t power_idle_percent(void);

/* ---- transitions --------------------------------------------------------- */

enum power_transition {
    POWER_OFF = 0,
    POWER_REBOOT,
    POWER_SUSPEND,              /* not implemented; refused, not ignored     */
};

/* Take the machine down.
 *
 * The ORDERLY part happens here, once, for every transition and both
 * architectures: dirty pages are flushed to the device, because a page cache
 * that is lost on a clean shutdown makes the cache a bug rather than a
 * feature. Only then is the machine handed to firmware.
 *
 * This does not return on success. It returns a negative -EMBK_* if the
 * platform has no mechanism -- which is worth reporting rather than hanging,
 * because "the kernel could not power off this machine" is actionable and a
 * silent halt is not. */
int power_transition(enum power_transition what);

/* The architecture's half: hand off to firmware. Never returns on success.
 * Implemented once per architecture (PSCI on aarch64, ACPI/legacy on x86). */
int arch_power_off(void);
int arch_power_reboot(void);

/* Name the last reason a transition failed, for the shell. */
const char *power_last_error(void);

void power_init(void);

#endif /* _POWER_H_ */
