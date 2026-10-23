# Laptop milestone validation

The laptop integration code is recorded in commit `be1d9bc`. Later commits add
captures, documentation and an explicit native-NVMe standalone boot assertion.
The tests below ran against
the rebuilt BIOS/userspace and GPT UEFI images on 2026-10-22/23.

## Build and host checks

The cloud build uses GCC 14.2, binutils 2.44, NASM, a C99-enabled newlib and QEMU
10.0.13. BIOS, userland and `uefi-usb.img` build successfully. Existing GCC 14
compatibility overrides permit implicit-declaration and callback-type warnings;
the unchanged ELF loader's `kprintf` declaration warning remains. This is not a
claim of a warning-free build on every host.

```sh
make -j4 images uefi-usb.img
TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' tests/drivers/run.sh
```

The complete ASan/UBSan host suite passes. It exercises EC and DesignWare I2C,
HID/I2C-HID, independent WPA2/CCMP vectors, station/AX200 firmware-MMIO/AP models,
DHCP/DNS/UDP filtering, Wi-Fi profile persistence, HDA/audio gain, installer GPT
and I/O failures, auth/profile boundaries, file short-I/O/Trash failures,
notifications, Intel display/PWM, HWP, idle policy, GOP/EDID, recovery cold reads
and explicit live boot.

## Native storage, IPC and session regressions

The final two-CPU UEFI live-image batch reports:

```text
PASS: test ipc cancel: exit=0, IPC counts restored=1 -> OK
PASS: test channel: exit=77 (want 77), surfaces 0->0, channels 0->0: OK
PASS: test compositor: exit=88 (want 88), surfaces 0->0, channels 0->0, endpoints 2->2: OK
PASS: test session drain: exit=0 -> OK
PASS: test posix: exit=0 -> OK
```

The IPC cancellation regression wakes real blocked accept, receive, send and
join calls; it verifies pending MOVE preservation, completed-operation ordering
and channel/endpoint counts. The drain regression holds the actual filesystem
lock inside an active syscall and proves its owner continues until it releases
that lock. The POSIX suite reports `ALL PASS`.

Separate two-CPU UEFI/NVMe runs pass POSIX, confined file/folder copy/move/Trash/
Restore, and ACPI policy including the no-HPET clock fallback. Earlier NVMe
persistence acceptance writes and flushes 64 KiB, then verifies every byte in a
separate boot of the retained disk. These are emulated-controller results.

## Actual GUI input and persistence

`tools/session_smoke.py` drives account setup, login and unlock with QMP keyboard
and pointer events and checks compositor/session state through GDB:

```text
session: account created and login displayed
session: authenticated desktop ready
session: lock gate and greeter active
session: wrong password leaves session locked
session: correct password restores desktop
session: Files/Terminal switching and session notification OK
session: logout drained disk writes, terminated the entire epoch and left a valid filesystem
session: account and login survive a new boot; notification history cleared
PASS: account persistence, login/lock/unlock, application switching, notifications and complete logout
```

Logout runs with a child repeatedly overwriting and synchronizing a 64-KiB file.
It leaves zero live old-epoch members and passes a cold filesystem check. Before
the IPC fix, two desktop service threads woke and immediately re-entered accept,
preventing logout; the same GUI path completes afterward.

`tools/desktop_smoke.py` also checks actual top-bar IPC into confined Settings,
mute/unmute, volume to 49%, firmware brightness 60% to 40%, and Power navigation.
The brightness/lid data come from a synthetic ACPI fixture. Separate native HDA
and AC97 streams were recorded, measured for duration/non-silence/frequency,
reopened, and released before child wait.

## Recovery and installation

`tools/recovery_smoke.py` boots Recovery from a source image padded by 64 MiB,
uses a destination only 16 MiB larger than the original image, and reports:

```text
PASS: padded USB source unchanged + recovery integrity + expanded NVMe install + target boots alone over NVMe into account setup
```

The source hash stays unchanged. The destination root expands, cold integrity
checks pass, and the target boots without the source disk. The standalone guest
attaches that disk as NVMe and asserts the native namespace was discovered. Only newly created
QEMU disk files are targets; no host storage device is written.

## Captures and remaining acceptance

The [progress gallery](media/laptop-progress/README.md) contains original-size,
lossless PNG exports and a website-ready JSON manifest. All screens are native
EmbLinkOS execution in QEMU; synthetic ACPI data are labeled individually.

Physical AX200 RF/WPA2/DHCP/DNS, laptop storage/input/HDA/panel/power compatibility,
general native GPU modesetting/rendering and S3/S0ix wake restoration remain
unverified or unimplemented as described in [DAILY_USE.md](DAILY_USE.md),
[WIFI.md](WIFI.md) and [TODO.md](TODO.md). These tests do not establish daily-driver
readiness. Browser sources and EmbCC are unchanged by this milestone.
