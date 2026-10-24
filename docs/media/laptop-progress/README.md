# Laptop progress captures

111 actual EmbLinkOS captures document the original laptop work, its
integration with master through `a281865`, system-menu polish after PR #40,
recoverable document workspaces, the Contour desktop redesign, keyboard
interaction, private terminal sessions, VT screen emulation and process compatibility. PNG exports preserve
original pixels and dimensions; no retouching, reconstruction or upscaling.
All are QEMU captures. They do not establish physical laptop compatibility.

Use [gallery.json](gallery.json) for ordering, captions, dimensions, source and
PNG hashes, fixture flags, featured images and historical/current metadata.
Captures 01–20 are historical (`c90c478`, 800 × 600). Captures 21–40 show the
merged OS; desktop captures are 1280 × 800, recovery captures 800 × 600.
The merged-desktop guest (21–38) includes a **synthetic ACPI fixture** throughout;
backlight, battery and lid readings do not come from physical hardware.

## Latest: process compatibility

The native process witness passes 203 checks inside Terminal, retaining 32 child statuses and transforming 66,049 bytes through two child processes. It verifies EOF, nonblocking writes, actual PIDs, copyout retry and competing waiters; a following shell command succeeds. This is the spawn-based compatibility foundation, not a ported Unix shell.

This original-pixel QMP capture uses a disposable UEFI/NVMe live session without a synthetic ACPI fixture.

![Pipes and child waits in the native terminal](111-process-pipeline-waits.png)

## Latest: full-screen terminal

Seven real QMP captures show the native screen witness, cross-application clipboard and restored shell. They use a disposable UEFI/NVMe session without a synthetic ACPI fixture. The witness exercises the emulator; it does not establish Vim, zsh or xterm compatibility.

### A screen with independent regions

The native raw-mode terminal witness keeps its heading and footer fixed while twelve log entries scroll inside its margins. Application-cursor Down selects the second row; colours and Unicode cells are rendered by the actual emulator.

![A screen with independent regions](104-terminal-screen-region.png)

### Cursor position, round trip

The native application positions its cursor, requests a report and verifies the terminal reply through its own standard input. This is a screen-emulation witness, not an upstream application compatibility claim.

![Cursor position, round trip](105-terminal-cursor-reply.png)

### One screen, a larger workspace

The real window control expands Terminal. The application queries updated PTY dimensions and redraws its screen at the new size; restoring the window is verified in the same run.

![One screen, a larger workspace](106-terminal-live-resize.png)

### Multiline paste reaches the program

Text copied from Editor reaches the running raw-mode application inside bracketed-paste delimiters. The program verifies 22 bytes and one newline without treating that newline as an ordinary command.

![Multiline paste reaches the program](107-terminal-bracketed-paste.png)

### Screen text back in Editor

Copy screen exports plain UTF-8 text without terminal escape sequences. Editor receives the complete multiline grid through a single paste command, replaces its selection and clears the retained highlight.

![Screen text back in Editor](108-terminal-screen-editor.png)

### Return to the native shell

Leaving the alternate screen restores the previous native command and its result. The witness confirms its exit; alternate-screen content does not enter the primary output.

![Return to the native shell](109-terminal-primary-restored.png)

### A large clipboard in the code editor

The guest doubles text using Editor selection/copy/paste commands, then copies more than 8 KiB into Note++. Its separate application paste hook receives the full multiline text without indexing beyond its clipboard buffer.

![A large clipboard in the code editor](110-terminal-long-editor-paste.png)

## Latest: interactive terminal sessions

Four real QMP captures accompany the terminal-device milestone. They come from a disposable UEFI/NVMe live session with no synthetic ACPI fixture. They show the native shell and terminal witness, not a ported Unix shell or a full VT screen emulator.

### The native prompt, preserved

Terminal runs its native shell on a private pseudo-terminal while retaining local caret editing and command history. The guest verifies edited commands and their separate result lines.

![The native prompt, preserved](100-terminal-prompt.png)

### A program takes keyboard input

The child reports measured terminal dimensions, receives an edited canonical line and switches to raw input. It is waiting for one key without Enter.

![A program takes keyboard input](101-terminal-raw-input.png)

### One key, delivered immediately

The native program receives z as byte 0x7a without Enter, then restores canonical input. Ctrl+D returns control to the shell; the same guest also verifies Ctrl-letter bytes and ANSI arrow input.

![One key, delivered immediately](102-terminal-direct-input.png)

### Interrupt a command and keep working

Ctrl+C stops a foreground child and interrupts a native shell loop. Subsequent commands produce their expected results in the same Terminal session.

![Interrupt a command and keep working](103-terminal-interrupt.png)

