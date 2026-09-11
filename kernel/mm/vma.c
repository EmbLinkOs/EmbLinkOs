#include "mm/vma.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "process/process.h"
#include "include/errno.h"
#include "include/kmalloc.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/spinlock.h"
#include "mm/vm_object.h"
#include "drivers/timer/timer.h"   /* time_get_ns: the fault path times itself */

/* See mm/vma.h. */

/* THE VMA LIST NEEDS A LOCK NOW, and did not before.
 *
 * While mmap was eager, the list was only touched by mmap/munmap/mprotect --
 * syscalls, on the calling thread, rare. Demand paging made a THIRD reader:
 * the page-fault handler, which now runs on every core, on every first touch
 * of every page. A walk of the list racing a munmap that is freeing its nodes
 * is a use-after-free, and with faults this frequent it stops being a
 * theoretical race and becomes a boot that dies on four cores while passing on
 * one -- which is exactly how it was found.
 *
 * A SPINLOCK, not the sleeping mutex the fd layer uses: this is taken inside a
 * fault handler, and a handler that can sleep is a handler that can be
 * preempted mid-fault by something that faults again.
 *
 * ONE GLOBAL LOCK rather than one per process, which is the wrong long-term
 * answer and the right first one -- every fault on every core serialises here.
 * Per-process is the shape this wants, and the moment to build it is when a
 * profile shows the contention rather than when it sounds better. Recorded in
 * docs/TODO.md.
 *
 * LOCK ORDER: vma_lock -> pmm_lock / vmm_lock. Never the reverse. vm_fault and
 * vma_munmap both take this and then call into the page-table and frame
 * allocators, which take their own; nothing in those calls back into here. */
static spinlock_t vma_lock = SPINLOCK_INIT;

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
    uint64_t cand = proc->layout.mmap_base;      /* this process's window */

    for (struct vm_area *v = proc->vma_list; v; v = v->next) {
        if (v->start >= cand + len)
            break;                      /* the gap before v is big enough */
        if (v->end > cand)
            cand = v->end;
    }
    if (cand + len > proc->layout.mmap_max || cand + len < cand)
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

/* Which VMA contains `addr`, or NULL. The list is sorted, so this stops early
 * on a miss rather than walking every mapping. */
static struct vm_area *vma_find(struct process *proc, uint64_t addr) {
    for (struct vm_area *v = proc->vma_list; v; v = v->next) {
        if (addr < v->start) return NULL;      /* sorted: no later one can match */
        if (addr < v->end)   return v;
    }
    return NULL;
}

static struct vm_fault_stats g_fault_stats;

void vm_fault_stats(struct vm_fault_stats *out) {
    if (out) *out = g_fault_stats;
}

static bool vm_fault_resolve(struct process *proc, uint64_t addr, bool write, bool exec);

bool vm_fault(struct process *proc, uint64_t addr, bool write, bool exec) {
    uint64_t t0 = time_get_ns();
    bool r = vm_fault_resolve(proc, addr, write, exec);
    g_fault_stats.ns += time_get_ns() - t0;
    return r;
}

static bool vm_fault_resolve(struct process *proc, uint64_t addr, bool write, bool exec) {
    if (!proc)
        return false;

    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);

    /* Held across the whole decision AND the mapping. Not just the lookup:
     * two cores faulting on the same page would otherwise both find it absent,
     * both allocate a frame, and both map it -- the second silently leaking
     * the first. Check-and-map has to be one step. */
    spin_lock(&vma_lock);

    struct vm_area *v = vma_find(proc, page);
    if (!v) {
        /* Not inside any mapping this process asked for. This is the wild
         * pointer, the null dereference, the stack that ran off its guard
         * page -- and it is the case that MUST stay fatal. A kernel that
         * quietly mapped a page here would turn every use of an uninitialised
         * pointer into silent corruption instead of a crash. */
        g_fault_stats.declined++;
        spin_unlock(&vma_lock);
        return false;
    }

    /* PERMISSIONS ARE CHECKED AGAINST THE VMA, NOT INFERRED FROM THE FAULT.
     * A store to a PROT_READ page is not a missing page; it is the program
     * being told no. Resolving it by mapping something writable would hand
     * over exactly the access mmap refused, and would do it silently. */
