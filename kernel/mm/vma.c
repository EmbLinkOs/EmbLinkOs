#include "mm/vma.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "process/process.h"
#include "include/errno.h"
#include "include/kmalloc.h"
#include "include/kprintf.h"
#include "include/kstring.h"

/* See mm/vma.h. */

static uint64_t page_align_up(uint64_t v) {
    return (v + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

/* prot -> the page-table flags both architectures understand.
 *
 * VMM_EXEC and VMM_NX are BOTH set explicitly, never left to a default: they
 * are read by different architectures (aarch64 reads the positive VMM_EXEC,
 * x86 the inverted VMM_NX), and silence means opposite things to them. That is
 * the rule vmm.h states, and it is why the kernel heap was W+X for years. */
static uint64_t prot_to_vmm(uint32_t prot) {
    uint64_t f = 0;

    /* PROT_NONE is the ABSENCE of VMM_USER, not a permission of its own. The
     * page stays mapped -- the frame is still allocated and still the
     * process's -- but neither ring 3 nor EL0 may touch it, so any access
     * faults. That is what makes a guard page a guard page.
     *
     * It works on both machines for the same reason: an x86 walk ANDs the
     * U/S bit down the levels, so a leaf without it is supervisor-only however
     * the tables above are set; and on aarch64 the AP field without PT_USER
     * encodes EL1-only. Note that on aarch64 this was NOT safe until nG stopped
     * being tied to PT_USER -- a kernel-only mapping in the user half used to
     * come out GLOBAL and match in every address space.
     *
     * What it does NOT stop is the kernel touching the page on the process's
     * behalf: copy_to_user walks the page tables for a frame, and finds one.
     * Recorded in docs/TODO.md rather than glossed. */
    if (prot != PROT_NONE)
        f |= VMM_USER;

    if (prot & PROT_WRITE) f |= VMM_WRITABLE;
    if (prot & PROT_EXEC)  f |= VMM_EXEC;
    else                   f |= VMM_NX;
    return f;
}

/* Find `len` bytes of free VA in the mmap window.
 *
 * A first-fit walk of a sorted list. Linear in the number of mappings, which
 * is the right complexity for a process with tens of them and the wrong one
 * for a process with thousands -- a tree is the answer then, and this is not
 * that yet. */
static uint64_t find_gap(struct process *proc, uint64_t len) {
    uint64_t cand = USER_MMAP_BASE;

    for (struct vm_area *v = proc->vma_list; v; v = v->next) {
        if (v->start >= cand + len)
            break;                      /* the gap before v is big enough */
        if (v->end > cand)
            cand = v->end;
    }
    if (cand + len > USER_MMAP_MAX || cand + len < cand)
        return 0;
    return cand;
}

static bool range_is_free(struct process *proc, uint64_t start, uint64_t end) {
    for (struct vm_area *v = proc->vma_list; v; v = v->next)
        if (start < v->end && v->start < end)
            return false;
    return true;
}

static void vma_insert(struct process *proc, struct vm_area *nv) {
    struct vm_area **pp = &proc->vma_list;
    while (*pp && (*pp)->start < nv->start)
        pp = &(*pp)->next;
    nv->next = *pp;
    *pp = nv;
}

/* Populate [start,end) with fresh zeroed frames. Returns false having undone
 * whatever it managed -- a half-mapped region is worse than a failed mmap,
 * because the caller has no way to describe it to munmap. */
static bool populate(struct process *proc, uint64_t start, uint64_t end,
                     uint64_t vmm_flags) {
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t pa = pmm_alloc_page();
        if (!pa)
            goto unwind;

        /* ZEROED, and that is a security property rather than a courtesy: a
         * fresh page carries whatever the last owner left in it, and handing
         * that to a different process is a disclosure. */
        memset((void *)(uintptr_t)P2V(pa), 0, PAGE_SIZE);

        if (vmm_map_in(proc->pml4_phys, va, pa, vmm_flags) != 0) {
            pmm_free_page(pa);
            goto unwind;
        }
    }
    return true;

unwind:
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t pa = vmm_get_phys_in(proc->pml4_phys, va);
        if (!pa)
            break;
        vmm_unmap_in(proc->pml4_phys, va);
        pmm_free_page(pa);
    }
    return false;
}

