# Graphics and displays

EmbLinkOS currently has one active framebuffer output and a CPU compositor.
Settings → Displays reports the adapter, rendering backend, current resolution
and validated monitor identity when EDID is available. Resolution and interface
scale are separate: scale remains in Appearance.

## Live resolution changes

VirtIO-GPU 2D and Bochs/QEMU DISPI support live modesetting. Choose a resolution
and select **Apply Resolution**. The new framebuffer is prepared before scanout
changes, and the previous surface remains allocated until confirmation. **Keep
Changes** commits the mode to the user's atomic settings file. **Revert**, Escape,
a 30-second timeout, closing the requester, locking the session or ending the
session restores the previous surface. A request cannot confirm another
process's trial or reuse a stale generation. Mutations require both GPU and power
capabilities, an unlocked session and an already-presented desktop/top bar.

The desktop, top bar and shared EmUI runtime observe display-size changes. They
recreate pixel mappings and renderer targets, reflow normal/maximized/tiled
windows and preserve application documents. Automatic placement preserves
z-order and keyboard focus, keeping the confirmation dialog in front; the old
window pixels remain visible until the replacement frame is drawn. Pointer bounds and pending drag/click
state are reset at the compositor's mode boundary. Confirmed dimensions are
restored after the next authenticated desktop starts; restoration is confirmed
only after the desktop presents a frame at that size. Unsupported saved sizes
leave the startup mode intact. Appearance presets preserve the display choice.

VirtIO uses distinct resource IDs and releases the previous resource only after
the host acknowledges the new selection or the trial is reverted. Backing command
construction no longer shares the device's in-flight command scratch space.
Transport timeout poisons further submissions; unacknowledged device-owned
buffers are retained until reboot rather than freed for unrelated allocations.
DISPI validates register readback and attempts to restore the old programming
if the requested mode is rejected. Framebuffer publication and drawing use the
same compositor lock. Rendering remains software for both adapters.

## Monitor identity and firmware handoff

The VirtIO optional EDID feature supplies a bounded monitor block. UEFI reads
active EDID on the same handle as the selected GOP interface; it never borrows
identity or preferred timing from an unrelated output. Boot protocol v5 appends
a copied 128-byte base block; v1–v4 producers remain supported. The kernel checks
header, version, checksum, manufacturer encoding, bounded text and detailed
timing bounds before publishing identity, physical dimensions or preferred mode.
Invalid or absent EDID leaves a generic display label. Listed refresh timing is
EDID metadata, not an implemented refresh-rate selector.

Intel Gen9 preserves the existing verified firmware plane/GTT/timing and native
PWM brightness support. General Intel modesetting is not implemented. Adapters
without prepare/select/release hooks remain at the firmware resolution and
Settings explains that live changes are unavailable. See [DAILY_USE.md](DAILY_USE.md)
for the supported Intel families and brightness limitations.

## Protocol references

The VirtIO GPU device protocol specifies GET_EDID (optional feature bit 1),
RESOURCE_CREATE_2D, RESOURCE_ATTACH_BACKING, SET_SCANOUT and RESOURCE_UNREF:
[VirtIO 1.2, GPU device](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html).
The firmware handoff follows the UEFI Graphics Output and EDID Active protocols;
DISPI uses the Bochs VBE register interface already used at startup. No Intel
register programming is added by this milestone.

## Validation and remaining work

`tests/drivers/run.sh` includes the real controller with a controlled clock and
fallible scanout, plus independently encoded EDID fixtures. `tools/display_smoke.py`
uses real UEFI/NVMe guests, QMP input and read-only GDB inspection to exercise
confirmation, rollback, shell reflow, document preservation and cold-boot
preferences. `--graphics bochs` selects the independent DISPI backend. Build with
`FB_W=1280 FB_H=800 images uefi-usb.img` before running; keep kernel/app binaries
unchanged while the guest is running. Screenshots are unmodified guest pixels.

This milestone does not implement multiple outputs, connector hotplug, native
Intel/AMD/NVIDIA modesetting, DisplayPort/HDMI link training, accelerated rendering,
refresh-rate selection, rotation, HDR/color profiles or suspend restoration.
Physical laptop display testing is still required. These are follow-up graphics
milestones, not capabilities inferred from successful virtual-device tests.