#define VMF_DECLINE() do { g_fault_stats.declined++; spin_unlock(&vma_lock); return false; } while (0)
    if (write && !(v->prot & PROT_WRITE)) VMF_DECLINE();
    if (exec  && !(v->prot & PROT_EXEC))  VMF_DECLINE();
    if (v->prot == PROT_NONE)             VMF_DECLINE();

    uint64_t already = vmm_get_phys_in(proc->pml4_phys, page);
    uint64_t idx = (v->file_off + (page - v->start)) / PAGE_SIZE;

    /* --- COPY ON WRITE ------------------------------------------------------
     *
     * A write fault on a page that IS mapped, inside a private file mapping,
     * means exactly one thing: this is the shared read-only page from the
     * cache, and the process is about to change its own copy of the file.
     *
     * It cannot be anything else. A private page this process already copied
     * would be mapped WRITABLE, so a write to it would not fault at all --
     * which is what makes "mapped, private, write fault" an unambiguous
     * signal rather than a guess.
     *
     * This is what makes MAP_PRIVATE of a file worth having: N processes
     * reading one file share its pages, and only a writer pays for a copy. */
    if (already && write && v->obj && !v->obj->anon && (v->flags & MAP_PRIVATE)) {
        uint64_t shared = vmo_page_phys(v->obj, idx);
        if (already == shared) {
            uint64_t copy = pmm_alloc_page();
            if (!copy) VMF_DECLINE();
            memcpy((void *)(uintptr_t)P2V(copy),
                   (const void *)(uintptr_t)P2V(already), PAGE_SIZE);
            if (vmm_map_in(proc->pml4_phys, page, copy, prot_to_vmm(v->prot)) != 0) {
                pmm_free_page(copy);
                VMF_DECLINE();
            }
            /* The cache's page is no longer in this address space, so the pin
             * this mapping held on it goes too -- otherwise a file mapped and
             * written by many processes would pin pages nothing maps. */
            vmo_unwire_page(v->obj, idx);
            g_fault_stats.cow++;
            g_fault_stats.handled++;
            spin_unlock(&vma_lock);
            return true;
        }
    }

    /* Already mapped and not the COW case? Then another core resolved this
     * exact page while we were deciding. Not an error and not a bug: the
     * instruction simply retries and succeeds. */
    if (already) {
        spin_unlock(&vma_lock);
        return true;
    }

    /* Every mapping has an object -- anonymous ones included, see
     * vma_map_common. A VMA without one is a bookkeeping bug, and the honest
     * answer to a fault inside it is the same as for no VMA at all: decline,
     * and let it be fatal. */
    if (!v->obj) VMF_DECLINE();
