# Desktop performance

What composing the desktop costs, as the compositor measures it, and how each
change moved the numbers. The plan these measurements serve takes the desktop
to a frame clock with adaptive 60–120 Hz, spring motion, materials and better
type; its open items are in [TODO.md](TODO.md#fluid-desktop-frame-clock-motion-materials-type-2026-10-26).

## Reading the numbers

A **compose** is one repaint of a screen region from the window stack and its
presentation: from the first pixel drawn to the copy to the scanout (or the
GPU's flush). The compositor charges each compose to the **cause** that asked
for it:

| Cause | What asked |
|---|---|
| present | an application presented new pixels |
| move | a window was moved, placed or resized by request |
| pointer | the cursor moved, a click raised a window, a kernel title bar was dragged |
| anim | window motion: open, minimise, restore, close |
| other | create, destroy, focus, workspaces, displays |

The **pointer latency** is the time from the mouse driver noticing a motion to
the compose that drew the cursor there. It is what software can observe;
photons need a camera.

Three ways to read it:

- **GUI+Shift+P** shows an overlay at the top right of the primary display,
  refreshed twice a second: the renderer and size; composes and megapixels in
  the last second; compose time (median, 95th percentile, worst of the last
  512); pointer latency; composes per cause since the counters started. Its own
  repaints are left out of what it reports.
- The serial console's `perf` prints the same as `key=value` integers, then the
  busiest presenting windows; `perf reset` starts the counters again; `perf hud`
  toggles the overlay.
- `embk_comp_stats()` (`user/lib/embk_comp_stats.h`) hands any program the
  snapshot. It says how long composing takes and why, never what a window shows.

`tools/perf_smoke.py` boots a disposable copy of `uefi-usb.img`, makes an
account, logs in and measures five scenes: ten idle seconds, the pointer swept
across the desktop, the Settings window dragged by its title and back, the
launcher opened and dismissed, and a Terminal opened from the dock and closed
with GUI+W. It writes `perf.json` and a capture with the overlay.

```sh
make FB_W=1280 FB_H=800 STAGED_APPS="build/cxxdemo.elf build/embcc.elf build/embld.elf" uefi-usb.img
EMBLINK_OVMF_DIR=<ovmf> EMBLINK_GDB=x86_64-elf-gdb python3 tools/perf_smoke.py --label <name>
```

Times measured under TCG are the emulator's. They compare one build with
another on the same host; they say nothing about a machine. The counts (how
many composes, of how many pixels, why, and how many covered the whole screen)
are the same anywhere.

## Baseline (241cb9d)

QEMU x86_64 under TCG on an Apple M5 host, 2 CPUs, UEFI, virtio-vga at
1280 × 800. The CPU composes, and the kernel is built `-O0` without SSE.

| Scene | Composes | Per second | Pixels | Full screen | Compose p50 | Compose p95 | Pointer p95 |
|---|---|---|---|---|---|---|---|
| Idle, 10 s | 104 | 10.1 | 2.9 Mpx | 0 | 6.0 ms | 15.4 ms | — |
| Pointer sweep | 929 | 19.5 | 14.5 Mpx | 0 | 5.9 ms | 15.3 ms | 18.6 ms |
| Drag Settings | 97 | 10.8 | 20.2 Mpx | 0 | 8.2 ms | 221.9 ms | 308.0 ms |
| Launcher | 29 | 3.6 | 10.9 Mpx | 3 | 16.7 ms | 279.4 ms | — |
| Terminal open + close | 100 | 4.9 | 24.8 Mpx | 3 | 15.4 ms | 287.6 ms | 211.4 ms |

What it says:

- **An idle desktop is not idle.** With nothing changing, the desktop process
  presented 84 times in ten seconds (about 26 thousand pixels each), and the
  top bar 20 times (38 thousand each). That is a compose every 100 ms for
  nothing.
- **A drag re-presents the whole window on every move.** The Settings window
  moved itself 10 times and presented 18 times, each time all 647 thousand of
  its pixels. That is where the 222 ms composes and the 308 ms pointer latency
  come from.
- **Window motion is one frame here.** Opening and closing a Terminal produced
  a single animation compose: the 170 ms open and 150 ms close are shorter than
  one compose under TCG, so the motion jumps to its end.
- **Composing is slow per pixel.** On the host, the compositor's own blend loop
  takes 11.4 ms for a 1280 × 800 screen as the kernel builds it (`-O0`), and
  0.7 ms at `-O2` with two channels per multiply. Its box blur takes 28 ms.
  Every number in the table above carries that factor.

**Correction.** The 0.7 ms quoted above for the blend at `-O2` was measured with
the host compiler vectorising the loop (NEON on the M5). The kernel is built
without SSE or NEON and can never do that. Scalar, as the kernel builds it
(`make pix-bench`), the blend goes from 11.6 ms to 2.4 ms and the box blur from
17.2 ms to 4.5 ms. The real gain is five times, not sixteen.

## After P0 (ec0718b)

What changed:

- **The pixel loops** (`kernel/gfx/pix.c`) are compiled `-O2` with
  `framebuffer.c`, `backdrop.c` and `soft_compose.c`; the rest of the kernel
  stays `-O0`. They are bit-exact with the old ones (`make pix-test`).
- **Nothing dirty is presented as nothing.** `em_app_run`, widgets and the
  desktop presented their whole window whenever a frame they built changed no
  pixel.
- **A glass box and an idle menu button are no longer dirty on every frame.**
  Each set a property twice per frame (`make em-test` covers both).
- **A moved window is not repainted whole.** Its pixels do not change when it
  moves.
- **Shortcuts work on arm64.** System shortcuts and GUI commands are decided
  for every keyboard, not only PS/2.
- **The arm64 kernel reads `perf`**, and `tools/perf_smoke.py --arch aarch64`
  measures the same scenes under HVF.

### x86 under TCG, against the baseline

| Scene | Composes | Pixels | App presents | Compose p95 | Pointer p95 |
|---|---|---|---|---|---|
| Idle, 10 s | 104 → **0** | 2.9 → 0 Mpx | 104 → 0 | 15.4 ms → — | — |
| Pointer sweep | 929 → 453 | 14.5 → 1.1 Mpx | 477 → 1 | 15.3 → 0.3 ms | 18.6 → 0.7 ms |
| Drag Settings | 97 → 184 | 20.2 → 79.8 Mpx | 54 → 1 | 221.9 → 23.8 ms | 308.0 → 23.8 ms |
| Launcher | 29 → 6 | 10.9 → 5.5 Mpx | 25 → 2 | 279.4 → 31.4 ms | — |
| Terminal open + close | 100 → 44 | 24.8 → 10.6 Mpx | 88 → 16 | 287.6 → 32.9 ms | 211.4 → 14.1 ms |

Reading the drag row:

- The window now follows the pointer more often in the same time: 111 move
  composes against 10. Each one recomposes the window's old and new
  footprints, about 0.7 Mpx.
- Its median compose time rose for a different reason: the small idle presents
  that used to pad the median are gone.

### arm64 under HVF: a real core composing

QEMU virt under HVF on the same Apple M5 host, 4 CPUs, virtio-gpu at
1280 × 800, CPU composition. The guest runs on the host's own core, so these
are the times of a real CPU, not an emulator's.

| Scene | Composes | Per second | Compose p50 | Compose p95 | Pointer p95 | Window motion |
|---|---|---|---|---|---|---|
| Idle, 10 s | 0 | 0 | — | — | — | — |
| Pointer sweep | 453 | 9.6 | 0.05 ms | 0.12 ms | 0.10 ms | — |
| Drag Settings | 462 | 55.2 | 1.97 ms | 2.33 ms | 1.89 ms | — |
| Launcher | 10 (8 full screen) | 1.3 | 2.23 ms | 2.46 ms | — | — |
| Terminal open + close | 51 | 6.0 | 2.73 ms | 3.07 ms | 0.23 ms | 31 frames |

What it says about the frame budget:

- **A full 1280 × 800 compose takes about 2.2 ms on one M5 core.** That is a
  fast core.
- **At 1920 × 1080 on a typical laptop core, the same compose would take about
  9–13 ms.** That estimate scales by pixels (×2) and by core speed (×2–3); it
  is not a measurement. That fits within 60 Hz (16.7 ms) and does not fit
  within 120 Hz (8.3 ms).
- **What P1 has to do next:**
  - compose only the damage, with one compose per frame on a frame clock;
  - make window drags and the cursor cheap, so motion never needs a full
    compose;
  - let the rate governor hold an even 60 Hz where 120 Hz will not fit.
- **The open and close motions run at about 100 frames a second here, 31
  frames in all.** Under TCG they show 4. The motion itself was never the
  problem; the emulator was.

The arm64 desktop's text layout is wrong in these captures, and it is the same
before this work: see TODO.md.
