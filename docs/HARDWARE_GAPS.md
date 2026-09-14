# Hardware this machine can emulate and this OS cannot drive

**Companion to [PILLARS.md](PILLARS.md), held to the same goal:** EmbLinkOS as
the main operating system of a separate, physical machine. PILLARS.md asks
"what is missing"; this asks the narrower and more actionable question —
**what can we build and TEST right now, on the machine we develop on.**

Everything below was checked on 2026-09-14 against `qemu-system-x86_64 -device
help`, `qemu-system-aarch64 -M virt -device help`, and the driver files in
`kernel/drivers/`. Not recalled. Where something is marked untestable, the
reason is named.

## What is already driven

| Class | Drivers |
|---|---|
| Storage | ATA/IDE, AHCI, NVMe, virtio-blk, USB mass storage (Bulk-Only Transport) |
| Network | virtio-net, e1000 (covers e1000e), rtl8139 |
| Display | bochs/VGA (`bochs_vbe.c`), virtio-gpu, UEFI GOP framebuffer |
| Input | i8042 PS/2, USB HID (keyboard + tablet), virtio-input |
| Audio | AC'97, Intel HDA, virtio-snd — behind a runtime `struct pcm_driver` table |
| USB host | UHCI, OHCI, EHCI, xHCI (+ single-level hubs, not on the hot-plug path) |
| Timers | PIT, HPET, RTC, LAPIC timer, ARM generic timer |
| Firmware | ACPI tables + a real AML interpreter; `_PRT` PCI interrupt routing |
| Entropy | RDRAND/RDSEED where the CPU has them (`cpu_features.c`) |

---

## A. Matters on a real machine

These are the ones where "we cannot drive it" means a person's hardware does
not work.

| Device | QEMU | Why it matters | Today |
|---|---|---|---|
| **I²C / SMBus** | `smbus-ipmi`, `i2c-ddc`, `i2c-echo` | SMBus reaches the battery, the memory SPD, and a monitor's EDID over its DDC lines | ✅ **host controller done** — `kernel/drivers/i2c/smbus.c` drives the PIIX4 and ICH9 hosts, probes the bus, and reads + validates EDID (header *and* checksum). `make test-i2c`. ⚠️ **This is not the touchpad.** I²C-HID hangs off an Intel LPSS or AMD designware controller, which **QEMU does not emulate at all** — that driver can only be written against the target machine |
| **SD / eMMC** | `sdhci-pci`, `sd-card`, `emmc` | Laptop card readers. Many ARM boards *boot* from eMMC | ✅ **done** — `kernel/drivers/storage/sdhci.c`, PIO. `make test-sdcard` covers SD (byte addressing), SDHC (block addressing) and eMMC (capacity from the extended CSD). ⚠️ No DMA yet: 128 register reads per block |
| **usb-net** | `usb-net` | A USB ethernet dongle is how you get networking on a laptop whose wireless chip has no driver | ✅ **RNDIS done** — `kernel/net/usb_net.c`, registered in the `net_driver` table below the PCI cards. DHCP, DNS, TCP and UDP all verified over it. ⚠️ **CDC-ECM is what most real dongles speak and QEMU emulates none**, so that path is detected and refused rather than guessed at |
| **usb-uas** | `usb-uas`, `usb-bot` | We speak Bulk-Only Transport only. A USB 3 stick negotiates UAS; BOT still works as a fallback, but UAS is the fast path | ✅ **done** — `kernel/drivers/usb/usb_uas.c`. BOT remains the fallback for a device whose pipes are not named. ⚠️ One command at a time: the tagging is on the wire but nothing above it is asynchronous yet |
| **ATAPI / ISO9660** | `ide-cd`, `scsi-cd` | No optical path at all — no ATAPI command set, no ISO9660 filesystem. El Torito is already open in TODO.md | ✅ **done** — `drivers/storage/atapi.c` (SCSI down an IDE cable) and `fs/iso9660.c` (read-only, because the medium is). `make test-cdrom` reads the same disc over IDE **and** virtio-scsi. ⚠️ No El Torito: a disc can be read, not booted from |
| **igb** | `igb` (Intel 82576) | Closer to modern Intel server parts than e1000. `i82559`/`i8255x` covers a lot of older machines | ✅ **both done** — `kernel/net/igb.c` (advanced descriptors; the link comes up through the PHY, not CTRL.SLU) and `kernel/net/i8255x.c` (a linked list of command blocks, MAC from a three-wire EEPROM). `make test-nics` covers both |
| **TPM** | `tpm-tis`, `tpm-crb` | The natural continuation of Secure Boot: measured boot, and sealing the EMBKFS encryption key to the boot state so a stolen disk is useless | absent. ⚠️ **Not testable here without installing `swtpm`**, which is not on this host |
| **IOMMU** | `intel-iommu`, `amd-iommu`, `virtio-iommu` | Any device can currently DMA anywhere in memory. This is what stops a malicious Thunderbolt device | absent |
| **SCSI / SAS HBAs** | `megasas`, `mptsas1068`, `lsi53c895a`, `am53c974`, `pvscsi` | Workstations and servers. Lower priority than the laptop parts above | ✅ **all five done**, on the shared SCSI layer: `megasas.c` (a firmware mailbox), `pvscsi.c` (paravirtual rings), `mptsas.c` (message-passing FIFOs), `esp.c` (a 16-byte FIFO and the bus phases walked by hand), `lsi53c895a.c` (a processor that executes a program the driver writes). `make test-hba` runs the same disk and the same read/write/verify through all five and virtio-scsi |
| **UFS** | `ufs` | Modern phone and thin-laptop storage | absent |

