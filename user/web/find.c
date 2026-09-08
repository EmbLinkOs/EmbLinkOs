/* user/web/find.c -- find in page.
 *
 * Ctrl+F, and the thing a person does to a long document before they read any
 * of it. It reuses the runs select.c already collected rather than walking the
 * scene again: two walks would be two chances to disagree about what counts as
 * page text and what counts as chrome, and the first bug that produced would be
 * "find highlights the address bar".
 *
 * Matching is CASE-INSENSITIVE and spans runs. That second part is the whole
 * difficulty: the renderer emits one run per WORD, so "operating system" is two
 * boxes with a space baked into the first, and a matcher that only looked
 * inside a single run would fail on every phrase anyone actually searches for.
 * So the page is matched as one flattened string, and each hit is mapped back
 * to the runs it covers.
 */
#include <string.h>

#include "find.h"
#include "select.h"

#define FIND_MAX_HITS 128
#define FLAT_MAX      (64 * 1024)

static char  g_needle[96];
static int   g_hit_first[FIND_MAX_HITS];   /* first run of each match */
static int   g_hit_last[FIND_MAX_HITS];
static int   g_nhit;
static int   g_current = -1;
static int   g_open;

/* the page as one string, plus which run each byte came from */
static char  g_flat[FLAT_MAX];
static int   g_owner[FLAT_MAX];
static int   g_flatn;

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

int  find_is_open(void) { return g_open; }
int  find_count(void)   { return g_nhit; }
int  find_current(void) { return g_nhit ? g_current + 1 : 0; }
const char *find_needle(void) { return g_needle; }

void find_close(void) { g_open = 0; g_nhit = 0; g_current = -1; g_needle[0] = 0; }
void find_open(void)  { g_open = 1; }

/* Flatten every run into one lowercase string, remembering which run each byte
 * belongs to so a hit can be mapped back to the boxes it covers. */
static void flatten(void) {
    g_flatn = 0;
    int n = vsel_run_count();
    for (int i = 0; i < n && g_flatn < FLAT_MAX - 1; i++) {
        const char *t = vsel_run_text(i);
        if (!t) continue;
        for (const char *p = t; *p && g_flatn < FLAT_MAX - 1; p++) {
            g_flat[g_flatn] = lower(*p);
            g_owner[g_flatn] = i;
            g_flatn++;
        }
    }
    g_flat[g_flatn] = 0;
}

void find_set_needle(const char *s) {
    size_t i = 0;
    for (; s && s[i] && i < sizeof g_needle - 1; i++) g_needle[i] = s[i];
    g_needle[i] = 0;
    g_nhit = 0;
    g_current = -1;
}

/* Rebuild the hit list against the page as it is NOW. Called once a frame from
 * the mark hook, because the document can change under a script and a hit list
 * that outlives its runs points at boxes that have moved. */
void find_rescan(void) {
    g_nhit = 0;
    if (!g_open || !g_needle[0]) return;
    flatten();

    char needle[sizeof g_needle];
    size_t nl = 0;
    for (; g_needle[nl] && nl < sizeof needle - 1; nl++) needle[nl] = lower(g_needle[nl]);
    needle[nl] = 0;
    if (!nl) return;

    for (int i = 0; i + (int)nl <= g_flatn && g_nhit < FIND_MAX_HITS; ) {
        if (!memcmp(g_flat + i, needle, nl)) {
            g_hit_first[g_nhit] = g_owner[i];
            g_hit_last[g_nhit]  = g_owner[i + (int)nl - 1];
            g_nhit++;
            i += (int)nl;                 /* no overlapping matches */
        } else i++;
    }
    if (g_nhit == 0) g_current = -1;
    else if (g_current < 0 || g_current >= g_nhit) g_current = 0;
}

/* Mark every hit, and the current one differently. Runs are re-marked every
 * frame -- select.c clears them -- so this is the whole of "showing" a find. */
void find_mark(void) {
    find_rescan();
    for (int h = 0; h < g_nhit; h++) {
        int kind = (h == g_current) ? 2 : 1;
        for (int r = g_hit_first[h]; r <= g_hit_last[h]; r++) vsel_run_mark(r, kind);
    }
}

int find_step(int delta) {
    if (g_nhit <= 0) return 0;
    g_current += delta;
    /* WRAPS. A find that stops at the last match makes you re-open it to check
     * the top of the page, which is exactly when you are least sure. */
    if (g_current >= g_nhit) g_current = 0;
    if (g_current < 0)       g_current = g_nhit - 1;
    return 1;
}

int find_current_y(float *out_y) {
    if (g_current < 0 || g_current >= g_nhit) return 0;
    float x, y, w, h;
    if (vsel_run_rect(g_hit_first[g_current], &x, &y, &w, &h) != 0) return 0;
    if (out_y) *out_y = y;
    return 1;
}
