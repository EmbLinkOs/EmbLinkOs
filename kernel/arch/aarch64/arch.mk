# aarch64 build fragment -- docs/ARM64.md, phases A0 onward.
#
# Included by the top-level Makefile ONLY when ARCH=aarch64, so the x86_64
# build never parses a line of it (§2.9: a second architecture must not tax the
# person who is not working on it).
#
# This settles open sub-decision §6.4 in favour of a per-arch fragment. The
# x86_64 build is deliberately NOT hoisted into a matching fragment: that would
# be pure churn today, and §2.3 says to derive the shared shape from two
# WORKING implementations rather than invent it from one. When the aarch64 side
# grows a real source list, the common parts will be visible and can move up.

AARCH64_PREFIX ?= aarch64-elf-
AARCH64_CC     := $(AARCH64_PREFIX)gcc
AARCH64_OBJCOPY:= $(AARCH64_PREFIX)objcopy

ARM_BUILD   := $(BUILD)/aarch64
ARM_ELF     := $(ARM_BUILD)/kernel.elf
ARM_IMG     := $(ARM_BUILD)/kernel.img
ARM_LINKER  := kernel/arch/aarch64/boot/linker.ld

ARM_ASM_SRC := kernel/arch/aarch64/boot/boot.S \
               kernel/arch/aarch64/irq/vectors.S \
               kernel/arch/aarch64/cpu/kcontext.S \
               kernel/arch/aarch64/syscall/el0_probe_blob.S
# The aarch64-specific sources...
ARM_C_SRC   := kernel/arch/aarch64/boot/early.c \
               kernel/arch/aarch64/boot/fdt.c \
               kernel/arch/aarch64/boot/boot_protocol_dtb.c \
               kernel/arch/aarch64/irq/exception.c \
               kernel/arch/aarch64/irq/gicv3.c \
               kernel/arch/aarch64/sched/bringup.c \
               kernel/arch/aarch64/drivers/timer_generic.c \
               kernel/arch/aarch64/cpu/spinlock.c \
               kernel/arch/aarch64/cpu/arch_thread.c \
               kernel/arch/aarch64/mm/pagetable.c \
               kernel/arch/aarch64/mm/pmm_arch.c \
               kernel/arch/aarch64/mm/usercopy.c \
               kernel/arch/aarch64/syscall/syscall.c \
               kernel/arch/aarch64/syscall/usermode.c \
               kernel/arch/aarch64/syscall/bringup_syscalls.c \
               kernel/arch/aarch64/drivers/pl011.c

# ...and the SHARED kernel, compiled for a second architecture with no #ifdef
# in it. This list is the honest measure of how portable the tree really is,
# and it is meant to grow every phase until it is most of kernel/. Anything
# that appears here and then needs an #ifdef to build is a design question, not
# a build question -- the answer is usually an arch hook, as pmm.c's
# arch_pmm_reserve_fixed() ended up being.
ARM_SHARED_SRC := kernel/mm/pmm.c \
                  kernel/mm/kheap.c \
                  kernel/mm/kmalloc.c \
                  kernel/lib/kprintf.c \
                  kernel/lib/kstring.c \
                  kernel/lib/errno.c

# Coarse on purpose, exactly as $(KERNEL_HDRS) is on the x86 side: one
# compile, no per-TU depfiles, so a header-only change must rebuild it all.
ARM_HDRS    := $(shell find kernel -name '*.h' 2>/dev/null)

