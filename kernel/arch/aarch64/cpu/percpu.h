#ifndef _AARCH64_PERCPU_H_
#define _AARCH64_PERCPU_H_

#include <stdint.h>
#include "include/types.h"

/* The aarch64 side of kernel/include/percpu.h. Include that, not this.
 *
 * Notably SHORTER than the x86 version, and not because anything is missing:
 * x86 stores a TSS, an APIC id and two 16 KiB interrupt stacks per core here
 * because a privilege transition fetches its stack pointer from a per-CPU
 * table. On aarch64 SP_EL1 is a register the exception mechanism uses
 * directly, so there is no table and nothing to keep in it. */

struct thread;
struct process;

#define MAX_CPUS 8      /* QEMU `virt` defaults to far fewer; raised when A9
                         * actually starts secondary CPUs via PSCI */

struct cpu_data {
    uint32_t        cpu_index;
    struct thread  *cur_thread;
    struct thread  *pending_thread_reap;
    struct process *pending_process_reap;
    bool            online;
};

extern struct cpu_data cpu_table[MAX_CPUS];

/* This core's slot.
 *
 * TPIDR_EL1 is the architectural per-CPU scratch register -- the aarch64
 * equivalent of x86 reading the APIC id, and cheaper, because it is a plain
 * register read rather than an MMIO access. It is set once per core during
 * bring-up; before that it is zero, which is cpu_table[0], which is the boot
 * core, which is correct. */
static inline struct cpu_data *this_cpu(void) {
    uint64_t p;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(p));
    return p ? (struct cpu_data *)(uintptr_t)p : &cpu_table[0];
}

/* Number of cores online. A VARIABLE, not a function, because that is what
 * kernel/process/process.c already reads and the shared contract in
 * kernel/include/percpu.h is whatever the existing code needs -- not whatever
 * looks tidier from this side. Getting that backwards is how a "portable"
 * interface ends up portable to exactly one caller. */
extern uint32_t cpu_count;

void percpu_init_this_cpu(uint32_t index);

#endif /* _AARCH64_PERCPU_H_ */
