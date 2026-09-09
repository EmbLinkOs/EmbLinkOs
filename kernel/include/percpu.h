#ifndef _PERCPU_H_
#define _PERCPU_H_

/* Per-CPU state, as much of it as portable code is allowed to know about.
 * docs/ARM64.md §2.3 -- the same dispatch shape as arch_irq.h and
 * arch_thread.h, for the same reason: `this_cpu()` is read on every scheduling
 * decision and must stay inline.
 *
 * EACH ARCHITECTURE DEFINES `struct cpu_data`, and it MUST contain at least
 * these fields, spelled exactly this way, because kernel/process/process.c
 * reads them by name:
 *
 *     uint32_t        cpu_index;              dense, 0-based, BSP is 0
 *     struct thread  *cur_thread;             what this core is running
 *     struct thread  *pending_thread_reap;    deferred teardown, see
 *     struct process *pending_process_reap;   schedule_locked()
 *     bool            online;
 *
 * and each must also provide `this_cpu()` returning a pointer to this core's
 * slot, `struct cpu_data cpu_table[MAX_CPUS]`, and `extern uint32_t cpu_count`
 * -- a VARIABLE, because that is what process.c already reads.
 *
 * Beyond that it is free to carry whatever the machine needs, and the two
 * differ substantially: x86 keeps a TSS, an APIC id and two 16 KiB interrupt
 * stacks per core in here, because a ring transition reads its stack pointer
 * out of a per-CPU table. aarch64 keeps none of that -- SP_EL1 is a register,
 * so there is nothing to look up (see arch_thread.h's
 * arch_kernel_stack_set()).
 *
 * That asymmetry is exactly why this is a per-arch struct with a shared field
 * CONTRACT rather than one shared struct with #ifdefs in it. */

#if defined(__x86_64__)
#include "arch/x86_64/cpu/percpu.h"
#elif defined(__aarch64__)
#include "arch/aarch64/cpu/percpu.h"
#else
#error "percpu.h: no per-CPU implementation for this architecture"
#endif

#endif /* _PERCPU_H_ */