#undef VMF_DECLINE

    /* --- THE PAGE COMES FROM THE OBJECT, NOT THE ALLOCATOR -----------------
     *
     * This is the whole payoff of one object per file. Mapping a file is
     * handing the process the pages the cache already holds -- so a reader and
     * a mapper cannot disagree, and two processes mapping the same file share
     * one set of frames.
     *
     * A SHARED mapping gets the page with the VMA's permissions, and a write
     * through it reaches the file by the ordinary writeback path. A PRIVATE
     * mapping gets it READ-ONLY however much the VMA permits, so that the
     * first write faults and lands in the copy-on-write path above.
     *
     * AN ANONYMOUS PAGE COMES FROM THE SAME PLACE. Its object has no file, so
     * the fill is zeroes and there is nothing to copy-on-write FROM: the page
     * is the process's own from the first touch and is mapped with the VMA's
     * full permissions. What the object buys it is a NAME -- the page can be
     * found, unmapped and written to the swap store when memory runs short,
     * and found again by the fault that follows. Before this, an anonymous
     * page was a bare frame nothing tracked, and so could never be taken back.
     *
     * AND GETTING IT MAY MEAN DISK, so the lock is dropped for that part. The
     * wire below can read the file, or read the page back from the swap
     * store, and a spinlock held across a disk wait is interrupts off across a
     * disk wait -- the ATA driver's canary for exactly that fired once per
     * page the first time anything was paged. The first version held the lock
     * throughout, which was tolerable while the only I/O here was a cold file
     * page and is not tolerable when every fault under pressure is one.
     *
     * What the lock protected was the VMA, so the VMA is CHECKED AGAIN after
     * the wire: the same mapping, the same object, the same page of it, and
     * still nothing in the page table. Any of those false is a race with
     * munmap or with another thread's fault on this page, and the answer is
     * to give the wire back and retry the instruction -- which then finds the
     * page mapped, or finds no mapping and is declined the ordinary way. The
     * object itself is kept alive across the gap by a reference, since the
     * munmap that could run in it may drop the mapping's own. */
    struct vm_object *obj = v->obj;
    uint32_t flags = v->flags;
    vmo_ref(obj);
    spin_unlock(&vma_lock);

    uint64_t tw = time_get_ns();
    uint64_t phys = vmo_wire_page(obj, idx);         /* may sleep; may hit the disk */
    g_fault_stats.wire_ns += time_get_ns() - tw;

    spin_lock(&vma_lock);
    v = vma_find(proc, page);
    bool same  = v && v->obj == obj &&
                 (v->file_off + (page - v->start)) / PAGE_SIZE == idx;
    bool taken = same && vmm_get_phys_in(proc->pml4_phys, page) != 0;
    bool mapped = false, mark_dirty = false;

    if (phys && same && !taken) {
        uint32_t eff = v->prot;
        if ((flags & MAP_PRIVATE) && !obj->anon)
            eff &= ~(uint32_t)PROT_WRITE;          /* force the COW fault */
        uint64_t tm = time_get_ns();
        int mrc = vmm_map_in(proc->pml4_phys, page, phys, prot_to_vmm(eff));
        g_fault_stats.map_ns += time_get_ns() - tm;
        if (mrc == 0) {
            mapped = true;
            /* A shared writable mapping may be written at any moment and the
             * kernel will never see the store, so the page is dirty from the
             * instant it is mapped. Anything less loses data. (Marked after
             * the unlock: the page is wired, so it cannot go anywhere first.) */
            mark_dirty = (flags & MAP_SHARED) && (v->prot & PROT_WRITE);
            g_fault_stats.handled++;
        }
    }
    bool retry = !same || taken;                    /* a race, not a failure */
    if (!mapped && !retry) g_fault_stats.declined++;
    spin_unlock(&vma_lock);

    if (phys && !mapped) vmo_unwire_page(obj, idx);  /* the pin was for a mapping that did not happen */
    if (mark_dirty)      vmo_mark_dirty(obj, idx);
    vmo_put(obj);
    return mapped || retry;
}

/* The shared core. `obj` NULL means anonymous, and an object is made for it
 * here; non-NULL means file-backed and the caller has already taken the
 * reference this mapping will hold. */
static int64_t vma_map_common(struct process *proc, uint64_t addr, uint64_t len,
                              uint32_t prot, uint32_t flags,
                              struct vm_object *obj, uint64_t file_off);

int64_t vma_mmap_file(struct process *proc, uint64_t len, uint32_t prot,
                      uint32_t flags, struct vm_object *obj, uint64_t file_off) {
    if (!obj)
        return -EMBK_EINVAL;
    /* A mapping is made of PAGES; an unaligned file offset has no page to
     * correspond to. Rounding it silently would map the wrong bytes. */
    if (file_off & (PAGE_SIZE - 1))
        return -EMBK_EINVAL;
    /* MAP_ANONYMOUS and a file are contradictory. Refusing beats guessing
     * which half the caller meant. */
    if (flags & MAP_ANONYMOUS)
        return -EMBK_EINVAL;
    return vma_map_common(proc, 0, len, prot, flags, obj, file_off);
}

