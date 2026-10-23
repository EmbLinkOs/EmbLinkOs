# Laptop progress captures

Forty-six actual EmbLinkOS captures document the original laptop work, its
integration with master through `a281865`, and system-menu polish after PR #40. PNG exports preserve
original pixels and dimensions; no retouching, reconstruction or upscaling.
All are QEMU captures. They do not establish physical laptop compatibility.

Use [gallery.json](gallery.json) for ordering, captions, dimensions, source and
PNG hashes, fixture flags, featured images and historical/current metadata.
Captures 01–20 are historical (`c90c478`, 800 × 600). Captures 21–40 show the
merged OS; desktop captures are 1280 × 800, recovery captures 800 × 600.
The merged-desktop guest (21–38) includes a **synthetic ACPI fixture** throughout;
backlight, battery and lid readings do not come from physical hardware.

## Latest: system-menu polish

Captures 41–46 compare the previous menu with the current native light/dark
palette, Escape dismissal, live preferences and action dispatch. Capture 41 is
the baseline; 42–46 use a fresh QEMU live image without an ACPI fixture.

### Before: mixed menu styling

The integrated desktop before this change: bright rows on the dark desktop. This baseline capture includes the synthetic ACPI fixture.

![Before: mixed menu styling in EmbLinkOS](41-menu-before.png)

### System menu in dark appearance

The bar applies the live dark palette; idle rows share one dense surface with a consistent outline.

![System menu in dark appearance in EmbLinkOS](42-menu-dark.png)

### System menu in light appearance

Changing Appearance through Settings updates the running menu bar without restarting it.

![System menu in light appearance in EmbLinkOS](43-menu-light.png)

### Escape dismisses the dropdown

The menu closes, releases keyboard ownership and returns the bar to 26 pixels. The test reopens it and then uses the dock.

![Escape dismisses the dropdown in EmbLinkOS](44-menu-escape.png)

### Live appearance controls

Actual pointer input switches to Light before the test verifies the running system menu palette.

![Live appearance controls in EmbLinkOS](45-menu-appearance.png)

### Menu action reaches notification service

About EmbLink still posts a native notification after repeated open, Escape, outside-click and appearance changes.

![Menu action reaches notification service in EmbLinkOS](46-menu-action.png)

## Merged desktop milestone

![Desktop after reboot and login](38-merged-persistence.png)

### First account on the merged desktop

Normal UEFI boot reaches first-account setup on a fresh NVMe disk.

![First account on the merged desktop in EmbLinkOS](21-first-account.png)

### Password login

The newly created account reaches the native login screen.

![Password login in EmbLinkOS](22-account-login.png)

### Merged desktop

The incoming graphite wallpaper, desktop and dock are preserved.

![Merged desktop in EmbLinkOS](23-merged-desktop.png)

### Appearance and wallpaper

The current Appearance pane retains theme, wallpaper, scale and accent controls.

![Appearance and wallpaper in EmbLinkOS](24-appearance.png)

### Wi-Fi status

Settings reports that this QEMU machine has no supported Wi-Fi radio. No physical connection is claimed.

![Wi-Fi status in EmbLinkOS](25-merged-wifi.png)

### HDA output

Settings discovers the emulated Intel HDA output; separate recorded-output tests measure real playback.

![HDA output in EmbLinkOS](26-hda-output.png)

### Mute applied

Pointer input enables mute, verified against the kernel audio state.

![Mute applied in EmbLinkOS](27-hda-muted.png)

### Saved output volume

Pointer input changes volume to 49%; the native test verifies kernel state and persistence after reboot.

![Saved output volume in EmbLinkOS](28-hda-volume.png)

### Display information

Settings reports 1280 × 800 VirtIO display and software rendering.

![Display information in EmbLinkOS](29-merged-display.png)

### Brightness round trip

The synthetic ACPI backlight fixture changes from 60% to 40%; kernel state is checked independently.

![Brightness round trip in EmbLinkOS](30-merged-brightness.png)

### Battery and power preferences

The pane uses synthetic ACPI batteries and lid data; mixed units leave total charge unavailable.

![Battery and power preferences in EmbLinkOS](31-merged-power.png)

### Keyboard settings

The incoming keyboard-layout pane remains accessible alongside the added laptop controls.

![Keyboard settings in EmbLinkOS](32-keyboard-layouts.png)

### Native notification banner

About EmbLink posts a real banner through the existing session notification service.

![Native notification banner in EmbLinkOS](33-notification-banner.png)

### Notification history

The same session message appears in the new history window.

![Notification history in EmbLinkOS](34-notification-history.png)

### Locked session

The native desktop owns the opaque password surface while the kernel gates other windows and input.

![Locked session in EmbLinkOS](35-merged-lock.png)

### Wrong password rejected

Authd refuses an incorrect password and the session stays locked.

![Wrong password rejected in EmbLinkOS](36-merged-rejection.png)

### Logout reaches login

Ending the native session stops its descendants and returns to login.

![Logout reaches login in EmbLinkOS](37-merged-logout.png)

### Desktop after a separate boot

