# Laptop hardware support

The target is a usable laptop OS. These are initial drivers, not a claim of
physical laptop compatibility. No laptop has been tested yet.

| Area | Implemented | Validation and remaining work |
| --- | --- | --- |
| Audio | Intel HDA analog output discovery, CORB/RIRB, MSI, PCM DMA; existing AC'97; live software volume/mute | One selected route and playback owner. QEMU WAV and host transport/sink checks; no microphone, mixing, jack switching, DSP/SoundWire or codec-specific laptop amplifier support. Physical routing untested. |
| Backlight | Standard ACPI `_BCL`/`_BCM`/`_BQC`; native Intel SPT/CNP PWM fallback | Synthetic AML round trip and native PWM register model; physical panel untested. See [LAPTOP_DESKTOP.md](LAPTOP_DESKTOP.md). |
| Graphics | UEFI preferred-mode selection; validated Intel Gen9 firmware scanout handoff | No native modesetting, connector management or 3D; software rendering. See [DAILY_USE.md](DAILY_USE.md). |
| CPU energy | Per-core Intel HWP/EPP with CPUID gating and advertised performance bounds | Register model tests. No measured battery-life or suspend/resume acceptance. |
| NVMe | PCI class-based discovery, admin/I/O queues, namespace identify, read/write/flush, native 512-byte through 4-KiB blocks | QEMU: full POSIX suite on both 512-byte and 4-KiB namespaces with two CPUs. Polling, one command at a time; two controllers and eight namespaces per controller maximum. No hotplug, interrupt mode, reset recovery or suspend restore. |
| Wired Ethernet | Intel 8254x/82574 (listed IDs in `e1000.c`), Realtek RTL8139, existing virtio-net | QEMU e1000, e1000e and rtl8139: DHCP, gateway ICMP and byte-verified 64-KiB HTTP transfers with an initially empty ARP cache. Polling; one selected interface. I219/I225 and RTL8168/8125 need separate drivers. Physical PHY behavior remains untested. |
| Battery/firmware | Native AML interpreter; cached `_BST`, `_BIF`/`_BIX`, `_PSR` and `_LID`; upstream shutdown/reset | Native synthetic SSDT round trips pass. Physical EC/battery/GPE behavior is unverified; suspend/resume remains open. |
| Built-in I2C input | Intel LPSS DesignWare master and ACPI HID-over-I2C discovery/reset/input, generic HID report decoder, native keyboard/pointer integration | Host-model tests cover FIFO bounds, repeated starts, reset completion, keyboard edges/modifiers, relative mouse and primary-contact touchpads. QEMU verifies native key/text integration. Intel PCI IDs are listed in `designware.c`; AMD controllers and GPIO interrupt routing are not implemented. Physical hardware testing remains pending. |
| Wi-Fi | Experimental Intel AX200 PCIe (`8086:2723`), API-77 firmware, DMA command/RX/TX queues, passive scan, legacy association, WPA2-Personal/CCMP and Ethernet-stack integration | Firmware/MMIO/AP model passes an association/handshake/data exchange and 1,100 TX with queue wrapping. Physical radio acceptance is outstanding. AX201 CNVio, AX210, Realtek, hidden SSIDs, WPA3/enterprise/required PMF and HT/VHT aggregation are unsupported. See [WIFI.md](WIFI.md) for setup and limits. |

The merged kernel uses the repository's native AML interpreter and SCI/PCI
routing. Laptop methods now use that API, including serialized AML execution
and typed buffer/package arguments. The older uACPI host, EC adapter and vendor
source remain as reference and host-model code; they are **not linked into the
kernel**. Their EC/GPE/global-lock behavior is therefore not active laptop
support. Native AML coverage and real firmware compatibility need hardware
acceptance. Missing or invalid readings remain unavailable.

The receive path learns the immediate neighbor from a validated unicast IPv4
Ethernet frame before generating replies. This avoids the receive worker waiting
for itself to deliver an ARP response during the first inbound TCP connection.
Header checksum, length, fragmentation and destination are checked before learning.
This has ordinary ARP trust semantics; it does not authenticate neighbors.

