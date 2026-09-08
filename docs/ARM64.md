# ARM64 — the second architecture

*Design record for the aarch64 campaign: give EmbLinkOS a second architecture
without forking the kernel. Every phase below is marked ❌ until it boots and
✅ only once its "done when" is machine-checked; this file is the plan and the
reasoning, and its status marks are claims that a `make` target will defend.*

**Status: A0–A3 done.** An aarch64 kernel builds, boots on QEMU `virt`, decodes
its own faults, runs in the higher half with the MMU on — page tables built in
assembly before any allocator exists, memory map from the device tree,
`kernel/mm/pmm.c` (the *existing, shared* allocator) running unmodified,
per-section kernel permissions, no identity map — and **preemptively switches
between kernel threads on a timer interrupt**, with GICv3 and the ARM generic
timer both configured from the device tree. Everything from A4 on is still
unwritten. Gaps each phase knowingly left are listed in `TODO.md`, not here.

```sh
brew install aarch64-elf-gcc          # Linux: gcc-aarch64-none-elf
make ARCH=aarch64                     # build
make ARCH=aarch64 run-arm64           # boot it (Ctrl-A X to quit)
make ARCH=aarch64 test-arm64-boot     # the A0 acceptance test, headless
make ARCH=aarch64 run-arm64-hvf       # Apple Silicon: hardware-accelerated
```

## 0. Thesis

> **aarch64 is a second arch tree, not a fork.** We bring the kernel up
> bottom-up on QEMU `virt` — UART → exceptions → MMU → timer → scheduler →
> EL0 → virtio — and we let the `arch_*` HAL be **derived from two working
> implementations** rather than invented against one. The port is finished when
> the same userland, built for aarch64, reaches the same graphical desktop.

The promise this campaign is cashing was made early and in writing:
`PROJECT_STATUS.md` says arch-specific code was "kept identifiable so an ARM64
port later is a contained campaign, not a rewrite," and `TODO.md` defers the
real port to "a later dedicated campaign." This is that campaign.

## 1. The audit — is the promise true?

Measured, not assumed:

| | lines | share |
|---|---:|---:|
| `kernel/arch/x86_64/` | 6,479 | **11%** |
| `boot/` (BIOS stage1+2, UEFI loader) | 1,864 | — |
| kernel total | 58,148 | 100% |

**The promise largely held.** The specific evidence:

* **Port I/O never escaped into core logic.** Every `inb`/`outb` outside the
  arch tree is in a *driver* for a device that does not exist on ARM at all
  (PS/2, PIT, RTC, ATA, AC97, bochs VBE, UHCI) — plus exactly two lines in
  `main.c` masking the legacy PIC. Those are not ports. They are absences.
* **The inline-asm leaks are repetitive, not semantic.** Thirteen files outside
  the arch tree use `__asm__`, but they collapse into **seven primitives**:
  IRQ save/restore (`pushfq`/`cli`/`sti` — one idiom, six files), `pause`,
  `hlt`, `invlpg`, `mov cr3`, `mov cr2`, `rdtsc`. Six of the seven are one-line
  HAL calls on any architecture.

**Where it did not hold** — one place, and it is the campaign's real work:

* **`arch/x86_64/syscall/syscall.c` is 2,019 lines, and most of it is not
  arch-specific at all.** It holds **89 syscall handlers** — `sys_open`,
  `sys_write`, the gfx and audio calls — whose *policy* is entirely portable.
  They are welded to x86 only by how they read their arguments: **187 direct
  reads of `r->rdi`, `r->rsi`, `r->rdx`, `r->r10`, `r->r8`, `r->r9`.** The
  logic is portable; the argument extraction is not. See §2.4.

**Assets already in place** (this port starts further along than it looks):

* `boot_protocol.h` is **already firmware-neutral** — a magic-tagged handoff
  struct (memory map, framebuffer, `BOOT_FW_BIOS`/`BOOT_FW_UEFI`) rather than
  BIOS structures passed raw. It needs a third producer, not a replacement.
* **virtio-gpu (585 lines) and virtio-net (357) already exist** — and virtio is
  precisely what QEMU `virt` offers. The graphics and network drivers for the
  ARM machine are, to a first approximation, already written.
