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

## Native Intel modesetting on the HP ProBook 450 G7

The target is the HP ProBook 450 G7: 10th-generation Core (Comet Lake-U),
Intel UHD Graphics with a Gen9 display engine, a Comet Point PCH, and an eDP
panel (some units also carry an NVIDIA GPU, which does not drive the panel).
Native modesetting means the OS programs that display engine itself --
display clock, PLLs, transcoder timings, planes, watermarks, DDI buffers, DP
link training, panel power sequencing and backlight -- instead of keeping the
mode the firmware left. QEMU has no Intel display engine, so every step is
measured on the laptop, and each one leaves its evidence on the boot stick.

| Step | What it delivers | State |
|---|---|---|
| 1. Capture | `/EMBKDISP.BIN`: a read-only record of the laptop's display, and a decoder | Built, host-tested |
| 2. Arithmetic | Link choice, M/N, HDMI PLL, CDCLK, DDB, watermarks, panel fitter (`intel_config.c`) | Built, checked against hand-worked values |
| 3. eDP modeset | The whole pipeline programmed natively (`intel_hw.c`), the policy around it (`intel_native.c`) | Built, checked against a register model |
| 4. Other modes | Lower resolutions on the panel through the pipe scaler, behind Settings → Displays | Built, checked against a register model |
| 5. External outputs | DP and HDMI enable paths exist in `intel_hw.c`; driving a second output is the multiple-monitor work | Hardware layer built; outputs not yet offered |

None of steps 2-5 has run on Intel silicon yet: QEMU has no Intel display
engine. The laptop's capture (step 1) and a boot of the stick on the ProBook
are what turn "checked against a model" into "works on the machine".

### Step 1: the capture

On any machine with an Intel display controller, the kernel records, before
any driver claims the device:

- the GPU's, PCH's and any other display controller's PCI configuration;
- the ACPI OpRegion and its VBT -- the board's own table of which DDI is the
  eDP panel, which is HDMI, which AUX channel and DDC pin each uses, the
  panel's power-sequencing delays and its backlight PWM;
- every Gen9 display register block: power wells, DC states, CDCLK, the four
  DPLLs, the transcoders' timings and M/N values, the DDIs and their buffer
  translation tables, the pipe scalers, the planes with their DDB allocation
  and watermarks, panel power and the backlight;
- each DP/eDP sink's DPCD (receiver capabilities, eDP link-rate table, link
  and lane status, eDP backlight capabilities) and its EDID read over AUX;
- each HDMI connector's EDID over GMBUS;
- the DRAM latencies pcode reports, which the watermark calculation needs;
- the GTT entries behind the firmware's framebuffer.

It writes nothing that changes the display: the only register writes are
AUX, GMBUS and pcode request transactions, the same ones Linux makes while the
firmware's mode is on screen. AUX channels B-D are probed only when the
firmware left power well 2 on. The record goes to the boot stick's EFI
partition as `EMBKDISP.BIN` (beside `EMBLINK.LOG`), and the boot log gains one
line beginning `intel-capture:` that summarises it. Read it on another
computer:

```sh
python3 tools/intel_display_dump.py /Volumes/EMBLINK/EMBKDISP.BIN
python3 tools/intel_display_dump.py /Volumes/EMBLINK/EMBKDISP.BIN --json
```

The report leads with what the firmware put on screen -- for example
`transcoder eDP -> DDI A, DP SST, 4 lanes, 8 bpc, pipe A: 1920x1080 (total
2080x1110), 142.5 MHz, 61.72 Hz, DPLL0 at 2.70 Gbps` -- followed by the VBT's
ports, the sinks' capabilities and EDIDs, and the raw decoded state.

Verification: `tests/drivers/intel_capture_test.c` runs the real AUX, GMBUS
and capture code against a simulated Comet Lake laptop -- a DP sink that
DEFERs, returns short replies and NACKs a reserved range, an EDID behind
I2C-over-AUX, an HDMI EDID behind GMBUS, pcode, an OpRegion 2.1 with its VBT
behind RVDA and one with the VBT in mailbox 4 -- checks every section of the
file, and asserts the capture never writes a register outside those
protocols. `tests/drivers/intel_capture_decode_test.py` then decodes that
file and checks every value the simulation holds. Both run from
`tests/drivers/run.sh`.

