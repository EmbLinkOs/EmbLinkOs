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

## 4. Using EmbLinkOS

You should see a few lines of bootloader text → kernel start-up messages → a
small **graphical desktop** (a launcher and a live clock). That is the OS running
on real hardware.

- Use the **USB keyboard** to interact.
- **No networking** — there is no driver for real network cards yet (only the
  virtual `virtio-net` used under QEMU), so no Wi-Fi/internet. Everything else
  runs locally.
- **To leave:** reboot or power off and remove the stick. The normal system
  returns untouched.

---

## 5. Troubleshooting

| Symptom | Fix |
|---|---|
| Stick not in the boot menu | Confirm Secure Boot is **Disabled** and you **saved**; try another USB port; ensure UEFI boot is enabled (not Legacy-only). |
| "Secure Boot Violation" / "failed to verify" | Secure Boot is still on — go back to §2 and disable it. |
| Black screen after selecting it | The display adapter may be unsupported; note the machine model and report it. |
| Boots, then an error / a page of registers | **This is the useful case.** Photograph the whole screen — the dump says exactly where it stopped. |

> **The single most useful thing:** if it doesn't reach the desktop, a clear
> **photo of the screen** (any text, error, or register dump) is the best clue
> for fixing it.

### Hardware reality on bare metal

| Subsystem | On real hardware |
|---|---|
| Display | Works via the firmware's UEFI GOP framebuffer (no GPU driver needed). |
| Storage | SATA/AHCI + IDE only — **no NVMe driver**. (Irrelevant here: the OS boots off the USB and doesn't install.) |
| Keyboard | USB HID + PS/2. A laptop's built-in keyboard may be I2C-HID (unsupported) — use a USB keyboard. |
| Networking | None (real NIC drivers not written yet). |

---

## Writing the stick yourself

Only needed if you must create the stick from `uefi-usb.img` rather than
receiving one ready-made. Done on a Linux machine.

> ⚠️ **`dd` erases the entire target device and does not ask twice.** Point it at
> the wrong disk and you wipe it. Identify the USB device carefully first.

1. **Find the USB device** — transport `usb`, size matching your stick:

   ```bash
   lsblk -o NAME,SIZE,TRAN,MODEL
   ```

2. **Write the image** — replace `sdX` with that device (e.g. `sdb`), and
   double-check before pressing Enter:

   ```bash
   sudo umount /dev/sdX*        # ignore "not mounted"
   sudo dd if=uefi-usb.img of=/dev/sdX bs=4M status=progress conv=fsync
   sync
   ```

To rebuild `uefi-usb.img` from source: `make uefi-usb.img` (see the Makefile
target; it packs the current kernel + EMBKFS via `tools/mkuefidisk.sh`).

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
