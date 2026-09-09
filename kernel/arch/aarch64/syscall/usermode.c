#include "arch/aarch64/syscall/usermode.h"
#include "arch/aarch64/cpu/kcontext.h"
#include "arch/aarch64/mm/pagetable.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"

/* Getting to EL0 and back -- docs/ARM64.md phase A5. See usermode.h. */

/* Where the probe's code and stack are mapped. Both are ordinary user virtual
 * addresses in the TTBR0 half; nothing else lives there, because the boot
 * identity map was removed at the end of A2 and TTBR0 has been empty since. */
#define USER_CODE_VA     0x0000000000400000ULL
#define USER_STACK_TOP   0x0000000000800000ULL
#define USER_STACK_PAGES 4

extern const unsigned char el0_probe_blob[];
extern const unsigned char el0_probe_blob_end[];

/* Where to come back to when the program exits. Same trick x86 uses
 * (g_user_exit_ctx in usermode.c): a setjmp/longjmp pair standing in for
 * "return here", because exit() cannot return through the syscall it made. */
static struct kcontext exit_ctx;
static int64_t exit_code;
static bool in_user;
static uint64_t probe_as;      /* the program's own address space */
static uint64_t free_before;   /* physical pages free before it was built */

/* Assembly, because the transition cannot be expressed in C: it ends in `eret`
 * and never returns to its caller. kcontext.S. */
#include "include/arch_thread.h"   /* arch_enter_user_mode() */

/* Called by the bring-up sys_exit. Does not return. */
void el0_exit(int64_t code) {
    exit_code = code;
    in_user = false;
    kernel_ctx_restore(&exit_ctx, 1);
}

bool el0_in_user_mode(void) { return in_user; }

/* Map `pages` fresh zeroed frames at `va` with `flags`. Returns false on any
 * failure, having said which -- a half-built address space that is then
 * entered is a fault at an address with no obvious relation to the cause. */
static bool map_user_pages(uint64_t va, unsigned pages, uint32_t flags,
                           const char *what) {
    for (unsigned i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            kprintf("el0: out of memory mapping %s\n", what);
            return false;
        }
        memset((void *)(uintptr_t)P2V(phys), 0, PAGE_SIZE);

        int rc = vm_map_page(va + (uint64_t)i * PAGE_SIZE, phys, flags);
        if (rc != PT_OK) {
            kprintf("el0: vm_map_page(%p) for %s failed (%d)\n",
                    (void *)(uintptr_t)(va + i * PAGE_SIZE), what, rc);
            return false;
        }
    }
    return true;
}

int64_t el0_probe_run(void) {
    uint64_t size = (uint64_t)(el0_probe_blob_end - el0_probe_blob);
    uint64_t code_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Its OWN address space, not the boot one. A5 mapped the program into the
     * global TTBR0 root because there was nothing else; there is now, and
     * using it means the program's mappings vanish with it rather than
     * accumulating in a table nothing owns.
     *
     * Switching is one register write and cannot disturb the kernel: kernel
     * mappings live in TTBR1 and are never part of a process address space --
     * the thing x86 has to arrange by copying the top half of every PML4. */
    free_before = pmm_free_pages();
    probe_as = vmm_create_address_space();
    if (!probe_as) {
        kprintf("el0: cannot create an address space\n");
        return -EMBK_ENOMEM;
    }
    vmm_switch_address_space(probe_as);

    kprintf("el0: probe is %d bytes; mapping code at %p, stack at %p\n",
            (int)size, (void *)USER_CODE_VA,
            (void *)(USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE));

    /* Code: readable and executable by EL0, and NOT writable. The kernel
     * writes it before the mapping is made read-only... which it cannot do
     * through a read-only mapping, so the copy goes through the DIRECT MAP
     * instead -- the kernel's own alias of the same physical pages. That is
     * what a direct map is for, and it is why user text never has to be
     * temporarily writable. */
    if (!map_user_pages(USER_CODE_VA, (unsigned)code_pages,
                        PT_USER | PT_EXEC, "probe text"))
        return -EMBK_ENOMEM;

    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t phys = vm_translate(USER_CODE_VA + off);
        uint64_t n = size - off < PAGE_SIZE ? size - off : PAGE_SIZE;
        memcpy((void *)(uintptr_t)P2V(phys), el0_probe_blob + off, n);
    }

    /* The instruction cache does not snoop the data cache on aarch64. We just
     * wrote code through a data mapping; without pushing it out to the point
     * of unification and invalidating I-cache, EL0 can fetch stale bytes --
     * which on a cold page means executing whatever was there before. This is
     * a class of bug x86 simply does not have (it has coherent I-caches), and
     * it appears as a random undefined-instruction fault at the entry point. */
    __asm__ volatile("dsb ish; ic iallu; dsb ish; isb" ::: "memory");

    /* Stack: writable, and never executable. */
    if (!map_user_pages(USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE,
                        USER_STACK_PAGES, PT_USER | PT_WRITE, "probe stack"))
        return -EMBK_ENOMEM;

    exit_code = -EMBK_EINVAL;

    /* The return point. kernel_ctx_save returns 0 now and (nonzero) later,
     * when el0_exit longjmps back into it. */
    if (kernel_ctx_save(&exit_ctx) != 0) {
        kprintf("el0: back in EL1, program exited with %d\n", (int)exit_code);

        /* Tear the whole thing down: every frame the program touched is
         * reachable from its root and nothing else is, so this frees the code,
         * the stack and the page tables in one walk. */
        vmm_switch_address_space(vmm_get_kernel_pml4());
        vmm_destroy_address_space(probe_as);
        probe_as = 0;

        /* The number is the point. Destroying an address space walks its
         * tables and frees both the page-table pages and the frames they
         * point at; if the walk misses a level, this comes back short and says
         * so, where "destroyed" would have looked like success. */
        uint64_t after = pmm_free_pages();
        kprintf("el0: address space destroyed -- free pages %d -> %d%s\n",
                (int)free_before, (int)after,
                after == free_before ? " (all reclaimed)"
                                     : " *** LEAKED, see below ***");
        if (after != free_before)
            kprintf("el0: %d pages LEAKED by the teardown walk\n",
                    (int)(free_before - after));
        return exit_code;
    }

    in_user = true;
    kprintf("el0: eret to %p, sp %p\n",
            (void *)USER_CODE_VA, (void *)USER_STACK_TOP);

    arch_enter_user_mode(USER_CODE_VA, USER_STACK_TOP, 0, 0, 0);
    return -EMBK_EINVAL;   /* not reached */
}