### How native modesetting works

At boot the driver reads the VBT from the OpRegion, the panel's DPCD (with
the eDP 1.4 link-rate table) and EDID over AUX A, the DRAM latencies from
pcode, and the firmware's live pipe. **When the firmware's mode is already
the panel's native mode, nothing is reprogrammed** -- the boot keeps the
firmware's picture, flicker-free. When it is not (a firmware that left
1024x768 scaled onto a 1920x1080 panel, or nothing lit), the boot sets the
panel's own mode natively.

Every later change is a real modeset through the gpu_driver hooks Settings →
Displays already uses (`prepare`, `select`, `release`, `describe`):

- **prepare** allocates a framebuffer in system RAM, maps it into the GGTT at
  a 256 KiB-aligned range of the aperture that never overlaps the firmware's
  framebuffer, and clears it through the aperture. Rows are padded to
  64 bytes, so a 1366-pixel-wide mode works (`gpu_driver.pitch`).
- **select** computes the full configuration: the panel's native timing on
  the transcoder, the panel fitter for anything smaller (aspect kept,
  centred), the link from the panel's capabilities and the VBT's fast-link
  parameters, the DDB split and watermarks from the pcode latencies. If only
  the surface changed it is a plane flip; otherwise the pipe is turned off
  (panel kept powered, so no 500 ms power cycle), and turned on again with
  link training. **If the new mode fails, the previous one is put back.**
- **release** clears the GGTT entries and frees the pages, and refuses a
  framebuffer still on screen.
- **describe** reports the panel's EDID identity and only the resolutions it
  can show -- nothing larger than the panel -- with its native mode preferred.

Safety rules: a port's buffer-translation table as the firmware programmed
it is kept rather than replaced with the built-in one; the firmware's panel
power-sequencing delays are kept; the brightness the user set survives a
modeset; DPLL0 (which feeds CDCLK) is never turned off. **Recovery and
Diagnostics boots never use native modesetting** -- they keep the firmware's
display, which is the way out if this driver ever fails a machine. The boot
log says which path it took: `intel-native: the firmware's mode is the
panel's own; keeping it`, `intel-native: eDP WxH on a WxH panel, R Gbps xL`,
or why native modesetting declined.

Tests (all in `tests/drivers/run.sh`): `intel_vbt_test.c` (VBT parsing,
including corrupt and overrunning VBTs), `display_modes_test.c` (EDID mode
lists), `intel_config_test.c` (the arithmetic, against values worked by hand:
the 148.5 MHz HDMI PLL, 1080p M/N, every watermark level), `intel_hw_test.c`
(a register model with a DP sink that demands a particular swing and
pre-emphasis: values and write ordering for eDP, DP with link fallback, HDMI
with AVI infoframes, failure undone), and `intel_native_test.c` (inherit,
prepare/select/release, panel fitter, plane flips, a failed modeset restored,
boot modesets, a 1366-wide panel).

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
Runtime acceptance is x86-64; ARM64 is compile-checked only. Its existing
screen-session isolation remains unported. The strip-width opt-in is a function
so the public EmApp descriptor keeps its existing binary layout.

This milestone does not implement multiple outputs, connector hotplug, native
Intel/AMD/NVIDIA modesetting, DisplayPort/HDMI link training, accelerated rendering,
refresh-rate selection, rotation, HDR/color profiles or suspend restoration.
Physical laptop display testing is still required. These are follow-up graphics
milestones, not capabilities inferred from successful virtual-device tests.

Verified on 2026-10-25: both x86-64 backends completed the native display suite,
including actual scanout dimensions, visible confirmation, explicit/timeout/exit/
lock rollback, unsaved Editor contents and restoration after password login on
a cold boot. The 32 driver protocol suites pass ASan/UBSan; appearance has 1,446
passing checks. Native ABI verification reports 155 unique calls and 14 matching
capabilities. The EmApp descriptor matches master (72 bytes, final field at 64).
BIOS POSIX and UEFI/NVMe device, preferences, files, IPC, session, compositor and
POSIX regressions pass. The ARM64 kernel links; runtime acceptance is outstanding.
