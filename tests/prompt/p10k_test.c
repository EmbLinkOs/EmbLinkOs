/* p10k_test.c -- the prompt engine's pieces the recordings do not reach:
 * the zsh a .p10k.zsh is written in, as the importer reads it, and segment
 * states no recorded session passes through. tools/golden_prompt.py holds
 * whole prompts to Powerlevel10k itself; this holds the parts. */
#include "prompt/p10k_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, what) do { if (cond) printf("  ok   %s\n", what); \
    else { printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); g_fail++; } } while (0)

static struct p10k *cfg;
static struct p10k_import rep;

static bool load(const char *text) {
    p10k_clear(cfg);
    return p10k_import(cfg, text, &rep);
}
static bool is(const char *name, const char *want) {
    const char *v = p10k_get(cfg, name);
    return v && !strcmp(v, want);
}

static void test_import(void) {
    printf("the importer:\n");
    CHECK(load("typeset -g POWERLEVEL9K_MODE=nerdfont-v3\n") && is("MODE", "nerdfont-v3") && rep.set == 1,
          "typeset -g POWERLEVEL9K_X=value: X, without the prefix");
    CHECK(load("  typeset -g POWERLEVEL9K_{LEFT,RIGHT}_{LEFT,RIGHT}_WHITESPACE=  # none\n") &&
          is("LEFT_LEFT_WHITESPACE", "") && is("RIGHT_RIGHT_WHITESPACE", "") && is("LEFT_RIGHT_WHITESPACE", "") && rep.set == 4,
          "brace lists, every combination; a comment after the value");
    CHECK(load("typeset -g POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(\n  # line one\n  dir   # here\n  vcs\n  newline\n  prompt_char\n)\n") &&
          is("LEFT_PROMPT_ELEMENTS", "dir\037vcs\037newline\037prompt_char"), "an array across lines, its comments left out");
    CHECK(load("typeset -g POWERLEVEL9K_A=(x)\ntypeset -g POWERLEVEL9K_A+=(y z)\ntypeset -g POWERLEVEL9K_E=()\n") &&
          is("A", "x\037y\037z") && is("E", ""), "+= appends; () is set and empty");
    CHECK(load("typeset -g POWERLEVEL9K_S='a b $x'\ntypeset -g POWERLEVEL9K_D=\"q \\\"$POWERLEVEL9K_S\\\"\"\n"
               "typeset -g POWERLEVEL9K_C=$'\\u276F\\n'\n") &&
          is("S", "a b $x") && is("D", "q \"a b $x\"") && is("C", "\xe2\x9d\xaf\n"),
          "'...' as written, \"...\" with $ and \\\", $'...' decoded");
    CHECK(load("() {\n  local grey=242\n  local -a files=(.git .hg)\n  typeset -g POWERLEVEL9K_DIR_FOREGROUND=$grey\n"
               "  typeset -g POWERLEVEL9K_SHORTEN_FOLDER_MARKER=\"(${(j:|:)files})\"\n}\n") &&
          is("DIR_FOREGROUND", "242") && is("SHORTEN_FOLDER_MARKER", "(.git|.hg)") && !p10k_get(cfg, "grey"),
          "locals and ${(j:|:)array}; a local is not a parameter");
    CHECK(load("typeset -g POWERLEVEL9K_G=' '\nif [[ $POWERLEVEL9K_G != ' ' ]]; then\n  typeset -g POWERLEVEL9K_A=1\n"
               "elif [[ -n $POWERLEVEL9K_G ]]; then\n  typeset -g POWERLEVEL9K_A=2\nelse\n  typeset -g POWERLEVEL9K_A=3\nfi\n") &&
          is("A", "2"), "if / elif / else on parameters already set");
    CHECK(load("typeset -g POWERLEVEL9K_X=a*\nif [[ $POWERLEVEL9K_X == a* ]]; then typeset -g POWERLEVEL9K_Y=glob; fi\n"
               "if [[ $POWERLEVEL9K_X == 'a*' ]] && [[ -z $nothing ]]; then typeset -g POWERLEVEL9K_Z=literal; fi\n") &&
          is("Y", "glob") && is("Z", "literal"), "unquoted: a pattern; quoted: the text; && between conditions");
    CHECK(load("function prompt_example() {\n  typeset -g POWERLEVEL9K_NO=1\n  p10k segment -t 'x}'\n}\n"
               "[[ -o aliases ]] || typeset -g POWERLEVEL9K_NOR=1\n(( 1 )) && typeset -g POWERLEVEL9K_NOA=1\n"
               "typeset -g POWERLEVEL9K_YES=1\n") &&
          !p10k_get(cfg, "NO") && !p10k_get(cfg, "NOR") && !p10k_get(cfg, "NOA") && is("YES", "1"),
          "functions skipped; commands joined by && or || are zsh's business");
    CHECK(load("function my_git_formatter() {\n  emulate -L zsh\n  if [[ -n $P9K_CONTENT ]]; then\n    local meta='%9F'\n  fi\n"
               "  if (( $1 )); then\n    local       meta='%f'\n    local      clean='%76F'\n  else\n"
               "    local      clean='%244F'\n  fi\n}\n") &&
          is("_GIT_FORMAT_META", "%f") && is("_GIT_FORMAT_CLEAN", "%76F") && is("_GIT_FORMAT_LOADING_CLEAN", "%244F"),
          "my_git_formatter: its colours, ready and loading");
    CHECK(load("function my_git_formatter() {\n  local      clean='%0F'\n}\n") && is("_GIT_FORMAT_CLEAN", "%0F") &&
          !p10k_get(cfg, "_GIT_FORMAT_LOADING_CLEAN"), "one set of colours (rainbow's): the ready ones");
    CHECK(load("typeset -g POWERLEVEL9K_A=1\nunset -m '(POWERLEVEL9K_*|DEFAULT_USER)~POWERLEVEL9K_GITSTATUS_DIR'\n"
               "typeset -g POWERLEVEL9K_B=1\n") && !p10k_get(cfg, "A") && is("B", "1") && rep.set == 1,
          "unset -m 'POWERLEVEL9K_*': what came before is gone");
    CHECK(load("typeset -g POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(dir direnv vcs_joined kubecontext)\n"
               "typeset -g POWERLEVEL9K_RIGHT_PROMPT_ELEMENTS=(time direnv)\n") && !strcmp(rep.ignored, "direnv kubecontext"),
          "segments that draw nothing here are reported, once each");
    CHECK(!load("typeset -g POWERLEVEL9K_A=1\n\ntypeset -g POWERLEVEL9K_B='open\n") && rep.line >= 3 &&
          strstr(rep.error, "unterminated"), "an unterminated quote: an error with its line");
    CHECK(!load("if [[ x == x ]]; then\n") && strstr(rep.error, "if without fi"), "an if without fi");
    CHECK(!load("}\n") && strstr(rep.error, "} without {"), "a } without {");
}

