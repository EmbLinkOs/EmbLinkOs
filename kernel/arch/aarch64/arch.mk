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
# Declared HERE, with the other output paths, and not beside the rule that
# builds it further down: make expands a rule's prerequisites when it READS the
# rule, so `arm64: ... $(ARM_ROOTFS)` below would expand to NOTHING if this line
# came after it. It did, and `make ARCH=aarch64` on a clean tree built a kernel
# with no userland and no disk for it to read -- which then failed only at run
# time, as "no EMBKFS volume".
ARM_ROOTFS  := $(ARM_BUILD)/embkfs-arm64.img
ARM_LINKER  := kernel/arch/aarch64/boot/linker.ld

ARM_ASM_SRC := kernel/arch/aarch64/boot/boot.S \
               kernel/arch/aarch64/irq/vectors.S \
               kernel/arch/aarch64/cpu/kcontext.S \

# The aarch64-specific sources...
ARM_C_SRC   := kernel/arch/aarch64/boot/early.c \
               kernel/arch/aarch64/boot/fdt.c \
               kernel/arch/aarch64/boot/boot_protocol_dtb.c \
               kernel/arch/aarch64/irq/exception.c \
               kernel/arch/aarch64/irq/gicv3.c \
               kernel/arch/aarch64/sched/bringup.c \
               kernel/arch/aarch64/drivers/timer_generic.c \
               kernel/arch/aarch64/drivers/pci_ecam.c \
               kernel/arch/aarch64/drivers/pl031.c \
               kernel/arch/aarch64/drivers/absent.c \
               kernel/arch/aarch64/cpu/spinlock.c \
               kernel/arch/aarch64/cpu/arch_thread.c \
               kernel/arch/aarch64/cpu/percpu.c \
               kernel/arch/aarch64/mm/pagetable.c \
               kernel/arch/aarch64/mm/pmm_arch.c \
               kernel/arch/aarch64/mm/usercopy.c \
               kernel/arch/aarch64/syscall/syscall.c \
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
                  kernel/lib/errno.c \
                  kernel/lib/ksym.c \
                  kernel/drivers/bus/pci.c \
                  kernel/drivers/storage/virtio_blk.c \
                  kernel/block/block.c \
                  kernel/block/partition.c \
                  kernel/process/process.c \
                  kernel/process/ksync.c \
                  kernel/process/debug.c \
                  kernel/loader/elf.c \
                  kernel/loader/embx.c \
                  kernel/syscall/syscalls.c \
                  kernel/fs/vfs.c \
                  kernel/fs/fd.c \
                  kernel/fs/namespace.c \
                  kernel/fs/epfs.c \
                  kernel/fs/embkfs/embkfs.c \
                  kernel/fs/embkfs/embkfs_compress.c \
                  kernel/fs/embkfs/crc32c.c \
                  kernel/fs/embkfs/embk_vfs.c \
                  kernel/fs/fat32.c \
                  kernel/ipc/handle.c \
                  kernel/ipc/channel.c \
                  kernel/ipc/clipboard.c \
                  kernel/ipc/endpoint.c \
                  kernel/ipc/pipe.c \
                  kernel/gfx/surface.c \
                  kernel/gfx/compositor.c \
                  kernel/kworker/kworker.c \
                  kernel/tty/tty.c \
                  kernel/acpi/acpi.c \
                  kernel/drivers/audio/audio.c \
                  kernel/crypto/sha256.c \
                  kernel/crypto/hmac.c \
                  kernel/crypto/pbkdf2.c \
                  kernel/crypto/aes.c \
                  kernel/crypto/xts.c \
                  user/lib/tls/crypto/hkdf.c \
                  user/lib/tls/crypto/gcm.c \
                  user/lib/tls/crypto/x25519.c \
                  user/lib/tls/crypto/selftest.c \
                  kernel/drivers/bus/virtio_pci.c \
                  kernel/drivers/input/virtio_input.c \
                  kernel/drivers/input/keyboard.c \
                  kernel/drivers/input/mouse.c \
                  kernel/drivers/video/framebuffer.c \
                  kernel/drivers/video/console.c \
                  kernel/drivers/video/font_8x16.c \
                  kernel/drivers/video/gpu.c \
                  kernel/drivers/video/bootanim.c \
                  kernel/drivers/video/virtio_gpu.c \
                  kernel/net/net.c \
                  kernel/net/virtio_net.c \
                  kernel/net/ethernet/eth.c \
                  kernel/net/ethernet/arp.c \
                  kernel/net/ip/ipv4.c \
                  kernel/net/ip/icmp.c \
                  kernel/net/udp/udp.c \
                  kernel/net/dhcp/dhcp.c \
                  kernel/net/dns/dns.c \
                  kernel/net/tcp/tcp.c

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
arm64: $(ARM_ELF) $(ARM_IMG) $(ARM_ROOTFS)
	@echo "aarch64 kernel: $(ARM_ELF)"
	@$(AARCH64_PREFIX)size $(ARM_ELF) 2>/dev/null || true

# ARCH=aarch64 means the aarch64 kernel is what `make` builds. The top-level
# `.DEFAULT_GOAL := all` would otherwise send us into the x86 boot image.
.DEFAULT_GOAL := arm64

