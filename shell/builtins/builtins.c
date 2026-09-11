/* ==========================================================================
 * builtins.c -- the PURE builtins (no OS dependency; host-testable):
 * echo, where, select, sort-by, first, count, plus the registry.
 * ls (OS-backed) lives in builtins_os.c.
 * ========================================================================== */
#include "builtins/builtins.h"
#include "hist/hist.h"
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
 * Registry: the PURE half; builtin_lookup_os() (builtins_os.c / host stub)
 * carries everything that needs a syscall.
 * ------------------------------------------------------------------------- */
builtin_fn builtin_lookup(const char *name) {
    static const struct { const char *name; builtin_fn fn; } tab[] = {
        { "echo",    bi_echo    },
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
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcmp(tab[i].name, name) == 0) return tab[i].fn;
    return builtin_lookup_os(name);
}
