# EmbBuild — the native build tool, and why

**Status:** design ratified 2026-07-19. Empirical basis: `test tcc tally`
(kernel/selftests.c:1881) — the make-equivalent hand-unrolled: per-unit
`tcc -c` argvs, one link argv, one install, one oracle. v1 scope: §10.
Format and method per docs/USERSPACE.md: decisions from invariants up,
the tool's on-disk tree derived last.

---

## 1. The deciding constraint, already run

The fork (port make vs. build native) was not settled by philosophy.
The question "what does rebuilding the userland actually require?" was
answered by running it: `test tcc tally` rebuilds a real sval pipeline
tool ON the OS — four compile argvs, one link argv against
`/system/abi`, an install, and an oracle cross-check (`ls / | tally`
vs the builtin `count`). Rebuilding the userland is N copies of that
shape. Every fact below is earned by that run, not guessed.

## 2. Decision — EmbBuild, a typed-manifest walker

**Targets are typed records. Recipes are argv arrays. The tool is a
data-structure walker, not an interpreter.**

1. **Recipes are one `spawn()` each.** The tally rebuild needed
   exactly: sources, an include dir, an object list, link inputs, an
   output path. No recipe in the real graph needs more. No `/bin/sh`,
   no string splitting, no quoting — the failure class of make's
   string world (the same small-integers-in-the-wrong-namespace shape
   as this series' `embk_close_handle(fd)` bug) is structurally absent.
2. **A source-tree convention, discovered not designed:**
   `/data/src/<project>/` with tree shape preserved — forced by
   tally's quote-includes. A manifest names a source root and `-I`
   dirs; that is the whole project model.
3. **The ABI as ambient constants.** `/system/abi/{crt0.o,
   syscalls.o, libc.a}` makes the link line near-constant across all
   targets. No PATH: EmbBuild spawns absolute argvs (USERSPACE.md
   §4.2, applied).
4. **What it deliberately does not have:** variables, functions,
   pattern rules, parallel jobs. Make's power features exist to
   compress graphs too big to hand-write; the real graph is ~50
   one-argv nodes. Start explicit; earn compression when a manifest
   becomes painful to write.

## 3. The sealed boundary — EmbBuild STAGES; adoption is an update

The v2 headline ("the shell rebuilds the shell") produces a new
`shell.elf` — and `/system/bin` is sealed (USERSPACE.md §3). The
resolution is already written there: *"a self-rebuild PRODUCES a new
sealed image; ADOPTING it is an update event."* Therefore:

- **EmbBuild never writes into `/system`. Ever.** All outputs land in
  staging: `/data/build/out/<project>/`.
- **Apps install directly:** for `/data/apps/<name>/` targets, the
  install step is a copy within mutable state — EmbBuild may do it.
- **System programs are ADOPTED, not installed:** crossing the seal is
  a separate, deliberate act — v1: snapshot, copy, reboot, by hand;
  later: a `sysupdate` tool owning that boundary (and, eventually,
  re-signing). A build tool that could overwrite `/system/bin` would
  be a system-update mechanism wearing a build tool's name; keeping
  them separate is the difference between this design and
  `curl | sudo sh`.
- v2's true shape, gained for free: build the shell to staging, VERIFY
  the staged artifact (run it `-c` against the shell-test
  expectations — the oracle pattern), only then adopt.

**✅ DONE (`test embbuild shell`).** The shell rebuilds the shell and the
first adoption event is real: 12 units TCC-built to staging; the STAGED
shell passes the `-c` oracles (expression eval, `where`/`sort-by`/`select`
pipeline, an extern `tally` pipeline); the system snapshots
(`pre-shell-adopt`) and copies staged → `/system/bin/shell.elf` (191,904
bytes); the ADOPTED shell passes the same oracles — every pipeline that
reaches `/system/bin/shell.elf` (the terminal, `test extern`) now runs
through a shell the system built for itself, on its next spawn. The build
tool never touched `/system`: the manifest has no install stanza, and the
seal-crossing copy is the system's act. (This also flushed out
`SPAWN_ARGV_MAX` 16 → 32 — the first machine-generated argv, a 19-entry
link line, outgrew a limit sized for hand-typed commands.)

