# Vellum — EmbLink's web browser

*Design document. Written before the code, so the code has something to be
wrong against.*

---

## 1. What this is, and what it is not

**Vellum is a document browser.** It fetches over EmbLink's own network stack,
parses HTML into a document, styles it, lays it out, paints it with our own
font rasteriser, and follows links. It is a real browser in the sense the word
had before it also meant "application runtime".

It is **not** a modern web application platform, and the design says so up
front rather than discovering it at milestone seven:

| | |
|---|---|
| JavaScript | Not in v1. See §9 — the decision is *when* and *ported*, not *whether we could*. |
| CSS | A subset, ours, arriving in stages (§5). Not a spec-complete cascade. |
| Images | ✅ PNG and baseline JPEG, both from scratch; `<img>` reserves its box from the start. |
| Video, audio, canvas, WebGL | No. |
| Cookies, storage, service workers | No. |

The realistic target is **an excellent reader**: documentation, READMEs,
man pages, text-first sites, RSS-era blogs, and everything our own `httpd`
serves. That is a genuinely useful thing to own, and it is reachable.

### Why the name

`Vellum` is the surface a document is written on. The name commits the project
to what this is — a thing for reading documents — rather than implying a
browser that will one day run web apps. It also sits apart from the system
components (`EmbCC`, `EmbLD`, `EmbDBG`, `EmbKFS`), which is right: those are
tools the OS uses, this is an application the *user* opens, like Files and
Terminal.

The engine inside it is `user/web/` — reusable, testable on its own, and not
named after the app, so a second consumer (a help viewer, a Markdown preview)
can use it without dragging the browser in.

---

## 2. The pipeline

Six stages, each a module, each testable without the ones after it. This is
the whole architecture:

```
   URL ──▶ fetch ──▶ parse ──▶ style ──▶ layout ──▶ paint ──▶ interact
           net.c     html.c    style.c   (EmUI)     (EmUI)    app
             │         │          │         │
        TLS/TCP    document   computed   boxes with
        our own      tree      styles     geometry
```

The seams matter more than the boxes:

- **fetch → parse** is *bytes*. The parser never learns what a socket is, so it
  can be fed a file, a test string, or a response body without changing.
- **parse → style** is the *document tree*. Styling never mutates the tree; it
  produces a parallel array of computed styles indexed by node. That is what
  lets CSS arrive later without touching the parser, and lets a page be
  re-styled without re-parsing.
- **style → layout** is a *computed style per node*. The layout stage never
  reads a tag name or a selector. When CSS lands, layout does not change at all.
- **layout → paint** is EmUI's existing scene tree. We are not writing a
  rasteriser; we already have one.

### Module layout

```
user/web/
  html.h/.c      the parser + URL resolution           [DONE, host-tested]
  style.h/.c     computed styles: UA stylesheet, later CSS
  doc.h/.c       document: fetch + parse + style, and the history stack
  net.h/.c       HTTP/HTTPS fetch over our TCP + libtls
  render.h/.c    document tree + styles -> EmUI nodes
  html_test.c    host test                              [DONE, 33 assertions]
  style_test.c   host test
user/bin/vellum.c  the application: chrome, history, input
```

One concern per file, shared header as the contract — the same rule the network
stack and `httpd` follow.

---

## 3. The document model

Already built (`html.h`). A tree of `html_node`, each either an element (tag,
optional `href`) or text, allocated from **caller-supplied arenas** — no
`malloc`, and truncation is reported rather than silent, because this parses
bytes that arrived from a network.

Deliberately kept: implicit end tags, void elements, character references,
`<script>`/`<style>` skipped wholesale, malformed input never derailing the
parse.

Deliberately absent: a full attribute map. Only `href`/`src` are stored,
because those are the only ones anything downstream can act on. When CSS lands,
`class` and `id` and `style` join them — and that is the *only* parser change
CSS requires.

---

## 4. Styling: the seam that makes CSS optional

```c
struct computed {
    unsigned char display;      /* BLOCK, INLINE, LIST_ITEM, NONE      */
    unsigned char font;         /* the EmUI role: Body, Heading, ...   */
    unsigned char weight, italic, mono, underline;
    unsigned char list_marker;  /* bullet, decimal, none               */
    short  margin_top, margin_bottom, indent;
    Color  color;               /* 0 alpha = inherit                   */
};
```

Every stage after this reads *only* this struct. The stylist fills it, and how
it fills it is the part that grows:

**Stage 1 — the user-agent stylesheet (v1).** A table: `h1` is 26pt bold with
margins, `p` is body with a gap, `a` is accent-coloured and underlined, `pre`
and `code` are mono, `ul`/`ol` indent and mark their `li`s, `b`/`strong` bold,
`i`/`em` italic. About a hundred lines, and it makes a document readable *today*
— which is exactly what every browser did before CSS existed, and what every
browser still falls back to.

**Stage 2 — inline `style=""`.** The narrowest useful slice of CSS: no
selectors, no cascade, no specificity. Just a declaration parser (`color`,
`font-weight`, `font-style`, `margin`, `display`) writing into the same struct.

**Stage 3 — `<style>` blocks and a real cascade.** Selector matching (type,
class, id, descendant), specificity, then origin order. The declaration parser
from stage 2 is reused unchanged; only *selection* is new.

Each stage is shippable and each improves real pages. None of them changes
layout, paint, or the parser.

---

## 5. Layout: block and inline over EmUI

We are not writing a layout engine — we have one. The question is only how the
CSS box model maps onto it, and for a *document* the answer is clean:

- A **block** becomes a vertical stack with the computed margins as padding.
  Blocks stack down the page; that is what a column does.
- An **inline run** — a paragraph's mixed text, `<b>`, `<a>`, `<code>` — becomes
  a wrapping row of text pieces. EmUI's layout already does flex-wrap and text
  wrapping, which is the hard half.
- A **list item** is a block with a marker box and an indent.
- `<pre>` is a block that does not collapse whitespace and does not wrap.

What this cleanly does **not** give us: floats, absolute positioning, tables as
layout, vertical-align, multi-column. Those are the parts of CSS that need a
real box-model engine, and the design's position is that a document browser
does not need them. If that turns out to be wrong, it is a decision to revisit
with evidence, not a gap to paper over.

**REVISITED (2026-08-07), for DATA tables only.** The evidence is the subject
matter: reference pages -- the thing this browser exists to read -- state their
facts in tables, and a documentation browser that cannot show one is missing
the format its own material is written in. That is not "tables as layout", the
practice this paragraph rightly refuses; the refusal stands.

It also cost nothing structural, which is the tell that it belonged. A table's
one hard requirement is that columns line up ACROSS independent rows -- which a
row of stacks cannot do and the layout engine's GRID already did, spans
included. So a `<table>` is one grid, every cell a child of it in reading
order, exactly as a block is a column and an inline run is a wrapping row.