int64_t vma_mmap(struct process *proc, uint64_t addr, uint64_t len,
                 uint32_t prot, uint32_t flags) {
    if (!proc || len == 0)
        return -EMBK_EINVAL;

    len = page_align_up(len);
    if (len == 0 || len > (USER_MMAP_MAX - USER_MMAP_BASE))
        return -EMBK_EINVAL;

    /* Only anonymous private mappings exist. Refusing the rest is the whole
     * point: a MAP_SHARED that silently behaved as MAP_PRIVATE would be two
     * processes each believing they see the other's writes. */
    if (!(flags & MAP_ANONYMOUS) || !(flags & MAP_PRIVATE))
        return -EMBK_EINVAL;
    if (flags & MAP_SHARED)
        return -EMBK_EINVAL;

    /* W^X, refused rather than granted-and-regretted. A page that is both
     * writable and executable is the primitive every code-injection attack
     * needs, and nothing in this OS requires one -- the JIT-shaped caller that
     * eventually does should map W, write, then mprotect to X. (mprotect is
     * not implemented yet; docs/TODO.md.) */
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
        return -EMBK_EINVAL;

    uint64_t start;
    if (flags & MAP_FIXED) {
        if (addr & (PAGE_SIZE - 1))
            return -EMBK_EINVAL;
        if (addr < USER_MMAP_BASE || addr + len > USER_MMAP_MAX)
            return -EMBK_EINVAL;
        if (!range_is_free(proc, addr, addr + len))
            return -EMBK_EEXIST;   /* MAP_FIXED does NOT silently replace here */
        start = addr;
    } else {
        start = find_gap(proc, len);
        if (!start)
            return -EMBK_ENOMEM;
    }

    struct vm_area *v = kmalloc(sizeof *v);
    if (!v)
        return -EMBK_ENOMEM;

    if (!populate(proc, start, start + len, prot_to_vmm(prot))) {
        kfree(v);
        return -EMBK_ENOMEM;
    }

    v->start = start;
    v->end   = start + len;
    v->prot  = prot;
    v->flags = flags;
    vma_insert(proc, v);

    return (int64_t)start;
}

int vma_munmap(struct process *proc, uint64_t addr, uint64_t len) {
    if (!proc || len == 0 || (addr & (PAGE_SIZE - 1)))
        return -EMBK_EINVAL;

    len = page_align_up(len);
    uint64_t end = addr + len;
    if (end < addr)
        return -EMBK_EINVAL;

    struct vm_area **pp = &proc->vma_list;
    bool touched = false;

    while (*pp) {
        struct vm_area *v = *pp;
        if (v->end <= addr || v->start >= end) {   /* no overlap */
            pp = &v->next;
            continue;
        }

        uint64_t lo = v->start > addr ? v->start : addr;
        uint64_t hi = v->end   < end  ? v->end   : end;

        for (uint64_t va = lo; va < hi; va += PAGE_SIZE) {
            uint64_t pa = vmm_get_phys_in(proc->pml4_phys, va);
            if (pa) {
                vmm_unmap_in(proc->pml4_phys, va);
                pmm_free_page(pa);
            }
        }
        touched = true;

        if (lo == v->start && hi == v->end) {
            *pp = v->next;                  /* whole mapping gone */
            kfree(v);
            continue;
        }
        if (lo == v->start) { v->start = hi; pp = &v->next; continue; }
        if (hi == v->end)   { v->end   = lo; pp = &v->next; continue; }

        /* A hole in the middle: the mapping becomes two. */
        struct vm_area *tail = kmalloc(sizeof *tail);
        if (!tail) {
            /* The pages ARE gone; the bookkeeping cannot describe it. Truncate
             * rather than lie -- the tail leaks address space, not memory, and
             * saying so beats a VMA that claims to cover unmapped pages. */
            v->end = lo;
            kprintf("vma: out of memory splitting a mapping; %d KiB of VA leaked\n",
                    (int)((v->end - hi) / 1024));
            return EMBK_OK;
        }
        tail->start = hi;
        tail->end   = v->end;
        tail->prot  = v->prot;
        tail->flags = v->flags;
        v->end      = lo;
        tail->next  = v->next;
        v->next     = tail;
        pp = &tail->next;
    }

    return touched ? EMBK_OK : -EMBK_EINVAL;
}

