# Installation, recovery and daily sessions

This milestone connects boot recovery, installation, authenticated sessions,
file operations, notifications, display handoff and power preferences. Browser
sources are unchanged. It is still a laptop alpha: physical compatibility,
general native GPU modesetting and suspend/resume are outstanding.

## Boot and installation

EmbBoot offers normal boot, Recovery, Diagnostics and an explicit live desktop.
Normal boot opens account setup on an empty system, then password login. The
live desktop uses `/home/.live-guest`, separate from account homes, and has
no password lock. The EFI loaded-image
device path identifies the actual boot disk by its GPT partition GUID, so an
internal disk enumerating before the boot USB does not silently become root.
Legacy BIOS boot records remain compatible with the extended boot protocol.

Recovery starts the kernel console without a desktop or network stack. EMBKFS
mounts read-only before the normal superblock repair and orphan-sweep paths.
Only the boot disk's root is mounted; other disks remain available as installer
targets. Recovery accepts these commands from the keyboard or serial console:

```text
recovery check
recovery report
install list
install plan sdb
poweroff
reboot
```

`recovery check` checks the mounted live tree's node checksums, parent pointers,
key ranges, levels, slot/payload bounds, cycles and root inode. It reads every
regular file through checksum verification, invalidating previous read caches
first. It reports errors without repairing or committing metadata. Snapshot
roots, a complete allocation-overlap audit and arbitrary corruption repair are
not part of this scanner. Diagnostics runs this check automatically at boot.
`recovery report` prints boot, device and mount information. Capture the serial
console for a retained report; there is no persistent panic-log store yet.

`install list` shows whole disks, capacity and the running source disk. ATA,
SATA and NVMe Identify data supply model and serial numbers when available.
`install plan <disk>` validates the source and target without writing. It prints
an exact `install erase ... ERASE-<disk>-<fingerprint>` command for that plan.
Executing that command overwrites the selected target's existing contents.
The running source, partition devices, mounted targets, targets smaller than the source GPT image, and
unsupported layouts are refused.

Installation supports the standard EmbLinkOS GPT image: one ESP and a trailing
EMBKFS root, presented as 512-byte logical sectors. The source must have valid,
matching primary/backup GPT and filesystem superblocks. A raw image flashed to a
larger USB is supported: unused space after the image does not require an equally
large destination. The kernel checks the
source filesystem before copying. New disk/partition GUIDs and an MBR signature
come from hardware entropy. Target payload writes are flushed and read back;
new boot metadata is published after the payload is verified. The target root
partition and both EMBKFS superblocks grow to use the target's capacity, keeping
existing data/tree block addresses intact and updating the free-block count.
No host disk is modified by the smoke test: its target is a new QEMU image.

After installation, shut down, remove the source media and select the installed
disk in UEFI. The loader remains at `/EFI/BOOT/BOOTX64.EFI`. Installation does
not edit firmware Boot#### variables, preserve another OS's partitions, or
provide in-place OS-image updates/rollback. The filesystem UUID and existing
accounts/data are cloned. To install a fresh account, start from a fresh build
image rather than an already-personalized source.

## Authentication and session lifetime

The development autologin shortcut is removed. Init alone supervises setup,
login and the unlock greeter. Passwords use salted PBKDF2-SHA256 (100,000 rounds
for new records; bounded legacy records remain readable). Account files and
session profiles use flushed replacements. Shadow is the authentication commit
point; retrying an interrupted passwd-only creation avoids duplicate entries.
These records are not a substitute for disk encryption.

User-editable `user.ns` profiles cannot ask init for arbitrary authority. Bindings
must stay within the authenticated home or `/run`, or read-only `/system` and
`/data/apps`, with component boundaries and traversal checks. The desktop's
capabilities exclude debugging, raw disk access and kernel extensions.