# -std=gnu11 for the same reason the x86 side pins it, and known in advance
# this time rather than discovered: aarch64-elf-gcc is GCC 16, which defaults
# to gnu23, where `bool`/`true`/`false` are keywords and kernel/include/types.h
# stops compiling. docs/ARM64.md §2.8.
#
# -mgeneral-regs-only is the aarch64 analogue of the x86 side's -mno-mmx/-msse
# wall: FP and Advanced SIMD are DISABLED at EL1 out of reset (CPACR_EL1.FPEN),
# so a compiler-generated `str q0, [...]` -- gcc will happily emit one to copy a
# 16-byte struct -- traps. Before A1 there are no exception vectors, so that
# trap is an unexplained hang at an address with no obvious relation to any
# floating-point code.
#
# -fno-stack-protector: no __stack_chk_guard exists in a freestanding kernel.
# -Wall -Wextra: this tree is 300 lines old. Everything the compiler is willing
# to notice is cheaper to fix now than after it has 6,000 lines of company.
# -mno-outline-atomics: gcc 10+ compiles C11 atomics into calls to libgcc
# helpers (__aarch64_swp4_acq and friends) that pick between LSE and LL/SC at
# RUN time by reading a global. A freestanding kernel links no libgcc, so the
# first spinlock is an undefined reference -- and even if it linked, a hidden
# indirect call inside the lock path is not something a kernel wants. This
# makes gcc emit LDAXR/STXR inline, which is what the code was written to be.
ARM_CFLAGS  = -ffreestanding -nostdlib -nostartfiles \
              -std=gnu11 -mgeneral-regs-only -mno-outline-atomics \
              -fno-stack-protector \
              -Wall -Wextra \
              -Ikernel \
              -Wl,--no-warn-rwx-segments \
              -g -O0

# --no-warn-rwx-segments: the linker objects that our single LOAD segment is
# readable, writable AND executable. It is, and it must be while the MMU is
# off -- there are no page permissions to enforce until A2 builds page tables.
# The warning is aimed at userspace binaries. Delete this flag at A2 and see
# whether it is still needed; if it is, the page tables are not doing their job.

$(ARM_BUILD):
	mkdir -p $(ARM_BUILD)

# --- the EL0 probe ----------------------------------------------------------
# User code, so it is built SEPARATELY from the kernel and embedded as bytes:
# it is linked at address 0 and mapped wherever the kernel decides, which is
# exactly what an ordinary kernel object file cannot be. Same shape as the x86
# side's AP trampoline blob.
#
# -Ttext=0 rather than the kernel's link address, and -nostdlib because there
# is no libc for this architecture until A6 -- which is the whole reason this
# program is hand-written assembly.
ARM_PROBE_SRC := kernel/arch/aarch64/syscall/el0_probe.S
ARM_PROBE_BIN := $(ARM_BUILD)/el0_probe.bin

$(ARM_PROBE_BIN): $(ARM_PROBE_SRC) | $(ARM_BUILD)
	$(AARCH64_CC) -ffreestanding -nostdlib -nostartfiles \
	    -Wl,-Ttext=0 -Wl,--build-id=none -Wl,-e,_probe_start \
	    -o $(ARM_BUILD)/el0_probe.elf $<
	$(AARCH64_OBJCOPY) -O binary $(ARM_BUILD)/el0_probe.elf $@
	@echo "aarch64: EL0 probe is $$($(STATSZ) $@) bytes"

# One compile, like the x86 kernel: every header is a prerequisite because
# there are no per-TU depfiles to consult. Same reasoning as $(KERNEL_HDRS).
$(ARM_ELF): $(ARM_ASM_SRC) $(ARM_C_SRC) $(ARM_SHARED_SRC) $(ARM_HDRS) $(ARM_LINKER) $(ARM_PROBE_BIN) | $(ARM_BUILD)
	$(AARCH64_CC) $(ARM_CFLAGS) -T $(ARM_LINKER) -o $@ $(ARM_ASM_SRC) $(ARM_C_SRC) $(ARM_SHARED_SRC)

