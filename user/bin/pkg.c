/* pkg -- the EmbLinkOS package manager (docs/PACKAGING_AND_SDK.md, PK1).
 *
 * A package is an authority-declaring bundle: an EMBX executable + a manifest
 * (§3) that mirrors, in one human-readable place, the app's identity and the
 * authority it declared in the binary itself (its EMBX capability table + its
 * UP4 .ns namespace). `pkg` does not grant power an installer promises; it
 * MEDIATES a grant the kernel then enforces -- and cross-checks the manifest
 * against the real EMBX so the declaration cannot drift from the binary.
 *
 *   pkg verify   <staged-dir>          -- check a bundle (signature, build_id,
 *                                         caps, abi); no adopt.
 *   pkg install [--allow-widen] <dir>  -- verify, present the declared authority,
 *                                         adopt into /data/apps/<name>/. On an
 *                                         UPDATE, re-negotiate authority: a version
 *                                         that WIDENS caps/namespace is refused
 *                                         unless --allow-widen, and the previous
 *                                         version is retained as a rollback point.
 *   pkg run      <name>                -- spawn an installed app under EXACTLY its
 *                                         declared caps (SET_CAPS) + ns (NS_BIND).
 *   pkg rollback <name>                -- restore the retained previous version.
 *   pkg remove   <name>                -- delete the bundle + its rollback point.
 *   pkg info     <name>                -- show an installed package's authority.
 *   pkg list                           -- installed packages.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "manifest.h"
#include "embxinfo.h"
#include "embk.h"
#include "ecdsa.h"             /* ecdsa_verify, ec_p256 (user/lib/tls/crypto) */
#include "crypto/sha256.h"     /* one-shot sha256 (via -Ikernel) */
#include "pkgkey.h"            /* PKG_SIGN_QX / PKG_SIGN_QY (trusted key) */

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

/* Offset of the line starting with "signature:", or the length if none. The
 * signed message is the manifest up to (not including) that line -- matches
 * pkgsign's canonical form (the file with the signature line removed). */
static size_t canonical_len(const uint8_t *t, size_t n) {
    for (size_t i = 0; i < n; ) {
        if (n - i >= 10 && !memcmp(t + i, "signature:", 10)) return i;
        while (i < n && t[i] != '\n') i++;
        if (i < n) i++;
    }
    return n;
}

/* Verify the manifest's ECDSA-P256 signature against the trusted build key. */
static int verify_sig(const uint8_t *text, size_t len, const uint8_t sig[64]) {
    uint8_t hash[32];
    sha256(text, canonical_len(text, len), hash);
    return ecdsa_verify(ec_p256(), PKG_SIGN_QX, PKG_SIGN_QY,
                        hash, 32, sig, 32, sig + 32, 32);
}

