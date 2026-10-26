# Contour desktop redesign

Contour uses mineral surfaces, a sea-glass accent, precise line symbols and
compact desktop controls. Content, navigation and window controls have distinct
roles. Depth is reserved for temporary surfaces and overlapping windows.

## Baseline audit (before Contour)

- EmUI (`ui/dsl`), the widget kit and semantic theme are shared by the native
  applications. Layout and the CPU scene renderer support clipping, gradients,
  alpha, shadows and bounded backdrop blur. Rendering is retained and damage
  based; full-screen animated blur is inappropriate on the software fallback.
- The compositor owns stacking, pointer routing, kernel decorations, shadows,
  rounded clipping and lifecycle animations. Most apps draw their own chrome.
  Its fixed work-area constants disagree with the dock's adjustable footprint.
- Preferences use a per-user text file with atomic replacement. The desktop
  supplies restricted shell components over a channel. Ordinary apps currently
  apply appearance only at launch. Several copies of theme application disagree.
- Symbols are Unicode stand-ins with incompatible silhouettes. Application SVGs
  use a mixture of borrowed glyphs. The multi-resolution EIC pipeline can supply
  original, optically sized symbols without a new runtime vector renderer.
- The top bar places text directly over wallpaper, uses punctuation for network
  status and has no coherent status grouping. The launcher consumes the entire
  display. Settings mixes fixed sizing, hard-coded selection colours and a long
  unsearchable sidebar. Fonts load regular weight for both regular and bold.

## Implementation coverage

The table distinguishes implementation from exercised behavior. Shared-token
coverage does not establish physical hardware compatibility or exhaustive
application-specific interaction coverage.

| Surface | Implementation and verification |
| --- | --- |
| Semantic colours, typography, dimensions, motion/materials | Shared theme and toolkit implemented; contrast/configuration and UI host suites pass, including ASan/UBSan |
| Preferences, validation, presets, persistence, live updates | Per-user snapshots, live updates and confirmed reset; native controls and cold-boot persistence pass |
| Original application icons and line-symbol family | 39 original line assets and coordinated app masters; source/EIC mapping checks pass |
| Desktop, dock, search and launcher | Bounded library, keyboard search/launch, side/bottom docks and edge reveal; native acceptance passes |
| Command strip, menus and measured system indicators | New layout and menus, top/bottom placement, group order and visibility; native menu/edge tests pass |
| Compositor, work area, cursor, decorations and workspaces | Versioned shared contract, isolated spaces, minimum-size rules, fill/restore, tiling and appearance controls; native workspace/input/geometry checks pass; drag-edge snapping implemented |
| Settings navigation, preview, controls and reset | 12 searchable panes, presets, colour/type/material controls and reset; actual control-driven acceptance passes |
| Files and shared file picker | Semantic navigation/selection and shared chrome; native Open/Save/replacement and picker worker-lifetime checks pass |
| Terminal | Shared chrome and scoped monospace transcript; native search/launch/capture passes |
| Editor and Note++ | Shared chrome, code/document palettes and metrics; three-boot save/dirty-close/recovery/reopen workflow passes |
| Calculator, Monitor, Photos and Music | Shared native chrome/tokens and usable minimum sizes; native launch/close/captures pass; Calculator arithmetic checked |
| Notification history and banners | Aligned history/empty state and banner work-area placement; history launch/close verified; banner edge combinations not separately exercised |
| Login, setup and lock | Shared default/session styling; authenticated setup/login, themed lock, blocked workspace shortcuts and password unlock pass |
| Clock widget and examples | Shared runtime appearance and real wall clock/date/uptime; clock source compile checked; example galleries not individually guest-tested |
| Vellum / NetSurf | Shared native toolkit/chrome; browser engine and content development remain with the user |
| Console tools and recovery | Console behavior retained; terminal chrome follows the design system; cold filesystem integrity and native service batch pass |

## Product rules

Controls offered in Settings must affect a real consumer. Unsupported features
are recorded here rather than represented by decorative switches. Opaque and
reduced-motion modes must work without compositor effects. Theme indices in old
preference files keep their meaning. Keyboard focus must remain visible. No
changes may weaken session, namespace or window ownership boundaries.

## Configuration and boundaries

`~/settings.conf` remains a readable integer-key format. Existing keys keep their
meaning; absent new keys get defaults. The complete snapshot fits in a bounded
2 KiB buffer, is atomically replaced and syncs both its contents and directory.
The same serializer supplies restricted processes through `/run/emlink.prefs`.
Unknown keys are ignored when reading and not retained by a Settings rewrite.

`oscfg.h` owns defaults, bounds and four coordinated presets. `appearance.h`
applies them once across shell/apps/widgets. Background workers fetch complete
snapshots; render threads alone change fonts, theme state and retained layout.
Preset/reset operations leave sound, time zone, keyboard layout, the system
shortcut modifier and power policy intact. Custom surface hues are adapted to the selected light/dark ground;
interactive accent tones are adjusted for readable text and selection contrast.

The versioned `desktop_control` ABI (147) shares insets, palette, corner/shadow,
cursor, material, shortcut and workspace preferences with the compositor. GPU
capability is required; only the process owning the desktop layer may publish
its configuration. Queries and workspace requests cannot bypass session locks.
Flags in existing window snapshots encode the workspace in bits 8–15 without
changing their layout. Hidden spaces are excluded from rendering, pointer
routing, keyboard focus and window cycling; minimizing remains a separate state.

