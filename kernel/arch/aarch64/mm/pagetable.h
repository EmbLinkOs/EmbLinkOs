#ifndef _AARCH64_PAGETABLE_H
#define _AARCH64_PAGETABLE_H

#include <stdint.h>
#include "include/types.h"

/* aarch64 translation tables -- docs/ARM64.md phase A2.
 *
 * boot.S builds an initial address space out of 1 GiB blocks before any
 * allocator exists. This file takes those same tables over once pmm can hand
 * out page frames, and manages them properly: four levels, 4 KiB pages, real
 * permissions, and splitting a block into finer entries when a mapping needs
 * to be more precise than 1 GiB.
 *
 * The x86 counterpart is kernel/mm/vmm.c. This is deliberately NOT written as
 * "the aarch64 half of vmm.c": docs/ARM64.md §2.3 says the shared interface is
 * derived from two working implementations, not invented from one, and vmm.c
 * has a lot of x86 in its bones (CR3, PML4 naming, the recursive-map habits).
 * The two get factored once this one is finished and A5 has shown what user
 * address spaces need.
 *
 * ARCHITECTURAL NOTE. The kernel/user split here is a hardware property, not a
 * convention: TTBR1_EL1 translates the top of the address space and TTBR0_EL1
 * the bottom, chosen by bit 63 of the address. So "is this a kernel pointer"
 * is decided by the CPU, switching address spaces touches only TTBR0, and the
 * kernel's mappings never have to be copied into each process -- three things
 * x86 has to arrange by hand in every PML4. */

/* Mapping attributes. Absent PT_WRITE means read-only; absent PT_EXEC means
 * the region is never executable (both PXN and UXN set). */
#define PT_READ     0x00u       /* implied; named so call sites read clearly */
#define PT_WRITE    0x01u
#define PT_EXEC     0x02u       /* kernel-executable (PXN clear)             */
#define PT_USER     0x04u       /* EL0 may access                            */
#define PT_DEVICE   0x08u       /* Device-nGnRnE instead of Normal cacheable */
#define PT_WC       0x10u       /* Normal NON-cacheable: the aarch64 answer to
                                 * x86 write-combining. Not Device memory --
                                 * Device forbids the unaligned and merged
                                 * writes a framebuffer blit depends on. */

#define PT_OK             0
#define PT_ERR_NOMEM     -1     /* out of page frames for a table            */
#define PT_ERR_ALIGN     -2     /* va/pa/size not 4 KiB aligned              */
#define PT_ERR_NOTMAPPED -3

/* Adopt the tables boot.S built and record the section boundaries. Call once,
 * after pmm_init(). */
void vm_init(void);

/* Map / unmap at 4 KiB granularity. Splits any block descriptor in the way. */
int vm_map_page(uint64_t va, uint64_t pa, uint32_t flags);
int vm_map_range(uint64_t va, uint64_t pa, uint64_t size, uint32_t flags);
int vm_unmap_page(uint64_t va);

/* Resolve `va` through the tables the way the hardware would. Returns the
 * physical address, or 0 if unmapped. Correct across block sizes, so it is
 * also the honest way to ask "is this actually mapped, and to what". */
uint64_t vm_translate(uint64_t va);

/* Re-map the kernel image at 4 KiB granularity with per-section permissions:
 * .text read-only + executable, .rodata read-only + never-executable,
 * .data/.bss read-write + never-executable.
 *
 * Until this runs, the whole kernel sits inside one 1 GiB RWX block -- which
 * is the only thing boot.S can build before there is an allocator, and is
 * exactly the state where a stray write can rewrite the kernel's own code. */
int vm_protect_kernel_sections(void);

/* Remove the TTBR0 identity map that existed only to survive enabling the MMU.
 *
 * Worth doing on its own account, not just tidiness: while it exists, every
 * physical address is also a valid kernel virtual address, so the read-only
 * .text established above has a writable alias, and a null pointer
 * dereference reads real memory instead of faulting. */
void vm_drop_identity_map(void);

/* Print the kernel's mappings and their permissions, resolved through an
 * actual table walk rather than from what we believe we asked for. */
void vm_dump_kernel_mapping(void);

#endif /* _AARCH64_PAGETABLE_H */
