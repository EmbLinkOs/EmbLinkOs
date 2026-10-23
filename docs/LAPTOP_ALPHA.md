# Laptop alpha acceptance

The target is an installed, usable laptop OS. Physical compatibility remains
unverified. This integration preserves the newer master architecture and adds
the laptop paths to its current ABI and desktop.

| Goal | Implementation | Current acceptance |
| --- | --- | --- |
| WPA2, DHCP, DNS, webpage | Experimental PCIe AX200 transport, firmware, station/WPA2/CCMP, asynchronous DHCP and saved profiles | Independent security and firmware/MMIO/AP model tests; native emulated Ethernet resolves DNS and fetches a fixture page. Physical RF connection remains unverified. |
| UEFI and NVMe storage | GPT source identity, native read/write/flush, read-only recovery and verified installation | Recovery preserves its USB source; expanded installed disk boots alone through NVMe into account setup. Native filesystem/POSIX checks pass. |
| Built-in input | Intel LPSS DesignWare, native AML I2C-HID discovery and generic HID reports | Host protocol/report tests and native keyboard/mouse UI input. Physical laptop keyboard/touchpad remains unverified. |
| Battery and power | Native AML battery/lid/backlight cache, idle policy, upstream ACPI shutdown/reset | Synthetic firmware methods and QMP shutdown/reboot checks. Real firmware/EC behavior and sleep/wake remain open. |

The bootable image uses 512-byte logical GPT sectors. The NVMe driver can also
access native 4-KiB disks; the recovery clone installer currently accepts only
the standard 512-byte layout and overwrites a whole destination disk.

`power` capability is ID 12, session is 11, screen is 13, network_admin is 14.
Power information is syscall 131; shutdown/reset retain upstream syscall 118.
The current power path writes back cached pages, flushes block devices, sends
NVMe shutdown, then invokes platform power control. It does not yet provide a
complete freeze/drain protocol against concurrent writers or propagate every
page-cache writeback error. Hardware power-loss acceptance remains outstanding.

The active interpreter is native AML, not the retained uACPI reference adapter.
In particular, old uACPI EC/GPE host tests do not establish that native firmware
operation regions work on a target laptop. I2C input discovery uses native AML
and a bounded serial-bus resource parser; GPIO interrupt routing, gestures,
palm rejection and runtime reset recovery remain open.

For exact tested commands and limitations, see
[LAPTOP_VALIDATION.md](LAPTOP_VALIDATION.md), [WIFI.md](WIFI.md),
[HARDWARE_DRIVERS.md](HARDWARE_DRIVERS.md) and [DAILY_USE.md](DAILY_USE.md).
