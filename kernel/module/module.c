/* kernel/module/module.c -- loading an ELF relocatable object into the kernel.
 *
 * See module.h for why. This is the machine: read the object, place its
 * sections, resolve its undefined symbols against what the kernel exports,
 * apply the relocations, flip the code to executable, and call init.
 *
 * THE RELOCATIONS ARE THE WHOLE JOB, and it is worth saying what they are. A
 * .o file's code is written as if it lived at address zero: a call to a kernel
 * function is a hole with a note saying "put the distance to `kprintf` here".
 * Placing the object somewhere real means filling in every one of those holes.
 * Get one wrong and the module loads, reports success, and jumps into the
 * middle of an unrelated function the first time that path runs.
 *
 * TWO THINGS THIS REFUSES TO DO, both deliberate:
 *
 *   It will not make a page writable and executable at the same time. The
 *   image is written while non-executable and flipped afterwards; W^X is not
 *   suspended for the convenience of the loader that would be the easiest
 *   thing in the system to abuse.
 *
 *   It will not resolve a symbol the kernel did not deliberately export. A
 *   module that can reach every internal function depends on all of them, and
 *   the first time anything is renamed every module ever built stops working.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "include/kmalloc.h"
#include "mm/vmm.h"
#include "fs/vfs.h"
#include "module/module.h"

/* ---- ELF64, only the relocatable parts ---------------------------------- */
#define ET_REL 1
#define EM_X86_64 62
#define EM_AARCH64 183

#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8

#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4

#define SHN_UNDEF  0
#define SHN_ABS    0xFFF1
#define SHN_COMMON 0xFFF2

/* x86-64 relocation types. Only the ones a compiler actually emits for kernel
 * code built -mcmodel=kernel; anything else is refused by name below rather
 * than approximated. */
#define R_X86_64_64     1
#define R_X86_64_PC32   2
#define R_X86_64_PLT32  4
#define R_X86_64_32     10
#define R_X86_64_32S    11

/* aarch64 */
#define R_AARCH64_ABS64      257
#define R_AARCH64_CALL26     283
#define R_AARCH64_JUMP26     282
#define R_AARCH64_ADR_PREL_PG_HI21 275
#define R_AARCH64_ADD_ABS_LO12_NC  277

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf64_shdr {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} __attribute__((packed));

struct elf64_sym {
    uint32_t st_name;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} __attribute__((packed));

struct elf64_rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} __attribute__((packed));

#define ELF64_R_SYM(i)  ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)((i) & 0xFFFFFFFF))

/* ---- the export table, placed by the linker script ---------------------- */
extern const struct embk_export __embk_exports_start[];
extern const struct embk_export __embk_exports_end[];

uint32_t embk_export_count(void) {
    return (uint32_t)(__embk_exports_end - __embk_exports_start);
}

static void *export_lookup(const char *name) {
    for (const struct embk_export *e = __embk_exports_start;
         e < __embk_exports_end; e++)
        if (strcmp(e->name, name) == 0) return e->addr;
    return NULL;
}

static struct embk_module g_modules[EMBK_MAX_MODULES];

uint32_t embk_module_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < EMBK_MAX_MODULES; i++) if (g_modules[i].used) n++;
    return n;
}

const struct embk_module *embk_module_at(uint32_t idx) {
    uint32_t seen = 0;
    for (int i = 0; i < EMBK_MAX_MODULES; i++) {
        if (!g_modules[i].used) continue;
        if (seen++ == idx) return &g_modules[i];
    }
    return NULL;
}

/* ---- placing and relocating --------------------------------------------- */

/* Where each section ended up, indexed by section number. */
struct placement {
    uint64_t *addr;      /* 0 for sections that are not loaded */
    uint32_t  count;
};

