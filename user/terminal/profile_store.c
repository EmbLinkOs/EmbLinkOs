/* Terminal's profile files. See profile_store.h. */
#include "profile_store.h"
#include "embk.h"

#include <stdio.h>
#include <string.h>

static int read_file(const char *path, char *buf, size_t cap) {
    int fd = (int)embk_open(path, EMBK_O_RDONLY, 0);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n < cap - 1) {
        int64_t got = embk_read(fd, buf + n, cap - 1 - n);
        if (got <= 0) break;
        n += (size_t)got;
    }
    embk_close(fd);
    buf[n] = 0;
    return (int)n;
}

/* A temporary beside it, flushed, renamed over it, the directory flushed:
 * the same steps as oscfg_write_to. */
static bool write_file(const char *path, const char *text, size_t n) {
    static unsigned sequence;
    char temporary[280];
    snprintf(temporary, sizeof temporary, "%s.pending-%u-%u", path, (unsigned)embk_getpid(), ++sequence);
    int fd = (int)embk_open(temporary, EMBK_O_WRONLY | EMBK_O_CREAT | EMBK_O_EXCL, 0600);
    if (fd < 0) return false;
    bool ok = true;
    for (size_t done = 0; done < n; ) {
        int64_t wrote = embk_write(fd, text + done, n - done);
        if (wrote <= 0) { ok = false; break; }
        done += (size_t)wrote;
    }
    if (ok && embk_fsync(fd)) ok = false;
    if (embk_close(fd)) ok = false;
    if (ok && embk_syscall2(EMBK_SYS_rename, (int64_t)(uintptr_t)temporary, (int64_t)(uintptr_t)path)) ok = false;
    if (!ok) { embk_unlink(temporary); return false; }
    char parent[280];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    fd = (int)embk_open(parent, EMBK_O_RDONLY, 0);
    if (fd >= 0) { embk_fsync(fd); embk_close(fd); }
    return true;
}

/* Whether a directory holds `name`, from its listing: opening a file that
 * is not there costs a line in the kernel's log, and the watch asks every
 * second. */
static bool listed(const char *dir, const char *name) {
    static struct embk_dirent ents[64];
    int64_t n = embk_readdir(dir, ents, 64);
    for (int64_t i = 0; i < n; i++) if (!strcmp(ents[i].name, name)) return true;
    return false;
}

static void profile_path(const struct profile_store *st, const char *name, char *out, size_t cap) {
    char file[80];
    profile_file_name(name, file, sizeof file);
    snprintf(out, cap, "%.206s/%.79s", st->profiles, file);
}

bool profile_store_init(struct profile_store *st, const char *home) {
    char config[200];
    snprintf(config, sizeof config, "%.180s/.config", home && home[0] ? home : "");
    snprintf(st->dir, sizeof st->dir, "%.188s/terminal", config);
    snprintf(st->profiles, sizeof st->profiles, "%.197s/profiles", st->dir);
    snprintf(st->conf, sizeof st->conf, "%.197s/terminal.conf", st->dir);
    embk_mkdir(config); embk_mkdir(st->dir); embk_mkdir(st->profiles);   /* EEXIST is fine */
    struct embk_stat s;
    return embk_stat(st->profiles, &s) == 0;
}

void profile_store_default(const struct profile_store *st, char *out, size_t cap) {
    char text[1024];
    if (!listed(st->dir, "terminal.conf") || read_file(st->conf, text, sizeof text) < 0 ||
        !profile_conf_get(text, "default_profile", out, cap) || !out[0])
        snprintf(out, cap, "%s", profile_preset_name(0));
}

bool profile_store_set_default(const struct profile_store *st, const char *name) {
    char text[1024], out[1400];
    if (!listed(st->dir, "terminal.conf") || read_file(st->conf, text, sizeof text) < 0) text[0] = 0;
    int n = profile_conf_set(text, "default_profile", name, out, sizeof out);
    return n > 0 && write_file(st->conf, out, (size_t)n);
}

int profile_store_list(const struct profile_store *st, char names[][PROFILE_NAME_MAX], int max) {
    int n = 0;
    for (int i = 0; i < profile_preset_count() && n < max; i++)
        snprintf(names[n++], PROFILE_NAME_MAX, "%s", profile_preset_name(i));
    static struct embk_dirent ents[64];
    int64_t count = embk_readdir(st->profiles, ents, 64);
    int first_file = n;
    for (int64_t e = 0; e < count && n < max; e++) {
        size_t len = strlen(ents[e].name);
        if (len < 6 || strcmp(ents[e].name + len - 5, ".conf")) continue;
        char path[300], text[PROFILE_TEXT_MAX], name[PROFILE_NAME_MAX];
        snprintf(path, sizeof path, "%.206s/%.58s", st->profiles, ents[e].name);
        if (read_file(path, text, sizeof text) < 0 || !profile_conf_get(text, "name", name, sizeof name) || !name[0]) continue;
        bool seen = false;
        for (int i = 0; i < n; i++) if (!strcmp(names[i], name)) seen = true;
        if (seen) continue;                          /* a preset's own file, or a second file of one name */
        /* in order of name among the files */
        int at = n;
        while (at > first_file && strcmp(names[at - 1], name) > 0) {
            memcpy(names[at], names[at - 1], PROFILE_NAME_MAX); at--;
        }
        snprintf(names[at], PROFILE_NAME_MAX, "%s", name);
        n++;
    }
    return n;
}

bool profile_store_load(const struct profile_store *st, const char *name, struct term_profile *p) {
    char path[300], text[PROFILE_TEXT_MAX];
    bool preset = profile_preset(p, name);
    if (!preset) profile_defaults(p);
    profile_path(st, name, path, sizeof path);
    if (!profile_store_has_file(st, name) || read_file(path, text, sizeof text) < 0) return preset;
    profile_parse(p, text);
    snprintf(p->name, sizeof p->name, "%s", name);   /* the file is found by its name */
    return true;
}

bool profile_store_save(const struct profile_store *st, const struct term_profile *p) {
    char path[300], old[PROFILE_TEXT_MAX], out[PROFILE_TEXT_MAX];
    profile_path(st, p->name, path, sizeof path);
    int had = profile_store_has_file(st, p->name) ? read_file(path, old, sizeof old) : -1;
    int n = profile_format(p, had >= 0 ? old : NULL, out, sizeof out);
    return n > 0 && write_file(path, out, (size_t)n);
}

bool profile_store_remove(const struct profile_store *st, const char *name) {
    char path[300];
    profile_path(st, name, path, sizeof path);
    return embk_unlink(path) == 0;
}

bool profile_store_has_file(const struct profile_store *st, const char *name) {
    char file[80];
    profile_file_name(name, file, sizeof file);
    return listed(st->profiles, file);
}

static uint64_t fnv(uint64_t h, const char *s, int n) {
    for (int i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; }
    return h;
}
uint64_t profile_store_signature(const struct profile_store *st, const char *name) {
    char path[300], text[PROFILE_TEXT_MAX];
    uint64_t h = 1469598103934665603ull;
    int n = listed(st->dir, "terminal.conf") ? read_file(st->conf, text, sizeof text) : -1;
    h = fnv(h, n >= 0 ? text : "-", n >= 0 ? n : 1);
    n = -1;
    if (profile_store_has_file(st, name)) {
        profile_path(st, name, path, sizeof path);
        n = read_file(path, text, sizeof text);
    }
    return fnv(h, n >= 0 ? text : "-", n >= 0 ? n : 1);
}
