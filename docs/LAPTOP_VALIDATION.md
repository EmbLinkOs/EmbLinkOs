# Laptop integration validation

Validated on 2026-10-23 after integrating master `a281865` (210 incoming
commits) into `laptop-daily-use`. Earlier results and captures belong to
`be1d9bc`–`c90c478`; they are historical rather than evidence for the merged OS.
The current native tests use rebuilt images and disposable QEMU disks.

## Build and host checks

GCC 14.2, binutils 2.44, NASM, C99-enabled newlib and QEMU 10.0.13 build the
x86 BIOS/userspace and GPT UEFI USB images. The cloud wrapper supplies GCC
compatibility warning overrides and dependency paths; this is not a warning-free
build claim. The ARM64 freestanding kernel also cross-links with Debian GCC
14.2 (`-fno-pie -no-pie`); ARM userspace/runtime was not revalidated here.

```sh
make -j4 FB_W=1280 FB_H=800 images uefi-usb.img
python3 tools/check_native_abi.py
tests/drivers/run.sh
TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' tests/drivers/run.sh
```

All 30 host tests pass both normally and with ASan/UBSan. Coverage includes
WPA2/CCMP, AX200 firmware/MMIO/AP transport, DHCP/DNS filtering, profile
persistence, HID/I2C and hostile ACPI I2C resources, HDA/audio gain, installer
failures, auth records/profile policy, file short-I/O/Trash failures,
notifications, display/PWM, HWP, idle policy, boot modes and recovery reads.
EC/uACPI transport tests exercise retained reference code; the merged kernel
uses native AML and does not link that older adapter.

The ABI check reports **143 unique syscalls and 14 matching capabilities**.
Incoming syscall IDs 1–128 and capability meanings are preserved. Laptop
services occupy 129–143; network_admin is capability 14.

## Native OS regression batch

`python3 tools/integration_smoke.py` boots one disposable UEFI/NVMe live image
with two CPUs and reports:

```text
[cmd] test devices: exit=0 -> OK
[cmd] test devices preferences: exit=0 -> OK
[cmd] test devices files: exit=0 -> OK
[cmd] test ipc cancel: exit=0, IPC counts restored=1 -> OK
[cmd] test channel: exit=77 (want 77), surfaces 0->0, channels 0->0: OK
[cmd] test compositor: exit=88 (want 88), surfaces 0->0, channels 0->0, endpoints 6->6: OK
[cmd] test session: OK
[cmd] test kill io: OK
[cmd] test posix: exit=0 -> OK
```

These exercise actual capability checks, confined preferences and file/folder
copy/move/Trash/Restore, blocked IPC cancellation, resource reclamation, native
session teardown and kill during filesystem I/O. The POSIX suite reports
`ALL PASS`. This uses the incoming safe-kill/session implementation rather than
the earlier branch's separate teardown controller.

## Actual GUI controls, authentication and persistence

Compile the synthetic firmware fixture, then run:

```sh
iasl -p /tmp/emblink-power tests/acpi/power.asl
EMBLINK_ACPI_TABLE=/tmp/emblink-power.aml python3 tools/desktop_smoke.py
python3 tools/lock_test.py
```

The UEFI desktop test drives real keyboard/pointer events and independently
checks kernel state through GDB. It creates an account, signs in, opens confined
Settings from the dock, changes mute/volume, verifies brightness 60% → 40%, opens
native notification banners/history, locks, rejects a wrong password, unlocks
and logs out. A separate boot of the same NVMe image reaches login without
setup, accepts the account, restores the saved volume and passes cold integrity:

```text
PASS: native account/login, confined Settings, mute/volume, brightness fixture, history, lock/passwords, logout, persistent account/preferences and cold integrity
```

The independent four-CPU BIOS lock test reports zero failed steps. It checks
that explicit live sessions refuse locking, then creates an authenticated
session and proves that the opaque lock covers visible dock/icons, blocks
GUI+Tab, refuses wrong passwords and restores the desktop on successful proof.
Its image regions are measured from the actual raster dimensions.

## Audio, networking and power

`tools/hardware_smoke.py` passes `test devices audio` on HDA and AC97. Recorded
WAVs independently pass `tools/audio_check.py --hz 440 --min-seconds 3.4`:

| Backend | Sound duration | Non-silent | Measured frequency |
| --- | --- | --- | --- |
| Intel HDA | 3.46 s | 100% | 440.4 Hz |
| AC97 | 3.49 s | 100% | 440.5 Hz |

The native test reopens playback and exits a child while it owns the device;
last-thread cleanup releases it before the parent waits. Measurement caught and
fixed AC97 wraparound drainage and reset bugs; stream ownership and PCM staging
are serialized for concurrent callers.

`python3 tools/network_smoke.py` passes DNS and an HTTP page fetch through the
actual QEMU e1000e driver, DHCP, DNS and TCP stack. Deterministic host fixtures
receive the guest DNS query and HTTP request. This proves the shared network
path, not public Internet access or physical Wi-Fi RF/WPA2 association.

Native synthetic-AML tests pass battery/lid/backlight bounds, brightness round
trip, idle dim/restore and policy/session authority checks. UEFI power tests
observe QMP `guest-shutdown` and `guest-reset` after page-writeback/device-flush
markers. They do not establish power-loss durability, a complete writer freeze,
propagation of every writeback error or physical firmware compatibility.

## Recovery and installation

`python3 tools/recovery_smoke.py` reports:

```text
PASS: padded USB source unchanged + recovery integrity + expanded NVMe install + target boots alone over NVMe into account setup
```

The source is padded by 64 MiB, while the disposable destination is only 16 MiB
larger than the original image. Recovery mounts the source read-only, rejects
it as an erase target, verifies copied data and expands the destination root.
The source hash stays unchanged. With the source absent, the target boots its
own EFI loader through native NVMe into account setup and passes integrity.

## Captures and physical acceptance

The [progress gallery](media/laptop-progress/README.md) contains 40 lossless PNGs
and website metadata. Captures 01–20 document the earlier milestone; 21–40 show
the integrated OS. The current desktop captures include a synthetic ACPI fixture,
explicitly labeled in captions/metadata. No screenshot is a physical laptop test.

Physical AX200, built-in input, HDA routing, panel, storage and firmware behavior
remain unverified. Native AML coverage, robust shutdown error handling, broader
radios/GPUs, native modesetting, mixing/capture and S3/S0ix remain open. See
[DAILY_USE.md](DAILY_USE.md), [LAPTOP_ALPHA.md](LAPTOP_ALPHA.md),
[HARDWARE_DRIVERS.md](HARDWARE_DRIVERS.md) and [WIFI.md](WIFI.md).
Incoming browser work is preserved; no browser feature was developed here.
EmbCC was unchanged and its independent test suite is not claimed as passing.
