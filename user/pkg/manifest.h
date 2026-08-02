#ifndef EMBK_PKG_MANIFEST_H
#define EMBK_PKG_MANIFEST_H
/* The package manifest (docs/PACKAGING_AND_SDK.md §3): a human-readable, single
 * source of truth that mirrors an app's declared authority -- its capabilities
 * (the EMBX cap table) and its namespace (the UP4 .ns manifest) -- plus its
 * identity (name/version/abi/build_id). `pkg` cross-checks it against the real
 * EMBX so the human declaration can never drift from the binary. */
#include <stddef.h>
#include <stdint.h>

#define PKG_MAX_CAPS 16
#define PKG_MAX_NS   8

struct pkg_ns { int ro; char prefix[256]; };   /* ro=1 read-only, 0 read-write */

struct pkg_manifest {
    char     name[64];
    char     version[32];
    uint32_t abi;
    uint8_t  build_id[32];
    int      have_build_id;
    int      caps[PKG_MAX_CAPS];   /* EMBX cap_ids (1=filesystem, 2=network, ...) */
    int      ncaps;
    struct pkg_ns ns[PKG_MAX_NS];
    int      nns;
    char     provides[256];        /* the EMBX filename within the bundle */
    uint8_t  signature[64];        /* ECDSA P-256 r||s over the canonical manifest */
    int      have_sig;
};

/* Capability id <-> lowercase name (EMBX §5.6 / kernel capabilities.h). */
int         pkg_cap_id(const char *name);   /* 0 if unknown */
const char *pkg_cap_name(int id);           /* NULL if invalid */

/* Parse a .pkg manifest (text, len bytes). On success fills *m and returns 0; on
 * a malformed manifest returns -1 and writes a reason into err[0..errsz). */
int pkg_manifest_parse(const char *text, size_t len, struct pkg_manifest *m,
                       char *err, size_t errsz);

#endif /* EMBK_PKG_MANIFEST_H */