int64_t vma_mmap(struct process *proc, uint64_t addr, uint64_t len,
                 uint32_t prot, uint32_t flags) {
    /* Only anonymous private mappings come through here. Refusing the rest is
     * the whole point: a MAP_SHARED that silently behaved as MAP_PRIVATE would
     * be two processes each believing they see the other's writes. */
    if (!(flags & MAP_ANONYMOUS) || !(flags & MAP_PRIVATE))
        return -EMBK_EINVAL;
    if (flags & MAP_SHARED)
        return -EMBK_EINVAL;
    return vma_map_common(proc, addr, len, prot, flags, NULL, 0);
}

static int64_t vma_map_common(struct process *proc, uint64_t addr, uint64_t len,
                              uint32_t prot, uint32_t flags,
                              struct vm_object *obj, uint64_t file_off) {
    if (!proc || len == 0)
        return -EMBK_EINVAL;

    len = page_align_up(len);
    if (len == 0 || len > (proc->layout.mmap_max - proc->layout.mmap_base))
        return -EMBK_EINVAL;

    /* Exactly one of SHARED/PRIVATE, and one of them. "Neither" has no
     * meaning and "both" is a caller that has not decided. */
    bool shared = (flags & MAP_SHARED) != 0, private_ = (flags & MAP_PRIVATE) != 0;
    if (shared == private_)
        return -EMBK_EINVAL;

    /* W^X, refused rather than granted-and-regretted. A page that is both
     * writable and executable is the primitive every code-injection attack
     * needs, and nothing in this OS requires one -- the JIT-shaped caller that
     * eventually does should map W, write, then mprotect to X. (mprotect is
     * not implemented yet; docs/TODO.md.) */
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
        return -EMBK_EINVAL;

    /* From here the LIST is read and then written, and a fault on another core
     * may be walking it. One critical section for both halves. */
    spin_lock(&vma_lock);

    uint64_t start;
    if (flags & MAP_FIXED) {
        if (addr & (PAGE_SIZE - 1))
            { spin_unlock(&vma_lock); return -EMBK_EINVAL; }
        if (addr < proc->layout.mmap_base || addr + len > proc->layout.mmap_max)
            { spin_unlock(&vma_lock); return -EMBK_EINVAL; }
        if (!range_is_free(proc, addr, addr + len))
            { spin_unlock(&vma_lock); return -EMBK_EEXIST; }  /* MAP_FIXED does NOT silently replace */
        start = addr;
    } else {
        start = find_gap(proc, len);
        if (!start)
            { spin_unlock(&vma_lock); return -EMBK_ENOMEM; }
    }

    struct vm_area *v = kmalloc(sizeof *v);
    if (!v)
        { spin_unlock(&vma_lock); return -EMBK_ENOMEM; }

    /* ANONYMOUS MEMORY GETS AN OBJECT, so that it can be paged. Before this
     * the fault handler handed out bare frames that nothing tracked: they
     * could not be found again, so they could not be evicted, so an anonymous
     * mapping was memory that had to fit or fail. The object is the record --
     * its pages, their frames, and where each went when it was swapped -- and
     * it is told the one address space that maps it, because evicting means
     * unmapping first. */
    if (!obj) {
        obj = vmo_create_anon(len);
        if (!obj)
            { kfree(v); spin_unlock(&vma_lock); return -EMBK_ENOMEM; }
        vmo_set_mapper(obj, proc->pml4_phys, start);
        file_off = 0;
    }

    /* NO PAGES ARE ALLOCATED HERE. The VMA is the promise; vm_fault() keeps it,
     * one page at a time, as the pages are actually touched.
     *
     * That makes mmap O(1) in the size of the mapping instead of O(n), and it
     * makes address space and memory two different resources -- a program can
     * reserve a gigabyte to use three pages of it and pay for three pages. It
     * also means mmap can no longer fail with ENOMEM for lack of memory: it
     * fails only for lack of ADDRESS SPACE, and the memory failure moves to
     * the first touch. Which is where every other kernel puts it, and is the
     * one genuinely awkward consequence -- a store can now fail. */
    v->start    = start;
    v->end      = start + len;
    v->prot     = prot;
    v->flags    = flags;
    v->obj      = obj;
    v->file_off = file_off;
    vma_insert(proc, v);

    spin_unlock(&vma_lock);
    return (int64_t)start;
}