The dock offers bottom/left/right placement. Auto-hide retains the work-area
band: this architecture draws the dock in the desktop layer, so releasing that
space to application input would make the dock unreachable. The command strip
supports top/bottom placement, height, group order, item visibility and edge
reveal. Menus grow a transparent canvas while open; tooltip canvases pass empty
pixels through, while menu dismissal canvases intentionally capture outside
clicks. Decorative toolkit overlays are excluded from hit testing.

Shared controls support Tab/Shift+Tab, Return/Space activation, visible focus
and scroll revelation. Dialog and menu scopes contain focus and restore their
initiating controls; menu arrows/Home/End/Escape retain slider and text-field
editing keys. See [KEYBOARD_FOCUS.md](KEYBOARD_FOCUS.md) for contracts and limits.
Assistive-technology semantics and arbitrary shortcut recording remain further
work. System shortcuts offer Super or Ctrl+Alt as their modifier:
Space opens the library, Tab cycles windows, W closes, Q quits, 1–6 selects a
workspace, Shift+1–6 moves a window, and [ / ] walks spaces; Shift+P shows what
composing costs ([DESKTOP_PERFORMANCE.md](DESKTOP_PERFORMANCE.md)). Editing
shortcuts retain their existing meanings.

Scaling applies to shared type, control and spacing metrics; it is not a new
fractional display compositor. Application minimum sizes and bounded scrolling
protect compact windows. Some application-specific canvas dimensions remain
fixed. Arbitrary font/icon-pack imports, free-form panel item dragging,
per-monitor profiles and arbitrary wallpaper-file selection are not implemented.
The browser engine and web content remain outside this change; shared native chrome still inherits Contour.

## Verification

The x86 BIOS/userspace and UEFI/NVMe images build with the project toolchain.
`make contour-test` checks 1,438 combinations and assertions for schema bounds,
serialization, presets, work areas and readable theme contrasts. Scene, declare,
kit and EmUI host suites cover layout, text editing, focus/carry input, search
submission and overlay hit testing. The close-control test also loads the
shipped EIC asset: a simultaneous hover tint change must retain its click.

`tools/contour_smoke.py` exercises an authenticated account, real workspace
isolation, live presets, application windows and preferences after a cold boot.
`tools/contour_controls_smoke.py` drives the actual Settings controls through QMP;
`tools/native_ui.py` only reads labels, arranged bounds and compositor state.
`tools/contour_shell_smoke.py` covers monochrome silhouettes, custom palette
values and power availability. `tools/contour_windows_smoke.py` exercises the
resize minimum, fill/restore and the layout board.
`tools/contour_modal_smoke.py` checks the complete explanation and cancellation
geometry at 115 percent scale with the serif family and compact spacing.
The authenticated desktop, customization controls, three-boot document workflow
and UEFI/NVMe integration batch pass. The integration batch includes desktop ABI
capability/ownership/argument gates, preferences, files, document barriers, input
snapshots, page cache, IPC cancellation, channels, compositor cleanup, sessions,
kill-I/O and POSIX. Native ABI checks report 147 unique calls and 14 matching
capability IDs. The ARM64 kernel cross-links; ARM runtime and physical laptop
validation remain separate work. A screenshot alone is not an interaction or
persistence assertion.

The icon-capture, field-width, wrapped-scroll, text-painting and reduced-toast
regressions fail before their fixes and pass afterward. Icon-family cache
capacity, default dialog fill and naturally wrapped dialog height have the same
fail-before/pass-after coverage. A held-toggle native
regression requires one physical gesture to persist exactly once across desktop
publication. A held mouse is suppressed until release when changing workspaces;
ordinary appearance updates retain capture. Native testing also verifies
alternate system modifiers, hidden workspace input isolation, side-band placement, inward bottom menus, reduced
effects, font/scale/density changes, cancelled reset and accepted reset. The
monochrome shell uses matching line silhouettes instead of tinting opaque app
tiles. Unknown third-party icons use the generic library silhouette. The bounded EIC
cache accommodates both complete shipped icon families in one process.
Dialogs retain an opaque semantic surface, including after live theme changes.
Opening the library or lock clears focus from persistent desktop controls; the
new search/password field receives it. Home explicitly invalidates its renderer
on structural modal transitions, rather than on every dock frame.

Actual captures and source metadata are in the
[progress gallery](media/laptop-progress/README.md). Test tools operate on copied
disks and use read-only GDB observations with real QMP keyboard/pointer actions.

## Text and motion contract

Measurement and CPU painting now use the same allocation-free UTF-8 line walk,
including word/character wrapping, repeated spaces and explicit line breaks.
`ui_text_line` preserves preformatted content; single-line fields and scoped code
content opt out of automatic wrapping. Glyph cache sizes use their canonical
quarter-pixel key so metrics do not depend on which nearby size was requested
first. The renderer vtable carries an explicit wrapping flag; bundled native
applications and the toolkit are rebuilt together.

The shared motion timing token drives menus, page transitions and toast fades.
Reduced motion presents toasts without a fade/slide and keeps indeterminate dots
static. No animation is required for a control to communicate its state.
