/* ==========================================================================
 * edit_test.c -- the line editor against the real screen model.
 *
 * Every byte the editor writes is fed to user/terminal/vt.c, the model the
 * Terminal itself draws from, and every reply the model makes (the size
 * report DECSET 2048 asks for) goes back to the editor -- so these check what
 * a person would SEE: the text on each row and where the cursor is, through
 * wrapping, wide and combining characters, completion menus, search, history,
 * paste, resize and the console's backspace-only screen.
 *
 * Build: make shell-test. Exit code = number of failures.
 * ========================================================================== */
#include "edit/edit.h"
#include "hist/hist.h"
#include "vt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("  ok   %s\n", name); \
    else      { printf("  FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

static struct vt term;
static struct editor *ed;
static char replies[256];
static size_t nreplies;
static enum edit_result last;
static bool use_home = true;     /* the EmbLink Terminal's OSC 133;R, or relative motion */

static void to_screen(void *ctx, const char *b, size_t n) { (void)ctx; vt_feed(&term, b, n); }
static void from_screen(void *ctx, const char *b, size_t n) {
    (void)ctx;
    if (nreplies + n <= sizeof replies) { memcpy(replies + nreplies, b, n); nreplies += n; }
}
static bool unfinished(void *ctx, const char *t) {
    (void)ctx;
    int depth = 0;
    for (const char *p = t; *p; p++) depth += *p == '{' ? 1 : *p == '}' ? -1 : 0;
    size_t n = strlen(t);
    while (n && t[n - 1] == ' ') n--;
    return depth > 0 || (n && t[n - 1] == '|');
}
static const char *words[] = { "echo", "env", "exit", "src/", "sed" };
static void complete(void *ctx, const char *line, size_t cur, struct edit_matches *m) {
    (void)ctx;
    size_t start = cur;
    while (start && line[start - 1] != ' ') start--;
    m->start = start;
    for (size_t i = 0; i < sizeof words / sizeof *words; i++)
        if (!strncmp(words[i], line + start, cur - start))
            edit_matches_add(m, words[i], words[i], words[i][strlen(words[i]) - 1] == '/');
}

/* The terminal's replies reach the editor first, as they would on a PTY. */
static void keys_n(const char *s, size_t n) {
    if (nreplies) {
        size_t r = nreplies;
        nreplies = 0;
        char copy[256];
        memcpy(copy, replies, r);
        edit_feed(ed, copy, r, 0);
    }
    size_t used = 0;
    last = edit_feed(ed, s, n, &used);
}
static void keys(const char *s) { keys_n(s, strlen(s)); }
static const char *row(int y) {
    static char text[1024];
    vt_line_text(&term, y, 0, text, sizeof text);
    return text;
}
static int cx(void) { return vt_active(&term)->cursor.x; }
static int cy(void) { return vt_active(&term)->cursor.y; }

static void start(int cols, int rows, enum edit_mode mode) {
    static bool live;
    if (live) vt_free(&term);
    live = vt_init(&term, cols, rows, from_screen, NULL);
    nreplies = 0;
    struct edit_prompt p = { "\033]133;A;edit=shell;cwd=/home\007", "\033[1;36m$\033[0m ", "\033[2m>\033[0m ",
                             "\033]133;B\007", use_home ? "\033]133;R\007" : NULL, NULL, NULL };
    if (mode == EDIT_DUMB) { p.mark = p.after = p.home = NULL; p.text = "$ "; p.more = "> "; }
    edit_begin(ed, &p, mode, cols);
}

static void test_typing(void) {
    printf("typing and moving:\n");
    start(40, 8, EDIT_VT);
    CHECK(!strcmp(row(0), "$") && cx() == 2 && cy() == 0, "the prompt, then the cursor after it");
    CHECK(term.inband_resize && term.bracketed_paste, "in-band resize and bracketed paste are on at the prompt");
    CHECK(term.edit_shell && term.prompt_row == 0, "the prompt is marked, and says the shell edits its line");
    keys("echo hi");
    CHECK(!strcmp(row(0), "$ echo hi") && cx() == 9, "typed text appears after the prompt");
    keys("\033[D\033[DX");
    CHECK(!strcmp(row(0), "$ echo Xhi") && cx() == 8, "Left twice, then text inserted AT the cursor");
    keys("\001");
    CHECK(cx() == 2, "Ctrl+A: to the start");
    keys("\005");
    CHECK(cx() == 10, "Ctrl+E: to the end");
    keys("\x7f\x7f");
    CHECK(!strcmp(row(0), "$ echo X"), "Backspace twice");
    keys("\033b");
    CHECK(cx() == 7, "Alt+B: a word back");
    keys("\033[3~");
    CHECK(!strcmp(row(0), "$ echo"), "Delete: the character under the cursor");
    keys("\033[1;5D");
    CHECK(cx() == 2, "Ctrl+Left: a word back");
    keys("\033[1;5C");
    CHECK(cx() == 6, "Ctrl+Right: a word forward");
}

static void test_kill_yank_undo(void) {
    printf("kill, yank, undo:\n");
    start(40, 8, EDIT_VT);
    keys("ls -la /tmp");
    keys("\027");
    CHECK(!strcmp(row(0), "$ ls -la"), "Ctrl+W kills the word before the cursor");
    keys("\027");
    CHECK(!strcmp(row(0), "$ ls"), "Ctrl+W again");
    keys("\031");
    CHECK(!strcmp(row(0), "$ ls -la /tmp"), "Ctrl+Y yanks both kills back as one");
    keys("\001\013");
    CHECK(!strcmp(row(0), "$"), "Ctrl+A Ctrl+K kills the line");
    keys("\037");
    CHECK(!strcmp(row(0), "$ ls -la /tmp"), "Ctrl+_ undoes the kill");
    keys("\005 one two");
    keys("\037");
    CHECK(!strcmp(row(0), "$ ls -la /tmp one"), "undo takes back the last word typed, not one key");
    keys("\024");
    CHECK(!strcmp(row(0), "$ ls -la /tmp oen"), "Ctrl+T transposes the two characters before the cursor");
    keys("\001\033u");
    CHECK(!strcmp(row(0), "$ LS -la /tmp oen"), "Alt+U upcases a word");
}

static void test_wrapping(void) {
    printf("wrapping, wide and combining characters:\n");
    start(10, 8, EDIT_VT);
    keys("abcdefghijklmnop");                     /* 2 + 16 cells: two rows */
    CHECK(!strcmp(row(0), "$ abcdefgh") && !strcmp(row(1), "ijklmnop"), "text wraps at the margin");
    CHECK(cx() == 8 && cy() == 1, "the cursor follows onto the second row");
    keys("\033[D\033[D\033[D\033[D\033[D\033[D\033[D\033[D\033[D");
    CHECK(cx() == 9 && cy() == 0, "Left across the wrap lands on the first row's last cell");
    keys("\005");
    CHECK(cx() == 8 && cy() == 1, "and End returns");
    start(10, 8, EDIT_VT);
    keys("abcdefgh");                              /* exactly fills the first row */
    CHECK(cx() == 0 && cy() == 1, "a full row puts the cursor at the next row's start");
    keys("Z");
    CHECK(!strcmp(row(1), "Z") && cx() == 1, "and the next character goes there");
    start(20, 8, EDIT_VT);
    keys("\xe6\xbc\xa2\xe5\xad\x97 e\xcc\x81!");   /* 漢字 é! */
    CHECK(cx() == 2 + 4 + 1 + 1 + 1, "wide characters take two cells, a combining mark none");
    keys("\033[D\033[D");
    CHECK(cx() == 7, "Left steps over the letter and its accent together");
    keys("\033[D\033[D");
    CHECK(cx() == 4, "and over a wide character in one step");
}

static void test_multiline(void) {
    printf("multi-line programs:\n");
    start(40, 8, EDIT_VT);
    keys("if true {\r");
    CHECK(last == EDIT_MORE && !strcmp(row(1), ">") && cy() == 1 && cx() == 2, "Enter on an open block starts another line");
    keys("echo in\r}\r");
    CHECK(last == EDIT_LINE && !strcmp(edit_line(ed), "if true {\necho in\n}"), "the closed block is one program");
    CHECK(!strcmp(row(0), "$ if true {") && !strcmp(row(1), "> echo in") && !strcmp(row(2), "> }"),
          "each line shown with its prompt");
    CHECK(cy() == 3 && cx() == 0, "the cursor ends on a fresh row for the output");
    start(40, 8, EDIT_VT);
    keys("ls |\r");
    CHECK(last == EDIT_MORE, "a trailing pipe waits for the rest");
    keys("count\r");
    CHECK(last == EDIT_LINE && !strcmp(edit_line(ed), "ls |\ncount"), "and takes it");
    start(40, 8, EDIT_VT);
    keys("one\033\rtwo");
    CHECK(!strcmp(row(1), "> two"), "Alt+Enter adds a line to a complete program");
    keys("\033[A");
    CHECK(cy() == 0 && cx() == 5, "Up moves within the program first, keeping the column");
}

static void test_history(void) {
    printf("history:\n");
    hist_reset();
    hist_push("git status");
    hist_push("ls");
    hist_push("git log");
    start(40, 8, EDIT_VT);
    keys("par");
    keys("\033[A");
    CHECK(!strcmp(row(0), "$ git log"), "Up recalls the newest");
    keys("\033[A\033[A");
    CHECK(!strcmp(row(0), "$ git status"), "and older");
    keys("\033[B\033[B\033[B");
    CHECK(!strcmp(row(0), "$ par"), "Down past the newest restores what was being typed");
    keys("\025git");
    keys("\033[5~");
    CHECK(!strcmp(row(0), "$ git log") && cx() == 5, "PgUp finds the newest line starting with what is typed");
    keys("\033[5~");
    CHECK(!strcmp(row(0), "$ git status"), "and the one before it, skipping lines that do not match");
    keys("\033[6~");
    CHECK(!strcmp(row(0), "$ git log"), "PgDn goes forward again");

    start(40, 8, EDIT_VT);
    keys("\022");
    CHECK(!strcmp(row(1), "search history:"), "Ctrl+R opens the search line");
    keys("sta");
    CHECK(!strcmp(row(0), "$ git status") && !strcmp(row(1), "search history: sta"), "typing searches as it goes");
    CHECK(cx() == 6, "the cursor sits on the match");
    keys("q");
    CHECK(!strcmp(row(1), "failing search: staq"), "a query with no match says so");
    keys("\033");
    nreplies = 0;
    last = edit_escape_timeout(ed);
    CHECK(!strcmp(row(0), "$") && !strcmp(row(1), ""), "Escape restores the line and closes the search");
    keys("\022g\022");
    CHECK(!strcmp(row(0), "$ git status"), "Ctrl+R again goes to an older match");
    keys("\r");
    CHECK(last == EDIT_LINE && !strcmp(edit_line(ed), "git status"), "Enter runs the match");
}

static void test_completion(void) {
    printf("completion:\n");
    start(40, 8, EDIT_VT);
    keys("ec\t");
    CHECK(!strcmp(row(0), "$ echo"), "one match completes, with a space after it");
    CHECK(cx() == 7, "and the cursor after the space");
    keys("\025sr\t");
    CHECK(!strcmp(row(0), "$ src/") && cx() == 6, "a directory completes with '/' and no space");
    keys("\025e\t");
    CHECK(!strcmp(row(0), "$ e") && strstr(row(1), "echo") && strstr(row(1), "env") && strstr(row(1), "exit"),
          "several matches with nothing in common: the menu");
    keys("\t");
    CHECK(!strcmp(row(0), "$ echo"), "Tab chooses the first");
    keys("\t");
    CHECK(!strcmp(row(0), "$ env"), "Tab again, the next");
    keys("\033[Z");
    CHECK(!strcmp(row(0), "$ echo"), "Shift+Tab goes back");
    keys("\r");
    CHECK(last == EDIT_MORE && !strcmp(row(0), "$ echo") && !strcmp(row(1), ""), "Enter takes the choice and closes the menu");
    keys("\r");
    CHECK(last == EDIT_LINE && !strcmp(edit_line(ed), "echo "), "and the next Enter runs the line");
    start(40, 8, EDIT_VT);
    keys("e\t\t\033");
    nreplies = 0;
    last = edit_escape_timeout(ed);
    CHECK(!strcmp(row(0), "$ e") && !strcmp(row(1), ""), "Escape puts back what was typed");
    start(40, 8, EDIT_VT);
    keys("s\t");
    CHECK(!strcmp(row(0), "$ s") && strstr(row(1), "src/") && strstr(row(1), "sed"),
          "matches sharing only the typed prefix open the menu at once");
    keys("e");
    CHECK(!strcmp(row(0), "$ se") && !strcmp(row(1), ""), "typing closes the menu and goes on");
}

static void test_control(void) {
    printf("Ctrl+C, Ctrl+D, paste, typeahead:\n");
    start(40, 8, EDIT_VT);
    keys("half a thought\003");
    CHECK(last == EDIT_CANCEL && !strcmp(row(0), "$ half a thought^C") && cy() == 1, "Ctrl+C abandons the line, visibly");
    CHECK(!term.inband_resize && !term.bracketed_paste, "the prompt's modes are off again before anything runs");
    start(40, 8, EDIT_VT);
    keys("x\033[D\004");
    CHECK(last == EDIT_MORE && !strcmp(row(0), "$"), "Ctrl+D on text deletes the character under the cursor");
    keys("\004");
    CHECK(last == EDIT_EOF, "Ctrl+D on an empty line is the end of input");
    start(40, 8, EDIT_VT);
    keys("\033[200~first line\r\nsecond\tline\033[201~");
    CHECK(last == EDIT_MORE && !strcmp(row(0), "$ first line") && !strcmp(row(1), "> second line"),
          "pasted newlines are newlines in the program, not Enter; a tab is a space");
    start(40, 8, EDIT_VT);
    size_t used = 0;
    const char *ahead = "ls\rpwd\r";
    last = edit_feed(ed, ahead, strlen(ahead), &used);
    CHECK(last == EDIT_LINE && used == 3 && !strcmp(edit_line(ed), "ls"), "typeahead: the feed stops after the line");
    start(40, 8, EDIT_VT);
    keys("\033");
    CHECK(edit_pending_escape(ed), "a lone ESC waits to see whether a sequence follows");
    keys("[D");
    CHECK(!edit_pending_escape(ed), "and the rest of an arrow completes it");
}

static void test_resize(void) {
    printf("resize:\n");
    start(20, 8, EDIT_VT);
    keys("printf hello world from here");           /* 2 + 28 cells: two rows at 20 */
    CHECK(cy() == 1, "two rows at 20 columns");
    /* What the Terminal does on a resize while the shell edits: erase the
     * prompt, resize, report in-band; the editor redraws from the origin. */
    CHECK(vt_prompt_erase(&term), "the terminal erases the prompt");
    CHECK(!strcmp(row(0), "") && cx() == 0 && cy() == 0, "and goes to where it began");
    CHECK(vt_resize(&term, 12, 8), "the screen narrows to 12 columns");
    vt_prompt_here(&term);
    char report[48];
    size_t n = vt_size_report(&term, report);
    keys_n(report, n);
    CHECK(!strcmp(row(0), "$ printf hel") && !strcmp(row(1), "lo world fro") && !strcmp(row(2), "m here"),
          "the line is redrawn wrapped at the new width");
    CHECK(!strcmp(row(3), ""), "with no leftover copy of the old prompt");
    CHECK(cy() == 2 && cx() == 6 && edit_cols(ed) == 12, "the cursor where the text ends");
    if (!use_home) return;
    /* The race: a key's redraw still on its way when the terminal erases and
     * resizes. It returns to the prompt's start first, so it lands right, and
     * the redraw after the report does too -- no second copy anywhere. */
    vt_prompt_erase(&term);
    vt_resize(&term, 16, 8);
    vt_prompt_here(&term);
    keys("!");
    n = vt_size_report(&term, report);
    keys_n(report, n);
    CHECK(!strcmp(row(0), "$ printf hello w") && !strcmp(row(1), "orld from here!") && !strcmp(row(2), ""),
          "a redraw already on its way when the window changed lands where the prompt is");
}

/* A prompt with a right side and a transient form, and one built for the width. */
static void start_rich(int cols, const char *right, const char *transient) {
    vt_free(&term);
    vt_init(&term, cols, 8, from_screen, NULL);
    nreplies = 0;
    struct edit_prompt p = { "\033]133;A;edit=shell;cwd=/home\007", "\033[1;36m$\033[0m ", "\033[2m>\033[0m ",
                             "\033]133;B\007", use_home ? "\033]133;R\007" : NULL, right, transient };
    edit_begin(ed, &p, EDIT_VT, cols);
}
static char g_wide[64];
static void wide_prompt(void *ctx, int cols, struct edit_prompt *p) {
    (void)ctx;
    snprintf(g_wide, sizeof g_wide, "[%d]$ ", cols);
    p->text = g_wide;
    p->right = cols >= 20 ? "R" : NULL;
}

static void test_right_transient(void) {
    printf("the right prompt and the transient prompt:\n");
    start_rich(30, "\033[2m12:00:00\033[0m", "\033[32m>\033[0m ");
    const char *r = row(0);
    CHECK(!strncmp(r, "$", 1) && strstr(r, "12:00:00") == r + 21 && cx() == 2,
          "the right side against the edge, one column in; the cursor where the typing starts");
    keys("echo hi");
    CHECK(!strncmp(row(0), "$ echo hi", 9) && strstr(row(0), "12:00:00") && cx() == 9, "typing leaves it while there is room");
    keys(" abcdefghij");                             /* the typing ends at column 20 */
    CHECK(strstr(row(0), "12:00:00") != NULL, "a column between them is still room");
    keys("k");
    CHECK(!strstr(row(0), "12:00:00") && !strcmp(row(0), "$ echo hi abcdefghijk"), "one more and it goes");
    keys("\x7f");
    CHECK(strstr(row(0), "12:00:00") != NULL, "and comes back when there is room again");
    keys("\r");
    CHECK(last == EDIT_LINE && !strcmp(row(0), "> echo hi abcdefghij") && cy() == 1,
          "accepted: the prompt is drawn the short way, without its right side");
    start_rich(30, "R", NULL);
    keys("\r");
    CHECK(!strncmp(row(0), "$", 1) && row(0)[28] == 'R' && cy() == 1,
          "no transient prompt: the line keeps its prompt and its right side, as zsh does");

    /* a prompt built for the width, built again when it changes */
    struct editor *saved = ed;
    struct edit_host host = { to_screen, complete, unfinished, NULL, wide_prompt };
    ed = edit_new(&host);
    vt_free(&term);
    vt_init(&term, 24, 8, from_screen, NULL);
    nreplies = 0;
    wide_prompt(NULL, 24, &(struct edit_prompt){0});
    struct edit_prompt p = { "\033]133;A;edit=shell;cwd=/home\007", g_wide, "> ", "\033]133;B\007",
                             use_home ? "\033]133;R\007" : NULL, "R", NULL };
    edit_begin(ed, &p, EDIT_VT, 24);
    keys("ls");
    CHECK(!strncmp(row(0), "[24]$ ls", 8) && row(0)[22] == 'R', "the prompt for 24 columns");
    vt_prompt_erase(&term);
    vt_resize(&term, 16, 8);
    vt_prompt_here(&term);
    char report[48];
    size_t n = vt_size_report(&term, report);
    keys_n(report, n);
    CHECK(!strcmp(row(0), "[16]$ ls") && edit_cols(ed) == 16, "and built again for 16: its text, and no right side");
    edit_free(ed);
    ed = saved;
}

/* Powerlevel10k's two lines: the editor redraws from the first, puts the
 * right side on the row the typing is on, and the transient prompt takes the
 * place of both. */
static void test_two_lines(void) {
    printf("a prompt of two lines:\n");
    vt_free(&term);
    vt_init(&term, 30, 8, from_screen, NULL);
    nreplies = 0;
    to_screen(NULL, "before\r\n", 8);
    struct edit_prompt p = { "\033]133;A;edit=shell;cwd=/home\007", "\r\n\033[38;5;238m\xe2\x95\xad\xe2\x94\x80\033[39m ~"
                             "          12:00:00\r\n\033[38;5;238m\xe2\x95\xb0\xe2\x94\x80\033[38;5;76m\xe2\x9d\xaf\033[39m ",
                             "> ", "\033]133;B\007", use_home ? "\033]133;R\007" : NULL, "R", "\033[38;5;76m\xe2\x9d\xaf\033[39m " };
    edit_begin(ed, &p, EDIT_VT, 30);
    CHECK(!strcmp(row(0), "before") && !strcmp(row(1), "") && !strncmp(row(2), "\xe2\x95\xad\xe2\x94\x80 ~", 7)
          && !strncmp(row(3), "\xe2\x95\xb0\xe2\x94\x80\xe2\x9d\xaf", 9) && cy() == 3 && cx() == 4,
          "an empty line, the first line, and the typing after the second");
    /* ╰─❯ is three cells in nine bytes: R ends the row at column 28 */
    CHECK(row(3)[strlen(row(3)) - 1] == 'R' && strlen(row(3)) - 6 == 29,
          "the right side on the second line, against the edge one column in");
    keys("ls -l");
    CHECK(!strcmp(row(1), "") && strstr(row(2), "12:00:00") && !strncmp(row(3) + 9, " ls -l", 6) && cy() == 3,
          "typing redraws both lines in place");
    keys("\r");
    CHECK(!strcmp(row(1), "\xe2\x9d\xaf ls -l") && !strcmp(row(2), "") && !strcmp(row(3), "") && cy() == 2,
          "accepted: the transient prompt takes the empty line's place, the rest is cleared");
}

static void test_dumb(void) {
    printf("the console (no escapes):\n");
    hist_reset();
    hist_push("first command");
    start(60, 6, EDIT_DUMB);
    CHECK(!strcmp(row(0), "$") && cx() == 2, "a plain prompt");
    keys("echo world");
    keys("\x11\x11\x11\x11\x11");                  /* the console's Left */
    keys("big ");
    CHECK(!strcmp(row(0), "$ echo big world") && cx() == 11, "inserting mid-line rewrites the tail with backspaces");
    keys("\x7f");
    CHECK(!strcmp(row(0), "$ echo big orld"), "the console's Delete key deletes forward");
    keys("\x13");                                   /* the console's Up */
    CHECK(!strcmp(row(0), "$ first command") && cx() == 15, "history replaces the whole line");
    keys("\b\b\b\b\b\b\b");
    CHECK(!strcmp(row(0), "$ first") && cx() == 8, "Backspace blanks what it removes");
    keys("\n");
    CHECK(last == EDIT_LINE && !strcmp(edit_line(ed), "first ") && cy() == 1, "Enter accepts");
}

int main(void) {
    struct edit_host host = { to_screen, complete, unfinished, NULL, NULL };
    ed = edit_new(&host);
    if (!ed) { puts("no memory"); return 1; }
    test_typing();
    test_kill_yank_undo();
    test_wrapping();
    printf("-- the same, with relative cursor motion (a terminal without OSC 133;R):\n");
    use_home = false;
    test_typing();
    test_wrapping();
    test_multiline();
    test_resize();
    use_home = true;
    printf("-- and back to the EmbLink Terminal:\n");
    test_multiline();
    test_history();
    test_completion();
    test_control();
    test_resize();
    test_right_transient();
    test_two_lines();
    use_home = false;
    printf("-- the right and transient prompts, with relative cursor motion:\n");
    test_right_transient();
    test_two_lines();
    use_home = true;
    test_dumb();
    edit_free(ed);
    printf(g_fail ? "edit: %d FAILED\n" : "edit: all passed\n", g_fail);
    return g_fail;
}
