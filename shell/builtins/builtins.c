/* ==========================================================================
 * builtins.c -- the PURE builtins (no OS dependency; host-testable):
 * echo, where, select, sort-by, first, count, plus the registry.
 * ls (OS-backed) lives in builtins_os.c.
 * ========================================================================== */
#include "builtins/builtins.h"
#include "hist/hist.h"
#include "prompt/prompt.h"
#include "prompt/wizard_os.h"
#include "sval/sval.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Shared helpers
 * ------------------------------------------------------------------------- */
static struct value err_args(const struct command *cmd, const char *what) {
    char msg[96];
    snprintf(msg, sizeof msg, "%s: %s", cmd->name, what);
    return value_error(msg);
}

/* Most row-transforms want "the input as rows": a TABLE. (A LIST of records
 * would be honest too, but v1 keeps it to tables -- ls and every transform
 * emit tables, so a list here means the user piped something odd.) */
static struct table *input_table(const struct command *cmd, struct value *input,
                                 struct value *out_err) {
    if (input->type != VAL_TABLE) {
        char msg[96];
        snprintf(msg, sizeof msg, "%s: expected a table from the previous stage (got %s)",
                 cmd->name, input->type == VAL_NULL ? "nothing" : "a non-table value");
        *out_err = value_error(msg);
        return NULL;
    }
    return input->u.table;
}

/* -------------------------------------------------------------------------
 * echo expr* -- evaluate args in the driver scope. The identity/constructor
 * builtin: `echo 1mb + 512kb` prints a filesize, `echo $x` prints a binding.
 * ------------------------------------------------------------------------- */
static struct value bi_echo(const struct command *cmd, struct value input,
                            struct scope *env) {
    value_free(&input);   /* echo ignores pipeline input */
    if (cmd->nargs == 0) return value_null();
    if (cmd->nargs == 1) return expr_eval(cmd->args[0], env);
    struct value list = value_list();
    for (size_t i = 0; i < cmd->nargs; i++) {
        struct value v = expr_eval(cmd->args[i], env);
        if (v.type == VAL_ERROR) { value_free(&list); return v; }
        value_list_push(&list, v);
    }
    return list;
}

/* -------------------------------------------------------------------------
 * where <expr> -- keep rows where the expr is TRUE (strict truthiness: a
 * non-boolean predicate result is an error naming the offending type).
 * ------------------------------------------------------------------------- */
static struct value bi_where(const struct command *cmd, struct value input,
                             struct scope *env) {
    if (cmd->nargs != 1) {
        value_free(&input);
        return err_args(cmd, "takes exactly one condition (e.g. where size > 1mb)");
    }
    struct value err;
    struct table *t = input_table(cmd, &input, &err);
    if (!t) { value_free(&input); return err; }

    struct value out = value_table();
    for (size_t i = 0; i < t->count; i++) {
        struct scope rowscope;
        scope_init(&rowscope, env);
        rowscope.row = &t->rows[i];
        struct value cond = expr_eval(cmd->args[0], &rowscope);
        scope_free(&rowscope);

        if (cond.type == VAL_ERROR) { value_free(&out); value_free(&input); return cond; }
        int truth = value_truthy(&cond);
        value_free(&cond);
        if (truth < 0) {
            value_free(&out);
            value_free(&input);
            return err_args(cmd, "condition must be a boolean (use ==, >, =~ ...)");
        }
        if (truth)
            value_table_push_row(&out, record_clone_value(&t->rows[i]));
    }
    value_free(&input);
    return out;
}

/* -------------------------------------------------------------------------
 * select col* -- project columns, in the order named. A row missing a named
 * field gets a null cell (heterogeneous rows are legal).
 * ------------------------------------------------------------------------- */
static struct value bi_select(const struct command *cmd, struct value input,
                              struct scope *env) {
    (void)env;
    if (cmd->nargs == 0) {
        value_free(&input);
        return err_args(cmd, "needs column names (e.g. select name size)");
    }
    struct value err;
    struct table *t = input_table(cmd, &input, &err);
    if (!t) { value_free(&input); return err; }

    /* column names come from the exprs as WORDS (bare idents / strings) */
    const char *cols[16];
    if (cmd->nargs > 16) { value_free(&input); return err_args(cmd, "too many columns (max 16)"); }
    for (size_t i = 0; i < cmd->nargs; i++) {
        cols[i] = expr_as_word(cmd->args[i]);
        if (!cols[i]) { value_free(&input); return err_args(cmd, "column names must be plain words"); }
    }

    struct value out = value_table();
    for (size_t r = 0; r < t->count; r++) {
        struct value row = value_record();
        for (size_t c = 0; c < cmd->nargs; c++) {
            const struct value *f = record_field(&t->rows[r], cols[c]);
            value_record_set(&row, cols[c], f ? value_clone(f) : value_null());
        }
        value_table_push_row(&out, row);
    }
    value_free(&input);
    return out;
}

/* -------------------------------------------------------------------------
 * sort-by <expr> -- ascending by the key expr, evaluated once per row.
 * Insertion sort: stable, no qsort_r dependency, fine at shell scale.
 * Unorderable key pairs (mixed types) are an error, not a silent shuffle.
 * ------------------------------------------------------------------------- */
