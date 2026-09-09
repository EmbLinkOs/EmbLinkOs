#include "arch/aarch64/cpu/percpu.h"
#include "include/kstring.h"

/* See percpu.h. One core until A9 brings up the rest through PSCI. */

struct cpu_data cpu_table[MAX_CPUS];
uint32_t cpu_count = 1;

void percpu_init_this_cpu(uint32_t index) {
    if (index >= MAX_CPUS)
        return;

    memset(&cpu_table[index], 0, sizeof(cpu_table[index]));
    cpu_table[index].cpu_index = index;
    cpu_table[index].online    = true;

    /* Point TPIDR_EL1 at this core's slot, so this_cpu() is one register read
     * from here on. Doing it per core is what makes the read correct once
     * there is more than one; doing it at all is what makes it fast. */
    __asm__ volatile("msr tpidr_el1, %0" :: "r"((uint64_t)(uintptr_t)&cpu_table[index]));
    __asm__ volatile("isb" ::: "memory");

    if (index + 1 > cpu_count)
        cpu_count = index + 1;
}
