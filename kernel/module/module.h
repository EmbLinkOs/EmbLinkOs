/* kernel/module/module.h -- code the kernel did not ship with.
 *
 * WHY A MODULE SYSTEM AT ALL, given this kernel compiles every driver in. The
 * answer is the goal in docs/PILLARS.md: a daily driver on a machine nobody
 * has described in advance. That machine has a graphics chip, a wireless part
 * and a touchpad this tree has never seen, and the alternative to loading a
 * driver for them is rebuilding and reinstalling the kernel -- which means
 * nobody but us can add one, and it means an update to one driver rewrites
 * the file the firmware verifies.
 *
 * WHAT A MODULE IS: an ordinary ELF64 RELOCATABLE object, the .o a compiler
 * already emits. Not a shared library -- there is no dynamic linker in the
 * kernel and no reason to invent one. The kernel resolves the object's
 * undefined symbols against a table of what it deliberately EXPORTS, applies
 * the relocations, and calls an entry point.
 *
 * WHAT IS DELIBERATELY NOT HERE, because each is a way to be subtly wrong:
 * no symbol versioning, no inter-module dependencies, no lazy binding, and no
 * unloading while anything is still using the module. See docs/TODO.md.
 */
#ifndef _EMBK_MODULE_H_
#define _EMBK_MODULE_H_

#include <stdint.h>
#include "include/types.h"

/* ---- what the kernel lets a module call ---------------------------------
 *
 * AN EXPLICIT LIST, not "every global symbol". A module that can call
 * anything in the kernel is a module that depends on every internal detail of
 * it, and the first refactor breaks every module ever built. Exporting is a
 * decision, made one symbol at a time, and the list IS the kernel's contract
 * with the drivers people write for it. */
struct embk_export {
    const char *name;
    void       *addr;
};

#define EMBK_EXPORT(sym)                                                   \
    static const struct embk_export __embk_export_##sym                    \
        __attribute__((used, section(".embk_exports"))) = {                \
            #sym, (void *)(uintptr_t)&sym }

/* ---- what a module must define ------------------------------------------
 *
 * One descriptor, by a fixed name, so the loader looks for exactly one thing.
 * `init` returns 0 to stay loaded; anything else and the module is unloaded
 * and the failure reported -- a driver whose hardware is absent should say so
 * rather than sit in the kernel doing nothing. */
struct embk_module_info {
    const char *name;
    const char *author;
    const char *description;
    int  (*init)(void);
    void (*exit)(void);
};

#define EMBK_MODULE(nm, auth, desc, initfn, exitfn)                        \
    const struct embk_module_info embk_module_info = {                     \
        .name = nm, .author = auth, .description = desc,                   \
        .init = initfn, .exit = exitfn }

/* ---- the loader ---------------------------------------------------------- */

#define EMBK_MODULE_NAME_LEN 32
#define EMBK_MAX_MODULES     16

struct embk_module {
    bool     used;
    char     name[EMBK_MODULE_NAME_LEN];
    uint64_t base;          /* the loaded image                            */
    uint64_t size;
    uint64_t text_base;     /* the part that was made executable           */
    uint64_t text_size;
    void   (*exit)(void);
    bool     signed_ok;     /* it carried a signature this kernel trusts   */
};

/* Load a module from a file. Returns 0, or a negative EMBK_* error.
 * The name reported is the module's own, not the file's. */
int embk_module_load(const char *path);

/* Unload by name. -EMBK_EBUSY if it is not loaded. */
int embk_module_unload(const char *name);

uint32_t embk_module_count(void);
const struct embk_module *embk_module_at(uint32_t i);

/* How many symbols the kernel exports -- the size of its contract. */
uint32_t embk_export_count(void);

#endif