* `EMBX_Specification_v2.md` already reserves `machine = 2` for arm64. The
  binary format anticipated this.
* Crypto, FS, TCP/IP, the compositor, EmUI, the whole of `user/` — endian-clean
  C with no arch ties. ~80% of the tree should compile unchanged.

## 2. Decisions

### 2.1 QEMU `virt` first; Apple Silicon bare metal is a different project
We target `qemu-system-aarch64 -M virt`, GICv3, PL011 UART, virtio. **Not** M-series
hardware. Rationale: `virt` gives us PSCI, a discoverable device tree, a real
serial port to debug through, and a spec to read. Bare-metal Apple Silicon
offers none of those — it is Asahi-scale reverse engineering, and you would be
debugging a black screen with no console. **Cost:** we do not run on the laptop
itself. That is the correct trade for a first bring-up, and `virt` remains the
right CI target forever regardless.

### 2.2 HVF changes the economics, and is half the reason to do this now
`qemu-system-aarch64` on this Mac supports **`-accel hvf`**: an aarch64 guest
runs on aarch64 hardware with hardware virtualization. **Confirmed at A0** —
under `make ARCH=aarch64 run-arm64-hvf` the guest reads `MIDR_EL1` =
`0x610f0000`, which is the host's own Apple core answering, not an emulated
Cortex-A72 (`0x410fd083`). Today's x86 build runs
under cross-arch TCG, which `BUILD_SETUP.md` already documents as the reason
every timing number on the Mac is untrustworthy. **The ARM port is expected to
be faster on this machine than the native-architecture port is** — and it makes
the timing-sensitive tests (`test-audio`, first-frame budgets) meaningful again
on a Mac. This is a genuine engineering payoff, not a nice-to-have.

### 2.3 The HAL is *derived*, not invented
`TODO.md` says it outright: *"don't pre-abstract against a single
architecture."* We honour that. The `arch_*` interfaces are **not** designed up
front from the x86 side; we bring up aarch64 far enough to see what both
architectures actually need, then factor the seam that two real implementations
reveal. An abstraction invented from one implementation is that implementation
wearing a hat. **Cost:** some x86 code gets touched twice. Cheaper than a wrong
interface welded into 58k lines.

### 2.4 The syscall seam is the one refactor that must happen on x86 first
The 187 register reads (§1) are the exception to §2.3, because their fix does
not require guessing what ARM needs — every calling convention on earth passes
arguments *somewhere*, and no handler should care where. The change:

```c
/* today: every handler is an x86 document */
static int64_t sys_open(struct regs *r) { ... r->rdi ... r->rsi ... }

/* after: the arch entry extracts once, handlers stop knowing */
static int64_t sys_open(const struct sysargs *a) { ... a->arg[0] ... a->arg[1] ... }
```

`arch/x86_64` fills `sysargs` from `rdi/rsi/rdx/r10/r8/r9`; `arch/aarch64` fills
it from `x0..x5`. This is mechanical, reviewable, and **verifiable while ARM
does not yet exist**: x86 must stay green through it, which makes it the safest
possible first code change. It should also *move* — 89 portable handlers do not
belong under `arch/x86_64/`.

### 2.5 `boot_protocol` gets a third producer, not a replacement
The magic/version/size contract already exists and already spans two firmwares.
aarch64 adds `BOOT_FW_DTB` (and later `BOOT_FW_UEFI` on ARM, which is the same
UEFI we already parse). Only two things are arch-specific: the struct address
arrives in **`x0`** rather than `rdi`, and the memory map is read from the
**device tree** rather than e820. Growing the struct is already a solved
problem — append, bump the version.

### 2.6 Legacy drivers are not ported. They are absent.
PS/2, PIT, RTC, ATA, AC97, bochs VBE, UHCI/OHCI/EHCI do not exist on `virt` and
will not be emulated. Their replacements: **PL011** (console), **ARM generic
timer** (`CNTVCT_EL0`/`CNTP_*`), **GICv3** (interrupts), **PCIe ECAM** +
**virtio-mmio** (everything else), **PSCI** (SMP bring-up, replacing
INIT-SIPI-SIPI). This follows the rule the build already lives by: absent means
absent, not broken. The arch gate decides which drivers are even compiled.

