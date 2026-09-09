# ARM64 — the second architecture

*Design record for the aarch64 campaign: give EmbLinkOS a second architecture
without forking the kernel. Every phase below is marked ❌ until it boots and
✅ only once its "done when" is machine-checked; this file is the plan and the
reasoning, and its status marks are claims that a `make` target will defend.*

**Status: A0–A7 COMPLETE. The REAL desktop session runs on ARM.**

**The whole shared kernel links and runs on aarch64** — the real scheduler, the
real 95-handler syscall table, EMBKFS, the VFS, IPC, the compositor and the
network stack, 495 KB of text — and it **mounts a real EMBKFS image over
virtio-blk and reads a file out of it**:

```
virtio-blk: sda, 262144 sectors (128 MiB), queue 64, polled
  [ ok ] sda: 512 bytes written and read back byte-for-byte
EMBKFS: sda: mounted  v1.0  block_size 4096  blocks 32768  free 15409
EMBKFS: sda: allocator built: 17359 used, 15409 free  (superblock says 15409)  -- OK
VFS: mounted fs at "/" (root ino 1)
  [ ok ] /system/bin/init.elf: 4 bytes, ELF magic intact
```

That image was built by the x86 toolchain and never touched. The B-tree walk,
the directory lookup, the extent map, the block layer, virtio-blk, PCI and ECAM
all agree with it.

**And it now RUNS a program out of that filesystem.** `hello.elf` is an ordinary
newlib binary -- the same `user/lib/crt0.c`, the same `user/lib/syscalls.c`, the
same `user/lib/newlib.ld` the x86 userland uses, compiled for a second machine:

```
--- userland (A6) ---
  [ ok ] /system/bin/hello.elf launched as pid 2
hello from newlib! argc=1 argv0=/system/bin/hello.elf
malloc/free of 4096 bytes (0xdeadbeefcafe): OK
snprintf: OK ("42-beef-ok")
time() = 1788962341 (plausible wall clock)
  [embk thread] running as a native thread of the newlib process
embk thread create/join: OK (tid=2)
hello: 5/5 checks passed
[syscall] exit code=0x0000000000000005
  [ ok ] hello.elf exited with 5 (checks passed)
```

Every line there is a different subsystem answering: `printf` through `_write`
to the PL011, `malloc` through `_sbrk`, `%zu` proving the C99-formats libc,
`time()` through the PL031, a **second EL0 thread** created and joined, and an
exit status that carries a value back. `make ARCH=aarch64 test-arm64-boot`
asserts all of it, under **both** HVF and TCG.
 An aarch64 kernel builds, boots on QEMU `virt`, decodes
its own faults, runs in the higher half with the MMU on — page tables built in
assembly before any allocator exists, memory map from the device tree,
`kernel/mm/pmm.c` (the *existing, shared* allocator) running unmodified,
per-section kernel permissions, no identity map — and **preemptively switches
between kernel threads on a timer interrupt**, with GICv3 and the ARM generic
timer both configured from the device tree, and **runs a program at EL0** that
makes system calls through the same architecture-neutral handlers x86 uses.
Everything from A6 on is still unwritten; gaps each phase knowingly left are
listed in `TODO.md`, not here.

**62 of the 75 shared kernel source files — 35,112 lines — now compile for
aarch64** (§2.3), including all 3,700 lines of the scheduler. Of the thirteen
that do not, eleven are x86 device drivers that will never be ported, because
the devices do not exist on this machine.

