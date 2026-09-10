/* ==========================================================================
 * parse.c -- see parse.h. A Pratt (precedence-climbing) expression parser
 * plus the flat pipeline/command grammar around it:
 *
 *   line     := "let" IDENT "=" expr  |  pipeline
 *   pipeline := command ( "|" command )*
 *   command  := IDENT expr*                 (args end at '|' / EOF / ')')
 *   expr     := the Pratt grammar over the locked precedence table
 *
 * One documented consequence of "args are expressions" + maximal-munch:
 * `echo a - b` is ONE argument (the subtraction a-b), not three. Quote or
 * parenthesize when a literal word sequence is wanted.
 * ========================================================================== */
#include "parse/parse.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Parser state: a cursor over the token array + the error sink. All error
 * paths funnel through perr() so every message carries line:col.
 * ------------------------------------------------------------------------- */
struct parser {
    const struct token *toks;
    size_t              n, pos;
    char               *err;
    size_t              errcap;
    bool                failed;
};

static const struct token *pk(struct parser *P)  { return &P->toks[P->pos]; }
static const struct token *adv(struct parser *P) { return &P->toks[P->pos < P->n - 1 ? P->pos++ : P->pos]; }

static void perr(struct parser *P, const struct token *at, const char *msg) {
    if (P->failed) return;               /* keep the FIRST error */
    P->failed = true;
    snprintf(P->err, P->errcap, "%zu:%zu: %s", at->line, at->col, msg);
}

static char *copy_slice(const char *s, size_t n) {
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* -------------------------------------------------------------------------
 * AST teardown (needed by every error path below, so defined first).
 * ------------------------------------------------------------------------- */
void expr_free(struct expr *e) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_LITERAL: value_free(&e->u.literal); break;
    case EXPR_VAR:
    case EXPR_COLUMN:  free(e->u.var.name); break;
    case EXPR_FIELD:   expr_free(e->u.field.obj); free(e->u.field.field); break;
    case EXPR_UNARY:   expr_free(e->u.unary.operand); break;
    case EXPR_CAPTURE: pipeline_free(e->u.capture); break;
    case EXPR_BINARY:  expr_free(e->u.binary.lhs); expr_free(e->u.binary.rhs); break;
    }
    free(e);
}

static void command_free(struct command *c) {
    if (!c) return;
    free(c->name);
    for (size_t i = 0; i < c->nargs; i++) expr_free(c->args[i]);
    free(c->args);
    free(c);
}

void pipeline_free(struct pipeline *p) {
    if (!p) return;
    for (size_t i = 0; i < p->nstages; i++) command_free(p->stages[i]);
    free(p->stages);
    free(p);
}

void block_free(struct block *b) {
    if (!b) return;
    for (size_t i = 0; i < b->n; i++) stmt_free(b->stmts[i]);
    free(b->stmts);
    free(b);
}

void stmt_free(struct stmt *s) {
    if (!s) return;
    free(s->bg_src);
    switch (s->kind) {
    case STMT_PIPELINE: pipeline_free(s->u.pipe); break;
    case STMT_LET:      free(s->u.let.name); expr_free(s->u.let.expr); break;
    case STMT_IF:
        expr_free(s->u.iff.cond);
        block_free(s->u.iff.then_b);
        block_free(s->u.iff.else_b);
        break;
    case STMT_WHILE:
        expr_free(s->u.wh.cond);
        block_free(s->u.wh.body);
        break;
    case STMT_FOR:
        free(s->u.fr.var);
        expr_free(s->u.fr.seq);
        block_free(s->u.fr.body);
        break;
    case STMT_DEF:
        free(s->u.def.name);
        for (size_t i = 0; i < s->u.def.nparams; i++) free(s->u.def.params[i]);
        free(s->u.def.params);
        block_free(s->u.def.body);
        break;
    case STMT_RETURN:   expr_free(s->u.ret); break;
    case STMT_BREAK:
    case STMT_CONTINUE: break;
    }
    free(s);
}