Two boundaries stated plainly, so the headline is not mistaken for more than
it is:
- **Verification depth.** The staged/adopted shell is proven by three `-c`
  oracles (expression eval, a `where`/`sort-by`/`select` pipeline, an extern
  `tally` pipeline), *not* the full 38-test host suite. It is "a working
  shell across the paths that matter," not "byte-for-byte the host shell"
  (different compiler — nor should it be).
- **Adoption depth.** The ritual exercised is snapshot → copy → re-spawn.
  The `reboot` leg of §3 was not run, and nothing at boot spawns the shell
  anyway (boot goes to `home.elf`); "adopted" here means the file is
  replaced and used on next spawn, with the snapshot as the rollback.

## 4. Staleness — content, not time

The RTC resolves to one second; TCC compiles in milliseconds.
Edit-then-rebuild-within-a-second false-fresh is the COMMON case here,
not the corner — timestamp staleness is disqualified on this machine.

- **v1: stamp files.** Per target, hash of: all input bytes + the full
  argv + the tool identity (compiler path and EmbBuild's own version —
  a flag-only or compiler upgrade must rebuild; make gets this wrong
  too). Stamps live in `/data/build/stamps/`.
- **Hash: CRC32C**, already the house function. Threat model stated
  honestly: ACCIDENTAL collision (~2^-32 per pair) — adversarial
  collision is not in scope for a local build stamp. Upgrade the
  function if that ever changes.
- EMBKFS's per-block CRC32C is internal, not exposed as a file
  identity through `stat` — so v1 hashes bytes in userspace (fine at
  these sizes). "Expose a cheap content-version from the CoW
  generation machinery" is a real kernel item later, pulled by need.

## 5. The manifest — the shell's own value model

Internally, a manifest is sval records: a table of targets
(`name, kind(compile|link|install), inputs, argv, output`). EmbBuild
is built against the sval SDK — typed records, a serializer, and the
code the OS just proved it can rebuild.

Concrete v1 syntax (my recommendation, veto open): a minimal
hand-writable, diff-able text form parsed into sval records —
one record per stanza, `key: value` lines, lists whitespace-split.
Shell-native literal syntax as the manifest surface is a v2 option
once the shell has one worth standardizing. When stdout is a pipe,
EmbBuild emits its plan/results as a typed table (`embbuild | where
state == stale`), fd-3 convention as with every sval tool.

## 6. The honest boundary (named exclusions, each a TCC fact)

"Rebuild-self" with TCC means the STATIC C userland: the shell, the
sval tools, EmbBuild itself. Excluded, with reasons already proven in
the tree: `__thread` (no PT_TLS via tcc link), C++, and the kernel
(wants GCC). None are EmbBuild's problem — they are compiler facts, and
they do not shrink the claim: THE SYSTEM CAN REBUILD THE SYSTEM'S OWN
PROGRAMS.

**Amended 2026-07-23 — the GUI left this list, in two steps.** It used to
read "`libembk.so` apps (the dynamic path is gcc-shaped)". First `test
tcc dyn` showed the *toolchain* could do it (PORTS.md § "The GUI wall").
That alone did not earn the amendment: a capability nothing exercises is
not a rebuild claim, so the exclusion stood on a missing manifest rather
than a missing compiler — a weaker reason, and worth saying out loud
rather than quietly deleting the line.

`/data/src/ui/build.ebm` (authored as `user/bin/clockw.build.ebm`) is
that manifest, and **`test embbuild gui`** is the proof: EmbBuild
compiles and dynamically links the clock widget on-OS, the staged ELF is
`ET_EXEC phnum=5` (the direct readout that the dynamic path was taken —
static gets 2), the kernel binds it to `libembk.so`, the compositor
windows it, it renders, a rerun reports `0 ran, 3 up_to_date`, and the
adopted binary at `/data/apps/clockw/clockw.elf` runs too.

The stanza shape is what was new, and §2's grammar took it unchanged —
no `-static`, `-rdynamic`, the `.so` named as a link input before `-lc`,
`emlink_dynstubs.o`, `libtcc1.o`. That the manifest format needed no
extension for a link line this different is the strongest evidence so
far that "recipes are argv arrays" was the right primitive.

