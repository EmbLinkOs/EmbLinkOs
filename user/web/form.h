/* user/web/form.h -- form state, and what submitting one means.
 *
 * A form is the first part of the web that is not READ-ONLY, and that makes it
 * structurally different from everything else this browser does. Text, styles,
 * images and tables are all functions of the document; a form's values are
 * not. They are the user's, they change under the keyboard, and they must
 * survive re-renders of a document that knows nothing about them.
 *
 * So the values live HERE, in a table keyed by node index, and not in the DOM.
 * The DOM is the page the server sent; this is what the person typed. Keeping
 * them apart is what stops a re-parse from wiping a half-filled form, and it
 * is why `value` is not simply another attribute.
 *
 * Bounded like everything a stranger's markup can grow: a fixed number of
 * fields, a fixed size each. A page with more inputs than that gets the first
 * N working and the rest inert -- which is a page, rather than a refusal.
 */
#ifndef _EMBLINK_WEB_FORM_H_
#define _EMBLINK_WEB_FORM_H_

#include <stddef.h>

#define FORM_MAX_FIELDS 32
#define FORM_VALUE_MAX 256

struct html_doc;

/* Forget every value. Called when a new document loads, so one page's typing
 * never appears in the next. */
void form_reset(void);

/* The editable buffer for `node`, created on first use. NULL when the table is
 * full. The RENDERER writes into this through the toolkit's text field, so it
 * must be stable for the life of the page -- which is why it is a table entry
 * and not a per-frame allocation. */
char *form_value(struct html_doc *doc, int node);

/* Read without creating -- for a script asking `el.value` of a field the user
 * has not touched, which must not consume a slot. Returns "" if unset. */
const char *form_peek(int node);

/* Set programmatically (a script, or an initial `value=` attribute). */
int form_set(struct html_doc *doc, int node, const char *v);

/* Build the submission for the form containing `node` (a submit button, or the
 * form itself). Writes the resolved URL into `url` and, for POST, the body
 * into `body`. Returns 1 for GET, 2 for POST, or 0 if there is nothing to
 * submit. `base` resolves a relative action, exactly as a link is resolved. */
/* The node index of a field whose value has changed since this was last
 * called, or -1. Call in a loop until it returns -1 -- more than one field can
 * change between two frames (a paste, a script setting several).
 *
 * A POLL rather than a callback because the toolkit writes into the value
 * buffer in place: there is no edit event to hook, so the only way to know is
 * to have kept what the field used to say. */
int form_take_changed(void);

int form_submit(struct html_doc *doc, int node, const char *base,
                char *url, size_t url_cap, char *body, size_t body_cap);

#endif /* _EMBLINK_WEB_FORM_H_ */