The honest limit: grid tracks are EQUAL width. Sizing each column to its
content needs a measurement pass the renderer cannot do -- it emits, layout
measures afterwards. Equal columns keep every row aligned, which is the
property that makes a table a table, and cost some space when one column is
short and another long. Content-proportional tracks are a layout-engine
feature, logged rather than faked.

**The known risk:** EmUI's layout was built for application UI, where the tree
is small and rebuilt per frame. A long document is a large tree. If it does not
hold, the fallback is to lay the document out *once* into a flat display list
and scroll that — which is what a browser does anyway. Milestone B3 measures it
rather than assuming.

---

## 6. Networking

Reuses what exists rather than growing a second HTTP client: `wget` already
resolves a name, opens a TCP socket, runs an authenticated TLS 1.3 handshake
through our libtls, and speaks HTTP/1.0. `user/web/net.c` is that logic lifted
into a module with a response-into-memory API instead of a file.

Additions the browser needs:

- **Redirects** (301/302/303/307/308), bounded — a redirect chain is a loop
  waiting to happen.
- **`Content-Type` sniffing** for charset and for "is this even HTML".
- **A byte cap per response.** A browser that will read an unbounded body from
  a stranger is a denial-of-service with a URL bar.

---

## 7. Security: what a page may do

This is short, and that is the point. The whole class of browser vulnerability
that comes from *executing what you downloaded* is absent by construction in
v1, and the rest is bounded by mechanisms the OS already enforces:

- **No script execution.** `<script>` is skipped at parse time, not
  sandboxed at run time.
- **Arenas are fixed.** A hostile page cannot exhaust memory; it gets truncated
  and told so.
- **The process holds `CAP_NETWORK` and nothing else it does not need.** It
  cannot write to the filesystem because it is not given the capability.
- **Its namespace is its confinement.** Vellum is granted `ro /system` for its
  font and `rw` on one downloads directory — so a bug in it reaches what it was
  given, and no more. Naming is owning.
- **No cookies, no storage, no credentials.** Nothing to steal.

When JavaScript eventually arrives, *this section is the thing it must not
break*, and the answer will be that the engine gets no bindings it has not been
explicitly handed.

---

## 8. The application

Chrome follows the house style already established by Files, Settings and the
Terminal: `AppBar` with the traffic lights leading, title centred, app controls
trailing. Specifically:

- **Bar:** back, forward, reload, then the URL field (the widest thing there),
  then a stop/loading indicator.
- **Page:** the rendered document in a scroll view.
- **Status line:** the URL under the pointer when hovering a link, the byte
  count and elapsed time after a load, errors in place of both.
- **History:** a back/forward stack of URLs, the same shape Files uses.
- **Errors are pages.** A DNS failure, a refused connection, a 404 and a TLS
  verification failure all render as a document explaining what happened. A
  browser that shows a blank window on failure is a browser you cannot debug.

### A capability the app model does not yet have

Vellum is the first **GUI application that needs the network**. Today an app
declares its *namespace* in a sidecar manifest (`<name>.ns`) which `home` reads
and grants; capabilities have no equivalent — they are passed by whoever spawns
the process. The design's answer is to extend the existing pattern rather than
invent a second one: a `<name>.caps` sidecar naming the capabilities the app
requires, which `home` turns into a `SET_CAPS` spawn action. The kernel already
enforces that a grant is a subset of the granter's own authority, so this adds
a declaration, not a privilege.

That is milestone **B0**, and it is worth doing properly because every future
networked app needs it.

---

## 9. On JavaScript

**The engine now runs on the OS (2026-08-07).** `js.elf` is QuickJS
2024-01-13, cross-compiled against newlib and hosted by `user/bin/js.c` -- a
hundred lines that create a runtime, bind `console.log`, evaluate, and report
an exception with its stack. QuickJS's own `qjs` CLI is deliberately NOT used:
it brings threads, dlopen, a poll loop and a module loader, none of which a
first port needs and each of which is a syscall surface to argue with.

The port cost one patch of two hunks. 58k lines cross-compiled with exactly
two errors, both optional POSIX -- `PTHREAD_MUTEX_INITIALIZER` (Atomics.* is
SharedArrayBuffer across OS threads; a single-context engine has nothing to
share with) and `struct tm::tm_gmtoff` (a BSD/GNU extension newlib lacks, for
which QuickJS already carries a portable path). The patch implements nothing;
it widens two existing `#ifdef`s so a port can select paths that were already
there. That is why it is a patch and not a fork, and it is the strongest
evidence for §9's original position.

Proven on the metal, from the OS's own shell:

    yves@emblink:~$ js /system/js/hello.js
    JavaScript, on an OS that wrote its own kernel.
    sum 1..100 = 5050
    sorted+squared: [1,9,25,81]
    regexp: 2024
    closure: 3628800

-- arithmetic and loops, Array.sort with a comparator, .map with an arrow
function, a regexp with a capture group, JSON.stringify, and a recursive
closure. A syntax error reports as QuickJS's own SyntaxError through the host's
handler.

**THE BINDINGS ARE DONE (2026-08-07).** `user/web/jsdom.c` gives a page's
script `document.querySelector` / `querySelectorAll`, `document.title`,
`el.textContent`, `el.setText`, `el.getAttribute`, `el.setStyle` and
`console.log`. Vellum runs every `<script>` after parsing, in document order,
and a mutation marks the document so the next frame shows it.

querySelector cost almost nothing, and that is the dividend from splitting the
CSS engine by concern: `sel.c` already answers "does this selector match this
element", so the binding is a tree walk plus a call. The browser's selector
engine and the DOM's are the same code.

Design decisions worth stating:

- An element reaches JavaScript as its NODE INDEX, never a pointer. The DOM
  arena is an array a mutation can grow, so a pointer handed to a script is one
  a later mutation invalidates -- a use-after-free with a stranger's page as
  the trigger. An index cannot dangle.
- ONE document. The tree a script mutates is the tree the renderer walks; there
  is no shadow DOM and therefore no synchronisation question.
- A NEW WORLD per page: the runtime is torn down and rebuilt on every
  navigation, so a script cannot outlive the document that wrote it and one
  page's globals are unreachable from the next.
- A page's script gets a BUDGET (16MB, 512KB stack). A browser that one line of
  someone else's JavaScript can hang is not one you can browse with.
- `el.setStyle("color:red")` is deliberately NOT spelled `el.style.color`.
  The real thing needs a property-per-CSS-property proxy; naming ours
  differently stops an author believing they have the real one and then
  wondering why it reads back undefined.

Proven on the metal: /system/web/script.html computes 10! with a recursive
closure, READS `document.title` out of the tree, counts `p span.out` with the
CSS engine, recolours an element through the cascade, and puts its
`console.log` in the status line.

**EVENTS ARE DONE (2026-08-07).** `addEventListener('click', fn)`,
`setTimeout`, `setInterval`, `clearTimeout`/`clearInterval`. A page responds.