### 2.7 EL1, 4KB granule, 48-bit VA, higher-half at TTBR1 — *built (A2)*
The kernel runs at **EL1** (we are a guest OS, not a hypervisor; drop from EL2
at boot if the firmware leaves us there). 4KB granule, 4 levels, 48-bit VA —
the closest analogue to the existing PML4 layout, which keeps `vmm.c`'s shape
recognisable. The higher-half split is *better* than x86's: `TTBR1_EL1` holds
kernel mappings and `TTBR0_EL1` user, so the address-space switch touches only
TTBR0 and the "is this a kernel address" test is a hardware property rather
than a convention.

### 2.8 `-std=gnu11`, pinned — a lesson already paid for
`aarch64-elf-gcc` is GCC **16.2.0**, the same version whose C23 default and
promoted-to-error warnings broke the x86 build on macOS. The new arch inherits
the pin from day one rather than rediscovering it. (`brew install aarch64-elf-gcc`
— prebuilt bottle, no 30-minute build.)

### 2.9 x86 stays the default, and stays green
A second architecture must not tax the person who is not working on it. The
build selects with `ARCH ?= x86_64`, and `make` on a Linux box keeps producing
exactly what it produces today. Every phase below is gated on x86 still
building and still booting. **No phase is done if it broke the other arch.**

## 3. The stack, bottom to top

```
                                    x86_64 (built)          aarch64 (this campaign)
  firmware handoff    boot_protocol BIOS stage1/2, UEFI      DTB from -kernel, later UEFI   [A2]
  console                           16550 @ 0x3F8            PL011 @ 0x9000000              [A0]
  exceptions                        IDT + ISR stubs          VBAR_EL1 + 16-entry vectors    [A1]
  paging                            PML4, CR3, invlpg        TTBR0/1_EL1, 4KB/48-bit, TLBI  [A2]
  interrupt ctrl                    PIC / IOAPIC / LAPIC     GICv3 (distributor + redistrib)[A3]
  timer                             PIT / HPET / TSC         generic timer (CNTV_TVAL/CNTVCT)[A3]
  context switch                    kcontext.asm (131 ln)    x19-x29, sp, pc, DAIF       [A3]
  user entry                        iretq / int 0x80         eret / svc #0
  SMP                               INIT-SIPI-SIPI           PSCI CPU_ON
  ─────────────────── everything above this line is shared ───────────────────
  pmm/kheap, fs, block, net, ipc, process, gfx, crypto, tty, all of user/
```

## 4. Phasing

Each phase is a thing that *works*, not a thing that is written. ❌ = not built.

* **A0 ✅ Skeleton + console.** `kernel/arch/aarch64/` — `boot/boot.S`,
  `boot/linker.ld`, `boot/early.c`, `drivers/pl011.c`, `arch.mk`. ~300 lines.
  **Done when:** `qemu-system-aarch64 -M virt -kernel` prints a banner over the
  serial line. **Verified by `make ARCH=aarch64 test-arm64-boot`**, which asserts
  four things rather than one: the banner appears, `CurrentEL` is EL1, the DTB
  pointer arrived with valid FDT magic, and control reached the end of
  `arch_early_main`. It also reports `SCTLR_EL1` (MMU off, as intended at A0)
  and `CNTFRQ_EL0` — proof that boot.S left EL1 able to read the timer, which
  A3 depends on.

  Three things A0 taught that the plan above did not know:
  1. **A bare ELF gets no device tree.** `-kernel kernel.elf` boots and prints
     — and `x0` is zero. QEMU only plants the stub that loads `x0` with the DTB
     address when it believes it loaded a *Linux* kernel. The fix is the 64-byte
     arm64 `Image` header on the front of a flat binary, so the build now
     produces both: the `.img` is what boots, the `.elf` is what gdb reads.
     This resolves the mechanics of §6.1 more sharply than "`-kernel` is faster".
  2. **`-mgeneral-regs-only` is not optional.** FP/SIMD is disabled at EL1 out
     of reset, and gcc will emit an FP register move to copy a 16-byte struct.
     Before A1 there are no vectors, so that trap is an unexplained hang.
  3. **`virt` enters at EL1 already**, so the EL2→EL1 drop in `boot.S` is dead
     code today. It is kept because `virtualization=on` and the UEFI path of
     §6.1 both enter at EL2, and the failure without it is silent.
