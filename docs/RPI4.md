# Raspberry Pi 4 — the first real ARM board

*Design record for running EmbLinkOS on a Raspberry Pi 4 Model B. Written the
way docs/ARM64.md is: every phase is ❌ until it boots and ✅ only once its
"done when" is machine-checked by a `make` target. A phase that has only been
seen on QEMU says so. The real board is the only witness for "real hardware".*

**Status: P0, P0b, P1 and P2 ✅ on QEMU `raspi4b`: it boots, the kernel log
is on the screen, interrupts work, and all four cores run. Not yet run on a
physical Pi. First real-hardware run planned for Monday 2026-09-28, on a
television over HDMI.**

```
make ARCH=aarch64 BOARD=rpi4 test-rpi4-boot   # P0-P2, asserted (serial, screen, per-core ticks)
make ARCH=aarch64 BOARD=rpi4 run-rpi4         # the same, on this terminal
make ARCH=aarch64 BOARD=rpi4 rpi4-sdboot      # an SD card's boot partition
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

* **P3 ❌ — the SD card is the disk.** EMMC2 (`brcm,bcm2711-emmc2`, 0xFE34_0000)
  is an SDHCI, and `kernel/drivers/storage/sdhci.c` exists but binds over PCI.
  Split its core from its PCI attach and add a DT attach. Card layout:
  partition 1 FAT32 (the `rpi4-sdboot` files), partition 2 EMBKFS (the
  rootfs). `embk_partition_scan_all()` already finds it. Build the whole card
  image with `rpi4-sdcard`.
  **Done when:** `/system/bin/init.elf` runs from the SD card on `raspi4b`
  (`-drive if=sd`).

* **P4 ❌ — the desktop on the screen.** The framebuffer exists from P0b, and
  `fb_init()` already uses the boot protocol's. What's missing is everything
  before it in the boot: P1–P3.
  **Done when:** the desktop draws on `raspi4b`'s display. On the real board,
  on the TV.

* **P5 ❌ (real hardware only) — USB keyboard and mouse.** The BCM2711 PCIe
  root complex (`brcm,bcm2711-pcie`, 0xFD50_0000) has to be brought up by the
  kernel: link training, outbound window, its own MSI block. Behind it sits
  the VL805, an **xHCI**, which `kernel/drivers/usb/xhci.c` already drives on
  x86. After the PCIe reset the VL805 needs firmware reloaded, via mailbox tag
  `NOTIFY_XHCI_RESET` (0x00030058).
  **Done when:** typing on a USB keyboard reaches the shell (checklist, §4).

* **P6 ❌ (real hardware only) — Ethernet.** GENET v5 (0xFD58_0000) and its
  BCM54213 PHY. Then the existing network stack and NTP set the clock (the Pi
  has no RTC).

* **P7 ❌ — all the RAM.** Extend the direct map past 4 GiB (from the DTB, in C,
  once pmm exists) so an 8 GB board uses all 8. Until then it runs with 4.

## 4. Running it on the real board

**You need:** a Pi 4B, a microSD card, a 5 V/3 A USB-C supply, a
**micro-HDMI to HDMI** cable and a TV or monitor. A 3.3 V USB-serial adapter
is optional (below).

1. `make ARCH=aarch64 BOARD=rpi4 rpi4-sdboot`
2. Format the card **FAT32** (MBR partition table) and copy everything in
   `build/aarch64/rpi4/sdboot/` to its root. On a Mac: Disk Utility → Erase →
   "MS-DOS (FAT)", scheme "Master Boot Record".
3. Plug the cable into **HDMI0**, the micro-HDMI port next to the USB-C power
   socket. Switch the TV to that input first, then power the Pi.

**What you should see:**
1. **A rainbow square** for a second or two. That's the firmware; it found the
   card.
2. **The kernel log, white on dark blue-black**, with a **blue bar** marking
   the newest line. It starts with the boot banner (`board : rpi4`,
   `CurrentEL : EL1`, …).
3. **A blink to black, then the log again in white on plain black.** That's
   the kernel's real console taking the screen over; it's normal.
4. It ends at `--- all self-tests done: N failure(s) ...` and
   `A7 reached: ...`, then sits there. **That is success.** Expect around ten
   failures, all about the missing disk or PCIe (`no EMBKFS volume`,
   `could not launch /system/bin/...`). Things worth reading on the way:
   `gic: initialised (GICv2`, `timer fired`, and `smp: 4 of 4 core(s)
   online`.

Take a photo of the screen either way. It carries everything needed.

**If it doesn't look like that:**
- **No rainbow, TV says "no signal":** the firmware never started. Check the
  card is FAT32 with the files at the top level (not in a folder), and that
  the cable is in HDMI0. The Pi's green LED flashing a pattern is an error
  code; count the long and short flashes.
- **Rainbow, then black:** the firmware loaded the kernel but the kernel never
  drew. Either it died before the screen came up (in boot.S's page tables or
  the EL2→EL1 drop), or the mailbox refused the framebuffer. Serial tells
  which; see below.
- **The log is there but the bar is orange, not blue:** red and blue are
  swapped. Harmless, and a one-line fix (P0b's note).
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