/* -------------------------------------------------------------------------
 * Node constructors. Every one can fail (OOM) -> perr + NULL; callers free
 * whatever they already own on NULL.
 * ------------------------------------------------------------------------- */
static struct expr *expr_new(struct parser *P, enum expr_kind k, const struct token *at) {
    struct expr *e = (struct expr *)calloc(1, sizeof(*e));
    if (!e) { perr(P, at, "out of memory"); return NULL; }
    e->kind = k;
    e->line = at->line;
    e->col  = at->col;
    return e;
}

/* -------------------------------------------------------------------------
 * The precedence table (parse.h's contract). Binding powers: an infix
 * operator binds its right side at (bp + 1) for left-assoc. Comparisons get
 * NON_CHAIN marking: after one comparison at that level, another comparison
 * immediately following is rejected.
 * ------------------------------------------------------------------------- */
enum { BP_NONE = 0, BP_OR = 1, BP_AND = 2, BP_CMP = 3, BP_ADD = 4, BP_MUL = 5,
       BP_UNARY = 6, BP_POSTFIX = 7 };

static int infix_bp(enum tok_type t) {
    switch (t) {
    case TOK_OR:  return BP_OR;
    case TOK_AND: return BP_AND;
    case TOK_EQ: case TOK_NE: case TOK_LT: case TOK_GT:
    case TOK_LE: case TOK_GE: case TOK_MATCH: return BP_CMP;
    case TOK_PLUS: case TOK_MINUS: return BP_ADD;
    case TOK_STAR: case TOK_SLASH: return BP_MUL;
    case TOK_DOT: return BP_POSTFIX;
    default: return BP_NONE;
    }
}
static bool is_comparison(enum tok_type t) {
    return t == TOK_EQ || t == TOK_NE || t == TOK_LT || t == TOK_GT ||
           t == TOK_LE || t == TOK_GE || t == TOK_MATCH;
}

static struct expr *parse_expr(struct parser *P, int min_bp);
static struct pipeline *parse_pipeline(struct parser *P);