The one design decision here that matters: the RENDERER asks the engine which
elements have listeners, and makes only those clickable. The alternative --
every container a hit target -- is a page whose links stop working and whose
text cannot be selected, so the engine decides and the renderer obeys.

Two refusals, both the same rule this browser applies everywhere. An event name
we cannot deliver THROWS rather than registering silently, because a listener
that never fires is the hardest bug in a page to see. And `setInterval(f, 0)`
is clamped to 10ms: a page asking to run as fast as the machine can go takes
the window with it on a shared UI thread, and cannot tell that it did anything
wrong.

A page with a live timer keeps getting frames; a page without one costs
nothing, because the app asks the engine when the next timer is due rather
than ticking unconditionally.

Proven on the metal, /system/web/interactive.html: a counter incremented by
clicks, restyled red by its own handler when it crosses five, a second button
that resets it, and a setInterval clock that ran past 100 seconds throughout --
independent of the clicking, with console.log from each handler in the status
line.

**fetch() IS DONE (2026-08-07).** `fetch(url)` returns a REAL Promise: QuickJS
hands over its resolve/reject pair, the request takes its turn on the one
worker the document and images share (the page you asked for outranks the data
a script wants about it), and the pump settles the Promise when the bytes land.
The Response is deliberately small -- `ok`, `status`, `text()` (async, as the
spec has it) -- because a documentation page's script wants status and text,
and a binding that pretends to more is the lie this browser refuses.

The part that is easy to omit and fatal to: the MICROTASK DRAIN.
`JS_ExecutePendingJob` runs the reactions promises resolve onto; without it,
`.then` callbacks queue forever and fetch "succeeds" while doing nothing
visible. The pump drains jobs after timers and fetches, every frame, and keeps
draining past a rejection so one bad promise cannot wedge the queue.

Proven on the metal: a page fetches /system/web/data.json, chains two .then()s,
JSON.parses the body, and writes three DOM fields -- 200 OK, the parsed name,
and the milestone list -- with its console.log in the status line.

STILL NOT DONE: forms and keyboard events, createElement/appendChild, event
bubbling beyond the listening element's own subtree, JPEG, caching. An engine that cannot touch the document is
still an interpreter that computes 2+2 -- exactly what §9 warned about. Next is
a `document` object over `struct html_doc`, then events, then the fetch the
browser already has. That work is Vellum's, not the engine's.

NOTE ON THE SHELL: `js -e EXPR` needs quoting -- `js "-e" "2+2"` -- because the
structured shell reads a bare `-e` as a unary minus in its own expression
syntax. Not a JS bug; worth knowing.

The original position, which held:

Not in v1. When it comes, the position this document takes is:

**Port QuickJS; do not write one.** ~50k lines of dependency-free C99 designed
to embed, and we have newlib and a working toolchain. Writing a JS engine to
any useful standard is a multi-month project that would buy nothing a port
would not. This is the line the project already draws well: own the *core*
(kernel, filesystem, TLS, libc, compiler, UI), port the *tools* (CPython, TCC,
git, zlib). A JS engine is a tool.

And it is genuinely last, not merely later: an engine's whole value is
manipulating a DOM, handling events and fetching. It presupposes a document, a
layout, an event loop and bindings. Built first, it is an interpreter that can
compute `2+2`.

---

## 10. Milestones

Each ends with something demonstrable. No milestone is "infrastructure".

| | | Witness |
|---|---|---|
| **B0** | `<name>.caps` sidecar; `home` grants capabilities declaratively | a GUI app opens a socket |
| **B1** | UA stylesheet + renderer; load HTML from a **file** | a local page renders with headings, paragraphs, lists, styled links |
| **B2** | `net.c`; fetch over HTTP; the app with URL bar and history | browse our own `httpd` — the OS reading itself over TCP |
| **B3** | Link following, back/forward, error pages; measure layout cost | click through a multi-page site; a number for "how big a document can we hold" |
| **B4** | HTTPS via libtls, redirects, byte caps | fetch a real site on the public internet |
| **B5** | ✅ Inline `style=`, `<style>` + selectors + cascade | a styled page looks like the author meant |
| **B6** | ✅ `<img>`: PNG via our own DEFLATE, alt text, async image fetch | a page with pictures |
| **B7** | ✅ JavaScript: QuickJS + DOM, events, fetch() | a page's own script reads, rewrites and responds |
| **+** | ✅ FORMS: text fields, buttons, GET and POST | a form you can fill in and submit |
| **+** | ✅ data tables over the layout grid (revisits §5 with evidence) | a reference page states a grid of facts |
| **+** | ✅ JPEG: Huffman + fixed-point IDCT + chroma upsampling | a photograph, not just a diagram |
| **+** | ✅ the raster made ~25x cheaper, on evidence (see below) | pages that scroll and load at a usable speed |
| **+** | ✅ SELECT and COPY: drag, Ctrl+A, Ctrl+C to the system clipboard | a browser you can take something out of |

**B1 through B3 are the ones that decide whether this is real.** If a
document renders and scrolls at a usable speed, everything after is addition.

---

## 11. Status

**B1 is rendering.** A document loads from the filesystem, parses, styles and
paints: headings at three sizes, paragraphs, bulleted lists whose text hangs
under itself, bold/italic/monospace runs, accent-coloured clickable links,
`<pre>` with its indentation intact, and character references decoded. The
status line reports the byte and node count.

Two bugs found in the TOOLKIT on the way, which is the useful part of building
a browser on your own UI stack:

- `Flow` set `ui_set_wrap(true)` and never set a size, so its width was
  intrinsic -- the sum of its children -- and a wrap container sized to its
  content can never overflow, so it never wrapped. Invisible for chips and
  tags, which do not overflow; fatal for inline runs, which exist to. Fixed:
  Flow fills its width, as Grid already did.
- Wrapped paragraphs laid out wrong: a paragraph's lines spread far enough
  apart that the next heading was drawn between them, but only at the TOP of a
  document -- the bottom was always fine. **Found and fixed.**

  THE CAUSE. A wrapping row overflows BY DESIGN; that is the trigger that makes
  it start a new line. The flex-shrink pass fired on it anyway and squashed all
  54 word boxes of a paragraph. The ARRANGEMENT never noticed, because the wrap
  arm places children at their base width and never reads the shrunken size.
  The MEASUREMENT did: a text child's cross size is its height at its final
  width, so every squashed word was measured at a fraction of its width and
  CHARACTER-wrapped. "a " reported one line, "This " three, "page " four. The
  line stride became the tallest of those lies -- 65px for a 16px line.

  Which explains the shape of the report exactly. A paragraph short enough not
  to overflow never enters that branch, so the bottom of a page was always
  right and only the top was wrong.

  Fix: a wrap row is exempt from shrink entirely (ui/layout/layout.c). Locked
  by layout test **T3d**, which was confirmed to fail when the bug is
  re-introduced -- a regression test nobody has watched fail is a guess.

  The same fault had a mirror on the other axis: a ROW takes a container
  child's height from `intrinsic_h`, computed before any width is known, so a
  wrapping column inside it counted as one line. That is a list item --
  [bullet][text column] -- and it is why a wrapped bullet had the next bullet
  drawn on top of it. Fixed by measuring such a child at the width it actually
  gets, gated on the subtree really containing a wrap so ordinary rows keep
  their intrinsic height and cannot regress.

  THE METHOD, which is the durable part. Everything from document bytes to
  resolved pixel geometry is syscall-free -- only the window and the network
  need the OS. So the whole pipeline runs on the host: `make browser-render`
  (user/web/render_host.c) parses, styles, renders, lays out, prints every
  resolved rectangle and writes a PNG, in two seconds. It reproduced the
  reported screenshot exactly on its first run, and the resolved tree named the
  culprit immediately -- every word box was a multiple of the line height.

  This should have been built before the first boot, not after the fifth. The
  symptom was visual, so the VM felt like the right instrument; it was not.
  Reach for the harness that matches the LAYER of the bug, not the layer of the
  symptom.