static struct value bi_sort_by(const struct command *cmd, struct value input,
                               struct scope *env) {
    if (cmd->nargs != 1) {
        value_free(&input);
        return err_args(cmd, "takes exactly one key (e.g. sort-by size)");
    }
    struct value err;
    struct table *t = input_table(cmd, &input, &err);
    if (!t) { value_free(&input); return err; }

    size_t n = t->count;
    struct value *keys = n ? (struct value *)calloc(n, sizeof(*keys)) : NULL;
    if (n && !keys) { value_free(&input); return value_error("out of memory"); }

    for (size_t i = 0; i < n; i++) {
        struct scope rowscope;
        scope_init(&rowscope, env);
        rowscope.row = &t->rows[i];
        keys[i] = expr_eval(cmd->args[0], &rowscope);
        scope_free(&rowscope);
        if (keys[i].type == VAL_ERROR) {
            struct value e = keys[i];
            keys[i] = value_null();          /* don't double-free below */
            for (size_t k = 0; k < n; k++) value_free(&keys[k]);
            free(keys);
            value_free(&input);
            return e;
        }
    }

    /* insertion sort rows + keys in lockstep (rows are struct record by
     * value -- moving one is a struct copy, cheap: three pointers) */
    for (size_t i = 1; i < n; i++) {
        struct record row = t->rows[i];
        struct value  key = keys[i];
        size_t j = i;
        while (j > 0) {
            int c;
            if (eval_compare_values(&keys[j - 1], &key, &c) != 0) {
                for (size_t k = 0; k < n; k++) value_free(&keys[k]);
                free(keys);
                value_free(&input);
                return err_args(cmd, "rows have mixed key types; can't order them");
            }
            if (c <= 0) break;
            t->rows[j] = t->rows[j - 1];
            keys[j] = keys[j - 1];
            j--;
        }
        t->rows[j] = row;
        keys[j] = key;
    }

    for (size_t k = 0; k < n; k++) value_free(&keys[k]);
    free(keys);
    return input;   /* sorted in place; ownership passes through */
}

/* -------------------------------------------------------------------------
 * first [n] -- the first n rows of a table (or items of a list). Default 1.
 * ------------------------------------------------------------------------- */
static struct value bi_first(const struct command *cmd, struct value input,
                             struct scope *env) {
    int64_t n = 1;
    if (cmd->nargs >= 1) {
        struct value nv = expr_eval(cmd->args[0], env);
        if (nv.type == VAL_ERROR) { value_free(&input); return nv; }
        if (nv.type != VAL_INT || nv.u.i < 0) {
            value_free(&nv);
            value_free(&input);
            return err_args(cmd, "takes a non-negative count");
        }
        n = nv.u.i;
    }

    if (input.type == VAL_TABLE) {
        struct value out = value_table();
        struct table *t = input.u.table;
        for (size_t i = 0; i < t->count && (int64_t)i < n; i++)
            value_table_push_row(&out, record_clone_value(&t->rows[i]));
        value_free(&input);
        return out;
    }
    if (input.type == VAL_LIST) {
        struct value out = value_list();
        for (size_t i = 0; i < input.u.list.count && (int64_t)i < n; i++)
            value_list_push(&out, value_clone(&input.u.list.items[i]));
        value_free(&input);
        return out;
    }
    value_free(&input);
    return err_args(cmd, "expected a table or list from the previous stage");
}

/* -------------------------------------------------------------------------
 * assert <condition> [message] -- stop here unless the condition holds.
 *
 * What a script checks its own work with: `assert ($n == 3) "three lines"`
 * fails the program -- exit status 1 under -c or as a script -- with the
 * message, and passes its input through when it holds, so it can sit in the
 * middle of a pipeline. The condition is strict, like `if`'s: something that
 * is not true or false is itself an error, not a silent pass.
 * ------------------------------------------------------------------------- */
static struct value bi_assert(const struct command *cmd, struct value input,
                              struct scope *env) {
    if (cmd->nargs < 1 || cmd->nargs > 2) {
        value_free(&input);
        return value_error("assert: assert <condition> [message]");
    }
    struct value c = expr_eval(cmd->args[0], env);
    if (c.type == VAL_ERROR) { value_free(&input); return c; }
    int t = value_truthy(&c);
    value_free(&c);
    if (t == 1) return input;
    value_free(&input);
    if (t < 0) return value_error("assert: the condition is neither true nor false");
    char msg[200];
    if (cmd->nargs == 2) {
        struct value m = expr_eval(cmd->args[1], env);
        if (m.type == VAL_ERROR) return m;
        char text[160];
        sval_format_scalar(&m, text, sizeof text);
        value_free(&m);
        snprintf(msg, sizeof msg, "assertion failed: %s", text);
    } else {
        snprintf(msg, sizeof msg, "assertion failed (line %zu)", cmd->line);
    }
    return value_error(msg);
}

/* -------------------------------------------------------------------------
 * count -- how many rows / items; a scalar counts as 1, null as 0.
 * ------------------------------------------------------------------------- */
static struct value bi_count(const struct command *cmd, struct value input,
                             struct scope *env) {
    (void)cmd; (void)env;
    int64_t n;
    switch (input.type) {
    case VAL_TABLE: n = (int64_t)input.u.table->count; break;
    case VAL_LIST:  n = (int64_t)input.u.list.count;   break;
    case VAL_NULL:  n = 0; break;
    default:        n = 1; break;
    }
    value_free(&input);
    return value_int(n);
}

/* -------------------------------------------------------------------------
 * last [n] / tail [n] -- the LAST n rows/items (default 1). first's mirror.
 * ------------------------------------------------------------------------- */
static struct value bi_last(const struct command *cmd, struct value input,
                            struct scope *env) {
    int64_t n = 1;
    if (cmd->nargs >= 1) {
        struct value nv = expr_eval(cmd->args[0], env);
        if (nv.type == VAL_ERROR) { value_free(&input); return nv; }
        if (nv.type != VAL_INT || nv.u.i < 0) {
            value_free(&nv);
            value_free(&input);
            return err_args(cmd, "takes a non-negative count");
        }
        n = nv.u.i;
    }

    if (input.type == VAL_TABLE) {
        struct table *t = input.u.table;
        size_t start = (int64_t)t->count > n ? t->count - (size_t)n : 0;
        struct value out = value_table();
        for (size_t i = start; i < t->count; i++)
            value_table_push_row(&out, record_clone_value(&t->rows[i]));
        value_free(&input);
        return out;
    }
    if (input.type == VAL_LIST) {
        size_t start = (int64_t)input.u.list.count > n ? input.u.list.count - (size_t)n : 0;
        struct value out = value_list();
        for (size_t i = start; i < input.u.list.count; i++)
            value_list_push(&out, value_clone(&input.u.list.items[i]));
        value_free(&input);
        return out;
    }
    value_free(&input);
    return err_args(cmd, "expected a table or list from the previous stage");
}

/* -------------------------------------------------------------------------
 * reverse -- rows/items in reverse order.
 * ------------------------------------------------------------------------- */
