# Booting EmbLinkOS from a USB stick

EmbLinkOS is a complete, from-scratch operating system — its own bootloader,
kernel, filesystem (EMBKFS), and graphical desktop (EmUI). This guide covers
running it **live on real hardware** from a single USB stick.

> **It is completely non-destructive.** The OS runs entirely from the stick. It
> installs nothing and **never writes to the computer's internal drive**. Remove
> the stick and reboot, and the machine is exactly as it was.

The stick is a single-device GPT image (`uefi-usb.img`): an EFI System Partition
holding the loader (with the kernel embedded) at
`/EFI/BOOT/BOOTX64.EFI`, plus an EMBKFS partition the kernel mounts as root.

---

## 1. What you need

- **The prepared EmbLinkOS USB stick** (see *Writing the stick* below if you must
  create it yourself).
- **A PC/laptop where you can change firmware (BIOS/UEFI) settings** — i.e. no
  unknown setup password. Secure Boot must be turn-off-able.
- **A wired USB keyboard, plugged in _before_ power-on.** EmbLinkOS may not drive
  every built-in laptop keyboard, and firmware menus want a keyboard present at
  start-up (they enumerate USB keyboards once, at POST — hot-plugging later is
  often ignored).
- **A USB mouse.** A laptop's touchpad will most likely not work yet.

---

## 2. Disable Secure Boot

EmbLinkOS's bootloader is **not signed by Microsoft**, so Secure Boot must be
**Disabled** or the firmware refuses to launch it.

1. Plug in the stick, restart, and **enter firmware setup** — tap the setup key
   repeatedly as the machine powers on (table below).
2. Find **Secure Boot** (usually under a *Security* or *Boot* tab) → set it to
   **Disabled**.
3. Leave the boot mode as **UEFI** — do **not** enable Legacy / CSM for this stick.
4. **Save & Exit** (usually `F10`).

| Brand | Enter setup |
|---|---|
| HP | `F10` (or `Esc` then `F10`) |
| Dell | `F2` |
| Lenovo | `F1` or `F2` (ThinkPad: `Enter` then `F1`) |
| ASUS | `Del` or `F2` |
| Acer | `F2` or `Del` |
| MSI / Gigabyte / ASRock | `Del` |
| Most desktops | `Del` · laptops usually `F2` |

---

## 3. Boot from the stick

After saving, the machine restarts. Open the **one-time boot menu** and pick the
stick.

1. As it restarts, tap the **boot-menu key** (table below).
2. Choose the USB stick — often listed as **"UEFI: &lt;stick name&gt;"**. If two
   entries appear, pick the **UEFI** one.

| Brand | Boot menu |
|---|---|
| HP | `F9` |
| Dell | `F12` |
| Lenovo | `F12` |
| ASUS | `Esc` or `F8` |
| Acer | `F12` |
| MSI | `F11` |
| Gigabyte / ASRock | `F12` |
| Other | `F12` · `F11` · `F8` · `Esc` |

---

## 4. What you will see

1. A plain-text **EmbBoot** menu. It boots on its own after 5 seconds.
2. **Kernel messages in white on dark blue-black**, scrolling from the top of
   the screen. These appear from the very first moments of the kernel, so if
   the machine stops somewhere, the last line on the screen says where.
3. The **desktop**: a menu bar, a dock at the bottom, and the wallpaper. The
   session is a development auto-login (user `yves`, no password).

- **Use a USB mouse.** Most laptop touchpads are I²C-HID, which has no driver
  yet. The built-in keyboard usually works (it is PS/2 on most laptops); a USB
  keyboard always does.
- **On a high-resolution panel everything will look small** — the desktop has
  no HiDPI scaling yet.
- **The clock may be off by some hours** if the laptop also runs Windows,
  which keeps the hardware clock in local time. With a network, NTP corrects it.
- **To leave:** EmbLink menu (top left) → Shut Down or Restart, then remove the
  stick.

---

## 5. If something goes wrong