---

## B. Platform plumbing the firmware already describes and nothing listens to

Each of these is something the machine is *offering* — the information is
already in the tables the AML interpreter now reads.

| Thing | QEMU | Note |
|---|---|---|
| **PCI bridge configuration** | `pcie-root-port`, `pci-bridge`, `pxb` | `pci_init` brute-forces all 256 buses, so devices *behind* a bridge are found. It never **configures** one — secondary/subordinate numbers, memory windows. Fine when firmware did it; not fine for hot-plug or a firmware that left something unassigned |
| **CPU hot-plug** | built into `-smp` | `test amldump` shows `\_SB.CPUS` with a `_EJ0` on every processor. The firmware is offering it and nothing listens |
| **Memory hot-plug** | `pc-dimm`, `nvdimm`, `virtio-mem` | Same shape: declared in the DSDT, ignored |
| **`acpi-erst`** | `acpi-erst` | Persists crash records across a reboot. This is the phase-3 "persistent syslog + crash reports" pillar, with hardware help |
| **`pvpanic`** | `pvpanic`, `pvpanic-pci` | Tells the host the guest panicked. A dozen lines; makes CI failures loud instead of a timeout |
| **Watchdog** | `i6300esb` | Auto-reset on a hang |
| **`isa-debug-exit`** | `isa-debug-exit` | Lets the guest exit QEMU with a status code. Would let a test return pass/fail **directly** instead of every harness scraping serial for a verdict string — see `tools/console_test.py` and the six other harnesses that all reimplement that |

---

## C. virtio we do not drive

| Device | Why it is worth something |
|---|---|
| **`virtio-9p` / `virtio-fs`** | Share the build directory straight into the VM. **Pays for itself immediately in development turnaround** — no image rebuild to test a changed file |
| `virtio-rng` | An entropy source. x86 has RDSEED; **aarch64 has nothing** |
| ~~`virtio-scsi`~~ | ✅ done — `drivers/storage/virtio_scsi.c`, on the shared SCSI layer. `make test-scsi` |
| ~~`virtio-console`~~ | ✅ done — `drivers/char/virtio_console.c` |
| ~~`virtio-balloon`~~ | ✅ done — `drivers/misc/virtio_balloon.c` |
| `virtio-crypto` | Offload |
| `virtio-iommu` | See IOMMU above |
| `virtio-pmem` | Persistent memory |
| `vhost-vsock` | Host/guest sockets |
| `virtio-multitouch` | Touchscreens |

---

## D. Deliberately not worth it

Said out loud so the absence does not read as an oversight.

`pcnet`, `tulip`, `ne2k`, `rocker` (legacy or toy NICs) · `cirrus-vga`,
`vmware-svga`, `ati-vga`, `qxl` (legacy or SPICE-specific display) · floppy ·
CAN bus, IndustryPack, SSI serial flash · `edu`, `pci-testdev`, `pc-testdev`,
`iommu-testdev` · `applesmc`.

**On display specifically:** QEMU cannot emulate an Intel, AMD or NVIDIA GPU.
For real hardware the UEFI GOP framebuffer we already have **is the ceiling**
until somebody writes a native GPU driver, and nothing in QEMU's device list
moves that. `ramfb` is the one small exception — a plain firmware-set
framebuffer, mildly useful on aarch64 where there is no VGA to fall back to.

---

## If you pick three

1. ~~**I²C / SMBus**~~ — the host controller is done. What is left of it is
   I²C-HID for the touchpad, and that needs the target machine: no emulator
   here has an LPSS/designware controller to develop against.
2. ~~**usb-net**~~ — done for RNDIS. CDC-ECM needs a real dongle to develop
   against; its MAC lives in a string descriptor named by a functional
   descriptor the USB core does not parse yet.
3. **virtio-9p** — the only item here that makes every *future* item cheaper.