/* --- prefix position: literals, names, unary ops, parens --- */
static struct expr *parse_prefix(struct parser *P) {
    const struct token *t = pk(P);
    switch (t->type) {
    case TOK_INT: {
        adv(P);
        struct expr *e = expr_new(P, EXPR_LITERAL, t);
        if (e) e->u.literal = value_int(t->int_val);
        return e;
    }
    case TOK_FLOAT: {
        adv(P);
        struct expr *e = expr_new(P, EXPR_LITERAL, t);
        if (e) e->u.literal = value_float(t->float_val);
        return e;
    }
    case TOK_FILESIZE: {
        adv(P);
        struct expr *e = expr_new(P, EXPR_LITERAL, t);
        if (e) e->u.literal = value_filesize(t->int_val);
        return e;
    }
    case TOK_STRING: {
        adv(P);
        struct expr *e = expr_new(P, EXPR_LITERAL, t);
        if (e) {
            e->u.literal = value_string_n(t->str, t->str_len);
            if (e->u.literal.type != VAL_STRING && t->str_len) {
                perr(P, t, "out of memory");
                expr_free(e);
                return NULL;
            }
        }
        return e;
    }
    case TOK_TRUE: case TOK_FALSE: {
        adv(P);
        struct expr *e = expr_new(P, EXPR_LITERAL, t);
        if (e) e->u.literal = value_bool(t->type == TOK_TRUE);
        return e;
    }
    case TOK_IDENT: {
        adv(P);
        struct expr *e = expr_new(P, EXPR_COLUMN, t);
        if (!e) return NULL;
        e->u.var.name = copy_slice(t->lexeme, t->lexeme_len);
        if (!e->u.var.name) { perr(P, t, "out of memory"); free(e); return NULL; }
        return e;
    }
    case TOK_DOLLAR_IDENT: {
        adv(P);
        struct expr *e = expr_new(P, EXPR_VAR, t);
        if (!e) return NULL;
        e->u.var.name = copy_slice(t->lexeme, t->lexeme_len);
        if (!e->u.var.name) { perr(P, t, "out of memory"); free(e); return NULL; }
        return e;
    }
    case TOK_NOT: case TOK_MINUS: {
        adv(P);
        struct expr *operand = parse_expr(P, BP_UNARY);
        if (!operand) return NULL;
        struct expr *e = expr_new(P, EXPR_UNARY, t);
        if (!e) { expr_free(operand); return NULL; }
        e->u.unary.op = (int)t->type;
        e->u.unary.operand = operand;
        return e;
    }
    case TOK_DOLLAR_LPAREN: {
        /* $( pipeline ) -- newlines inside are insignificant: the thing is
         * bracketed, so "where does it end" is already answered. */
        adv(P);
        while (pk(P)->type == TOK_NEWLINE) adv(P);
        struct pipeline *pl = parse_pipeline(P);
        if (!pl) return NULL;
        while (pk(P)->type == TOK_NEWLINE) adv(P);
        if (pk(P)->type != TOK_RPAREN) {
            /* `$(cmd &)` asks for the VALUE of something that has not run yet.
             * Saying that is worth four lines: the generic "expected ')'" sends
             * the reader hunting for a typo that is not there. */
            perr(P, pk(P), pk(P)->type == TOK_AMP
                    ? "a background job has no value yet -- write `cmd &` as its own "
                      "statement and use `jobs` or `fg`"
                    : "expected ')' to close '$('");
            pipeline_free(pl);
            return NULL;
        }
        adv(P);
        struct expr *e = expr_new(P, EXPR_CAPTURE, t);
        if (!e) { pipeline_free(pl); return NULL; }
        e->u.capture = pl;
        return e;
    }
    case TOK_LPAREN: {
        adv(P);
        struct expr *inner = parse_expr(P, BP_NONE);
        if (!inner) return NULL;
        if (pk(P)->type != TOK_RPAREN) {
            perr(P, pk(P), "expected ')'");
            expr_free(inner);
            return NULL;
        }
        adv(P);
        return inner;
    }
    case TOK_SLASH: {
        /* A '/' in PREFIX position is the bare word "/" -- the root path
         * (`ls /`). In INFIX position (the Pratt loop) it stays division;
         * the grammar position disambiguates what the lexer can't. */
        adv(P);
        struct expr *e = expr_new(P, EXPR_COLUMN, t);
        if (!e) return NULL;
        e->u.var.name = copy_slice("/", 1);
        if (!e->u.var.name) { perr(P, t, "out of memory"); free(e); return NULL; }
        return e;
    }
    case TOK_ERROR:
        perr(P, t, t->lexeme ? t->lexeme : "lex error");
        return NULL;
    default:
        perr(P, t, "expected an expression");
        return NULL;
    }
}

/* --- the Pratt loop --- */
static struct expr *parse_expr(struct parser *P, int min_bp) {
    struct expr *lhs = parse_prefix(P);
    if (!lhs) return NULL;

    bool saw_comparison = false;   /* non-chaining enforcement, per level */