/* A state for one prompt, home /h, in `pwd`. */
static struct p10k_state state(const char *pwd) {
    struct p10k_state st = { 0 };
    st.pwd = pwd; st.home = "/h"; st.user = "yves"; st.host = "emblink.local";
    st.status = 0; st.duration = -1; st.os = "emblink";
    st.hour = 9; st.minute = 5; st.second = 7;
    return st;
}
static char *visible(const char *s) {          /* SGR removed */
    static char out[4096];
    size_t n = 0;
    for (; *s && n + 1 < sizeof out; s++) {
        if (*s == '\033') { while (*s && *s != 'm') s++; if (!*s) break; continue; }
        out[n++] = *s;
    }
    out[n] = 0;
    return out;
}
static char *render(const struct p10k_state *st, int cols, char *right, char *transient) {
    static char left[4096];
    struct p10k_prompt p;
    if (!p10k_render(cfg, st, cols, &p)) return NULL;
    snprintf(left, sizeof left, "%s", visible(p.left));
    if (right) snprintf(right, 512, "%s", visible(p.right));
    if (transient) snprintf(transient, 512, "%s", visible(p.transient));
    p10k_prompt_free(&p);
    return left;
}

static void test_segments(void) {
    printf("segments the recordings do not reach:\n");
    char right[512], tr[512];
    load("typeset -g POWERLEVEL9K_MODE=ascii\ntypeset -g POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(context dir)\n"
         "typeset -g POWERLEVEL9K_RIGHT_PROMPT_ELEMENTS=(status)\ntypeset -g POWERLEVEL9K_LEFT_SEGMENT_SEPARATOR=\n"
         "typeset -g POWERLEVEL9K_RIGHT_SEGMENT_SEPARATOR=\ntypeset -g POWERLEVEL9K_DIR_CLASSES=()\n"
         "typeset -g POWERLEVEL9K_STATUS_EXTENDED_STATES=true\ntypeset -g POWERLEVEL9K_TRANSIENT_PROMPT=same-dir\n");
    struct p10k_state st = state("/h/src");
    char *l = render(&st, 80, right, tr);
    CHECK(l && strstr(l, "yves@emblink ") && strstr(l, "~/src"), "context: %n@%m (the host to its first dot), then ~/src");
    st.status = 130;
    render(&st, 80, right, tr);
    CHECK(strstr(right, "SIGINT(2)") != NULL, "status 130: SIGINT(2) (VERBOSE_SIGNAME's default)");
    p10k_set(cfg, "STATUS_VERBOSE_SIGNAME", "false");
    render(&st, 80, right, tr);
    CHECK(strstr(right, "INT") && !strstr(right, "SIG"), "and INT without it");
    p10k_set(cfg, "STATUS_ERROR_SIGNAL", "false");
    render(&st, 80, right, tr);
    CHECK(!strstr(right, "INT"), "STATUS_ERROR_SIGNAL=false: a signal is not shown");
    CHECK(!tr[0], "transient same-dir: no transient prompt right after a cd");
    st.last_prompt_pwd = "/h/src";
    render(&st, 80, right, tr);
    CHECK(strstr(tr, "\xe2\x9d\xaf") != NULL, "and one once the directory stays");
    p10k_set(cfg, "_DEFAULT_USER", "yves");
    l = render(&st, 80, right, tr);
    CHECK(strstr(l, " yves ") && !strstr(l, "yves@"), "context: DEFAULT_USER shows the user alone");
    p10k_set(cfg, "CONTEXT_ROOT_TEMPLATE", "%n!");
    p10k_set(cfg, "ALWAYS_SHOW_CONTEXT", "true");
    st.root = true;
    l = render(&st, 80, right, tr);
    CHECK(strstr(l, "yves!") != NULL, "ALWAYS_SHOW_CONTEXT, and root: CONTEXT_ROOT_TEMPLATE");
    st.root = false;

    p10k_set(cfg, "DIR_CLASSES", "~/work(|/*)\037WORK\037W>\037*\037\037");
    p10k_set(cfg, "DIR_WORK_FOREGROUND", "196");
    st = state("/h/work/emb");
    struct p10k_prompt p;
    p10k_render(cfg, &st, 80, &p);
    CHECK(strstr(visible(p.left), "W> ~/work/emb") && strstr(p.left, "38;5;196;44m"),
          "DIR_CLASSES: ~/work(|/*) matched, its class's colour and icon");
    p10k_prompt_free(&p);
    st = state("/elsewhere");
    l = render(&st, 80, NULL, NULL);
    CHECK(strstr(l, "/elsewhere") && !strstr(l, "W>"), "and * for the rest, with no icon");

    p10k_set(cfg, "SHORTEN_STRATEGY", "truncate_to_last");
    st = state("/h/a/b/c");
    l = render(&st, 80, NULL, NULL);
    CHECK(strstr(l, " c ") && !strstr(l, "~/a"), "truncate_to_last: the last part alone");
    p10k_unset(cfg, "SHORTEN_STRATEGY");
    p10k_set(cfg, "HOME_FOLDER_ABBREVIATION", "@");
    l = render(&st, 80, NULL, NULL);
    CHECK(strstr(l, "@/a/b/c") != NULL, "HOME_FOLDER_ABBREVIATION for ~");
}

