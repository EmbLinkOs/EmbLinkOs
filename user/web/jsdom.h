/* user/web/jsdom.h -- the DOM, as JavaScript sees it.
 *
 * This is the file docs/BROWSER.md §9 was waiting for. An engine that cannot
 * touch the document is an interpreter that computes 2+2; the value is in the
 * bindings, and the bindings presuppose everything this browser already has --
 * a parsed document, a stylist, a layout, a renderer and a fetch.
 *
 * The surface is SMALL and deliberately so. Every binding here is one the
 * renderer can honour immediately: query the tree, read and write text, read
 * attributes. A binding that accepts a call and does nothing observable is
 * worse than a missing one, because a script author cannot tell the two apart
 * -- the same rule the CSS declaration parser follows.
 *
 * There is exactly ONE document: the tree a script mutates is the same
 * `struct html_doc` the renderer walks. No shadow copy, no synchronisation
 * question, and a mutation is visible on the next frame by construction.
 */
#ifndef _EMBLINK_WEB_JSDOM_H_
#define _EMBLINK_WEB_JSDOM_H_

struct html_doc;
struct css_sheet;

/* HAVE_JSDOM is defined only when QuickJS (QJS_SRC) is present, which is also
 * the only case where jsdom.c is compiled. Without it the real prototypes are
 * replaced by the no-op stubs at the bottom of this file -- see the note
 * there. Declaring both would be a static-follows-extern conflict, so the two
 * halves are mutually exclusive rather than additive. */
#ifdef HAVE_JSDOM

/* Create a runtime + context bound to `doc`. Returns 0, or -1 if the engine
 * could not be created. One per document: a new page gets a new world, which
 * is also how a script cannot outlive the page that wrote it. */
int  jsdom_open(struct html_doc *doc, const struct css_sheet *sheet);

/* The page's own address, for `location`. Set BEFORE jsdom_open. */
void jsdom_set_url(const char *url);

/* Run every <script> the document carried, in order. Returns the number that
 * threw -- a script that fails must not stop the ones after it, exactly as in
 * a browser, because one broken analytics tag should not blank a page. */
int  jsdom_run_scripts(void);

/* Evaluate one snippet (the console, or a javascript: link). Any output goes
 * through the same console hook. Returns 0, or -1 if it threw. */
int  jsdom_eval(const char *src, const char *name);

/* Did a script CHANGE the document since the last time this was asked? The app
 * uses it to decide whether to re-render, so an idle script costs nothing. */
int  jsdom_take_dirty(void);

/* ---- events -------------------------------------------------------------
 * A script that runs once at load and can never respond is a template
 * engine. These three calls are what make it a program: the renderer asks
 * which elements care, tells us when one is clicked, and lets timers run.
 */

/* Does this node have a click listener? The RENDERER asks, so that only
 * elements a script actually cares about become clickable -- a page where
 * every <div> swallows clicks is a page whose links stop working. */
int  jsdom_has_listener(int node);

/* Deliver a click. Returns 1 if a handler ran (and may have mutated the
 * document -- check jsdom_take_dirty afterwards). */
int  jsdom_dispatch_click(int node);
/* A form was submitted. Bubbles like click, and returns 1 if a handler called
 * preventDefault -- in which case the caller must NOT navigate. That is what
 * lets a page validate, or submit with fetch() itself. */
int  jsdom_dispatch_submit(int node);
/* A field's value changed: fires 'input' then 'change', both bubbling. */
int  jsdom_dispatch_input(int node);

/* Drive everything the engine owes the page: due timers, a landed fetch, and
 * the MICROTASK QUEUE. Returns 1 if anything ran. Call once a frame.
 *
 * The microtask drain is the part that is easy to leave out and fatal to omit:
 * QuickJS queues `.then` callbacks as pending jobs, so a promise that resolves
 * is a promise whose handlers never run until someone executes them. Without
 * it `fetch(...).then(...)` is a call that succeeds and does nothing -- the
 * exact failure this browser refuses to ship. */
int  jsdom_pump(unsigned long long now_ms);

/* When is the next timer due, or 0 if none? The app uses it to decide whether
 * to keep asking for frames -- a page with no timers must cost nothing. */
unsigned long long jsdom_next_timer(void);

/* Is the engine waiting on anything (a fetch in flight, a promise unsettled)?
 * The app keeps frames coming while this is true. */
int  jsdom_busy(void);

/* Tear down. Safe to call when never opened. */
void jsdom_close(void);

/* Where console.log goes. Set by the app so the browser can show a script's
 * output instead of dropping it. `line` is NUL-terminated and transient. */
void jsdom_set_console(void (*fn)(const char *line));
/* How many listeners were declined for an event type this browser does not
 * deliver. A note for whoever is building the browser: the reader of the page
 * cannot act on it, so it does not belong on their status line. */
int  jsdom_declined_listeners(void);

#else /* !HAVE_JSDOM */
/* ---- no engine in this build -------------------------------------------
 * QuickJS is an OPTIONAL port (QJS_SRC), and without it build/web_jsdom.o is
 * never compiled -- so every call above would be an undefined symbol at link
 * time and the whole browser would fail to build over a port that is supposed
 * to be skippable. These no-ops are what make "absent" mean absent rather than
 * broken, the same bargain the other ports make (docs/BUILD_SETUP.md).
 *
 * This does NOT violate the rule at the top of this file -- that a binding
 * which accepts a call and does nothing observable is worse than a missing
 * one. That rule is about lying to a SCRIPT AUTHOR. Here there is no script
 * author, because there is no engine to run their script: the page renders as
 * a no-script page, which is a state the web has always defined and which the
 * reader can see. The dishonest version would be shipping an engine that
 * silently drops what it cannot do.
 *
 * jsdom_open() returns -1 -- "the engine could not be created" -- which is the
 * answer its own contract already specifies and which vellum.c already
 * handles. Nothing else here needs a special case.
 */
static inline int  jsdom_open(struct html_doc *d, const struct css_sheet *s)
                                              { (void)d; (void)s; return -1; }
static inline void jsdom_set_url(const char *u)          { (void)u; }
static inline int  jsdom_run_scripts(void)               { return 0; }
static inline int  jsdom_eval(const char *src, const char *name)
                                              { (void)src; (void)name; return -1; }
static inline int  jsdom_take_dirty(void)                { return 0; }
static inline int  jsdom_has_listener(int n)             { (void)n; return 0; }
static inline int  jsdom_dispatch_click(int n)           { (void)n; return 0; }
/* 0 == nobody called preventDefault, so the caller navigates -- a form still
 * submits without JS, which is the behaviour a no-script page must have. */
static inline int  jsdom_dispatch_submit(int n)          { (void)n; return 0; }
static inline int  jsdom_dispatch_input(int n)           { (void)n; return 0; }
static inline int  jsdom_pump(unsigned long long now_ms) { (void)now_ms; return 0; }
static inline unsigned long long jsdom_next_timer(void)  { return 0; }
static inline int  jsdom_busy(void)                      { return 0; }
static inline void jsdom_close(void)                     { }
static inline void jsdom_set_console(void (*fn)(const char *line)) { (void)fn; }
static inline int  jsdom_declined_listeners(void)        { return 0; }
#endif /* HAVE_JSDOM */

#endif /* _EMBLINK_WEB_JSDOM_H_ */