**B1 is done.** The start page renders correctly on the metal.

**FORMS (2026-08-07): the first part of the web that is not read-only.**

`user/web/form.{c,h}`. Text fields, submit buttons, GET and POST, Enter to
submit, and `el.value` from script.

The structural decision is where a value LIVES. Everything else this browser
draws is a function of the document; a form's values are not -- they are the
user's, they change under the keyboard, and they must survive a re-render of a
document that knows nothing about them. So they live in a table keyed by node
index, deliberately NOT in the DOM. That separation is what stops a re-render
from wiping a half-filled form, and it is why `value` is not simply another
attribute. The markup's `value=` seeds a field ONCE, on first use: re-seeding
every frame would fight the keyboard, and the field would appear to reject
input.

The rest is reuse rather than new machinery. A text field is EmUI's -- the
toolkit already owns editing, focus, the caret and the keyboard, so a control
is the browser handing it a buffer and getting typing back. A submission is a
NAVIGATION, so it goes down the same load path a link uses; POST differs only
in what goes on the wire. And `action` is parsed into the same slot as `href`,
because a form's target is a link target and resolves identically.

Correctness the tests pin down (11 assertions): `&`, `=`, `/` and `?` are
percent-encoded so a value cannot forge a second field; space becomes `+`; a
control with no NAME is not submitted, which is the HTML rule; a form REPLACES
its action's query rather than appending to it; POST/redirect/GET, so a 303
after a POST does not re-post the body.

Proven on the metal: two fields typed, Submit pressed, the address bar shows
`?who=vellum&what=hello+there`, and the page's own script reads
`location.search` back and prints `who = vellum | what = hello there`.

**B6 is done: the browser shows pictures, and the page does not jump.**

Sizing came second and matters as much as decoding. `width`/`height` from the
markup and `width`/`height`/`max-width` from CSS decide a picture's box in
cascade order, and an oversized image is scaled to the column with its ASPECT
PRESERVED rather than overflowing. When the size is known, the layout holds the
box open BEFORE the bytes arrive -- so the paragraph the reader is in the
middle of does not get shoved down when a picture lands.

That claim is checked rather than asserted: `make browser-render` lays the
document out twice, once with every picture outstanding and once with them
decoded, and compares where the text ended up. On a page whose images all state
their size: `last text y=716.3 before images -> y=716.3 after : NO REFLOW`.

Two things that check caught about itself, which is the useful part: it first
measured the LOWEST text in the window and kept reporting the same y on every
page -- that was the status bar, furniture that never moves, so the check could
not fail. And an image with NO stated size legitimately swaps alt text for a
picture, so the node count differs by design and only the Y is the verdict.
Unsized images still reflow, in this browser and in every other one; that is
what the width/height attributes are for. `user/web/png.c` decodes PNG on
top of the DEFLATE this OS already had (written for the package installer --
the reuse was the point of having it). Colour types 0/2/3/4/6, bit depths 1-16,
all five filters, PLTE and tRNS, straight to premultiplied BGRA so the decode
lands in the compositor's own format with no conversion pass. Interlaced PNG is
REFUSED rather than half-decoded.

`user/web/imgcache.c` is the other half, and the harder one: a document NAMES
images, it does not contain them, so each is another round trip after the HTML
is already complete. One fetch at a time on the worker the document uses, so
the window keeps drawing; pictures appear as they land; alt text stands in
until then, which is what alt text is for.

Two bugs worth remembering, both found on the metal:

- The document and the images share one worker, and `fetchjob_poll` REPORTED
  the owner but consumed the job regardless -- so the document's poll silently
  ate every image completion and pictures stayed "loading" forever with no
  error anywhere. Consumption is now per-owner.
- `render_inline_run` was a hand-unrolled three-level walk that only knew about
  text, so an `<img>` nested inside any inline wrapper was dropped in silence.
  It is recursive now, depth-bounded.

**B5 is done: the cascade is live.** `user/web/css/` -- three files because
there are three concerns that fail differently: `decl.c` (what a declaration
MEANS), `sel.c` (what a selector MATCHES and how strongly), `sheet.c` (the
CASCADE: order, not just winners). Origin order is user-agent < author <
inline, and specificity ties break on document order.

The prediction in §3 held exactly: CSS needed ONE parser change -- keep
`class`, `id`, `style`, and stop discarding `<style>` bodies. Nothing else
moved. The renderer still reads only `struct vstyle`; layout, the network
layer and the app are untouched.

Supported: type/class/id/universal selectors, compounds, the descendant
combinator, selector lists, comments, and the properties this renderer can
actually honour (color, font-weight/style/family/size, text-decoration,
display, the margin and padding shorthands). `>` `+` `~` parse as descendant
-- over-matching degrades a page, dropping the rule loses the author's intent
entirely. `@media` is DROPPED rather than misapplied, because a print
stylesheet applied to a screen rewrites the whole page.

Deliberately absent, and each for a reason rather than a shrug: no
percentages (they need a containing block this box model does not expose), no
floats or positioning (the layout engine has no such concept and pretending
otherwise is a lie that is harder to find than a missing feature), no
`!important`, no pseudo-elements.

CSS also exposed a real gap in the toolkit: `Title` and `Heading` were both
hardwired to the bold face, so `font-size: 19px` with no `font-weight` came
out bold. Size and weight are independent in CSS and must be in the roles too
-- hence `Subtitle`, the large regular face.

47 host assertions (`make html-test`), and metal-proven: the demo page renders
`200 file 1477 bytes 51 nodes 11 css rules`.

**B2 is done: Vellum is on the network.** The seam held -- `user/web/net.c`
appeared and one call in the app changed, nothing else. `vnet_fetch` is the one
entry point ("give me the bytes for this location"), so the address bar takes a
path and a URL without the user telling it which is which; `user/web/url.c` is
the pure string half, kept separate because deciding what a location MEANS
should not happen while holding a socket.