    for (;;) {
        const struct token *t = pk(P);
        int bp = infix_bp(t->type);
        if (bp == BP_NONE || bp < min_bp) break;

        /* postfix field access binds tightest: expr '.' IDENT */
        if (t->type == TOK_DOT) {
            adv(P);
            const struct token *name = pk(P);
            if (name->type != TOK_IDENT) {
                perr(P, name, "expected a field name after '.'");
                expr_free(lhs);
                return NULL;
            }
            adv(P);
            struct expr *f = expr_new(P, EXPR_FIELD, t);
            if (!f) { expr_free(lhs); return NULL; }
            f->u.field.obj = lhs;
            f->u.field.field = copy_slice(name->lexeme, name->lexeme_len);
            if (!f->u.field.field) { perr(P, name, "out of memory"); expr_free(f); return NULL; }
            lhs = f;
            continue;
        }

        if (is_comparison(t->type)) {
            if (saw_comparison) {
                perr(P, t, "comparisons don't chain (use 'and': a < b and b < c)");
                expr_free(lhs);
                return NULL;
            }
            saw_comparison = true;
        }

        adv(P);
        struct expr *rhs = parse_expr(P, bp + 1);   /* +1: left-assoc */
        if (!rhs) { expr_free(lhs); return NULL; }
        struct expr *b = expr_new(P, EXPR_BINARY, t);
        if (!b) { expr_free(lhs); expr_free(rhs); return NULL; }
        b->u.binary.op  = (int)t->type;
        b->u.binary.lhs = lhs;
        b->u.binary.rhs = rhs;
        lhs = b;
    }
    return lhs;
}

/* -------------------------------------------------------------------------
 * command := IDENT expr*    (args stop at '|', ')' or EOF)
 * ------------------------------------------------------------------------- */
static struct command *parse_command(struct parser *P) {
    const struct token *name = pk(P);
    if (name->type != TOK_IDENT) {
        perr(P, name, "expected a command name");
        return NULL;
    }
    adv(P);

    struct command *cmd = (struct command *)calloc(1, sizeof(*cmd));
    if (!cmd) { perr(P, name, "out of memory"); return NULL; }
    cmd->name = copy_slice(name->lexeme, name->lexeme_len);
    cmd->line = name->line;
    cmd->col  = name->col;
    if (!cmd->name) { perr(P, name, "out of memory"); free(cmd); return NULL; }

    size_t cap = 0;
    /* Arguments stop at anything that ENDS a command. The three new ones are
     * the statement separators and '{': without them, `if ok { echo hi }`
     * would swallow the brace as another argument to `ok` and the block would
     * never be seen. '}' is here for the same reason, one level out. */
    while (pk(P)->type != TOK_PIPE   && pk(P)->type != TOK_EOF &&
           pk(P)->type != TOK_RPAREN && pk(P)->type != TOK_NEWLINE &&
           pk(P)->type != TOK_SEMI   && pk(P)->type != TOK_LBRACE &&
           pk(P)->type != TOK_RBRACE && pk(P)->type != TOK_AMP) {
        struct expr *arg = parse_expr(P, BP_NONE);
        if (!arg) { command_free(cmd); return NULL; }
        if (cmd->nargs == cap) {
            size_t nc = cap ? cap * 2 : 4;
            struct expr **na = (struct expr **)realloc(cmd->args, nc * sizeof(*na));
            if (!na) { perr(P, pk(P), "out of memory"); expr_free(arg); command_free(cmd); return NULL; }
            cmd->args = na;
            cap = nc;
        }
        cmd->args[cmd->nargs++] = arg;
    }
    return cmd;
}

/* -------------------------------------------------------------------------
 * pipeline := command ('|' command)*
 * ------------------------------------------------------------------------- */
static struct pipeline *parse_pipeline(struct parser *P) {
    struct pipeline *pl = (struct pipeline *)calloc(1, sizeof(*pl));
    if (!pl) { perr(P, pk(P), "out of memory"); return NULL; }

    size_t cap = 0;
    for (;;) {
        struct command *cmd = parse_command(P);
        if (!cmd) { pipeline_free(pl); return NULL; }
        if (pl->nstages == cap) {
            size_t nc = cap ? cap * 2 : 4;
            struct command **ns = (struct command **)realloc(pl->stages, nc * sizeof(*ns));
            if (!ns) { perr(P, pk(P), "out of memory"); command_free(cmd); pipeline_free(pl); return NULL; }
            pl->stages = ns;
            cap = nc;
        }
        pl->stages[pl->nstages++] = cmd;

        if (pk(P)->type != TOK_PIPE) break;
        adv(P);                                   /* consume '|' */
        /* A line that ends in '|' obviously continues. This is the one place
         * a newline is NOT a separator, and it is why long pipelines can be
         * written down the page instead of off the right of the screen. */
        while (pk(P)->type == TOK_NEWLINE) adv(P);
        if (pk(P)->type == TOK_EOF) {
            perr(P, pk(P), "expected a command after '|'");
            pipeline_free(pl);
            return NULL;
        }
    }
    return pl;
}