static struct value bi_reverse(const struct command *cmd, struct value input,
                               struct scope *env) {
    (void)env;
    if (input.type == VAL_TABLE) {
        struct table *t = input.u.table;
        for (size_t i = 0, j = t->count; i + 1 < j; i++, j--) {
            struct record tmp = t->rows[i];
            t->rows[i] = t->rows[j - 1];
            t->rows[j - 1] = tmp;
        }
        return input;   /* reversed in place */
    }
    if (input.type == VAL_LIST) {
        for (size_t i = 0, j = input.u.list.count; i + 1 < j; i++, j--) {
            struct value tmp = input.u.list.items[i];
            input.u.list.items[i] = input.u.list.items[j - 1];
            input.u.list.items[j - 1] = tmp;
        }
        return input;
    }
    value_free(&input);
    return err_args(cmd, "expected a table or list from the previous stage");
}

/* -------------------------------------------------------------------------
 * get <field> -- a record's field, or a table column as a LIST. The bridge
 * from "rows" to "plain values" (`ls | get name`).
 * ------------------------------------------------------------------------- */
static struct value bi_get(const struct command *cmd, struct value input,
                           struct scope *env) {
    (void)env;
    if (cmd->nargs != 1) {
        value_free(&input);
        return err_args(cmd, "takes one field name (e.g. get name)");
    }
    const char *field = expr_as_word(cmd->args[0]);
    if (!field) { value_free(&input); return err_args(cmd, "field must be a plain word"); }

    if (input.type == VAL_RECORD) {
        const struct value *f = value_record_get(&input, field);
        struct value out = f ? value_clone(f) : value_null();
        if (!f) {
            value_free(&input);
            return err_args(cmd, "record has no such field");
        }
        value_free(&input);
        return out;
    }
    if (input.type == VAL_TABLE) {
        struct value out = value_list();
        struct table *t = input.u.table;
        for (size_t i = 0; i < t->count; i++) {
            const struct value *f = record_field(&t->rows[i], field);
            value_list_push(&out, f ? value_clone(f) : value_null());
        }
        value_free(&input);
        return out;
    }
    value_free(&input);
    return err_args(cmd, "expected a record or table from the previous stage");
}

/* -------------------------------------------------------------------------
 * wc -- counts. A string counts lines/words/bytes (cat file | wc); a
 * table/list reports its row/item count as a record for consistency.
 * ------------------------------------------------------------------------- */
static struct value bi_wc(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)env;
    struct value out = value_record();
    if (input.type == VAL_STRING || input.type == VAL_PATH) {
        int64_t lines = 0, words = 0, bytes = (int64_t)input.u.s.len;
        bool in_word = false;
        for (size_t i = 0; i < input.u.s.len; i++) {
            char c = input.u.s.bytes[i];
            if (c == '\n') lines++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') in_word = false;
            else if (!in_word) { in_word = true; words++; }
        }
        if (bytes && input.u.s.bytes[input.u.s.len - 1] != '\n') lines++;
        value_record_set(&out, "lines", value_int(lines));
        value_record_set(&out, "words", value_int(words));
        value_record_set(&out, "bytes", value_int(bytes));
    } else if (input.type == VAL_TABLE) {
        value_record_set(&out, "rows", value_int((int64_t)input.u.table->count));
    } else if (input.type == VAL_LIST) {
        value_record_set(&out, "items", value_int((int64_t)input.u.list.count));
    } else {
        value_free(&out);
        value_free(&input);
        return err_args(cmd, "expected text, a table or a list");
    }
    value_free(&input);
    return out;
}

/* -------------------------------------------------------------------------
 * history -- past commands, as a Table {n, command}. A table, of course:
 * `history | where command =~ "ls"` is the payoff of not printing text.
 * ------------------------------------------------------------------------- */
/* ==========================================================================
 * THE VOCABULARY A SCRIPT NEEDS
 *
 * `if`, `while` and `def` gave the shell control flow. Control flow with
 * nothing to compute is still not much: a script that can loop but cannot add
 * up a column, split a line, or produce a range of numbers has to shell out to
 * a program for every one of them, and there is no program for most.
 *
 * Everything below is PURE -- input value in, value out, no OS -- which is why
 * it lives in this file and is covered by the host tests rather than only by a
 * boot. That is not an accident of layout: a builtin that needs the machine to
 * be running is a builtin nobody can test cheaply, and these are exactly the
 * ones that want dozens of small cases.
 * ========================================================================== */

/* Is this value part of the numeric family? The evaluator's rule
 * (eval.h): INT / FLOAT / FILESIZE / DATE are mutually comparable, FLOAT
 * contaminates to double, and the "richer" tag survives arithmetic. The
 * aggregates below follow the same rule rather than inventing a second one. */
static bool num_of(const struct value *v, double *f, int64_t *i, bool *is_float) {
    switch (v->type) {
    case VAL_INT:      *i = v->u.i; *f = (double)v->u.i; *is_float = false; return true;
    case VAL_FILESIZE:
    case VAL_DATE:     *i = v->u.i; *f = (double)v->u.i; *is_float = false; return true;
    case VAL_FLOAT:    *f = v->u.f; *i = (int64_t)v->u.f; *is_float = true;  return true;
    default: return false;
    }
}

/* The values an aggregate should walk: a LIST's items, or -- when a column is
 * named -- that column of a TABLE. `ls | sum size` and `ls | get size | sum`
 * mean the same thing, and both should work: the first is what a person
 * writes, the second is what composes. */
struct agg_src {
    const struct value *items;   /* LIST arm: borrowed */
    size_t              n;
    const struct table *tbl;     /* TABLE arm */
    const char         *col;
};

static int agg_source(const struct command *cmd, struct value *input,
                      struct scope *env, struct agg_src *out, struct value *err) {
    memset(out, 0, sizeof *out);

    if (cmd->nargs > 1) { *err = err_args(cmd, "takes at most one column name"); return -1; }

    if (cmd->nargs == 1) {
        const char *col = expr_as_word(cmd->args[0]);
        if (!col) { *err = err_args(cmd, "expected a column name"); return -1; }
        if (input->type != VAL_TABLE) {
            *err = err_args(cmd, "a column name needs a table from the previous stage");
            return -1;
        }
        out->tbl = input->u.table;
        out->col = col;
        out->n   = input->u.table->count;
        return 0;
    }
    (void)env;

    if (input->type == VAL_LIST) { out->items = input->u.list.items; out->n = input->u.list.count; return 0; }
    if (input->type == VAL_NULL) { out->n = 0; return 0; }

    *err = err_args(cmd, "expected a list, or a table plus a column name");
    return -1;
}