| What you see | What it means / what to do |
|---|---|
| Stick not in the boot menu | Secure Boot still on, or not saved; try another USB port; make sure UEFI boot is enabled. |
| "Secure Boot Violation" | Secure Boot is still on — §2. |
| The menu, then `EmbBoot FATAL: ...` | The loader stopped before the kernel. **Photograph it.** |
| Kernel messages that stop and never move again | A hang. **Photograph the screen** — the last lines say which driver it was in. |
| **A red bar reading "EmbLinkOS stopped: ..."** with numbers and log lines | A kernel fault. **Photograph the whole screen.** It names the function and source line that faulted. |
| The desktop, but the mouse or keyboard does nothing | Try a USB mouse/keyboard in a different port. Note the laptop model. |
| Black screen right after choosing Boot | The firmware gave no usable framebuffer. Note the model. |

> **The single most useful thing is a clear photo of the screen**, plus the
> laptop's exact model. If the machine does reach the desktop, that is worth a
> photo too.

### Hardware reality on a laptop

| Subsystem | On real hardware |
|---|---|
| Display | The firmware's UEFI framebuffer, at the resolution the firmware chose. No GPU driver. |
| Storage | Boots and runs entirely from the stick. NVMe and SATA drivers exist, and **the internal drive is only read, never written** — checked by `make test-laptop`, which hashes an emulated internal disk before and after a boot. |
| Keyboard / mouse | PS/2 and USB HID. I²C-HID touchpads: not yet. |
| Network | No Wi-Fi. Built-in Ethernet: probably not (only some Intel and older Realtek chips have drivers). **USB tethering from an Android phone works** (RNDIS) — plug in the phone and turn on USB tethering. |
| Timers | Does not need an HPET or the 8254 PIT, which recent laptops often lack or clock-gate: the processor's clock is measured against CPUID or the ACPI PM timer, and every wait on the PIT is bounded. Tested in emulation with both absent — not yet on silicon. |
| Sound | Intel HDA exists; whether the laptop's codec produces sound is unknown. |
| Power | Shut Down and Restart work through ACPI. No sleep, no battery indicator yet. |

**What the OS writes:** its own files on the stick. If the kernel faults, it may
also leave one small crash record (a few kilobytes) through the firmware's ACPI
error-record store, if the laptop has one — the same mechanism other operating
systems use for crash reports. It never writes the internal drive. **Do not run
the `install` tool on a laptop you care about**: installing is the one thing
that erases a disk.

---

## Writing the stick

> ⚠️ **Writing the image erases the entire target device.** Point it at the
> wrong disk and you wipe that disk instead. Identify the stick carefully.

The image is `uefi-usb.img` in the repository (build it with `make uefi-usb.img`).
The stick must be at least 256 MB; everything on it is erased.

### On macOS

1. Plug the stick in and **find it** — look for the size that matches your stick:

   ```bash
   diskutil list external physical
   ```

   It is shown as `/dev/diskN` (for example `/dev/disk4`). **Your Mac's own
   disks are `disk0`/`disk1`/`disk3`-style internal entries and are not listed
   under `external physical`.**

2. **Unmount it and write** — replace `N` with that number. Note `rdisk`, which
   is much faster than `disk`:

   ```bash
   diskutil unmountDisk /dev/diskN
   sudo dd if=uefi-usb.img of=/dev/rdiskN bs=4m
   sync
   diskutil eject /dev/diskN
   ```

3. If macOS says **"The disk you inserted was not readable by this computer"**,
   click **Ignore** — never *Initialize*. The second partition is EmbLinkOS's
   own filesystem, which macOS does not know.

### On Linux

```bash
lsblk -o NAME,SIZE,TRAN,MODEL           # transport "usb", size matching the stick
sudo umount /dev/sdX*                   # ignore "not mounted"
sudo dd if=uefi-usb.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

---

## Note on Secure-Boot-locked machines

If Secure Boot **cannot** be disabled (e.g. a firmware/BIOS password you don't
have), an unsigned OS cannot be booted directly — that is Secure Boot working as
designed. The only route is `shim` + a Machine Owner Key enrolled through
**MokManager**, which requires a working keyboard at the MokManager screen (USB
keyboards must be present at power-on; some firmware only feeds the built-in
keyboard there). If neither the internal keyboard nor a USB keyboard can navigate
MokManager, that machine can't run an unsigned OS until the BIOS password is
cleared. Use a machine where Secure Boot is turn-off-able instead.