# The flat Image. QEMU boots the ELF directly, so this is not on the run path
# -- it exists because a real ARM board loads a headerless blob at a fixed
# address, and building it now keeps that assumption honest (it is why
# .text.boot is KEEP'd first in the linker script).
$(ARM_IMG): $(ARM_ELF)
	$(AARCH64_OBJCOPY) -O binary $< $@
	@# The arm64 Image magic must land at byte 56 or QEMU silently falls back
	@# to "raw blob", and the DTB pointer quietly becomes zero again -- which
	@# is a boot that LOOKS fine until A2 asks for memory. Assert it here, at
	@# the moment it can still be cheap to explain.
	@m=$$(od -An -tx1 -j56 -N4 $@ | tr -d ' \n'); \
	  if [ "$$m" != "41524d64" ]; then \
	    printf 'aarch64: Image magic at byte 56 is %s, expected 41524d64 ("ARM" 0x64).\n' "$$m" >&2; \
	    echo 'aarch64: the header in boot.S has drifted from 64 bytes.' >&2; exit 1; fi
	@echo "aarch64: Image header ok (magic at byte 56)"

.PHONY: arm64
arm64: $(ARM_ELF) $(ARM_IMG)
	@echo "aarch64 kernel: $(ARM_ELF)"
	@$(AARCH64_PREFIX)size $(ARM_ELF) 2>/dev/null || true

# ARCH=aarch64 means the aarch64 kernel is what `make` builds. The top-level
# `.DEFAULT_GOAL := all` would otherwise send us into the x86 boot image.
.DEFAULT_GOAL := arm64

# --- running ----------------------------------------------------------------
# gic-version=3 is NOT redundant. docs/ARM64.md §6.2 said "v3 is what `virt`
# gives by default" -- that is true under KVM/HVF and FALSE under TCG, where
# QEMU still defaults to GICv2 and the kernel finds no arm,gic-v3 node at all.
# Asking for it explicitly is also more honest: the machine we target is a
# GICv3 machine, and that should be visible in the command line rather than
# inherited from an accelerator's default.
ARM_MACHINE ?= virt,gic-version=3
ARM_MEM     ?= 512M

# WHICH ACCELERATOR, and why it is a per-HOST default rather than a fixed one.
#
# On Apple Silicon an aarch64 guest can run on aarch64 hardware: -accel hvf,
# which needs -cpu host because HVF cannot emulate a core it is not running on.
# Measured against TCG on the same machine, same kernel: identical wall-clock
# for a timer-bound workload (both run in real time), and ~8x the computation
# per unit of time for a CPU-bound one -- 93.4M loop iterations per scheduler
# slice against 11.6M. From A7 on, when there are pixels to composite, that is
# the difference between usable and not.
#
# Everywhere else -- including the Linux box this project is also developed on,
# where an aarch64 guest cannot be accelerated at all -- it is TCG, and nothing
# about the default changes for that host.
#
# TCG IS NOT A FALLBACK, IT IS A SECOND OPINION. `-d int,unimp` reports what the
# CPU actually did and HVF offers no equivalent, so `run-arm64-tcg` stays one
# word away. More to the point, the two disagree in ways that find bugs: A3's
# level-triggered-interrupt ordering error was invisible under TCG and obvious
# under HVF. That is why test-arm64-boot runs BOTH where both exist.
ARM_HOST_ARCH := $(shell uname -m)
ifeq ($(UNAME_S)-$(ARM_HOST_ARCH),Darwin-arm64)
ARM_ACCEL       ?= hvf
ARM_TEST_ACCELS ?= hvf tcg
else
ARM_ACCEL       ?= tcg
ARM_TEST_ACCELS ?= tcg
endif

ARM_CPU_tcg ?= cortex-a72
ARM_CPU_hvf ?= host

# -nographic wires the PL011 to this terminal's stdio (quit with Ctrl-A X).
# There is no framebuffer to show until A7, so a window would be an empty one.
#
# We boot $(ARM_IMG), NOT $(ARM_ELF). Both run, but only the flat image carries
# the arm64 Image header, and only the Image header makes QEMU hand us the
# device tree pointer in x0 -- see the long note at the top of boot.S. The ELF
# is still what you point gdb at: same addresses, plus symbols.
ARM_QEMU_hvf = qemu-system-aarch64 -M $(ARM_MACHINE),accel=hvf -cpu $(ARM_CPU_hvf) -m $(ARM_MEM)
ARM_QEMU_tcg = qemu-system-aarch64 -M $(ARM_MACHINE) -cpu $(ARM_CPU_tcg) -m $(ARM_MEM)