/* -------------------------------------------------------------------------
 * STATEMENTS AND BLOCKS -- the grammar that turned this from a launcher into
 * a language.
 *
 *   program := sep* ( stmt ( sep+ stmt )* )? sep*
 *   sep     := NEWLINE | ';'
 *   stmt    := "let" IDENT "=" expr
 *            | "if" expr block ( "else" ( block | if-stmt ) )?
 *            | "while" expr block
 *            | "for" IDENT "in" expr block
 *            | "def" IDENT ( "(" IDENT ("," IDENT)* ")" )? block
 *            | "break" | "continue" | "return" expr?
 *            | pipeline
 *   block   := "{" program "}"
 *
 * `else if` is parsed as an else-block containing one if-statement rather
 * than as its own keyword. One rule, arbitrary depth, and nothing in the
 * evaluator has to know the difference.
 * ------------------------------------------------------------------------- */
static struct block *parse_block_braced(struct parser *P);
static struct stmt  *parse_stmt(struct parser *P);

static bool is_sep(enum tok_type t) { return t == TOK_NEWLINE || t == TOK_SEMI; }

static void skip_seps(struct parser *P) {
    while (is_sep(pk(P)->type)) adv(P);
}

static struct stmt *stmt_new(struct parser *P, enum stmt_kind k, const struct token *at) {
    struct stmt *s = (struct stmt *)calloc(1, sizeof(*s));
    if (!s) { perr(P, at, "out of memory"); return NULL; }
    s->kind = k;
    s->line = at->line;
    s->col  = at->col;
    return s;
}

static int block_push(struct parser *P, struct block *b, struct stmt *st, size_t *cap) {
    if (b->n == *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        struct stmt **ns = (struct stmt **)realloc(b->stmts, nc * sizeof(*ns));
        if (!ns) { perr(P, pk(P), "out of memory"); return -1; }
        b->stmts = ns;
        *cap = nc;
    }
    b->stmts[b->n++] = st;
    return 0;
}

/* Statements until `end` (TOK_RBRACE inside a block, TOK_EOF at top level). */
static struct block *parse_stmts_until(struct parser *P, enum tok_type end) {
    struct block *b = (struct block *)calloc(1, sizeof(*b));
    if (!b) { perr(P, pk(P), "out of memory"); return NULL; }

    size_t cap = 0;
    for (;;) {
        skip_seps(P);
        if (pk(P)->type == end || pk(P)->type == TOK_EOF) break;

        struct stmt *st = parse_stmt(P);
        if (!st) { block_free(b); return NULL; }
        if (block_push(P, b, st, &cap) != 0) { stmt_free(st); block_free(b); return NULL; }

        /* After a statement the only legal things are a separator or the
         * end of the block. Saying so here is what turns `echo a echo b`
         * into an error instead of one command with three arguments. */
        if (is_sep(pk(P)->type)) continue;
        if (pk(P)->type == end || pk(P)->type == TOK_EOF) break;
        perr(P, pk(P), "expected a newline or ';' between statements");
        block_free(b);
        return NULL;
    }
    return b;
}

static struct block *parse_block_braced(struct parser *P) {
    if (pk(P)->type != TOK_LBRACE) {
        perr(P, pk(P), "expected '{'");
        return NULL;
    }
    adv(P);
    struct block *b = parse_stmts_until(P, TOK_RBRACE);
    if (!b) return NULL;
    if (pk(P)->type != TOK_RBRACE) {
        perr(P, pk(P), "expected '}' to close the block");
        block_free(b);
        return NULL;
    }
    adv(P);
    return b;
}