**One honest wrinkle, recorded because it is the format's real cost.**
The first draft of that manifest listed eight header inputs; the
transitive `#include` closure is twelve. It would have built correctly
and gone silently stale on a `backend.h` edit — §2.4's "explicit first"
is only true if the explicit list is *right*, and a hand-written list
scales badly exactly where it matters. The list is now derived by
walking the include graph. This is the concrete argument for
auto-depfiles (§2.4's deferred item), and it is no longer hypothetical.

## 7. Where make lives

The ports story: rebuilding git or CPython on-OS means autotools, sh,
sed, a POSIX layer — an epoch entered deliberately when a foreign tree
demands it (USERSPACE.md §4.3's seam), never a dependency smuggled in
by the native tool. Fourth instance of the fork, same resolution as
the first three — with the difference that this time the native
option's primitive already ran. Exit 42.

## 8. EmbBuild itself

- **An application:** `/data/apps/embbuild/embbuild.elf`. By D2's own
  logic the orchestrator of compilers is no more sealed than the
  compilers. Consequence: EmbBuild rebuilds EmbBuild without touching
  the seal.
- **Host-bootstrapped once** (like TCC), then self-hosting: its own
  manifest is target #3 after tally and sysinfo.

## 9. The tree — derived

```
/data/build/
├── out/<project>/        staged artifacts (NEVER /system; §3)
└── stamps/<project>/     content stamps (§4)
```
`clean` is honest and total: `rm -r /data/build`. All tool-owned state
lives in one deletable directory.

## 10. v1 scope + acceptance — ✅ COMPLETE, including target #3

All three targets are live (`test embbuild` a–f + `test embbuild self`):
tally and sysinfo rebuild from `/data/src` via manifests, and **EmbBuild
rebuilds EmbBuild** — the TCC-built successor is staged, installed to
`/data/apps/embbuild/` (an apps write, no seal crossed), and cross-checked
two ways: the STAGED successor reruns the tally manifest and reports
`0 ran, 6 up_to_date` (a gcc-built tool and its TCC-built child agreeing on
the state of the world — the two-implementations oracle), and the INSTALLED
successor reruns its own manifest to the same verdict. Closing the loop
surfaced two real bugs beneath it: the ABI's syscall stubs were gcc-only
(`register …asm("r10")` bindings tcc ignores — embk_syscall.h now carries a
`__TINYC__` branch passing high args through memory), and **tcc 0.9.27 never
relocates the GOT in static links** (the relocation walk skips `s1->got`;
right with a dynamic loader, NULL-deref without one — every newlib
`stderr`/`errno` is a GOTPCREL `_impure_ptr` access). That is
`tools/tcc/0003-static-link-relocate-got.patch`, sibling of 0001.

Original acceptance definition, all exercised: green =
(a) both oracles pass on the staged binaries; (b) no-change rebuild is
a no-op (stamps hit); (c) one edited byte rebuilds exactly that unit's
chain; (d) one changed flag rebuilds despite identical sources —
the false-fresh case make fails, exercised as a selftest per the
house rule: a change is not done until a test exercises the invariant.

## 11. What would reopen this

A manifest that becomes genuinely painful to hand-write (earns
compression features, §2.4); a foreign source tree on the critical
path (earns the make port, §7); staging-vs-adoption friction so
constant it argues the boundary is drawn wrong (§3).

---

## 12. Scope — EmbBuild builds the KERNEL (the last self-host frontier)

*The crown of the own-the-stack arc: EmbBuild, on the OS, rebuilds the **kernel**
(and then the bootable image) from source — so EmbLinkOS reproduces itself, kernel
included, with no external tools. This is a scope, not a plan of record.*

### 12.1 What is already proven (so this is orchestration, not invention)

The compilers work on kernel-grade C. On the host: **EmbCC compiles all 88 kernel
TUs** (`-mno-sse`), **EmbLD links** them with the higher-half layout, and the
result **boots to the home desktop** — no GCC, no `ld` (see EmbCC `docs/todo.md`
K1–K13, L1–L2). So the remaining question is purely: can EmbBuild drive that same
pipeline *on the OS*? Everything below is what stands between here and yes.

### 12.2 The DAG EmbBuild must express

```
88 × (embcc -c foo.c -mno-sse …)  ─┐
 6 × (assemble  foo.asm)           ─┼─▶  embld -e _start -Ttext … *.o  ─▶  kernel.elf
                                    │         (needs kernel_end = L1)
 stage2.asm ──(assemble, KERNEL_LOAD_SECTORS = ⌈sizeof(kernel.elf)/512⌉)─┐
 stage1.asm ──(assemble, STAGE2_LOAD_SECTORS  = ⌈sizeof(stage2.bin)/512⌉)┼▶ cat → myos.img
                                                          kernel.elf ─────┘
```

The compile fan-out and the link are the shell manifest's `kind: compile` /
`kind: link` shape, one level larger — pointed at `/data/apps/embcc/embcc.elf`
and `/data/apps/embld/embld.elf` instead of `tcc.elf`. A single kernel compile
target reads:

```
name: mm_pmm.o
kind: compile
inputs: /data/src/kernel/mm/pmm.c  /data/src/kernel/mm/pmm.h  … (its headers)
args: /data/apps/embcc/embcc.elf -c -mno-sse -I/data/src/kernel /data/src/kernel/mm/pmm.c -o /data/build/out/kernel/mm_pmm.o
output: /data/build/out/kernel/mm_pmm.o
```

### 12.3 The gaps, ranked (each a real dependency)

- **G1 — an on-OS assembler for the 6 kernel `.asm` (and stage1/stage2). THE
  blocker.** They are **NASM syntax**; TCC's integrated assembler is GAS/AT&T, so
  nothing on the image assembles them today. EmbCC's inline-asm encoder (K1)
  already knows the instruction *encodings* but not a standalone NASM front-end.
  Options: **(a)** port `nasm` (one self-contained binary — the TCC/git/python
  porting lane applies, no fork/exec needed); **(b)** grow EmbLD/EmbCC a standalone
  assembler (`embas`) reusing the K1 encoder + a small NASM-subset parser; **(c)**
  a purpose-built micro-assembler for just these 6 short files. (a) is the most
  in-spirit and reusable; (b) keeps it inside the owned toolchain.
- **G2 — EmbLD `kernel_end`.** The link's one open item (EmbCC `docs/todo.md` L1):
  minimal `-T`/`SYM = .` support so `kernel_end` is defined at the image end,
  instead of the diagnostic stub used to prove the boot.
- **G3 — a derived value feeding a later step's args** (the `KERNEL_LOAD_SECTORS`
  two-pass). Today's manifest args are static; stage2 must be assembled with a
  `-D` computed from `kernel.elf`'s size, and stage1 from stage2's. Small EmbBuild
  feature: a target whose output is a *value* another target's args interpolate
  (or a `kind: measure` step). This is the one genuinely new EmbBuild capability.
- **G4 — stage the kernel source on the image.** ~88 `.c` + ~110 headers + 6
  `.asm` + `linker.ld` under `/data/src/kernel/` (the `/data/src` convention
  already exists; this is mechanical but grows the image by the kernel tree).
- **G5 — generate the manifest.** 88+ targets is past hand-writing (§11's
  "painful to hand-write" trigger, met): a small `Makefile → build.ebm` emitter
  from `KERNEL_SRC` + the header deps, checked in like the other manifests.

### 12.4 Phasing

- **KM1 — `kernel.elf` on the OS.** G1 (assemble the 6 `.asm`) + G2 (`kernel_end`)
  + G4 + G5, then `embcc`-compile + `embld`-link on the metal. *Green:* the
  on-OS `kernel.elf` boots (and/or matches the host EmbCC/EmbLD one).
- **KM2 — the bootable image.** Add stage1/stage2 assembly + G3 (computed sector
  counts) + the `cat`. *Green:* EmbBuild emits a `myos.img` on the OS.
- **KM3 — self-reproduction.** Boot the `myos.img` the OS just built. *Green:* the
  desktop comes up from an image the running system produced from source — the
  loop fully closed, kernel included.

### 12.5 Honest boundary

This is the hardest EmbBuild target and it earns real new work: an **assembler on
the image** (G1) is a genuine port/feature, not a manifest tweak, and the derived-
value step (G3) is the first EmbBuild capability beyond a static walker. Until G1
lands, "EmbBuild builds the kernel" is **blocked on assembling six files**, and
should be stated that plainly. The *userland* image (`embkfs.img`) is a separate
concern (its own mkfs); KM1–KM3 produce the kernel/`myos.img` against a
pre-existing userland.