* **A1 ✅ Exceptions.** `irq/vectors.S` (the 16-entry table + one shared frame
  builder) and `irq/exception.c` (the decoder). **Done when:** a deliberate bad
  access prints a decoded fault instead of hanging — extended in practice to
  *and then carries on running*, which is a stronger claim and a more useful
  mechanism. Covered by `test-arm64-boot`, which now asserts A0 and A1 together.

  What it actually prints, for an unaligned load:

  ```
  === aarch64 exception ===
    vector    : 4  sync, from EL1h (current EL, kernel)
    ESR_EL1   : 0x0000000096000021
                EC=0x00000025  data abort, same EL
                FSC=0x00000021  alignment fault, on a READ
    FAR_EL1   : 0x0000000040200003   <- the address that faulted
    ELR_EL1   : 0x00000000400818ec   <- the instruction
  ```

  This is where ARM is simply better than the architecture we came from, and it
  is worth being explicit about: x86 hands you `#PF` plus a 4-bit error code and
  you infer the rest. `ESR_EL1` carries an exception class *and* a fault status
  code, so the handler can say "level 2 translation fault, on a write" without
  guessing. At A2, when the page tables are new and wrong, that is the entire
  diagnosis. `kernel/lib/ksym.c` exists on the x86 side because a panic that
  prints only hex is a panic you cannot act on; here most of that comes free.

  Four things worth recording:
  1. **A vector slot is 128 bytes — 32 instructions — and building the frame
     takes about fifty.** Inlining the save assembles *without complaint* and
     silently overruns into the next entry, so the table still looks right and
     every exception but the first executes the tail of its predecessor. Each
     stub is therefore four instructions and branches to shared code, and the
     table's size and 2 KiB alignment are asserted **in the linker script**
     (`.if` on a label difference across `.balign` is not an assembly-time
     constant, but it is a link-time one). Both assertions were negative-tested
     — deliberately broken, and they fire.
  2. **`VBAR_EL1` ignores the low 11 bits of what you write to it.** A
     misaligned table does not fail; it dispatches every exception a little way
     off. Hence the alignment assertion rather than a comment.
  3. **SVC and BRK disagree about `ELR`.** For a breakpoint or an abort, `ELR`
     points *at* the instruction; for `SVC` it already points *past* it. The
     recovery path's "+4" is therefore conditional — get it wrong and it skips
     an innocent instruction, with the damage appearing somewhere unrelated.
  4. **The recovery mechanism is not a debugging toy.** `exception_probe()`
     makes synchronous faults recoverable for the duration of one call, and A2
     needs exactly that shape to ask "is there memory here?" while walking the
     device tree. Building it at A1 means it is proven before A2 relies on it —
     and it is what lets the self-test *demonstrate* the handler rather than
     assert it. A fault handler that has never fired does not work.

  It also answered a question A2 was going to have to ask: **QEMU `virt` raises
  a synchronous external abort on a read from unassigned physical space** (it
  does not quietly return zero). So probing for RAM by reading is possible —
  but the DTB memory node remains the right source, and now that is a choice
  rather than an assumption.
