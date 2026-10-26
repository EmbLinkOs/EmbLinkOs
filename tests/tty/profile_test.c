/* profile_test.c -- Terminal profiles (user/terminal/profile.c), host-run:
 * the presets, the parser's limits, and a writer that keeps what it does
 * not understand. */
#include "profile.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x, what) do { checks++; if (!(x)) { failures++; printf("  FAIL: %s (line %d)\n", what, __LINE__); } \
                            else printf("  ok:   %s\n", what); } while (0)
static bool same(const char *a, const char *b) {
    if (strcmp(a, b)) printf("    got \"%s\", want \"%s\"\n", a, b);
    return !strcmp(a, b);
}
static int lines_with_keys(const char *t) {
    int n = 0;
    for (const char *p = t; *p; ) {
        const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len && *p != '#' && memchr(p, '=', len)) n++;
        p += len + (e ? 1 : 0);
    }
    return n;
}
static bool profiles_equal(const struct term_profile *a, const struct term_profile *b) {
    return !strcmp(a->name, b->name) && !strcmp(a->font, b->font) && a->font_size == b->font_size &&
           a->line_spacing == b->line_spacing && a->letter_spacing == b->letter_spacing &&
           a->bold_is_bright == b->bold_is_bright && a->theme_colors == b->theme_colors &&
           !memcmp(a->color, b->color, sizeof a->color) && a->fg == b->fg && a->bg == b->bg &&
           a->bold == b->bold && a->selection == b->selection && a->cursor == b->cursor &&
           a->opacity == b->opacity && a->blur == b->blur && a->padding == b->padding &&
           a->cursor_shape == b->cursor_shape && a->cursor_blink == b->cursor_blink &&
           a->columns == b->columns && a->rows == b->rows && a->scrollback_mb == b->scrollback_mb &&
           a->bell == b->bell && !strcmp(a->command, b->command) && !strcmp(a->directory, b->directory) &&
           !strcmp(a->term, b->term) && a->clipboard_write == b->clipboard_write;
}

