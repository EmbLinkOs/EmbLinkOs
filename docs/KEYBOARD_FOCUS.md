# Native keyboard focus

The shared toolkit now carries each key byte together with the modifier snapshot
captured by the input driver. `ui_input_key(code, modifiers)` preserves that
snapshot; `ui_input_char(code)` remains the unmodified-byte compatibility API.
The existing kernel ABI and the editing/navigation byte values are unchanged.

## Behavior

- Tab and Shift+Tab walk fields and shared controls in declaration/reading order.
  Both directions wrap inside the active focus scope. Keys following a transfer
  follow the destination; fields retain their edited-buffer identity when rows
  change. Removing a target drops its queued text instead of replaying it into
  another field or keeping an idle app rebuilding. Reverse transfers to an
  already-built control finish
  on the next requested build. A surviving sibling in the same form can receive
  focus without receiving the removed field's text. Return/Space activate shared
  buttons and toggles.
- A newly focused control inside a shared scroll view is revealed. Subsequent
  manual scrolling remains under the user's control until focus changes again.
- Dialog overlays contain keyboard focus and restore their initiating control
  after closing. Explicit field autofocus takes precedence; otherwise the first
  eligible control receives focus. Destructive dialogs should declare Cancel
  before destructive/confirming actions. Nested overlays restore their own
  initiating controls before returning to the surrounding interface.
- A focused menu opens with Down at its first control or Up at its last. Open
  menus use Up/Down, Home/End and Tab/Shift+Tab to navigate, Return/Space to
  activate, and Escape to dismiss. Left/Right walks adjacent bar menus, except
  when a focused slider or text field owns horizontal input. Hidden menu
  descendants never enter the focus order. Context menus use the same scope.
- While a shared modal scope owns input, the runtime keeps ordinary keys out of
  application hooks behind it. App-specific Escape cancellation remains
  available. Standard overlays expose Escape through `OverlayDismissed()`;
  Settings uses it to cancel Reset without closing the Settings application.
- Text-field context menus retain their original field and selection. Editing
  commands wait for that field's focus to return before entering its input
  queue. A destroyed owner does not receive a deferred command.

Multi-line document editors retain their existing editing meaning for Tab.
System window/workspace shortcuts remain compositor-owned and retain their
session and capability checks.

## Shared API

`ui_focus_register` records eligible controls. `ui_focus_scope_begin/end` contain
focus within an overlay, with an optional explicit restoration target for menu
buttons. Scope restoration uses generation-checked retained handles. Menu scopes
select their first control while building; ordinary dialogs allow an explicit
field autofocus before selecting their default control. Replacing a menu can
retain subsequent queued keys; opening a new dialog consumes its triggering
activation. Decorative and explicitly excluded subtrees cannot take focus.

The SDK loop requests another build for pending traversal, focus restoration or
scroll revelation. Home uses the same modifier-aware key feed in its custom
loop. All state is per UI process and updated on its render thread. The reading
order uses bounded arrays for 512 controls and eight nested scopes; no heap
allocation is required per navigation event. Key queues retain their existing
bounded capacity. This is keyboard interaction support; assistive-technology
roles, labels, announcements and platform integration remain separate work.

## Verification

`make kit-test em-test declare-test` covers queued modifier snapshots, backward
wrap, mixed transfers, fields/buttons/toggles, scroll revelation without scroll
pinning, nested restoration, modal isolation, menu separators/arrows/endpoints,
queued menu switching, slider input, removed-field transfers and context-menu Copy. Existing text-editing,
click-capture and window-control regressions remain in these suites.

`tools/contour_keyboard_smoke.py` uses real QMP input and read-only GDB geometry:
Settings Reset cycling and cancellation, restored keyboard activation, system
menu navigation and an unsaved Editor document whose text and focus survive
keyboard cancellation. `tools/contour_smoke.py` covers authenticated desktop,
workspaces, live appearance, application launching, cold preferences and lock.
`tools/workspace_smoke.py` covers document dialogs and three independent boots.
Native tests run on disposable UEFI/NVMe image copies with matching binaries.