* **A2 ✅ MMU + higher half.** `boot/linker.ld` (link high, load physical),
  `boot/boot.S`'s assembly bootstrap, `boot/fdt.c` (a device-tree reader),
  `boot/boot_protocol_dtb.c` (the third producer of §2.5), `mm/pagetable.c`
  (four-level tables in C), `mm/pmm_arch.c`, `cpu/spinlock.c`.
  **Done when:** the kernel runs from `0xFFFF…` and the DTB memory node drives
  the existing `pmm` — both asserted by `test-arm64-boot`, along with the
  section permissions and the absence of the identity map.

  **The headline is what did NOT have to change.** `kernel/mm/pmm.c` — 300
  lines of allocator, written for x86, never touched for this — compiles and
  runs on aarch64 with *one* change, and that change was to stop it hardcoding
  an x86 fact (see below). Same for `kernel/lib/kprintf.c` and
  `kernel/lib/kstring.c`. The virtual address layout in `kernel/mm/pmm.h`
  (`KERNEL_VIRTUAL_BASE`, `DIRECT_MAP_BASE`, `MMIO_BASE`, `P2V`/`V2P`/`KV2P`)
  was adopted **unchanged**, which is why. §1's claim that the tree is ~80%
  portable stops being an estimate here.

  Three seams opened, each because a second implementation made the first one's
  assumption visible — §2.3 working exactly as intended:
  1. **`arch_pmm_reserve_fixed()`.** `pmm_init()` ended with
     `pmm_reserve_page(AP_TRAMPOLINE_PHYS)` — an x86 SMP detail welded into
     portable code. It is now a hook: x86 reserves the AP trampoline, aarch64
     reserves the **device tree** (which firmware puts inside the RAM `/memory`
     reports as usable, so the allocator would otherwise hand out the only
     description of the machine) and firmware's reservations. "aarch64 reserves
     no trampoline" is a *fact* about PSCI, not a stub.
  2. **`kprintf_set_secondary()`.** `kprintf` called `console_is_ready()` and
     `console_putchar()` directly, so the log path — the thing you need when
     everything else is broken — carried a link dependency on the framebuffer
     console, the font data and the compositor. The console now *registers*
     itself. On x86 that was merely distasteful; on a second architecture it is
     fatal, because the memory manager needs `kprintf` long before there is a
     display.
  3. **`boot_protocol.h` moved to `kernel/boot/`** and gained `BOOT_FW_DTB`.
     §2.5 predicted "a third producer, not a replacement"; that is exactly what
     it took. `kernel/drivers/char/serial.h` had the same latent property —
     written for a 16550, it turned out to describe a *console*, and PL011
     implements it verbatim.

  Four things A2 taught:
  1. **Everything before the MMU must be position-independent, so it is
     assembly.** The kernel is linked at its virtual address and loaded at its
     physical one, so in that window only `adrp`/`add` and literal pools are
     correct; a pointer stored in `.rodata` is a link-time absolute and is
     wrong. C cannot be trusted there — the compiler will happily emit an
     absolute pointer table for a `static const char *const[]` — so the whole
     bootstrap is `.text.boot`, and the beacon it prints (`ABPM`) exists
     because "died before the console" and "died after it" are otherwise
     indistinguishable.
  2. **1 GiB blocks are what makes the bootstrap tractable.** With a 4 KiB
     granule a level-1 entry maps 1 GiB directly, so the entire initial address
     space is one L0 table, four L1 tables and about twenty stores — no
     allocator required, which matters because there cannot be one yet.
     Refinement to 4 KiB pages happens later, in C, and needs *block
     splitting*: replacing a block with a table of equivalent finer entries,
     which is safe to do to live mappings including the one you are executing.
  3. **`AF` (bit 10) is the one everyone forgets.** Without `FEAT_HAFDBS`
     enabled, hardware does not *set* the access flag, it **faults**. An
     otherwise perfect descriptor missing `AF` gives an "access flag fault" on
     first touch, which reads like a permission bug and is not one.
  4. **Dropping the identity map is a security change, not tidiness.** While it
     exists every physical address is also a valid kernel virtual address — so
     the read-only `.text` established moments earlier still has a *writable
     alias*, and a null-pointer dereference reads real memory instead of
     faulting. The self-test proves both: a write to `.rodata` now takes a
     permission fault, and a read of a low address now takes a translation
     fault.

  A1's unaligned-load test had to move, exactly as predicted: with the MMU off
  every access is Device memory where unaligned is illegal, and once RAM is
  mapped Normal it becomes legal. It now targets the MMIO window instead —
  which is a *better* test, because it verifies the window really is
  Device-nGnRnE rather than accidentally Normal.
