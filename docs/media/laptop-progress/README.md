# Laptop progress captures

These are real EmbLinkOS screens captured from QEMU during development and native
acceptance runs. PNG exports preserve the original pixels and 800 x 600 dimensions;
there is no retouching, reconstruction or upscaling. The dates follow the guest UTC
clock. Synthetic ACPI readings are identified in the captions and manifest.

The captures show software progress, not physical laptop compatibility. Wi-Fi RF,
physical device behavior, general GPU modesetting/acceleration and sleep/wake remain
open; see [DAILY_USE.md](../../DAILY_USE.md).

For the website, use the PNGs directly and read [gallery.json](gallery.json) for
ordered phases, captions, alt text, dimensions, source names and SHA-256 hashes.
`featured` marks useful lead images. Keep the fixture/compatibility context when
reusing the associated captures. Only the screenshot assets belong in a website
build; no boot images or firmware are needed.

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
