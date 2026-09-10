/* ==========================================================================
 * parse.h -- tokens -> AST. Two node families (the sketch's contract):
 * pipeline/command structure, and expression trees. One top-level statement
 * wrapper distinguishes `let name = expr` from a pipeline.
 *
 * Locked parsing decisions (see shell-architecture-sketch / docs/SHELL.md):
 *   - A command argument is ALWAYS an expression. No command has a bespoke
 *     arg parser; `where`'s arg is the expr `size > 1mb`, `ls`'s arg is the
 *     expr `"/foo"` or the column-ident `/foo`.
 *   - A bare ident parses to EXPR_COLUMN uniformly; whether it means "the
 *     current row's field" or "a path-ish word" is the EVALUATOR's (or the
 *     builtin's) call. Keeps the parser context-free.
 *   - Pratt/precedence-climbing with the locked table (lowest->highest):
 *       or < and < comparisons(non-chaining) < +- < * / < unary(not,-) < .()
 *   - Comparisons do NOT chain: `a < b < c` is a parse error, not `(a<b)<c`.
 *   - Ownership: the AST owns everything it points at (names are copied out
 *     of the token slices; literal Values are moved in). stmt_free() is a
 *     complete teardown. Tokens can be freed as soon as parse() returns.
 * ========================================================================== */
#ifndef __EMBK_PARSE_H__
#define __EMBK_PARSE_H__

#include "lex/lex.h"
#include "value/value.h"
#include <stddef.h>

/* --- expressions --- */
enum expr_kind {
    EXPR_LITERAL,   /* a literal Value (int/float/string/filesize/bool) */
    EXPR_VAR,       /* $ident -- explicit variable reference */
    EXPR_COLUMN,    /* bare ident -- current row's field (or a bare word the
                     * builtin interprets, e.g. a path argument to ls) */
    EXPR_FIELD,     /* expr.ident -- field access on a record value */
    EXPR_UNARY,     /* not expr, - expr */
    EXPR_BINARY,    /* expr OP expr */
    EXPR_CAPTURE,   /* $(pipeline) -- run it, use its VALUE.
                     * The bridge between the two halves of the language: a
                     * pipeline produces data, and an expression consumes it.
                     * Without this, `for row in $(ls /)` cannot be written and
                     * the control-flow forms have nothing real to work on. */
};

struct expr {
    enum expr_kind kind;
    size_t line, col;      /* of the node's first token, for error messages */
    union {
        struct value literal;                             /* EXPR_LITERAL (owned) */
        struct { char *name; } var;                       /* EXPR_VAR / EXPR_COLUMN (owned) */
        struct { struct expr *obj; char *field; } field;  /* EXPR_FIELD (owned) */
        struct { int op; struct expr *operand; } unary;   /* op = a TOK_* */
        struct { int op; struct expr *lhs, *rhs; } binary;
        struct pipeline *capture;                         /* EXPR_CAPTURE (owned) */
    } u;
};

/* --- pipeline / command --- */
struct command {
    char         *name;    /* "ls", "where", ... (owned) */
    struct expr **args;    /* each arg is an expression (owned) */
    size_t        nargs;
    size_t        line, col;
};

struct pipeline {
    struct command **stages;   /* left-to-right; stage[i] feeds stage[i+1] */
    size_t           nstages;
};

/* --- statement ------------------------------------------------------------
 * A statement was one shell LINE. It is now one step of a program, because a
 * shell that cannot branch or repeat is a launcher, not a language: with 30
 * builtins and 50-odd programs and no way to combine them, a user has 80
 * capabilities. With `if`, `while`, `for` and `def` they have as many as they
 * are willing to write down, and -- the part that matters -- they can add one
 * without rebuilding the OS.
 *
 * The control-flow forms take a BLOCK, not a line, and a block is brace
 * delimited rather than indentation- or keyword-terminated. Braces because
 * the lexer already had to learn newlines, and `}` is then the one unambiguous
 * "this is over" in a grammar where almost everything else is a bare word. */
struct block;

enum stmt_kind {
    STMT_PIPELINE,
    STMT_LET,
    STMT_IF,
    STMT_WHILE,
    STMT_FOR,
    STMT_DEF,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_RETURN,
};

struct stmt {
    enum stmt_kind kind;
    size_t line, col;

    /* `pipeline &` -- run it in the BACKGROUND and carry on.
     *
     * `bg_src` is a COPY of the statement's own source text, taken at parse
     * time. Backgrounding is implemented by spawning another shell to run that
     * text, which is the only way a BUILTIN pipeline can go to the background
     * at all: builtins run in-process, and this process is busy being the
     * shell. Re-parsing the source in the child costs a millisecond and means
     * `ls | where size > 1mb &` works exactly like `wget ... &`, rather than
     * backgrounding being a privilege only external programs have.
     *
     * A copy rather than a slice because the AST outlives the source buffer:
     * the REPL's line is a stack array and a script's text is freed after the
     * program runs. NULL unless this statement ended in '&'. */
    char *bg_src;
    union {
        struct pipeline *pipe;                          /* STMT_PIPELINE */
        struct { char *name; struct expr *expr; } let;  /* STMT_LET (owned) */
        struct {                                        /* STMT_IF */
            struct expr  *cond;
            struct block *then_b;
            struct block *else_b;    /* NULL when there is no else */
        } iff;
        struct {                                        /* STMT_WHILE */
            struct expr  *cond;
            struct block *body;
        } wh;
        struct {                                        /* STMT_FOR */
            char         *var;       /* the loop variable name (owned) */
            struct expr  *seq;       /* what to walk: a list, table, or range */
            struct block *body;
        } fr;
        struct {                                        /* STMT_DEF */
            char         *name;      /* owned */
            char        **params;    /* owned names */
            size_t        nparams;
            struct block *body;
        } def;
        struct expr *ret;                               /* STMT_RETURN (may be NULL) */
    } u;
};

/* A brace-delimited sequence of statements -- also what a whole script is. */
struct block {
    struct stmt **stmts;
    size_t        n;
};

/* Parse one line's tokens (from lex(); must end in TOK_EOF). On success
 * returns an owned stmt. On error returns NULL and writes a human message
 * ("3:14: expected expression after '|'") into err (NUL-terminated,
 * truncated to errcap). A TOK_ERROR from the lexer is surfaced the same way. */
struct stmt *parse(const struct token *toks, size_t ntoks, char *err, size_t errcap);

/* Parse a whole PROGRAM: any number of statements separated by newlines or
 * ';', including nested blocks. What a script file and the REPL both run --
 * the REPL is simply a program that arrives one piece at a time.
 *
 * `parse()` above is the single-statement form and stays: it is a strictly
 * narrower thing (one statement, then EOF), it is what the parser's own tests
 * pin, and keeping it means the addition of a program grammar cannot quietly
 * change what a single line means. */
struct block *parse_program(const struct token *toks, size_t ntoks,
                            char *err, size_t errcap);

void expr_free(struct expr *e);          /* recursive; NULL-safe */
void pipeline_free(struct pipeline *p);  /* NULL-safe */
void stmt_free(struct stmt *s);          /* NULL-safe */
void block_free(struct block *b);        /* NULL-safe; recursive */

#endif /* __EMBK_PARSE_H__ */