* **A3 ✅ GICv3 + generic timer.** `irq/gicv3.c`, `drivers/timer_generic.c`,
  `cpu/kcontext.S` + `cpu/kcontext.h`, `sched/bringup.c`.

  **The "done when" as written was wrong, and it is worth saying why rather
  than quietly restating it.** It said *"the existing timer-preemptive scheduler
  switches between two kthreads with no scheduler code changed."* That
  presumed `kernel/process/process.c` is portable. It is not — not because of
  its own logic, but because of its **dependency set**: 3,700 lines that
  include the compositor, the surface layer, IPC channels and pipes, the ELF
  and EMBX loaders, the GDT and the LAPIC. Reaching it is A5–A6.

  So A3 proves the three things underneath it instead, each of which
  `process.c` will simply use: **the GIC delivers**, **the timer fires
  periodically**, and **`kernel_ctx_switch()` actually switches**. Those are
  exercised by `sched/bringup.c` — a round-robin scheduler with a scheduled
  deletion date, written against the interfaces `process.c` already uses
  (`struct kcontext`, `kernel_ctx_switch`, a timer tick calling the scheduler)
  so that what is proven transfers. `TODO.md` records that it is deleted at A5,
  and its own header says so at length. Finding out that the GIC is misrouted
  *while also* bringing up `process.c` is the failure this avoids.

  What the run actually shows:

  ```
  gic: GICD 0x08000000 (64 KiB), GICR 0x080a0000 [from the device tree]
  timer: device tree says virtual timer is PPI 11
  gic: INTID 27 -> generic timer
    worker 1 resumed: slice 4, uptime 124 ms, 2686085 spins
    worker 2 finishing after 4 slices
  sched:   0 boot       18 slices
  sched:   1 worker-A   18 slices
  sched:   2 worker-B    4 slices (done)
  gic:   INTID 27   generic timer    40 deliveries,  spurious: 0
  ```

  Five things A3 taught:
  1. **`-M virt` does NOT default to GICv3 under TCG** — §6.2 said it did, and
     that is only true under KVM/HVF. Under TCG you get a GICv2 and the kernel
     finds no `arm,gic-v3` node at all. The run targets now pass
     `gic-version=3` explicitly, which is more honest anyway: the machine we
     target is a GICv3 machine and that should be visible on the command line.
     `gic_init()` detects the v2 case and says exactly that, because "node not
     found" alone sends you looking in the wrong place.
  2. **End the interrupt BEFORE running the handler.** Not the obvious order,
     and load-bearing: the timer handler *context-switches*, so it does not
     return. With EOI afterwards, the interrupt stays active until that thread
     is scheduled again, the GIC refuses to deliver another at the same
     priority meanwhile, and the other thread never gets a tick — one
     preemption, then silence. Safe here because everything runs at one
     priority with `PSTATE.I` masked, so nothing can nest.
  3. **A new thread must start with interrupts ENABLED, explicitly.** It is
     first entered from inside the IRQ handler, where the exception itself set
     `PSTATE.I` — and unlike every later resumption it never returns through
     the vector epilogue's `eret` to have PSTATE restored. Inherit the
     handler's DAIF and the thread runs forever without being preempted: the
     scheduler appears to work exactly once. Hence `daif = 0` in
     `kernel_ctx_prepare()`.
  4. **Re-arm the timer first, tick second.** `CNTV_TVAL` is a *down* counter
     that keeps going negative after firing, and the interrupt stays asserted
     until it is reloaded. Reload after calling the scheduler and the interrupt
     re-fires immediately on return — a livelock that presents as a timer
     running impossibly fast.
  5. **Three x86 devices collapse into one.** x86 needs the PIT for a tick, the
     HPET for a monotonic clock and the TSC for cheap high-resolution time —
     and `tsc_calibrate()` exists because the TSC's frequency can only be
     *measured*, so the kernel spends 10 ms at boot timing one clock against
     another. The ARM generic timer is one device that does all three and
     `CNTFRQ_EL0` **states** its frequency. `tsc_calibrate()` on this side is a
     no-op because the measurement is genuinely unnecessary, not unwritten.

  The virtual timer (`CNTV_*`, PPI 11) is used rather than the physical one:
  under HVF or KVM the hypervisor owns the physical timer and a guest is
  expected to use the virtual, and `boot.S` already zeroed `CNTVOFF_EL2` so
  they read alike under TCG. Which PPI that is comes from `/timer`'s
  `interrupts` property — `virt` lists four (secure physical, non-secure
  physical, virtual, hypervisor) and picking by index without decoding is a
  guess.
