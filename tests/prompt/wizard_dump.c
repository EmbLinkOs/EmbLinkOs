/* wizard_dump -- `prompt configure` (shell/prompt/wizard.c) driven by
 * recorded keys, for tools/golden_wizard.py to hold against Powerlevel10k's
 * own wizard.
 *
 *   wizard_dump COLS ROWS [--auto] [--exists] [--8colors] [--config FILE] STEP...
 *
 * A STEP is a key (one character) or COLSxROWS, the window resized. What the
 * wizard writes goes to stdout as the terminal would get it, with
 * ESC ] 7777 ; page BEL before each step is taken -- the page the step is
 * taken on is everything before it. --auto: the wizard a shell with no
 * configuration starts (not `prompt configure`); --exists: a configuration is
 * there to overwrite; --8colors: TERM has 8 colours. With --config, the
 * configuration the answers make is written to FILE (from the templates in
 * third_party/powerlevel10k/config, dated "DATE"). Exits 0 when every
 * question was answered, 1 when the wizard was quit, 2 on a usage error. */
#include "prompt/wizard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_cols, g_rows, g_next, g_steps;
static char **g_step;

static void out(void *ctx, const char *bytes, size_t n) { (void)ctx; fwrite(bytes, 1, n, stdout); }

static int key(void *ctx, int ms) {
    (void)ctx; (void)ms;
    if (g_next == g_steps) return WIZARD_EOF;
    const char *s = g_step[g_next++];
    fputs("\033]7777;page\007", stdout);
    int c, r;
    if (strlen(s) > 1 && sscanf(s, "%dx%d", &c, &r) == 2) {
        g_cols = c;
        g_rows = r;
        return WIZARD_RESIZED;
    }
    return (unsigned char)s[0];
}

static void size(void *ctx, int *cols, int *rows) { (void)ctx; *cols = g_cols; *rows = g_rows; }

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *s = malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) == (size_t)n) s[n] = 0;
    else { free(s); s = NULL; }
    fclose(f);
    return s;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: wizard_dump COLS ROWS [--auto] [--exists] [--8colors] [--config FILE] STEP...\n"); return 2; }
    g_cols = atoi(argv[1]);
    g_rows = atoi(argv[2]);
    struct wizard_env env = { true, true, true, false, "~/.config/shell/p10k.zsh", "~/.config/shell/p10k.zsh.bak",
                              "~/.config/shell/rc.esh" };
    const char *config = NULL;
    int i = 3;
    for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
        if (!strcmp(argv[i], "--auto")) env.force = false;
        else if (!strcmp(argv[i], "--exists")) env.exists = true;
        else if (!strcmp(argv[i], "--8colors")) env.colors256 = false;
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) config = argv[++i];
        else { fprintf(stderr, "wizard_dump: %s?\n", argv[i]); return 2; }
    }
    g_step = argv + i;
    g_steps = argc - i;
    struct wizard_io io = { NULL, out, key, size };
    struct wizard_answers a;
    enum wizard_end end = wizard_run(&io, &env, &a);
    fflush(stdout);
    if (end != WIZARD_DONE) return 1;
    if (config) {
        char tmpl[32], path[256];
        wizard_template(&a, tmpl, sizeof tmpl);
        const char *dir = getenv("PROMPT_STYLES");
        snprintf(path, sizeof path, "%s/p10k-%s.zsh", dir && dir[0] ? dir : "third_party/powerlevel10k/config", tmpl);
        char *base = slurp(path);
        if (!base) { fprintf(stderr, "wizard_dump: no %s\n", path); return 2; }
        char *text = wizard_config(&a, base, "DATE");
        FILE *f = fopen(config, "wb");
        if (!text || !f) { fprintf(stderr, "wizard_dump: cannot write %s\n", config); return 2; }
        fputs(text, f);
        fclose(f);
        free(text);
        free(base);
    }
    return 0;
}
