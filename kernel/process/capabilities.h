#ifndef __CAPABILITIES_H__
#define __CAPABILITIES_H__

#include <stdint.h>
#include "include/types.h"   /* kernel bool — NOT <stdbool.h> */

/* -------------------------------------------------------------------------
 * Per-process capabilities: coarse resource-class authority.
 *
 * This is the kernel mechanism EMBX's capability contract (EMBX_Specification
 * v2, §5–§6) is checked against. A binary DECLARES the classes it needs; the
 * loader verifies that declaration is a subset of the spawning process's set
 * (§6 step 9) and the child is born holding exactly its granted set.
 *
 * WHAT THIS IS AND IS NOT. These are COARSE resource classes ("may touch the
 * GPU at all"), a different and deliberately weaker thing than the OS's typed
 * object HANDLES ("holds an unforgeable reference to THIS surface"). Handles
 * stay the real enforcement; a capability is the coarse GATE that governs
 * which handles a parent will install into a child at spawn. Nothing in the
 * kernel checks a capability at a syscall yet -- there is no consumer until
 * EMBX or a subsystem opts in -- so v1 of this mechanism REPRESENTS, SEEDS and
 * ATTENUATES the set and proves the invariant below; it does not gate anything
 * at runtime. That layer is next, and it should gate handle-install, not
 * become a second ambient-authority system.
 *
 * IDs match EMBX §5.6 (cap_id 1..9). The bitmask uses bit position == cap_id,
 * so bit 0 (cap_id 0 = invalid) is never set.
 * ------------------------------------------------------------------------- */

#define EMBK_CAP_FILESYSTEM  1
#define EMBK_CAP_NETWORK     2
#define EMBK_CAP_GPU         3
#define EMBK_CAP_AUDIO       4
#define EMBK_CAP_CAMERA      5
#define EMBK_CAP_USB         6
#define EMBK_CAP_SERIAL      7
#define EMBK_CAP_RAWDISK     8
#define EMBK_CAP_KERNEL_EXT  9
/* cap_id 10 — the authority to debug: read/write another process's registers
 * and memory, plant breakpoints, single-step (EMBDBG_Specification.md §6.1).
 * Strictly more powerful than any resource class, so it is its own capability,
 * held by init/kernel threads and attenuated down the tree like every other.
 * This is the FIRST capability the kernel actually GATES a syscall on — the
 * debug syscalls (69-75) refuse without it — which is the "coarse gate governs
 * which handles a parent installs" step the header note below anticipated. */
#define EMBK_CAP_DEBUG      10
#define EMBK_CAP_MAX_ID      10

#define EMBK_CAP_BIT(id)  (1ULL << (id))

/* Bits 1..MAX_ID set — the maximal set the kernel roots authority at (held by
 * init, and by every kernel thread, which IS the kernel). */
#define EMBK_CAP_ALL  ((((1ULL << (EMBK_CAP_MAX_ID + 1)) - 1)) & ~1ULL)

/* Spawn sentinel: "give the child the parent's whole set" (inherit, no
 * attenuation). Bit 63 is set, which no real cap_id uses, so it can never be
 * confused with a genuine subset request. */
#define EMBK_CAP_INHERIT  (1ULL << 63)

/* THE INVARIANT, as one pure function. Returns EMBK_OK and writes *out with the
 * child's granted set, or -EMBK_EPERM if `requested` asks for anything the
 * parent does not hold. INHERIT resolves to the parent's set unchanged.
 *
 * This is where "no process holds a capability its parent did not" (EMBX §5.2)
 * is enforced, and it is pure so a selftest can exhaust it: chaining it down a
 * tree can only ever shrink the set, never grow it. Defined inline in the
 * header so both the kernel and its test share the exact same decision. */
static inline int embk_caps_attenuate(uint64_t parent, uint64_t requested,
                                      uint64_t *out) {
    if (requested == EMBK_CAP_INHERIT) { if (out) *out = parent; return 0; }
    if (requested & ~parent) return -1;   /* -EMBK_EPERM; kept literal to avoid
                                           * pulling errno.h into a leaf header */
    if (out) *out = requested;
    return 0;
}

#endif /* __CAPABILITIES_H__ */