## I2C and embedded-controller details

The retained EC transport has bounded host-model tests. Its uACPI integration
is not active after the merge. Native firmware operation-region coverage must
be validated before claiming EC-backed battery or lid support on a laptop.

The Intel I2C driver selects known 120-MHz LPSS controllers, checks the Synopsys
component ID, disables DMA/interrupts and uses conservative standard/fast timings.
It supports 7-bit addresses and firmware bus speeds from 100 to 400 kHz, combined
write/read transfers, repeated START and STOP, and bounds outstanding reads by
receive FIFO capacity. A controller timeout stops that bus until reboot.

HID discovery resolves `_CRS` controller paths and PCI bridge ancestry, evaluates
the standard HID `_DSM`, powers the device, sends RESET, waits for completion,
then reads its report descriptor. No arbitrary I2C addresses are probed. Input is
polled at 100 Hz. This avoids requiring GPIO interrupt routing for the initial
driver but uses more power, and zero-length idle reads cannot be distinguished
from unsolicited reset notifications without GPIO state. They are ignored during
normal polling; runtime reset recovery remains unfinished. Device power-resource
sequencing beyond `_PS0` is also unfinished.

The report decoder handles short HID items, report IDs, variable/array keyboard
fields and relative/absolute pointer fields. Keyboard events feed the existing
layout, modifiers, text and key-release streams. Touchpads use primary-contact
motion plus physical buttons; multi-finger gestures, palm rejection, tap-to-click,
haptic output and HID LED output remain unfinished. Long HID descriptor items and
layouts above the documented fixed buffer/field/report limits are rejected.

## Reproduce checks

Build `make images` with the cross compiler and dependencies configured, then use
`tools/hardware_smoke.py` with `qemu-system-x86_64` and `curl` on PATH. It boots a disposable
copy of `embkfs.img`; logs and the copy are retained in a printed temporary path.
Run tests sequentially (the HTTP test uses host TCP port 18080).

```sh
# Host tests use the same protocol/report code as the real drivers.
TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' tests/drivers/run.sh
EMBLINK_CPUS=2 EMBLINK_DISK=nvme EMBLINK_SMOKE_COMMAND='test hidkeys' \
  EMBLINK_SMOKE_EXPECT='[cmd] test hidkeys: OK' python3 tools/hardware_smoke.py

EMBLINK_CPUS=2 EMBLINK_DISK=nvme python3 tools/hardware_smoke.py
EMBLINK_CPUS=2 EMBLINK_DISK=nvme EMBLINK_NVME_SECTOR=4096 python3 tools/hardware_smoke.py

# Repeat with e1000e and rtl8139.
EMBLINK_CPUS=2 EMBLINK_DISK=nvme EMBLINK_NIC=e1000 \
  EMBLINK_SMOKE_COMMAND='test httpdbig' EMBLINK_SMOKE_EXPECT='[cmd] test httpdbig: OK' \
  EMBLINK_NETDEV_OPTIONS=',hostfwd=tcp:127.0.0.1:18080-:8080' \
  EMBLINK_HTTP_VERIFY=1 python3 tools/hardware_smoke.py

iasl -p /tmp/emblink-power tests/acpi/power.asl
EMBLINK_CPUS=2 EMBLINK_DISK=nvme EMBLINK_ACPI_TABLE=/tmp/emblink-power.aml \
  EMBLINK_SMOKE_COMMAND='test power' EMBLINK_SMOKE_EXPECT='[cmd] test power: OK' \
  EMBLINK_POWER_FIXTURE=1 python3 tools/hardware_smoke.py
```

The SSDT is a test fixture. It does not emulate a physical EC or battery bus.
Do not load it on real hardware.

See [LAPTOP_ALPHA.md](LAPTOP_ALPHA.md) for the four acceptance goals and current implementation limits.

Desktop integration and reproducible device ABI checks: [LAPTOP_DESKTOP.md](LAPTOP_DESKTOP.md).