/* Unmap the pages of a FILE-backed (or, historically, bare) range and settle
 * who owns each frame. Caller holds vma_lock. */
static void unmap_range_pages(struct process *proc, struct vm_area *v,
                              uint64_t lo, uint64_t hi) {
    for (uint64_t va = lo; va < hi; va += PAGE_SIZE) {
        uint64_t pa = vmm_get_phys_in(proc->pml4_phys, va);
        if (!pa)
            continue;                      /* never faulted in */

        vmm_unmap_in(proc->pml4_phys, va);

        /* WHOSE FRAME IS THIS? For a file mapping it may be the CACHE's page,
         * which this mapping only pinned -- freeing that would hand the
         * allocator a frame the cache still believes it owns, and the next
         * reader of the file would get whatever was written into it next. Or
         * it may be a private copy-on-write copy, which is this process's
         * alone and must be freed.
         *
         * The cache itself answers: ask it which frame backs that index and
         * compare. Equal means it is the cache's and we only unpin; different
         * means we copied it and it is ours to free. */
        if (v->obj) {
            uint64_t idx = (v->file_off + (va - v->start)) / PAGE_SIZE;
            if (pa == vmo_page_phys(v->obj, idx)) {
                vmo_unwire_page(v->obj, idx);
                continue;
            }
        }
        pmm_free_page(pa);
    }
}