/* Split the VMA containing `at` so that a mapping boundary falls exactly
 * there. Returns true if `at` is now a boundary (including the case where it
 * already was, or falls in no mapping at all). The page tables are NOT touched
 * -- this only changes how the range is described. */
static bool vma_split_at(struct process *proc, uint64_t at) {
    for (struct vm_area *v = proc->vma_list; v; v = v->next) {
        if (at <= v->start || at >= v->end)
            continue;
        struct vm_area *tail = kmalloc(sizeof *tail);
        if (!tail)
            return false;
        tail->start = at;
        tail->end   = v->end;
        tail->prot  = v->prot;
        tail->flags = v->flags;
        tail->next  = v->next;
        v->end      = at;
        v->next     = tail;
        return true;
    }
    return true;
}

int vma_mprotect(struct process *proc, uint64_t addr, uint64_t len, uint32_t prot) {
    if (!proc || len == 0 || (addr & (PAGE_SIZE - 1)))
        return -EMBK_EINVAL;

    len = page_align_up(len);
    uint64_t end = addr + len;
    if (end < addr)
        return -EMBK_EINVAL;

    if (prot & ~(uint32_t)(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EMBK_EINVAL;

    /* W^X, and this is the function that makes it a rule rather than an
     * inconvenience: a JIT maps writable, writes, then comes HERE to make the
     * page executable. What it may never do is hold both at once. */
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
        return -EMBK_EINVAL;

    /* EVERY page in the range must already be mapped -- POSIX says ENOMEM, and
     * the reason to enforce it is that the alternative is worse: silently
     * protecting the mapped part of a range would leave the caller believing a
     * guarantee it does not have over the rest. Checked BEFORE anything is
     * changed, so a refusal changes nothing. */
    uint64_t covered = addr;
    for (struct vm_area *v = proc->vma_list; v && covered < end; v = v->next) {
        if (v->end <= covered)
            continue;
        if (v->start > covered)
            break;                      /* a hole */
        covered = v->end;
    }
    if (covered < end)
        return -EMBK_ENOMEM;

    /* Cut the list at both ends so the range is a whole number of VMAs. Done
     * before the page tables are touched: a kmalloc failure here must leave
     * the process exactly as it was. */
    if (!vma_split_at(proc, addr) || !vma_split_at(proc, end))
        return -EMBK_ENOMEM;

    uint64_t vmm_flags = prot_to_vmm(prot);

    for (struct vm_area *v = proc->vma_list; v; v = v->next) {
        if (v->end <= addr || v->start >= end)
            continue;

        for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE)
            if (vmm_protect_in(proc->pml4_phys, va, vmm_flags) != 0)
                /* The VMA says the page is there and the page tables disagree.
                 * That is a kernel bug, not a caller error, and it is worth a
                 * line rather than a silent skip. */
                kprintf("vma: mprotect found no PTE for %p (VMA and page tables disagree)\n",
                        (void *)(uintptr_t)va);

        v->prot = prot;
    }

    return EMBK_OK;
}

void vma_destroy_all(struct process *proc) {
    if (!proc)
        return;
    struct vm_area *v = proc->vma_list;
    while (v) {
        struct vm_area *next = v->next;
        kfree(v);
        v = next;
    }
    proc->vma_list = 0;
}

uint64_t vma_total_bytes(struct process *proc) {
    uint64_t total = 0;
    for (struct vm_area *v = proc ? proc->vma_list : 0; v; v = v->next)
        total += v->end - v->start;
    return total;
}
