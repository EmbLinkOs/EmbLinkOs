/* user/web/history.h -- where you have been.
 *
 * Back and forward already existed, and they are not history: they are a stack
 * that dies with the window. History is the list you consult when you cannot
 * remember what the page was called, which means it has to outlive the process
 * -- so it rides the same persistence, the same directory and the same three
 * answers as the cookie jar (see store.h).
 *
 * Newest first, one entry per URL. Revisiting a page MOVES it to the top
 * rather than adding a second row: a history where the same address appears
 * forty times is a history you cannot read, which is the only thing it is for.
 */
#ifndef _EMBLINK_WEB_HISTORY_H_
#define _EMBLINK_WEB_HISTORY_H_

#include <stddef.h>

/* Record a visit. `title` may be empty -- a page without one is listed by its
 * URL, which is still better than not listing it. */
void        hist_add(const char *url, const char *title, unsigned long long when);

int         hist_count(void);
const char *hist_url(int i);
const char *hist_title(int i);
unsigned long long hist_when(int i);

void        hist_clear(void);

/* Persistence, through store.c's blob interface. */
int         hist_save(void);
int         hist_load(void);

/* Render the list as an HTML document, into `out`. The browser shows its own
 * history by PARSING this with its own engine -- which is why there is no
 * history widget anywhere in the app: a list of links is a page, and this
 * browser already knows how to draw one. */
size_t      hist_as_html(char *out, size_t cap);

#endif /* _EMBLINK_WEB_HISTORY_H_ */