# --- A6: the userland -------------------------------------------------------
# docs/ARM64.md phase A6. user/lib is ONE implementation for both machines --
# embk_syscall.h gained an `svc #0` branch, crt0.c an aarch64 _start and
# variant-I TLS -- so there is nothing here but paths and a pattern rule. The
# COMPILER FLAGS are the top-level Makefile's ($(NEWLIB_CFLAGS),
# $(NEWLIB_LDFLAGS), $(USER_CFLAGS)), which now derive from $(USER_TRIPLE); they
# are deliberately NOT restated here, because two lists of userland flags is how
# the two architectures start drifting apart.
#
# The objects live under $(ARM_BUILD)/user rather than build/, and that is not
# tidiness: `build/crt0.o` cannot mean both x86 and aarch64 machine code, and a
# shared path would make `make` hand whichever was built last to the other
# architecture's linker. The top-level Makefile hardcodes `build/` in 372
# places; redirecting all of them is churn with a real chance of breaking the
# x86 image, so the second architecture takes a subdirectory instead.
ARM_USER      := $(ARM_BUILD)/user
ARM_CRT0      := $(ARM_USER)/crt0.o
ARM_SYSCALLS  := $(ARM_USER)/syscalls.o

# The programs built for aarch64. This is SHORT ON PURPOSE and is the honest
# statement of how much userland the second architecture has: hello.elf is a
# real newlib program (printf, malloc, time, a native thread) and is the A6
# witness; init.elf is the freestanding pid-1. The GUI apps are absent because
# libembk.so and the compositor are A7 work -- see docs/TODO.md. Add a name here
# and it builds; nothing else needs editing.
# Static newlib console programs (-T newlib.ld, no libembk.so).
ARM_NEWLIB_PROGS ?= hello beep capchild capfs capgpu capreload capspawn \
                    crasher ioracer sockdemo udptest nbsock httpget \
                    tlstest wget pkgfetch pkg pkgbuild httpd \
                    posixdemo

# Freestanding: own _start, no libc at all (-T user.ld).
ARM_PLAIN_PROGS  ?= init primtest

# The dynamically-linked EmUI apps. DERIVED FROM THE x86 LIST rather than
# retyped: $(EMUI_APP_SRCS) is the top-level Makefile's auto-discovery of
# user/bin/*.c minus the programs that are not EmUI apps, and it is defined
# before this fragment is included. Deriving it means a new app appears on BOTH
# architectures the moment it is dropped in user/bin -- which is the whole point
# of that auto-discovery, and it would have been lost by keeping a second list
# here that someone has to remember to update.
#
# TWO SUBTRACTIONS. $(EMUI_APP_SRCS) is "everything that is not on the
# exclusion list", and on x86 two of the survivors are still not dynamic EmUI
# apps -- they are rescued by EXPLICIT rules that shadow the generic pattern,
# which is knowledge the variable itself does not carry:
#   beep      is a STATIC newlib console program (-T newlib.ld), below.
#   primtest  is FREESTANDING, with its own _start and no main (-T user.ld).
# Left in, primtest fails at link with "undefined reference to `main'" from
# crt0 -- which is a true statement about a program that has none.
#
# Override on the command line to build a subset:  make ARCH=aarch64 ARM_UI_PROGS=uidemo
# Programs that link emlibc INSTEAD of newlib -- see the emlibc section below.
# Declared here with the other lists, and not beside their rules, because
# ARM_USER_ELVES is a `:=` assignment: a name defined later expands to nothing
# and the program silently never builds. This fragment has produced that bug
# four times now.
# mathself belongs HERE, not with the newlib programs: it is an emlibc program
# (its open()/O_WRONLY come from emlibc's <unistd.h>, where POSIX would put them
# in <fcntl.h>), which is why x86 builds it only through EmbCC and never as a
# newlib ELF. Putting it in the newlib list compiled it against newlib's headers
# and failed on O_WRONLY -- and "fixing" that with #include <fcntl.h> then broke
# the EmbCC build, whose header set has no fcntl.h. The list was the bug.
ARM_EMLIBC_PROGS ?= emlibc_demo emlibc_net emlibc_caps emlibc_math \
                    emlibc_embxapp mathself

ARM_UI_PROGS     ?= $(filter-out beep primtest,\
                      $(patsubst user/bin/%.c,%,$(EMUI_APP_SRCS))) \
                    photos mp3play

# A few apps are more than one translation unit, exactly as they are on x86
# (where each has an explicit rule with extra objects). Naming the extra
# SOURCES per app keeps that fact in one place instead of a second explicit
# rule per app down here.
ARM_XSRC_home   := user/lib/appauth.c
ARM_XSRC_notepp := user/note/syntax.c user/note/doc.c user/note/edit.c