static const struct value *agg_at(const struct agg_src *s, size_t i) {
    if (s->tbl) return record_field(&s->tbl->rows[i], s->col);
    return &s->items[i];
}

/* sum / avg / min / max [column]
 *
 * A NULL is SKIPPED, not treated as zero. A missing cell is not a measurement
 * of zero, and averaging it as one silently drags the answer down -- the same
 * SQL semantics `sort-by` already follows for null keys. */
enum agg_kind { AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX };

static struct value agg_run(const struct command *cmd, struct value input,
                            struct scope *env, enum agg_kind kind) {
    struct agg_src src;
    struct value err = value_null();
    if (agg_source(cmd, &input, env, &src, &err) != 0) { value_free(&input); return err; }

    bool    any = false, is_float = false, filesize = false;
    double  facc = 0;
    int64_t iacc = 0;
    size_t  counted = 0;

    for (size_t k = 0; k < src.n; k++) {
        const struct value *v = agg_at(&src, k);
        if (!v || v->type == VAL_NULL) continue;

        double f; int64_t i; bool vf;
        if (!num_of(v, &f, &i, &vf)) {
            value_free(&input);
            return err_args(cmd, "every value must be a number");
        }
        if (v->type == VAL_FILESIZE) filesize = true;
        if (vf) is_float = true;

        if (!any) { facc = f; iacc = i; any = true; }
        else switch (kind) {
        case AGG_SUM: case AGG_AVG: facc += f; iacc += i; break;
        case AGG_MIN: if (f < facc) { facc = f; iacc = i; } break;
        case AGG_MAX: if (f > facc) { facc = f; iacc = i; } break;
        }
        counted++;
    }

    value_free(&input);

    /* NOTHING to aggregate is NULL, not zero. `sum` of an empty list being 0
     * is defensible; `min` of one being 0 is a lie, and having the four agree
     * is worth more than the one convenient case. */
    if (!any) return value_null();

    if (kind == AGG_AVG) {
        double mean = facc / (double)counted;
        return filesize ? value_filesize((int64_t)mean) : value_float(mean);
    }
    if (is_float)  return value_float(facc);
    if (filesize)  return value_filesize(iacc);
    return value_int(iacc);
}

static struct value bi_sum(const struct command *c, struct value in, struct scope *e) { return agg_run(c, in, e, AGG_SUM); }
static struct value bi_avg(const struct command *c, struct value in, struct scope *e) { return agg_run(c, in, e, AGG_AVG); }
static struct value bi_min(const struct command *c, struct value in, struct scope *e) { return agg_run(c, in, e, AGG_MIN); }
static struct value bi_max(const struct command *c, struct value in, struct scope *e) { return agg_run(c, in, e, AGG_MAX); }

/* -------------------------------------------------------------------------
 * range N | range A B -- the integers [0,N) or [A,B).
 *
 * Half-open on purpose: `range $n` produces exactly n items, and
 * `range 0 $n | count` is n. A closed range makes both statements need a
 * "+1" that is forgotten exactly often enough to matter.
 * ------------------------------------------------------------------------- */
static struct value bi_range(const struct command *cmd, struct value input,
                             struct scope *env) {
    value_free(&input);
    if (cmd->nargs != 1 && cmd->nargs != 2)
        return err_args(cmd, "takes one or two integers");

    int64_t bound[2] = { 0, 0 };
    for (size_t i = 0; i < cmd->nargs; i++) {
        struct value v = expr_eval(cmd->args[i], env);
        if (v.type == VAL_ERROR) return v;
        if (v.type != VAL_INT) { value_free(&v); return err_args(cmd, "expected an integer"); }
        bound[i] = v.u.i;
        value_free(&v);
    }
    int64_t lo = cmd->nargs == 1 ? 0 : bound[0];
    int64_t hi = cmd->nargs == 1 ? bound[0] : bound[1];

    /* A descending range is EMPTY, not an error and not reversed. `range $a $b`
     * where a happens to exceed b is a loop that should not run, which is what
     * every caller means; guessing that they wanted it backwards would run the
     * body with values they never asked for. */
    struct value out = value_list();
    for (int64_t i = lo; i < hi; i++)
        value_list_push(&out, value_int(i));
    return out;
}

/* -------------------------------------------------------------------------
 * skip N -- drop the first N rows/items. first's complement, and what makes
 * "everything after the header" expressible.
 * ------------------------------------------------------------------------- */
static struct value bi_skip(const struct command *cmd, struct value input,
                            struct scope *env) {
    int64_t n = 1;
    if (cmd->nargs > 1) { value_free(&input); return err_args(cmd, "takes at most one count"); }
    if (cmd->nargs == 1) {
        struct value v = expr_eval(cmd->args[0], env);
        if (v.type == VAL_ERROR) { value_free(&input); return v; }
        if (v.type != VAL_INT || v.u.i < 0) { value_free(&v); value_free(&input); return err_args(cmd, "expected a count >= 0"); }
        n = v.u.i;
        value_free(&v);
    }

    if (input.type == VAL_TABLE) {
        struct value out = value_table();
        for (size_t i = (size_t)n; i < input.u.table->count; i++)
            value_table_push_row(&out, record_clone_value(&input.u.table->rows[i]));
        value_free(&input);
        return out;
    }
    if (input.type == VAL_LIST) {
        struct value out = value_list();
        for (size_t i = (size_t)n; i < input.u.list.count; i++)
            value_list_push(&out, value_clone(&input.u.list.items[i]));
        value_free(&input);
        return out;
    }
    value_free(&input);
    return err_args(cmd, "expected a table or a list");
}

