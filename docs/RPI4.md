# Raspberry Pi 4 — the first real ARM board

*Design record for running EmbLinkOS on a Raspberry Pi 4 Model B. Written the
way docs/ARM64.md is: every phase is ❌ until it boots and ✅ only once its
"done when" is machine-checked by a `make` target. A phase that has only been
seen on QEMU says so. The real board is the only witness for "real hardware".*

**Status: P0–P4 ✅ on QEMU `raspi4b`, P5's USB stack ✅ on QEMU `virt`: it boots, the kernel log is on the
screen, interrupts work, all four cores run, and the SD card is the disk. The
userland and the desktop session run from it. Not yet run on a physical Pi.
First real-hardware run planned for Monday 2026-09-28, on a television over
HDMI.**

```
make ARCH=aarch64 BOARD=rpi4 test-rpi4-boot   # P0-P3, asserted (serial, two screens, per-core ticks)
make ARCH=aarch64 BOARD=rpi4 run-rpi4         # the same, on this terminal
make ARCH=aarch64 BOARD=rpi4 rpi4-sdcard      # the whole SD card image, to flash
```

The Pi kernel boots on QEMU's `raspi4b` with the **real** Pi 4 device tree.
It gets from the firmware's EL2 entry to EL1 in the higher half, takes its
memory map from that device tree, and gets a framebuffer from the firmware
over the mailbox and draws its log on it. It brings up the GIC-400, preempts
on the generic timer, and releases cores 1–3 from the firmware's spin-table.
Then it runs the whole boot self-test to the end. What fails there is what
needs a disk (P3) or PCIe (P5):

```
  board       : rpi4
  CurrentEL   : EL1
  MIDR_EL1    : 0x00000000410fd083          <- Cortex-A72, the Pi 4's core
boot: dtb memory 0x0000000000000000 + 0x000000003c000000
mbox: framebuffer 640x480, pitch 2560, RGB, at 0x000000003c100000 (bus 0x000000003c100000)
rawcon: kernel log on the firmware framebuffer, 640x480, 79x30 cells
--- self-test done: 0 failure(s) ---
gic: GICD 0x00000000ff841000, GICC 0x00000000ff842000 [GICv2, from the device tree]
gic: initialised (GICv2, memory-mapped CPU interface)
  [ ok ] timer fired: 40 ticks in 392 ms
  [ ok ] worker 1 was scheduled 18 times
smp: cpu1: released from its spin-table at 0x00000000000000e0
smp: 4 of 4 core(s) online
  [ ok ] IPI: 3 of 3 other core(s) took the interrupt
--- all self-tests done: 10 failure(s) ...     <- all of them need the disk or PCIe
```

---

## 1. Why this is a board, not a port

The architecture work is done: docs/ARM64.md A0–A9 run the whole shared
kernel and the full userland on QEMU `virt`, and they already do what a real
board needs. The EL2→EL1 drop is there, and so is the arm64 `Image` header a
bootloader looks for. Every device is found through the device tree. Devices
are mapped at runtime with `vmm_map_mmio`.

What a Pi changes is **the machine**, and for P0 that was three constants and
one table layout:

| | QEMU `virt` | Raspberry Pi 4 |
|---|---|---|
| DRAM | from 1 GiB | from **0** |
| kernel load address | 0x4008_0000 | **0x0008_0000** (firmware, `kernel_address`) |
| devices | below 1 GiB | **0xFC00_0000–0xFF80_0000** (top of GiB 3) |
| console | PL011 @ 0x0900_0000, 24 MHz | PL011 @ **0xFE20_1000**, **48 MHz** |
| entry EL | EL1 | **EL2** (all cores) |
| interrupt controller | GICv3 + ITS | **GIC-400 = GICv2**, no ITS |
| secondary cores | PSCI `CPU_ON` | **spin-table** (`cpu-release-addr` 0xd8…0xf0) |
| DT addresses | CPU addresses | **bus addresses**, translated by `ranges` (0x7e…→0xfe…) |
| storage, display, input, net | virtio | EMMC2 (SDHCI), mailbox framebuffer, VL805 xHCI over PCIe, GENET |

Everything below the first line is either done (P0) or a phase.

