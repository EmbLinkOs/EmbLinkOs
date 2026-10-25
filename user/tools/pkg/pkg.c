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
 *   pkg sync     [url]                 -- clone the registry git repo into
 *                                         /data/registry (over our git-over-HTTPS);
 *                                         the url is remembered.
 *   pkg install [--allow-widen|--local] <dir|name>
 *                                      -- a path installs that bundle; a bare NAME
 *                                         resolves to the synced registry. Verify,
 *                                         present the declared authority, adopt
 *                                         into /data/apps/<name>/. On an
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
#include <errno.h>
#include "manifest.h"
#include "embxinfo.h"
#include "embk.h"
#include "ecdsa.h"             /* ecdsa_verify, ec_p256 (user/lib/tls/crypto) */
#include "crypto/sha256.h"     /* one-shot sha256 (via -Ikernel) */
#include "pkgkey.h"            /* PKG_SIGN_QX / PKG_SIGN_QY (trusted key) */

#define PKG_ABI   1              /* EMBX_ABI_VERSION this OS provides */
#define APPS_ROOT "/data/apps"
#define PKG_ROOT  "/data/pkg"
#define REGISTRY_DIR "/data/registry"
#define GITCLONE  "/system/bin/gitclone.elf"
#define WGET      "/system/bin/wget.elf"

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
    size_t w = n ? fwrite(b, 1, n, f) : 0;
    int rc=w==n && !fflush(f) && !embk_fsync(fileno(f))?0:-1;
    if(fclose(f))rc=-1;return rc;
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
    uint8_t key[64];
    FILE *f=fopen("/system/update/key","rb");
    if(f) {int ok=fread(key,1,64,f)==64 && fgetc(f)==EOF;fclose(f);if(!ok)return 0;}
    else {memcpy(key,PKG_SIGN_QX,32);memcpy(key+32,PKG_SIGN_QY,32);}
    return ecdsa_verify(ec_p256(), key, key+32,
                        hash, 32, sig, 32, sig + 32, 32);
}

/* Cross-check the manifest against the EMBX + its signature. `local` allows an
 * UNSIGNED on-device dev build (installed with --local); a present signature is
 * still required to be valid, and everything else is checked as normal. 0 = ok. */
static int cross_check(const struct pkg_manifest *m, const struct embx_info *ei, int sig_ok, int local) {
    int ok = 1;
    if (!m->have_sig) {
        if (local) printf("  signature: none (local build, --local)\n");
        else { printf("  x signature: MISSING -- packages must be signed (or install with --local)\n"); ok = 0; }
    } else if (!sig_ok) { printf("  x signature: INVALID -- not signed by the trusted key (or the manifest was altered)\n"); ok = 0; }
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

/* If the bundle's EMBX isn't present but a `<name>.url` is (§9.1: the registry
 * ships only the signed manifest; the binary is a separate release asset),
 * fetch it with wget. Safe because the caller then checks the fetched binary's
 * build_id against the SIGNED manifest -- a tampered host cannot inject a bad
 * binary. Needs CAP_NETWORK (inherited by wget). 0 = present or fetched. */
static int fetch_binary(const char *dir, const struct pkg_manifest *m) {
    char dest[1024]; snprintf(dest, sizeof dest, "%s/%s", dir, basename_of(m->provides));
    struct embk_stat es;
    if (embk_stat(dest, &es) == 0) return 0;               /* already in the bundle */
    char urlpath[1024]; snprintf(urlpath, sizeof urlpath, "%s/%s.url", dir, m->name);
    size_t n; char *u = read_file(urlpath, &n);
    if (!u) return 0;                                      /* no url; report a missing binary later */
    size_t k = strlen(u); while (k && (u[k-1]=='\n'||u[k-1]=='\r'||u[k-1]==' ')) u[--k] = 0;
    printf("  fetching binary (release asset) from %s\n", u);
    char *av[] = { (char *)WGET, (char *)"-O", dest, u, NULL };
    int64_t h = embk_spawn(WGET, av, NULL, 0);
    int rc = (h >= 0) ? (int)embk_wait((int)h) : -1;
    free(u);
    if (rc != 0) { fprintf(stderr, "pkg: failed to fetch the binary (%d)\n", rc); return -1; }
    return 0;
}

/* Load + parse an EMBX bundle: the manifest, the EMBX it `provides` (fetched via
 * its `.url` if not in the bundle), and whether the manifest's signature
 * verifies against the trusted key (*sig_ok). */
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

    if(m->have_sig && !*sig_ok){fprintf(stderr,"pkg: invalid manifest signature\n");return -1;}
    char expected[1024];snprintf(expected,sizeof expected,"%s/%s",dir,basename_of(m->provides));
    struct embk_stat present;
    if(embk_stat(expected,&present) && !*sig_ok){fprintf(stderr,"pkg: refusing unsigned remote asset fetch\n");return -1;}
    if (fetch_binary(dir, m) != 0) return -1;              /* release-asset fetch, if needed */
    snprintf(binpath, bincap, "%s/%s", dir, basename_of(m->provides));
    if (embx_read_info(binpath, ei, err, sizeof err) != 0) { fprintf(stderr, "pkg: %s\n", err); return -1; }
    return 0;
}