## Latest: keyboard interaction

Four real guest captures accompany keyboard acceptance. They use a disposable UEFI/NVMe live session, with no synthetic ACPI fixture.

### Keyboard focus stays in dialogs

Reset appearance opens with Cancel focused. The guest verifies forward traversal, keyboard cancellation and restoration to the initiating control.

![Keyboard focus stays in dialogs](96-keyboard-dialog.png)

### Reverse traversal, the same boundary

Shift+Tab wraps to Reset inside the confirmation; Tab returns to Cancel without reaching controls behind the dialog.

![Reverse traversal, the same boundary](97-reverse-dialog.png)

### Menus work from the keyboard

Right reaches the adjacent workspace menu, Down selects its second item, Home returns to its first, and Escape releases menu ownership.

![Menus work from the keyboard](98-keyboard-menu.png)

### A safe choice for unsaved work

The dirty Editor guard focuses Cancel, contains keyboard traversal and blocks background Save. Cancelling preserves the exact text and restores typing focus.

![A safe choice for unsaved work](99-document-dialog.png)

## Contour desktop

These are actual UEFI/NVMe guest captures of the redesign, including customization, applications and authenticated document workflows. They contain no synthetic ACPI fixture.

### A calmer desktop

Contour introduces mineral surfaces, compact original icons and a readable command strip.

![A calmer desktop](62-contour-desktop.png)

### The application library

A bounded, searchable library launches installed native applications through keyboard or pointer input.

![The application library](63-contour-library.png)

### One shared appearance

Settings coordinates semantic colours, typography, control density and live application appearance.

![One shared appearance](64-contour-appearance.png)

### Paper after a cold boot

The saved Paper preset returns after an independent UEFI boot and account authentication.

![Paper after a cold boot](65-contour-paper.png)

### Workspaces preserve running work

Editor moves to another workspace while preserving its text; hidden windows are separately checked for input isolation.

![Workspaces preserve running work](66-contour-spaces.png)

### Window and workspace preferences

Shared decorations, shadows, corner geometry and workspace behavior are configurable in Settings.

![Window and workspace preferences](67-contour-windows.png)

### Readable and reduced effects

Text size, contrast, cursor appearance and reduced-motion preferences share the same configuration system.

![Readable and reduced effects](68-contour-accessibility.png)

### Terminal with native chrome

Terminal keeps a monospace transcript while its surrounding controls follow the selected interface family.

![Terminal with native chrome](69-contour-terminal.png)

### Files follows the desktop

Files uses semantic selection, a shared sidebar surface and compact native window controls.

![Files follows the desktop](70-contour-files.png)

### Editor in Contour

Editor inherits shared typography and chrome while retaining safe saving and recovery.

![Editor in Contour](71-contour-editor.png)

### Photos in Contour

The photo application uses the shared palette, window controls and preferred corner geometry.

![Photos in Contour](72-contour-photos.png)

### An honest empty music library

Music explains its empty state and retains the common desktop hierarchy.

![An honest empty music library](73-contour-music.png)

### Measured system activity

Activity Monitor presents actual process measurements within the shared native interface.

![Measured system activity](74-contour-monitor.png)

### Notification history

The history window has an original icon and a readable empty state.

![Notification history](75-contour-notifications.png)

### A dock on the left

A reserved side band keeps application windows clear of the configurable dock.

![A dock on the left](76-contour-left-dock.png)

### A dock on the right

The same dock supports the opposite screen edge with shared placement rules.

![A dock on the right](77-contour-right-dock.png)

### The command strip moves

The command strip can occupy either horizontal screen edge.

![The command strip moves](78-contour-bottom-strip.png)

### Menus open toward the workspace

At the bottom edge, the system menu opens upward and retains its dismissal surface.

![Menus open toward the workspace](79-contour-bottom-menu.png)

### A different typographic character

A real serif regular/bold family updates Settings and the command strip.

![A different typographic character](80-contour-serif.png)

### Scale and density are independent

The acceptance run selects 115 percent interface scale and compact spacing through the actual controls.

![Scale and density are independent](81-contour-density.png)

### Reset remains a deliberate choice

Reset appearance asks for confirmation; acceptance checks cancellation and reset separately, including a scaled serif explanation.

![Reset remains a deliberate choice](82-contour-reset.png)

### A coherent sign-in surface

The account sign-in screen uses the shared default visual language and retains the authentication service.

![A coherent sign-in surface](83-contour-login.png)

### The lock follows the session

The themed lock acquires screen ownership; workspace shortcuts are checked while the session is locked.

![The lock follows the session](84-contour-lock.png)

### The shared Save panel

