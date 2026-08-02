#ifndef EMBK_PKG_EMBXGEN_H
#define EMBK_PKG_EMBXGEN_H
/* On-OS EMBX producer: repackage a fully-linked static ELF into a byte-exact
 * EMBX APP image, adding the one thing ELF cannot carry -- a declared capability
 * table (EMBX_Specification_v2 §3). The C twin of tools/embx/mkembx.py, so a
 * package can be generated ON THE DEVICE (PK2b), not only cross-built. Produces
 * the same bytes as mkembx for the same input, so the build_id matches. */
#include <stddef.h>
#include <stdint.h>

/* Write `elf_path` as an EMBX to `out_path`, declaring capability ids caps[0..n)
 * (ascending, unique -- the caller sorts/dedups). 0 on success; -1 with a reason
 * in err[0..errsz). */
int embxgen_write(const char *elf_path, const int *caps, int ncaps,
                  const char *out_path, char *err, size_t errsz);

#endif /* EMBK_PKG_EMBXGEN_H */
