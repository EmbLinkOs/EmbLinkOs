# ARM64 — the second architecture

*Design record for the aarch64 campaign: give EmbLinkOS a second architecture
without forking the kernel. Every phase below is marked ❌ until it boots and
✅ only once its "done when" is machine-checked; this file is the plan and the
reasoning, and its status marks are claims that a `make` target will defend.*

**Status: A0 done.** An aarch64 kernel builds and boots to a console on QEMU
`virt`, at EL1, with the device tree handed over intact. Everything from A1 on
is still unwritten.

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

### 2.7 EL1, 4KB granule, 48-bit VA, higher-half at TTBR1
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
  firmware handoff    boot_protocol BIOS stage1/2, UEFI      DTB from -kernel, later UEFI
  console                           16550 @ 0x3F8            PL011 @ 0x9000000
  exceptions                        IDT + ISR stubs          VBAR_EL1 + 16-entry vector table
  paging                            PML4, CR3, invlpg        TTBR0/1_EL1, 4KB/48-bit, TLBI
  interrupt ctrl                    PIC / IOAPIC / LAPIC     GICv3 (distributor + redistributor)
  timer                             PIT / HPET / TSC         generic timer (CNTP_TVAL/CNTVCT)
  context switch                    kcontext.asm (131 ln)    x19-x30, sp, ELR/SPSR
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
* **A1 ❌ Exceptions.** `VBAR_EL1`, the 16-entry vector table, a panic path that
  prints ESR/ELR/FAR. **Done when:** a deliberate bad access prints a decoded fault
  instead of hanging. *(Debuggability before capability — everything after this is
  cheaper because of it.)*
* **A2 ❌ MMU + higher half.** Build tables, enable the MMU, jump to the virtual
  kernel. **Done when:** the kernel runs from `0xFFFF...` and the DTB memory node
  drives the existing `pmm`.
* **A3 ❌ GICv3 + generic timer.** **Done when:** the existing timer-preemptive
  scheduler switches between two kthreads with no scheduler code changed.
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
2. **GICv2 or GICv3?** v3 is the modern default and what `virt` gives by
   default; v2 is simpler to write. Lean v3, accept the extra day.
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