# The TLS stack, as SOURCES rather than the x86 side's object list, because the
# objects are built into a different directory here. Same files, same order.
ARM_TLS_SRC := kernel/crypto/sha256.c kernel/crypto/hmac.c kernel/crypto/aes.c \
               user/lib/tls/crypto/hkdf.c user/lib/tls/crypto/gcm.c \
               user/lib/tls/crypto/x25519.c user/lib/tls/crypto/sha512.c \
               user/lib/tls/crypto/bignum.c user/lib/tls/crypto/ecdsa.c \
               user/lib/tls/crypto/rsa.c user/lib/tls/x509/asn1.c \
               user/lib/tls/x509/cert.c user/lib/tls/x509/trust.c \
               user/lib/tls/keysched.c user/lib/tls/record.c \
               user/lib/tls/handshake.c user/lib/tls/tls.c user/lib/tls/prf12.c \
               user/lib/tls/record12.c user/lib/tls/tls12.c \
               user/lib/tls/tls_handle.c

# mp3_test.c and mp3dec.c are excluded because each has its OWN main(): they
# are standalone host tools that live in the decoder's directory, not part of
# the decoder. The x86 side spells the eleven decoder objects out one by one
# and so never had to notice; a wildcard does, and links two extra main()s into
# the app if it does not.
ARM_MP3_SRC := $(filter-out user/audio/mp3/mp3_test.c user/audio/mp3/mp3dec.c,\
                 $(wildcard user/audio/mp3/*.c)) \
               user/audio/resample.c

ARM_XSRC_photos   := user/photos/decode.c user/photos/resample.c \
                     user/photos/album.c user/web/png.c user/web/jpeg.c \
                     user/lib/inflate.c
ARM_XSRC_mp3play  := $(ARM_MP3_SRC)
ARM_XSRC_tlstest  := $(ARM_TLS_SRC)
ARM_XSRC_wget     := $(ARM_TLS_SRC)
ARM_XSRC_pkgfetch := user/lib/inflate.c user/lib/unzip.c $(ARM_TLS_SRC)
ARM_XSRC_pkg      := user/pkg/manifest.c user/pkg/embxinfo.c \
                     kernel/crypto/sha256.c user/lib/tls/crypto/ecdsa.c \
                     user/lib/tls/crypto/bignum.c
ARM_XSRC_pkgbuild := user/pkg/embxgen.c user/pkg/manifest.c kernel/crypto/sha256.c
ARM_XSRC_httpd    := user/httpd/http.c user/httpd/mime.c user/httpd/serve.c

# Per-app include paths. The EmUI set is added to every app anyway; this is the
# extra a particular app needs, mirroring its explicit x86 rule.
# Taken from each app's x86 compile rule, and reusing the Makefile's OWN
# $(TLS_LIB_INC) rather than a second spelling of it -- the TLS tree's kshim
# and -Ikernel paths are not obvious and are exactly the kind of thing two
# copies would drift on.
ARM_INC_photos   := -Iuser/photos -Iuser/web
ARM_INC_mp3play  := -Iuser/audio/mp3 -Iuser/audio
ARM_INC_tlstest  := $(TLS_LIB_INC)
ARM_INC_wget     := $(TLS_LIB_INC)
ARM_INC_pkgfetch := $(TLS_LIB_INC) -Iuser/lib -Iuser/pkg
ARM_INC_pkg      := -Iuser/pkg -Iuser/lib -Iuser/lib/tls/crypto -Ikernel
ARM_INC_pkgbuild := -Iuser/pkg -Iuser/lib
ARM_INC_httpd    := -Iuser/httpd


# The EmUI apps. Same link shape as x86's: NO -static (it would forbid the .so)
# and NO -T newlib.ld (the DEFAULT script is what emits the .dynamic/.dynsym/
# .rela.plt/.got the in-kernel loader reads). libembk.so comes before -lc -lm so
# ld pulls the libc the toolkit needs INTO the app, and --export-dynamic exports
# it back to the .so. --no-dynamic-linker because the kernel is the loader.
# Every extra source any app needs, deduplicated -- two apps may share one.
ARM_XSRC_ALL := $(sort $(foreach p,$(ARM_UI_PROGS) $(ARM_NEWLIB_PROGS),$(ARM_XSRC_$(p))))
# The union of every per-app include, used when compiling a SHARED extra object
# (one file may be pulled in by two apps -- kernel/crypto/sha256.c is pulled in
# by three -- so it is compiled once with the superset rather than several
# times with different sets).
#
# NOT $(sort). Sorting deduplicates, which would be welcome, but it also
# REORDERS, and -I order is significant: $(TLS_LIB_INC) puts
# user/lib/tls/kshim ahead of -Ikernel precisely so the shim's headers shadow
# the kernel's for userspace builds. Sorted alphabetically, -Ikernel wins and
# the TLS tree compiles against the wrong headers. Duplicate -I flags are
# harmless; a reordered one is not.
ARM_INC_ALL  := $(foreach p,$(ARM_UI_PROGS) $(ARM_NEWLIB_PROGS),$(ARM_INC_$(p)))
ARM_XOBJ      = $(patsubst %,$(ARM_USER)/x_%.o,$(subst /,_,$(basename $(1))))

define ARM_XOBJ_RULE
$(ARM_USER)/x_$(subst /,_,$(basename $(1))).o: $(1) | $(ARM_USER)
	$$(USER_CC) $$(NEWLIB_CFLAGS) $$(UIDEMO_INC) -Iuser/note $(ARM_INC_ALL) -c $$< -o $$@
endef
$(foreach src,$(ARM_XSRC_ALL),$(eval $(call ARM_XOBJ_RULE,$(src))))

ARM_USER_ELVES := $(patsubst %,$(ARM_USER)/%.elf,$(ARM_NEWLIB_PROGS)) \
                  $(patsubst %,$(ARM_USER)/%.elf,$(ARM_PLAIN_PROGS)) \
                  $(patsubst %,$(ARM_USER)/%.elf,$(ARM_UI_PROGS))

$(ARM_USER):
	mkdir -p $(ARM_USER)

# The retargeting layer, once.
$(ARM_USER)/crt0.o: user/lib/crt0.c | $(ARM_USER)
	$(USER_CC) $(NEWLIB_CFLAGS) -c $< -o $@
$(ARM_USER)/syscalls.o: user/lib/syscalls.c | $(ARM_USER)
	$(USER_CC) $(NEWLIB_CFLAGS) -c $< -o $@

# EXPLICIT rules, generated per program, NOT pattern rules -- and that is a
# correctness requirement, not a style. The top-level Makefile defines
#
#     build/%.elf: build/%.o build/crt0.o build/syscalls.o build/libembk.so
#
# for the dynamically-linked EmUI apps, and `build/aarch64/user/hello.elf`
# MATCHES it with the stem `aarch64/user/hello`. It won, too: the first dry run
# of this fragment linked hello.elf against build/crt0.o (x86 machine code) and
# an aarch64 libembk.so it had helpfully just built. An explicit rule always
# takes precedence over a pattern rule, so generating one per program is what
# makes the outcome depend on nothing but this file.
define ARM_NEWLIB_PROG
$(ARM_USER)/$(1).o: user/bin/$(1).c | $(ARM_USER)
	$$(USER_CC) $$(NEWLIB_CFLAGS) $(ARM_INC_$(1)) -c $$< -o $$@
$(ARM_USER)/$(1).elf: $(ARM_USER)/$(1).o $(call ARM_XOBJ,$(ARM_XSRC_$(1))) \
                      $(ARM_CRT0) $(ARM_SYSCALLS) user/lib/newlib.ld
	$$(USER_CC) $$(NEWLIB_LDFLAGS) $(ARM_CRT0) $(ARM_SYSCALLS) \
	    $(ARM_USER)/$(1).o $(call ARM_XOBJ,$(ARM_XSRC_$(1))) -lc -lm -lgcc -o $$@
endef
$(foreach p,$(ARM_NEWLIB_PROGS),$(eval $(call ARM_NEWLIB_PROG,$(p))))

# Freestanding programs (init.elf): own _start, no libc, linked with the raw ld
# against user.ld exactly as x86 does -- $(USER_LD) is aarch64-elf-ld here.
define ARM_PLAIN_PROG
$(ARM_USER)/$(1).plain.o: user/bin/$(1).c | $(ARM_USER)
	$$(USER_CC) $$(USER_CFLAGS) -c $$< -o $$@
$(ARM_USER)/$(1).elf: $(ARM_USER)/$(1).plain.o user/lib/user.ld
	$$(USER_LD) -T user/lib/user.ld -z max-page-size=0x1000 $$< -o $$@
endef
$(foreach p,$(ARM_PLAIN_PROGS),$(eval $(call ARM_PLAIN_PROG,$(p))))

.PHONY: arm64-user
arm64-user: $(ARM_USER_ELVES)
	@echo "aarch64 userland:"; for f in $(ARM_USER_ELVES); do \
	  printf '  %-40s %s bytes\n' "$$f" "$$(wc -c < $$f | tr -d ' ')"; done

# --- A6 + A7: libembk.so and the dynamically-linked EmUI apps ---------------
# A6's "done when" is a dynamically-linked EmUI app loading, and it needed A7's
# display first: an app with nothing to draw on is not a test of anything.
#
# The TOOLKIT is `ui/` -- scene graph, CPU raster backend, font, layout,
# reactive, declare, theme, kit, the DSL -- plus user/lib/auth.c, built -fPIC
# into one shared object. It is exactly the x86 object list, which is why it is
# derived from ONE list of sources here rather than the twelve near-identical
# three-line rules the x86 side still spells out.
#
# The kernel is the dynamic loader (there is no ld.so and no PT_INTERP), and it
# already understood aarch64: elf.h has carried EM_AARCH64 and the R_AARCH64_*
# set behind the neutral ELF_RELOC_* names since A4.
ARM_LIBEMBK_SRC := ui/scene/scene.c ui/backend/cpu_backend.c ui/backend/font.c \
                   ui/backend/scene_render.c ui/layout/layout.c \
                   ui/reactive/reactive.c ui/declare/declare.c \
                   ui/theme/theme.c ui/kit/kit.c ui/dsl/em.c ui/dsl/em_app.c \
                   user/lib/auth.c

# build/aarch64/user/pic_ui_scene_scene.o -- the source path flattened, so two
# files with the same basename in different directories cannot collide.
ARM_LIBEMBK_OBJ := $(patsubst %,$(ARM_USER)/pic_%.o,\
                     $(subst /,_,$(basename $(ARM_LIBEMBK_SRC))))
ARM_LIBEMBK     := $(ARM_USER)/libembk.so

define ARM_PIC_OBJ
$(ARM_USER)/pic_$(subst /,_,$(basename $(1))).o: $(1) | $(ARM_USER)
	$$(USER_CC) $$(NEWLIB_CFLAGS) -fPIC $$(UIDEMO_INC) -c $$< -o $$@
endef
$(foreach src,$(ARM_LIBEMBK_SRC),$(eval $(call ARM_PIC_OBJ,$(src))))

$(ARM_LIBEMBK): $(ARM_LIBEMBK_OBJ)
	$(USER_LD) -shared -soname libembk.so --hash-style=sysv $(ARM_LIBEMBK_OBJ) -o $@


define ARM_UI_PROG
$(ARM_USER)/$(1).o: user/bin/$(1).c user/lib/embk.h | $(ARM_USER)
	$$(USER_CC) $$(NEWLIB_CFLAGS) $$(UIDEMO_INC) -Iuser/note $(ARM_INC_$(1)) -c $$< -o $$@
$(ARM_USER)/$(1).elf: $(ARM_USER)/$(1).o $(call ARM_XOBJ,$(ARM_XSRC_$(1))) \
                      $(ARM_CRT0) $(ARM_SYSCALLS) $(ARM_LIBEMBK)
	$$(USER_CC) $$(NEWLIB_DYN_LDFLAGS) $(ARM_CRT0) $(ARM_SYSCALLS) \
	    $(ARM_USER)/$(1).o $(call ARM_XOBJ,$(ARM_XSRC_$(1))) \
	    $(ARM_LIBEMBK) -lc -lm -lgcc $$(NEWLIB_DYN_WL) -o $$@
endef
$(foreach p,$(ARM_UI_PROGS),$(eval $(call ARM_UI_PROG,$(p))))

# --- emlibc: the ALTERNATIVE libc, and the programs that link it -------------
# user/emlibc is a second C library -- not a wrapper over newlib but a
# replacement for it -- and its headers DELIBERATELY SHADOW newlib's. That is
# why it cannot simply join the app lists above: it compiles -nostdinc against
# its own include tree, links no -lc and no crt0.o/syscalls.o, and its programs
# take neither the static-newlib nor the dynamic-EmUI shape.
#
# Adding -Iuser/emlibc/include to the ordinary apps instead is the mistake that
# looks like the fix -- it breaks every newlib program in the tree, because
# emlibc's <stdio.h> is then the one they get.
#
# The flags are the x86 rules' with the x86 out of them: no -mno-red-zone
# (AAPCS64 has no red zone) and no SSE note, everything else identical.
EMLIBC_DIR       := user/emlibc
ARM_GCC_FREEINC  := $(shell $(USER_CC) -print-file-name=include)
ARM_EMLIBC_INC   := -nostdinc -isystem $(ARM_GCC_FREEINC) \
                    -I$(EMLIBC_DIR)/include -Iuser/lib
ARM_EMLIBC_CFLAGS = -MMD -MP -MF $@.d -std=c99 -ffreestanding -fno-builtin \
                    -fno-stack-protector -O2 -Wall -Wextra $(ARM_EMLIBC_INC)

# fdlibm is vendored third-party (Sun's ~1-ulp library, the source newlib's
# libm is built from). It compiles -w on purpose: we do not "fix" third-party
# warnings.
ARM_EMLIBC_FD_DIR := $(EMLIBC_DIR)/math/fdlibm
ARM_EMLIBC_FD_SRC := $(wildcard $(ARM_EMLIBC_FD_DIR)/*.c)
ARM_EMLIBC_FD_CFLAGS = -MMD -MP -MF $@.d -std=c99 -ffreestanding -fno-builtin \
                       -fno-stack-protector -O2 -w -nostdinc \
                       -isystem $(ARM_GCC_FREEINC) -I$(EMLIBC_DIR)/include \
                       -Iuser/lib -I$(ARM_EMLIBC_FD_DIR)

ARM_EMLIBC_SRC := $(EMLIBC_DIR)/string/string.c $(EMLIBC_DIR)/stdlib/stdlib.c \
                  $(EMLIBC_DIR)/stdio/stdio.c $(EMLIBC_DIR)/rim/syscalls.c \
                  $(EMLIBC_DIR)/rim/errno.c $(EMLIBC_DIR)/process/process.c \
                  $(EMLIBC_DIR)/math/math.c $(EMLIBC_DIR)/net/net.c

ARM_EMLIBC_OBJ := $(patsubst %,$(ARM_USER)/em_%.o,$(subst /,_,$(basename $(ARM_EMLIBC_SRC))))
ARM_EMLIBC_FD_OBJ := $(patsubst $(ARM_EMLIBC_FD_DIR)/%.c,$(ARM_USER)/emfd_%.o,$(ARM_EMLIBC_FD_SRC))
ARM_LIBEMLIBC  := $(ARM_USER)/libemlibc.a

define ARM_EMLIBC_OBJ_RULE
$(ARM_USER)/em_$(subst /,_,$(basename $(1))).o: $(1) | $(ARM_USER)
	$$(USER_CC) $$(ARM_EMLIBC_CFLAGS) -c $$< -o $$@
endef
$(foreach src,$(ARM_EMLIBC_SRC),$(eval $(call ARM_EMLIBC_OBJ_RULE,$(src))))

$(ARM_USER)/emfd_%.o: $(ARM_EMLIBC_FD_DIR)/%.c | $(ARM_USER)
	$(USER_CC) $(ARM_EMLIBC_FD_CFLAGS) -c $< -o $@

$(ARM_LIBEMLIBC): $(ARM_EMLIBC_OBJ) $(ARM_EMLIBC_FD_OBJ)
	$(AARCH64_PREFIX)ar rcs $@ $(ARM_EMLIBC_OBJ) $(ARM_EMLIBC_FD_OBJ)

# emlibc's own crt0: the SAME source as newlib's, compiled against emlibc's
# headers so exit/malloc/environ resolve to emlibc's.
$(ARM_USER)/emlibc_crt0.o: user/lib/crt0.c | $(ARM_USER)
	$(USER_CC) $(ARM_EMLIBC_CFLAGS) -c $< -o $@

# Each program's object name differs from its source basename in two cases,
# both inherited from x86: emlibc_demo builds from emlibc_demo.c but the rim's
# net.c already owns the name emlibc_net.o, and emlibc_math's object is
# .elf.o. Naming them here keeps that quirk in one place.
ARM_EMSRC_emlibc_net := user/bin/emlibc_net.c

define ARM_EMLIBC_PROG
$(ARM_USER)/app_$(1).o: $(if $(ARM_EMSRC_$(1)),$(ARM_EMSRC_$(1)),user/bin/$(1).c) | $(ARM_USER)
	$$(USER_CC) $$(ARM_EMLIBC_CFLAGS) -c $$< -o $$@
$(ARM_USER)/$(1).elf: $(ARM_USER)/emlibc_crt0.o $(ARM_USER)/app_$(1).o \
                      $(ARM_LIBEMLIBC) user/lib/newlib.ld
	$$(USER_CC) -nostdlib -static -T user/lib/newlib.ld \
	    -Wl,-z,max-page-size=0x1000 \
	    $(ARM_USER)/emlibc_crt0.o $(ARM_USER)/app_$(1).o \
	    -L$(ARM_USER) -lemlibc -lgcc -o $$@
endef
$(foreach p,$(ARM_EMLIBC_PROGS),$(eval $(call ARM_EMLIBC_PROG,$(p))))

# --- A6: the root filesystem ------------------------------------------------
# A SEPARATE, MINIMAL image, not the x86 embkfs.img. tools/embkfs_mkfs's main
# script packs the whole x86 desktop -- icons, wallpapers, music, CPython, tcc,
# the newlib headers for on-OS compilation -- none of which exists for aarch64
# yet, and most of which is machine code that would be silently wrong here. The
# arm64 packer reuses the SAME make_image()/build_root_items() formatter (so the
# on-disk format cannot drift between the two) and simply gives it a shorter
# object list.
$(ARM_ROOTFS): tools/embkfs_mkfs/mkfs_arm64.py $(ARM_USER_ELVES) $(ARM_LIBEMBK) | $(ARM_BUILD)
	python3 tools/embkfs_mkfs/mkfs_arm64.py $@ $(ARM_USER)

.PHONY: arm64-rootfs
arm64-rootfs: $(ARM_ROOTFS)

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

# The root filesystem is attached on every run target: without a disk there is
# no /system/bin/hello.elf, so the kernel reaches A6 and has nothing to run.
ARM_DISK = -drive file=$(ARM_ROOTFS),format=raw,if=none,id=d0 \
           -device virtio-blk-pci,drive=d0

# The display. `virt` has no VGA and no firmware framebuffer -- there is no
# legacy path to fall back to, so if this device is absent the machine simply
# has no screen (ARM64.md §6.5 settled: virtio-gpu only).
ARM_GPU  = -device virtio-gpu-pci

# Input. TWO devices, because a keyboard and a pointer are two virtio-input
# functions; the driver tells them apart by asking each what events it can
# produce. The TABLET rather than virtio-mouse on purpose: it reports ABSOLUTE
# coordinates, so the guest cursor tracks the host's 1:1 and never "escapes"
# the window the way a relative device does.
ARM_INPUT = -device virtio-keyboard-pci -device virtio-tablet-pci

.PHONY: run-arm64
run-arm64: $(ARM_IMG) $(ARM_ROOTFS)
	$(ARM_QEMU_$(ARM_ACCEL)) -nographic $(ARM_DISK) $(ARM_GPU) $(ARM_INPUT) -kernel $(ARM_IMG)

# Force one or the other regardless of host.
.PHONY: run-arm64-hvf
run-arm64-hvf: $(ARM_IMG) $(ARM_ROOTFS)
	$(ARM_QEMU_hvf) -nographic $(ARM_DISK) $(ARM_GPU) $(ARM_INPUT) -kernel $(ARM_IMG)

.PHONY: run-arm64-tcg
run-arm64-tcg: $(ARM_IMG) $(ARM_ROOTFS)
	$(ARM_QEMU_tcg) -nographic $(ARM_DISK) $(ARM_GPU) $(ARM_INPUT) -kernel $(ARM_IMG)

# Interrupt/exception tracing. TCG only -- this is the capability HVF does not
# have, and the reason TCG stays a first-class target rather than a fallback.
.PHONY: debug-arm64
debug-arm64: $(ARM_IMG) $(ARM_ROOTFS)
	$(ARM_QEMU_tcg) -nographic $(ARM_DISK) $(ARM_GPU) $(ARM_INPUT) -kernel $(ARM_IMG) \
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
# Each accelerator boots a COPY of the root filesystem, never the original.
# posixdemo WRITES -- mkdir, create, rename, unlink -- so a run mutates the
# image, and without a scratch copy the second accelerator inherits the first
# one's leftovers and a re-run is not the same test as the first. It showed up
# as an EMBKFS free-block count that disagreed with the superblock by one.
.PHONY: test-arm64-boot
test-arm64-boot: $(ARM_IMG) $(ARM_ROOTFS)
	@overall=0; \
	for acc in $(ARM_TEST_ACCELS); do \
	  case $$acc in \
	    hvf) qcmd="$(ARM_QEMU_hvf)"; secs=40;;  \
	    *)   qcmd="$(ARM_QEMU_tcg)"; secs=90;; \
	  esac; \
	  log=$(ARM_BUILD)/boot-$$acc.log; rm -f $$log; \
	  echo "=== $$acc ==="; \
	  scratch=$(ARM_BUILD)/rootfs-$$acc.img; \
	  cp $(ARM_ROOTFS) $$scratch; \
	  disk="-drive file=$$scratch,format=raw,if=none,id=d0 -device virtio-blk-pci,drive=d0"; \
	  port=$$(awk 'BEGIN{srand();print 4500+int(rand()*400)}'); \
	  $$qcmd $$disk $(ARM_GPU) $(ARM_INPUT) -display none -serial file:$$log \
	      -qmp tcp:127.0.0.1:$$port,server,nowait -kernel $(ARM_IMG) 2>/dev/null & \
	  qpid=$$!; \
	  python3 tools/arm64_input_probe.py $$port $$secs >/dev/null 2>&1 & \
	  ipid=$$!; \
	  sleep $$secs; kill $$qpid 2>/dev/null; wait $$qpid 2>/dev/null; \
	  kill $$ipid 2>/dev/null; wait $$ipid 2>/dev/null; \
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
	  chk 'pci: ECAM at'                   A7 'the PCIe host bridge was not found in the device tree'; \
	  chk 'Network controller'             A7 'PCIe enumeration found no virtio device'; \
	  chk 'pci: assigned'                  A7 'no BAR was assigned -- there is no firmware to do it here'; \
	  chk 'virtio-blk: sda'              A7 'the virtio-blk device did not come up'; \
	  chk 'written and read back'        A7 'the disk failed a write/read round trip'; \
	  chk 'EMBKFS: sda: mounted'         A7 'the real filesystem did not mount'; \
	  chk 'ELF magic intact'             A7 'could not read a file out of the mounted image'; \
	  chk 'distinct line'                  A7 'PCI interrupt routing was not exercised'; \
	  chk 'all reclaimed'                  A6 'destroying an address space leaks pages'; \
	  chk 'hello.elf launched as pid'      A6 'the kernel could not create a user process'; \
	  chk 'hello from newlib'              A6 'printf never reached the console -- write(2) or the retargeting layer'; \
	  chk 'argv0=/system/bin/hello.elf'    A6 'argc/argv did not survive the transition to EL0'; \
	  chk 'malloc/free of 4096 bytes'      A6 'malloc/sbrk does not work in ring 3'; \
	  chk 'snprintf: OK'                   A6 'libc formatting is broken (C99 formats?)'; \
	  chk 'plausible wall clock'           A6 'time() did not reach the RTC'; \
	  chk 'embk thread create/join: OK'    A6 'a second EL0 thread could not be created or joined'; \
	  chk 'hello: 5/5 checks passed'       A6 'the userland witness did not pass every check'; \
	  chk 'exited with 5 (checks passed)'  A6 'the process did not exit cleanly with its status'; \
	  chk 'posixdemo: ALL PASS'            A6 'the POSIX conformance suite reported failures'; \
	  chk 'posixdemo exited 0'             A6 'posixdemo did not exit clean'; \
	  chk 'libembk.so linked'              A6 'the dynamic link failed -- no EmUI app can load'; \
	  chk 'init.elf launched as pid'       A6 'pid 1 did not start'; \
	  chk 'ns: /system is read-only'       A6 'init could not confine itself -- the namespace is not enforced'; \
	  chk 'desktop session started'        A6 'init never spawned the session'; \
	  chk 'home: desktop ready'            A7 'the desktop came up but never finished'; \
	  chk 'first frame presented'          A7 'the compositor never presented a frame'; \
	  chk 'framebuffer 1280x800'           A7 'virtio-gpu did not come up'; \
	  chk 'is a keyboard'                  A7 'virtio-input found no keyboard'; \
	  chk 'is a tablet'                    A7 'virtio-input found no pointer'; \
	  chk 'statusq (LEDs)'                 A7 'the keyboard has no status queue -- the lock LEDs cannot work'; \
	  keyn=$$(sed -n 's/.*virtio-input: \([0-9][0-9]*\) key event.*/\1/p' $$log | tail -1); \
	  ptrn=$$(sed -n 's/.*virtio-input: [0-9][0-9]* key event(s), \([0-9][0-9]*\) pointer.*/\1/p' $$log | tail -1); \
	  [ -n "$$keyn" ] && [ "$$keyn" -gt 0 ] 2>/dev/null || \
	    { echo "FAIL(A7): no KEY events reached the driver ($${keyn:-none}) -- the queue is armed but empty"; fail=1; }; \
	  [ -n "$$ptrn" ] && [ "$$ptrn" -gt 0 ] 2>/dev/null || \
	    { echo "FAIL(A7): no POINTER events reached the driver ($${ptrn:-none})"; fail=1; }; \
	  n=$$(grep -c 'matches KV2P' $$log); \
	    [ "$$n" = "4" ] || { echo "FAIL(A2): $$n/4 kernel sections translate to KV2P"; fail=1; }; \
	  if grep -q 'KV2P.*MISMATCH\|MISMATCH.*KV2P' $$log; then \
	    echo "FAIL(A2): a translation does not match KV2P"; fail=1; fi; \
	  if grep -q '\[FAIL\]' $$log; then echo "FAIL: a self-test case reported failure"; fail=1; fi; \
	  if grep -q 'DID NOT TAKE' $$log; then echo "FAIL(A7): a BAR write did not stick"; fail=1; fi; \
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
	  echo "  A6 the shared kernel heap, per-process address spaces, clean teardown"; \
	  echo "  A7 PCIe ECAM, virtio-blk, and the REAL filesystem mounted + read"; \
	  echo "  A6 a newlib program from that filesystem, at EL0: argv, printf,"; \
	  echo "     malloc, snprintf, time, a second thread, and a clean exit(5)"; \
	  echo "  A6 posixdemo: the whole POSIX suite, ALL PASS -- including"; \
	  echo "     variant-I TLS (TPIDR_EL0, block above the pointer)"; \
	  echo "  A6 the REAL session: init -> auth -> a namespace-confined desktop,"; \
	  echo "     dynamically linked against libembk.so"; \
	  echo "  A7 virtio-gpu 1280x800, the compositor presenting a frame"; \
	  echo "  A7 virtio-input: keyboard + tablet, events injected and RECEIVED"; \
	else exit 1; fi

# The USERLAND libc is probed the way the x86 check-tools does it: by actually
# COMPILING an #include with the build's own flags, so a wrong NEWLIB_PREFIX and
# a toolchain that ships no libc are both caught. Homebrew's aarch64-elf-gcc
# ships NO newlib at all, so this is the step people will miss -- and its
# absence shows up otherwise as a wall of "undefined reference" at the first
# link, a long way from "you have not built a libc yet".
.PHONY: check-tools-arm64
check-tools-arm64:
	@miss=0; \
	echo "EmbLinkOS aarch64 prerequisites  (make ARCH=aarch64 check-tools-arm64)"; echo; \
	for t in $(AARCH64_CC) $(AARCH64_PREFIX)ld $(AARCH64_OBJCOPY) qemu-system-aarch64; do \
	  if command -v $$t >/dev/null 2>&1; then printf '  [ ok ]  %s\n' "$$t"; \
	  else printf '  [MISS]  %s\n' "$$t"; miss=$$((miss+1)); fi; \
	done; \
	if [ -z "$(NEWLIB_PREFIX)" ]; then \
	  printf '  [ -- ]  userspace libc  (NEWLIB_PREFIX empty -- kernel only, no userland)\n'; \
	elif echo '#include <string.h>' | $(USER_CC) $(NEWLIB_INC) -ffreestanding -x c -E - >/dev/null 2>&1; then \
	  printf '  [ ok ]  userspace libc  (<string.h> resolves via NEWLIB_PREFIX=$(NEWLIB_PREFIX))\n'; \
	else \
	  printf '  [MISS]  userspace libc  for aarch64 -- build it with:\n'; \
	  printf '            tools/newlib/build-newlib-emblink.sh <newlib-src> aarch64-elf\n'; \
	  miss=1; fi; \
	echo; \
	if [ $$miss -gt 0 ]; then \
	  echo "==> missing. macOS:  brew install aarch64-elf-gcc qemu"; \
	  echo "                Linux:  your distro's gcc-aarch64-none-elf (or crosstool-ng)"; exit 1; \
	else echo "==> ok.  Build + boot:  make ARCH=aarch64 && make ARCH=aarch64 run-arm64"; fi

.PHONY: clean-arm64
clean-arm64:
	rm -rf $(ARM_BUILD)
