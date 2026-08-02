/* pkg -- the EmbLinkOS package manager (docs/PACKAGING_AND_SDK.md, PK1).
 *
 * A package is an authority-declaring bundle: an EMBX executable + a manifest
 * (§3) that mirrors, in one human-readable place, the app's identity and the
 * authority it declared in the binary itself (its EMBX capability table + its
 * UP4 .ns namespace). `pkg` does not grant power an installer promises; it
 * MEDIATES a grant the kernel then enforces -- and cross-checks the manifest
 * against the real EMBX so the declaration cannot drift from the binary.
 *
 *   pkg verify  <staged-dir>   -- check a bundle (build_id, caps, abi); no adopt.
 *   pkg install <staged-dir>   -- verify, present the declared authority, adopt
 *                                 it into /data/apps/<name>/ + register it.
 *   pkg run     <name>         -- spawn an installed app under EXACTLY its
 *                                 declared caps (SET_CAPS) + namespace (NS_BIND).
 *   pkg list                   -- installed packages (name version build_id).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "manifest.h"
#include "embxinfo.h"
#include "embk.h"

#define PKG_ABI   1              /* EMBX_ABI_VERSION this OS provides */
#define APPS_ROOT "/data/apps"
#define PKG_ROOT  "/data/pkg"

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f); b[n] = 0; if (len) *len = (size_t)n; return b;
}
static int write_file(const char *path, const void *b, size_t n) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    size_t w = n ? fwrite(b, 1, n, f) : 0; fclose(f); return w == n ? 0 : -1;
}
static int copy_file(const char *src, const char *dst) {
    size_t n; char *b = read_file(src, &n); if (!b) return -1;
    int rc = write_file(dst, b, n); free(b); return rc;
}
static const char *basename_of(const char *p) {
    const char *s = strrchr(p, '/'); return s ? s + 1 : p;
}

/* Find the single *.pkg in a staged bundle directory. */
static int find_manifest(const char *dir, char *out, size_t cap) {
    DIR *d = opendir(dir); if (!d) return -1;
    struct dirent *e; int found = 0;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcmp(e->d_name + n - 4, ".pkg")) {
            snprintf(out, cap, "%s/%s", dir, e->d_name); found = 1; break;
        }
    }
    closedir(d);
    return found ? 0 : -1;
}

static int cap_in(const int *list, int n, int id) {
    for (int i = 0; i < n; i++) if (list[i] == id) return 1;
    return 0;
}

/* Cross-check the manifest against the EMBX. Prints each failed check. 0 = ok. */
static int cross_check(const struct pkg_manifest *m, const struct embx_info *ei) {
    int ok = 1;
    if (!ei->build_id_ok) { printf("  x build_id does not recompute -- binary is corrupt\n"); ok = 0; }
    else if (memcmp(m->build_id, ei->build_id, 32) != 0) {
        printf("  x build_id: the manifest describes a different binary\n"); ok = 0; }
    if (m->abi != ei->abi) { printf("  x abi: manifest says %u, binary says %u\n", m->abi, ei->abi); ok = 0; }
    if (ei->abi != PKG_ABI) { printf("  x abi %u is not this OS's ABI (%u)\n", ei->abi, (unsigned)PKG_ABI); ok = 0; }
    /* caps must match as a SET (order-independent). */
    if (m->ncaps != ei->ncaps) { printf("  x caps: manifest lists %d, binary declares %d\n", m->ncaps, ei->ncaps); ok = 0; }
    for (int i = 0; i < m->ncaps; i++)
        if (!cap_in(ei->caps, ei->ncaps, m->caps[i])) {
            printf("  x cap '%s' is in the manifest but NOT in the binary\n", pkg_cap_name(m->caps[i])); ok = 0; }
    for (int i = 0; i < ei->ncaps; i++)
        if (!cap_in(m->caps, m->ncaps, ei->caps[i])) {
            printf("  x cap '%s' is in the binary but NOT declared in the manifest\n",
                   pkg_cap_name(ei->caps[i]) ? pkg_cap_name(ei->caps[i]) : "?"); ok = 0; }
    return ok ? 0 : -1;
}