static int cmd_verify(const char *dir) {
    struct pkg_manifest m; struct embx_info ei; char binpath[1024]; int sig_ok;
    if (load_bundle(dir, &m, &ei, binpath, sizeof binpath, &sig_ok) != 0) return 1;
    printf("PKG verify %s %s (abi %u):\n", m.name, m.version, m.abi);
    int rc = cross_check(&m, &ei, sig_ok, 0);
    if (rc == 0 && m.have_sig) printf("  signature: valid (trusted build key)\n");
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

/* A published bundle is immutable. CURRENT changes only after every file and
 * directory has crossed a storage barrier. Existing handles keep their vnode. */
static int sync_dir(const char *path) {
    int fd=(int)embk_open(path,EMBK_O_RDONLY,0);if(fd<0)return -1;
    int rc=embk_fsync(fd);if(embk_close(fd))rc=-1;return rc;
}
static int valid_id(const char *p) {
    for(unsigned i=0;i<64;i++)if(!((p[i]>='0' && p[i]<='9') || (p[i]>='a' && p[i]<='f')))return 0;
    return 1;
}
static void package_dir(const char *name,char *out,size_t n) {snprintf(out,n,"%s/apps/%s",PKG_ROOT,name);}
static int current_ids(const char *name,char current[65],char previous[65]) {
    char path[700];package_dir(name,path,sizeof path);strcat(path,"/CURRENT");
    size_t n;char *p=read_file(path,&n);if(!p)return 0;
    if((n!=65 && n!=130) || p[64]!='\n' || !valid_id(p) || (n==130 && (p[129]!='\n' || !valid_id(p+65)))){free(p);return -1;}
    memcpy(current,p,64);current[64]=0;previous[0]=0;
    if(n==130){memcpy(previous,p+65,64);previous[64]=0;}free(p);return 1;
}
static int publish_pointer(const char *name,const char *current,const char *previous) {
    char dir[600],path[700],temp[760],text[131];package_dir(name,dir,sizeof dir);
    snprintf(path,sizeof path,"%s/CURRENT",dir);snprintf(temp,sizeof temp,"%s/.CURRENT-%u",dir,(unsigned)embk_getpid());
    int n=snprintf(text,sizeof text,"%s\n%s%s",current,previous,previous[0]?"\n":"");
    int fd=(int)embk_open(temp,EMBK_O_WRONLY|EMBK_O_CREAT|EMBK_O_EXCL,0600);if(fd<0)return -1;
    int rc=embk_write(fd,text,n)==n && !embk_fsync(fd)?0:-1;if(embk_close(fd))rc=-1;
    if(!rc)rc=(int)embk_syscall2(EMBK_SYS_rename,(int64_t)(uintptr_t)temp,(int64_t)(uintptr_t)path);
    if(!rc)rc=sync_dir(dir);if(rc)embk_unlink(temp);return rc;
}
static int bundle_lock(const char *name,char lock[700]) {
    mkdir(PKG_ROOT,0755);char store[600];snprintf(store,sizeof store,"%s/apps",PKG_ROOT);mkdir(store,0755);
    package_dir(name,store,sizeof store);mkdir(store,0755);
    snprintf(lock,700,"%s/.install-lock",store);
    int fd=(int)embk_open(lock,EMBK_O_WRONLY|EMBK_O_CREAT|EMBK_O_EXCL,0600);
    if(fd<0){fprintf(stderr,"pkg: %s is locked; if a previous installer was interrupted, remove %s before retrying\n",name,lock);return -1;}
    embk_close(fd);return 0;
}
static int stage_bundle(const char *name,const char *provides,const char *binary,
                        const char *manifest,const struct pkg_manifest *m,int local,char id[65]) {
    char dir[600],versions[700],path[900],dst[1100];package_dir(name,dir,sizeof dir);
    snprintf(versions,sizeof versions,"%s/versions",dir);mkdir(versions,0755);
    static unsigned sequence;uint64_t nonce[3]={(uint64_t)embk_getpid(),embk_uptime_ms(),++sequence};
    struct sha256_ctx hash;uint8_t digest[32];sha256_init(&hash);sha256_update(&hash,m->build_id,32);sha256_update(&hash,nonce,sizeof nonce);sha256_final(&hash,digest);
    for(unsigned i=0;i<32;i++)sprintf(id+2*i,"%02x",digest[i]);
    snprintf(path,sizeof path,"%s/%s",versions,id);if(mkdir(path,0755))return -1;
    snprintf(dst,sizeof dst,"%s/%s",path,basename_of(provides));if(copy_file(binary,dst))return -1;
    snprintf(dst,sizeof dst,"%s/%s.pkg",path,name);if(copy_file(manifest,dst) || write_ns(path,m))return -1;
    snprintf(dst,sizeof dst,"%s/LOCAL",path);if(write_file(dst,local?"1\n":"0\n",2))return -1;
    /* Verify the copied bytes, not just the source we previously checked. */
    struct pkg_manifest verified;struct embx_info ei;char bin[1100];int signed_ok;
    if(load_bundle(path,&verified,&ei,bin,sizeof bin,&signed_ok) || cross_check(&verified,&ei,signed_ok,local))return -1;
    return sync_dir(path) || sync_dir(versions) || sync_dir(dir)?-1:0;
}
static int adopt(const char *name,const char *provides,const char *binary,const char *manifest,
                 const struct pkg_manifest *m,int local,const struct pkg_manifest *old) {
    char lock[700];if(bundle_lock(name,lock))return -1;
    char current[65]={0},previous[65]={0},next[65];int rc=current_ids(name,current,previous);
    if(rc<0)goto done;
    /* Retain a legacy installation as one complete version before publishing. */
    if(!rc && old) {
        char dir[700],bin[1000],mpath[1000];snprintf(dir,sizeof dir,"%s/%s",APPS_ROOT,name);
        snprintf(bin,sizeof bin,"%s/%s",dir,basename_of(old->provides));snprintf(mpath,sizeof mpath,"%s/%s.pkg",dir,name);
        if(stage_bundle(name,old->provides,bin,mpath,old,!old->have_sig,current)){rc=-1;goto done;}
    }
    rc=stage_bundle(name,provides,binary,manifest,m,local,next);
    if(!rc)rc=publish_pointer(name,next,current);
done:embk_unlink(lock);if(rc)fprintf(stderr,"pkg: bundle publication failed; current pointer was not intentionally removed\n");return rc;
}

static int cmd_install(const char *arg, int allow_widen, int local) {
    /* A path installs from that bundle dir; a bare name resolves to the synced
     * registry (/data/registry/<name>) -- `pkg sync` then `pkg install <name>`. */
    char dirbuf[600]; const char *dir = arg;
    if (arg[0] != '/') {
        snprintf(dirbuf, sizeof dirbuf, "%s/%s", REGISTRY_DIR, arg);
        struct embk_stat es;
        if (embk_stat(dirbuf, &es) != 0) {
            fprintf(stderr, "pkg: '%s' is not in the registry -- run `pkg sync` first?\n", arg); return 1; }
        dir = dirbuf;
        printf("PKG install: '%s' from the registry (%s)\n", arg, dir);
    }
    struct pkg_manifest m; struct embx_info ei; char binpath[1024]; int sig_ok;
    if (load_bundle(dir, &m, &ei, binpath, sizeof binpath, &sig_ok) != 0) return 1;

    printf("PKG install %s %s (abi %u)%s\n", m.name, m.version, m.abi, local ? " [local]" : "");
    if (cross_check(&m, &ei, sig_ok, local) != 0) { printf("PKG install: REJECTED (unsigned / altered / does not match the binary)\n"); return 1; }
    if (m.have_sig) printf("  signature: valid (trusted build key)\n");

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

    char msrc[1024]; find_manifest(dir, msrc, sizeof msrc);
    if (adopt(m.name, m.provides, binpath, msrc, &m, local, updating?&old:NULL) != 0) return 1;

    printf("PKG %s %s -> %s/%s -> OK\n", updating ? "update" : "install", m.name, APPS_ROOT, m.name);
    return 0;
}

static int cmd_rollback(const char *name) {
    char lock[700];if(bundle_lock(name,lock))return 1;
    char current[65]={0},previous[65]={0},dir[800],policy[850];int rc=1;
    if(current_ids(name,current,previous)!=1 || !previous[0]){fprintf(stderr,"pkg: no retained version for %s\n",name);goto done;}
    package_dir(name,dir,sizeof dir);strcat(dir,"/versions/");strcat(dir,previous);
    snprintf(policy,sizeof policy,"%s/LOCAL",dir);size_t n;char *p=read_file(policy,&n);
    int local=p && n==2 && !memcmp(p,"1\n",2);free(p);
    struct pkg_manifest m;struct embx_info ei;char binary[1100];int signed_ok;
    if(load_bundle(dir,&m,&ei,binary,sizeof binary,&signed_ok) || strcmp(m.name,name) || cross_check(&m,&ei,signed_ok,local))goto done;
    rc=publish_pointer(name,previous,current)?1:0;
    if(!rc)printf("PKG rollback %s -> %s -> OK\n",name,m.version);
done:embk_unlink(lock);return rc;
}
static int cmd_remove(const char *name) {
    char lock[700];if(bundle_lock(name,lock))return 1;
    char dir[700],pointer[800];package_dir(name,dir,sizeof dir);snprintf(pointer,sizeof pointer,"%s/CURRENT",dir);
    struct embk_stat st;int modern=embk_stat(pointer,&st)==0;
    int rc=modern?embk_unlink(pointer):0;
    if(!rc && modern)rc=sync_dir(dir);
    /* Remove a legacy writable installation too. A lower OS app remains
     * protected by its read-only volume and is revealed by deactivation. */
    struct pkg_manifest old;
    if(!rc && !load_installed(name,&old)) {
        char appdir[700],path[1000];snprintf(appdir,sizeof appdir,"%s/%s",APPS_ROOT,name);
        snprintf(path,sizeof path,"%s/%s.pkg",appdir,name);
        int remove_rc=embk_unlink(path);
        if(!remove_rc) {
            snprintf(path,sizeof path,"%s/%s",appdir,basename_of(old.provides));if(embk_unlink(path))rc=-1;
            snprintf(path,sizeof path,"%s/%s.ns",appdir,name);if(embk_unlink(path))rc=-1;
            if(embk_rmdir(appdir))rc=-1;
        } else if(remove_rc!=-EROFS || !modern)rc=remove_rc;
    } else if(!rc && !modern)rc=-1;
    embk_unlink(lock);
    if(!rc)printf("PKG remove %s -> deactivated; retained versions kept\n",name);
    return rc?1:0;
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
    char current[65]={0},previous[65]={0};
    if(current_ids(name,current,previous)==1 && previous[0])printf("  rollback available (retained version)\n");
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
    DIR *d=opendir(APPS_ROOT);if(!d)return 1;struct dirent *entry;unsigned count=0;
    printf("PKG list:\n");
    while((entry=readdir(d))) {
        struct pkg_manifest m;if(load_installed(entry->d_name,&m))continue;
        printf("%s\t%s\n",m.name,m.version);count++;
    }
    closedir(d);if(!count)printf("(none installed)\n");return 0;
}

/* Sync the package index by cloning the registry git repo (over our own
 * git-over-HTTPS) into /data/registry. The url is remembered so a later
 * `pkg sync` needs no argument. Requires CAP_NETWORK (inherited by gitclone). */
static int cmd_sync(const char *url_arg) {
    char urlbuf[600]; const char *url = url_arg;
    char urlpath[700]; snprintf(urlpath, sizeof urlpath, "%s/registry.url", PKG_ROOT);
    if (!url) {
        size_t n; char *u = read_file(urlpath, &n);
        if (u) { size_t k = strlen(u); while (k && (u[k-1]=='\n'||u[k-1]=='\r'||u[k-1]==' ')) u[--k]=0;
                 strncpy(urlbuf, u, sizeof urlbuf - 1); urlbuf[sizeof urlbuf-1]=0; free(u); url = urlbuf; }
    }
    if (!url || !url[0]) { fprintf(stderr, "pkg sync: no registry url (pass one; it is then remembered)\n"); return 1; }

    printf("PKG sync %s -> %s\n", url, REGISTRY_DIR);
    char *av[] = { (char *)GITCLONE, (char *)url, (char *)REGISTRY_DIR, NULL };
    int64_t h = embk_spawn(GITCLONE, av, NULL, 0);   /* inherits pkg's caps + ns */
    if (h < 0) { fprintf(stderr, "pkg sync: cannot run %s (%lld)\n", GITCLONE, (long long)h); return 1; }
    if (embk_wait((int)h) != 0) { fprintf(stderr, "pkg sync: registry clone failed\n"); return 1; }

    mkdir(PKG_ROOT, 0755);
    write_file(urlpath, url, strlen(url));
    printf("PKG sync -> OK (index at %s)\n", REGISTRY_DIR);
    return 0;
}

/* Batch updates keep the existing authority boundary. A package that asks
 * for broader access is left for explicit review, never silently approved. */
static int cmd_update_all(void) {
    DIR *d=opendir(APPS_ROOT);if(!d)return 1;struct dirent *entry;int failed=0;unsigned updated=0;
    while((entry=readdir(d))) {
        struct pkg_manifest old,newer;char path[700];
        if(load_installed(entry->d_name,&old))continue;
        snprintf(path,sizeof path,"%s/%s/%s.pkg",REGISTRY_DIR,old.name,old.name);
        if(load_manifest_file(path,&newer))continue;
        if(!strcmp(newer.version,old.version) && !memcmp(newer.build_id,old.build_id,32))continue;
        if(cmd_install(old.name,0,0))failed++;else updated++;
    }
    closedir(d);printf("PKG update-all: %u updated, %d require attention\n",updated,failed);return failed?1:0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: pkg sync [url] | pkg install [--allow-widen|--local] <dir|name>\n"
                        "       pkg verify <dir> | pkg run|rollback|remove|info <name> | pkg list\n");
        return 2;
    }
    if (!strcmp(argv[1], "verify")   && argc >= 3) return cmd_verify(argv[2]);
    if (!strcmp(argv[1], "install")  && argc >= 3) {
        int allow = 0, local = 0; const char *dir = NULL;
        for (int i = 2; i < argc; i++) {
            if      (!strcmp(argv[i], "--allow-widen")) allow = 1;
            else if (!strcmp(argv[i], "--local"))       local = 1;
            else dir = argv[i];
        }
        if (!dir) { fprintf(stderr, "pkg install: need a bundle dir\n"); return 2; }
        return cmd_install(dir, allow, local);
    }
    if (!strcmp(argv[1], "run")      && argc >= 3) return cmd_run(argv[2], argc - 3, argv + 3);
    if (!strcmp(argv[1], "rollback") && argc >= 3) return cmd_rollback(argv[2]);
    if (!strcmp(argv[1], "remove")   && argc >= 3) return cmd_remove(argv[2]);
    if (!strcmp(argv[1], "info")     && argc >= 3) return cmd_info(argv[2]);
    if (!strcmp(argv[1], "sync"))                  return cmd_sync(argc >= 3 ? argv[2] : NULL);
    if (!strcmp(argv[1], "update-all")) return cmd_update_all();
    if (!strcmp(argv[1], "list"))                  return cmd_list();
    fprintf(stderr, "pkg: unknown command '%s'\n", argv[1]);
    return 2;
}