HTTP/1.0 with `Connection: close` on purpose, the same choice wget made: the
server closing the socket frames the body, so there is no chunked decoder and no
keep-alive state machine. Redirects are followed to a depth of 5 and the address
bar follows. Responses are bounded by the caller's buffer and truncation is
REPORTED, never silently grown into.

Proven live, on the metal, each with the status line as the witness:

- `200  file  1254 bytes  72 nodes` -- the local start page, now through the
  same one entry point.
- `200  http  698 bytes  38 nodes` -- a page served from the host, fetched over
  our virtio-net, ARP, IPv4 and TCP. The server's log shows our request:
  `"GET /hello.html HTTP/1.0" 200`.
- A relative link on that page resolved against a NETWORK base and navigated to
  `http://10.0.2.2:8000/second.html`.
- `200  https (authenticated)  4067 bytes  62 nodes` --
  `https://valid-isrgrootx1.letsencrypt.org/`, a real page off the actual
  internet, through our own TLS 1.3, our own X25519/AES-GCM/SHA-384 and our own
  X.509 chain verification to ISRG Root X1. Rendered by our own engine, in a
  window drawn by our own compositor, in a process holding exactly the three
  capabilities its manifest asked for.

One bug found and fixed in the SDK on the way: `emb_resolve` sent dotted-quad
LITERALS to DNS, so typing `http://10.0.2.2:8000/` failed with "cannot resolve
10.0.2.2" on a machine that could reach it perfectly well. Asking a name server
to resolve an address is the wrong question. Fixed in `emb_resolve` rather than
in the browser, because every caller wants it -- wget and gitclone had the same
gap. Host-checked against 13 cases first, including the ones that must NOT
short-circuit DNS (`1example.com`, `256.0.0.1`, `10.0.2.2x`).

## 12. Open questions

Recorded rather than assumed:

1. **Does EmUI's layout hold a long document?** §5. B3 measures; the fallback
   is a flat display list.
2. **Text selection and copy.** Not in the milestones. A reader you cannot
   copy from is half a reader — but it needs selection in the toolkit, which
   the Terminal also wants. Shared work, unscheduled.
3. **Fonts.** One face at several sizes. Real pages ask for families we do not
   have; the stylist will have to map families onto what exists.
4. **Encodings.** UTF-8 only. Latin-1 pages will mangle. Cheap to add when a
   real page forces it.
5. **The name.** Proposed here as Vellum; alternatives considered were EmbView
   (consistent with the tool naming but implies a system component) and Reader
   (honest but generic).


## JPEG, and the measurement that found the real cost

`user/web/jpeg.c` decodes baseline JPEG: Huffman codes rebuilt from a table of
code lengths, an inverse DCT, chroma upsampling, and the YCbCr conversion --
four algorithms sharing nothing with the PNG path, which is why it is its own
file. Progressive JPEG is a different decoder wearing the same extension and is
REFUSED rather than half-decoded, for the same reason interlaced PNG is. Which
decoder runs is chosen by the file's SIGNATURE, never its extension: a server is
free to send a JPEG from a path ending `.png`, and a browser that trusts the
name renders garbage.

Fifteen host assertions pin it against real encoder output -- 4:4:4, 4:2:0,
greyscale, a size that is not a multiple of 8, progressive-refused, and every
truncation of a valid file either refusing or decoding at the right size.

### The part worth writing down

A photograph took **21 seconds** to appear on the metal. The IDCT was the
obvious suspect -- it was written for clarity, with a float inner loop, and this
OS runs under TCG where SSE float is emulated in software. Rewriting it in fixed
point with a DC-only fast path made it 1.75x faster on the host.

On the metal it changed nothing: 21.1s became 20.2s.

So the decoder was instrumented instead of theorised about, and the answer was
not in the decoder at all:

    tot 4773   net 6   poll 4758   dec 96      (milliseconds, per image)

Six milliseconds to fetch, ninety-six to decode -- and **four and a half seconds
sitting finished**, waiting for the UI thread to come back and collect it. The
next measurement said one frame in 4.7 seconds, and the one after that said the
whole frame was `scene_render_frame`, and the one after that said the whole
render was `draw_rect`: 5262 ms across 136 rectangles.

`cpu_draw_rect` had no early-out for a fill that paints nothing. An invisible
rect still walked every pixel of its box -- clip coverage, a rounded-box SDF
with its `sqrtf`, paint sampling, a float blend -- and discarded the result at
the very end when the blend found alpha 0. It never showed in this OS's own
apps, where a box with no background simply is not emitted. **A document is the
opposite**: every block element gets a background whether or not the page names
one, so an html/body/div/p nest is a stack of full-width transparent rectangles,
each paying for a full-window float pass.

Two lines fixed it. Frame render went from **4946 ms to 170-483 ms**, the image
round trip from 4773 ms to 343 ms, and scrolling from about four seconds to
0.74 s.

Two further hoists landed with it, because the same shape was in three places.
The image blit and the solid-fill fast path both asked "is coverage trivial?" as
`no dirty region and no clips at all` -- but every scrollable view pushes a clip,
so in a document that scrolls the answer was permanently no and every interior
pixel paid a per-pixel predicate. Clips and damage rects are axis-aligned boxes,
so `box_fully_covered()` answers it ONCE per primitive; the glyph blit uses it
per glyph cell. And the image blit's source coordinates, which step linearly,
are a 16.16 accumulator instead of a float divide per pixel.

The lesson is the one this project keeps relearning: **measure, don't theorise.**
The first fix was aimed at a real inefficiency that was not the problem, and
only cost a day because the measurement was cheap once it was finally taken.


### The scroll blit was dead code, and the check could not tell

Step 1s in `scene_render.c` turns a wheel tick into a `memmove` plus a strip of
glyphs instead of a full repaint. It had a host check asserting the incremental
frame is pixel-identical to a from-scratch one, and that check read PIXEL-EXACT
from the day it was written -- because the blit was never being taken on a real
page, so the two renders agreed by being the same render.

The classifier asks whether any node moved and is still visible OUTSIDE the
scrolled clip, since that would mean the frame was not that clip's scroll. It
asked using each node's raw footprint. But a document taller than its viewport
ALWAYS has content scrolled past the bottom of its container -- clipped away,
staining nothing -- so every real page vetoed its own fast path.

`gather` now threads the inherited clip down the tree, so every node carries its
visible extent (footprint intersected with every clipping ancestor) and the
classifier reasons about that. A moved node that itself clips still aborts, since
its descendants' extents were computed against a clip that is in motion.

`make browser-render` now FAILS when the blit path is not exercised, and exits
non-zero. A check that passes because the code under test never ran is not a
check -- that is the property that hid this for as long as it was hidden.


## Selecting text, and taking it away

A browser you cannot copy out of is a browser you can only look at. Drag across
the page to select, `Ctrl+A` for the whole document, `Ctrl+C` to put it on the
system clipboard, `Esc` to drop it. Proven end to end on the metal: text
selected in Vellum, copied, and pasted into the shell in the Terminal with
`Ctrl+V`.

