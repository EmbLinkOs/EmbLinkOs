/* user/web/find.h -- find in page.
 *
 * Ctrl+F. It reuses the runs select.c already collected rather than walking the
 * scene again -- two walks would be two chances to disagree about what is page
 * text and what is chrome, and the first bug that produces is "find highlights
 * the address bar".
 *
 * Matches are case-insensitive and SPAN RUNS, which is the whole difficulty:
 * the renderer emits one run per word, so "operating system" is two boxes, and
 * a matcher confined to a single run fails on every phrase anyone searches for.
 */
#ifndef _EMBLINK_WEB_FIND_H_
#define _EMBLINK_WEB_FIND_H_

void        find_open(void);
void        find_close(void);
int         find_is_open(void);

/* What to look for. Setting it clears the current position, so a new query
 * starts from the first match rather than wherever the last one ended. */
void        find_set_needle(const char *s);
const char *find_needle(void);

/* Move to the next (+1) or previous (-1) match; both WRAP. Returns 1 if there
 * was anywhere to go. */
int         find_step(int delta);

/* How many matches, and which one is current (1-based, 0 = none) -- the
 * "3 of 17" a person reads to know whether to keep pressing. */
int         find_count(void);
int         find_current(void);

/* The y of the current match, so the caller can scroll it into view. */
int         find_current_y(float *out_y);

/* Re-mark every hit. Install as select.c's mark hook: runs are cleared each
 * frame, so this is the whole of "showing" a find. */
void        find_mark(void);
void        find_rescan(void);

#endif /* _EMBLINK_WEB_FIND_H_ */
