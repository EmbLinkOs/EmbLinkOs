# System-menu polish

The system bar now follows live appearance/accent/scale preferences using a
coherent worker-to-render snapshot. Dropdowns share one legible material,
outline and shadow rather than separate filled cards for every row. They open
beneath the label at the same position regardless of where it was clicked.

Escape dismisses an open menu-bar dropdown. A transparent surface catches
outside clicks across the display, including below the old 340-pixel limit;
it exists only while a menu is open. The bar then returns to its original
26-pixel height, so the dock and applications receive input again.

This required backend fixes. Translucent chrome previously never received
keyboard input, and the compositor's shared resize minimum rejected the bar's
thin height. `win_menu` (syscall 144) now lends a caller-owned translucent window
the keyboard temporarily. It requires GPU authority, checks ID/boolean bounds,
and cannot override the session lock owner. Destroy/reap releases ownership.
Only translucent bars receive the smaller resize minimum; normal app limits
are preserved. Existing syscall IDs 1–143 are unchanged.

Build and run the relevant checks:

```sh
make -j4 FB_W=1280 FB_H=800 images uefi-usb.img
make em-test kit-test
python3 tools/check_native_abi.py
python3 tools/menu_smoke.py
python3 tools/integration_smoke.py
iasl -p /tmp/emblink-power tests/acpi/power.asl
EMBLINK_ACPI_TABLE=/tmp/emblink-power.aml python3 tools/desktop_smoke.py
```

Use the configured OVMF/QEMU/GDB paths (`EMBLINK_OVMF_DIR` where necessary).
Run GUI tests sequentially. The menu test reads actual pointer coordinates
through GDB before clicking, avoiding dropped relative packets under software
rendering. It checks native pixels, stable anchoring, Escape without a bar
restart, below-menu outside clicks, live light/dark switching and action dispatch.
Devicecheck additionally rejects another process's window ID, absent GPU
capability, invalid arguments and a destroyed window; it exercises a thin
resize round trip. The desktop test covers authenticated lock/unlock and input
return after using menus, with account/preferences across reboot.

This is one desktop usability milestone. Arrow-key navigation, type-to-select,
keyboard menu activation, accessibility roles and context-menu Escape handling
remain open. It does not establish macOS-level reliability or physical laptop
compatibility. Browser development remains separate.

## Validation record (2026-10-23)

The x86 BIOS/userspace and GPT UEFI images build. Existing compiler warnings
remain. `em-test` and `kit-test` pass with zero failures. The ABI check reports
144 unique matching syscalls and 14 capability IDs. The ARM64 freestanding
kernel also cross-links; this is not ARM runtime validation.

The native menu runner reports:

```text
PASS: dark/light menus, stable label anchor, Escape without bar exit, outside dismissal and native action dispatch
```

The two-CPU UEFI/NVMe batch passes device ownership/capability/bounds, thin bar
resizing, preferences, file operations, IPC cancellation/resource counts,
sessions, kill during I/O and the full POSIX suite. The native GUI runner checks
account creation/login, device controls, notification history rendering,
wrong/correct lock passwords, logout and persistence across a separate boot.
Captures 41–46 in the [progress gallery](media/laptop-progress/README.md) are
original-size before/after exports with hashes and fixture context.