int vma_munmap(struct process *proc, uint64_t addr, uint64_t len) {
    if (!proc || len == 0 || (addr & (PAGE_SIZE - 1)))
        return -EMBK_EINVAL;

    len = page_align_up(len);
    uint64_t end = addr + len;
    if (end < addr)
        return -EMBK_EINVAL;

    /* The list is REWRITTEN here -- nodes freed, split, retargeted -- and a
     * fault on another core walks it. This is the use-after-free the lock
     * exists for. */
    spin_lock(&vma_lock);

    struct vm_area **pp = &proc->vma_list;
    bool touched = false;
    struct vm_area *gone = NULL;        /* unlinked nodes, each carrying a pending vmo_put */

    while (*pp) {
        struct vm_area *v = *pp;
        if (v->end <= addr || v->start >= end) {   /* no overlap */
            pp = &v->next;
            continue;
        }

        uint64_t lo = v->start > addr ? v->start : addr;
        uint64_t hi = v->end   < end  ? v->end   : end;

        /* ANONYMOUS: the object does it. It knows which of its pages exist,
         * which are in frames and which are out on the swap store, and the
         * page tables it is mapped in -- so it unmaps and frees in O(pages
         * that exist) rather than walking every address in the range, and a
         * page that is on disk gives back its slot, which a page-table walk
         * would never have found. */
        if (v->obj && v->obj->anon)
            vmo_discard_range(v->obj, (v->file_off + (lo - v->start)) / PAGE_SIZE,
                              (hi - lo) / PAGE_SIZE);
        else
            unmap_range_pages(proc, v, lo, hi);
        touched = true;

        if (lo == v->start && hi == v->end) {
            *pp = v->next;                  /* whole mapping gone */
            /* The reference this mapping held on its object goes with it --
             * AFTER the lock. A last-reference vmo_put flushes, and a flush is
             * disk I/O; under this spinlock that is a disk wait with
             * interrupts off, which the ATA driver's canary reported the first
             * time posixdemo unmapped a file whose descriptor was already
             * closed. The dead node itself carries the pending put: it is off
             * the list, nothing else can reach it, and its `next` is free. */
            v->next = gone;
            gone = v;
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
            break;                          /* touched: reports OK below */
        }
        tail->start    = hi;
        tail->end      = v->end;
        tail->prot     = v->prot;
        tail->flags    = v->flags;
        tail->obj      = v->obj;
        /* The tail starts further into the FILE than the head did. Copying
         * file_off unchanged would map the same bytes twice. */
        tail->file_off = v->file_off + (hi - v->start);
        if (tail->obj) vmo_ref(tail->obj);              /* a second holder */
        v->end         = lo;
        tail->next  = v->next;
        v->next     = tail;
        pp = &tail->next;
    }

    spin_unlock(&vma_lock);

    while (gone) {
        struct vm_area *n = gone->next;
        if (gone->obj) vmo_put(gone->obj);
        kfree(gone);
        gone = n;
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
        tail->start    = at;
        tail->end      = v->end;
        tail->prot     = v->prot;
        tail->flags    = v->flags;
        tail->obj      = v->obj;
        tail->file_off = v->file_off + (at - v->start);
        if (tail->obj) vmo_ref(tail->obj);              /* a second holder */
        tail->next     = v->next;
        v->end         = at;
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
    spin_lock(&vma_lock);
    if (!vma_split_at(proc, addr) || !vma_split_at(proc, end)) {
        spin_unlock(&vma_lock);
        return -EMBK_ENOMEM;
    }

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

    spin_unlock(&vma_lock);
    return EMBK_OK;
}

void vma_destroy_all(struct process *proc) {
    if (!proc)
        return;
    spin_lock(&vma_lock);
    struct vm_area *v = proc->vma_list;
    struct vm_area *gone = NULL;
    while (v) {
        struct vm_area *next = v->next;
        struct vm_object *o = v->obj;

        /* THE OBJECT'S FRAMES ARE THE OBJECT'S. The address-space teardown
         * that follows frees every frame it finds in the page tables. A cache
         * page still mapped here would be freed under the cache -- and served,
         * later, to whoever the allocator handed the frame next -- or freed
         * TWICE if this was the object's last holder. Until now this function
         * only freed the vm_area records and let that happen; a process that
         * exited with a file mapped was corrupting the cache. The mapping has
         * to let go of the pages BEFORE the tables are walked.
         *
         * Anonymous: the object knows its one mapper and every resident page,
         * so it detaches in O(resident). File-backed: the wire is this
         * mapping's, page by page; a copy-on-write copy that is NOT the
         * cache's stays mapped for the teardown to free, as it should. */
        if (o) {
            if (o->anon) {
                vmo_detach_mapper(o);
            } else {
                for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE) {
                    uint64_t pa = vmm_get_phys_in(proc->pml4_phys, va);
                    if (!pa) continue;
                    uint64_t idx = (v->file_off + (va - v->start)) / PAGE_SIZE;
                    if (pa == vmo_page_phys(o, idx)) {
                        vmm_unmap_in(proc->pml4_phys, va);
                        vmo_unwire_page(o, idx);
                    }
                }
            }
        }
        v->next = gone;                 /* the put waits for the unlock; see vma_munmap */
        gone = v;
        v = next;
    }
    proc->vma_list = 0;
    spin_unlock(&vma_lock);

    while (gone) {
        struct vm_area *n = gone->next;
        if (gone->obj) vmo_put(gone->obj);
        kfree(gone);
        gone = n;
    }
}

uint64_t vma_total_bytes(struct process *proc) {
    uint64_t total = 0;
    for (struct vm_area *v = proc ? proc->vma_list : 0; v; v = v->next)
        total += v->end - v->start;
    return total;
}