/* -------------------------------------------------------------------------
 * uniq -- distinct items of a LIST, first occurrence wins, order preserved.
 *
 * A LIST only. Row equality over a table means comparing every field of every
 * row against every other, which is a different (and much slower) operation
 * wearing the same name; `get name | uniq` says which column you meant.
 * ------------------------------------------------------------------------- */
static struct value bi_uniq(const struct command *cmd, struct value input,
                            struct scope *env) {
    (void)env;
    if (input.type != VAL_LIST) {
        value_free(&input);
        return err_args(cmd, "expected a list (use `get <column> | uniq` for a table)");
    }
    struct value out = value_list();
    for (size_t i = 0; i < input.u.list.count; i++) {
        bool seen = false;
        for (size_t j = 0; j < out.u.list.count && !seen; j++)
            seen = value_equal(&input.u.list.items[i], &out.u.list.items[j]);
        if (!seen)
            value_list_push(&out, value_clone(&input.u.list.items[i]));
    }
    value_free(&input);
    return out;
}

/* -------------------------------------------------------------------------
 * TEXT. A shell that can loop but cannot take a string apart still has to
 * spawn a program for every trivial edit -- and there is no program for most
 * of these.
 * ------------------------------------------------------------------------- */
static bool text_of(const struct value *v, const char **b, size_t *n) {
    if (v->type != VAL_STRING && v->type != VAL_PATH) return false;
    *b = v->u.s.bytes; *n = v->u.s.len;
    return true;
}

/* A non-text scalar as the bytes `join` should splice in. Deliberately the
 * PLAIN form -- 1048576 rather than "1 MB" -- because join builds machine
 * input (a path, a CSV row, a command line) far more often than a display
 * line, and a pretty-printed filesize in the middle of one is a bug. Use
 * `echo` when a human is reading. */
static size_t scalar_text(const struct value *v, char *buf, size_t cap) {
    int n = 0;
    switch (v->type) {
    case VAL_INT:
    case VAL_FILESIZE:
    case VAL_DATE:  n = snprintf(buf, cap, "%lld", (long long)v->u.i); break;
    case VAL_FLOAT: n = snprintf(buf, cap, "%g", v->u.f); break;
    case VAL_BOOL:  n = snprintf(buf, cap, "%s", v->u.b ? "true" : "false"); break;
    case VAL_NULL:  n = 0; break;
    default:        n = snprintf(buf, cap, "<%s>", "value"); break;
    }
    if (n < 0) return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

/* split SEP -- a string into a list, on a literal separator (not a pattern).
 * An empty separator splits into characters. Adjacent separators produce EMPTY
 * fields rather than being collapsed: "a,,b" is three fields, and a shell that
 * silently dropped the middle one would corrupt every CSV it touched. */
static struct value bi_split(const struct command *cmd, struct value input,
                             struct scope *env) {
    const char *b; size_t n;
    if (!text_of(&input, &b, &n)) { value_free(&input); return err_args(cmd, "expected text"); }
    if (cmd->nargs != 1) { value_free(&input); return err_args(cmd, "takes one separator"); }

    struct value sv = expr_eval(cmd->args[0], env);
    if (sv.type == VAL_ERROR) { value_free(&input); return sv; }
    const char *sep; size_t seplen;
    if (!text_of(&sv, &sep, &seplen)) { value_free(&sv); value_free(&input); return err_args(cmd, "separator must be text"); }

    struct value out = value_list();
    if (seplen == 0) {
        for (size_t i = 0; i < n; i++) value_list_push(&out, value_string_n(b + i, 1));
    } else {
        size_t start = 0;
        for (size_t i = 0; i + seplen <= n; ) {
            if (memcmp(b + i, sep, seplen) == 0) {
                value_list_push(&out, value_string_n(b + start, i - start));
                i += seplen;
                start = i;
            } else i++;
        }
        value_list_push(&out, value_string_n(b + start, n - start));
    }
    value_free(&sv);
    value_free(&input);
    return out;
}

/* join SEP -- a list into one string. Non-text items are rendered the way
 * `echo` would render them, so `range 3 | join ","` is "0,1,2". */
static struct value bi_join(const struct command *cmd, struct value input,
                            struct scope *env) {
    if (input.type != VAL_LIST) { value_free(&input); return err_args(cmd, "expected a list"); }

    const char *sep = ""; size_t seplen = 0;
    struct value sv = value_null();
    if (cmd->nargs == 1) {
        sv = expr_eval(cmd->args[0], env);
        if (sv.type == VAL_ERROR) { value_free(&input); return sv; }
        if (!text_of(&sv, &sep, &seplen)) { value_free(&sv); value_free(&input); return err_args(cmd, "separator must be text"); }
    } else if (cmd->nargs > 1) {
        value_free(&input);
        return err_args(cmd, "takes at most one separator");
    }

    size_t cap = 64, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { value_free(&sv); value_free(&input); return value_error("out of memory"); }

    for (size_t i = 0; i < input.u.list.count; i++) {
        char scratch[64];
        const char *piece; size_t plen;
        if (!text_of(&input.u.list.items[i], &piece, &plen)) {
            plen = scalar_text(&input.u.list.items[i], scratch, sizeof scratch);
            piece = scratch;
        }
        size_t need = len + plen + (i ? seplen : 0) + 1;
        if (need > cap) {
            while (cap < need) cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); value_free(&sv); value_free(&input); return value_error("out of memory"); }
            buf = nb;
        }
        if (i && seplen) { memcpy(buf + len, sep, seplen); len += seplen; }
        memcpy(buf + len, piece, plen); len += plen;
    }

    struct value out = value_string_n(buf, len);
    free(buf);
    value_free(&sv);
    value_free(&input);
    return out;
}

/* lines -- text into a list of lines. A trailing newline does NOT produce a
 * final empty line: every text file ends in one, and a phantom last element in
 * every loop over a file is a bug in every script that never asked for it. */
static struct value bi_lines(const struct command *cmd, struct value input,
                             struct scope *env) {
    (void)env;
    const char *b; size_t n;
    if (!text_of(&input, &b, &n)) { value_free(&input); return err_args(cmd, "expected text"); }

    struct value out = value_list();
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != '\n') continue;
        size_t end = i;
        if (end > start && b[end - 1] == '\r') end--;      /* CRLF */
        value_list_push(&out, value_string_n(b + start, end - start));
        start = i + 1;
    }
    if (start < n) {
        size_t end = n;
        if (end > start && b[end - 1] == '\r') end--;
        value_list_push(&out, value_string_n(b + start, end - start));
    }
    value_free(&input);
    return out;
}