static struct stmt *parse_if(struct parser *P) {
    const struct token *kw = pk(P);
    adv(P);                                   /* 'if' */

    struct stmt *s = stmt_new(P, STMT_IF, kw);
    if (!s) return NULL;

    s->u.iff.cond = parse_expr(P, BP_NONE);
    if (!s->u.iff.cond) { stmt_free(s); return NULL; }

    s->u.iff.then_b = parse_block_braced(P);
    if (!s->u.iff.then_b) { stmt_free(s); return NULL; }

    /* `else` may sit on the line after '}' -- which reads naturally and would
     * otherwise be a separator ending the statement. Look past newlines for
     * it, but ONLY for it: anything else and the if is complete. */
    size_t save = P->pos;
    while (pk(P)->type == TOK_NEWLINE) adv(P);
    if (pk(P)->type != TOK_ELSE) { P->pos = save; return s; }
    adv(P);                                   /* 'else' */

    if (pk(P)->type == TOK_IF) {
        /* else-if: an else block holding exactly one if. */
        struct stmt *inner = parse_if(P);
        if (!inner) { stmt_free(s); return NULL; }
        struct block *b = (struct block *)calloc(1, sizeof(*b));
        if (!b) { perr(P, kw, "out of memory"); stmt_free(inner); stmt_free(s); return NULL; }
        size_t cap = 0;
        if (block_push(P, b, inner, &cap) != 0) { stmt_free(inner); free(b); stmt_free(s); return NULL; }
        s->u.iff.else_b = b;
        return s;
    }

    s->u.iff.else_b = parse_block_braced(P);
    if (!s->u.iff.else_b) { stmt_free(s); return NULL; }
    return s;
}

static struct stmt *parse_while(struct parser *P) {
    const struct token *kw = pk(P);
    adv(P);
    struct stmt *s = stmt_new(P, STMT_WHILE, kw);
    if (!s) return NULL;
    s->u.wh.cond = parse_expr(P, BP_NONE);
    if (!s->u.wh.cond) { stmt_free(s); return NULL; }
    s->u.wh.body = parse_block_braced(P);
    if (!s->u.wh.body) { stmt_free(s); return NULL; }
    return s;
}

static struct stmt *parse_for(struct parser *P) {
    const struct token *kw = pk(P);
    adv(P);
    struct stmt *s = stmt_new(P, STMT_FOR, kw);
    if (!s) return NULL;

    const struct token *var = pk(P);
    if (var->type != TOK_IDENT) {
        perr(P, var, "expected a loop variable name after 'for'");
        stmt_free(s);
        return NULL;
    }
    adv(P);
    s->u.fr.var = copy_slice(var->lexeme, var->lexeme_len);
    if (!s->u.fr.var) { perr(P, var, "out of memory"); stmt_free(s); return NULL; }

    if (pk(P)->type != TOK_IN) {
        perr(P, pk(P), "expected 'in' after the loop variable");
        stmt_free(s);
        return NULL;
    }
    adv(P);

    s->u.fr.seq = parse_expr(P, BP_NONE);
    if (!s->u.fr.seq) { stmt_free(s); return NULL; }
    s->u.fr.body = parse_block_braced(P);
    if (!s->u.fr.body) { stmt_free(s); return NULL; }
    return s;
}

static struct stmt *parse_def(struct parser *P) {
    const struct token *kw = pk(P);
    adv(P);
    struct stmt *s = stmt_new(P, STMT_DEF, kw);
    if (!s) return NULL;

    const struct token *name = pk(P);
    if (name->type != TOK_IDENT) {
        perr(P, name, "expected a command name after 'def'");
        stmt_free(s);
        return NULL;
    }
    adv(P);
    s->u.def.name = copy_slice(name->lexeme, name->lexeme_len);
    if (!s->u.def.name) { perr(P, name, "out of memory"); stmt_free(s); return NULL; }