## 2. Decisions

### 2.1 `BOARD` is chosen at build time
`make ARCH=aarch64 BOARD=rpi4`. `virt` stays the default, and CI still runs on it.

The kernel is linked at a fixed physical address inside RAM, and the two boards
put RAM in different places. One image for both would need a physically
relocatable kernel. That's a real project with no user-visible benefit. So:

- `kernel/arch/aarch64/board.h` holds what is needed **before** the device tree
  can be read: the console UART and its clock, and `BOARD_RAM_BASE`. Nothing
  else goes there. After the DTB is parsed, both boards run the same code.
- `linker.ld` takes `BOARD_KERNEL_PHYS_BASE` via `--defsym` (it uses `DEFINED`,
  because a plain assignment in the script would override the defsym).
- The Pi kernel builds into `build/aarch64/rpi4/`, together with its rootfs,
  because the rootfs carries `/system/kernel.embdbg`, which holds *this*
  kernel's symbols. The userland ELFs are the same bytes for both boards, and
  both boards share them.

### 2.2 The Pi's first page tables: devices are never Normal memory
boot.S builds the Pi's tables the other way up from `virt`'s:

- **identity**: GiB 0 as executable RAM (the kernel is there) and GiB 3 as
  Device (so `pl011_init()` works before it moves to the MMIO window).
- **kernel window**: `base+0` → PA 0–1 GiB, executable RAM.
- **direct map**: GiB 0–2 as RAM, then **GiB 3 in 2 MiB blocks stopping at
  0xFC00_0000**. Normal memory can be accessed speculatively, and a
  speculative read of a BCM2711 peripheral is not harmless: some registers
  have read side effects, and some hang the bus. So the peripheral hole gets
  no Normal mapping anywhere.
- **MMIO window**: GiB 3 as Device, which covers the UART, the GIC-400 at
  0xFF84_0000 and the PCIe controller at 0xFD50_0000.

`MMIO_DYN_BASE` moved from +1 GiB to +4 GiB for both boards. Otherwise the
runtime MMIO bump allocator would eventually walk into the Pi's static GiB-3
block.

### 2.3 QEMU `raspi4b` is the Pi's CI target, and it has limits
QEMU 11 emulates the BCM2711's memory map, GIC-400, PL011, generic timer,
mailbox and SD host. It has **no PCIe, no Ethernet and no USB**. It also sizes
`/memory` itself (960 MiB) rather than the way the firmware does. So:

- P0–P4 can be defended on QEMU. `test-rpi4-boot` boots with the **same
  `bcm2711-rpi-4-b.dtb`** the firmware hands a real board.
- P5 (USB) and P6 (Ethernet) can only be proven on the real board. They get a
  real-hardware checklist rather than a `make` target, and the status line
  must say so.

### 2.4 The firmware is fetched, pinned and hashed, not committed
`tools/rpi4_firmware.sh` downloads `start4.elf`, `fixup4.dat`,
`bcm2711-rpi-4-b.dtb` and `overlays/disable-bt.dtbo` from **one** tagged
raspberrypi/firmware release (`1.20260915`). It checks each file against a
SHA-256 recorded in the script, and a file that doesn't match is deleted
rather than kept. They're redistributable Broadcom binaries, so they don't
belong in this tree. A boot partition built from an unexpected firmware
version would send debugging after the wrong problem.

### 2.5 The screen is a debug console from P0b, not a P4 luxury
The first real board will be tested on a **television**, with no serial
adapter. So the kernel log has to be on the screen from the earliest moment
possible, and the screen is the witness for every phase after it.

That turned out to be cheap, because every piece already existed:
- the VideoCore firmware hands out a framebuffer through the mailbox, before
  interrupts, pmm or anything else;
- `rawcon` (x86's "text straight onto the firmware framebuffer", written for
  laptops with no serial port) draws the log on whatever the boot protocol's
  `fb_*` fields describe;
- `fb_init()` already falls back to those same fields when no GPU driver
  claims the display, so P4's desktop needs no new display code either.

Serial still works, and it's still the better tool when it's available: it
scrolls, and you can copy from it. The screen is for when it isn't.

## 3. Phases