/* Cross-check the manifest against the EMBX + its signature. 0 = ok. */
static int cross_check(const struct pkg_manifest *m, const struct embx_info *ei, int sig_ok) {
    int ok = 1;
    if (!m->have_sig) { printf("  x signature: MISSING -- packages must be signed\n"); ok = 0; }
    else if (!sig_ok) { printf("  x signature: INVALID -- not signed by the trusted key (or the manifest was altered)\n"); ok = 0; }
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

/* Load + parse an EMBX bundle: the manifest, the EMBX it `provides`, and whether
 * the manifest's signature verifies against the trusted key (*sig_ok). */
static int load_bundle(const char *dir, struct pkg_manifest *m, struct embx_info *ei,
                       char *binpath, size_t bincap, int *sig_ok) {
    char mpath[1024];
    if (find_manifest(dir, mpath, sizeof mpath) != 0) { fprintf(stderr, "pkg: no .pkg manifest in %s\n", dir); return -1; }
    size_t mlen; char *mtext = read_file(mpath, &mlen);
    if (!mtext) { fprintf(stderr, "pkg: cannot read %s\n", mpath); return -1; }
    char err[256];
    int rc = pkg_manifest_parse(mtext, mlen, m, err, sizeof err);
    *sig_ok = (rc == 0 && m->have_sig) ? verify_sig((const uint8_t *)mtext, mlen, m->signature) : 0;
    free(mtext);
    if (rc != 0) { fprintf(stderr, "pkg: %s: %s\n", mpath, err); return -1; }

    snprintf(binpath, bincap, "%s/%s", dir, basename_of(m->provides));
    if (embx_read_info(binpath, ei, err, sizeof err) != 0) { fprintf(stderr, "pkg: %s\n", err); return -1; }
    return 0;
}

static int cmd_verify(const char *dir) {
    struct pkg_manifest m; struct embx_info ei; char binpath[1024]; int sig_ok;
    if (load_bundle(dir, &m, &ei, binpath, sizeof binpath, &sig_ok) != 0) return 1;
    printf("PKG verify %s %s (abi %u):\n", m.name, m.version, m.abi);
    int rc = cross_check(&m, &ei, sig_ok);
    if (rc == 0) printf("  signature: valid (trusted build key)\n");
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

/* Parse a manifest file at `path` into m. 0 on success, -1 if absent/bad. */
static int load_manifest_file(const char *path, struct pkg_manifest *m) {
    size_t n; char *t = read_file(path, &n);
    if (!t) return -1;
    char err[256]; int rc = pkg_manifest_parse(t, n, m, err, sizeof err); free(t);
    return rc == 0 ? 0 : -1;
}

/* Read the currently-installed manifest for `name` into m. 0 if installed. */
static int load_installed(const char *name, struct pkg_manifest *m) {
    char mpath[1024]; snprintf(mpath, sizeof mpath, "%s/%s/%s.pkg", APPS_ROOT, name, name);
    return load_manifest_file(mpath, m);
}

/* Does `old` already grant a bind covering (prefix @ mode ro)? old rw covers any
 * mode at the same prefix; old ro covers only a new ro. */
static int ns_covered(const struct pkg_manifest *old, int ro, const char *prefix) {
    for (int i = 0; i < old->nns; i++)
        if (!strcmp(old->ns[i].prefix, prefix) && (old->ns[i].ro == 0 || ro == 1)) return 1;
    return 0;
}

/* Would adopting `nw` grant MORE authority than the installed `old`? Prints the
 * widening delta. A new cap, a new namespace prefix, or ro->rw on a prefix all
 * count -- so a new version cannot silently widen its reach (§6). */
static int authority_widens(const struct pkg_manifest *old, const struct pkg_manifest *nw) {
    int widen = 0;
    for (int i = 0; i < nw->ncaps; i++)
        if (!cap_in(old->caps, old->ncaps, nw->caps[i])) {
            printf("    + NEW capability: %s\n", pkg_cap_name(nw->caps[i])); widen = 1; }
    for (int i = 0; i < nw->nns; i++)
        if (!ns_covered(old, nw->ns[i].ro, nw->ns[i].prefix)) {
            printf("    + WIDER namespace: %s %s\n", nw->ns[i].ro ? "ro" : "rw", nw->ns[i].prefix); widen = 1; }
    return widen;
}

/* Place a verified bundle under /data/apps/<name>/: the EMBX, the .ns (from the
 * manifest home enforces), the manifest record, and a registry line. */
static int adopt(const char *name, const char *provides, const char *src_embx,
                 const char *src_manifest, const struct pkg_manifest *m) {
    char appdir[512], dst[1024], mdst[1024];
    mkdir(APPS_ROOT, 0755);
    snprintf(appdir, sizeof appdir, "%s/%s", APPS_ROOT, name);
    mkdir(appdir, 0755);
    snprintf(dst, sizeof dst, "%s/%s", appdir, basename_of(provides));
    if (copy_file(src_embx, dst) != 0) { fprintf(stderr, "pkg: failed to write %s\n", dst); return -1; }
    if (write_ns(appdir, m) != 0) { fprintf(stderr, "pkg: failed to write the .ns\n"); return -1; }
    snprintf(mdst, sizeof mdst, "%s/%s.pkg", appdir, name);
    copy_file(src_manifest, mdst);
    mkdir(PKG_ROOT, 0755);
    char reg[600]; snprintf(reg, sizeof reg, "%s/registry", PKG_ROOT);
    FILE *rf = fopen(reg, "ab");
    if (rf) { char bid[65]; for (int i = 0; i < 32; i++) sprintf(bid + 2*i, "%02x", m->build_id[i]);
              fprintf(rf, "%s\t%s\t%s\n", name, m->version, bid); fclose(rf); }
    return 0;
}

/* The retained-previous-version dir (the rollback point). */
static void prev_dir(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/versions/%s/prev", PKG_ROOT, name);
}

/* Retain the currently-installed bundle so an update is reversible. Each app is
 * a self-contained bundle (§6), so keeping its 3 files IS the rollback point --
 * no whole-FS snapshot needed (an EMBKFS-snapshot-backed variant is a future
 * optimization once a snapshot syscall exists). */
static int retain_current(const char *name, const struct pkg_manifest *old) {
    char appdir[512], prev[600], s[1200], d[1200];
    snprintf(appdir, sizeof appdir, "%s/%s", APPS_ROOT, name);
    mkdir(PKG_ROOT, 0755);
    char t[700]; snprintf(t, sizeof t, "%s/versions", PKG_ROOT); mkdir(t, 0755);
    snprintf(t, sizeof t, "%s/versions/%s", PKG_ROOT, name); mkdir(t, 0755);
    prev_dir(name, prev, sizeof prev); mkdir(prev, 0755);
    snprintf(s, sizeof s, "%s/%s", appdir, basename_of(old->provides));
    snprintf(d, sizeof d, "%s/%s", prev, basename_of(old->provides)); copy_file(s, d);
    snprintf(s, sizeof s, "%s/%s.pkg", appdir, name); snprintf(d, sizeof d, "%s/%s.pkg", prev, name); copy_file(s, d);
    snprintf(s, sizeof s, "%s/%s.ns", appdir, name);  snprintf(d, sizeof d, "%s/%s.ns", prev, name);  copy_file(s, d);
    return 0;
}

static int cmd_install(const char *dir, int allow_widen) {
    struct pkg_manifest m; struct embx_info ei; char binpath[1024]; int sig_ok;
    if (load_bundle(dir, &m, &ei, binpath, sizeof binpath, &sig_ok) != 0) return 1;

    printf("PKG install %s %s (abi %u)\n", m.name, m.version, m.abi);
    if (cross_check(&m, &ei, sig_ok) != 0) { printf("PKG install: REJECTED (unsigned / altered / does not match the binary)\n"); return 1; }
    printf("  signature: valid (trusted build key)\n");

    /* Update-aware: if already installed, re-negotiate authority before adopting.
     * A version that declares MORE than the installed one must be consented to. */
    struct pkg_manifest old; int updating = (load_installed(m.name, &old) == 0);
    if (updating) {
        printf("  updating: %s %s -> %s\n", m.name, old.version, m.version);
        if (authority_widens(&old, &m)) {
            if (!allow_widen) { printf("PKG install: REJECTED -- this update WIDENS authority beyond the installed "
                                       "version. Re-run with --allow-widen to consent.\n"); return 1; }
            printf("  (the wider authority above was consented to via --allow-widen)\n");
        }
    }
    present_authority(&m);

    if (updating) retain_current(m.name, &old);        /* the rollback point */
    char msrc[1024]; find_manifest(dir, msrc, sizeof msrc);
    if (adopt(m.name, m.provides, binpath, msrc, &m) != 0) return 1;

    printf("PKG %s %s -> %s/%s -> OK\n", updating ? "update" : "install", m.name, APPS_ROOT, m.name);
    return 0;
}

static int cmd_rollback(const char *name) {
    char prev[600], mpath[900], binprev[1200];
    prev_dir(name, prev, sizeof prev);
    snprintf(mpath, sizeof mpath, "%s/%s.pkg", prev, name);
    size_t n; char *t = read_file(mpath, &n);
    if (!t) { fprintf(stderr, "pkg: no rollback point for %s (nothing to roll back to)\n", name); return 1; }
    struct pkg_manifest pm; char err[256]; int rc = pkg_manifest_parse(t, n, &pm, err, sizeof err); free(t);
    if (rc != 0) { fprintf(stderr, "pkg: retained manifest bad: %s\n", err); return 1; }
    snprintf(binprev, sizeof binprev, "%s/%s", prev, basename_of(pm.provides));
    if (adopt(name, pm.provides, binprev, mpath, &pm) != 0) return 1;
    printf("PKG rollback %s -> %s -> OK\n", name, pm.version);
    return 0;
}

static int cmd_remove(const char *name) {
    struct pkg_manifest m;
    if (load_installed(name, &m) != 0) { fprintf(stderr, "pkg: %s is not installed\n", name); return 1; }
    char appdir[512], p[1200], prev[600];
    snprintf(appdir, sizeof appdir, "%s/%s", APPS_ROOT, name);
    snprintf(p, sizeof p, "%s/%s", appdir, basename_of(m.provides)); remove(p);
    snprintf(p, sizeof p, "%s/%s.ns", appdir, name);  remove(p);
    snprintf(p, sizeof p, "%s/%s.pkg", appdir, name); remove(p);
    remove(appdir);
    prev_dir(name, prev, sizeof prev);
    snprintf(p, sizeof p, "%s/%s", prev, basename_of(m.provides)); remove(p);
    snprintf(p, sizeof p, "%s/%s.ns", prev, name);  remove(p);
    snprintf(p, sizeof p, "%s/%s.pkg", prev, name); remove(p);
    remove(prev);
    printf("PKG remove %s -> OK\n", name);
    return 0;
}

static int cmd_info(const char *name) {
    struct pkg_manifest m;
    if (load_installed(name, &m) != 0) { fprintf(stderr, "pkg: %s is not installed\n", name); return 1; }
    printf("PKG info %s\n  version: %s  abi: %u\n", m.name, m.version, m.abi);
    printf("  capabilities:");
    for (int i = 0; i < m.ncaps; i++) printf(" %s", pkg_cap_name(m.caps[i]));
    if (!m.ncaps) printf(" (none)");
    printf("\n  namespace:\n");
    for (int i = 0; i < m.nns; i++) printf("    %s %s\n", m.ns[i].ro ? "ro" : "rw", m.ns[i].prefix);
    char prev[600], mpath[900]; prev_dir(name, prev, sizeof prev);
    snprintf(mpath, sizeof mpath, "%s/%s.pkg", prev, name);
    struct pkg_manifest pm;
    if (load_manifest_file(mpath, &pm) == 0) printf("  rollback available -> %s\n", pm.version);
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
    if (argc < 2) {
        fprintf(stderr, "usage: pkg verify|install [--allow-widen] <dir>\n"
                        "       pkg run|rollback|remove|info <name> | pkg list\n");
        return 2;
    }
    if (!strcmp(argv[1], "verify")   && argc >= 3) return cmd_verify(argv[2]);
    if (!strcmp(argv[1], "install")  && argc >= 3) {
        int allow = 0; const char *dir = NULL;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--allow-widen")) allow = 1;
            else dir = argv[i];
        }
        if (!dir) { fprintf(stderr, "pkg install: need a bundle dir\n"); return 2; }
        return cmd_install(dir, allow);
    }
    if (!strcmp(argv[1], "run")      && argc >= 3) return cmd_run(argv[2], argc - 3, argv + 3);
    if (!strcmp(argv[1], "rollback") && argc >= 3) return cmd_rollback(argv[2]);
    if (!strcmp(argv[1], "remove")   && argc >= 3) return cmd_remove(argv[2]);
    if (!strcmp(argv[1], "info")     && argc >= 3) return cmd_info(argv[2]);
    if (!strcmp(argv[1], "list"))                  return cmd_list();
    fprintf(stderr, "pkg: unknown command '%s'\n", argv[1]);
    return 2;
}
