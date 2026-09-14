# The pillars still missing

**The goal this document is held to:** EmbLinkOS as the *main* operating system
of a separate, physical machine — installed on its disk, booted by its firmware,
used every day. Not a demo on QEMU.

Everything below was checked against the source on 2026-09-11, not recalled.
Where a pillar is "absent", the audit searched the tree for it and found nothing;
where it is "partial", the file that has the partial version is named.

## What already stands

Two architectures at parity. SMP scheduler with a deadline policy. Paging,
demand paging, swap with second-chance reclaim, ASLR in seven windows, PIE
userland, W^X, SMEP/SMAP/PAN, kernel stack canaries. EMBKFS (copy-on-write,
snapshots, compression, crash-consistent, XTS encryption), FAT32, a unified page
cache. TCP/IPv4, DHCP, DNS, userland TLS. USB (UHCI/OHCI/EHCI/xHCI, HID). AHCI,
ATA, virtio-blk, **NVMe**. Capabilities, namespaces, sessions, a real account
store. A compositor, a UI toolkit, a desktop, a browser, a shell, git, Python.

That is a lot of operating system. It is also, on a real machine, mostly
unreachable — because of what is still open in phase 1.

## Phase 1 — it boots and runs on the machine

Hardware nobody can route around. Each of these is testable on QEMU, which
emulates the common real part, so none has to wait for the machine.

| Pillar | Today | Why it blocks the goal |
|---|---|---|
| **NVMe** | ✅ done — both architectures, root filesystem verified on it | Most machines built since ~2016 boot from NVMe |
| **ACPI power** | ✅ power-off from the FADT and the DSDT's `\_S5_` package, reboot from the FADT reset register — verified on both QEMU chipsets (`make test-power`) | Power-off used emulator constants and did nothing on a real PC |
| **ACPI AML interpreter** | ✅ **done for what can be tested** — `kernel/acpi/aml.c`: the namespace built from every DSDT and SSDT, the object model, control methods with locals and arguments, and operation regions over system memory, I/O ports, PCI configuration space and the embedded controller. `\_S5_` is now EVALUATED rather than pattern-matched, and **PCI interrupt routing comes from `_PRT`** — cross-checked against the byte the firmware independently wrote into each device's Interrupt Line register, on both QEMU chipsets, in matching interrupt modes. `make test-acpi` | No sleep, no battery, no lid, no thermal zones, no `_S5` that is a method |
| **A real network card** | ✅ **done for what can be tested** — `e1000.c` (Intel 8254x/8257x) and `rtl8139.c` (Realtek), behind a `struct net_driver` table. DHCP, DNS, TCP, `test net`, `test netudp` on `e1000`, `e1000e` AND `rtl8139`; carrier detection proved by pulling the virtual cable with QMP `set_link`, down to the menu bar's indicator. `make test-nics` | A physical machine has no network at all. Intel e1000e and Realtek r8169 cover most wired machines |
| **Intel HD Audio** | ✅ **done for what can be tested** — `hda.c`: CORB/RIRB command ring, the codec graph walked to find a DAC→pin route from the board's own configuration defaults, and the cyclic DMA engine reconciled with the stream contract. Behind a `struct pcm_driver` table alongside AC'97 and virtio-snd. Verified on QEMU's ICH6 and ICH9 controllers and with an AC'97 attached at the same time, where the table has to choose. `make test-audio-cards` | AC97 left real hardware around 2008; no sound |
| **USB hot-plug + mass storage mount** | ✅ **done for the shared-core controllers** — ports are compared against the device table every 500 ms, a device that appears is enumerated and a device that leaves is torn down. A mass-storage device is scanned for partitions and mounted under `/media/<device>` (FAT32 or EMBKFS) by `kernel/fs/automount.c`, and unmounted before its block device is unregistered. Proved by attaching a FAT32 stick over QMP to a RUNNING machine, reading a named file off it, and pulling it out again: `make test-usb-hotplug`. **xHCI is not covered** — it has its own device model and never touches the shared core. | A USB stick plugged in after boot does nothing |
| **UEFI boot** | ✅ **done** — the firmware launches our own EFI loader (`boot/uefi/`, no GNU-EFI), it relocates itself, draws its menu through ConOut, locates a GOP framebuffer, loads the kernel, builds page tables, exits boot services and jumps. Verified end to end under OVMF on BOTH images: loader and root on separate disks, and the single GPT disk (ESP + EMBKFS) you write to a USB stick — where the root is found on the same device the firmware booted from. `make test-uefi` | A machine from the last several years boots this way and no other |

**Still open on the Secure Boot pillar:** the key is ours, so this boots on a
machine whose owner has enrolled it — not on a stock laptop, where the enrolled
keys are Microsoft's. That needs either their signature on a shim or the owner
going into firmware setup, and no amount of code here changes it. The signing
key is generated into `build/` and is a DEVELOPMENT key; a release one is a
policy about where a private key lives, not a Makefile rule.

**Still open on the USB pillar:** xHCI. It keeps its own enumeration path
rather than the shared `usb_core` one, so its ports are still scanned once at
boot; its `rescan` hook is left NULL and skipped rather than guessed at. Modern
machines put USB 3 ports on xHCI, so this is the half that a recent laptop
actually needs, and it is a real piece of work rather than a copy of the
three controllers that do share the core.