static void test_lines(void) {
    printf("lines, links and icons the recordings do not reach:\n");
    load("typeset -g POWERLEVEL9K_MODE=nerdfont-v3\n"
         "typeset -g POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(os_icon dir newline prompt_char)\n"
         "typeset -g POWERLEVEL9K_RIGHT_PROMPT_ELEMENTS=(time)\ntypeset -g POWERLEVEL9K_DIR_CLASSES=()\n"
         "typeset -g POWERLEVEL9K_DIR_HYPERLINK=true\n"
         "typeset -g POWERLEVEL9K_MULTILINE_FIRST_PROMPT_GAP_CHAR='-'\n"
         "typeset -g POWERLEVEL9K_MULTILINE_FIRST_PROMPT_GAP_EXPANSION='[${P9K_GAP}]'\n");
    struct p10k_state st = state("/h/a b");
    struct p10k_prompt p;
    p10k_render(cfg, &st, 40, &p);
    CHECK(strstr(p.left, "\033]8;;file:///h/a%20b\a") && strstr(p.left, "\033]8;;\a"),
          "DIR_HYPERLINK: the directory a link to itself, its URL escaped");
    const char *line1 = visible(p.left);
    CHECK(strstr(line1, "[--") && strstr(line1, "-]") && strstr(line1, "09:05:07"), "GAP_EXPANSION: ${P9K_GAP} the filler");
    CHECK(!strstr(line1, "\xef\x85\xbc"), "os_icon: nothing on an OS p10k does not know");
    p10k_prompt_free(&p);
    st.os = "linux";
    p10k_render(cfg, &st, 40, &p);
    CHECK(strstr(p.left, "\xef\x85\xbc") != NULL, "and Linux's on Linux (nerdfont-v3: U+F17C)");
    p10k_prompt_free(&p);
}