A kernel session epoch follows all descendants, including orphaned apps. Lock
hides other windows and isolates keyboard/pointer/clipboard access while init
runs the greeter. Killing/closing the greeter does not unlock. Only successful
proof for the current account lets init release the gate. Raw framebuffer
presentation is unavailable once a managed session exists. Clipboard contents
are cleared on locking and logout. App key events go to the focused window.

Use **Lock Screen** in the EmbLink menu, **Ctrl+Alt+L**, or **Super+L**. **Alt+Tab**
and **Alt+Shift+Tab** cycle normal visible windows in stable window order.
The compositor supports 32 windows and reserves two slots for the greeter.
Logout blocks further old-session spawns, terminates every session member,
drains in-flight syscalls so filesystem/device locks can be released, waits for
live threads to stop and returns to login. A newly scheduled process
also checks its session lifetime before entering userspace.
IPC accept/receive, sends waiting for queue space, and thread joins honor
cancellation before blocking again. Completed operations still win; a cancelled
pending send leaves its message and ancillary handles uncommitted.

The serial kernel console remains a privileged development interface. This is
not a complete multi-user security model or a physical-access security claim.

Disposable app screenshots explicitly request the live session through QEMU's
`opt/emblink/live=1` fw_cfg file. The kernel checks the hypervisor vendor and
fw_cfg signature, bounds the directory and requires that exact opt-in. Normal
VM boots and physical boots authenticate; recovery selection takes precedence.

## Files and notifications

Files supports file/folder copy, Cut/Move, cross-window file clipboard entries,
rename without replacement, Trash and Restore. Copies use private staging,
handle short writes and read errors, flush contents and publish with an atomic
no-replace rename. Pasting into the source or onto an existing file preserves
existing data. Folder traversal is bounded to 16 levels/100,000 entries and
refuses symlinks/special files. Moves require the same filesystem. A crash can
leave a staging item; there is no automatic staging recovery yet.

Trash stores an item with its original path in a per-item container under
`$HOME/Trash`. Metadata and movement are synchronized. Restore checks the home
boundary and refuses an occupied original name. Select the container in Trash
and choose Restore, or use Undo Trash immediately after moving an item. The
separate Delete action remains permanent and refuses non-empty directories.

The top bar has a notification center with paged history and Clear. The kernel
keeps eight bounded messages, assigns source PID/sequence/time, checks UTF-8 and
rate limits delivery. Histories are session-scoped, hidden while locked and
cleared for the next session. System battery warnings are distinguished from
application messages. Native applications can use `embk_notify_send`; from a
terminal:

```sh
/data/apps/notify/notify.elf "Build" "Finished successfully"
```

The clipboard still supports text; file operations use a recognizable text
record. Rich MIME negotiation, notification actions and a persistent history
are not implemented.

## Graphics and power

The UEFI loader validates linear RGB/BGR GOP modes and framebuffer bounds. A
checksum-valid EDID preferred timing is selected when supported. It rejects
unsupported pixel layouts instead of treating arbitrary bitmasks as BGR.
VirtIO mode queries now reuse their existing resource rather than attempting to
create the same resource twice.

The Intel display driver has an explicit Gen9 Skylake/Kaby Lake/Coffee Lake/
Comet Lake device-ID list. It verifies enabled linear XRGB8888 plane dimensions,
stride, offset and aperture against the firmware framebuffer, preserving the
firmware's timing, trained links and GTT. Recognized Sunrise Point/Kaby Point
and Cannon Point PCH layouts expose native PWM brightness when firmware already
enabled a valid controller. Duty writes preserve frequency, enable and polarity;
unsupported modes/controllers fall back rather than receiving guessed writes.
ACPI `_BCL/_BCM/_BQC` remains the preferred brightness backend.

This is firmware-mode handoff plus native brightness, **not** native resolution
changes, connector discovery/hotplug, external-display management, GPU rendering,
Gen11/Xe support or an AMD/NVIDIA driver. Rendering remains software. Intel
register behavior is model-tested; physical panel validation is outstanding.