**Still open on the ACPI pillar:** battery, lid and thermal zone are WRITTEN
(`acpi_dev.c`) and have never run — QEMU emulates none of them, so the embedded
controller transactions underneath them are specification-derived and unproven.
Sleep states below S5 are not entered. `IndexField`/`BankField` are skipped
rather than half-implemented. Each is in docs/TODO.md with why.

**Still open on the audio pillar:** a descriptor is fixed at 21 ms (HDA's
cyclic engine fixes the buffer's total length before it starts, so
`audio_set_latency` below that is granted in name only), there is no capture,
no jack sensing, and a machine with two codecs takes the first with an output
path. Each is written up in docs/TODO.md with why. None of it is testable
beyond QEMU, and the driver prints the graph it found so the first real machine
says which of it was wrong.

**Still open on this pillar, and each needs the actual machine:** the Realtek
RTL8168/8169 in most modern consumer boards, and the Intel PCH parts in laptops
since about 2013 (I217/I218/I219). QEMU emulates NEITHER, so a driver for them
written here would be a guess with no way to check it -- which is the one thing
this tree does not ship. Both are separate drivers rather than extensions: the
8169 has a descriptor ring where the 8139 has a circular buffer, and the PCH
parts split the MAC from the PHY across an internal bus.

Also deliberately absent, and not blocking: TSO, checksum offload, MSI-X,
multiple queues, and asynchronous TX (both drivers wait for the card to report
the frame gone). Each is a separate way to be subtly wrong; they are additions
to a working driver, not prerequisites for one.

## Phase 2 — it installs itself

| Pillar | Today | Why |
|---|---|---|
| **Secure Boot** | ✅ **done for what can be tested** — the loader is Authenticode-signed by `tools/sbsign.py` (PE hash + PKCS#7, written here because the signing tools are Linux-only), keys are enrolled into the firmware's variable store by `tools/efivars.py`, and the loader reports the firmware's `SecureBoot` state and verifies the kernel's build hash before jumping. Proved as a PAIR: unsigned is REFUSED and signed BOOTS, on enforcing OVMF. `make test-secureboot` | Retail hardware ships with it on, and refuses an unsigned loader without a warning |
| **Installer** | absent | There is no way to put the OS onto the target's disk: partition (GPT), create the EFI system partition, format EMBKFS, copy the system, register the UEFI boot entry |
| **Partitions on 4096-byte-sector disks** | `partition.c` skips them | Many NVMe drives can be 4Kn; the installer must partition them |
| **Partitions on aarch64 at all** | never scanned | An ARM machine's disk is partitioned |
| **Encrypted install** | XTS and encrypted EMBKFS exist | Offer it in the installer, unlock at boot |

## Phase 3 — you can live in it

| Pillar | Today | Why |
|---|---|---|
| **Unicode keyboard input + your layout** | ✅ done — the key stream is UTF-8, layouts are codepoint tables with dead keys and AltGr, AZERTY ships, and the layout is picked in Settings (Keyboard) and applied by the shell at login. Proven byte-for-byte by `test keymap` | You could not type your own language |
| **Screen lock** | absent | Walking away from the machine leaves the session open |
| **Real HiDPI scaling** | `ui_scale` exists, clamped to 80–130% | A modern laptop panel needs 200%; the desktop looks half-size |
| **Time: NTP + time zones** | absent — the RTC only | The clock drifts and is in the wrong zone |
| **A persistent system log + crash reports** | absent — `syslog.h` is deliberately a declaration-only stub | A problem on the real machine cannot be diagnosed after the fact |
| **Notifications and status indicators** | notifications ✅ (`/run/emlink.notify`, user/system/notifyd -- banners from any program, and the desktop now reports a failed launch through it); status indicators still absent | Battery, network, volume, "update ready" have nowhere to appear |
| **System updates** | absent — `pkg` installs apps, nothing updates the OS | Every fix means reinstalling |

## Beyond the phases — added because the machine needs them

| Pillar | Today | Why |
|---|---|---|
| **Loadable kernel modules** | ✅ **done** — `kernel/module/`: an ELF64 relocatable object (the `.o` a compiler already emits) is placed, its undefined symbols resolved against an explicit export table, its relocations applied, and its code flipped to read-only + executable before `init` runs. Proved by loading a RAM-disk driver into a running kernel, reading back through the block layer what its code wrote, checking the page-table entry for W^X, and unloading it. `make test-modules` | A machine has a graphics chip, a wireless part and a touchpad this tree has never seen. The alternative to loading a driver is rebuilding the kernel — which means nobody else can add one |

## Phase 4 — it grows

IPv6. A packet filter. A general dynamic linker (today the kernel links exactly
one shared object, `libembk.so`). An asynchronous notification mechanism for
ported software (there are no signals, by design, and `posixdemo` says so).
exFAT, which USB sticks over 32 GB ship with. Wi-Fi and a WPA supplicant.
Bluetooth. Video playback. A PDF viewer.

## The question that decides the order inside phase 1

**Which machine?** The pillars above are chosen to cover the common parts, but
the exact network chip, Wi-Fi chip and audio codec of the target decide which
driver comes next, and whether it is x86 or ARM decides whether ACPI is the
blocker or the device tree is. `lspci -nn` from any Linux live USB on that
machine answers all of it at once.
