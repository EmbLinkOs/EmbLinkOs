# Laptop desktop integration

Settings adds Wi-Fi, Sound, Displays and Battery & Power using the existing EmUI
layout, typography and accent palette. Browser development is separate.

## Desktop and preferences

The top bar shows live Wi-Fi, sound and battery state. Its sound menu changes
volume/mute, and its system menu opens Settings or requests restart/shutdown.
Inert File/Edit/View menus and decorative status chips have been removed.
The desktop receives Settings launch requests over `/run/emlink.settings` and
restores an existing window. It restarts a failed top bar at most once every
five seconds. This is limited desktop supervision, not a general service manager;
init already supervises the desktop session.

Session preferences live at `$HOME/.settings.conf`. Non-session tools without
HOME retain the old `/data/settings.conf` fallback. This fixes preference writes
being outside the normal session's writable namespace. Theme, scale, dock,
volume and mute use atomic replacement with flushes before and after rename.
The desktop restores sound preferences at launch and persists top-bar changes.
Bound sliders/toggles explicitly flush EmUI staging before reading changed values;
this also repairs the existing dock-size slider callback. Per-pane reconciliation
keys keep retained slider geometry out of newly displayed power buttons.

## Sound

The PCM sink selects Intel HDA when it finds a usable analog output, otherwise
the existing AC'97 driver. HDA uses PCI class 04/03, controller reset, bounded
CORB/RIRB verbs, function-group/widget discovery, connection-list traversal,
power/pin/EAPD/amplifier setup, converter stream/format assignment and a 32-page
output BDL. It requires MSI on dedicated vector 0x53, without overwriting a
legacy IRQ handler. DMA pages are allocated only for a controller.

The first usable speaker route is preferred, then headphones, then line out.
There is one output route, no runtime output chooser or automatic jack switching.
HDMI/DisplayPort audio, microphone capture, DSP/SoundWire/SST-only devices,
codec-specific laptop speaker amplifiers and suspend restoration are unsupported.
A PCI class match alone is not a physical laptop compatibility claim.

The sink remains one owner at a time, stereo S16 at 48 kHz. It applies linear
software volume/mute while filling DMA pages; queued samples are not changed
retroactively. A non-sleeping lock serializes ownership, writes and controls.
Repeated open by the owner is idempotent. Last-thread exit releases the speaker
before the parent waits, including a killed or crashed player.

Completion interrupts clear consumed pages. An underrun stops playback; restart
resets the stream and rotates the BDL to the next logical page, avoiding replay
and differences in controller RUN/resume behavior. Failed controller setup
stops DMA and disables PCI bus mastering, retaining the allocated DMA memory.

AC'97 drainage uses its last-valid completion latch rather than comparing circular
indices. The stream is reset between opens. This fixes early completion on a
wrapped ring and skipped pages on a second sound. Partial or above-4-GiB DMA
allocations are rejected instead of advertising a usable controller.

## Displays and power

ACPI video discovery requires `_BCL`, `_BCM` and `_BQC`. It validates, deduplicates
and sorts at most 32 brightness levels. Slider changes choose the nearest level
and queue `_BCM` on the firmware worker. UI snapshots read cached `_BQC` values,
reporting unavailable/unknown values instead of substituting an overlay.
A limited Intel Gen9 firmware-mode handoff and native PWM fallback are described
in [DAILY_USE.md](DAILY_USE.md). General native modesetting, multi-monitor support
and native 3D acceleration remain open.

Battery & Power shows cached charge/charging state and shutdown/restart controls.
Low/critical battery state remains visible in the top bar. ACPI lid state is
sampled with `_LID`; closing it does not initiate sleep. Idle dim/lock, lid lock
and Intel HWP preferences are described in [DAILY_USE.md](DAILY_USE.md).
Suspend/resume remains open. [LAPTOP_ALPHA.md](LAPTOP_ALPHA.md) describes
flush-before-power-transition behavior and limits.

## ABI and checks

Shared versioned structs: `user/lib/embk_{wifi,audio,display,power}.h`.
Syscalls 99/100 are Wi-Fi info/request; 101/102 display info/brightness;
103/104 audio info/volume. Wi-Fi changes require `network_admin` (ID 12),
brightness requires `power`, and sound requires `audio`. UI snapshot reads never
execute AML, wait for association or write profile files. Wi-Fi has one queued
request and one latest completion record; a client refreshes state if a later
request supersedes its own completion. Saving a network does not claim WPA2
success or a DHCP lease. [WIFI.md](WIFI.md) describes profiles and radio limits.

```sh
make images uefi-usb.img
TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' tests/drivers/run.sh
iasl -p /tmp/emblink-power tests/acpi/power.asl

# Disposable images; use the hardware smoke tool's normal environment setup.
EMBLINK_CPUS=2 EMBLINK_DISK=nvme EMBLINK_AUDIO=hda \
  EMBLINK_SMOKE_COMMAND='test devices audio' \
  EMBLINK_SMOKE_EXPECT='[cmd] test devices: exit=0 -> OK' python3 tools/hardware_smoke.py
# Repeat with EMBLINK_AUDIO=ac97 for the existing backend.
EMBLINK_SMOKE_COMMAND='test devices preferences' \
  EMBLINK_SMOKE_EXPECT='[cmd] test devices: exit=0 -> OK' python3 tools/hardware_smoke.py
EMBLINK_ACPI_TABLE=/tmp/emblink-power.aml \
  EMBLINK_SMOKE_COMMAND='test devices fixture' \
  EMBLINK_SMOKE_EXPECT='[cmd] test devices: exit=0 -> OK' python3 tools/hardware_smoke.py
```

Audio acceptance produces `audio.wav` in the printed log directory. Measure it
with `tools/audio_check.py <path> --hz 440 --min-seconds 3.4`; serial success alone
is insufficient. The ACPI fixture checks duplicate/unsorted brightness levels,
a `_BCM`/`_BQC` round trip and lid information; it is not a physical panel.
The preferences check confines a child to its temporary home and read-only
application/system trees, checking initial save and atomic replacement without
replacing existing user preferences.

Physical AX200, laptop HDA routing, panel brightness, EC/battery and I2C touchpad
acceptance remain outstanding. [DAILY_USE.md](DAILY_USE.md) covers the initial
recovery installer, authenticated default boot, session lock/logout, file
operations and power policies added subsequently. Broader drivers, suspend/resume,
OS-image updates/rollback and general service supervision remain open; package
signing/update/rollback already cover applications.

For a real UI interaction check (QEMU plus GDB on PATH):

```sh
EMBLINK_ACPI_TABLE=/tmp/emblink-power.aml python3 tools/desktop_smoke.py
```

It opens Settings through the top bar and desktop IPC, clicks mute/unmute and
volume, and checks the kernel sink state. It checks a firmware brightness change and captures Displays and Power.
Use `tools/app_shot.py --attach-audio` to attach a controller for a Settings
screenshot without asserting that the app plays a tone; `--audio` retains its
existing playback assertion.
