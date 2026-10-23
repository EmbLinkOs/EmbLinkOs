/* pkgbuild -- generate a package bundle ON THE DEVICE (PK2b, the on-OS twin of
 * tools/embx/pkggen.py). Reads ONE .pkgspec (name/version/caps/grant) + a linked
 * ELF and emits the three views -- the EMBX (cap table baked in, via embxgen),
 * the .ns, and the package manifest -- consistent by construction. This is how
 * an app declares its authority as part of building on the OS itself.
 *
 * On-device builds are UNSIGNED (the signing key lives off-device, PK3/PK4); a
 * developer installs their own build with `pkg install --local`. A registry
 * package is signed and installed the normal (verified) way.
 *
 *   pkgbuild <spec.pkgspec> <input.elf> <out-dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "embxgen.h"
#include "manifest.h"

struct spec {
    char name[64], version[32];
    int  caps[PKG_MAX_CAPS]; int ncaps;
    struct { int ro; char prefix[256]; } grant[PKG_MAX_NS]; int ngrant;
};

static char *skip_ws(char *p) { while (*p == ' ' || *p == '\t') p++; return p; }
static void rstrip(char *s) { size_t n = strlen(s); while (n && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '||s[n-1]=='\t')) s[--n]=0; }

static int parse_spec(const char *path, struct spec *sp) {
    memset(sp, 0, sizeof *sp);
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "pkgbuild: cannot open %s\n", path); return -1; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        rstrip(line);
        char *p = skip_ws(line); if (!*p) continue;
        char *colon = strchr(p, ':'); if (!colon) { fclose(f); fprintf(stderr, "pkgbuild: bad spec line: %s\n", p); return -1; }
        *colon = 0; char *key = p, *val = skip_ws(colon + 1); rstrip(key);
        if (!strcmp(key, "name")) strncpy(sp->name, val, sizeof sp->name - 1);
        else if (!strcmp(key, "version")) strncpy(sp->version, val, sizeof sp->version - 1);
        else if (!strcmp(key, "caps")) {
            for (char *t = strtok(val, " ,\t"); t; t = strtok(NULL, " ,\t")) {
                int id = pkg_cap_id(t);
                if (!id) { fclose(f); fprintf(stderr, "pkgbuild: unknown capability '%s'\n", t); return -1; }
                if (sp->ncaps < PKG_MAX_CAPS) sp->caps[sp->ncaps++] = id;
            }
        } else if (!strcmp(key, "grant")) {
            for (char *g = strtok(val, ","); g; g = strtok(NULL, ",")) {
                g = skip_ws(g); rstrip(g);
                char mode[8], prefix[300];
                if (sscanf(g, "%7s %299s", mode, prefix) != 2) continue;
                if (sp->ngrant >= PKG_MAX_NS) break;
                sp->grant[sp->ngrant].ro = !strcmp(mode, "ro") ? 1 : 0;
                strncpy(sp->grant[sp->ngrant].prefix, prefix, sizeof sp->grant[sp->ngrant].prefix - 1);
                sp->ngrant++;
            }
        }
    }
    fclose(f);
    if (!sp->name[0] || !sp->version[0]) { fprintf(stderr, "pkgbuild: spec needs name and version\n"); return -1; }
    /* sort+dedup caps (EMBX §5.5) */
    for (int i = 1; i < sp->ncaps; i++) { int t = sp->caps[i], j = i-1;
        while (j >= 0 && sp->caps[j] > t) { sp->caps[j+1] = sp->caps[j]; j--; } sp->caps[j+1] = t; }
    return 0;
}

static int read_build_id_hex(const char *embx, char hex[65]) {
    FILE *f = fopen(embx, "rb"); if (!f) return -1;
    uint8_t b[32];
    if (fseek(f, 0x50, SEEK_SET) != 0 || fread(b, 1, 32, f) != 32) { fclose(f); return -1; }
    fclose(f);
    for (int i = 0; i < 32; i++) sprintf(hex + 2*i, "%02x", b[i]);
    hex[64] = 0;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: pkgbuild <spec.pkgspec> <input.elf> <out-dir>\n"); return 2; }
    struct spec sp;
    if (parse_spec(argv[1], &sp) != 0) return 1;
    const char *elf = argv[2], *outdir = argv[3];

    char embx[1024], nsf[1024], pkgf[1024];
    snprintf(embx, sizeof embx, "%s/%s.embx", outdir, sp.name);
    snprintf(nsf,  sizeof nsf,  "%s/%s.ns",   outdir, sp.name);
    snprintf(pkgf, sizeof pkgf, "%s/%s.pkg",  outdir, sp.name);

    /* 1) EMBX -- embxgen bakes the cap table from the spec's caps. */
    char err[256];
    if (embxgen_write(elf, sp.caps, sp.ncaps, embx, err, sizeof err) != 0) {
        fprintf(stderr, "pkgbuild: %s\n", err); return 1;
    }
    /* 2) .ns -- from the spec's grant. */
    FILE *nf = fopen(nsf, "wb");
    if (nf) { fprintf(nf, "# generated on-device by pkgbuild from %s's spec\n", sp.name);
              for (int i = 0; i < sp.ngrant; i++) fprintf(nf, "%s %s\n", sp.grant[i].ro ? "ro" : "rw", sp.grant[i].prefix);
              fclose(nf); }
    /* 3) manifest -- caps from the spec, build_id read back from the EMBX (so it
     *    matches the binary), abi = this OS. Unsigned (on-device build). */
    char bid[65];
    if (read_build_id_hex(embx, bid) != 0) { fprintf(stderr, "pkgbuild: cannot read build_id\n"); return 1; }
    FILE *pf = fopen(pkgf, "wb");
    if (!pf) { fprintf(stderr, "pkgbuild: cannot write %s\n", pkgf); return 1; }
    fprintf(pf, "name:     %s\nversion:  %s\nabi:      1\nbuild_id: %s\ncaps:    ", sp.name, sp.version, bid);
    for (int i = 0; i < sp.ncaps; i++) fprintf(pf, " %s", pkg_cap_name(sp.caps[i]));
    fprintf(pf, "\nnamespace:\n");
    for (int i = 0; i < sp.ngrant; i++) fprintf(pf, "  %s %s\n", sp.grant[i].ro ? "ro" : "rw", sp.grant[i].prefix);
    fprintf(pf, "provides: %s.embx\n", sp.name);
    fclose(pf);

    printf("PKGBUILD %s %s -> %s.{embx,ns,pkg} (on-device, unsigned local build)\n", sp.name, sp.version, sp.name);
    return 0;
}
