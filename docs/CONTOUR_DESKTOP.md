# Contour desktop redesign

Contour uses mineral surfaces, a sea-glass accent, precise line symbols and
compact desktop controls. Content, navigation and window controls have distinct
roles. Depth is reserved for temporary surfaces and overlapping windows.

## Audit

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

This is a working checklist. A shared-token migration is not a claim that every
application-specific interaction has been redesigned or hardware-tested.

| Surface | Implementation and verification |
| --- | --- |
| Semantic colours, typography, dimensions, motion/materials | In progress |
| Preferences, validation, presets, persistence, live updates | In progress |
| Original application icons and line-symbol family | In progress |
| Desktop, dock, search and launcher | In progress |
| Command strip, menus and measured system indicators | In progress |
| Compositor, work area, cursor, decorations and workspaces | In progress |
| Settings navigation, preview, controls and reset | In progress |
| Files and shared file picker | Pending alignment |
| Terminal | Pending alignment |
| Editor and Note++ | Pending alignment; preserve safe-save/recovery |
| Calculator, Monitor, Photos and Music | Pending alignment |
| Notification history and banners | Pending alignment |
| Login, setup and lock | Pending alignment; preserve authentication |
| Clock widget and examples | Pending shared-component verification |
| Vellum / NetSurf | Shared toolkit only; browser development reserved for user |

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
Preset/reset operations leave sound, time zone, keyboard layout and power policy
intact. Custom surface hues are adapted to the selected light/dark ground;
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

Shared controls support forward Tab traversal, Return/Space activation and
visible focus; sliders also accept arrow keys. Reverse traversal, directional
menu navigation, assistive-technology semantics and arbitrary shortcut recording
remain further work. System shortcuts offer Super or Ctrl+Alt as their modifier:
Space opens the library, Tab cycles windows, W closes, Q quits, 1–6 selects a
workspace, Shift+1–6 moves a window, and [ / ] walks spaces. Editing shortcuts
retain their existing meanings.

Scaling applies to shared type, control and spacing metrics; it is not a new
fractional display compositor. Application minimum sizes and bounded scrolling
protect compact windows. Some application-specific canvas dimensions remain
fixed. Arbitrary font/icon-pack imports, free-form panel item dragging and
per-monitor profiles are not implemented. The browser engine and web content
remain outside this change; shared native chrome still inherits Contour.
