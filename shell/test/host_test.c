/* ==========================================================================
 * host_test.c -- host-side tests for the shell's pure pipeline:
 * lexer -> parser -> evaluator -> builtins, plus the wire round-trip.
 * Build: make shell-test. Exit code = number of failures.
 * ========================================================================== */
#include "lex/lex.h"
#include "parse/parse.h"
#include "eval/eval.h"
#include "builtins/builtins.h"
#include "wire/wire.h"
#include "sval/sval.h"
#include "hist/hist.h"
#include "prompt/prompt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("  ok   %s\n", name); \
    else      { printf("  FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

/* --- helpers ------------------------------------------------------------- */

/* Evaluate one full line's pipeline against an input value (NULL-type for
 * none) and a fresh scope; returns the resulting value. */
static struct value run(const char *line, struct value input, struct scope *top) {
    size_t n = 0;
    struct token *toks = lex(line, &n);
    if (!toks) return value_error("lex OOM");
    char err[160];
    struct stmt *st = parse(toks, n, err, sizeof err);
    lex_free_tokens(toks, n);
    if (!st) return value_error(err);
    if (st->kind != STMT_PIPELINE) { stmt_free(st); return value_error("expected pipeline"); }

    /* thread `input` into the FIRST stage by hand (pipeline_run starts from
     * null): run stage 0 with input, then the rest. */
    struct value cur = input;
    for (size_t i = 0; i < st->u.pipe->nstages; i++) {
        const struct command *cmd = st->u.pipe->stages[i];
        builtin_fn fn = builtin_lookup(cmd->name);
        cur = fn ? fn(cmd, cur, top) : extern_stage_run(cmd, cur, top, false);
        if (cur.type == VAL_ERROR) break;
    }
    stmt_free(st);
    return cur;
}

/* A fabricated ls-shaped table: {name, size} rows. */
static struct value sample_table(void) {
    struct value t = value_table();
    const char *names[] = { "kernel.bin", "font.ttf", "readme.txt" };
    int64_t sizes[]     = { 3 * 1024 * 1024, 800 * 1024, 512 };
    for (int i = 0; i < 3; i++) {
        struct value r = value_record();
        value_record_set(&r, "name", value_string(names[i]));
        value_record_set(&r, "size", value_filesize(sizes[i]));
        value_table_push_row(&t, r);
    }
    return t;
}

/* --- lexer ---------------------------------------------------------------- */
static void test_lexer(void) {
    printf("lexer:\n");
    size_t n = 0;
    struct token *t = lex("ls | where size > 1mb  # trailing comment", &n);
    CHECK(t && n == 7, "token count (6 + EOF)");
    CHECK(t[0].type == TOK_IDENT && t[1].type == TOK_PIPE && t[2].type == TOK_IDENT &&
          t[3].type == TOK_IDENT && t[4].type == TOK_GT && t[5].type == TOK_FILESIZE,
          "token kinds");
    CHECK(t[5].int_val == 1024 * 1024, "1mb pre-parsed to bytes");
    lex_free_tokens(t, n);

    t = lex("sort-by file.txt \"a\\nb\" 'raw\\n' 3.5", &n);
    CHECK(t && t[0].type == TOK_IDENT && t[0].lexeme_len == 7, "dash joins sort-by");
    CHECK(t[1].type == TOK_IDENT && t[1].lexeme_len == 8, "dot joins file.txt");
    CHECK(t[2].type == TOK_STRING && t[2].str_len == 3 && t[2].str[1] == '\n',
          "double-quote escapes decode");
    CHECK(t[3].type == TOK_STRING && t[3].str_len == 5 && t[3].str[3] == '\\',
          "single-quote stays raw");
    CHECK(t[4].type == TOK_FLOAT && t[4].float_val > 3.4 && t[4].float_val < 3.6,
          "float literal");
    lex_free_tokens(t, n);

    t = lex("\"unterminated", &n);
    bool has_err = false;
    for (size_t i = 0; i < n; i++) if (t[i].type == TOK_ERROR) has_err = true;
    CHECK(has_err, "unterminated string -> TOK_ERROR");
    lex_free_tokens(t, n);
}

/* --- wire round-trip ------------------------------------------------------- */
static void test_wire(void) {
    printf("wire:\n");
    struct value t = sample_table();
    struct value extras = value_list();
    value_list_push(&extras, value_int(-42));
    value_list_push(&extras, value_float(2.5));
    value_list_push(&extras, value_bool(true));
    value_list_push(&extras, value_date(1752537600));
    value_list_push(&extras, value_path("/boot/kernel"));
    value_list_push(&extras, value_null());
    struct value rec = value_record();
    value_record_set(&rec, "table", t);          /* MOVES t */
    value_record_set(&rec, "list", extras);      /* MOVES extras */

    struct wire_buf frame;
    wire_buf_init(&frame);
    CHECK(wire_serialize(&rec, &frame) == 0, "serialize");

    /* feed the frame byte-by-byte through the incremental reader -- the
     * pipe-boundary case */
    struct wire_reader rd;
    wire_reader_init(&rd);
    struct value back = value_null();
    int got = 0;
    for (size_t i = 0; i < frame.len && !got; i++) {
        if (wire_reader_feed(&rd, frame.data + i, 1) != 0) break;
        int rc = wire_reader_next(&rd, &back);
        if (rc == 1) got = 1;
        else if (rc < 0) break;
    }
    CHECK(got == 1, "incremental decode completes");
    CHECK(value_equal(&rec, &back), "round-trip equality");

    value_free(&rec);
    value_free(&back);
    wire_buf_free(&frame);
    wire_reader_free(&rd);
}

/* --- parser ---------------------------------------------------------------- */
static void test_parser(void) {
    printf("parser:\n");
    size_t n = 0;
    char err[160];

    struct token *t = lex("ls /data | where size > 1mb and name =~ \"txt\" | sort-by size", &n);
    struct stmt *s = parse(t, n, err, sizeof err);
    lex_free_tokens(t, n);
    CHECK(s && s->kind == STMT_PIPELINE && s->u.pipe->nstages == 3, "3-stage pipeline");
    CHECK(s && s->u.pipe->stages[1]->nargs == 1 &&
          s->u.pipe->stages[1]->args[0]->kind == EXPR_BINARY &&
          s->u.pipe->stages[1]->args[0]->u.binary.op == TOK_AND,
          "where's arg is one AND expr (precedence)");
    stmt_free(s);

    t = lex("echo 1 + 2 * 3", &n);
    s = parse(t, n, err, sizeof err);
    lex_free_tokens(t, n);
    CHECK(s && s->u.pipe->stages[0]->nargs == 1 &&
          s->u.pipe->stages[0]->args[0]->u.binary.op == TOK_PLUS,
          "* binds tighter than +");
    stmt_free(s);

    t = lex("where a < b < c", &n);
    s = parse(t, n, err, sizeof err);
    lex_free_tokens(t, n);
    CHECK(!s && strstr(err, "chain"), "comparison chaining rejected");

    t = lex("let x = 1mb + 512kb", &n);
    s = parse(t, n, err, sizeof err);
    lex_free_tokens(t, n);
    CHECK(s && s->kind == STMT_LET && strcmp(s->u.let.name, "x") == 0, "let statement");
    stmt_free(s);

    t = lex("echo $row.size", &n);
    s = parse(t, n, err, sizeof err);
    lex_free_tokens(t, n);
    CHECK(s && s->u.pipe->stages[0]->args[0]->kind == EXPR_FIELD, "field access parses");
    stmt_free(s);

    t = lex("ls |", &n);
    s = parse(t, n, err, sizeof err);
    lex_free_tokens(t, n);
    CHECK(!s && err[0] != '\0', "trailing pipe is an error with a message");
}

/* --- evaluator + builtins --------------------------------------------------- */
static void test_eval(void) {
    printf("eval/builtins:\n");
    struct scope top;
    scope_init(&top, NULL);

    struct value v = run("echo 1 + 2 * 3", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 7, "arithmetic precedence evaluates");
    value_free(&v);

    v = run("echo 1mb + 512kb", value_null(), &top);
    CHECK(v.type == VAL_FILESIZE && v.u.i == 1536 * 1024, "filesize arithmetic keeps the tag");
    value_free(&v);

    v = run("echo 2mb > 1000000", value_null(), &top);
    CHECK(v.type == VAL_BOOL && v.u.b, "filesize/int cross-compare coerces");
    value_free(&v);

    v = run("echo 1 / 0", value_null(), &top);
    CHECK(v.type == VAL_ERROR, "division by zero is an error value");
    value_free(&v);

    v = run("echo 6 / 2", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 3, "infix / divides");
    value_free(&v);

    v = run("echo /", value_null(), &top);
    CHECK(v.type == VAL_STRING && strcmp(v.u.s.bytes, "/") == 0,
          "prefix / is the bare word (root path)");
    value_free(&v);

    /* let + $var through a real scope */
    {
        size_t n = 0;
        struct token *t = lex("let cutoff = 600kb", &n);
        char err[160];
        struct stmt *s = parse(t, n, err, sizeof err);
        lex_free_tokens(t, n);
        struct value bound = expr_eval(s->u.let.expr, &top);
        scope_bind(&top, s->u.let.name, bound);
        stmt_free(s);

        v = run("echo $cutoff + 1", value_null(), &top);
        CHECK(v.type == VAL_FILESIZE && v.u.i == 600 * 1024 + 1, "let binding resolves via $");
        value_free(&v);

        v = run("echo $nope", value_null(), &top);
        CHECK(v.type == VAL_ERROR, "unresolved $var errors");
        value_free(&v);

        v = run("echo hi", value_null(), &top);
        CHECK(v.type == VAL_STRING && strcmp(v.u.s.bytes, "hi") == 0,
              "bare word falls back to string");
        value_free(&v);
    }

    /* where / sort-by / select / first / count on the sample table */
    v = run("where size > 600kb", sample_table(), &top);
    CHECK(v.type == VAL_TABLE && v.u.table->count == 2, "where filters rows");
    value_free(&v);

    v = run("where size", sample_table(), &top);
    CHECK(v.type == VAL_ERROR, "non-boolean condition is an error (strict truthiness)");
    value_free(&v);

    v = run("sort-by size", sample_table(), &top);
    CHECK(v.type == VAL_TABLE && v.u.table->count == 3 &&
          record_field(&v.u.table->rows[0], "size")->u.i == 512 &&
          record_field(&v.u.table->rows[2], "size")->u.i == 3 * 1024 * 1024,
          "sort-by orders ascending");
    value_free(&v);

    v = run("where name =~ \"txt\" | count", sample_table(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 1, "=~ substring + count");
    value_free(&v);

    v = run("select name | first 2 | count", sample_table(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 2, "select | first | count chain");
    value_free(&v);

    v = run("sort-by size | first | select name", sample_table(), &top);
    CHECK(v.type == VAL_TABLE && v.u.table->count == 1 &&
          strcmp(record_field(&v.u.table->rows[0], "name")->u.s.bytes, "readme.txt") == 0,
          "smallest file wins the full chain");
    value_free(&v);

    v = run("frobnicate", value_null(), &top);
    CHECK(v.type == VAL_ERROR, "unknown command surfaces an error (host stub)");
    value_free(&v);

    /* the standard-name pure batch: head/tail/last, reverse, get, wc, help */
    v = run("head 2 | count", sample_table(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 2, "head is first");
    value_free(&v);

    v = run("last 2 | first | get name", sample_table(), &top);
    CHECK(v.type == VAL_LIST && v.u.list.count == 1 &&
          strcmp(v.u.list.items[0].u.s.bytes, "font.ttf") == 0,
          "last 2 | first picks the middle row; get pulls the column");
    value_free(&v);

    v = run("reverse | first | get name", sample_table(), &top);
    CHECK(v.type == VAL_LIST && v.u.list.count == 1 &&
          strcmp(v.u.list.items[0].u.s.bytes, "readme.txt") == 0,
          "reverse flips row order");
    value_free(&v);

    v = run("get size | count", sample_table(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 3, "get column -> list of 3");
    value_free(&v);

    v = run("echo \"one two\\nthree\" | wc", value_null(), &top);
    CHECK(v.type == VAL_RECORD &&
          value_record_get(&v, "lines") && value_record_get(&v, "lines")->u.i == 2 &&
          value_record_get(&v, "words") && value_record_get(&v, "words")->u.i == 3,
          "wc counts lines and words of text");
    value_free(&v);

    v = run("help | count", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i >= 20, "help lists the command set");
    value_free(&v);

    /* null cells (a dir's size): comparisons are FALSE, sorts put nulls first */
    {
        struct value nt = value_table();
        struct value r1 = value_record();
        value_record_set(&r1, "name", value_string("bin"));
        value_record_set(&r1, "size", value_null());
        value_table_push_row(&nt, r1);
        struct value r2 = value_record();
        value_record_set(&r2, "name", value_string("kernel.bin"));
        value_record_set(&r2, "size", value_filesize(3 * 1024 * 1024));
        value_table_push_row(&nt, r2);

        struct value c1 = value_clone(&nt);
        v = run("where size > 1mb | count", c1, &top);
        CHECK(v.type == VAL_INT && v.u.i == 1, "null > x is false (dir rows don't match)");
        value_free(&v);

        struct value c2 = value_clone(&nt);
        v = run("where size < 1mb | count", c2, &top);
        CHECK(v.type == VAL_INT && v.u.i == 0, "null < x is ALSO false (SQL semantics)");
        value_free(&v);

        v = run("sort-by size", nt, &top);
        CHECK(v.type == VAL_TABLE && v.u.table->count == 2 &&
              strcmp(record_field(&v.u.table->rows[0], "name")->u.s.bytes, "bin") == 0,
              "sort-by puts null keys first");
        value_free(&v);
    }

    scope_free(&top);
}

/* --- history ring ---------------------------------------------------------- */
static void test_hist(void) {
    printf("history:\n");
    CHECK(hist_count() == 0, "starts empty");

    hist_push("ls");
    hist_push("ps");
    CHECK(hist_count() == 2 && strcmp(hist_get(0), "ls") == 0 &&
          strcmp(hist_get(1), "ps") == 0, "stores in order (0 = oldest)");

    hist_push("ps");
    CHECK(hist_count() == 2, "ignoredups: repeating the last line is collapsed");
    hist_push("ls");
    CHECK(hist_count() == 3, "a NON-adjacent repeat is still stored");

    hist_push("");
    CHECK(hist_count() == 3, "empty lines are never stored");

    /* Up-arrow recall walks back from the newest: the shell asks for
     * hist_get(count - browse), so browse=1 must be the last command. */
    CHECK(strcmp(hist_get(hist_count() - 1), "ls") == 0,
          "browse=1 recalls the LAST command (the core ask)");

    /* overfill: oldest falls off, newest survives, count saturates */
    for (int i = 0; i < HIST_MAX + 10; i++) {
        char line[32];
        snprintf(line, sizeof line, "cmd%d", i);
        hist_push(line);
    }
    CHECK(hist_count() == HIST_MAX, "count saturates at HIST_MAX");
    {
        char want[32];
        snprintf(want, sizeof want, "cmd%d", HIST_MAX + 10 - 1);
        CHECK(strcmp(hist_get(hist_count() - 1), want) == 0,
              "newest survives the ring wrap");
        CHECK(strcmp(hist_get(0), "ls") != 0, "oldest fell off the ring");
    }
    CHECK(hist_get(hist_count()) == NULL, "out-of-range index is NULL");
}

/* --- history on disk ------------------------------------------------------- */
static size_t file_lines(const char *path) {
    FILE *f = fopen(path, "r");
    size_t n = 0;
    for (int c; f && (c = fgetc(f)) != EOF;) n += c == '\n';
    if (f) fclose(f);
    return n;
}
static void test_hist_file(void) {
    printf("history file:\n");
    char path[] = "/tmp/emblink-hist-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "a scratch file");
    if (fd < 0) return;
    close(fd);
    remove(path);
    hist_reset();
    CHECK(hist_open(path) == 0 && hist_count() == 0, "a missing file is an empty history");
    hist_push("one");
    hist_push("if x {\n  echo \"a\\b\"\n}");
    hist_push("if x {\n  echo \"a\\b\"\n}");
    CHECK(file_lines(path) == 2, "accepted lines are appended, one file line each; a repeat is not");
    hist_reset();
    CHECK(hist_open(path) == 2 && hist_count() == 2, "a new session reads them back");
    CHECK(!strcmp(hist_get(0), "one"), "in order");
    CHECK(!strcmp(hist_get(1), "if x {\n  echo \"a\\b\"\n}"), "a multi-line entry with a backslash survives the file");
    hist_push("two");
    CHECK(file_lines(path) == 3, "the reopened history appends to the same file");
    /* Years of appends: the file is trimmed to what the ring holds. */
    FILE *f = fopen(path, "a");
    for (int i = 0; f && i < 2 * HIST_MAX; i++) fprintf(f, "line%d\n", i);
    if (f) fclose(f);
    hist_reset();
    hist_open(path);
    CHECK(hist_count() == HIST_MAX, "the ring holds HIST_MAX");
    CHECK(file_lines(path) == HIST_MAX, "and the file is rewritten to the same");
    char want[32];
    snprintf(want, sizeof want, "line%d", 2 * HIST_MAX - 1);
    CHECK(!strcmp(hist_get(HIST_MAX - 1), want), "keeping the newest");
    hist_reset();
    remove(path);
}

/* ---------------------------------------------------------------------------
 * The PROGRAM grammar: statements, blocks, control flow, definitions.
 *
 * These test the PARSER, not the executor -- the executor needs a sink and a
 * top-level scope that only the driver has. What is being pinned here is that
 * the shape of a program is understood: that a newline separates statements
 * but does not end a pipeline mid-'|', that a block spans lines, and that the
 * forms nest.
 * ------------------------------------------------------------------------- */
static struct block *prog_ok(const char *src) {
    size_t n = 0;
    struct token *t = lex(src, &n);
    char err[160];
    struct block *b = parse_program(t, n, err, sizeof err);
    if (!b) printf("       parse failed: %s\n", err);
    lex_free_tokens(t, n);
    return b;
}

static bool prog_fails(const char *src) {
    size_t n = 0;
    struct token *t = lex(src, &n);
    char err[160];
    struct block *b = parse_program(t, n, err, sizeof err);
    lex_free_tokens(t, n);
    if (b) { block_free(b); return false; }
    return true;
}

/* Unfinished is not wrong: what the line editor asks before Enter runs. */
static void test_incomplete(void) {
    printf("unfinished programs:\n");
    CHECK(program_incomplete("if x {"), "an open block is unfinished");
    CHECK(program_incomplete("ls |"), "a trailing pipe is unfinished");
    CHECK(program_incomplete("echo \"abc"), "a string that runs off the end is unfinished");
    CHECK(program_incomplete("echo $(ls"), "an open command substitution is unfinished");
    CHECK(program_incomplete("def f(a,"), "a parameter list is unfinished");
    CHECK(!program_incomplete("echo hi"), "a whole statement is finished");
    CHECK(!program_incomplete("if x {\n echo 1\n}"), "a closed block is finished");
    CHECK(!program_incomplete("echo }"), "a stray '}' is an error, not unfinished");
    CHECK(!program_incomplete("ls | | count"), "an error before the end is an error");
    CHECK(!program_incomplete(""), "nothing is finished");
}

static void test_program(void) {
    printf("program grammar:\n");

    struct block *b = prog_ok("echo one\necho two\necho three");
    CHECK(b && b->n == 3, "newlines separate statements");
    block_free(b);

    b = prog_ok("echo one; echo two");
    CHECK(b && b->n == 2, "';' separates statements on one line");
    block_free(b);

    b = prog_ok("\n\n  \n echo hi \n\n");
    CHECK(b && b->n == 1, "blank lines are not statements");
    block_free(b);

    /* The one place a newline must NOT end the statement. */
    b = prog_ok("ls |\n  where size > 1 |\n  count");
    CHECK(b && b->n == 1 && b->stmts[0]->kind == STMT_PIPELINE &&
          b->stmts[0]->u.pipe->nstages == 3,
          "a pipeline continues across a newline after '|'");
    block_free(b);

    b = prog_ok("if true { echo yes }");
    CHECK(b && b->n == 1 && b->stmts[0]->kind == STMT_IF &&
          b->stmts[0]->u.iff.else_b == NULL, "if with no else");
    block_free(b);

    b = prog_ok("if true {\n  echo yes\n} else {\n  echo no\n}");
    CHECK(b && b->n == 1 && b->stmts[0]->kind == STMT_IF &&
          b->stmts[0]->u.iff.then_b->n == 1 &&
          b->stmts[0]->u.iff.else_b->n == 1, "if/else spanning lines");
    block_free(b);

    /* `else` on the line after '}' -- which reads naturally and would
     * otherwise be a separator ending the statement. */
    b = prog_ok("if true { echo a }\nelse { echo b }");
    CHECK(b && b->n == 1 && b->stmts[0]->u.iff.else_b != NULL,
          "'else' may start the next line");
    block_free(b);

    /* A statement is a pipeline, a let, or a control form -- a bare literal
     * is not one, so the arms here are commands. */
    b = prog_ok("if a { echo 1 } else if b { echo 2 } else { echo 3 }");
    CHECK(b && b->n == 1 && b->stmts[0]->u.iff.else_b &&
          b->stmts[0]->u.iff.else_b->n == 1 &&
          b->stmts[0]->u.iff.else_b->stmts[0]->kind == STMT_IF,
          "else-if is an else block holding one if");
    block_free(b);

    b = prog_ok("while true { break }");
    CHECK(b && b->n == 1 && b->stmts[0]->kind == STMT_WHILE &&
          b->stmts[0]->u.wh.body->stmts[0]->kind == STMT_BREAK, "while + break");
    block_free(b);

    b = prog_ok("for f in $files { echo $f }");
    CHECK(b && b->n == 1 && b->stmts[0]->kind == STMT_FOR &&
          strcmp(b->stmts[0]->u.fr.var, "f") == 0, "for/in binds a loop variable");
    block_free(b);

    b = prog_ok("def hello { echo hi }");
    CHECK(b && b->n == 1 && b->stmts[0]->kind == STMT_DEF &&
          b->stmts[0]->u.def.nparams == 0, "def with no parameter list");
    block_free(b);

    b = prog_ok("def greet(name, punct) {\n  echo $name\n  return 1\n}");
    CHECK(b && b->n == 1 && b->stmts[0]->kind == STMT_DEF &&
          b->stmts[0]->u.def.nparams == 2 &&
          strcmp(b->stmts[0]->u.def.params[1], "punct") == 0 &&
          b->stmts[0]->u.def.body->n == 2, "def with parameters and a return");
    block_free(b);

    b = prog_ok("for x in $xs {\n  if $x > 3 {\n    while true { break }\n  }\n}");
    CHECK(b && b->n == 1 && b->stmts[0]->kind == STMT_FOR, "the forms nest");
    block_free(b);

    /* Two commands on one line with nothing between them was previously read
     * as one command with extra arguments. Saying so is the difference
     * between a typo and a surprise. */
    CHECK(prog_fails("echo a echo b\n" ) == false,
          "bare words are still arguments, not a missing separator");
    b = prog_ok("sleep 5 &");
    CHECK(b && b->n == 1 && b->stmts[0]->bg_src &&
          strcmp(b->stmts[0]->bg_src, "sleep 5") == 0,
          "'&' backgrounds a statement and copies its source");
    block_free(b);

    b = prog_ok("ls | where size > 1 &\necho after");
    CHECK(b && b->n == 2 && b->stmts[0]->bg_src &&
          strcmp(b->stmts[0]->bg_src, "ls | where size > 1") == 0 &&
          b->stmts[1]->bg_src == NULL,
          "a whole pipeline backgrounds, and the next statement does not");
    block_free(b);

    CHECK(prog_fails("if true { echo hi"), "an unclosed block is an error");
    CHECK(prog_fails("if true echo hi"), "a block is required after if");
    CHECK(prog_fails("for in $xs { }"), "for needs a variable name");
    CHECK(prog_fails("def (a) { }"), "def needs a name");
}

/* ---------------------------------------------------------------------------
 * The vocabulary: aggregation, sequences, text.
 *
 * All PURE -- value in, value out, no OS -- which is why they can be pinned
 * here with dozens of small cases instead of only by booting a machine. The
 * cases that matter most are the EDGE ones: what an empty input does, what a
 * null cell does, what a trailing separator does. Those are where a shell
 * quietly corrupts data.
 * ------------------------------------------------------------------------- */
static void test_vocabulary(void) {
    printf("vocabulary (aggregates, sequences, text):\n");
    struct scope top;
    scope_init(&top, NULL);

    struct value v;

    /* --- range --- */
    v = run("range 4 | count", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 4, "range N produces exactly N items");
    value_free(&v);

    v = run("range 2 5 | count", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 3, "range A B is half-open");
    value_free(&v);

    v = run("range 5 2 | count", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 0, "a descending range is EMPTY, not reversed");
    value_free(&v);

    /* --- aggregates --- */
    v = run("range 5 | sum", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 10, "sum adds a list of ints (0+1+2+3+4)");
    value_free(&v);

    v = run("range 5 | max", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 4, "max");
    value_free(&v);

    v = run("range 5 | min", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 0, "min");
    value_free(&v);

    v = run("range 4 | avg", value_null(), &top);
    CHECK(v.type == VAL_FLOAT && v.u.f > 1.49 && v.u.f < 1.51, "avg of 0..3 is 1.5");
    value_free(&v);

    /* Empty is NULL, not zero -- min of nothing being 0 would be a lie, and
     * the four agreeing is worth more than the one convenient case. */
    v = run("range 0 | sum", value_null(), &top);
    CHECK(v.type == VAL_NULL, "an empty aggregate is null, not zero");
    value_free(&v);

    v = run("echo hello | sum", value_null(), &top);
    CHECK(v.type == VAL_ERROR, "a non-numeric value is refused, not coerced");
    value_free(&v);

    /* --- skip / uniq --- */
    v = run("range 5 | skip 2 | count", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 3, "skip drops the first N");
    value_free(&v);

    v = run("range 3 | skip 99 | count", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 0, "skipping past the end is empty, not an error");
    value_free(&v);

    v = run("echo \"a,b,a,c,b\" | split \",\" | uniq | count", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 3, "uniq keeps first occurrences");
    value_free(&v);

    /* --- split / join --- */
    v = run("echo \"a,b,c\" | split \",\" | count", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 3, "split on a separator");
    value_free(&v);

    /* The case that quietly corrupts every CSV a shell touches. */
    v = run("echo \"a,,b\" | split \",\" | count", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 3, "adjacent separators produce an EMPTY field");
    value_free(&v);

    v = run("echo \"abc\" | split \"\" | count", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 3, "an empty separator splits into characters");
    value_free(&v);

    v = run("range 3 | join \",\"", value_null(), &top);
    CHECK(v.type == VAL_STRING && v.u.s.len == 5 &&
          memcmp(v.u.s.bytes, "0,1,2", 5) == 0, "join renders non-text items plainly");
    value_free(&v);

    v = run("echo \"a,b,c\" | split \",\" | join \"-\"", value_null(), &top);
    CHECK(v.type == VAL_STRING && memcmp(v.u.s.bytes, "a-b-c", 5) == 0,
          "split then join round-trips through a different separator");
    value_free(&v);

    /* --- lines --- */
    v = run("echo \"a\\nb\\nc\\n\" | lines | count", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 3,
          "a trailing newline does NOT produce a phantom last line");
    value_free(&v);

    v = run("echo \"a\\r\\nb\" | lines | count", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 2, "CRLF counts as one line ending");
    value_free(&v);

    /* --- text maps --- */
    v = run("echo \"  hi  \" | trim", value_null(), &top);
    CHECK(v.type == VAL_STRING && v.u.s.len == 2 &&
          memcmp(v.u.s.bytes, "hi", 2) == 0, "trim strips both ends");
    value_free(&v);

    v = run("echo \"aBc\" | upper", value_null(), &top);
    CHECK(v.type == VAL_STRING && memcmp(v.u.s.bytes, "ABC", 3) == 0, "upper");
    value_free(&v);

    v = run("echo \"aBc\" | lower", value_null(), &top);
    CHECK(v.type == VAL_STRING && memcmp(v.u.s.bytes, "abc", 3) == 0, "lower");
    value_free(&v);

    v = run("echo \"a-b-a\" | replace \"a\" \"X\"", value_null(), &top);
    CHECK(v.type == VAL_STRING && v.u.s.len == 5 &&
          memcmp(v.u.s.bytes, "X-b-X", 5) == 0, "replace hits every occurrence");
    value_free(&v);

    v = run("echo \"aaa\" | replace \"aa\" \"b\"", value_null(), &top);
    CHECK(v.type == VAL_STRING && v.u.s.len == 2 &&
          memcmp(v.u.s.bytes, "ba", 2) == 0,
          "replace scans forward, it does not re-scan what it wrote");
    value_free(&v);

    v = run("echo \"abc\" | replace \"\" \"X\"", value_null(), &top);
    CHECK(v.type == VAL_ERROR, "an empty `old` is refused (it would never terminate)");
    value_free(&v);

    scope_free(&top);
}

/* --- redirections, assert, and values as text ----------------------------- */
static int kinds_of(const char *src, enum tok_type *types, int *kinds, size_t max) {
    size_t n = 0;
    struct token *t = lex(src, &n);
    if (!t) return -1;
    size_t k = 0;
    for (size_t i = 0; i < n && k < max; i++, k++) { types[k] = t[i].type; kinds[k] = (int)t[i].int_val; }
    lex_free_tokens(t, n);
    return (int)k;
}

static void test_redirect_text(void) {
    printf("\n-- redirections, assert, text --\n");
    enum tok_type ty[12]; int kd[12];
    int n = kinds_of("sh out> a.txt err>> e.log in< x", ty, kd, 12);
    CHECK(n == 8 && ty[0] == TOK_IDENT && ty[1] == TOK_REDIRECT && kd[1] == REDIR_OUT && ty[2] == TOK_IDENT &&
          ty[3] == TOK_REDIRECT && kd[3] == REDIR_ERR_APPEND && ty[5] == TOK_REDIRECT && kd[5] == REDIR_IN,
          "out> err>> in< lex as redirections, each followed by its file");
    n = kinds_of("p out+err>> both.log", ty, kd, 12);
    CHECK(n == 4 && ty[1] == TOK_REDIRECT && kd[1] == REDIR_BOTH_APPEND, "out+err>> is one redirection");
    n = kinds_of("where out > 3", ty, kd, 12);
    CHECK(n == 5 && ty[1] == TOK_IDENT && ty[2] == TOK_GT, "with a space, out > 3 is still a comparison");
    n = kinds_of("sh -c \"x\" --verbose --width=80", ty, kd, 12);
    CHECK(n == 6 && ty[1] == TOK_IDENT && ty[2] == TOK_STRING && ty[3] == TOK_IDENT && ty[4] == TOK_IDENT,
          "-c, --verbose and --width=80 are flags: words, not a minus");
    n = kinds_of("echo -3 5 - 2", ty, kd, 12);
    CHECK(n == 7 && ty[1] == TOK_MINUS && ty[2] == TOK_INT && ty[4] == TOK_MINUS,
          "-3 is still a negative number and 5 - 2 still subtraction");
    n = kinds_of("echo layout>3", ty, kd, 12);
    CHECK(n == 5 && ty[1] == TOK_IDENT && ty[2] == TOK_GT, "out> inside a word is not a redirection");

    char err[160];
    size_t nt = 0;
    struct token *toks = lex("sh -c x out> f.txt err> e.txt", &nt);
    struct stmt *st = parse(toks, nt, err, sizeof err);
    lex_free_tokens(toks, nt);
    const struct command *c = st && st->kind == STMT_PIPELINE ? st->u.pipe->stages[0] : NULL;
    CHECK(c && c->nargs == 2 && c->nredirs == 2 && c->redirs[0].kind == REDIR_OUT && c->redirs[1].kind == REDIR_ERR &&
          !strcmp(expr_as_word(c->redirs[0].target), "f.txt"), "redirections sit beside a command's arguments");
    stmt_free(st);
    toks = lex("sh out> a out+err> b", &nt);
    st = parse(toks, nt, err, sizeof err);
    lex_free_tokens(toks, nt);
    CHECK(!st && strstr(err, "redirected twice"), "a stream redirected twice is refused");
    stmt_free(st);
    toks = lex("sh out>", &nt);
    st = parse(toks, nt, err, sizeof err);
    lex_free_tokens(toks, nt);
    CHECK(!st && strstr(err, "expected a file"), "a redirection without a file is refused");
    stmt_free(st);

    struct scope top; scope_init(&top, NULL);
    struct value v = run("echo 5 | assert 2 + 2 == 4", value_null(), &top);
    CHECK(v.type == VAL_INT && v.u.i == 5, "assert passes its input through when the condition holds");
    value_free(&v);
    v = run("assert 1 == 2 \"one is not two\"", value_null(), &top);
    CHECK(v.type == VAL_ERROR && strstr(value_error_msg(&v), "one is not two"), "assert fails with its message");
    value_free(&v);
    v = run("assert 7", value_null(), &top);
    CHECK(v.type == VAL_ERROR && strstr(value_error_msg(&v), "neither true nor false"), "assert is strict about its condition");
    value_free(&v);
    v = run("echo 1 | where 1 == 1 out> f.txt", value_null(), &top);
    CHECK(v.type == VAL_ERROR, "a redirected builtin needs the OS (host)");
    value_free(&v);
    scope_free(&top);

    /* Text for a text program's stdin: whole, one newline, rows as a person reads them. */
    char longs[300];
    memset(longs, 'x', sizeof longs - 1); longs[sizeof longs - 1] = 0;
    struct wire_buf b; wire_buf_init(&b);
    struct value s = value_string(longs);
    CHECK(sval_render(&s, &b) == 0 && b.len == 300 && b.data[299] == '\n', "a long string is rendered whole");
    value_free(&s); wire_buf_free(&b); wire_buf_init(&b);
    s = value_string("line\n");
    CHECK(sval_render(&s, &b) == 0 && b.len == 5, "a string ending in a newline gets no second one");
    value_free(&s); wire_buf_free(&b); wire_buf_init(&b);
    struct value l = value_list();
    value_list_push(&l, value_int(1)); value_list_push(&l, value_string("two"));
    CHECK(sval_render(&l, &b) == 0 && b.len == 6 && !memcmp(b.data, "1\ntwo\n", 6), "a list is one item per line");
    value_free(&l); wire_buf_free(&b); wire_buf_init(&b);
    struct value t = sample_table();
    int found = 0;
    if (sval_render(&t, &b) == 0)
        for (size_t i = 0; i + 10 <= b.len && !found; i++) found = !memcmp(b.data + i, "readme.txt", 10);
    CHECK(found && b.len > 40, "a table is its grid");
    value_free(&t); wire_buf_free(&b);
}

static bool count_is(const char *line, struct scope *top, int64_t want) {
    struct value v = run(line, value_null(), top);
    bool ok = v.type == VAL_INT && v.u.i == want;
    value_free(&v);
    return ok;
}
static bool string_is(const char *line, struct scope *top, const char *want) {
    struct value v = run(line, value_null(), top);
    bool ok = v.type == VAL_STRING && v.u.s.len == strlen(want) && !memcmp(v.u.s.bytes, want, v.u.s.len);
    value_free(&v);
    return ok;
}
static bool fails(const char *line, struct scope *top) {
    struct value v = run(line, value_null(), top);
    bool ok = v.type == VAL_ERROR;
    value_free(&v);
    return ok;
}
static const char *after(const char *hay, const char *needle, const char *from) {
    const char *at = strstr(from ? from : hay, needle);
    return at ? at : NULL;
}

static void test_prompt(void) {
    printf("the prompt: options and what they build:\n");
    struct scope top;
    scope_init(&top, NULL);
    prompt_opt_reset();
    struct value v = run("prompt set right status duration time", value_null(), &top);
    CHECK(v.type == VAL_NULL && !strcmp(prompt_opt("right"), "status duration time"), "set: the words are one value");
    value_free(&v);
    v = run("prompt set char \"\xe2\x9d\xaf\"", value_null(), &top);
    CHECK(v.type == VAL_NULL && !strcmp(prompt_opt("char"), "\xe2\x9d\xaf"), "a quoted string is its value");
    value_free(&v);
    v = run("prompt set duration.threshold 5", value_null(), &top);
    CHECK(v.type == VAL_NULL && !strcmp(prompt_opt("duration.threshold"), "5"), "a number too");
    value_free(&v);
    v = run("prompt set time.format 12h", value_null(), &top);
    CHECK(v.type == VAL_NULL && !strcmp(prompt_opt("time.format"), "12h"), "12h is one word, though it lexes as 12 and h");
    value_free(&v);
    v = run("prompt set right 12 h", value_null(), &top);
    CHECK(v.type == VAL_NULL && !strcmp(prompt_opt("right"), "12 h") && prompt_opt_unset("time.format"), "apart, two");
    value_free(&v);
    v = run("prompt set right status duration time", value_null(), &top);
    value_free(&v);
    CHECK(string_is("prompt get char", &top, "\xe2\x9d\xaf"), "get: the value");
    CHECK(count_is("prompt | count", &top, 3), "prompt: a table of them");
    CHECK(fails("prompt set Dir.fg 4", &top) && fails("prompt set \"a..b\" 4", &top) && fails("prompt set \".a\" 4", &top) &&
          fails("prompt set \"a b\" 4", &top) && fails("prompt set \"a.\" 4", &top),
          "a key is lower-case words joined by single dots");
    v = run("prompt set \"dir.fg\" 4", value_null(), &top);
    CHECK(v.type == VAL_NULL && !strcmp(prompt_opt("dir.fg"), "4") && prompt_opt_unset("dir.fg"), "and may be quoted");
    value_free(&v);
    CHECK(fails("prompt set right", &top) && fails("prompt frob", &top), "and usage errors say so");
    v = run("prompt set dir.anchor.fg 39", value_null(), &top);
    CHECK(v.type == VAL_NULL && !strcmp(prompt_opt("dir.anchor.fg"), "39"), "a key the builder does not use yet is kept");
    value_free(&v);
    v = run("prompt unset char", value_null(), &top);
    value_free(&v);
    v = run("prompt get char", value_null(), &top);
    CHECK(v.type == VAL_NULL && count_is("prompt | count", &top, 3), "unset: gone");
    value_free(&v);
    v = run("prompt reset", value_null(), &top);
    value_free(&v);
    CHECK(count_is("prompt | count", &top, 0), "reset: all of them");

    struct prompt_state st = { .user = "yves", .host = "emblink", .dir = "~/src", .last_dir = "~/src", .status = 1,
                               .duration_ms = 4200, .hour = 14, .minute = 5, .second = 9 };
    struct prompt_text t;
    prompt_build(&st, 80, &t);
    CHECK(strstr(t.left, "yves@emblink:~/src$") && !t.right[0] && !t.transient[0],
          "by default: the prompt as before, nothing on the right, no transient prompt");
    prompt_opt_set("right", "status duration time");
    prompt_build(&st, 80, &t);
    const char *a = after(t.right, "\xe2\x9c\x98 1", NULL), *b = a ? after(t.right, "4s", a) : NULL, *c = b ? after(t.right, "14:05:09", b) : NULL;
    CHECK(a && b && c, "the right side: ✘ and the status, the duration, the time -- in that order");
    prompt_opt_set("time.format", "12h");
    st.status = 0; st.duration_ms = 500;
    prompt_build(&st, 80, &t);
    CHECK(strstr(t.right, "\xe2\x9c\x94") && !strstr(t.right, "0s") && strstr(t.right, "02:05:09 PM"),
          "✔ for success; a short command shows no duration; a 12-hour clock");
    st.status = -1;
    prompt_build(&st, 80, &t);
    CHECK(!strstr(t.right, "\xe2\x9c") && strstr(t.right, "PM"), "before the first command: the time alone");
    st.status = 0;
    st.duration_ms = 3725000;
    prompt_build(&st, 80, &t);
    CHECK(strstr(t.right, "1h 2m 5s") != NULL, "an hour and more");
    st.duration_ms = 65000;
    prompt_build(&st, 80, &t);
    CHECK(strstr(t.right, "1m 5s") != NULL, "a minute and more");
    prompt_opt_set("duration.threshold", "90");
    prompt_build(&st, 80, &t);
    CHECK(!strstr(t.right, "1m 5s"), "under the threshold: not shown");
    prompt_opt_set("right", "status nonsense time");
    prompt_build(&st, 80, &t);
    CHECK(strstr(t.right, "\xe2\x9c\x94") && !strstr(t.right, "nonsense"), "an element it does not know is left out");
    prompt_opt_set("transient", "always");
    prompt_opt_set("char", "\xe2\x9d\xaf");
    prompt_build(&st, 80, &t);
    CHECK(strstr(t.transient, "\033[1;32m\xe2\x9d\xaf") && strstr(t.left, "\xe2\x9d\xaf"),
          "transient: the prompt character alone, green after success");
    st.status = 2;
    prompt_build(&st, 80, &t);
    CHECK(strstr(t.transient, "\033[1;31m") != NULL, "red after a failure");
    prompt_opt_set("transient", "same-dir");
    st.last_dir = "~";
    prompt_build(&st, 80, &t);
    CHECK(!t.transient[0], "same-dir: not right after the directory changed");
    st.last_dir = "~/src";
    prompt_build(&st, 80, &t);
    CHECK(t.transient[0] != 0, "and again once it stays");
    prompt_opt_reset();
    scope_free(&top);
}

/* Powerlevel10k as the base: a style, a person's ~/.p10k.zsh, options on top. */
static void test_prompt_p10k(void) {
    printf("the prompt: Powerlevel10k styles and configurations:\n");
    struct scope top;
    scope_init(&top, NULL);
    prompt_opt_reset();
    setenv("PROMPT_STYLES", "third_party/powerlevel10k/config", 1);
    CHECK(string_is("prompt style", &top, "plain"), "no style: the plain prompt");
    struct value v = run("prompt style lean", value_null(), &top);
    CHECK(v.type == VAL_NULL && string_is("prompt style", &top, "lean"), "style: lean loaded as the base");
    value_free(&v);
    struct prompt_state st = { .user = "yves", .host = "emblink", .dir = "~/src", .last_dir = "", .status = -1,
                               .hour = 14, .minute = 5, .second = 9, .pwd = "/home/yves/src", .home = "/home/yves" };
    struct prompt_text t;
    prompt_build(&st, 80, &t);
    const char *dir = strstr(t.left, "src");
    CHECK(dir && strstr(dir, "\xe2\x9d\xaf") && !strstr(t.left, "yves@emblink"), "drawn by Powerlevel10k: the directory, then \xe2\x9d\xaf");
    CHECK(strstr(t.left, "\r\n") && strncmp(t.left, "\r\n", 2), "two lines, \\r\\n between them; no empty line above the first prompt");
    st.status = 0;
    prompt_build(&st, 80, &t);
    CHECK(!strncmp(t.left, "\r\n", 2), "then an empty line above every prompt (PROMPT_ADD_NEWLINE)");
    v = run("prompt set dir.anchor.fg 196", value_null(), &top);
    value_free(&v);
    prompt_build(&st, 80, &t);
    CHECK(strstr(t.left, "38;5;196m") != NULL, "an option on top: dir.anchor.fg is DIR_ANCHOR_FOREGROUND");
    v = run("prompt set time.format 12h", value_null(), &top);
    value_free(&v);
    v = run("prompt set right status time", value_null(), &top);
    value_free(&v);
    prompt_build(&st, 80, &t);
    CHECK(strstr(t.left, "02:05:09 PM") != NULL, "the plain prompt's options keep their meaning: right, time.format");
    char name[64];
    prompt_p10k_name("dir.anchor.fg", name, sizeof name);
    bool ok = !strcmp(name, "DIR_ANCHOR_FOREGROUND");
    prompt_p10k_name("left_prompt_elements", name, sizeof name);
    ok &= !strcmp(name, "LEFT_PROMPT_ELEMENTS");
    prompt_p10k_name("multiline.first.prompt.gap.bg", name, sizeof name);
    CHECK(ok && !strcmp(name, "MULTILINE_FIRST_PROMPT_GAP_BACKGROUND"), "names: dots are underscores, fg and bg in full");

    v = run("prompt import-p10k tests/prompt/configs/user.p10k.zsh", value_null(), &top);
    const struct value *n = v.type == VAL_RECORD ? value_record_get(&v, "parameters") : NULL;
    const struct value *ign = v.type == VAL_RECORD ? value_record_get(&v, "ignored") : NULL;
    CHECK(n && n->type == VAL_INT && n->u.i > 300 && ign && ign->type == VAL_LIST && ign->u.list.count > 10,
          "import-p10k: a ~/.p10k.zsh read, and the segments that draw nothing here listed");
    value_free(&v);
    CHECK(strstr(prompt_p10k_source(), "user.p10k.zsh") != NULL, "the imported file is the base");
    CHECK(fails("prompt import-p10k tests/prompt/configs/nothing-here.zsh", &top) && fails("prompt style frob", &top),
          "a missing file or an unknown style is an error");
    FILE *f = fopen("/tmp/emblink-broken.p10k.zsh", "w");
    if (f) { fputs("typeset -g POWERLEVEL9K_MODE=ascii\nif [[ x == x ]]; then\n", f); fclose(f); }
    v = run("prompt import-p10k /tmp/emblink-broken.p10k.zsh", value_null(), &top);
    CHECK(v.type == VAL_ERROR && strstr(v.u.s.bytes, "if without fi") && strstr(prompt_p10k_source(), "user.p10k.zsh"),
          "one that does not read says where, and the prompt stays as it was");
    value_free(&v);
    remove("/tmp/emblink-broken.p10k.zsh");
    v = run("prompt configure", value_null(), &top);
    CHECK(v.type == VAL_ERROR && strstr(v.u.s.bytes, "terminal") && strstr(prompt_p10k_source(), "user.p10k.zsh"),
          "configure: with no terminal to draw on it says so, and changes nothing");
    value_free(&v);
    v = run("prompt style plain", value_null(), &top);
    value_free(&v);
    prompt_build(&st, 80, &t);
    CHECK(string_is("prompt style", &top, "plain") && strstr(t.left, "yves@emblink:~/src"), "style plain: back to the plain prompt");
    prompt_opt_reset();
    unsetenv("PROMPT_STYLES");
    scope_free(&top);
}

int main(void) {
    printf("=== shell host tests ===\n");
    test_lexer();
    test_wire();
    test_parser();
    test_program();
    test_eval();
    test_vocabulary();
    test_hist();
    test_hist_file();
    test_incomplete();
    test_redirect_text();
    test_prompt();
    test_prompt_p10k();
    printf(g_fail ? "\n%d FAILURE(S)\n" : "\nall green\n", g_fail);
    return g_fail;
}
