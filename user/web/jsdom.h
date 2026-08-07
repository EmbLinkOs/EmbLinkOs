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

/* Create a runtime + context bound to `doc`. Returns 0, or -1 if the engine
 * could not be created. One per document: a new page gets a new world, which
 * is also how a script cannot outlive the page that wrote it. */
int  jsdom_open(struct html_doc *doc, const struct css_sheet *sheet);

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

/* Tear down. Safe to call when never opened. */
void jsdom_close(void);

/* Where console.log goes. Set by the app so the browser can show a script's
 * output instead of dropping it. `line` is NUL-terminated and transient. */
void jsdom_set_console(void (*fn)(const char *line));

#endif /* _EMBLINK_WEB_JSDOM_H_ */
