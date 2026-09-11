/* js.c -- JavaScript on EmbLinkOS, on QuickJS.
 *
 * docs/BROWSER.md §9 decided this and the reasoning still holds: own the CORE
 * (kernel, filesystem, TLS, libc, compiler, UI), PORT the tools. A JS engine
 * to any useful standard is a multi-month project that would buy nothing a
 * port would not -- and this OS already ports CPython, TCC and git.
 *
 * What is ours is this file and the build. QuickJS ships a `qjs` CLI, and we
 * deliberately do not use it: it brings an OS layer of threads, dlopen, a
 * poll loop and a module loader, none of which a first port needs and each of
 * which is a syscall surface to argue with. The engine's C API is small and
 * clean -- a runtime, a context, an eval -- so the whole host is a hundred
 * lines that speak only newlib.
 *
 *   js -e '2+2'          evaluate an expression, print the result
 *   js FILE.js           run a file
 *   js                   read a program from stdin
 *
 * Exit: 0 ok, 1 usage, 2 cannot read the file, 3 the script threw.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

/* Print an exception the way a person can act on it: the message, and the
 * stack when the engine gives us one. A bare "SyntaxError" with no location
 * is a worse diagnostic than no diagnostic, because it looks like an answer. */
static void dump_error(JSContext *ctx) {
    JSValue e = JS_GetException(ctx);
    const char *msg = JS_ToCString(ctx, e);
    fprintf(stderr, "js: %s\n", msg ? msg : "(unprintable exception)");
    if (msg) JS_FreeCString(ctx, msg);

    if (JS_IsError(ctx, e)) {
        JSValue st = JS_GetPropertyStr(ctx, e, "stack");
        if (!JS_IsUndefined(st)) {
            const char *s = JS_ToCString(ctx, st);
            if (s && *s) fprintf(stderr, "%s", s);
            if (s) JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, st);
    }
    JS_FreeValue(ctx, e);
}

/* Evaluate, then print the value unless it is undefined -- the REPL rule, and
 * the one that makes `js -e '2+2'` say 4 instead of saying nothing. */
static int run(JSContext *ctx, const char *code, size_t len, const char *name,
               int print_result) {
    JSValue v = JS_Eval(ctx, code, len, name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { dump_error(ctx); JS_FreeValue(ctx, v); return 3; }
    if (print_result && !JS_IsUndefined(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) { printf("%s\n", s); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    return 0;
}

/* console.log, because a language without a way to say anything is a
 * calculator. Bound here rather than pulled from quickjs-libc for the same
 * reason the CLI is not used: this is the whole binding surface, visible. */
static JSValue js_print(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv) {
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) return JS_EXCEPTION;
        printf("%s%s", i ? " " : "", s);
        JS_FreeCString(ctx, s);
    }
    printf("\n");
    return JS_UNDEFINED;
}

static char *read_all(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return 0; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    *out_len = got;
    return buf;
}

int main(int argc, char **argv) {
    const char *expr = 0, *file = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-e") && i + 1 < argc) expr = argv[++i];
        else if (argv[i][0] != '-') file = argv[i];
        else { fprintf(stderr, "usage: js [-e EXPR | FILE.js]\n"); return 1; }
    }

    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { fprintf(stderr, "js: cannot create a runtime\n"); return 1; }
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "js: cannot create a context\n"); JS_FreeRuntime(rt); return 1; }

    JSValue g = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, js_print, "log", 1));
    JS_SetPropertyStr(ctx, g, "console", console);
    JS_SetPropertyStr(ctx, g, "print", JS_NewCFunction(ctx, js_print, "print", 1));
    JS_FreeValue(ctx, g);

    int rc;
    if (expr) {
        rc = run(ctx, expr, strlen(expr), "<expr>", 1);
    } else if (file) {
        size_t n = 0;
        char *src = read_all(file, &n);
        if (!src) { fprintf(stderr, "js: cannot read %s\n", file); rc = 2; }
        else { rc = run(ctx, src, n, file, 0); free(src); }
    } else {
        static char buf[64 * 1024];
        size_t n = fread(buf, 1, sizeof buf - 1, stdin);
        buf[n] = 0;
        rc = run(ctx, buf, n, "<stdin>", 0);
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return rc;
}