* **P0 ✅ (QEMU) — serial boot to the end of pre-interrupt bring-up.**
  `board.h`; board-selected load address, beacon UART and page tables; PL011
  divisors computed from the board clock (48 MHz → IBRD 26, FBRD 3);
  `CPTR_EL2` cleared on the EL2 path; `config.txt`, the firmware fetch,
  `rpi4-sdboot`, `run-rpi4`, `test-rpi4-boot`.
  **Done when:** `test-rpi4-boot` sees EL1, the higher half, a memory range
  from the Pi's DTB, the firmware's page-0 reservation honoured, a Device-memory
  alignment fault at the Pi's UART, and `self-test done: 0 failure(s)`. ✅
  **On real hardware:** not yet. Same markers, read off the serial adapter.

  Found on the way, all fixed:
  1. **The Image header's `image_size` said 1 MiB; the kernel is 4.6.** It had
     been a hardcoded value since A0, when that was true. `virt` never noticed,
     because QEMU puts the DTB *below* the kernel. A loader that puts the DTB
     just past `image_size` would have it zeroed along with .bss. It's now
     `__kernel_image_size`, computed by the linker.
  2. **RAM above 4 GiB would have been handed out and then faulted on.** pmm
     manages every usable page up to the highest address, and boot.S's direct
     map ends at 4 GiB. An 8 GB Pi 4 (or `virt -m 5G`, which was never tried)
     gets pages the kernel can't reach. `boot_protocol_dtb.c` now truncates
     memory to the direct map and prints how much it dropped. Verified with
     `virt -m 5G`: "truncated to the 4 GiB direct map (2048 MiB ignored)".
     Extending the direct map is in docs/TODO.md.
  3. `fdt_find_compatible()` searches the root's children and grandchildren
     but **not the root itself**, which is where a board's own `compatible`
     lives. Test the root with `fdt_prop_has_string(fdt_root(), …)`.

* **P0b ✅ (QEMU) — the kernel log on the television.** `drivers/bcm_mbox.c`
  asks the VideoCore firmware, over the mailbox's property channel, for the
  display's size (capped at 1920×1080: a 4K buffer can fail to fit in the
  default GPU memory split) and a 32-bpp framebuffer, then records it in the
  boot protocol. `rawcon_init()` now runs on aarch64 too, right after the
  kernel's final page tables. It **replays the klog ring** first, so the lines
  printed before the screen existed (the banner, the memory map, pmm) are on
  it as well, sized to one screen by counting wrapped rows. This benefits the
  x86 laptop path equally. `config.txt` adds `hdmi_force_hotplug=1` and
  `disable_overscan=1`.
  **Done when:** `test-rpi4-boot` takes a QMP screenshot of `raspi4b`'s
  display and finds the log drawn in rawcon's colours. It also finds rawcon's
  "newest line" bar **blue**, which is the byte-order check: with red and blue
  swapped the bar comes out orange. ✅
  **On real hardware:** the same bar on the TV should be blue. If it's orange,
  the firmware ignored the pixel-order request *and* misreported it. Swap
  `FB_FORMAT_*` in `bcm_fb_probe()`.

  This needed the device tree read properly, which was P1's first item:
  - **`fdt_reg()` now translates through `ranges`.** The mailbox's `reg` is
    bus address 0x7e00b880; the CPU sees it at 0xfe00b880. `fdt_parent()` is
    new (nodes have no parent pointer; it walks down from the root, one child
    per level). `fdt_reg_cells()` now reads the **parent's** cell counts
    instead of always the root's. With the root's two address cells, every
    `/soc` device would have been misread.
  - **`fdt_dma_translate()`** reads `dma-ranges`. The VideoCore addresses RAM
    at 0xC0000000, so the message buffer's physical address must be converted
    before it goes into the FIFO, and the framebuffer's address converted back.
  On `virt` every device is the root's child, so all this is a no-op there;
  the full `test-arm64-boot` confirms it.