The saved account signs in after a separate NVMe boot; volume preferences survive and cold integrity passes.

![Desktop after a separate boot in EmbLinkOS](38-merged-persistence.png)

### Recovery installation

Read-only recovery verifies and expands a disposable NVMe target while preserving the source image hash.

![Recovery installation in EmbLinkOS](39-merged-recovery.png)

### Installed disk boots alone

The installed disk boots without the source media through native NVMe into account setup.

![Installed disk boots alone in EmbLinkOS](40-merged-installed.png)

## Historical milestone (before the master merge)

The following captions describe the earlier implementation and its validation.
Current behavior and test evidence are in [LAPTOP_VALIDATION.md](../../LAPTOP_VALIDATION.md).

## Early bring-up to a persistent desktop

| Early UEFI status | Desktop after reboot and login |
| --- | --- |
| ![Early UEFI battery fixture](01-uefi-battery.png) | ![Persistent authenticated desktop](18-persistent-desktop.png) |

## Hardware bring-up

### Early UEFI battery status

An early firmware-status capture, before the desktop filled in. The 50% reading comes from a synthetic ACPI battery fixture.

![Early UEFI battery status in EmbLinkOS](01-uefi-battery.png)

## Desktop device controls

### Wi-Fi controls reach the desktop

The first integrated Wi-Fi Settings pane reports that QEMU has no supported Wi-Fi adapter. This is not a connected physical radio.

![Wi-Fi controls reach the desktop in EmbLinkOS](02-wifi-controls.png)

### Native sound output

Settings discovers the emulated AC97 stereo sink and starts at full volume. The native audio tests separately measured recorded PCM.

![Native sound output in EmbLinkOS](03-sound-output.png)

### Mute reaches the audio sink

Actual pointer input enables mute; the desktop acceptance test checks the corresponding kernel state.

![Mute reaches the audio sink in EmbLinkOS](04-sound-muted.png)

### Volume changes are applied

The output slider reaches 49% through the native audio control path. Microphone capture is still unavailable.

![Volume changes are applied in EmbLinkOS](05-sound-volume.png)

### Display and brightness controls

Settings reports VirtIO display with software rendering. A synthetic ACPI backlight fixture receives the brightness change from 60% to 40%.

![Display and brightness controls in EmbLinkOS](06-display-brightness.png)

### Power and automatic locking

Idle/lid locking and dimming preferences are wired up. Synthetic mixed battery units deliberately produce an unavailable combined percentage; sleep remains unavailable.

![Power and automatic locking in EmbLinkOS](07-power-preferences.png)

## Authenticated sessions

### First account setup

The native GUI test enters a disposable test account and masked password through real keyboard and pointer input.

![First account setup in EmbLinkOS](08-first-account.png)

### Password login

Normal boot presents the login screen after account creation instead of development autologin.

![Password login in EmbLinkOS](09-login.png)

### Lock screen

The authenticated session is hidden behind its unlock greeter after Ctrl+Alt+L.

![Lock screen in EmbLinkOS](10-locked.png)

### Incorrect passwords keep the session locked

The GUI test submits an incorrect password and verifies that the kernel session gate stays locked.

![Incorrect passwords keep the session locked in EmbLinkOS](11-password-rejected.png)

## Everyday desktop

### Files and Terminal together

Both applications run in the authenticated session. Alt+Tab and Alt+Shift+Tab are checked against the compositor focused-window ID.

![Files and Terminal together in EmbLinkOS](12-app-switching.png)

### Notification center

A native application posts Build / Workspace ready inside the live session. The notification center displays its application source and Clear action.

![Notification center in EmbLinkOS](13-notifications.png)

### Files in the live home

Files is launched from the dock with the live session home and its standard folders, including Trash. Copy/move/Trash/Restore behavior has separate native tests.

![Files in the live home in EmbLinkOS](14-files-home.png)

### Current Wi-Fi Settings

The current desktop continues to report absent radio hardware accurately. Physical AX200 WPA2/DHCP/DNS acceptance is still outstanding.

![Current Wi-Fi Settings in EmbLinkOS](15-wifi-status.png)

## Session persistence

### Logout from the desktop

The GUI regression chooses Log Out while a session child repeatedly writes and synchronizes a 64-KiB file.

![Logout from the desktop in EmbLinkOS](16-logout-menu.png)

### Logout returns to login

The whole former session has stopped, and a cold filesystem integrity check succeeds after the active writer is drained.

![Logout returns to login in EmbLinkOS](17-after-logout.png)

### Desktop after a new boot

A new VM boot uses the saved account, authenticates successfully and starts with an empty notification history.

![Desktop after a new boot in EmbLinkOS](18-persistent-desktop.png)

## Recovery and installation

### Verified installation from recovery

The recovery console completes a verified copy from a padded source image to disposable NVMe storage, expands the root and prints device/mount diagnostics.

![Verified installation from recovery in EmbLinkOS](19-recovery-install.png)

### Installed disk boots alone

The newly installed NVMe target boots without the source disk and presents first-account setup. Both disks are disposable QEMU images.

![Installed disk boots alone in EmbLinkOS](20-installed-first-boot.png)