static void present_authority(const struct pkg_manifest *m) {
    printf("  %s wants:\n", m->name);
    printf("    capabilities: ");
    if (m->ncaps == 0) printf("(none)");
    for (int i = 0; i < m->ncaps; i++) printf("%s%s", i ? ", " : "", pkg_cap_name(m->caps[i]));
    printf("\n    namespace:%s\n", m->nns ? "" : " (inherits caller's view)");
    for (int i = 0; i < m->nns; i++)
        printf("      %s %s\n", m->ns[i].ro ? "ro" : "rw", m->ns[i].prefix);
    printf("    nothing else -- what it did not declare, it cannot get.\n");
}

/* Load + parse an EMBX bundle: the manifest and the EMBX it `provides`. */
static int load_bundle(const char *dir, struct pkg_manifest *m, struct embx_info *ei,
                       char *binpath, size_t bincap) {
    char mpath[1024];
    if (find_manifest(dir, mpath, sizeof mpath) != 0) { fprintf(stderr, "pkg: no .pkg manifest in %s\n", dir); return -1; }
    size_t mlen; char *mtext = read_file(mpath, &mlen);
    if (!mtext) { fprintf(stderr, "pkg: cannot read %s\n", mpath); return -1; }
    char err[256];
    int rc = pkg_manifest_parse(mtext, mlen, m, err, sizeof err);
    free(mtext);
    if (rc != 0) { fprintf(stderr, "pkg: %s: %s\n", mpath, err); return -1; }

    snprintf(binpath, bincap, "%s/%s", dir, basename_of(m->provides));
    if (embx_read_info(binpath, ei, err, sizeof err) != 0) { fprintf(stderr, "pkg: %s\n", err); return -1; }
    return 0;
}

static int cmd_verify(const char *dir) {
    struct pkg_manifest m; struct embx_info ei; char binpath[1024];
    if (load_bundle(dir, &m, &ei, binpath, sizeof binpath) != 0) return 1;
    printf("PKG verify %s %s (abi %u):\n", m.name, m.version, m.abi);
    int rc = cross_check(&m, &ei);
    printf("PKG verify: %s\n", rc == 0 ? "OK" : "REJECTED");
    return rc == 0 ? 0 : 1;
}

static int write_ns(const char *dir, const struct pkg_manifest *m) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.ns", dir, m->name);
    char buf[PKG_MAX_NS * 300]; size_t o = 0;
    o += (size_t)snprintf(buf + o, sizeof buf - o,
            "# generated by pkg from %s's manifest -- the authority home enforces\n", m->name);
    for (int i = 0; i < m->nns; i++)
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s %s\n", m->ns[i].ro ? "ro" : "rw", m->ns[i].prefix);
    return write_file(path, buf, o);
}

static int cmd_install(const char *dir) {
    struct pkg_manifest m; struct embx_info ei; char binpath[1024];
    if (load_bundle(dir, &m, &ei, binpath, sizeof binpath) != 0) return 1;

    printf("PKG install %s %s (abi %u)\n", m.name, m.version, m.abi);
    if (cross_check(&m, &ei) != 0) { printf("PKG install: REJECTED (declaration does not match the binary)\n"); return 1; }
    present_authority(&m);

    /* Adopt: place the bundle under /data/apps/<name>/ (direct copy -- no atomic
     * snapshot yet; there is no userspace EMBKFS snapshot API, that is PK3). */
    char appdir[512], dst[1024], mdst[1024];
    mkdir(APPS_ROOT, 0755);                            /* exists; EEXIST is fine */
    snprintf(appdir, sizeof appdir, "%s/%s", APPS_ROOT, m.name);
    if (mkdir(appdir, 0755) != 0) { /* may already exist on reinstall */ }

    snprintf(dst, sizeof dst, "%s/%s", appdir, basename_of(m.provides));
    if (copy_file(binpath, dst) != 0) { fprintf(stderr, "pkg: failed to write %s\n", dst); return 1; }
    if (write_ns(appdir, &m) != 0) { fprintf(stderr, "pkg: failed to write the .ns\n"); return 1; }

    /* Keep the manifest as the install record. */
    char msrc[1024]; find_manifest(dir, msrc, sizeof msrc);
    snprintf(mdst, sizeof mdst, "%s/%s.pkg", appdir, m.name);
    copy_file(msrc, mdst);

    /* Register (name  version  build_id). */
    mkdir(PKG_ROOT, 0755);
    char reg[600]; snprintf(reg, sizeof reg, "%s/registry", PKG_ROOT);
    FILE *rf = fopen(reg, "ab");
    if (rf) { char bid[65]; for (int i = 0; i < 32; i++) sprintf(bid + 2*i, "%02x", m.build_id[i]);
              fprintf(rf, "%s\t%s\t%s\n", m.name, m.version, bid); fclose(rf); }

    printf("PKG install %s -> %s -> OK\n", m.name, appdir);
    return 0;
}