int main(void) {
    printf("=== Terminal profiles ===\n");
    struct term_profile p, q;
    char out[PROFILE_TEXT_MAX], v[64];

    printf("the default is Clear Dark, as the Mac has it:\n");
    profile_defaults(&p);
    CHECK(same(p.name, "Clear Dark") && same(p.font, "MesloLGS Nerd Font Mono") && p.font_size == 14, "name, font, 14 px");
    CHECK(p.bg == 0x212734 && p.fg == 0xe6e6e6 && p.bold == 0xf0f0f0 && p.selection == 0x334e5e, "colours in sRGB");
    CHECK(p.color[0] == 0x35424c && p.color[15] == 0xe5eff5, "the sixteen");
    CHECK(fabsf(p.opacity - 0.95f) < 1e-6f && fabsf(p.blur - 0.5f) < 1e-6f, "95% over a blur");
    CHECK(p.columns == 120 && p.rows == 30 && p.bold_is_bright && !p.clipboard_write, "120x30, bright bold, no OSC 52 writes");
    CHECK(same(p.command, "/system/bin/shell.elf") && same(p.directory, "same") && same(p.term, "xterm-256color") &&
          p.scrollback_mb == 8 && p.padding == 10, "the shell, new tabs where the shown one is, TERM, 8 MB, padding");
    CHECK(profile_decscusr(&p) == 2, "a steady block: DECSCUSR 2");

    printf("presets:\n");
    CHECK(profile_preset_count() == 5 && same(profile_preset_name(0), "Clear Dark"), "five, Clear Dark first");
    bool all = true;
    for (int i = 0; i < profile_preset_count(); i++) {
        profile_defaults(&q);
        int took = profile_parse(&q, profile_preset_text(i));
        if (took != lines_with_keys(profile_preset_text(i))) { all = false; printf("    %s: took %d of %d\n", profile_preset_name(i), took, lines_with_keys(profile_preset_text(i))); }
        if (profile_preset_find(profile_preset_name(i)) != i) all = false;
    }
    CHECK(all, "every preset's every key is taken, and each is found by name");
    CHECK(profile_preset(&q, "Solarized Light") && q.bg == 0xfdf6e3 && q.opacity == 1 && q.color[4] == 0x268bd2, "Solarized Light");
    CHECK(profile_preset(&q, "EmbLink") && q.theme_colors && q.blur == 0, "EmbLink follows the theme");
    CHECK(!profile_preset(&q, "Nope") && profile_preset_find("Nope") == -1, "an unknown name is no preset");

    printf("what the parser refuses:\n");
    profile_defaults(&p);
    int took = profile_parse(&p, "font_size = 200\nopacity = 0.1\nbackground = #12345\ncolor3 = red\n"
                                 "cursor_blink = maybe\ncursor_shape = round\ncolumns = 12x\ncolor16 = #ffffff\n"
                                 "color03 = #ffffff\nunknown = 3\nbell = loud\n  # comment = #000000\nname =\n");
    profile_defaults(&q);
    CHECK(took == 0 && profiles_equal(&p, &q), "out of range, malformed, unknown: all left as they were");
    took = profile_parse(&p, "  font_size=16.5  \n\ncolor3 = #ABCDEF\ncursor_shape = bar\ncursor_blink = yes\n"
                             "directory = /data/src  \nname = My Profile\nbell = none\nclipboard_write = on\n");
    CHECK(took == 8 && p.font_size == 16.5f && p.color[3] == 0xabcdef && profile_decscusr(&p) == 5 &&
          same(p.directory, "/data/src") && same(p.name, "My Profile") && p.bell == PROFILE_BELL_NONE && p.clipboard_write,
          "spaces trimmed, hex either case, a blinking bar, a path, a name with a space");

    printf("writing keeps what it does not understand:\n");
    const char *old = "# my settings\nname = Mine\nfont_size = 12\nfuture_key = something\n\nbackground = #000000\n"
                      "font_size = 13\n# end\n";
    profile_defaults(&p);
    profile_parse(&p, old);
    p.font_size = 15; p.bg = 0x102030;
    int n = profile_format(&p, old, out, sizeof out);
    CHECK(n > 0 && !strncmp(out, "# my settings\nname = Mine\nfont_size = 15\nfuture_key = something\n\nbackground = #102030\n# end\n", 89),
          "comments, blanks and unknown keys in place; values replaced where they stood");
    CHECK(!strstr(out, "font_size = 13"), "a key said twice is written once");
    CHECK(strstr(out, "\ncolor15 = #e5eff5\n") && strstr(out, "\nclipboard_write = no\n"), "missing keys added at the end");
    profile_defaults(&q);
    profile_parse(&q, out);
    CHECK(profiles_equal(&p, &q), "and it reads back the same");
    n = profile_format(&p, NULL, out, sizeof out);
    profile_defaults(&q); profile_parse(&q, out);
    CHECK(n > 0 && profiles_equal(&p, &q), "a new file reads back the same");
    CHECK(profile_format(&p, NULL, out, 40) == -1, "too small a buffer is refused");
    for (int i = 0; i < profile_preset_count(); i++) {
        profile_preset(&p, profile_preset_name(i));
        profile_format(&p, NULL, out, sizeof out);
        profile_defaults(&q); profile_parse(&q, out);
        if (!profiles_equal(&p, &q)) { all = false; printf("    %s does not round-trip\n", profile_preset_name(i)); }
    }
    CHECK(all, "every preset round-trips");

    printf("file names:\n");
    profile_file_name("Clear Dark", v, sizeof v);
    CHECK(same(v, "clear-dark.conf"), "Clear Dark");
    profile_file_name("  My  Profile!! 2", v, sizeof v);
    CHECK(same(v, "my-profile-2.conf"), "runs of anything else are one dash");
    profile_file_name("***", v, sizeof v);
    CHECK(same(v, "x.conf"), "nothing usable");

    printf("terminal.conf:\n");
    const char *conf = "# Terminal\ndefault_profile = Clear Dark\nother = 1\n";
    CHECK(profile_conf_get(conf, "default_profile", v, sizeof v) && same(v, "Clear Dark"), "read a key");
    CHECK(!profile_conf_get(conf, "missing", v, sizeof v), "an absent key");
    n = profile_conf_set(conf, "default_profile", "Basic", out, sizeof out);
    CHECK(n > 0 && same(out, "# Terminal\ndefault_profile = Basic\nother = 1\n"), "set in place");
    n = profile_conf_set(conf, "new_key", "x", out, sizeof out);
    CHECK(n > 0 && same(out, "# Terminal\ndefault_profile = Clear Dark\nother = 1\nnew_key = x\n"), "or added");
    n = profile_conf_set(NULL, "default_profile", "Basic", out, sizeof out);
    CHECK(n > 0 && same(out, "default_profile = Basic\n"), "from nothing");

    printf("=== profiles: %u checks, %u failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