```sh
brew install aarch64-elf-gcc          # Linux: gcc-aarch64-none-elf
make ARCH=aarch64                     # build
make ARCH=aarch64 run-arm64           # boot it (HVF on Apple Silicon, else TCG)
make ARCH=aarch64 test-arm64-boot     # acceptance test, headless, every accelerator
make ARCH=aarch64 run-arm64-tcg       # force emulation
make ARCH=aarch64 debug-arm64         # TCG + -d int,unimp
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
  arch-specific at all.** *(Fixed by A4; the counts below were measured by
  grepping that one file and were wrong in both directions — see A4.)* It holds **89 syscall handlers** — `sys_open`,
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
the guest reads `MIDR_EL1` = `0x610f0000`, the host's own Apple core answering,
not an emulated Cortex-A72 (`0x410fd083`).

**Measured at A3, and now the default on Apple Silicon.** Same kernel, same
machine, `test-arm64-boot`'s workload:

| | wall clock to A3 | work per scheduler slice |
|---|---|---|
| TCG | 0.46 s | 11.6M loop iterations |
| HVF | 0.58 s | **93.4M loop iterations** |

Wall clock is a tie because that test is *timer*-bound and both run in real
time; the second column is the real number — **~8x the computation per unit of
time**. From A7 on, when there are pixels to composite, that is the difference
between usable and not. `ARM_ACCEL` follows the host the same way
`QEMU_DISPLAY` and `FB_W` already do, so the Linux box — where an aarch64 guest
cannot be accelerated at all — sees no change.

**TCG is not a fallback, it is a second opinion,** and `run-arm64-tcg` and
`debug-arm64` keep it one word away. `-d int,unimp` reports what the CPU
actually did and HVF has no equivalent. More importantly the two *disagree*,
and the disagreements are where the bugs are: A3's interrupt-ordering error
passed cleanly under TCG for an entire phase and failed immediately under HVF.
`test-arm64-boot` therefore runs under **both** wherever both exist. Today's x86 build runs
under cross-arch TCG, which `BUILD_SETUP.md` already documents as the reason
every timing number on the Mac is untrustworthy. **The ARM port is expected to
be faster on this machine than the native-architecture port is** — and it makes
the timing-sensitive tests (`test-audio`, first-frame budgets) meaningful again
on a Mac. This is a genuine engineering payoff, not a nice-to-have.

### 2.3 The HAL is *derived*, not invented — *first pieces extracted, and measured*
`TODO.md` says it outright: *"don't pre-abstract against a single
architecture."* We honour that. The `arch_*` interfaces are **not** designed up
front from the x86 side; we bring up aarch64 far enough to see what both
architectures actually need, then factor the seam that two real implementations
reveal. An abstraction invented from one implementation is that implementation
wearing a hat. **Cost:** some x86 code gets touched twice. Cheaper than a wrong
interface welded into 58k lines.

**Done after A5, and the measurement is the point.** Rather than guess which
abstractions were needed, every shared (non-`arch/`) kernel source file was
compiled for aarch64 to see what actually broke:

| | files | lines |
|---|---:|---:|
| compiled for aarch64 unmodified | 52 | 18,681 |
| after extracting `arch_irq_*` | 59 | 31,062 |
| after `arch_cpu_idle*` + `arch_fault_addr` | 61 | 31,465 |
| after the scheduler's six seams | 62 | 35,112 |
| after the loader, per-CPU and MMIO seams | **64** | **35,801** |

**And it LINKS.** Compiling was never the finish line. Linking all 64 shared
files against the aarch64 tree leaves **37 undefined symbols, and every single
one is a device driver**:

```
  9  pci_*        port-CF8 configuration space
  9  keyboard_*   PS/2
  9  ac97_*       the audio codec
  3  mouse_*      PS/2
  2  rtc_*   1 pit_*      legacy timers
  1  uhci_*  1 bochs_*    USB, VGA
  1  ioapic_* 1 irq_register
