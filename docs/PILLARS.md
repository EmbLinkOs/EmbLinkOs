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
unreachable — because of the four pillars in phase 1.

## Phase 1 — it boots and runs on the machine

Hardware nobody can route around. Each of these is testable on QEMU, which
emulates the common real part, so none has to wait for the machine.

| Pillar | Today | Why it blocks the goal |
|---|---|---|
| **NVMe** | ✅ done — both architectures, root filesystem verified on it | Most machines built since ~2016 boot from NVMe |
| **ACPI AML interpreter** | absent — MADT and HPET are parsed, the DSDT is not (`power_x86.c` says so) | Power-off does not work on real x86 hardware; no sleep, no battery, no lid, no thermal |
| **A real network card** | absent — virtio-net only | A physical machine has no network at all. Intel e1000e and Realtek r8169 cover most wired machines |
| **Intel HD Audio** | absent — AC97 and virtio-snd only | AC97 left real hardware around 2008; no sound |
| **USB hot-plug + mass storage mount** | ports scanned once at boot; no hot-plug | A USB stick plugged in after boot does nothing |
| **Native-resolution UEFI framebuffer** | GOP handoff exists (`boot/uefi/loader.c`), untested on hardware | The first thing seen on the machine |

## Phase 2 — it installs itself

| Pillar | Today | Why |
|---|---|---|
| **Installer** | absent | There is no way to put the OS onto the target's disk: partition (GPT), create the EFI system partition, format EMBKFS, copy the system, register the UEFI boot entry |
| **Partitions on 4096-byte-sector disks** | `partition.c` skips them | Many NVMe drives can be 4Kn; the installer must partition them |
| **Partitions on aarch64 at all** | never scanned | An ARM machine's disk is partitioned |
| **Encrypted install** | XTS and encrypted EMBKFS exist | Offer it in the installer, unlock at boot |

## Phase 3 — you can live in it

| Pillar | Today | Why |
|---|---|---|
| **Unicode keyboard input + your layout** | the key stream is 7-bit ASCII; `keyboard.c` refuses AZERTY by name because é è ç à ù cannot be typed | You cannot type your own language |
| **Screen lock** | absent | Walking away from the machine leaves the session open |
| **Real HiDPI scaling** | `ui_scale` exists, clamped to 80–130% | A modern laptop panel needs 200%; the desktop looks half-size |
| **Time: NTP + time zones** | absent — the RTC only | The clock drifts and is in the wrong zone |
| **A persistent system log + crash reports** | absent — `syslog.h` is deliberately a declaration-only stub | A problem on the real machine cannot be diagnosed after the fact |
| **Notifications and status indicators** | absent | Battery, network, volume, "update ready" have nowhere to appear |
| **System updates** | absent — `pkg` installs apps, nothing updates the OS | Every fix means reinstalling |

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