static void test_zsh(void) {
    printf("zsh's patterns and prompt escapes:\n");
    CHECK(p9k_glob("~/work(|/*)", 11, "~/work", 6) && p9k_glob("~/work(|/*)", 11, "~/work/x", 8) &&
          !p9k_glob("~/work(|/*)", 11, "~/workshop", 10), "(a|b) groups, an empty alternative");
    CHECK(p9k_glob("*[^[:alnum:]]wip", 16, "fix: wip", 8) && !p9k_glob("*[^[:alnum:]]wip", 16, "fixwip", 6),
          "classes, negated, [:alnum:]");
    CHECK(p9k_glob_name("(.git|*.toml)", ".git") && !p9k_glob_name("*", ".git") && !p9k_glob_name("?git", ".git") &&
          p9k_glob_name("*", "go.mod") && p9k_glob_name(".*", ".hg"), "a file name's leading dot only by a dot");
    struct p10k_state st = state("/h/x");
    struct sb out = { 0 };
    p9k_zexpand("%F{39}a%f%F{39}b%B%bc%f%K{238}%k", &st, &out);
    CHECK(!strcmp(sb_text(&out), "\033[38;5;39mabc\033[39m"), "one SGR per change, none for changes undone; the end applied");
    sb_reset(&out);
    p9k_zexpand("%(?.ok.err) %D{%I:%M %p} %~ %{\033]0;t\007%}", &st, &out);
    CHECK(!strcmp(sb_text(&out), "ok 09:05 AM ~/x \033]0;t\007"), "%(?..), %D{}, %~, %{raw%}");
    sb_free(&out);
    CHECK(p10k_width("%F{1}\xe2\x95\xad\xe2\x94\x80%f %B~%b", &st) == 4, "widths count cells, not escapes");
}

int main(void) {
    cfg = p10k_new();
    test_import();
    test_segments();
    test_lines();
    test_zsh();
    p10k_free(cfg);
    printf(g_fail ? "p10k: %d FAILED\n" : "p10k: all passed\n", g_fail);
    return g_fail != 0;
}
