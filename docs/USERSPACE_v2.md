# Userspace v2 — Authority *is* the Namespace

*The concrete architecture that realizes [USERSPACE.md](USERSPACE.md) §2's
decision (per-process namespaces, "naming is owning"). Ratified 2026-07-28:
**pure capability-namespace** — no uid/gid, no `rwx`. This doc supersedes the
tree/entry parts of USERSPACE.md; §3 there (the sealed/mutable boundary) still
holds and is folded in below.*

## 0. Why now — the gap between the design and what shipped

USERSPACE.md decided the distinctive thing and then "built it later." Reality
diverged:

- **No init.** The kernel spawns `/system/bin/home.elf` directly as the first
  process. No pid-1, no root of authority, no supervisor.
- **One flat global namespace.** Every process resolves the same `/` (one VFS
  mount). Per-process namespaces were decided (§2.3) but never built; fonts and
  demos even strayed to bare `/`.
- **Capabilities gate classes only.** The 9 caps
  (`FILESYSTEM/NETWORK/GPU/AUDIO/CAMERA/USB/SERIAL/RAWDISK/KERNEL_EXT`) attenuate
  at spawn (`embk_caps_attenuate`), but there is no per-object authority, no
  ownership, no notion of a user.

So this is not "add features." It is: **make the process model the spine**, the
way §2 intended.

## 1. The thesis

> **A process is born holding two grants: a NAMESPACE (the names it can resolve)
> and a CAPABILITY SET (the resource classes it may touch). Both come from its
> parent; both only ever attenuate. There is no ambient global root. You may act
> on X only if X is *in your namespace* AND you hold X's *capability*.**

Capabilities already work exactly this way (grant + attenuate at spawn). v2 is
**the same law applied to names.** init + permissions + multi-user are then not
four subsystems — they are three consequences of one rule.

## 2. The process model (birth)

A process is `{ namespace, cap_set, … }`. `spawn` gains one argument beyond
today's cap grant: the **child's namespace**, which the parent composes from
*its own* bindings (it can only hand down names it holds — attenuation, same as
caps). A child therefore sees a **subset/rebinding** of its parent's tree and a
**subset** of its parent's caps. Nothing is ambient; everything is inherited by
explicit grant.

## 3. The namespace mechanism (concrete) — hybrid: names on top, handles underneath

*Ratified 2026-07-28.* **Internally, everything is an object handle.** A namespace
is a small table mapping a **prefix → a root object handle** (each with a
`ro`/`rw` mode). The kernel **never walks from a global root** — resolution is:

```
open("/home/a.txt")
  → namespace lookup:  "/home"  → RootHandle(UserFS, rw)
  → object_open(RootHandle, "a.txt")      // relative to the held object
```

So applications still enjoy **pathnames**, but the kernel always starts from an
*object it was handed*. This is the best of both: prefix ergonomics on the
surface, object-capability purity underneath.

Example init table:

| prefix | root object |
|--------|-------------|
| `/`      | RootFS |
| `/system`| SysFS (sealed, ro) |
| `/apps`  | AppFS (ro) |
| `/home`  | UserFS (a user's home) |
| `/run`   | IPC / ephemeral namespace |
| `/state` | StateFS |

- The backing filesystems (EMBKFS, epfs) remain the **substrate**; a process is
  simply handed root handles into *parts* of it, never the whole.
- init's table binds every root. A session's table binds a narrowed set. An app's
  is narrower still. Handing a child a prefix it lacks is impossible (attenuation).
- A prefix with no binding **does not resolve** — *absent*, not "denied"
  (USERSPACE.md §2.3).

*Kernel change:* the per-process `struct` gains this prefix→handle table; the
path-resolution entry point does the namespace lookup, then object-relative
opens from the matched root handle. Cheap: a session ~5 bindings, an app 1–3.

## 4. init — pid 1, the root of authority

The kernel spawns **exactly one** process: `/system/bin/init.elf`, holding the
full namespace + `EMBK_CAP_ALL`. init is the only holder of the whole tree; every
other process's authority is a narrowing of init's. It:

1. Brings up `/run` and any boot services.
2. Reads a small config (which sessions/services to start).
3. Starts the **session manager** (or, single-user, one session directly), each
   with a *narrowed* namespace + cap profile.
4. Supervises: reaps, and (config-permitting) restarts a died service. Minimal —
   a supervisor, not systemd.

This replaces "kernel spawns home.elf." The desktop becomes *a session's* child,
not the first process.

## 5. Permissions — nameable **and** capable

There are **no owner/mode bits, no uid/gid, no `chmod`.** "May I open
`/home/notes/todo`?" = (a) it resolves in my namespace, and (b) I hold
`CAP_FILESYSTEM`. Read-only vs read-write is the **binding's** `mode`, or the
granted object-capability — the same object handed with `ro` cannot be written,
full stop. Finer than a class cap: a process can be handed a binding to *one
file/dir* (an object-capability), not a whole class of the FS.

## 6. Multi-user — namespace/capability domains, not uid/gid

A **user** is a named identity with (a) a home subtree `/users/<name>`, and (b) a
default namespace + cap profile. "Log in as alice" = the session manager spawns
alice's session with namespace `{ /system ro, /apps ro, /home → /users/alice rw,
/run → /run/alice }` and alice's caps. **Isolation is structural:** alice's
session holds no binding that names `/users/bob`, so bob's files are *unnameable*
— not access-checked, unreachable. No uid, no ACL, no `chmod`. That is the
capability-OS answer, and it is genuinely not Linux/Windows.

## 7. The tree (init's full view — nothing at bare `/`)

| Path        | What | Mode to a session |
|-------------|------|-------------------|
| `/system`   | the sealed, verified OS — bin, lib, abi, fonts, kernel (USERSPACE.md §3) | `ro` |
| `/apps`     | installed apps; each an EMBX package that *declares* its caps | `ro` |
| `/users/<name>` | homes — the only writable user data | own home only, `rw` |
| `/run`      | ephemeral: IPC endpoints, service handles (epfs) | a scoped subtree |
| `/state`    | mutable system state — logs, config, build outputs | services only |

A session sees `/system`, `/apps`, its `/home`, its `/run` — and nothing else. A
normal app sees *less*. The "Files app sees all of `/`" mess is gone by
construction: a broad view is an explicit privilege (a file-manager gets one),
not the default every process inherits.

## 8. Migration + phasing (honest: this is a multi-phase kernel+userland rework)

Every existing app assumes the global `/` (home reads `/font.ttf`, spawns
`/data/apps/…`). So the namespace turn can't flip at once. Phased, each shippable:

- **UP1 — init as pid 1. ✅ SHIPPED (2026-07-28).** The kernel now spawns
  `/system/bin/init.elf` (the first user process, root of authority), which
  brings up the desktop session (`home.elf`) and *supervises* it — reaps + respawns
  a died desktop; never exits. The desktop is init's child, not the first process.
  Namespace still global (compat). The old `init.elf` (a native-primitive test
  harness that had squatted the name) was renamed `primtest.elf` — `test ring3
  threads` still passes. The bare-`/` **font** stragglers moved to `/system/fonts/`
  (`font.ttf`, `mono.ttf`); the `.txt` items at `/` are deliberate FS-format/POSIX
  test *fixtures* and correctly stay there. `user/bin/init.c` is freestanding
  (own `_start`, no libc — init keeps no dependency it must keep alive).
- **UP2 — the namespace mechanism. ✅ SHIPPED (2026-07-28).** Per-process
  binding tables in the kernel (`kernel/fs/namespace.{c,h}`, `struct namespace`
  on `struct process` beside `cap_set`). `vfs_resolve` now does *namespace lookup
  then walk from the bound root object* — it never starts from a global root; a
  process with no active namespace (kernel/early boot) falls back to the mount
  table, so nothing outside userspace changed. init is born with the global view
  (`ns_seed_global`: one RW binding per mount + a **read-only** binding for the
  sealed `/system`), and children inherit it (`ns_copy`); attenuation
  (`ns_attenuate`: a child may narrow, never widen) is built and proven, ready
  for the spawn-grant ABI (UP2b). Read-only bindings are ENFORCED at every write
  choke point (`ns_check_writable` in `vfs_open`/`vfs_write`/`vfs_{unlink,rename,
  mkdir,rmdir,chmod}_path`) — a userspace write to `/system` returns EROFS
  *before* the path even resolves. Verified: `test namespace` 13/13 (lookup,
  longest-prefix, absence, attenuation, seeding); init's live ring-3 probe
  confirms `/system` write-refused yet still readable; `test posix` ALL PASS
  (writes at the `/` RW binding intact); `test ring3 threads` OK. The absence
  property (a narrowed process cannot *name* what it wasn't granted) is the
  distinctive core; UP2b makes it live per-app.
- **UP2b — the spawn-grant ABI. ✅ SHIPPED (2026-07-28).** A parent hands a child
  a NARROWED namespace at spawn: `SPAWN_ACTION_NS_BIND` (kernel `spawn.h`,
  userspace `embk_action_ns_bind(a, prefix, EMBK_NS_RO|RW)`). Adding any NS_BIND
  action makes the child's namespace *exactly* the granted prefixes (absent =>
  inherit the parent's whole view). The kernel resolves each prefix in the
  **parent's** namespace (`vfs_resolve_ex`, in `process_create_caps`) — so a
  parent can only grant what it can itself name, at a mode no wider than it holds:
  that resolve *is* the attenuation, and it hands back the real directory object.
  Proven live by `ns_spawn_test` inside `test ring3 threads`: a child granted
  `{/system ro, /data rw}` reads `/system`, writes `/data`, is refused a write to
  `/system` (EROFS), and **cannot name `/run`** (a real mount — unnameable, not
  "denied"); and a parent granting an unnameable prefix has its spawn refused.
  The absence property is now live per-process. What remains (UP4) is apps
  *declaring* their needed prefixes in the EMBX manifest so the session grants
  exactly that.
- **UP3 — multi-user.** `/users/<name>`, the session/login manager, per-user
  namespace + cap profiles. Two users provably can't name each other's files.
- **UP4 — declared namespaces. ✅ SHIPPED (2026-07-28).** An app *ships* its
  declared namespace as a per-app manifest (`user/bin/<name>.ns` →
  `/data/apps/<name>/<name>.ns`, lines of `<ro|rw> <prefix>`), and the session
  (home) reads it and grants EXACTLY those bindings via NS_BIND — no manifest =>
  the app inherits the full view (un-manifested apps unaffected). Shipped:
  `clockw.ns` = `ro /system` (narrow), `edit.ns` = `ro /system` + `rw /data`,
  `files.ns` = `rw /` + `ro /system` (the file manager's broad view, now
  *explicitly declared* rather than ambient, with the OS still sealed). Proven
  live: at boot home logs `spawn …/clockw.elf -> ns[ro /system] (1 bind)` and
  clockw then *runs and renders* (`Clock: widget up` / `first frame`) with only
  `/system` nameable — a real GUI app confined by its own declaration. (Carrier
  note: ELF apps ship a sidecar manifest; an EMBX binary could carry the same
  declaration as an inline section beside its capability table — future, and
  EmbCC-side.)

## 9. What we deliberately do NOT do (it's our OS)

- **No uid/gid, no `rwx`/`chmod`, no ACLs.** Authority is names + caps.
- **No POSIX FS layout** (`/etc /usr /bin /var/lib …`). Five roots, each with a
  reason.
- **No ambient global root** for ordinary processes. The global tree is init's
  private full view, not the world's.
- **No setuid / privilege escalation.** You cannot *gain* a name or cap you
  weren't handed — attenuation is monotonic (the existing cap invariant).

## 10. Open sub-decisions (for us to settle before/inside each phase)

1. ~~Namespace = paths or handles?~~ **DECIDED (2026-07-28): hybrid** — prefix →
   root object handle; resolution is namespace-lookup then `object_open(root,
   rel)`. Names on top, handles underneath. See §3.
2. **init config format.** A tiny declarative file (EmbBuild-manifest-shaped?) vs
   compiled-in. *Lean: a small text file under `/system` or `/state`.*
3. **Where a user's identity lives** — a `/users/<name>/profile` (namespace + cap
   template) vs a system registry under `/state`. *Lean: per-home profile.*
4. **`/data` → new tree.** Today's `/data/{apps,src,build,tmp}` maps to
   `/apps` + `/state` + a user's home. One-time migration in UP1.
5. **Session ↔ desktop.** Is the desktop the session, or does a session host a
   desktop it can restart? *Lean: session hosts; desktop is restartable.*