Power preferences persist in the user's settings: idle dimming, idle lock,
lid-close lock and processor energy profile. Dimming uses actual supported panel
brightness and restores the previous level on input. Keyboard, PS/2, USB and
I2C pointer paths record activity. Fresh low-battery readings generate alerts.
Policy polling also runs through the firmware worker, so a busy debug-console
command does not stop idle policy. Clock fallback uses calibrated TSC or correctly
scaled timer ticks when HPET is absent; firmware/UI timestamps share that clock.

Intel HWP is enabled only after Intel vendor/CPUID capability checks. Each core
uses its own advertised performance bounds, autonomous performance selection
and EPP for Balanced/Save energy/Performance. Unsupported CPUs do not receive
MSR writes or advertise selectable profiles. This is model-tested, not a measured
physical battery-life result. Existing idle threads already use HLT.

**S3/S0ix suspend, device quiesce/resume, wake restoration and automatic sleep
on lid close remain unimplemented.** A safe implementation needs coordinated
storage, USB, Wi-Fi, audio, display and interrupt/timer restoration; the UI does
not advertise sleep as working.

## Reproducing validation

Cross-build images, then run host and native acceptance:

```sh
make -j4 images uefi-usb.img
TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' tests/drivers/run.sh
python3 tools/recovery_smoke.py
python3 tools/session_smoke.py

iasl -p /tmp/emblink-power tests/acpi/power.asl
EMBLINK_ACPI_TABLE=/tmp/emblink-power.aml python3 tools/desktop_smoke.py
EMBLINK_FIRMWARE=uefi EMBLINK_DISK=nvme EMBLINK_CPUS=2 \
  EMBLINK_SMOKE_COMMAND='test ipc cancel' \
  EMBLINK_SMOKE_EXPECT='IPC counts restored=1 -> OK' python3 tools/hardware_smoke.py
EMBLINK_FIRMWARE=uefi EMBLINK_DISK=nvme EMBLINK_CPUS=2 \
  EMBLINK_SMOKE_COMMAND='test session drain' \
  EMBLINK_SMOKE_EXPECT='[cmd] test session drain: exit=0 -> OK' python3 tools/hardware_smoke.py
EMBLINK_FIRMWARE=uefi EMBLINK_DISK=nvme EMBLINK_CPUS=2 \
  EMBLINK_SMOKE_COMMAND='test devices files' \
  EMBLINK_SMOKE_EXPECT='[cmd] test devices: exit=0 -> OK' python3 tools/hardware_smoke.py
EMBLINK_FIRMWARE=uefi EMBLINK_DISK=nvme EMBLINK_CPUS=2 EMBLINK_NO_HPET=1 \
  EMBLINK_ACPI_TABLE=/tmp/emblink-power.aml \
  EMBLINK_SMOKE_COMMAND='test devices policy' \
  EMBLINK_SMOKE_EXPECT='[cmd] test devices: exit=0 -> OK' python3 tools/hardware_smoke.py
```

Use `EMBLINK_OVMF_DIR` for a nonstandard OVMF location. Native runners use QEMU;
the GUI runners also need GDB. Host tests need a C compiler and zlib development
headers/library for an independent GPT CRC oracle. Runners retain logs and
screenshots and terminate their guests on completion/failure.

The remaining main-OS gates are physical storage/input/Wi-Fi/audio/panel/power
acceptance, broader hardware drivers, native GPU modesetting/acceleration,
suspend/resume, and OS-image update/recovery beyond the initial installer.
See [LAPTOP_ALPHA.md](LAPTOP_ALPHA.md), [HARDWARE_DRIVERS.md](HARDWARE_DRIVERS.md)
and [WIFI.md](WIFI.md). Browser development remains separate.

Validation results: [LAPTOP_VALIDATION.md](LAPTOP_VALIDATION.md).
Screen captures and website assets: [progress gallery](media/laptop-progress/README.md).