    /* The parameter list is optional: `def hello { ... }` is a command that
     * takes nothing, and having to write `()` for it would be ceremony. */
    if (pk(P)->type == TOK_LPAREN) {
        adv(P);
        size_t cap = 0;
        while (pk(P)->type != TOK_RPAREN) {
            const struct token *pt = pk(P);
            if (pt->type != TOK_IDENT) {
                perr(P, pt, "expected a parameter name");
                stmt_free(s);
                return NULL;
            }
            adv(P);
            if (s->u.def.nparams == cap) {
                size_t nc = cap ? cap * 2 : 4;
                char **np = (char **)realloc(s->u.def.params, nc * sizeof(*np));
                if (!np) { perr(P, pt, "out of memory"); stmt_free(s); return NULL; }
                s->u.def.params = np;
                cap = nc;
            }
            s->u.def.params[s->u.def.nparams] = copy_slice(pt->lexeme, pt->lexeme_len);
            if (!s->u.def.params[s->u.def.nparams]) {
                perr(P, pt, "out of memory"); stmt_free(s); return NULL;
            }
            s->u.def.nparams++;

            if (pk(P)->type == TOK_COMMA) { adv(P); continue; }
            if (pk(P)->type != TOK_RPAREN) {
                perr(P, pk(P), "expected ',' or ')' in the parameter list");
                stmt_free(s);
                return NULL;
            }
        }
        adv(P);                               /* ')' */
    }

    s->u.def.body = parse_block_braced(P);
    if (!s->u.def.body) { stmt_free(s); return NULL; }
    return s;
}

static struct stmt *parse_let(struct parser *P) {
    const struct token *kw = pk(P);
    adv(P);
    const struct token *name = pk(P);
    if (name->type != TOK_IDENT) {
        perr(P, name, "expected a variable name after 'let'");
        return NULL;
    }
    adv(P);
    if (pk(P)->type != TOK_ASSIGN) {
        perr(P, pk(P), "expected '=' in let binding");
        return NULL;
    }
    adv(P);
    struct expr *e = parse_expr(P, BP_NONE);
    if (!e) return NULL;

    struct stmt *s = stmt_new(P, STMT_LET, kw);
    if (!s) { expr_free(e); return NULL; }
    s->u.let.name = copy_slice(name->lexeme, name->lexeme_len);
    s->u.let.expr = e;
    if (!s->u.let.name) { perr(P, name, "out of memory"); stmt_free(s); return NULL; }
    return s;
}

static struct stmt *parse_stmt(struct parser *P) {
    const struct token *t = pk(P);
    switch (t->type) {
    case TOK_LET:   return parse_let(P);
    case TOK_IF:    return parse_if(P);
    case TOK_WHILE: return parse_while(P);
    case TOK_FOR:   return parse_for(P);
    case TOK_DEF:   return parse_def(P);
    case TOK_BREAK:
    case TOK_CONTINUE: {
        struct stmt *s = stmt_new(P, t->type == TOK_BREAK ? STMT_BREAK : STMT_CONTINUE, t);
        if (s) adv(P);
        return s;
    }
    case TOK_RETURN: {
        struct stmt *s = stmt_new(P, STMT_RETURN, t);
        if (!s) return NULL;
        adv(P);
        /* `return` alone is legal; a value is optional. Anything that ends a
         * statement means there is no expression to read. */
        if (!is_sep(pk(P)->type) && pk(P)->type != TOK_EOF &&
            pk(P)->type != TOK_RBRACE) {
            s->u.ret = parse_expr(P, BP_NONE);
            if (!s->u.ret) { stmt_free(s); return NULL; }
        }
        return s;
    }
    default: {
        const struct token *first = pk(P);
        struct pipeline *pl = parse_pipeline(P);
        if (!pl) return NULL;
        struct stmt *s = stmt_new(P, STMT_PIPELINE, t);
        if (!s) { pipeline_free(pl); return NULL; }
        s->u.pipe = pl;

        /* A trailing '&' backgrounds the statement. The source text is copied
         * out HERE, while the tokens still point into it -- from the first
         * token of the pipeline to the last one before the '&'. */
        if (pk(P)->type == TOK_AMP) {
            const struct token *last = &P->toks[P->pos - 1];
            const char *b = first->lexeme;
            const char *e = last->lexeme + last->lexeme_len;
            if (b && e > b) {
                s->bg_src = copy_slice(b, (size_t)(e - b));
                if (!s->bg_src) { perr(P, first, "out of memory"); stmt_free(s); return NULL; }
            } else {
                perr(P, first, "cannot background this statement");
                stmt_free(s);
                return NULL;
            }
            adv(P);                        /* consume '&' */
        }
        return s;
    }
    }
}