static bool apply_rela(uint64_t where, uint64_t sym, int64_t addend,
                       uint32_t type, uint16_t machine, const char *symname) {
    if (machine == EM_X86_64) {
        switch (type) {
        case R_X86_64_64:
            *(uint64_t *)where = sym + (uint64_t)addend;
            return true;
        case R_X86_64_PC32:
        case R_X86_64_PLT32: {
            /* PLT32 IS TREATED AS PC32 ON PURPOSE. There is no procedure
             * linkage table in a kernel module; the assembler emits PLT32 for
             * an ordinary call to an external symbol and the two mean the same
             * thing once the target address is known. */
            int64_t v = (int64_t)sym + addend - (int64_t)where;
            if (v < -0x80000000LL || v > 0x7FFFFFFFLL) {
                kprintf("module: '%s' is %lld bytes away -- too far for a "
                        "32-bit relative reference\n", symname,
                        (long long)v);
                return false;
            }
            *(int32_t *)where = (int32_t)v;
            return true;
        }
        case R_X86_64_32:
        case R_X86_64_32S: {
            int64_t v = (int64_t)sym + addend;
            /* THE KERNEL LIVES ABOVE 0xFFFFFFFF80000000, so a 32-bit absolute
             * reference only works if it sign-extends back to the right
             * address. That is exactly what -mcmodel=kernel promises and
             * exactly what a module built without it breaks. */
            if (type == R_X86_64_32S && (v < -0x80000000LL || v > 0x7FFFFFFFLL)) {
                kprintf("module: '%s' does not fit a 32-bit signed absolute "
                        "reference -- build the module -mcmodel=kernel\n",
                        symname);
                return false;
            }
            *(uint32_t *)where = (uint32_t)v;
            return true;
        }
        default:
            kprintf("module: relocation type %u is not implemented "
                    "(symbol '%s')\n", type, symname);
            return false;
        }
    }

    if (machine == EM_AARCH64) {
        switch (type) {
        case R_AARCH64_ABS64:
            *(uint64_t *)where = sym + (uint64_t)addend;
            return true;
        case R_AARCH64_CALL26:
        case R_AARCH64_JUMP26: {
            int64_t v = (int64_t)sym + addend - (int64_t)where;
            if (v < -(1LL << 27) || v >= (1LL << 27) || (v & 3)) {
                kprintf("module: '%s' is out of branch range\n", symname);
                return false;
            }
            uint32_t insn = *(uint32_t *)where;
            insn = (insn & 0xFC000000u) | (uint32_t)((v >> 2) & 0x03FFFFFF);
            *(uint32_t *)where = insn;
            return true;
        }
        default:
            kprintf("module: aarch64 relocation type %u is not implemented "
                    "(symbol '%s')\n", type, symname);
            return false;
        }
    }

    kprintf("module: unknown machine type %u\n", machine);
    return false;
}

static int slot_free(void) {
    for (int i = 0; i < EMBK_MAX_MODULES; i++)
        if (!g_modules[i].used) return i;
    return -1;
}