`user/web/select.c` does all of it AFTER layout, and that is the design. It
reads the laid-out scene to find where the words are and writes back a
background on the selected ones; `render.c` does not know the file exists. The
alternative -- having each emitter announce its own words -- would break
silently the first time one was forgotten, and render.c emits text from several
places (words, image alt text, list bullets, error strings). Reading the scene
cannot be forgotten.

The walk only counts nodes INSIDE a clipping node, which is the ScrollView
holding the document. Without that test it also picks up the address bar and the
status line, and dragging across the page selects the chrome -- the host check
asserts the copied text never contains the URL bar.

**Word granularity, deliberately.** The renderer already emits one node per word
(that is what keeps a link clickable on both sides of a line break), so the word
is the grain the document is already cut along. Character-level selection would
mean measuring glyph prefixes through the font engine on every pointer move. It
is a real limitation and it is in `docs/TODO.md`, not hidden.

Two things had to be true for the highlight to appear at all, and neither was:

- **`dirty` is not `dirty_content`.** The scene splits them so a pure transform
  reads as a MOVE rather than a repaint. The first version marked a highlight
  with `scene_mark_dirty`, which sets only `dirty` -- so the selection was
  applied to the scene, copied correctly, reported the right byte count, and
  never drew a single pixel.
- **Whoever sets a background owns removing it.** The declarative layer briefly
  cleared every run's background on each build, so each selected word was
  marked content-dirty twice a frame and a live selection became a full repaint
  per frame -- the exact cost the dirty-rect path exists to avoid. Now the build
  only ever SETS, and the post-layout pass paints the range and unpaints
  everything else. `scene_set_text_bg` is a no-op when the colour already
  matches, so the steady state marks nothing dirty: scrolling with a selection
  up costs the same 0.66s as scrolling without one.


## The CSS property set, and a layout bug it uncovered

Measuring against real pages said the vocabulary was the gap: eighteen
properties, none of which could paint anything. Added:
`background` / `background-color` (the shorthand is scanned for a colour rather
than dropped whole), `border` and its longhands, `border-radius`, real
`padding` (all four edges), `text-align`, `line-height`, and `rgb()`/`rgba()`
colours -- modern stylesheets write colours that way constantly, and a page
whose every colour is unparsed renders as if it had no CSS at all.

`padding` used to alias onto `margin`. That was harmless while nothing was
painted and stopped being harmless the moment a box could have a background:
padding sits inside the painted area, margin outside it.

**text-align turned out to be a layout-engine bug.** `arrange()`'s wrap arm
started every line at the padding edge and never consulted `justify`; the
non-wrap arm always had. Since a paragraph of text is exactly a wrapping row,
`text-align: center` in a stylesheet could not work on any page. Each line box
now justifies on its own -- which is what text-align means: centre every line,
not the paragraph as a block.

`line-height` is parsed and stored but layout does not use it yet.


## Hacker News, and the bug under it

HN turned 1307 DOM nodes into `1. by | 2. by |`. The DOM was perfect -- adding
a `DOM=1` dump to `make browser-render` showed every story title sitting in its
`<a>` exactly where it should be -- so the fault was entirely in emission, and
it had two halves.

`<center>` was not in the tag table, so it defaulted to INLINE. HN wraps its
whole page in one, which put a nested-table document inside an inline
formatting context. And `emit_inline` refused to recurse past depth 8. With
`<center><table><tr><td><table><tr><td><span><b><a>` being nine levels deep,
every link's text was dropped -- while the plain `" | "` between the links, one
level shallower, came through. That is why the page read as punctuation.

`<center>` is now a block that centres its text, which is what it means. And a
block-level box inside an inline run now ENDS the run and renders as a block,
which is what CSS says happens and what stops a table being walked as if it
were a sentence.

## The 64-child cap

Chasing an "overlapping first list item" on danluu.com led somewhere much
worse. `arrange()` gathered a container's children into `kids[64]` on the C
stack. Past sixty-four they were never positioned, kept their default 0x0, and
every one of them painted at the parent's origin -- so a hundred-item list drew
its first row correctly and piled the remaining thirty-six on top of it.

Sixty-four is generous for an application's dialog and nothing for a document.
The arrays could not simply be grown: `arrange()` recurses once per nesting
level, so 1024-entry stack arrays would be megabytes deep on a real page. They
now come from a shared bump pool that `arrange()` pops on exit, which is exactly
right because the walk is strictly depth-first.

The general lesson is the failure MODE, not the number. Both this and the
declarative instance pool silently produced a wrong page instead of saying they
had run out. Both now count what they refused, and `make browser-render` fails
on either -- because a renderer that quietly drops content is worse than one
that stops.


## The corpus

`make web-corpus` renders every page in `tests/web/` and checks what each page
claims about itself. A page states its expectations in HTML comments, next to
the markup they are about:

    <!-- EXPECT-COLOR: GRANDCHILD #ecedf1 -->
    <!-- EXPECT-LEFT-OF: 1. headline -->
    <!-- EXPECT-BELOW: LASTMARKER Item -->

Every check runs against the RESOLVED render -- each text run's position and
computed colour, dumped by `TEXTDUMP=1` -- not against the source, which is the
only way to test a cascade rather than a parser.

The pages are ours, not vendored copies of other people's sites: the point is
the SHAPE, and a shape can be reproduced without taking someone's page. Every
page exists for a bug that actually happened, and each was checked to FAIL when
that bug is put back -- re-introducing the depth-8 guard loses NESTEDLINK, and
re-introducing the 64-child cap makes long-list report 66 dropped containers.
A check that cannot fail has been the recurring hazard in this project; these
were verified against it before being trusted.

For a real site: fetch it outside the tree, `make web-corpus CORPUS=<dir>`, and
when it finds something, add a page here that reproduces the shape.


## Flex and grid (C2)

`display:flex` and `display:grid` from a stylesheet, with flex-direction,
flex-wrap, justify-content, align-items, gap, flex-grow and
grid-template-columns. Short, because the layout engine has done flex and grid
since it was written -- the whole toolkit is built on them -- so this is the
CSS spelling of machinery that already exists rather than new layout.

Two rules CSS states that a renderer does not get for free:

- **Element children of a flex container are blockified.** Without it,
  `<nav style="display:flex"><a>One</a><a>Two</a></nav>` merges its links into
  one inline run and therefore one item, so the gap and the justification apply
  to the whole strip instead of between the links.
- **Whitespace-only text makes no anonymous item.** A document is written with
  newlines between its tags; left in, the newline between two `<div>`s becomes
  a third grid cell and every card after it lands one column late.

### The bug that only appeared on the metal

The corpus said flex was correct. The metal disagreed: the first item of every
row ate the leftover space and shoved its siblings to the right edge.

`em_apply_box` calls `ui_set_size` only when a prop asks for one, so a widget
instance that is REUSED next frame keeps whatever size it was last given. A
harness renders one tree a few times and never notices. An app builds an empty
view first and the document second, and the declarative layer matches instances
BY POSITION across those two different trees -- so a box inherited `grow` from
whatever had occupied its slot before.

