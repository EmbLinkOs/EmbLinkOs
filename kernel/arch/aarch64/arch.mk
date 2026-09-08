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
               kernel/arch/aarch64/irq/vectors.S
# The aarch64-specific sources...
ARM_C_SRC   := kernel/arch/aarch64/boot/early.c \
               kernel/arch/aarch64/boot/fdt.c \
               kernel/arch/aarch64/boot/boot_protocol_dtb.c \
               kernel/arch/aarch64/irq/exception.c \
               kernel/arch/aarch64/cpu/spinlock.c \
               kernel/arch/aarch64/mm/pagetable.c \
               kernel/arch/aarch64/mm/pmm_arch.c \
               kernel/arch/aarch64/drivers/pl011.c

# ...and the SHARED kernel, compiled for a second architecture with no #ifdef
# in it. This list is the honest measure of how portable the tree really is,
# and it is meant to grow every phase until it is most of kernel/. Anything
# that appears here and then needs an #ifdef to build is a design question, not
# a build question -- the answer is usually an arch hook, as pmm.c's
# arch_pmm_reserve_fixed() ended up being.
ARM_SHARED_SRC := kernel/mm/pmm.c \
                  kernel/lib/kprintf.c \
                  kernel/lib/kstring.c

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

# One compile, like the x86 kernel: every header is a prerequisite because
# there are no per-TU depfiles to consult. Same reasoning as $(KERNEL_HDRS).
$(ARM_ELF): $(ARM_ASM_SRC) $(ARM_C_SRC) $(ARM_SHARED_SRC) $(ARM_HDRS) $(ARM_LINKER) | $(ARM_BUILD)
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
# TCG with an explicit -cpu is the DEFAULT even on an Apple Silicon host, which
# looks like leaving §2.2's headline performance win on the table. It is a
# bring-up choice, not a permanent one: under TCG, `-d int,mmu` and `-d
# unimp` actually report what the CPU did, and A1/A2 are exactly the phases
# where "it hangs" needs to become "it took a data abort at this address."
# HVF gives you neither. Once there is a desktop to be slow at, `run-arm64-hvf`
# is the target that makes this machine fast.
ARM_MACHINE ?= virt
ARM_CPU     ?= cortex-a72
ARM_MEM     ?= 512M

# -nographic wires the PL011 to this terminal's stdio (quit with Ctrl-A X).
# There is no framebuffer to show until A7, so a window would be an empty one.
#
# We boot $(ARM_IMG), NOT $(ARM_ELF). Both run, but only the flat image carries
# the arm64 Image header, and only the Image header makes QEMU hand us the
# device tree pointer in x0 -- see the long note at the top of boot.S. The ELF
# is still what you point gdb at: same addresses, plus symbols.

.PHONY: run-arm64
run-arm64: $(ARM_IMG)
	qemu-system-aarch64 -M $(ARM_MACHINE) -cpu $(ARM_CPU) -m $(ARM_MEM) \
	    -nographic -kernel $(ARM_IMG)

# Hardware-virtualized, Apple Silicon only. -cpu host is not optional: HVF
# cannot emulate a CPU model it is not running on.
.PHONY: run-arm64-hvf
run-arm64-hvf: $(ARM_IMG)
	qemu-system-aarch64 -M $(ARM_MACHINE),accel=hvf -cpu host -m $(ARM_MEM) \
	    -nographic -kernel $(ARM_IMG)

# Interrupt/exception tracing, for when something hangs and A1's vectors are
# not written yet (or are the thing that is broken).
.PHONY: debug-arm64
debug-arm64: $(ARM_IMG)
	qemu-system-aarch64 -M $(ARM_MACHINE) -cpu $(ARM_CPU) -m $(ARM_MEM) \
	    -nographic -kernel $(ARM_IMG) -d int,unimp,guest_errors -D $(ARM_BUILD)/qemu.log

# --- the acceptance test ----------------------------------------------------
# Every phase's "done when" from docs/ARM64.md, made machine-checkable so it
# stays true. Cumulative on purpose: A1 must not quietly break A0. No `timeout(1)`: it is GNU coreutils and this repo builds on macOS,
# where it is absent unless someone installed gtimeout. Backgrounding qemu and
# killing it is portable to both hosts, which is the same rule the top-level
# Makefile applies to stat and truncate.
.PHONY: test-arm64-boot
test-arm64-boot: $(ARM_IMG)
	@log=$(ARM_BUILD)/boot.log; rm -f $$log; \
	qemu-system-aarch64 -M $(ARM_MACHINE) -cpu $(ARM_CPU) -m $(ARM_MEM) \
	    -display none -serial file:$$log -kernel $(ARM_IMG) & \
	qpid=$$!; sleep 5; kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	echo "--- serial ---"; cat $$log; echo "--- end ---"; \
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
	chk 'self-test done: 0 failure'      A2 'a self-test case failed'; \
	chk 'A2 reached'                     A2 'did not reach the end of arch_early_main'; \
	n=$$(grep -c 'matches KV2P' $$log); \
	  [ "$$n" = "4" ] || { echo "FAIL(A2): $$n/4 kernel sections translate to KV2P"; fail=1; }; \
	if grep -q 'MISMATCH' $$log; then echo "FAIL(A2): a translation does not match KV2P"; fail=1; fi; \
	if grep -q '\[FAIL\]' $$log; then echo "FAIL: a self-test case reported failure"; fail=1; fi; \
	if [ $$fail -eq 0 ]; then \
	  echo "PASS: A0 (banner, EL1, DTB handoff)"; \
	  echo "      A1 (vectors, ESR/FAR decode, recovery)"; \
	  echo "      A2 (higher half, DTB memory map, pmm, section permissions, no identity map)"; \
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