.PHONY: run-arm64
run-arm64: $(ARM_IMG)
	$(ARM_QEMU_$(ARM_ACCEL)) -nographic -kernel $(ARM_IMG)

# Force one or the other regardless of host.
.PHONY: run-arm64-hvf
run-arm64-hvf: $(ARM_IMG)
	$(ARM_QEMU_hvf) -nographic -kernel $(ARM_IMG)

.PHONY: run-arm64-tcg
run-arm64-tcg: $(ARM_IMG)
	$(ARM_QEMU_tcg) -nographic -kernel $(ARM_IMG)

# Interrupt/exception tracing. TCG only -- this is the capability HVF does not
# have, and the reason TCG stays a first-class target rather than a fallback.
.PHONY: debug-arm64
debug-arm64: $(ARM_IMG)
	$(ARM_QEMU_tcg) -nographic -kernel $(ARM_IMG) \
	    -d int,unimp,guest_errors -D $(ARM_BUILD)/qemu.log

# --- the acceptance test ----------------------------------------------------
# Every phase's "done when" from docs/ARM64.md, made machine-checkable so it
# stays true. Cumulative on purpose: a later phase must not quietly break an
# earlier one.
#
# Run under EVERY available accelerator, not just the default. This is not
# thoroughness for its own sake: TCG and HVF disagree, and the disagreements are
# where the bugs are. A3's interrupt-ordering error (ending a level-triggered
# interrupt before the device de-asserted) passed cleanly under TCG for an
# entire phase and failed immediately under HVF.
#
# No `timeout(1)`: it is GNU coreutils and this repo builds on macOS, where it
# is absent unless someone installed gtimeout. Backgrounding qemu and killing it
# is portable to both hosts -- the same rule the top-level Makefile applies to
# stat and truncate.
.PHONY: test-arm64-boot
test-arm64-boot: $(ARM_IMG)
	@overall=0; \
	for acc in $(ARM_TEST_ACCELS); do \
	  case $$acc in \
	    hvf) qcmd="$(ARM_QEMU_hvf)"; secs=8;;  \
	    *)   qcmd="$(ARM_QEMU_tcg)"; secs=15;; \
	  esac; \
	  log=$(ARM_BUILD)/boot-$$acc.log; rm -f $$log; \
	  echo "=== $$acc ==="; \
	  $$qcmd -display none -serial file:$$log -kernel $(ARM_IMG) 2>/dev/null & \
	  qpid=$$!; sleep $$secs; kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	  fail=0; \
	  chk() { grep -q "$$1" $$log || { echo "FAIL($$2): $$3"; fail=1; }; }; \
	  chk 'EmbLinkOS aarch64'              A0 'no banner -- PL011 or the entry point is wrong'; \
	  chk 'CurrentEL   : EL1'              A0 'not running at EL1'; \
	  chk 'fdt: blob at phys'              A0 'the DTB pointer did not survive the handoff'; \
	  chk 'VBAR_EL1 installed'             A1 'exception vectors were never installed'; \
	  chk 'BRK instruction'                A1 'brk was not decoded (ESR EC 0x3C)'; \
	  chk 'alignment fault, on a READ'     A1 'the abort was not decoded down to FSC + direction'; \
	  chk 'higher half: YES'               A2 'the kernel is not executing from the kernel window'; \
	  chk 'boot: dtb memory'               A2 'the device tree did not yield a memory map'; \
	  chk 'translation fault, on a READ'   A2 'low addresses still resolve -- identity map not dropped'; \
	  chk 'permission fault, on a WRITE'   A2 '.rodata is still writable -- section permissions are not real'; \
	  chk 'wrote and read back via the direct map' A2 'the physical allocator or the direct map is broken'; \
	  chk 'gic: initialised (GICv3'        A3 'the interrupt controller did not come up'; \
	  chk 'generic timer'                  A3 'the timer never registered an interrupt line'; \
	  chk '100 Hz tick'                    A3 'the generic timer was not programmed'; \
	  chk 'worker 1 was scheduled'         A3 'no preemption report'; \
	  chk 'boot thread was scheduled back' A3 'the scheduler never returned to the boot thread'; \
	  chk 'spurious: 0'                    A3 'the GIC delivered spurious interrupts'; \
	  chk 'kmalloc/kfree across 5 size'    A6 'the shared kernel heap does not work here'; \
	  chk 'in one space and'               A6 'address spaces are not isolated from each other'; \
	  chk 'all reclaimed'                  A6 'destroying an address space leaks pages'; \
	  chk 'hello from EL0'                 A5 'user code never ran at EL0'; \
	  chk 'REFUSED write'                  A5 'the kernel accepted an unmapped user pointer'; \
	  chk 'exited with 42'                 A5 'the exit argument did not survive the trip to a handler'; \
	  chk 'A5 reached'                     A5 'did not reach the end of arch_early_main'; \
	  n=$$(grep -c 'matches KV2P' $$log); \
	    [ "$$n" = "4" ] || { echo "FAIL(A2): $$n/4 kernel sections translate to KV2P"; fail=1; }; \
	  if grep -q 'MISMATCH' $$log; then echo "FAIL(A2): a translation does not match KV2P"; fail=1; fi; \
	  if grep -q '\[FAIL\]' $$log; then echo "FAIL: a self-test case reported failure"; fail=1; fi; \
	  if [ $$fail -ne 0 ]; then \
	    echo "--- serial ($$acc) ---"; cat $$log; echo "--- end ---"; overall=1; \
	  else \
	    grep -E 'timer fired|CNTVCT and the tick|was scheduled|spurious:' $$log | sed 's/^/  /'; \
	    echo "  PASS ($$acc)"; \
	  fi; \
	done; \
	if [ $$overall -eq 0 ]; then \
	  echo; echo "PASS on [$(ARM_TEST_ACCELS)]:"; \
	  echo "  A0 banner, EL1, DTB handoff"; \
	  echo "  A1 vectors, ESR/FAR decode, recovery"; \
	  echo "  A2 higher half, DTB memory map, pmm, section permissions, no identity map"; \
	  echo "  A3 GICv3, generic timer, preemptive context switching"; \
	  echo "  A5 EL0, svc, user-pointer boundary, arch-neutral handlers"; \
	  echo "  A6 the shared kernel heap, per-process address spaces, clean teardown"; \
	else exit 1; fi

.PHONY: check-tools-arm64
check-tools-arm64:
	@miss=0; \
	echo "EmbLinkOS aarch64 prerequisites  (make ARCH=aarch64 check-tools-arm64)"; echo; \
	for t in $(AARCH64_CC) $(AARCH64_PREFIX)ld $(AARCH64_OBJCOPY) qemu-system-aarch64; do \
	  if command -v $$t >/dev/null 2>&1; then printf '  [ ok ]  %s\n' "$$t"; \
	  else printf '  [MISS]  %s\n' "$$t"; miss=$$((miss+1)); fi; \
	done; \
	echo; \
	if [ $$miss -gt 0 ]; then \
	  echo "==> missing. macOS:  brew install aarch64-elf-gcc qemu"; \
	  echo "                Linux:  your distro's gcc-aarch64-none-elf (or crosstool-ng)"; exit 1; \
	else echo "==> ok.  Build + boot:  make ARCH=aarch64 && make ARCH=aarch64 run-arm64"; fi

.PHONY: clean-arm64
clean-arm64:
	rm -rf $(ARM_BUILD)
