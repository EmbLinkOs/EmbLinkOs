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
| Images | PNG/JPEG decoding is a later milestone; `<img>` reserves its box from the start. |
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
| **B5** | Inline `style=`, then `<style>` + selectors | a styled page looks like the author meant |
| **B6** | `<img>`: PNG via our zlib, sized boxes | a page with pictures |
| **B7** | (open) JavaScript, by porting | — |

**B1 through B3 are the ones that decide whether this is real.** If a
document renders and scrolls at a usable speed, everything after is addition.

---

## 11. Open questions

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
