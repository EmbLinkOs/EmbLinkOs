/* ==========================================================================
 * complete_test.c -- what Tab offers, on the host's own filesystem.
 *
 * A scratch directory holds the awkward names (a space, a leading digit, a
 * hidden file, a directory); the completer is asked about lines a person
 * would type and must offer exactly what the shell would then read as the
 * name -- quoted where the lexer would split or misread it.
 *
 * Build: make shell-test. Exit code = number of failures.
 * ========================================================================== */
#define _DEFAULT_SOURCE   /* mkdtemp */
#include "complete/complete.h"
#include "eval/eval.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int g_fail = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("  ok   %s\n", name); \
    else      { printf("  FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

/* The executor is not in the host build; one defined command stands in. */
size_t exec_user_command_count(void) { return 1; }
const char *exec_user_command_name(size_t i) { return i == 0 ? "deploy" : NULL; }

static struct edit_matches m;
static struct scope top;

static void ask(const char *line) {
    for (size_t i = 0; i < m.n; i++) { free(m.items[i].insert); free(m.items[i].show); }
    free(m.items);
    memset(&m, 0, sizeof m);
    shell_complete(&top, line, strlen(line), &m);
}
static bool offers(const char *insert) {
    for (size_t i = 0; i < m.n; i++) if (!strcmp(m.items[i].insert, insert)) return true;
    return false;
}
static void touch(const char *name) { FILE *f = fopen(name, "w"); if (f) fclose(f); }

int main(void) {
    char dir[] = "/tmp/emblink-complete-XXXXXX";
    if (!mkdtemp(dir) || chdir(dir) != 0) { puts("no scratch directory"); return 1; }
    touch("alpha.txt");
    mkdir("alpine", 0755);
    touch("alpine/inside.c");
    touch("my file.txt");
    touch("2024-report.txt");
    touch(".hidden");
    scope_init(&top, NULL);
    scope_bind(&top, "alpha", value_int(1));
    scope_bind(&top, "args", value_list());

    printf("files:\n");
    ask("cat al");
    CHECK(m.n == 2 && m.start == 4, "two names start with al; the word begins after 'cat '");
    CHECK(offers("alpha.txt") && offers("alpine/"), "a file, and a directory with its slash");
    CHECK(!strcmp(m.items[0].show, "alpha.txt") && m.items[1].dir, "sorted, and the directory marked as one");
    ask("cat my");
    CHECK(m.n == 1 && offers("\"my file.txt\""), "a name with a space is offered quoted");
    ask("cat 20");
    CHECK(offers("\"2024-report.txt\""), "so is a name the lexer would read as a number");
    ask("cat \"my f");
    CHECK(m.n == 1 && m.start == 4 && offers("\"my file.txt\""), "inside an open quote the whole quoted word completes");
    ask("cat alpine/");
    CHECK(m.n == 1 && offers("alpine/inside.c"), "into a directory");
    ask("cat a");
    CHECK(!offers(".hidden"), "hidden names stay hidden");
    ask("cat .h");
    CHECK(offers("\".hidden\""), "until the word asks for them");

    printf("commands:\n");
    ask("ec");
    CHECK(offers("echo") && m.start == 0, "a builtin, where a command goes");
    ask("exi");
    CHECK(offers("exit"), "the REPL's own exit");
    ask("ls | cou");
    CHECK(offers("count") && m.start == 5, "after a pipe is a command position too");
    ask("dep");
    CHECK(offers("deploy"), "a command defined with def");
    ask("echo dep");
    CHECK(!offers("deploy"), "an argument is not a command position");
    ask("if true { whe");
    CHECK(offers("where"), "the start of a block is a command position");
    ask("./al");
    CHECK(offers("./alpha.txt") && offers("./alpine/"), "a path where a command goes completes as a path");

    printf("variables:\n");
    ask("echo $a");
    CHECK(m.n == 2 && offers("$alpha") && offers("$args"), "$ completes the session's names");
    ask("echo 'al");
    CHECK(m.n == 0, "nothing inside single quotes");

    scope_free(&top);
    remove("alpine/inside.c"); rmdir("alpine");
    remove("alpha.txt"); remove("my file.txt"); remove("2024-report.txt"); remove(".hidden");
    if (chdir("/") == 0) rmdir(dir);
    ask("");
    printf(g_fail ? "complete: %d FAILED\n" : "complete: all passed\n", g_fail);
    return g_fail;
}