static struct value text_map(const struct command *cmd, struct value input,
                             int mode) {
    const char *b; size_t n;
    if (!text_of(&input, &b, &n)) { value_free(&input); return err_args(cmd, "expected text"); }

    size_t start = 0, end = n;
    if (mode == 2) {                                   /* trim */
        while (start < end && (b[start]==' '||b[start]=='\t'||b[start]=='\n'||b[start]=='\r')) start++;
        while (end > start && (b[end-1]==' '||b[end-1]=='\t'||b[end-1]=='\n'||b[end-1]=='\r')) end--;
    }

    struct value out = value_string_n(b + start, end - start);
    if (mode == 0 || mode == 1) {
        for (size_t i = 0; i < out.u.s.len; i++) {
            char c = out.u.s.bytes[i];
            if (mode == 0 && c >= 'a' && c <= 'z') out.u.s.bytes[i] = (char)(c - 32);
            if (mode == 1 && c >= 'A' && c <= 'Z') out.u.s.bytes[i] = (char)(c + 32);
        }
    }
    value_free(&input);
    return out;
}

static struct value bi_upper(const struct command *c, struct value in, struct scope *e) { (void)e; return text_map(c, in, 0); }
static struct value bi_lower(const struct command *c, struct value in, struct scope *e) { (void)e; return text_map(c, in, 1); }
static struct value bi_trim (const struct command *c, struct value in, struct scope *e) { (void)e; return text_map(c, in, 2); }

/* replace OLD NEW -- every occurrence, literal. */
static struct value bi_replace(const struct command *cmd, struct value input,
                               struct scope *env) {
    const char *b; size_t n;
    if (!text_of(&input, &b, &n)) { value_free(&input); return err_args(cmd, "expected text"); }
    if (cmd->nargs != 2) { value_free(&input); return err_args(cmd, "takes two arguments: old new"); }

    struct value ov = expr_eval(cmd->args[0], env);
    if (ov.type == VAL_ERROR) { value_free(&input); return ov; }
    struct value nv = expr_eval(cmd->args[1], env);
    if (nv.type == VAL_ERROR) { value_free(&ov); value_free(&input); return nv; }

    const char *o, *r; size_t ol, rl;
    if (!text_of(&ov, &o, &ol) || !text_of(&nv, &r, &rl) || ol == 0) {
        value_free(&ov); value_free(&nv); value_free(&input);
        return err_args(cmd, "old and new must be text, and old must not be empty");
    }

    size_t cap = n + 16, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { value_free(&ov); value_free(&nv); value_free(&input); return value_error("out of memory"); }

    for (size_t i = 0; i < n; ) {
        bool hit = (i + ol <= n) && memcmp(b + i, o, ol) == 0;
        size_t add = hit ? rl : 1;
        if (len + add + 1 > cap) {
            while (len + add + 1 > cap) cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); value_free(&ov); value_free(&nv); value_free(&input); return value_error("out of memory"); }
            buf = nb;
        }
        if (hit) { memcpy(buf + len, r, rl); len += rl; i += ol; }
        else     { buf[len++] = b[i++]; }
    }

    struct value out = value_string_n(buf, len);
    free(buf);
    value_free(&ov); value_free(&nv); value_free(&input);
    return out;
}

static struct value bi_history(const struct command *cmd, struct value input,
                               struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    struct value out = value_table();
    size_t n = hist_count();
    for (size_t i = 0; i < n; i++) {
        const char *line = hist_get(i);
        if (!line) continue;
        struct value r = value_record();
        value_record_set(&r, "n", value_int((int64_t)i + 1));   /* 1-based, oldest first */
        value_record_set(&r, "command", value_string(line));
        value_table_push_row(&out, r);
    }
    return out;
}

/* -------------------------------------------------------------------------
 * help -- the command reference, as a table (of course it's a table).
 * ------------------------------------------------------------------------- */
static struct value bi_help(const struct command *cmd, struct value input,
                            struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    static const struct { const char *name, *usage; } rows[] = {
        { "ls [path]",        "list a directory as a table {name,type,size}" },
        { "cat <file>",       "a file's contents, as a string" },
        { "cd [dir] / pwd",   "change / show the shell's working directory" },
        { "mkdir <dir>",      "create a directory" },
        { "rmdir <dir>",      "remove an EMPTY directory" },
        { "touch <file>",     "create an empty file" },
        { "rm <file>",        "remove a file" },
        { "cp <src> <dst>",   "copy a file" },
        { "mv <src> <dst>",   "move (copy + remove) a file" },
        { "save <path>",      "write the piped value to a file as text" },
        { "ps",               "processes, as a table {pid,ppid,state,pri,kind,cpu,sess}" },
        { "kill <pid>",       "terminate a process by pid (one of your session's)" },
        { "session",          "this shell's session, as the kernel records it {id,user,leader}" },
        { "env",              "the environment passed to spawned commands, as a table" },
        { "env set K V",      "set a variable (children get it; nothing is inherited)" },
        { "env unset K",      "remove a variable" },
        { "history",          "commands you ran, as a table {n,command}" },
        { "which <cmd>",      "how a command would resolve (builtin or path)" },
        { "clip",             "pipe text to the system clipboard (paste: Ctrl+V)" },
        { "whoami / hostname","who and where you are" },
        { "echo <expr>...",   "evaluate expressions (echo 1mb + 512kb)" },
        { "where <cond>",     "keep rows where the condition is true" },
        { "select <col>...",  "project columns" },
        { "sort-by <expr>",   "sort rows ascending by a key" },
        { "first/head [n]",   "the first n rows (default 1)" },
        { "last/tail [n]",    "the last n rows (default 1)" },
        { "reverse",          "rows in reverse order" },
        { "get <field>",      "a column as a list / a record's field" },
        { "count",            "how many rows/items" },
        { "wc",               "line/word/byte counts of text" },
        { "history",          "past commands as a table (Up/Down recalls them)" },
        { "uptime / date",    "system uptime / wall-clock date" },
        { "clear",            "clear the terminal" },
        { "let x = <expr>",   "bind a variable ($x)" },
        { "assert <cond> [msg]", "stop the program unless the condition is true" },
        { "source <file>",    "run a file's program here: its lets and defs stay" },
        { "prompt",           "the prompt's options, as a table (~/.config/shell/rc.esh sets them)" },
        { "prompt set K V...", "set an option (prompt set right status duration time)" },
        { "prompt get/unset K", "read / forget an option; prompt reset forgets them all" },
        { "prompt configure", "Powerlevel10k's wizard: answer questions, get a prompt" },
        { "cmd out> f / out>> f", "a command's output to a file (replace / append)" },
        { "cmd err> f / out+err> f", "its errors, or both, to a file" },
        { "cmd in< f",        "a file as the command's input" },
        { "exit",             "leave the shell" },
    };
    struct value out = value_table();
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        struct value r = value_record();
        value_record_set(&r, "command", value_string(rows[i].name));
        value_record_set(&r, "what",    value_string(rows[i].usage));
        value_table_push_row(&out, r);
    }
    return out;
}