```

Not one portability gap remains. `kernel/process/process.c`,
`kernel/syscall/syscalls.c`, the whole of `fs/`, `ipc/`, `gfx/`, `net/`,
`block/` and the loaders resolve completely. What is left is **§2.6 exactly as
written** — devices that do not exist on this machine, whose replacements
(virtio-input, virtio-blk, PCIe ECAM) are A7's job. `keyboard_release_grab_pid`,
the scheduler's one call into an input driver, is in that list and belongs
there: it is not a seam that was dodged, it is a driver that is genuinely
absent.

A twelfth extraction followed for free: `arch_cpu_relax()` (`pause` / `yield`),
which had five hand-written copies in `process.c` alone.

**Then the scheduler itself.** `kernel/process/process.c` is 3,700 lines and was
the gate on everything downstream. It reached into `arch/x86_64/` for six
things; naming each one took it to **zero lines of inline assembly**, and it now
compiles for aarch64:

| seam | x86_64 | aarch64 |
|---|---|---|
| `arch_kernel_stack_set()` | `TSS.rsp0`, mandatory every switch | **nothing** — `SP_EL1` is a separate register |
| `arch_cpu_id()` | local APIC ID | `MPIDR_EL1.Aff0` |
| `timer_sched_ticks()` | LAPIC timer | the one generic timer |
| `kernel_ctx_prepare()` | `rip`/`rsp`/`rflags`, `sp - 8`, IF=**0** | `pc`/`sp`, aligned exactly, DAIF=**enabled** |
| `arch_enter_user_mode()` | build an `iretq` frame, selectors carry privilege | `SPSR`/`ELR`/`SP_EL0` + `eret` |
| `arch_fpu_pattern_load/store()` | `movdqa %xmm0` | `ldr/str q0` |

Three of those rows are worth reading twice, because each is a place a HAL
designed from one side would have been wrong:

* **`arch_kernel_stack_set()` does nothing on aarch64, legitimately.** x86 must
  rewrite `TSS.rsp0` on every switch because a ring transition fetches its
  stack pointer from a per-CPU table; get it stale and the next syscall builds
  its frame on the *previous thread's* kernel stack. aarch64 has `SP_EL1` as a
  register distinct from `SP_EL0`, so the value is simply still there. An
  interface derived from ARM alone would not have had this call at all.
* **`kernel_ctx_prepare()` needs opposite answers to the same two questions.**
  The stack pointer: x86 wants `kstack_top - 8`, because its trampoline is
  entered by `jmp` while GCC compiles it expecting the 8-byte skew a `call`
  would have left; aarch64 wants exactly 16-byte alignment, because its return
  address is in `x30` and not on the stack. Interrupts on entry: x86 fabricates
  **IF=0** (enabling them early races the trampoline's own `spin_unlock`, a bug
  observed under `-smp 4`); aarch64 fabricates them **ENABLED** (a thread first
  entered from inside an IRQ handler inherits `PSTATE.I` set and would never be
  preempted again). Same function, same purpose, opposite constants — and both
  were previously written inline in shared code.
* **The FP-context self-test became portable for the price of two lines.** Two
  kthreads hold distinct 16-byte patterns across real preemptions and check
  them byte for byte. That test was written to validate x86's FXSAVE/FXRSTOR;
  hoisting only the two vector instructions behind
  `arch_fpu_pattern_load/store()` makes it validate aarch64's V-register save
  too — the one A5 added and had no way to exercise.

**And one file was simply in the wrong place.** `kernel/mm/vmm.c` was counted as
a shared file that failed to compile; it is 817 lines of PML4 walking with 72
mentions of `pml4` and three `mov %0, %%cr3`. It is not portable code that needs
fixing, it is x86 code in a portable directory, and it now sits at
`kernel/arch/x86_64/mm/vmm.c` next to its aarch64 counterpart
(`arch/aarch64/mm/pagetable.c`). `kernel/mm/vmm.h` stays where it is: the
*interface* is shared, and aarch64 now implements the part of it that shared
code actually calls.

**One `static inline` was worth 12,381 lines.** `current_thread_atomic()` in
`kernel/process/process.h` contained `__asm__("pushfq; popq %0; cli")`, and
half the kernel includes that header — so nine files, including
`kernel/syscall/syscalls.c`, `fs/fd.c`, all of `ipc/` and `embkfs.c`, failed to
compile **not because they use it, but because they include the header it sits
in**. Nothing about that is visible from reading the code; it took compiling to
find. The same idiom had also been copy-pasted as a local `save_if`/`restore_if`
pair into `ipc/channel.c` and `ipc/endpoint.c` — and the real coupling in all of
them was not the assembly but `if (flags & (1ULL << 9))`, because bit 9 is a
fact about one CPU.

`kernel/include/arch_irq.h` is the result: a contract, plus a six-line dispatch
to `arch/<arch>/cpu/irqflags.h`. It is the only place in the tree where an
architecture is chosen by a compiler predefine rather than by the build, and
that is deliberate — these are two or three instructions inside spinlocks and
the scheduler's hot path, so a function call would cost more than the work.

Two of its members exist in the shape they do because the machines genuinely
disagree, which is what §2.3 is for:

* **`arch_irq_restore()` is not symmetric.** It enables interrupts if the saved
  state had them enabled, and otherwise does nothing — it never disables. That
  is what *both* implementations already did independently, before there was a
  shared interface to agree on.
* **`arch_cpu_idle_irq_on()` is one call, not `enable(); idle();`** On x86,
  `sti; hlt` relies on `sti`'s one-instruction interrupt shadow; split them and
  an interrupt arriving in the gap leaves `hlt` waiting for one already
  delivered, and the core sleeps forever. On aarch64 that hazard *cannot* occur
  — WFI returns immediately on a pending event, masked or not. A HAL derived
  from the ARM side alone would have exposed two calls and silently broken x86.

**The loader turned out to be portable already.** `kernel/loader/elf.c` — 440
lines of ELF parsing, dynamic linking and relocation — compiled for aarch64
*as it stood*, under `arch/x86_64/`, before anything was changed. The only
architecture in it was five relocation numbers and a machine ID, because the
relocation SEMANTICS are identical on both machines:

| | x86_64 | aarch64 |
|---|---:|---:|
| `ELF_RELOC_RELATIVE` `*where = base + addend` | 8 | 1027 |
| `ELF_RELOC_COPY` copy the symbol's bytes | 5 | 1024 |
| `ELF_RELOC_ABS64` `*where = sym + addend` | 1 | 257 |
| `ELF_RELOC_GLOB_DAT` / `JUMP_SLOT` `*where = sym` | 6 / 7 | 1025 / 1026 |

So it moved to `kernel/loader/` unchanged apart from those names. Anyone adding
a third architecture should expect the same, and should be suspicious of a
patch that needs more. (TLS relocations are the one family where the two
genuinely diverge — x86 counts *down* from the thread pointer, aarch64 counts
*up* — and nothing emits them yet. `TODO.md`.)

**Address spaces followed, and they are the simplest part of the whole port.**
`kernel/mm/vmm.h`'s address-space half — create, destroy, switch, map-in,
get-phys-in, plus guarded kernel stacks — is now implemented on aarch64. On x86
a process's PML4 must also contain the *kernel's* mappings: every address space
carries a copy of the top half, they must be kept in step, and switching means
reloading a root that describes both halves at once. Here `TTBR1_EL1` holds the
kernel and `TTBR0_EL1` holds the process, permanently and separately. So
creating an address space is **one zeroed page**, switching is **one register
write that cannot disturb the kernel**, and destroying it is a walk that
physically cannot reach kernel memory because kernel memory was never in the
table. §2.7 said this would be better than x86's convention; this is where the
difference is cashed.

Two things are now checked rather than claimed:

* **Isolation.** Two address spaces, one virtual address, two physical pages.
  Reading `0x30000000` gives `0xAAAA…` in one and `0xBBBB…` in the other. That
  a switch changes what an address *means* is the property every process
  depends on, and "create returned non-zero" is not evidence of it.
* **Teardown.** The EL0 probe now runs in its own address space, and destroying
  it reports free pages **before and after**: 130,610 → 130,610, all reclaimed.
  A walk that misses a level comes back short and says so, where "destroyed"
  would have read as success.

**What is left is thirteen files, and eleven of them are meant to fail.** Those
eleven are legacy x86 device drivers that §2.6 says are *absent* on ARM rather
than portable — excluding them is a build change, not a code change. The two
that remain are `main.c` (the bring-up ORDER: GDT, IDT, PIC, LAPIC, and which
of them even exist) and `selftests.c`. Neither blocks anything: the scheduler,
the syscall layer, the filesystem, IPC, the heap, the loaders and the network
stack are all through.

**The portability campaign is therefore finished.** Everything remaining is
hardware: A7's virtio drivers, and A6's newlib/userland. There is no more
"this code assumes x86" left to find in the shared tree — the last of it was
`this_cpu()`, and it was the last because it was measured, not guessed.

**Meanwhile the shared kernel heap now runs on aarch64.** `kernel/mm/kheap.c` —
619 lines of slab allocator with canaries and coalescing, written years before
any of this — compiles unchanged and passes a five-size-class allocate/fill/
verify/free test on ARM. The only thing it needed was for `kernel/mm/vmm.h` to
mean something here, which `mm/pagetable.c` now provides. That is the whole
argument for the portability discipline, in one file.

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
  user entry                        iretq / int 0x80         eret / svc #0              [A5]
  syscall args        struct sysargs rdi,rsi,rdx,r10,r8,r9    x0..x5, number in x8   [A4/A5]
  user pointer check  access_ok()   canonical + PML4 walk    bit 63 + TTBR0 walk        [A5]
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
  2. **Handler, then EOI, then scheduler — and the first version of this got it
     wrong.** A3 originally ended the interrupt *before* running the handler,
     reasoning that the timer handler context-switches and therefore never
     returns to do the EOI itself. That reasoning was right and the conclusion
     was wrong, because it ignored the other half: the generic timer is
     **level-triggered**, and the line stays asserted until the deadline moves
     forward. Ending the interrupt while the device is still asserting makes
     the GIC latch another one immediately — a 10 ms tick arriving every 6.4 ms,
     and a thread that could be re-preempted before executing a single
     instruction.

     The two requirements genuinely pull opposite ways: the handler must run
     **first** so the device de-asserts, and the scheduler must run **last**
     because it does not return. They are reconciled by making the scheduler a
     *post-EOI hook on the controller* rather than something the timer handler
     calls — `gic_set_post_eoi()`. Switching inside the handler can only satisfy
     one of the two.

     **This is the bug that TCG hid and HVF exposed** (§2.2), which is why the
     acceptance test now runs under both.
  3. **A new thread must start with interrupts ENABLED, explicitly.** It is
     first entered from inside the IRQ handler, where the exception itself set
     `PSTATE.I` — and unlike every later resumption it never returns through
     the vector epilogue's `eret` to have PSTATE restored. Inherit the
     handler's DAIF and the thread runs forever without being preempted: the
     scheduler appears to work exactly once. Hence `daif = 0` in
     `kernel_ctx_prepare()`.
  4. **Re-arm with an ABSOLUTE deadline, not a relative one.** `CNTV_TVAL` says
     "fire N counts from now", so every period is the interval *plus* however
     long it took to get into the handler, and the error accumulates: measured
     11.1 ms per programmed 10 ms tick, an 11% drift that a monotonic clock
     would inherit. `CNTV_CVAL` takes an absolute counter value, so advancing
     the deadline makes the tick exact regardless of handler latency — late
     once, not late forever. After the change, 40 ticks measure 399 ms under
     HVF and 400 ms under TCG against a nominal 400 ms.

     (This also corrected a claim made earlier in this file: the ~24% timing
     error attributed to TCG in the A2 notes was this drift, not the emulator.)
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
* **A4 ✅ The `sysargs` refactor (on x86).** §2.4.
  `kernel/include/syscall_abi.h`, `kernel/syscall/syscalls.c`, and an
  `arch/x86_64/syscall/syscall.c` that shrank from **2,019 lines to 88**.
  **Done when:** x86 boots to the desktop with zero `r->rdi` left in a handler,
  and the handlers no longer live under `arch/`. Both true, and the boot is the
  real test — this change touches every system call the desktop makes.

  **The audit's number was wrong in both directions, and the refactor is what
  found out.** §1 said *187 reads across 89 handlers*. Actually:

  | | handlers | argument reads |
  |---|---:|---:|
  | `arch/x86_64/syscall/syscall.c` | 88 | 184 |
  | `kernel/process/debug.c` | **7** | **15** |
  | total | **95** | **199** |

  The 187 counted three `r->rax` uses that are the *dispatcher's* own (the
  syscall number and the return value — those are supposed to be there), and
  missed seven handlers entirely because they had grown in `process/debug.c`,
  next to the debugger state they manipulate, rather than in the file anyone
  thought to grep. A count taken by grepping one file measures that file, not
  the problem.

  What is left under `arch/x86_64/syscall/` is 88 lines, and the entire
  x86-ness of the system call interface is now six of them:

  ```c
  a->arg[0] = r->rdi;   a->arg[3] = r->r10;
  a->arg[1] = r->rsi;   a->arg[4] = r->r8;
  a->arg[2] = r->rdx;   a->arg[5] = r->r9;
  ```

  aarch64 will write the same six slots from `x0..x5` and nothing above that
  line will notice.

  Two decisions worth recording:
  1. **No pointer to the trap frame in `struct sysargs`.** It was checked
     first: not one of the 95 handlers read anything from `struct regs` except
     an argument register. An escape hatch nobody needs is one somebody
     eventually uses, and a single handler reaching into the frame puts the
     whole file back under `arch/`. Settles §6.3 — `sysargs` lives in a new
     neutral `kernel/include/syscall_abi.h`, not in the arch syscall header,
     precisely because that header's reason to exist is `struct regs`.
  2. **`kernel/drivers/char/usercopy.h` moved to `kernel/include/`** for the
     same reason `spinlock.h` and `boot_protocol.h` did: `access_ok`,
     `copy_from_user`, `copy_to_user` describe a *privilege boundary*, not a
     machine, and every handler needs them. The implementation stays per-arch.

  The debugger keeps `struct regs`, legitimately: exposing a stopped thread's
  registers is machine-specific by definition. That is the one remaining arch
  include in `kernel/process/`, and `TODO.md` records it.
* **A5 ✅ EL0 + `svc`.** `syscall/usermode.c` (the transition),
  `syscall/syscall.c` (30 lines: trap frame → `struct sysargs`),
  `mm/usercopy.c` (the privilege boundary), `syscall/el0_probe.S` (the first
  program), `syscall/bringup_syscalls.c` (four handlers, temporary).
  **Done when:** a static aarch64 binary runs and calls `write`. What it
  actually prints:

  ```
  el0: probe is 112 bytes; mapping code at 0x400000, stack at 0x7fc000
  el0: eret to 0x0000000000400000, sp 0x0000000000800000
  hello from EL0 -- aarch64 user mode
  el0: REFUSED write of 8 bytes from 0x1000 (not a mapped user address)
  el0: back in EL1, program exited with 42
  ```

  Four claims, each with its own failure mode: user code ran at EL0; a syscall
  with arguments came back; a syscall with **no** arguments came back (so the
  number travelled independently of `x0..x5`); an unmapped pointer was
  **refused** rather than faulting the kernel; and the exit code arrived as 42,
  which means a value travelled from an EL0 register through the trap frame
  into `struct sysargs` into a handler that has no idea which machine it is on.

  **A4 paid off exactly as designed.** `syscall/syscall.c` on this side is
  30 lines of substance — fill six slots, call `syscall_invoke()`, write the
  result back — because the 95 handlers stopped reading registers a phase
  earlier. When A6 compiles the real `kernel/syscall/syscalls.c` for aarch64,
  the arch entry point does not change by a single line. That is the test of
  whether A4 was done properly, and it is why `bringup_syscalls.c` implements
  `syscall_invoke()` rather than being wired in some other way.

  **The convention: number in `x8`, arguments in `x0..x5`, result in `x0`.**
  `x8` rather than `x0` so all six argument registers stay free — the same
  choice Linux/aarch64 makes, and a genuine improvement on x86, where `rax`
  carries the number in and the result out and reading the wrong one has been a
  real bug in this tree before.

  Four things A5 taught:
  1. **`SP_EL0` is a separate register from the kernel's stack pointer.** We
     run at EL1h, i.e. on `SP_EL1`, so setting the user stack does not disturb
     the kernel's — and when `svc` arrives, the vector builds its frame on
     `SP_EL1`, still exactly where it was. That is x86's `TSS.rsp0`, except the
     hardware keeps both pointers instead of reloading one from a table.
  2. **The I-cache does not snoop the D-cache.** The kernel writes the
     program's instructions through a *data* mapping; without
     `dsb; ic iallu; dsb; isb` before entering it, EL0 can fetch stale bytes —
     which on a fresh page means executing whatever was there before. x86
     simply does not have this failure mode (its I-caches are coherent), and it
     presents as an undefined-instruction fault at the entry point.
  3. **Zero every register before `eret`.** Whatever is left in them is kernel
     state, and handing user space a kernel pointer is the kind of leak that
     makes every other mitigation pointless. `SPSR = 0` also matters for a
     second reason: it means DAIF clear, so user code runs with interrupts
     *enabled* — a program entered with them masked cannot be preempted and
     owns the machine.
  4. **The boundary check is easier here, and worth saying why rather than
     leaving as an unexplained absence.** x86's `access_ok` reasons about
     canonical addresses and one page-table root shared by both halves. Here
     bit 63 selects the translation base in *hardware*, so "is this a user
     address" is a single bit test the CPU itself agrees with. The check is
     still needed — the bit says which half, not whether the page is mapped —
     but it cannot be tricked by a clever address.

  `CPACR_EL1.FPEN` is opened here, which is why the FP half of
  `kernel_ctx_switch` had to land in the same change (`TODO.md` said so, and it
  did). The kernel is built `-mgeneral-regs-only` on purpose so an accidental
  FP instruction is a loud fault; user code cannot be built that way, because a
  stock compiler emits `str q0` to copy a 16-byte struct — so an EL0 program
  that never mentions a float still traps on its first `memcpy`.
* **A6 ✅ Userland toolchain.** **Done when: a dynamically-linked EmUI app
  loads.** It does -- `uidemo.elf`, ET_EXEC + PT_DYNAMIC + DT_NEEDED
  libembk.so, 56 `R_AARCH64_JUMP_SLOT` relocations applied by the kernel acting
  as its own dynamic loader:

  ```
    [ ok ] /system/bin/init.elf launched as pid 3
  init: up -- root of EmbLink userspace authority
  init: ns: /system is read-only for userspace (write refused) -- OK
  init: DEV auto-login as 'yves'
  init: authenticated session 'yves' -> ns[ro /system, ro /data/apps, rw /home/yves, rw /run]
  ELF dynlink: /system/lib/libembk.so linked
  init: desktop session started
  compositor: desktop window 1 created (1280x800, 1000 pages) for pid 4
  home: desktop ready
  TopBar: first frame presented (+133ms)
  ```

  **This is the same pid 1 x86 runs, not a demo.** init authenticates, builds a
  confined namespace and spawns the session, so the desktop is init's CHILD --
  and the kernel's one `process_create()` therefore exercises the freestanding
  loader (init), the dynamic loader (home, TopBar) and the namespace machinery
  in a single boot.

  **THIRTY user programs build for aarch64**, not three: `ARM_UI_PROGS` is
  DERIVED from the x86 `$(EMUI_APP_SRCS)` auto-discovery rather than retyped, so
  a new app appears on both architectures the moment it is dropped in
  `user/bin`. Two subtractions are needed and are named in `arch.mk` -- `beep`
  is a static newlib console program and `primtest` is freestanding, facts the
  x86 side encodes in explicit rules that shadow its generic pattern and that
  the variable itself does not carry. Left in, `primtest` fails to link with
  "undefined reference to `main`", which is a true statement about a program
  that has none.

  Not one line of `elf.c` was written for it. The two-way link -- the app's
  imports resolving to the toolkit's exports, and the toolkit's libc imports
  (`malloc`, `memcpy`, `sinf`) resolving BACK into the app, where newlib was
  statically pulled in -- is the same code x86 runs, behind the neutral
  `ELF_RELOC_*` names A4 introduced.

  **The libc was the whole first half, and it was a configuration problem, not a
  porting one.** `tools/newlib/build-newlib-emblink.sh` builds newlib for either
  target from one recipe, because two hand-typed sets of configure flags are two
  things to get subtly wrong -- and they had already diverged in a way nobody
  chose. newlib's `configure.host` gives `x86_64-elf` and `aarch64-elf`
  DIFFERENT syscall contracts:

  | | x86_64-elf (default `*)` case) | aarch64-\*-\*(its own case) |
  |---|---|---|
  | `syscall_dir` | empty -- libc supplies no syscall layer | `syscalls` -- libc **defines** `write()` |
  | `MISSING_SYSCALL_NAMES` | defined, so `_write` → `write` | **not** defined, so `_write_r` calls `_write` |

  `user/lib/syscalls.c` defines the bare POSIX names, so against a stock aarch64
  newlib every one of them is the wrong name: ten `undefined reference to
  _write`-shaped errors, with libc's own colliding `write()` waiting in the
  archive behind them. The script patches that one case to match x86 and then
  **checks the result** (`nm` must show libc asking for `write` and not defining
  it) rather than trusting the patch took.

  **The ELF loader needed nothing.** `elf.h` already carried `EM_AARCH64` and the
  `R_AARCH64_*` set behind the neutral `ELF_RELOC_*` names, and `elf.c` was
  already written against them -- work done during A4/A5 that paid off here as a
  file that did not have to be opened.

  **What was actually written:** an `svc #0` branch in `embk_syscall.h` (x8 for
  the number, x0-x5 for arguments -- so all six argument registers stay free,
  which x86 cannot say); an aarch64 `_start` in `crt0.c`; and variant-I TLS
  beside x86's variant II, which is the one place the two userlands genuinely
  disagree --

      x86-64, VARIANT II            aarch64, VARIANT I
      [ .tdata | .tbss ][ TCB ]     [ TCB ][ .tdata | .tbss ]
                        ^TP         ^TP
      addr = TP - align(memsz)+o    addr = TP + align_up(16, a) + o

  -- where the 16 is not a number we chose but `TCB_SIZE` in binutils'
  `elfNN_aarch64_tpoff_base()`, which is how every TPREL offset in the binary was
  computed. Reserve "a bit extra for safety" there and every thread-local lands
  at the wrong address with no fault to show for it.

  **Three bugs the first run found, each invisible until real user code ran:**

  1. **Every process started with `argc = 0`.** `kcontext.S` parked the three
     user arguments in x9/x10/x11 "in registers the zeroing loop below has not
     reached yet" -- and the loop plainly had. Three zeroes were moved into
     x0/x1/x2. Nothing caught it earlier because the A5 EL0 probe took no
     arguments.
  2. **User text was mapped non-executable**, so the program faulted on its
     first instruction fetch -- a permission fault at the entry point, which
     reads like a loader bug and is a flag bug. `VMM_NX` is INVERTED (absent
     means executable), so the shared ELF loader saying nothing meant
     "executable" on x86 and "not executable" here. Fixed as `TODO.md` had
     already prescribed: a positive `VMM_EXEC`, asked for in the direction a
     permission reads. It costs x86 nothing -- bit 9 is software-available
     there, and that path still decides execution from `VMM_NX`.
  3. **`SP_EL0` was neither saved nor restored across an EL0 exception**, and
     nothing else preserves it: `kcontext.S` saves x19-x28, SP_EL1, LR and DAIF,
     and SP_EL0 is in none of them. Preempt a thread at EL0, run another thread
     of the same process, come back, and the first thread is running on the
     second one's stack. `hello.elf` took a translation fault in `main`'s own
     epilogue, five instructions from a clean exit, reading a joined thread's
     freed stack. **x86 never has this bug** because the user RSP rides in the
     `iretq` frame the hardware itself builds; aarch64 has to say it. The vector
     stub now saves SP_EL0 when `SPSR.M[3:0] == 0` and restores it on the way
     back -- which also makes a user fault report print the USER stack pointer
     rather than the kernel's.

  **The build system** carries the userland for both targets from one set of
  rules: `USER_CC`, `NEWLIB_PREFIX`, `NEWLIB_INC/LIB`, `NEWLIB_CFLAGS` and
  `NEWLIB_LDFLAGS` all derive from `$(USER_TRIPLE)`. The x86 flag lists are
  written out in full per architecture rather than factored, because factoring
  would REORDER them and `make -n all` would stop being byte-identical -- which
  it still is, all 184 recipe lines of it (§2.9). aarch64 objects live under
  `build/aarch64/user/`, because `build/crt0.o` cannot mean two machines.

  Programs are declared by NAME (`ARM_NEWLIB_PROGS`, `ARM_PLAIN_PROGS`) and get
  EXPLICIT generated rules, not pattern rules: the top-level `build/%.elf`
  pattern for EmUI apps matches `build/aarch64/user/hello.elf` with the stem
  `aarch64/user/hello`, and it won -- the first dry run linked hello.elf against
  x86 machine code and an aarch64 `libembk.so` it had helpfully just built.

  `tools/embkfs_mkfs/mkfs_arm64.py` packs a minimal root image, reusing the
  SAME `make_image()`/`build_root_items()` formatter as the x86 image so the
  on-disk format cannot drift between architectures. It refuses to pack a
  binary whose `e_machine` is not `EM_AARCH64`, because the two build trees sit
  side by side and an x86 binary would otherwise get all the way to the
  kernel's loader before anything noticed.