* **P1 ✅ (QEMU) — GIC-400 and the timer.** The GIC driver is now two
  back-ends behind one interface (`irq/gic_hw.h`): `irq/gic.c` holds what
  every GIC does (the handler table, the handler → EOI → scheduler order that
  `gic_dispatch()` depends on, the counters), and `irq/gicv3.c` and the new
  `irq/gicv2.c` hold what differs. `gic_init()` picks one from the device
  tree at **runtime**. That is deliberate: it lets QEMU `virt` with
  `gic-version=2` put the GICv2 driver through the *whole* `virt` suite (four
  cores over PSCI, IPIs, virtio on INTx, the desktop, input), which the Pi's
  boot alone could never do. SGI sending moved into the back-ends: v3 names
  a core by MPIDR affinity (`ICC_SGI1R_EL1`), v2 by a CPU-interface bit that
  each core reads from the banked `GICD_ITARGETSR0` (`GICD_SGIR`). `ipi.c` is
  now version-neutral.
  **Done when:** the scheduler preempts on `raspi4b` and `selftest_preemption`
  passes. ✅ Also: `test-arm64-boot` passes on `virt` GICv3 (HVF), and on
  `virt,gic-version=2` (TCG; HVF can't emulate a GICv2) everything passes
  except PAN, which TCG's Cortex-A72 doesn't have.

  Two things P1 turned up:
  - **Interrupt groups.** A real Pi runs the kernel non-secure after firmware
    has put every interrupt in Group 1; QEMU's `virt` GICv2 has a single
    security state and everything is Group 0. Writing "Group 0" and control
    bit 0 does the right thing in both: the non-secure view ignores the group
    write, and there bit 0 *is* the Group 1 enable. **Real-hardware risk:**
    QEMU `raspi4b` imitates the firmware's GIC set-up rather than running it,
    so Monday is the first real test of this.
  - **PCI's `interrupt-map` assumed a GICv3.** `pci_ecam.c` looked up the
    interrupt controller as `arm,gic-v3` only. On a GICv2 it found nothing,
    fell back to 0 parent address cells where the tree says 2, and read every
    entry misaligned: "8 of 9 devices routed, on 1 distinct line of 4".
    `fdt_find_gic()` now knows every GIC name, and both it and
    `fdt_interrupt()` use it. Only `virt,gic-version=2` could have shown this.
  - **The munmap self-test was miscounting.** It compares the whole machine's
    free pages, which also sees the kernel heap growing to hold the VMA
    record, and the heap keeps what it grows. On `virt` the disk had grown it
    long before; on the diskless Pi it looked like "munmap leaked 17 pages".
    The test now runs one unmeasured cycle first. A real leak still fails.

* **P2 ✅ (QEMU) — four cores, over spin-table.** `cpus_probe()` records each
  core's `enable-method` and `cpu-release-addr`; with no PSCI node,
  `spin_table_release()` publishes whose turn it is (`smp_spin_index`) and
  that core's stack. It then writes the physical entry point into the core's
  slot (0xe0/0xe8/0xf0, through the direct map) and `sev`s.
  `secondary_entry_spin` reads the index, because the firmware's loop doesn't
  pass it in `x0` the way PSCI does, and joins the common path. That path
  now clears `CPTR_EL2` too, since every Pi secondary arrives at EL2.
  **Caches:** a released core runs with its MMU and caches off, so it reads
  RAM directly and sees nothing still dirty in core 0's cache. Everything it
  reads is cleaned to the point of coherency (`dc civac`) first. The PSCI
  path had relied on a `dsb`, which orders but doesn't clean. QEMU models no
  caches, so only the real board can prove this.
  **Done when:** 4 cores report in and tick on `raspi4b`. ✅ `test-rpi4-boot`
  measures the ticks itself: it reads every core's interrupt counter twice,
  5 s apart, through QMP. The kernel's own "every core is taking interrupts"
  check can't be the witness yet: on a diskless Pi it samples about 50 ms of
  guest time after bring-up, and an idle core sleeps up to 1 s between ticks.
  It gets its time back when P3 gives the userland tests something to run.

* **Reboot and halt ✅ (QEMU, checked by hand).** The Pi has no PSCI, so
  `power_arm.c` falls back to the BCM2835 power-management watchdog, the same
  sequence as Linux's `bcm2835_wdt`. It sets the RSTS "partition" the firmware
  should boot next (0 = normal; 63 = halt, the nearest a Pi gets to off), arms
  the watchdog for ten ticks, and requests a full reset. Every write carries
  the `0x5a` password, without which the block silently ignores it. Checked
  on `raspi4b` by calling `arch_power_reboot()` at the end of boot, where
  `-no-reboot` QEMU exited on the reset. It isn't automated yet: nothing on
  a diskless Pi asks for a reboot.

* **P3 ✅ (QEMU, through the older controller) — the SD card is the disk.**
  `make ARCH=aarch64 BOARD=rpi4 rpi4-sdcard` builds `sdcard.img`, the whole
  card: an MBR, partition 1 FAT32 with the firmware and `kernel8.img`,
  partition 2 the EMBKFS root. The kernel finds the card, partitions it, and
  mounts `sda2` as `/`; `init`, the accounts, futexes and the rest of the
  userland self-test run from it.

  - **`tools/mkrpi4sd.py`** writes the card, including its own FAT32. The
    firmware needs long names and a subdirectory (`bcm2711-rpi-4-b.dtb`,
    `overlays/`), which `mkfat32.py` doesn't write. It also needs a volume
    that is FAT32 by the specification's rule: the FAT type is decided by the
    **cluster count**, and `mkfat32.py`'s ~8,000 clusters are FAT16 to any
    reader that follows it. The card uses 128 MiB of 1 KiB clusters, about
    130,000. Verified independently on macOS: `fsck_msdos` is clean, and
    `hdiutil` mounts the image and every file is byte-identical to its source.
  - **`sdhci.c` gained what a Pi's controllers need**, as flags on a new
    `sdhci_attach_mmio()` (the PCI path is unchanged in behaviour):
    - `SDHCI_32BIT_ONLY`: Broadcom's controllers implement only 32-bit
      register accesses. Byte and halfword writes become read-modify-write,
      except transfer mode and block size/count. Those are held in a shadow
      and written with the command, because writing the command half is what
      *issues* it. This is the same scheme as Linux's `sdhci-iproc`.
    - `SDHCI_BROKEN_CD`: the Pi 4 device tree says `broken-cd` for the SD
      slot, so the driver asks the card rather than trusting the
      present-state bit.
    - A real clock. The driver ran at its initialisation divider forever,
      about 400 kHz on one data line, roughly 50 KB/s. That's invisible on
      QEMU and minutes of boot on a real card. It now computes the divider
      from the base clock (from the capabilities register, or from the
      firmware over the mailbox), then switches to a 4-bit bus (ACMD6) and
      25 MHz after selection.
  - **`drivers/sdhci_dt.c`** attaches controllers from the device tree:
    EMMC2 (`brcm,bcm2711-emmc2`, the Pi 4's slot) and the older
    `brcm,bcm2835-sdhci`. Disabled nodes are skipped, and so is any
    controller with an SDIO function under it (the Pi's Wi-Fi, `wifi@1`,
    lives at the same address as that older controller).

  **The disk found a deadlock in the shared scheduler, on every aarch64
  machine.** With a root filesystem, the boot runs the userland tests, and
  about one boot in six hung for good at the deadline-scheduler `jitter` run.
  The hung VM was read over QMP: all four cores in `spin_lock` on
  `g_sched_lock`, whose `holder_lr` named `schedule()`, and two backtraces
  ending at a new thread's frame-chain reset. `kernel_ctx_prepare()` gave
  every new thread `DAIF = 0`, interrupts on from its first instruction. But
  a new thread is entered holding the scheduler lock, which its trampoline
  releases as its first action. A timer interrupt pending at the switch fired
  in between, called `schedule()`, and spun forever on the lock its own core
  held. New contexts now start fully masked, as x86's do, and interrupts are
  enabled by the entry code once the lock is gone (the trampolines already
  did; `bringup.c`'s self-test threads got a wrapper). `virt` had the same
  window, and one of its suite runs today hung on the same line.

  **And a second one: the exception return was interruptible.** In the
  20-boot confirmation of that fix, one boot killed a program right after
  `home: desktop ready`, with an instruction abort *from user mode* at a
  *kernel* address, `exc_common`'s `msr spsr_el1`. `exc_common` loaded
  `ELR_EL1` and `SPSR_EL1` and ran `eret` with interrupts on whenever the
  handler returned with them on, as every system call does. An IRQ between
  the two `msr`s replaced ELR, and the `eret` went there in user mode. When
  returning to kernel mode instead, the same race writes a register into SPSR
  and takes an illegal-execution-state exception: the unexplained 2026-09-12
  crash in docs/TODO.md. Both return paths (`exc_common` and
  `aarch64_eret_to_el0`) now mask everything before touching ELR/SPSR. Both
  of these bugs live in shared aarch64 code, so `virt` benefits too.

  **And a third, older than all of this: a core woken from idle kept the
  idle loop's timer.** Tickless idle arms a halted core's timer for the next
  thing due, up to a whole second. When a reschedule IPI or a device
  interrupt wakes it early, `schedule()` runs inside that interrupt and
  switches straight to real work, so the new thread inherits the long arm
  and runs up to a second unpreempted. On `virt,gic-version=2` under TCG,
  two runs in five saw the deadline test's periodic thread starve for
  800–1,600 ms. An A/B with today's two fixes reverted showed it wasn't them.
  `schedule_locked()` now re-arms the quantum whenever it dispatches on a
  core `g_idle_cpus` says was idle. This is shared code, so x86's tickless
  idle had the same hole and gets the same fix. On a desktop it's the
  difference between smooth and an occasional one-second stutter.

  **QEMU is not the board here, and the test says so.** `raspi4b` wires its
  SD card to the *older* controller at 0x7e300000, where the real Pi has its
  Wi-Fi, and leaves EMMC2 empty. EMMC2's `sd-bus` can't be reached from the
  command line either. So the QEMU runs boot a copy of the real device tree
  with that controller's SD-card node enabled (`fdtput`, in `arch.mk`). The
  kernel learns nothing about QEMU; the tree just describes the emulated
  machine as it is wired. Everything from the register path to the mounted
  root is exercised. **Only the real board tests EMMC2's own two quirks**:
  no card-detect, and a clock only the firmware knows.

* **P4 ✅ (QEMU) — the desktop on the screen.** Nothing new was needed once
  P3 gave it a disk: `fb_init()` takes the mailbox framebuffer from the boot
  protocol, `init` logs the development user in, and the compositor, top bar,
  dock, file panel and notification daemon come up on it. The screenshot at
  the end of `test-rpi4-boot` is the EmbLink desktop at `raspi4b`'s
  640×480: the hummingbird wallpaper, the top bar's clock and CPU meter, the
  dock, and the icon colours right (blue folder, purple globe). On a TV it
  will be the TV's own resolution, capped at 1080p (P0b).
  **Done when:** the desktop draws on `raspi4b`'s display. ✅ The test checks
  it has more than 32 distinct colours, where a console has two.
  **Not yet:** input (keyboard and mouse are USB, P5), and the clock, which
  reads 1 January 1970 until NTP runs (P6; a Pi has no RTC).

* **P5 — USB keyboard and mouse. ✅ on QEMU for everything QEMU can model;
  the PCIe half is real-hardware only.** A Pi 4's USB is a VIA VL805 xHCI
  behind the SoC's PCIe, and its USB 2.0 ports are a hub inside the VL805, so
  a keyboard is always PCIe → xHCI → hub → device. Each layer:

  - **The USB stack on aarch64** (✅ `test-arm64-usb`). The x86 xHCI, EHCI,
    OHCI and HID code now builds for aarch64: `timer_delay_ms()` instead of
    the x86 PIT, UHCI (I/O ports) answered by `absent.c`, and the controller
    **polled** from the boot loop (`xhci_poll()` via `usb_poll()`, next to
    `virtio_input_poll()`), with x86's interrupt path unchanged. Proven on
    `virt` with `qemu-xhci` and **no virtio input**: keys sent through QEMU's
    input layer raise the driver's report counter, and a (+100, +50) motion
    moves the kernel's cursor by exactly that.
  - **Mouse support** (✅). The driver used to detect a boot mouse and never
    configure it. Now keyboards and mice share the endpoint setup, and mouse
    reports go to a new shared `mouse_move_relative()`.
  - **Hubs** (✅, `test-arm64-usb` runs a second topology). Mandatory on a Pi.
    It covers the hub descriptor, the slot marked as a hub (Configure
    Endpoint), port power and reset, route strings, Transaction Translator
    fields for low/full-speed devices behind a high-speed hub, recursive
    enumeration up to five tiers, per-slot teardown (a hub takes its children
    with it), and hub-port hotplug in `xhci_rescan()`. Hot-adding a keyboard
    to a hub port over QMP enumerates it and it types. QEMU's hub is full
    speed, so **the TT fields are the one part only the Pi's high-speed hub
    exercises.**
  - **Four real-hardware bugs QEMU hid, fixed while here:**
    - `SET_PROTOCOL(boot)` was never sent. Devices start in report protocol;
      keyboards happen to match, many real mice don't.
    - EP0 used 64 bytes for **low**-speed devices, which must use 8.
      **Full**-speed devices now get their EP0 size from the first 8 bytes of
      the descriptor, then Evaluate Context. Before, a device with an 8-byte
      EP0 returned an 8-byte descriptor padded with zeros.
    - The raw `bInterval` went to xHCI, which wants an exponent: a
      full-speed mouse polled every 128 ms instead of 10.
    - The command ring's Link TRB never got its cycle bit updated, so the
      controller would stop dead after 255 commands. One `xhci_cmd_advance()`
      now serves all five enqueue sites.
  - **DMA coherency** (✅ mechanism on QEMU; the need is real-hardware only).
    A Pi 4's PCIe doesn't snoop the CPU caches. `include/arch_dma.h` adds
    `arch_dma_uncached()` (remap page-aligned kernel memory non-cacheable)
    and `arch_dma_flush()`. The xHCI's whole DMA bundle is remapped the
    moment it's claimed; the kernel reads the page tables back and complains
    if a page stayed cacheable, and the test requires it didn't. Scratchpad
    pages are flushed after zeroing. No-ops on x86.
  - **The Pi's PCIe host bridge** (`drivers/pcie_brcmstb.c`, **real hardware
    only**: `raspi4b` has no PCIe). Linux's `pcie-brcmstb` bring-up for the
    BCM2711, with the windows from the device tree:
    1. Reset, SerDes out of IDDQ.
    2. Unsupported config reads return all-ones, so the scan can't fault.
    3. The inbound DMA window from `dma-ranges`.
    4. Interrupts masked, PERST released, link awaited.
    5. The outbound window (this firmware: CPU `0x6_0000_0000` = PCI
       `0xC0000000`, 1 GiB) and the root port's bus numbers and window.

    Then the VL805's firmware: if its version register (config `0x50`) reads
    0, the VideoCore is asked to load it (`NOTIFY_XHCI_RESET`), and the BAR
    and command register are restored afterwards. Every step prints; the
    version is on screen.
  - **PCI addresses ≠ CPU addresses.** `pci_read_bar()` now returns CPU
    addresses via `arch_pci_bus_to_cpu()`, and BARs and bridge windows are
    programmed with bus addresses. The identity on x86 and `virt`.

  **Done when (on the Pi):** typing on a USB keyboard reaches the desktop,
  and the mouse moves the cursor.

* **P6 ❌ (real hardware only) — Ethernet.** GENET v5 (0xFD58_0000) and its
  BCM54213 PHY. Then the existing network stack and NTP set the clock (the Pi
  has no RTC).

* **P7 ❌ — all the RAM.** Extend the direct map past 4 GiB (from the DTB, in C,
  once pmm exists) so an 8 GB board uses all 8. Until then it runs with 4.

## 4. Running it on the real board

**You need:** a Pi 4B, a microSD card (1 GB or more; whatever is on it will be
erased), a 5 V/3 A USB-C supply, a **micro-HDMI to HDMI** cable, and a TV or
monitor. A 3.3 V USB-serial adapter is optional (below).

1. `make ARCH=aarch64 BOARD=rpi4 rpi4-sdcard`. This builds
   `build/aarch64/rpi4/sdcard.img`, the whole card: boot files and root
   filesystem.
2. Write it to the card with **Raspberry Pi Imager**: *Choose OS* → *Use
   custom* → pick `sdcard.img` → *Choose storage* → the card → *Write*. Skip
   any "OS customisation" it offers; that's for Raspberry Pi OS. (balenaEtcher
   works too, and so does `dd` to the **whole** device, not a partition.)
3. Plug the cable into **HDMI0**, the micro-HDMI port next to the USB-C power
   socket. Switch the TV to that input first, then power the Pi.

**What you should see:**
1. **A rainbow square** for a second or two. That's the firmware; it found the
   card.
2. **The kernel log, white on dark blue-black**, with a **blue bar** marking
   the newest line. It starts with the boot banner (`board : rpi4`,
   `CurrentEL : EL1`, …) and reaches `sdhci: sda = SD card`,
   `EMBKFS: sda2: mounted` and `VFS: mounted fs at "/"`. That's the card
   becoming the disk.
3. **A blink to black, then the log again in white on plain black.** That's
   the kernel's real console taking the screen over; it's normal.
4. **The desktop**, with its top bar, logged in automatically as the
   development user. The whole boot takes seconds on the real board (the
   minute-plus figures in this file are QEMU emulating every instruction).
   **That is success.**
5. **With a USB keyboard and mouse plugged in** (any of the four ports), the
   mouse should move the cursor and the keyboard should type. This is P5's
   first real test. If they don't, the lines to photograph are the ones
   starting `pcie:` and `xHCI` near the top of the log. The `pcie:` lines
   show the link training and the USB controller's firmware version; the
   `xHCI` lines show the hub and every device behind it.

Take a photo of the screen either way. It carries everything needed.

**If it doesn't look like that:**
- **No rainbow, TV says "no signal":** the firmware never started. Re-write
  the card (the whole image, to the whole card), and check the cable is in
  HDMI0. The Pi's green LED flashing a pattern is an error
  code; count the long and short flashes.
- **Rainbow, then black:** the firmware loaded the kernel but the kernel never
  drew. Either it died before the screen came up (in boot.S's page tables or
  the EL2→EL1 drop), or the mailbox refused the framebuffer. Serial tells
  which; see below.
- **The log is there but the bar is orange, not blue:** red and blue are
  swapped. Harmless, and a one-line fix (P0b's note).
- **`sdhci: the SD slot (EMMC2): no card answered`:** the SD path, on the
  one controller QEMU cannot test (P3). The lines just above it (the base
  clock the firmware reported) are the evidence; a photo of them is the bug
  report.
- **Desktop fine, but no keyboard or mouse:** look for `pcie: LINK DOWN`
  (PCIe never trained), `firmware version 0 ... STILL NO FIRMWARE` (the
  VL805 has no firmware), or `xHCI hub slot1: ... port N` lines with no
  `HID keyboard ready` after them (enumeration behind the hub failed;
  usually the TT fields, which QEMU can't test).
- **The log stops somewhere before the end:** the photo of the last lines is
  the bug report. Three places are new on real silicon and the likeliest to
  stop: right after `gic: initialised` (interrupt groups: the timer never
  fires), at `smp: cpuN: released from its spin-table` (a core that never
  reports in, which points at caches), and anything after `fb_init` (the
  console taking the screen over).
- **Edges cut off:** the TV is overscanning. Set its picture size to "Just
  Scan", "Screen Fit" or "1:1".

**Serial, if you get an adapter.** A 3.3 V USB-serial adapter
(FTDI/CP2102/CH340; a 5 V one can damage the Pi) shows everything from the
very first instruction, including the `ABPM` beacon boot.S prints before the
MMU is on. Wire adapter **GND → pin 6**, adapter **RX → pin 8** (GPIO 14),
adapter **TX → pin 10** (GPIO 15); don't connect its power pin. On the Mac:
`screen /dev/tty.usbserial-* 115200`. With `uart_2ndstage=1` uncommented in
`config.txt`, the firmware logs its own boot there too. That's how to tell
"the firmware never loaded the kernel" from "the kernel died early". The
beacon letters: **A** reached EL1, **B** stack up, **P** page tables built,
**M** MMU on.