/* -------------------------------------------------------------------------
 * prompt -- the interactive prompt's options (shell/prompt/prompt.h).
 *
 *   prompt                    -> table {key, value}
 *   prompt set KEY VALUE...   -> set; several values are one, space-separated
 *   prompt get KEY            -> its value, or null
 *   prompt unset KEY          -> forget it
 *   prompt reset              -> forget every option
 *   prompt style [NAME]       -> Powerlevel10k's style NAME as the base (plain: none);
 *                                alone, the base's name
 *   prompt import-p10k FILE   -> a ~/.p10k.zsh as the base -> {parameters, ignored}
 *   prompt configure          -> Powerlevel10k's wizard: questions, then the
 *                                configuration written and loaded
 *
 * Keys and words are taken as written (`prompt set right status time`);
 * a quoted string or a number is its value (`prompt set char "❯"`).
 * ------------------------------------------------------------------------- */
static bool prompt_value(const struct command *cmd, size_t from, struct scope *env, char *out, size_t cap,
                         struct value *error) {
    out[0] = 0;
    size_t n = 0, after_number = 0;   /* where the last argument, a number, ended: 12h lexes as 12, h */
    for (size_t i = from; i < cmd->nargs; i++) {
        char num[32];
        const struct expr *e = cmd->args[i];
        bool glued = i > from && after_number && e->line == cmd->args[i - 1]->line && e->col == after_number;
        after_number = 0;
        if (e->kind == EXPR_LITERAL && e->u.literal.type == VAL_INT)
            after_number = e->col + (size_t)snprintf(num, sizeof num, "%lld", (long long)e->u.literal.u.i);
        const char *word = expr_as_word(cmd->args[i]);
        size_t k = word ? strlen(word) : 0;
        struct value v = value_null();
        if (!word) {
            v = expr_eval(cmd->args[i], env);
            if (v.type == VAL_ERROR) { *error = v; return false; }
            if (v.type == VAL_STRING) { word = v.u.s.bytes; k = v.u.s.len; }
            else if (v.type == VAL_INT) { snprintf(num, sizeof num, "%lld", (long long)v.u.i); word = num; k = strlen(num); }
            else { value_free(&v); *error = value_error("prompt set: a value is a word, a string or a number"); return false; }
        }
        if (n + (n ? 1 : 0) + k >= cap || memchr(word, 0, k)) {
            value_free(&v); *error = value_error("prompt set: the value is too long"); return false;
        }
        if (n && !glued) out[n++] = ' ';
        memcpy(out + n, word, k);
        n += k;
        out[n] = 0;
        value_free(&v);
    }
    return true;
}

/* A configuration file read and loaded as the prompt's Powerlevel10k base. */
static struct value prompt_load(const char *path, const char *source, const char *who) {
    struct value text = shell_read_file(path, who);
    if (text.type == VAL_ERROR) return text;
    if (text.type != VAL_STRING) { value_free(&text); return value_error("prompt: not a text file"); }
    struct p10k_import rep;
    bool ok = prompt_p10k_load(source, text.u.s.bytes, &rep);
    value_free(&text);
    if (!ok) {
        char msg[400];
        snprintf(msg, sizeof msg, "%s: %s: %s", who, path, rep.error);
        return value_error(msg);
    }
    struct value r = value_record();
    value_record_set(&r, "parameters", value_int(rep.set));
    struct value ignored = value_list();
    for (const char *p = rep.ignored; *p;) {
        size_t n = strcspn(p, " ");
        if (n) value_list_push(&ignored, value_string_n(p, n));
        p += n;
        while (*p == ' ') p++;
    }
    value_record_set(&r, "ignored", ignored);
    return r;
}

/* prompt style [NAME] / prompt import-p10k FILE: Powerlevel10k as the base.
 * A style is one of the configurations p10k configure starts from, installed
 * in /system/share/powerlevel10k (PROMPT_STYLES elsewhere); `plain` is the
 * prompt without one. */