The browser now states every box's size out loud each frame, which is what
makes a retained tree behave like an immediate one. The general fix belongs in
`em_apply_box` and is left open, because every existing app is written against
the current behaviour.

Two Makefile bugs surfaced while chasing it, both of the same family as the
stale `vellum.elf`: hand-written header dependencies (`web_jsdom.o` did not
list `style.h`, so it kept an old `struct vstyle` layout inside a binary where
everything else had the new one). Dependencies are now generated by the
compiler with `-MMD -MP`.


## Percentages and the box model (C3, first half)

`width: 50%` cannot be turned into pixels when the stylesheet is read: the
containing block does not exist yet. So it travels through `struct vstyle` as a
percentage and the layout engine resolves it, via a new `SIZE_PERCENT` mode
whose `fixed_value` holds the fraction. Anywhere that mode is not handled it
falls through to intrinsic -- a sane degradation rather than a wrong number.

`vw`/`vh` read the same viewport a media query asks about, so the environment
has one definition instead of two copies that can disagree.

**box-sizing was the reverse of the expected job.** The layout engine subtracts
padding from a node's size to get its content box, which means a layout node
already IS a border box. `box-sizing: border-box` -- the thing nearly every
modern stylesheet sets globally -- was therefore free, and the CSS DEFAULT was
the broken case: under `content-box` a stated width is the CONTENT and the
border box is that much wider. `tests/web/sizing.html` puts the two side by
side: 200px content-box renders 240 wide, 200px border-box renders 200.

The corpus gained `EXPECT-X`, which pins a resolved position. That is what lets
a percentage be tested for the number it produced -- 22 + 30% of 896 = 290.8 --
rather than for not crashing; removing SIZE_PERCENT moves that run to x=82.5
and the check says so.


## Position, overflow and calc (C3, second half)

`position: relative` offsets a box after the flow has placed it, so its
siblings never notice -- that is the whole difference from absolute, and the
reason relative is safe to apply late. `absolute` and `fixed` leave the flow
through the layout engine's existing overlay path, which has been taught CSS's
insets: a stated edge pins that side, both edges on an axis give the size, and
neither leaves the box at the content origin sized to its content.

`overflow: hidden`, `auto` and `scroll` all CLIP. The difference between them
is a scrollbar this renderer does not draw on an arbitrary box; the clipping is
the part that changes the layout, and leaving it out is how an overflowing box
paints across the rest of the page.

`calc()` reduces to a LINEAR EXPRESSION -- `pct` percent of the containing
block plus `px` pixels -- because the percentage is against a block that does
not exist when the stylesheet is read. So `calc(100% - 240px)` travels as
width_pct 100 with a -240 pixel term, and layout finishes it. Precedence is the
real one; `calc(100% - 2 * 20px)` evaluated left to right is off by exactly one
gap, which looks like a rounding bug rather than a parser bug.

### A struct that grew in the middle

Adding `pct_px` to `struct layout_size` broke flex grow and shrink instantly,
because the struct is built with POSITIONAL initialisers in several files and
`sz_grow()`'s `1` landed in the new field instead of `flex_grow`. The field
moved to the end of the struct and the initialisers in `em.c` and `kit.c` were
made designated, so position stops mattering. The layout tests caught it in
seconds -- which is the argument for having them.


## float and clear

CSS floats take a box out of flow, pin it to one edge, and shorten the LINE
BOXES of everything that follows until something clears -- so text wraps around
a floated image and then reclaims the full width below it.

This renderer has no exclusion regions: an inline run is a wrapping row that
knows nothing about boxes beside it. So a float and the content that flows
beside it become an actual ROW -- `[float][the rest]`, or `[the rest][float]`
for `float: right` -- and `clear` ends the row.

That is exactly right for the two shapes floats are really used in: an image
with text beside it, and float-based columns. It is wrong in one visible way,
and the way is worth knowing: **the text never reclaims the full width below
the float.** It stays in its column, so a tall float beside a short paragraph
leaves a gap a real browser would fill.

Doing it properly means teaching the wrap arm about exclusion rects. It already
walks lines with a y cursor, so the algorithm is reachable; the hard part is
that the floats live in an ancestor block while the text is in a nested Flow,
which is a coordinate-space problem rather than an algorithmic one.

A host test had to be corrected rather than satisfied: it asserted that
`float:left` was *not* honoured, because it was written when that was true. Its
intent -- an unknown property is skipped rather than half-applied -- is
unchanged; the example moved to properties that are still genuinely beyond this
browser.


## The DOM a script builds, and events that bubble (C4, part 1)

`document.createElement` / `createTextNode` / `body`, `appendChild`,
`removeChild`, `remove`, `setAttribute`, `className` and `classList`. A page
that only reads its own markup is a document; one that creates nodes is an
application, and every framework written in the last fifteen years does it.

Created nodes come from the SAME arenas the parse used, so a script's nodes
live exactly as long as the document does and there is no second lifetime to
reason about. When an arena is full they FAIL and set `truncated` rather than
growing into a page's hands. A node cannot be appended into its own subtree --
every walker in this browser recurses without a visited set, so a cycle is not
a wrong picture, it is a hang.

Events bubble: `click`, `submit`, `input` and `change` fire on the node and
then on each ancestor, with `event.target` staying the node the event happened
on, `currentTarget` the one that is listening, and `stopPropagation()` ending
the walk. An event name this browser cannot deliver is still refused loudly.

### Two bugs of the same shape

Both made a feature look like it worked while being useless:

- `jsdom_has_listener` asked only about the node itself. A DELEGATED listener
  -- a handler on the `<ul>` rather than on every `<li>`, which is how most
  pages are written -- therefore left its children unclickable.
- `render_block` returned early for list items, images, tables and controls,
  all before the clickable-box code. So a click on an `<li>` inside a listening
  `<ul>` was consumed by the ul and arrived with `event.target` set to the ul.
  Delegation exists precisely so a handler can ask which item was clicked. The
  hit box now wraps every display path, and the innermost listening box
  consumes -- a child's box is closed before its parent's, so the deepest one
  asks first.

### The harness runs JavaScript now

`browser_render` links QuickJS and jsdom when they are available, and runs the
page's scripts exactly as the app does. Without that the corpus tested a
DIFFERENT document than the browser renders: a page that builds its own DOM
looked empty on the host and correct on the metal, which is the exact
divergence a two-second loop exists to prevent.


## Form controls, and the two gaps from part 1 (C4, part 2)

`preventDefault` is now a real veto on `submit`: the browser asks after
dispatching and does not navigate if a handler said no. That is how a page
validates a form, or submits it with `fetch()` itself, which is how most forms
on the modern web work.

`input` and `change` fire from a per-frame POLL of the value table rather than
a callback. The toolkit writes into a field's value buffer in place -- there is
no edit event to hook -- so the only way to know a field changed is to have
kept what it used to say. Both gaps were logged in the previous commit rather
than left silent, which is what made them easy to close.