The session file service uses Contour chrome, user places and the same shared controls as its callers.

![The shared Save panel](85-contour-picker.png)

### A real multi-document workspace

Note++ keeps a monospace document canvas and shared native chrome; both dirty tabs survive a forced stop.

![A real multi-document workspace](86-contour-note-workspace.png)

### Recovery retains the new interface

An independent boot restores both Note++ drafts, with exact document contents checked by the acceptance run.

![Recovery retains the new interface](87-contour-recovered.png)

### Original monochrome silhouettes

The shell uses matching line silhouettes, preserving distinct application glyphs in a single ink.

![Original monochrome silhouettes](88-contour-monochrome.png)

### One icon family throughout the library

The application library follows the same monochrome icon preference as the dock and desktop.

![One icon family throughout the library](89-contour-monochrome-library.png)

### A coordinated custom palette

A custom light surface and rust accent are entered through Settings, persisted and applied live.

![A coordinated custom palette](90-contour-custom-palette.png)

### Power status follows the provider

The emulated guest reports no battery; the bar avoids suggesting a permanently full battery.

![Power status follows the provider](91-contour-power-status.png)

### A compact, readable window

Wrapped descriptions and previews remain readable at the actual 640 by 420 minimum window size.

![A compact, readable window](92-contour-compact-window.png)

### Fill the available workspace

Window fill respects the shared command-strip and dock insets.

![Fill the available workspace](93-contour-fill.png)

### Restore the previous geometry

The window control restores the position and dimensions recorded before filling.

![Restore the previous geometry](94-contour-restored.png)

### Tile with a deliberate layout board

The native hover board places Settings in the right half while preserving its usable minimum size.

![Tile with a deliberate layout board](95-contour-tiled.png)

## Document workspaces

Captures 47–61 follow real file creation, saving, unsaved-close protection, forced-stop recovery and reopening after independent boots. They use authenticated QEMU accounts and disposable NVMe storage, with no synthetic ACPI fixture.

### A shared Save dialog

Both editors use the session file service. The picker shows the actual authenticated home and user places.

![A shared Save dialog](47-shared-save.png)

### A durable saved document

Editor reports the completed save in Documents after synced temporary-file replacement.

![A durable saved document](48-document-saved.png)

### Confirming file replacement

Save As asks before replacing the existing Milestone.txt; cancelling keeps the original path and contents.

![Confirming file replacement](49-replace-confirmation.png)

### Editor protects unsaved changes

GUI+W opens Save, Discard and Cancel. The window remains alive beyond the former compositor close deadline.

![Editor protects unsaved changes](50-editor-unsaved-close.png)

### Quitting a multi-document workspace

Note++ checks every dirty tab. The acceptance test also cancels a partially answered quit and verifies dirty state is retained.

![Quitting a multi-document workspace](51-workspace-unsaved-close.png)

### Two dirty tabs before a forced stop

Both modified tabs remain open after cancelling quit. The VM is then terminated without logout or shutdown.

![Two dirty tabs before a forced stop](52-workspace-before-crash.png)

### Editor recovers after a cold boot

The original saved document and its unsaved addition return from a separately checkpointed draft on NVMe.

![Editor recovers after a cold boot](53-editor-recovered.png)

### Both Note++ drafts recovered

A new boot restores both unsaved tabs, including the second draft that directory mutation could previously skip.

![Both Note++ drafts recovered](54-workspace-recovered.png)

### Saving the recovered workspace

A recovered document is saved through the common picker. Its path and cursor are recorded for session reopening.

![Saving the recovered workspace](55-workspace-saved.png)

### Saved work reopens after another reboot

A third independent boot authenticates the user and reopens the saved Note++ document with its exact contents.

![Saved work reopens after another reboot](56-workspace-after-reboot.png)

### Browsing persisted documents

The shared Open panel lists both saved documents in the authenticated Documents folder. File rows now receive selection clicks.

![Browsing persisted documents](57-shared-open.png)

### The original document reopens from the picker

Editor loads Milestone.txt through the shared Open panel after the final cold boot; exact bytes are checked.

![The original document reopens from the picker](58-document-after-reboot.png)

### Creating a document in Files

The authenticated Files window offers New Document in the current folder.

![Creating a document in Files](59-files-new-document.png)

### Files hands the document to Editor

Files creates FromFiles.txt, supplies HOME and EDIT_FILE to Editor, and the edited document is saved in the same home.

![Files hands the document to Editor](60-files-created-and-saved.png)

### Duplicate document names are refused

Creating FromFiles.txt again reports that the name already exists and does not open a replacement editor.

![Duplicate document names are refused](61-files-name-conflict.png)

## System-menu milestone

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
