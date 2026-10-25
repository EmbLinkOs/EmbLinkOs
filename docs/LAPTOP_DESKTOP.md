# Laptop desktop integration

Settings adds Wi-Fi, Sound, Displays and Battery & Power using the existing EmUI
layout, typography and accent palette. Browser development is separate.

## Desktop and preferences

The incoming desktop, keyboard layouts, time-zone controls and wallpapers are
preserved. Its status menus open Settings through the desktop service; the
EmbLink menu opens a separate Notifications history window. Native notification
banners still use `/run/emlink.notify`.

Session preferences live at `$HOME/settings.conf`, with `/data/settings.conf`
as the non-session fallback. Theme, scale, dock, keyboard, time-zone, sound and
power preferences share this store. Saving writes a unique exclusive staging
file, flushes it and atomically replaces the destination. A failed write leaves
the previous preferences intact. Home reapplies sound and policy changes.

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
retroactively. A non-sleeping lock serializes stream ownership and writes; volume/mute are atomic.
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
Incoming syscall numbers 1–128 are unchanged. Laptop calls are appended:
129/130 Wi-Fi info/request, 131 power info, 132/133 display info/brightness,
134/135 audio info/control, 136 GPU info, 137 no-replace rename, 138 boot mode,
139 session control, 140/141 notification history, 142/143 policy.
Wi-Fi changes require `network_admin` (ID 14),
brightness requires `power`, and sound requires `audio`. UI snapshot reads never
execute AML, wait for association or write profile files. Wi-Fi has one queued
request and one latest completion record; a client refreshes state if a later
request supersedes its own completion. Saving a network does not claim WPA2
success or a DHCP lease. [WIFI.md](WIFI.md) describes profiles and radio limits.

```sh
make FB_W=1280 FB_H=800 images uefi-usb.img
TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' tests/drivers/run.sh
iasl -p /tmp/emblink-power tests/acpi/power.asl

# Disposable images; use the hardware smoke tool's normal environment setup.
EMBLINK_CPUS=2 EMBLINK_DISK=nvme EMBLINK_AUDIO=hda \
  EMBLINK_SMOKE_COMMAND='test devices audio' \
  EMBLINK_SMOKE_EXPECT='[cmd] test devices audio: exit=0 -> OK' python3 tools/hardware_smoke.py
# Repeat with EMBLINK_AUDIO=ac97 for the existing backend.
EMBLINK_SMOKE_COMMAND='test devices preferences' \
  EMBLINK_SMOKE_EXPECT='[cmd] test devices preferences: exit=0 -> OK' python3 tools/hardware_smoke.py
EMBLINK_ACPI_TABLE=/tmp/emblink-power.aml \
  EMBLINK_SMOKE_COMMAND='test devices fixture' \
  EMBLINK_SMOKE_EXPECT='[cmd] test devices fixture: exit=0 -> OK' python3 tools/hardware_smoke.py
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
Managed UEFI system updates now stage an inactive OS slot, verify disk readback,
confirm a healthy desktop boot and retain a local rollback system. Accounts and
files live on shared Data. Settings uses a supervised signed-release worker;
independent applications use atomic version publication and retained bundles.
Production release hosting, legacy migration and broader service supervision
remain open. See [UPDATES.md](UPDATES.md) for scope and validation.

For a real UI interaction check (QEMU plus GDB on PATH):

```sh
EMBLINK_ACPI_TABLE=/tmp/emblink-power.aml python3 tools/desktop_smoke.py
```

It opens Settings through the top bar and desktop IPC, clicks mute/unmute and
volume, and checks the kernel sink state. It checks a firmware brightness change and captures Displays and Power.
Use `tools/app_shot.py --attach-audio` to attach a controller for a Settings
screenshot without asserting that the app plays a tone; `--audio` retains its
existing playback assertion.