struct block *parse_program(const struct token *toks, size_t ntoks,
                            char *err, size_t errcap) {
    struct parser P = { toks, ntoks, 0, err, errcap, false };
    if (errcap) err[0] = '\0';

    for (size_t i = 0; i < ntoks; i++) {
        if (toks[i].type == TOK_ERROR) {
            perr(&P, &toks[i], toks[i].lexeme ? toks[i].lexeme : "lex error");
            return NULL;
        }
    }

    struct block *b = parse_stmts_until(&P, TOK_EOF);
    if (!b) return NULL;
    if (pk(&P)->type != TOK_EOF) {
        perr(&P, pk(&P), "unexpected trailing input");
        block_free(b);
        return NULL;
    }
    return b;
}

/* -------------------------------------------------------------------------
 * The entry point: let-statement or pipeline, then require EOF (trailing
 * tokens are an error, not silently ignored).
 * ------------------------------------------------------------------------- */
struct stmt *parse(const struct token *toks, size_t ntoks, char *err, size_t errcap) {
    struct parser P = { toks, ntoks, 0, err, errcap, false };
    if (errcap) err[0] = '\0';

    /* surface a lexer error immediately, wherever it sits */
    for (size_t i = 0; i < ntoks; i++) {
        if (toks[i].type == TOK_ERROR) {
            perr(&P, &toks[i], toks[i].lexeme ? toks[i].lexeme : "lex error");
            return NULL;
        }
    }

    /* Newlines are TOKENS now (lex.h). A single-statement caller means "this
     * one statement", so blank lines around it are not input -- skip them
     * rather than making every existing caller strip its own whitespace. */
    while (pk(&P)->type == TOK_NEWLINE) adv(&P);

    struct stmt *s = (struct stmt *)calloc(1, sizeof(*s));
    if (!s) { perr(&P, pk(&P), "out of memory"); return NULL; }

    if (pk(&P)->type == TOK_LET) {
        adv(&P);
        const struct token *name = pk(&P);
        if (name->type != TOK_IDENT) {
            perr(&P, name, "expected a variable name after 'let'");
            free(s);
            return NULL;
        }
        adv(&P);
        if (pk(&P)->type != TOK_ASSIGN) {
            perr(&P, pk(&P), "expected '=' in let binding");
            free(s);
            return NULL;
        }
        adv(&P);
        struct expr *e = parse_expr(&P, BP_NONE);
        if (!e) { free(s); return NULL; }
        s->kind = STMT_LET;
        s->u.let.name = copy_slice(name->lexeme, name->lexeme_len);
        s->u.let.expr = e;
        if (!s->u.let.name) { perr(&P, name, "out of memory"); stmt_free(s); return NULL; }
    } else {
        struct pipeline *pl = parse_pipeline(&P);
        if (!pl) { free(s); return NULL; }
        s->kind = STMT_PIPELINE;
        s->u.pipe = pl;
    }

    while (pk(&P)->type == TOK_NEWLINE || pk(&P)->type == TOK_SEMI) adv(&P);
    if (pk(&P)->type != TOK_EOF) {
        perr(&P, pk(&P), "unexpected trailing input");
        stmt_free(s);
        return NULL;
    }
    return s;
}