**Checkbox, radio and select.** A boolean control keeps a stable bool per node,
because the toolkit binds a pointer to it; the FORM's copy is still the source
of truth for submission, and the working copy is synced in before the control
draws and out straight after, so a click is visible to `form_submit` in the
same frame it happened. Radios clear their group by `name` -- the whole
difference between a radio and a round checkbox. A `<select>` is its `<option>`
children, and an `<option>` is `display: none` so it cannot leak into the page
as stray text.

`<textarea>` still renders as a single-line field. It submits correctly and it
does not look right; a multi-line control needs the toolkit to grow one.


## Cookies

A cookie is how a site remembers you between two requests. Without one, every
page load is a stranger arriving -- no login survives a click. It is also the
first state this browser keeps that belongs to a SERVER rather than to the
document, which is why it lives in `user/web/cookie.c` and is scoped by host
rather than by page.

Sent on every request whose host and path match, taken from `Set-Cookie` before
a redirect is followed (that hop carries the session the next request needs),
and exposed as `document.cookie` with HttpOnly cookies excluded -- which is the
entire security value of that flag.

The scoping rules are where the security is, so they are the tests:

- a suffix match must fall on a dot, or `evil-example.com` claims
  `example.com`'s cookies;
- `/app` must not match `/applesauce`;
- a host cannot set a cookie for a domain it does not belong to;
- Secure stays off a plain connection;
- `Max-Age=0` deletes, which is how logout works.

One of those tests found a real bug: **a cookie with no `Domain` must be
host-only.** Defaulting it to domain-scoped quietly widens every cookie a site
sets, handing a session set on `example.com` to any subdomain -- including one
an attacker controls. Stating a `Domain` is how a site opts INTO sharing.

Proven on the metal against a real server: two cookies stored from one
response, and the next request came back `SERVERSAW sid=SESSION42; pref=dark`.

The jar is not persisted -- a session survives navigation but not a restart.
That is a deliberate stopping point: writing cookies to disk means deciding
where, deciding who else may read them, and deciding when they expire on a
machine whose clock may not have been set. This OS's answer to all three should
be the capability system, not a path hard-coded here.


## State that outlives the process

The jar is persisted now, and so is localStorage. The three questions the
previous section raised got answers rather than a shrug:

**Where:** `$HOME/.vellum`, and the browser may name nothing else. `vellum.ns`
used to be comments only -- which means INHERIT the whole session namespace --
so writing the grant down made the browser MORE confined than it was when it
could not persist at all.

It is `$HOME` and not `/data/apps/vellum` because the session grants
`ro /data/apps` deliberately: an application rewriting its own installed files
is what a package manager exists to prevent. A browser's cookies are the user's
data anyway, not the program's. That needed a new `$HOME` token in the manifest
format -- an app cannot hard-code a user name, and on a machine with two users
there are two answers -- which the launcher expands to whoever is logged in.

**Who may read it:** whoever can name that directory, which is the session's own
tree -- the same boundary that already separates one user's documents from
another's.

**When things expire:** by the wall clock, and only when there is one. An unset
clock reads as "no opinion" everywhere in this browser rather than as 1970, so a
machine whose RTC was never set does not silently throw away every saved session
on boot.

`Expires` is now a real RFC 1123 date with the leap-year rule, checked to the
second against independently computed values. Session cookies -- the ones with
no expiry -- are deliberately NOT saved: they are defined to end with the
browsing session, and writing them to disk would make "session" mean something
the user never agreed to.

Proven across a reboot on one disk: `SERVERSAW keeper=LIVES`, with the session
cookie correctly absent.

### Two things that made this take longer than it should have

A `.ns` prefix had to start with `/`, and that check ran BEFORE `$HOME` was
expanded -- so the grant vanished from the manifest with no error anywhere, and
the app started with one binding fewer than it asked for. A manifest that
silently drops a line it does not understand is the same hazard as a check that
cannot fail.

And a granted prefix must EXIST at spawn time, because the kernel resolves it
in the parent's namespace. An app cannot create what it has not been granted,
and cannot be granted what does not exist; `mkfs` ships `$HOME/.vellum` to break
the cycle. That wants a real answer before a second app hits it.


## Find in page (C5)

Ctrl+F opens a bar with a live count, Prev/Next that wrap, Enter for next and
Esc to close. Every match is highlighted, and the CURRENT one in a different
colour -- without that, "next" moves an indicator you cannot see, which is the
one thing find-in-page has to show.

`find.c` reuses the runs `select.c` already collected rather than walking the
scene a second time. Two walks would be two chances to disagree about what
counts as page text and what counts as chrome, and the first bug that produces
is "find highlights the address bar".

Matching is case-insensitive and **spans runs**, which is the whole difficulty:
the renderer emits one box per WORD, so "operating system" is two boxes with a
space baked into the first, and a matcher confined to a single run fails on
every phrase anyone actually searches for. The page is flattened into one
string with a run index per byte, and each hit maps back to the boxes it covers.

### The count was a frame behind

The host reported 3 matches for "browser" and the metal showed **1 of 24**. The
view draws `find_count()` before the post-layout hook rescans, so the number is
always one frame behind the query -- and with nothing requesting another frame
it froze showing the count for a shorter prefix. 24 is the answer for "b".

The harness is what made that obvious: `FIND=<text> browser_render` prints the
count, so the matcher could be checked independently of the UI drawing it. On a
screenshot you only ever see the highlight, never the number.


## History, and a page the browser writes itself

Back and forward were never history: they are a stack that dies with the
window. `history.c` keeps the list you consult when you cannot remember what a
page was called -- newest first, one entry per URL, and revisiting MOVES a row
rather than adding a second, because a history where an address appears forty
times is one you cannot read.

It is shown as `about:history`, a page the browser writes and then parses with
its own engine. That is why there is no history widget anywhere in the app: a
list of links is a page, and this program already knows how to draw one. It also
means the history is styled by the same cascade, selectable by the same
selection, and searchable by the same find. Titles are HTML-escaped, because a
title comes off the network and this is a document the browser vouches for.

### The link colour bug it exposed

The history page set `body { color: ... }` and every link went grey. That was
not a bug in the page: an inherited colour was outranking the user-agent's link
colour, so ANY page setting a body colour -- which is most pages -- lost all its
link colouring. CSS gives the UA's `a` rule precedence over inheritance, and
only a rule naming the link itself beats the UA. `vstyle` now records whether a
colour was set on the element or inherited, and `tests/web/selectors.html` pins
all three cases.

### Why tabs are not here

A tab needs its own parse arenas AND its own JS context, and `jsdom` is built
around one global `g_ctx`/`g_doc`. Implementing tabs by re-parsing on switch
would re-run every page's scripts, so a tab would silently lose its state every
time you left it -- a feature that looks right and behaves wrongly. It wants
jsdom made multi-instance first, and that is worth doing on its own.
