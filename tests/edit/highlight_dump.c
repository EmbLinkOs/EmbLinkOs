/* highlight_dump -- the shell's line editor with its own highlighter, drawn by
 * the Terminal's screen model, for tools/golden_highlight.py to hold against
 * zsh-syntax-highlighting and zsh-autosuggestions.
 *
 *   highlight_dump COLS [-h LINE]... KEYS...
 *
 * -h puts LINE in the history (oldest first). Each KEYS is what is typed, as
 * the terminal sends it. The prompt is "$ ", in the directory it is run in.
 * JSON on stdout: the prompt's row as [text, fg, bold, underline] cells (fg
 * the palette index, null for the default) and the cursor, its row counted
 * from the prompt's. */
#include "edit/edit.h"
#include "hist/hist.h"
#include "complete/highlight.h"
#include "eval/eval.h"
#include "vt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The executor is not in the host build; no command is defined. */
size_t exec_user_command_count(void) { return 0; }
const char *exec_user_command_name(size_t i) { (void)i; return NULL; }

static struct vt term;
static char replies[256];
static size_t nreplies;

static void to_screen(void *ctx, const char *b, size_t n) { (void)ctx; vt_feed(&term, b, n); }
static void from_screen(void *ctx, const char *b, size_t n) {
    (void)ctx;
    if (nreplies + n <= sizeof replies) { memcpy(replies + nreplies, b, n); nreplies += n; }
}

static void json_text(const char *s) {
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: highlight_dump COLS [-h LINE]... KEYS...\n"); return 2; }
    int cols = atoi(argv[1]), i = 2;
    for (; i + 1 < argc && !strcmp(argv[i], "-h"); i += 2) hist_push(argv[i + 1]);
    if (!vt_init(&term, cols, 6, from_screen, NULL)) return 3;
    struct scope top;
    scope_init(&top, NULL);
    struct edit_host host = { to_screen, NULL, NULL, &top, NULL, shell_highlight };
    struct editor *ed = edit_new(&host);
    struct edit_prompt prompt = { NULL, "$ ", "> ", NULL, NULL, NULL, NULL };
    edit_begin(ed, &prompt, EDIT_VT, cols);
    int y0 = vt_active(&term)->cursor.y;
    enum edit_result r = EDIT_MORE;
    for (; i < argc && r == EDIT_MORE; i++) {
        if (nreplies) {                        /* the terminal's answers come first, as on a PTY */
            char copy[256];
            size_t n = nreplies;
            memcpy(copy, replies, n);
            nreplies = 0;
            edit_feed(ed, copy, n, NULL);
        }
        r = edit_feed(ed, argv[i], strlen(argv[i]), NULL);
    }
    const struct vt_screen *s = vt_active(&term);
    printf("{\"cursor\":[%d,%d],\"row\":[", s->cursor.x, s->cursor.y - y0);
    for (int x = 0; x < cols; x++) {
        const struct vt_cell *c = vt_view_cell(&term, y0, x, 0);
        if (x) putchar(',');
        if (!c->width) { printf("null"); continue; }
        char text[64];
        vt_cell_text(&term, c, text);
        const struct vt_style *st = vt_style_of(&term, c);
        putchar('[');
        json_text(text);
        if (VT_COLOR_KIND(st->fg) == 1) printf(",%u", st->fg & 0xff);
        else printf(",null");
        printf(",%s,%s]", st->attrs & VT_BOLD ? "true" : "false", st->underline ? "true" : "false");
    }
    printf("]}\n");
    edit_free(ed);
    vt_free(&term);
    return r == EDIT_MORE || r == EDIT_LINE ? 0 : 1;
}