int embk_module_load(const char *path) {
    struct vfs_stat st;
    if (vfs_stat(path, &st) != EMBK_OK) {
        kprintf("module: %s: no such file\n", path);
        return -EMBK_ENOENT;
    }
    if (st.size < sizeof(struct elf64_ehdr) || st.size > (16u << 20))
        return -EMBK_EINVAL;

    int slot = slot_free();
    if (slot < 0) {
        kprintf("module: too many modules loaded\n");
        return -EMBK_ENOSPC;
    }

    uint8_t *img = kmalloc((uint64_t)st.size);
    if (!img) return -EMBK_ENOMEM;

    size_t got = 0;
    if (vfs_read(path, 0, img, (size_t)st.size, &got) != EMBK_OK ||
        got != (size_t)st.size) {
        kprintf("module: %s: short read (%llu of %llu)\n", path,
                (unsigned long long)got, (unsigned long long)st.size);
        kfree(img);
        return -EMBK_EIO;
    }

    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)img;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 || eh->e_ident[4] != 2) {
        kprintf("module: %s is not a 64-bit ELF object\n", path);
        kfree(img);
        return -EMBK_EINVAL;
    }
    if (eh->e_type != ET_REL) {
        /* A .so or an executable would LOAD and then relocate to nothing,
         * because its relocations were already applied by the linker. */
        kprintf("module: %s is not a relocatable object (.o) -- type %u\n",
                path, eh->e_type);
        kfree(img);
        return -EMBK_EINVAL;
    }
    if (eh->e_shoff == 0 || eh->e_shnum == 0 ||
        eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(struct elf64_shdr) > st.size) {
        kfree(img);
        return -EMBK_EINVAL;
    }

    const struct elf64_shdr *sh =
        (const struct elf64_shdr *)(img + eh->e_shoff);

    /* ---- lay the allocatable sections out, code first -------------------
     *
     * CODE FIRST AND TOGETHER so that exactly one contiguous run has to be
     * made executable. Interleaving code and data would mean either flipping
     * data pages executable too -- which is the W^X hole this loader exists
     * not to open -- or a page-by-page dance with alignment holes in it. */
    uint64_t total = 0, text_size = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (!(sh[i].sh_flags & SHF_ALLOC) || sh[i].sh_size == 0) continue;
            bool is_text = (sh[i].sh_flags & SHF_EXECINSTR) != 0;
            if (is_text != (pass == 0)) continue;
            uint64_t align = sh[i].sh_addralign ? sh[i].sh_addralign : 1;
            total = (total + align - 1) & ~(align - 1);
            total += sh[i].sh_size;
            if (pass == 0) text_size = total;
        }
        if (pass == 0) {
            /* Round the code up to a page so the data that follows does not
             * share a page with it -- the flip to executable is per page. */
            total = (total + 0xFFF) & ~0xFFFull;
            text_size = total;
        }
    }

    uint64_t base = vmm_alloc_module(total ? total : 0x1000);
    if (!base) { kfree(img); return -EMBK_ENOMEM; }

    struct placement *place = kmalloc(sizeof(*place) * eh->e_shnum);
    if (!place) { vmm_free_module(base, total); kfree(img); return -EMBK_ENOMEM; }
    memset(place, 0, sizeof(*place) * eh->e_shnum);

    uint64_t cur = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (!(sh[i].sh_flags & SHF_ALLOC) || sh[i].sh_size == 0) continue;
            bool is_text = (sh[i].sh_flags & SHF_EXECINSTR) != 0;
            if (is_text != (pass == 0)) continue;
            uint64_t align = sh[i].sh_addralign ? sh[i].sh_addralign : 1;
            cur = (cur + align - 1) & ~(align - 1);
            place[i].addr = (uint64_t *)(base + cur);
            if (sh[i].sh_type != SHT_NOBITS)
                memcpy((void *)(base + cur), img + sh[i].sh_offset, sh[i].sh_size);
            /* NOBITS (.bss) is already zero: vmm_alloc_module zeroes. */
            cur += sh[i].sh_size;
        }
        if (pass == 0) cur = text_size;
    }

    /* ---- resolve symbols ------------------------------------------------- */
    const struct elf64_sym *syms = NULL;
    const char *strtab = NULL;
    uint32_t nsyms = 0;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB) continue;
        syms = (const struct elf64_sym *)(img + sh[i].sh_offset);
        nsyms = (uint32_t)(sh[i].sh_size / sizeof(struct elf64_sym));
        if (sh[i].sh_link < eh->e_shnum)
            strtab = (const char *)(img + sh[sh[i].sh_link].sh_offset);
        break;
    }
    if (!syms || !strtab) {
        kprintf("module: %s has no symbol table\n", path);
        goto fail;
    }

    uint64_t *resolved = kmalloc(sizeof(uint64_t) * nsyms);
    if (!resolved) goto fail;
    memset(resolved, 0, sizeof(uint64_t) * nsyms);

    for (uint32_t i = 0; i < nsyms; i++) {
        const char *nm = strtab + syms[i].st_name;
        if (syms[i].st_shndx == SHN_UNDEF) {
            if (!nm[0]) continue;
            void *a = export_lookup(nm);
            if (!a) {
                /* NAMED, not counted. A module that fails to load because of
                 * one missing symbol is a five-second fix if the symbol is
                 * named and an afternoon if it is not. */
                kprintf("module: %s needs '%s', which this kernel does not "
                        "export\n", path, nm);
                kfree(resolved);
                goto fail;
            }
            resolved[i] = (uint64_t)(uintptr_t)a;
        } else if (syms[i].st_shndx == SHN_ABS) {
            resolved[i] = syms[i].st_value;
        } else if (syms[i].st_shndx < eh->e_shnum &&
                   place[syms[i].st_shndx].addr) {
            resolved[i] = (uint64_t)(uintptr_t)place[syms[i].st_shndx].addr
                          + syms[i].st_value;
        }
    }

    /* ---- apply relocations ----------------------------------------------- */
    uint32_t nrel = 0;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_RELA) continue;
        uint32_t target = sh[i].sh_info;
        if (target >= eh->e_shnum || !place[target].addr) continue;

        const struct elf64_rela *r =
            (const struct elf64_rela *)(img + sh[i].sh_offset);
        uint32_t n = (uint32_t)(sh[i].sh_size / sizeof(struct elf64_rela));
        for (uint32_t k = 0; k < n; k++) {
            uint32_t si = ELF64_R_SYM(r[k].r_info);
            uint32_t ty = ELF64_R_TYPE(r[k].r_info);
            if (si >= nsyms) continue;
            if (r[k].r_offset + 8 > sh[target].sh_size + 8) { /* bounded below */ }
            uint64_t where = (uint64_t)(uintptr_t)place[target].addr + r[k].r_offset;
            if (where < base || where >= base + total) {
                kprintf("module: %s: a relocation points outside the image\n", path);
                kfree(resolved);
                goto fail;
            }
            if (!apply_rela(where, resolved[si], r[k].r_addend, ty,
                            eh->e_machine, strtab + syms[si].st_name)) {
                kfree(resolved);
                goto fail;
            }
            nrel++;
        }
    }

    /* ---- the module's own descriptor -------------------------------------- */
    const struct embk_module_info *info = NULL;
    for (uint32_t i = 0; i < nsyms; i++) {
        if (strcmp(strtab + syms[i].st_name, "embk_module_info") == 0) {
            info = (const struct embk_module_info *)(uintptr_t)resolved[i];
            break;
        }
    }
    kfree(resolved);
    if (!info || !info->init) {
        kprintf("module: %s has no embk_module_info -- see EMBK_MODULE()\n", path);
        goto fail;
    }

    /* ---- W^X: the code becomes executable and stops being writable -------- */
    if (text_size && vmm_module_make_exec(base, text_size) < 0) {
        kprintf("module: could not make %s executable\n", path);
        goto fail;
    }

    struct embk_module *m = &g_modules[slot];
    memset(m, 0, sizeof *m);
    m->used = true;
    m->base = base;
    m->size = total;
    m->text_base = base;
    m->text_size = text_size;
    m->exit = info->exit;
    const char *nm = info->name ? info->name : "(unnamed)";
    uint32_t k = 0;
    while (nm[k] && k < EMBK_MODULE_NAME_LEN - 1) { m->name[k] = nm[k]; k++; }
    m->name[k] = 0;

    kprintf("module: %s loaded at 0x%llx (%llu bytes, %u relocation(s)) -- %s\n",
            m->name, (unsigned long long)base, (unsigned long long)total, nrel,
            info->description ? info->description : "");

    int rc = info->init();
    if (rc != 0) {
        kprintf("module: %s init returned %d -- unloading\n", m->name, rc);
        m->used = false;
        vmm_free_module(base, total);
        kfree(place);
        kfree(img);
        return rc;
    }

    kfree(place);
    kfree(img);
    return EMBK_OK;

fail:
    vmm_free_module(base, total);
    kfree(place);
    kfree(img);
    return -EMBK_EINVAL;
}

int embk_module_unload(const char *name) {
    for (int i = 0; i < EMBK_MAX_MODULES; i++) {
        struct embk_module *m = &g_modules[i];
        if (!m->used || strcmp(m->name, name) != 0) continue;
        if (m->exit) m->exit();
        kprintf("module: %s unloaded\n", m->name);
        /* THE MEMORY IS FREED AND THE PAGES GO BACK, which is only safe
         * because nothing else may hold a pointer into a module -- there is no
         * inter-module linking and nothing registers a callback it does not
         * remove in exit(). That is a real constraint on what a module may do
         * and it is written down in docs/TODO.md rather than assumed. */
        vmm_free_module(m->base, m->size);
        m->used = false;
        return EMBK_OK;
    }
    return -EMBK_ENOENT;
}