* **A7 ✅ virtio on ECAM.** **Done when: the compositor presents a frame on
  `virt`.** It does, at 1280x800, and there is a keyboard and a pointer to
  drive it with.

  * **virtio-gpu came up unmodified** -- `gpu_init()`, `fb_init()`,
    `console_init()` in the order `main.c` uses them, on a driver already in the
    shared source list. `bochs_gpu_probe()` returns NULL here and always will
    (§6.5 settled: virtio-gpu only, `virt` has no VGA).
  * **virtio-input is new, and deliberately NOT a second keyboard driver.**
    `drivers/input/keyboard.c` was SPLIT: its PS/2 half is behind
    `#if defined(__x86_64__)`, and its policy half -- the char ring, the
    key-event ring, modifier tracking, layouts, the Ctrl-C route, the grab -- is
    now shared and compiled for both machines. The new driver is a translation
    table from Linux evdev codes into `keyboard_inject_event()`, the seam that
    split created. The pointer needed less still: `mouse_set_absolute()` already
    existed for USB tablets, and QEMU's `virtio-tablet-pci` reports ABSOLUTE
    coordinates, so the guest cursor tracks the host's 1:1 instead of drifting.
    Both devices are identified by CAPABILITY -- does it report `KEY_ESC`, does
    it report `ABS_X` -- rather than by the name string, which belongs to QEMU.
    `absent.c`'s eleven keyboard stubs and two mouse stubs are DELETED, as that
    file's own warning demands: a leftover definition there would have won at
    link time and given the machine a working driver and no keyboard.
  * **`drivers/bus/virtio_pci.c` is the shared transport** `TODO.md` asked for
    "when a fourth appears, not before". virtio-input was the fourth, so it was
    written INSTEAD of the fourth copy of the capability walk. The three
    existing drivers still carry their own and should migrate; folding a
    refactor of three working drivers into a bring-up would have made any
    regression ambiguous.

  **Three more bugs, and the first one was mine:**

  0. **The SP_EL0 fix above tested the WRONG REGISTER.** Its comment said "x3
     still holds the SPSR read just above" -- and the ESR/FAR snapshot in
     between had overwritten x3 with FAR_EL1. So which stack got saved was
     decided by the low nibble of a fault address, and for an `svc`, where
     FAR_EL1 is architecturally UNKNOWN, by stale bits. It survived `hello.elf`
     (whose faults happened to have the right nibble) and killed `init.elf` on
     its first syscall-heavy stretch, as a near-NULL data abort whose reported
     ELR pointed at an `add` instruction -- which cannot fault, and was the
     clue. The SPSR is re-read now rather than reused.
  1. **`FPCR` was never initialised, and it is UNKNOWN out of reset.** It
     carries the IEEE trap-enable bits, so user code could take EC 0x2C
     ("trapped floating-point exception") for doing ordinary arithmetic --
     libembk.so's rasterizer did, the moment it scaled a glyph.
     `aarch64_eret_to_el0` now zeroes FPCR and FPSR, which is the AAPCS64
     startup state. x86 has the same requirement and meets it in `fpu.c`.
  2. **The image was missing everything the session needs.** A directory that
     does not exist cannot be GRANTED -- the kernel resolves an ns-bind prefix
     in the parent's namespace at spawn time -- so `/home/<user>` and `/run`
     have to be on the image before init runs, and with `/home` absent init's
     mkdir loop failed forever while the log filled with `"home" not found`.
     `mkfs_arm64.py` now IMPORTS `_SYSTEM_BIN` and `_tree_objects` from the x86
     packer rather than duplicating them, because the paths are a contract with
     init.c (which hardcodes `/system/bin/home.elf`), not a preference.

  **Two bugs found by looking at the SCREEN rather than the log:**

  1. **Five PCI devices on four interrupt lines was reported as a FAILURE.** The
     routing self-test asserted `distinct == routed`, true when this machine had
     two PCI devices and still true at three. `virt` rotates slots over the FOUR
     INTx pins, so the expected count is `min(routed, 4)`. The assertion had
     outgrown its assumption, not the code -- five distinct lines are not
     available to be wrong about.
  2. **Input worked for five seconds and then stopped.** The boot path ended in
     `for (;;) wfi`, which was right when there was nothing to pump and wrong
     the moment there was a desktop: virtio-input is POLLED, so a kernel that
     stops calling `virtio_input_poll()` has a keyboard that works for exactly
     as long as the self-test window. A screenshot taken twelve seconds in
     showed a cursor sitting where the test had left it, ignoring everything
     sent since. The boot CPU now runs `main.c`'s loop -- drain input, tick the
     compositor pointer, advance animation, idle -- for the reasons that file
     gives, chief among them that all three repaint and so must run in
     schedulable context, never from an IRQ handler.

  `arch/aarch64/drivers/pci_ecam.c`. The bus enumerates and its devices are
  addressable:

  ```
  pci: ECAM at 0x4010000000 (256 MiB) [from the device tree]
  PCI 0:0.0 1b36:8    [6.0.0] Bridge device
  PCI 0:1.0 1af4:1001 [1.0.0] Mass storage controller
  PCI 0:2.0 1af4:1000 [2.0.0] Network controller
  pci: 32-bit MMIO window 0x10000000 + 751 MiB [from the device tree]
  pci: 0:1.0 BAR4 -> 0x10000000 (16384 bytes)
  pci: 0:2.0 BAR4 -> 0x10004000 (16384 bytes)
  ```

  **`drivers/bus/pci.c` needed three seams and nothing else.** Enumeration, BAR
  sizing, the capability walk and bus mastering are all built on 32-bit config
  reads and writes, and only *how a config access reaches the bus* differs:

  | | x86_64 | aarch64 |
  |---|---|---|
  | config access | write port `0xCF8`, read `0xCFC` | **ECAM: it is memory**, `base + (bus<<20 \| dev<<15 \| fn<<12 \| off)` |
  | MSI message | LAPIC doorbell `0xFEE00000 \| apic<<12` | the GIC ITS translater — **not implemented, returns false** |

  ECAM is not merely different, it is *better*: a config read is a load, needs
  no lock, and cannot race the way a two-port address-then-data sequence can.

  Two things worth recording:
  1. **There is no firmware, so the kernel is its own PCI resource allocator.**
     Every BAR read back as **zero**. On x86 the BIOS programs them before the
     kernel exists; booting with `-kernel` on `virt` there is nobody to do it,
     the device decodes nothing, and a driver mapping BAR0 would map physical
     address zero. `pci_assign_resources()` walks the 32-bit MMIO window out of
     the host bridge's `ranges` and places each BAR on its own size boundary
     (which is not a convention — the device compares high address bits, so a
     BAR of size N can only sit on an N boundary), then enables Memory Space
     **and Bus Master**, without which a virtio device cannot complete a single
     DMA. It is deliberately not called on x86.
  2. **The assignment is read back.** A BAR's low bits are read-only, so what
     the device kept is not necessarily what was written; the code checks and
     prints `*** DID NOT TAKE ***` rather than trusting the write. The
     acceptance test fails on that string.
  3. **A PCI device's interrupt line is not in config space here.** x86 reads
     `PCI_INTERRUPT_LINE`, which firmware wrote. Nothing wrote it on this
     machine, so the routing is in the host bridge's `interrupt-map` and has to
     be decoded — a fourth seam, `arch_pci_irq_connect()`.

     **The first version of that decode looked like it worked and did not.** It
     assumed the interrupt controller had no unit address (`#address-cells = 0`,
     which is common) and used an 8-cell stride. `virt`'s GIC declares
     `#address-cells = <2>`, so entries are 10 cells; the walk misaligned after
     the first entry and every device matched the same stale bytes. The output
     was two devices, two "routed" lines, and both on SPI 0 — a passing-looking
     result with a wrong answer in it. The fix reads all four cell counts out
     of the tree, and the self-test now asserts the routed devices are on
     **distinct** lines, because `virt` wires slots in a rotating pattern and
     "N routed" was the claim that could not fail:

     ```
     pci: 0:1.0 INTA -> SPI 4 (INTID 36) [from the device tree]
     pci: 0:2.0 INTA -> SPI 5 (INTID 37) [from the device tree]
       [ ok ] 2 of 3 devices routed to the GIC, on 2 distinct line(s)
     ```
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
3. ~~**Where does `sysargs` live?**~~ **Settled at A4: a new
   `kernel/include/syscall_abi.h`.** The arch syscall header's whole reason to
   exist is `struct regs`, so putting the machine-independent contract in it
   would have meant every handler including the thing it was being freed from.
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