static struct value prompt_p10k(const struct command *cmd, const char *sub, struct scope *env) {
    char arg[256] = "";
    if (cmd->nargs > 1) {
        const char *w = expr_as_word(cmd->args[1]);
        if (w) snprintf(arg, sizeof arg, "%s", w);
        else {
            struct value v = expr_eval(cmd->args[1], env);
            if (v.type == VAL_STRING && v.u.s.len < sizeof arg && !memchr(v.u.s.bytes, 0, v.u.s.len)) {
                memcpy(arg, v.u.s.bytes, v.u.s.len);
                arg[v.u.s.len] = 0;
            }
            value_free(&v);
            if (!arg[0]) return value_error("prompt: a name or a path is a word or a string");
        }
    }
    if (!strcmp(sub, "style")) {
        static const char *const styles[] = { "lean", "lean-8colors", "classic", "rainbow", "pure" };
        if (cmd->nargs == 1) return value_string(prompt_p10k_source() ? prompt_p10k_source() : "plain");
        if (cmd->nargs != 2) return value_error("prompt style: prompt style NAME (lean, lean-8colors, classic, rainbow, pure, plain)");
        if (!strcmp(arg, "plain")) { prompt_p10k_drop(); return value_null(); }
        for (size_t i = 0; i < sizeof styles / sizeof *styles; i++) {
            if (strcmp(arg, styles[i])) continue;
            const char *dir = getenv("PROMPT_STYLES");
            char path[320];
            snprintf(path, sizeof path, "%s/p10k-%s.zsh", dir && dir[0] ? dir : "/system/share/powerlevel10k", arg);
            struct value r = prompt_load(path, arg, "prompt style");
            if (r.type == VAL_ERROR) return r;
            value_free(&r);
            return value_null();
        }
        return value_error("prompt style: lean, lean-8colors, classic, rainbow, pure or plain");
    }
    /* import-p10k: a person's own ~/.p10k.zsh (~ is the home) */
    if (cmd->nargs != 2) return value_error("prompt import-p10k: prompt import-p10k FILE");
    char file[320], path[320];
    const char *home = getenv("HOME");
    if ((arg[0] == '~' && (!arg[1] || arg[1] == '/')) && home)
        snprintf(file, sizeof file, "%s%s", strcmp(home, "/") ? home : "", arg + 1);
    else snprintf(file, sizeof file, "%s", arg);
    shell_path_resolve(file, path, sizeof path);
    return prompt_load(path, path, "prompt import-p10k");
}

static struct value bi_prompt(const struct command *cmd, struct value input, struct scope *env) {
    value_free(&input);
    static const char usage[] = "prompt: usage: prompt | prompt set KEY VALUE... | prompt get KEY | prompt unset KEY | "
                                "prompt reset | prompt style [NAME] | prompt import-p10k FILE | prompt configure";
    if (cmd->nargs == 0) {
        struct value out = value_table();
        for (size_t i = 0; i < prompt_opt_count(); i++) {
            struct value r = value_record();
            value_record_set(&r, "key", value_string(prompt_opt_key(i)));
            value_record_set(&r, "value", value_string(prompt_opt_value(i)));
            value_table_push_row(&out, r);
        }
        return out;
    }
    const char *sub = expr_as_word(cmd->args[0]);
    const char *key = cmd->nargs > 1 ? expr_as_word(cmd->args[1]) : NULL;
    char quoted[PROMPT_KEY_MAX + 1];
    if (cmd->nargs > 1 && !key) {                    /* a key may be quoted too */
        struct value k = expr_eval(cmd->args[1], env);
        if (k.type == VAL_STRING && k.u.s.len < sizeof quoted && !memchr(k.u.s.bytes, 0, k.u.s.len)) {
            memcpy(quoted, k.u.s.bytes, k.u.s.len);
            quoted[k.u.s.len] = 0;
            key = quoted;
        }
        value_free(&k);
    }
    if (!sub) return value_error(usage);
    if (!strcmp(sub, "style") || !strcmp(sub, "import-p10k")) return prompt_p10k(cmd, sub, env);
    if (!strcmp(sub, "reset") && cmd->nargs == 1) { prompt_opt_reset(); return value_null(); }
    if (!strcmp(sub, "configure") && cmd->nargs == 1) {
        char err[200];
        return wizard_os_run(true, err, sizeof err) < 0 ? value_error(err) : value_null();
    }
    if (!key) return value_error(usage);
    if (!prompt_key_valid(key)) return value_error("prompt: a key is lower-case words joined by dots (dir.anchor.fg)");
    if (!strcmp(sub, "set")) {
        if (cmd->nargs < 3) return value_error("prompt set: needs KEY and VALUE");
        char value[PROMPT_VALUE_MAX];
        struct value error;
        if (!prompt_value(cmd, 2, env, value, sizeof value, &error)) return error;
        if (!prompt_opt_set(key, value)) return value_error("prompt set: too many options");
        return value_null();
    }
    if (!strcmp(sub, "get") && cmd->nargs == 2) {
        const char *v = prompt_opt(key);
        return v ? value_string(v) : value_null();
    }
    if (!strcmp(sub, "unset") && cmd->nargs == 2) { (void)prompt_opt_unset(key); return value_null(); }
    return value_error(usage);
}

/* -------------------------------------------------------------------------
 * Registry: the PURE half; builtin_lookup_os() (builtins_os.c / host stub)
 * carries everything that needs a syscall.
 * ------------------------------------------------------------------------- */
static const struct { const char *name; builtin_fn fn; } g_pure[] = {
    { "echo",    bi_echo    },
    { "assert",  bi_assert  },
    { "where",   bi_where   },
    { "select",  bi_select  },
    { "sort-by", bi_sort_by },
    { "first",   bi_first   },
    { "head",    bi_first   },   /* the standard name, same stage */
    { "last",    bi_last    },
    { "tail",    bi_last    },
    { "reverse", bi_reverse },
    { "get",     bi_get     },
    { "count",   bi_count   },
    { "wc",      bi_wc      },
    { "history", bi_history },
    { "help",    bi_help    },
    { "sum",     bi_sum     }, { "avg",     bi_avg     },
    { "min",     bi_min     }, { "max",     bi_max     },
    { "range",   bi_range   }, { "skip",    bi_skip    },
    { "uniq",    bi_uniq    },
    { "split",   bi_split   }, { "join",    bi_join    },
    { "lines",   bi_lines   }, { "trim",    bi_trim    },
    { "upper",   bi_upper   }, { "lower",   bi_lower   },
    { "replace", bi_replace },
    { "prompt",  bi_prompt  },
};

builtin_fn builtin_lookup(const char *name) {
    for (size_t i = 0; i < sizeof g_pure / sizeof g_pure[0]; i++)
        if (strcmp(g_pure[i].name, name) == 0) return g_pure[i].fn;
    return builtin_lookup_os(name);
}

const char *builtin_name(size_t i) {
    if (i < sizeof g_pure / sizeof g_pure[0]) return g_pure[i].name;
    return builtin_os_name(i - sizeof g_pure / sizeof g_pure[0]);
}