/* Spawn an installed app under exactly its declared caps + namespace. */
static int cmd_run(const char *name, int argc, char **argv) {
    char appdir[512], mpath[1024], binpath[1024];
    snprintf(appdir, sizeof appdir, "%s/%s", APPS_ROOT, name);
    snprintf(mpath, sizeof mpath, "%s/%s.pkg", appdir, name);
    size_t mlen; char *mtext = read_file(mpath, &mlen);
    if (!mtext) { fprintf(stderr, "pkg: %s is not installed\n", name); return 1; }
    struct pkg_manifest m; char err[256];
    int rc = pkg_manifest_parse(mtext, mlen, &m, err, sizeof err); free(mtext);
    if (rc != 0) { fprintf(stderr, "pkg: %s\n", err); return 1; }
    snprintf(binpath, sizeof binpath, "%s/%s", appdir, basename_of(m.provides));

    /* Actions: attenuate to exactly the declared cap set, then narrow the
     * namespace to exactly the declared binds. Both kernel-enforced. */
    struct embk_spawn_file_action acts[1 + PKG_MAX_NS]; int na = 0;
    unsigned mask = 0;
    for (int i = 0; i < m.ncaps; i++) mask |= EMBK_CAP_BIT(m.caps[i]);
    embk_action_set_caps(&acts[na++], mask);
    for (int i = 0; i < m.nns; i++)
        embk_action_ns_bind(&acts[na++], m.ns[i].prefix, m.ns[i].ro ? EMBK_NS_RO : EMBK_NS_RW);

    char *av[8]; int ac = 0; av[ac++] = binpath;
    for (int i = 0; i < argc && ac < 7; i++) av[ac++] = argv[i];
    av[ac] = NULL;

    printf("PKG run %s (caps 0x%x, %d ns bind%s)\n", name, mask, m.nns, m.nns == 1 ? "" : "s");
    int64_t h = embk_spawn(binpath, av, acts, na);
    if (h < 0) { fprintf(stderr, "pkg: spawn %s failed (%lld)\n", binpath, (long long)h); return 1; }
    int64_t code = embk_wait((int)h);
    printf("PKG run %s -> exit %lld\n", name, (long long)code);
    return code == 0 ? 0 : 1;
}

static int cmd_list(void) {
    char reg[600]; snprintf(reg, sizeof reg, "%s/registry", PKG_ROOT);
    size_t n; char *t = read_file(reg, &n);
    if (!t) { printf("PKG list: (none installed)\n"); return 0; }
    printf("PKG list:\n%s", t); free(t);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: pkg verify|install <dir> | run <name> | list\n"); return 2; }
    if (!strcmp(argv[1], "verify")  && argc >= 3) return cmd_verify(argv[2]);
    if (!strcmp(argv[1], "install") && argc >= 3) return cmd_install(argv[2]);
    if (!strcmp(argv[1], "run")     && argc >= 3) return cmd_run(argv[2], argc - 3, argv + 3);
    if (!strcmp(argv[1], "list"))                 return cmd_list();
    fprintf(stderr, "pkg: unknown command '%s'\n", argv[1]);
    return 2;
}