* **A4 ❌ The `sysargs` refactor (on x86).** §2.4. **Done when:** x86 boots to the
  desktop with zero `r->rdi` left in a handler, and the 89 handlers no longer live
  under `arch/`.
* **A5 ❌ EL0 + `svc`.** Address-space switch on TTBR0, `eret` to user, syscalls
  land in the now-neutral handlers. **Done when:** a static aarch64 binary runs and
  calls `write`.
* **A6 ❌ Userland toolchain.** newlib for aarch64, `EM_AARCH64` + the
  `R_AARCH64_*` relocations mirroring today's `R_X86_64_*` set, `libembk.so`.
  **Done when:** a dynamically-linked EmUI app loads.
* **A7 ❌ virtio on ECAM.** PCIe ECAM probe + virtio-mmio; reuse virtio-gpu/net
  as-is. **Done when:** the compositor presents a frame on `virt`.
* **A8 ❌ EMBX `machine = 2`.** The spec already reserves it.
* **A9 ❌ SMP via PSCI.** Last, deliberately — the x86 SMP work says per-CPU
  structures are the hard part, and those are already written.

The HAL (`arch_irq_save`, `arch_cpu_relax`, `arch_tlb_flush_page`,
`arch_fault_addr`, `arch_timestamp`, …) is **not a phase**. It is extracted
during A3–A5, when both sides exist and the seam is visible. See §2.3.

## 5. What we deliberately do NOT do

* **No 32-bit ARM.** aarch64 only.
* **No big-endian.** Little-endian only; the FS and network code assume it and
  there is no reason to pay for the alternative.
* **No EL2 / hypervisor.** We drop to EL1 and stay there.
* **No device-tree *driver* framework.** We read the few nodes we need (memory,
  UART, GIC, PCIe) and hardcode the rest of `virt`'s well-known layout. A general
  DT framework is a project of its own and buys nothing on one board.
* **No 16KB/64KB granules.** 4KB only, to keep one page-size story in `vmm`.
* **No bare-metal Apple Silicon.** §2.1.
* **No unified fat binaries.** One image per architecture.

## 6. Open sub-decisions

1. ~~**`-kernel` or UEFI on `virt`?**~~ **Settled at A0: `-kernel`, with an
   arm64 `Image` header** (see A0's note 1 — without the header there is no
   device tree, so this was never really a free choice). UEFI stays open for
   later, once there is something worth booting properly, and `boot.S`'s EL2
   drop is already there for it.
2. ~~**GICv2 or GICv3?**~~ **Settled at A3: v3** — and the premise was wrong.
   `virt` gives GICv3 by default only under KVM/HVF; under TCG it still gives a
   **GICv2**, so `gic-version=3` is passed explicitly on every run target. No
   v2 fallback exists on purpose: a half-configured controller is worse than
   none, because interrupts then appear enabled and simply never arrive.
3. **Where does `sysargs` live** — a new `kernel/include/syscall_abi.h`, or
   inside the existing syscall header once it is hoisted out of `arch/`?
4. ~~**Does `ARCH` select via directory or via a per-arch fragment `.mk`?**~~
   **Settled at A0: a fragment**, `kernel/arch/$(ARCH)/arch.mk`, included at the
   bottom of the top-level Makefile and *only* when `ARCH != x86_64`. The x86
   build never parses a line of it, which is how §2.9 is enforced rather than
   promised — and it is checked: `make -n all` produces byte-identical recipes
   with and without the hook, on both `UNAME_S` settings. The x86 build was
   deliberately **not** hoisted into a matching fragment; that is churn today,
   and §2.3 says to derive the shared shape from two working implementations.
5. **Do we keep a bochs-VBE-style linear framebuffer path at all on ARM,** or is
   virtio-gpu the only display? Leaning: virtio-gpu only — `virt` has no VGA.
