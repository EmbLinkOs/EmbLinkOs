/* kernel/loader/elf.c -- the user ELF loader.
 *
 * Loads a static ET_EXEC; a POSITION-INDEPENDENT executable (ET_DYN, loaded at
 * a per-process random bias -- see elf_load_at); and DYNAMICALLY-linked
 * executables: an app with a PT_DYNAMIC + DT_NEEDED libembk.so. The kernel IS the
 * dynamic linker (there is no userspace ld.so, and no PT_INTERP). It loads the
 * one shared object (the PIC UI toolkit) at a fixed bias, then does two-way
 * symbol resolution + relocation:
 *   - the app imports toolkit functions -> resolved to libembk.so's exports;
 *   - libembk.so imports libc (malloc, memcpy, sinf, ...) -> resolved back to
 *     the app's --export-dynamic'd static newlib (newlib is non-PIC, so libc
 *     can't live in the .so; it stays in each app and the .so binds to it).
 * Relocs are applied eagerly (no lazy PLT binding). */
#include "loader/elf.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "include/kmalloc.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "drivers/char/serial.h"
#include "include/errno.h"
#include "include/kprintf.h"   /* load_segments diagnostics */

#define PAGE_SIZE_4K 0x1000ULL
#define PAGE_DOWN(x) ((x) & ~(PAGE_SIZE_4K - 1))
#define PAGE_UP(x)   (((x) + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1))

/* Translate ELF p_flags into VMM flags. NOTE the inversion: executable is the
 * ABSENCE of NX; non-exec data SETS NX. Write maps VMM_WRITABLE. Always USER. */
static uint64_t elf_flags_to_vmm(uint32_t p_flags)
{
    uint64_t f = VMM_USER;
    if (p_flags & PF_W) f |= VMM_WRITABLE;
    if (p_flags & PF_X) {
        /* Ask POSITIVELY for execution as well as leaving VMM_NX clear. The
         * two say the same thing to different machines: x86 reads the absent
         * NX bit, aarch64 reads VMM_EXEC and clears UXN. Setting only one of
         * them mapped this segment non-executable on aarch64 and the program
         * faulted on its first instruction fetch -- a permission fault at the
         * entry point, which reads like a loader bug and is a flag bug. */
        f |= VMM_EXEC;
    } else {
        f |= VMM_NX;                        /* no exec permission -> set NX */
    }
    return f;
}

static int kstreq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Read a whole file off the VFS into a fresh kmalloc'd buffer (caller frees). */
static int read_file_kbuf(const char *path, uint8_t **out_buf, uint64_t *out_len)
{
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) return fd;

    struct vfs_stat st;
    int rc = vfs_fd_fstat(fd, &st);
    if (rc != EMBK_OK) { vfs_close(fd); return rc; }
    if (st.size == 0)  { vfs_close(fd); return -EMBK_EINVAL; }

    uint8_t *buf = kmalloc(st.size);
    if (!buf) { vfs_close(fd); return -EMBK_ENOMEM; }

    uint64_t total = 0;
    while (total < st.size) {
        size_t got = 0;
        rc = vfs_fd_read(fd, buf + total, (size_t)(st.size - total), &got);
        if (rc != EMBK_OK) { kfree(buf); vfs_close(fd); return rc; }
        if (got == 0)      { kfree(buf); vfs_close(fd); return -EMBK_EIO; }
        total += got;
    }
    vfs_close(fd);
    *out_buf = buf;
    *out_len = total;
    return EMBK_OK;
}

/* Map + copy every PT_LOAD segment of `image` into `pml4_phys` at bias+p_vaddr.
 * bias == 0 for an ET_EXEC app; DYLIB_VA_BASE for the ET_DYN shared object. */
static int load_segments(const uint8_t *image, uint64_t image_len, uint64_t pml4_phys,
                         uint64_t bias, const struct elf64_phdr *ph, uint16_t phnum)
{
    for (uint16_t i = 0; i < phnum; i++) {
        const struct elf64_phdr *p = &ph[i];
        if (p->p_type != PT_LOAD)
            continue;
        if (p->p_filesz > p->p_memsz) {
            kprintf("elf: segment %u filesz 0x%lx > memsz 0x%lx\n",
                    (unsigned)i, p->p_filesz, p->p_memsz);
            return -EMBK_EINVAL;
        }
        if (p->p_offset + p->p_filesz > image_len) {
            kprintf("elf: segment %u ends at 0x%lx, image is 0x%lx\n",
                    (unsigned)i, p->p_offset + p->p_filesz, image_len);
            return -EMBK_EINVAL;
        }
        uint64_t vstart = bias + p->p_vaddr;
        if (vstart + p->p_memsz >= DIRECT_MAP_BASE) { /* user (lower half) only */
            kprintf("elf: segment %u spans 0x%lx..0x%lx, into kernel space\n",
                    (unsigned)i, vstart, vstart + p->p_memsz);
            return -EMBK_EINVAL;
        }

        uint64_t seg_start = PAGE_DOWN(vstart);
        uint64_t seg_end   = PAGE_UP(vstart + p->p_memsz);

        /* map writable first so the copy + any relocations can land */
        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE_4K) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) return -EMBK_ENOMEM;
            vmm_map_in(pml4_phys, va, phys, VMM_USER | VMM_WRITABLE);
        }

        /* copy file bytes, zero the .bss tail + alignment slack, via the kernel
         * direct map of each freshly-mapped frame (pml4_phys isn't the live CR3). */
        const uint8_t *src = image + p->p_offset;
        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE_4K) {
            uint64_t phys = vmm_get_phys_in(pml4_phys, va) & ~0xFFFULL;
            uint8_t *page = (uint8_t *)P2V(phys);
            for (uint64_t off = 0; off < PAGE_SIZE_4K; off++) {
                uint64_t v = va + off;
                if (v >= vstart && v < vstart + p->p_filesz)
                    page[off] = src[v - vstart];
                else
                    page[off] = 0;
            }
        }

        /* tighten to the real p_flags (drop WRITABLE on code, set NX on data) */
        uint64_t vmm_flags = elf_flags_to_vmm(p->p_flags);
        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE_4K) {
            uint64_t phys = vmm_get_phys_in(pml4_phys, va) & ~0xFFFULL;
            vmm_map_in(pml4_phys, va, phys, vmm_flags);
        }
    }
    return EMBK_OK;
}

/* ------------------------------------------------------------------------- */
/* dynamic linking                                                           */
/* ------------------------------------------------------------------------- */

/* Module-relative vaddr -> pointer INTO the file image (for reading dyn tables).
 * Uniform for the app (absolute vaddrs, p_vaddr absolute) and the .so (both
 * module-relative). Returns 0 if vaddr isn't covered by a PT_LOAD's file data. */
static const uint8_t *img_at(const uint8_t *image, const struct elf64_phdr *ph,
                             uint16_t phnum, uint64_t vaddr)
{
    for (uint16_t i = 0; i < phnum; i++) {
        const struct elf64_phdr *p = &ph[i];
        if (p->p_type == PT_LOAD && vaddr >= p->p_vaddr && vaddr < p->p_vaddr + p->p_filesz)
            return image + p->p_offset + (vaddr - p->p_vaddr);
    }
    return 0;
}

struct dynmod {
    const uint8_t *image;
    const struct elf64_phdr *ph;
    uint16_t phnum;
    uint64_t bias;                    /* load bias (0 for the app)            */
    const struct elf64_sym *symtab;   /* IMAGE pointer to .dynsym             */
    const char *strtab;               /* IMAGE pointer to .dynstr             */
    uint32_t symcount;                /* nchain of DT_HASH (== #dynsyms)      */
};

/* Fill `m` from the module's PT_DYNAMIC and return its reloc-table locations. */
static int parse_dynamic(const uint8_t *image, const struct elf64_phdr *ph, uint16_t phnum,
                         uint64_t bias, struct dynmod *m,
                         uint64_t *rela_v, uint64_t *rela_sz,
                         uint64_t *jmprel_v, uint64_t *jmprel_sz,
                         int *needed_out)
{
    const struct elf64_dyn *dyn = 0;
    for (uint16_t i = 0; i < phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) { dyn = (const struct elf64_dyn *)(image + ph[i].p_offset); break; }
    if (!dyn) return -EMBK_EINVAL;

    uint64_t symtab_v = 0, strtab_v = 0, hash_v = 0;
    int needed = 0;
    *rela_v = *rela_sz = *jmprel_v = *jmprel_sz = 0;
    for (const struct elf64_dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_NEEDED:   needed++;            break;
        case DT_SYMTAB:   symtab_v   = d->d_val; break;
        case DT_STRTAB:   strtab_v   = d->d_val; break;
        case DT_HASH:     hash_v     = d->d_val; break;
        case DT_RELA:     *rela_v    = d->d_val; break;
        case DT_RELASZ:   *rela_sz   = d->d_val; break;
        case DT_JMPREL:   *jmprel_v  = d->d_val; break;
        case DT_PLTRELSZ: *jmprel_sz = d->d_val; break;
        default: break;
        }
    }
    m->image = image; m->ph = ph; m->phnum = phnum; m->bias = bias;
    m->symtab = (const struct elf64_sym *)img_at(image, ph, phnum, symtab_v);
    m->strtab = (const char *)img_at(image, ph, phnum, strtab_v);
    const uint32_t *hash = (const uint32_t *)img_at(image, ph, phnum, hash_v);
    m->symcount = hash ? hash[1] : 0;   /* DT_HASH: [nbucket][nchain][...]    */
    if (needed_out) *needed_out = needed;
    if (!m->symtab || !m->strtab || !m->symcount) return -EMBK_EINVAL;
    return EMBK_OK;
}

/* Resolve a DEFINED global symbol by name across the modules (skipping
 * `skip` if non-NULL -- an ELF_RELOC_COPY source must come from ANOTHER
 * module, never the requester's own dynsym). Fills def_mod/def_sym when
 * provided.
 *
 * Returns TRUE/FALSE for "was it found", and writes the value through
 * *val_out. It does NOT return the value, because a symbol's legitimate value
 * can be 0 and a returned 0 cannot be told apart from "not found" -- which is
 * precisely the bug that made every tcc-built EmUI app fail to load: the six
 * weak crt0 bracket symbols are defined SHN_ABS 0 by emlink_dynstubs.o, were
 * FOUND here, and were then reported as UNRESOLVED by the caller.
 *
 * SHN_ABS is an absolute value, not an address: it must NOT be biased. For the
 * ET_EXEC app (bias 0) that is invisible; for the .so (bias DYLIB_VA_BASE) it
 * would turn a 0 into a wild pointer. */
static bool resolve_sym_ex(struct dynmod *mods, int nmods, const char *name,
                           const struct dynmod *skip, uint64_t *val_out,
                           struct dynmod **def_mod, const struct elf64_sym **def_sym)
{
    for (int k = 0; k < nmods; k++) {
        struct dynmod *m = &mods[k];
        if (m == skip) continue;
        for (uint32_t i = 0; i < m->symcount; i++) {
            const struct elf64_sym *s = &m->symtab[i];
            if (s->st_shndx == SHN_UNDEF || s->st_name == 0) continue;
            if (kstreq(m->strtab + s->st_name, name)) {
                if (def_mod) *def_mod = m;
                if (def_sym) *def_sym = s;
                if (val_out)
                    *val_out = (s->st_shndx == SHN_ABS) ? s->st_value
                                                        : m->bias + s->st_value;
                return true;
            }
        }
    }
    return false;
}

static bool resolve_sym(struct dynmod *mods, int nmods, const char *name, uint64_t *val_out)
{
    return resolve_sym_ex(mods, nmods, name, 0, val_out, 0, 0);
}

/* Write one 8-aligned qword into a loaded user page (via the kernel direct map,
 * so it works regardless of the page's user W permission). */
static int poke_user(uint64_t pml4, uint64_t va, uint64_t val)
{
    uint64_t phys = vmm_get_phys_in(pml4, va & ~0xFFFULL) & ~0xFFFULL;
    if (!phys) return -EMBK_EFAULT;
    *(volatile uint64_t *)(P2V(phys) + (va & 0xFFFULL)) = val;
    return EMBK_OK;
}

/* Arbitrary-length write into loaded user pages (ELF_RELOC_COPY payloads),
 * page-by-page via the direct map. src == NULL writes zeroes (a copy source
 * that lives in the .so's .bss has no file bytes -- it IS zeroes). */
static int poke_user_bytes(uint64_t pml4, uint64_t va, const uint8_t *src, uint64_t len)
{
    for (uint64_t off = 0; off < len; ) {
        uint64_t page_va = (va + off) & ~0xFFFULL;
        uint64_t phys = vmm_get_phys_in(pml4, page_va) & ~0xFFFULL;
        if (!phys) return -EMBK_EFAULT;
        uint64_t in_page = (va + off) & 0xFFFULL;
        uint64_t n = 0x1000ULL - in_page;
        if (n > len - off) n = len - off;
        uint8_t *dst = (uint8_t *)P2V(phys) + in_page;
        for (uint64_t k = 0; k < n; k++) dst[k] = src ? src[off + k] : 0;
        off += n;
    }
    return EMBK_OK;
}

/* Apply one RELA table belonging to module `m` (entries read from m->image;
 * results written into pml4). Symbols resolve across all `mods`. */
static int apply_relocs(uint64_t pml4, struct dynmod *m, struct dynmod *mods, int nmods,
                        uint64_t rela_v, uint64_t rela_sz)
{
    if (!rela_v || !rela_sz) return EMBK_OK;
    uint64_t n = rela_sz / sizeof(struct elf64_rela);
    for (uint64_t i = 0; i < n; i++) {
        const struct elf64_rela *r = (const struct elf64_rela *)
            img_at(m->image, m->ph, m->phnum, rela_v + i * sizeof(struct elf64_rela));
        if (!r) return -EMBK_EINVAL;
        uint32_t type = ELF64_R_TYPE(r->r_info);
        uint64_t where = m->bias + r->r_offset;
        uint64_t val;

        if (type == ELF_RELOC_NONE) {
            continue;                       /* padding; see ELF_RELOC_NONE */
        } else if (type == ELF_RELOC_RELATIVE) {
            val = m->bias + (uint64_t)r->r_addend;
        } else if (type == ELF_RELOC_COPY) {
            /* ET_EXEC app referencing a DATA object that lives in the .so: the
             * link editor reserved space in the APP (at r_offset, where the
             * app's own dynsym also defines the symbol -- so app-first symbol
             * search makes the .so's references bind to this copy) and asks
             * the loader to copy the initial value from the DEFINING module.
             * Source bytes come from the .so's file image; a .bss source has
             * no file bytes and is (correctly) zero-filled. */
            const struct elf64_sym *own = &m->symtab[ELF64_R_SYM(r->r_info)];
            const char *name = m->strtab + own->st_name;
            struct dynmod *dm = 0; const struct elf64_sym *ds = 0;
            if (!resolve_sym_ex(mods, nmods, name, m, 0, &dm, &ds)) {
                serial_write_string("ELF dynlink: COPY source not found: ");
                serial_write_string(name);
                serial_write_string("\n");
                return -EMBK_ENOEXEC;
            }
            uint64_t sz = own->st_size ? own->st_size : ds->st_size;
            const uint8_t *src = img_at(dm->image, dm->ph, dm->phnum, ds->st_value);
            if (poke_user_bytes(pml4, where, src, sz) != EMBK_OK) return -EMBK_EFAULT;
            continue;
        } else if (type == ELF_RELOC_ABS64 || type == ELF_RELOC_GLOB_DAT ||
                   type == ELF_RELOC_JUMP_SLOT) {
            const struct elf64_sym *s = &m->symtab[ELF64_R_SYM(r->r_info)];
            const char *name = m->strtab + s->st_name;
            uint64_t symval = 0;
            if (!resolve_sym(mods, nmods, name, &symval)) {
                /* Genuinely absent. A WEAK reference to it is legal and binds
                 * to 0 -- that is what weak MEANS, and the referring code is
                 * required to test the symbol before using it. Only a STRONG
                 * reference to a missing symbol is an error. */
                if (ELF64_ST_BIND(s->st_info) != STB_WEAK) {
                    serial_write_string("ELF dynlink: UNRESOLVED symbol '");
                    serial_write_string(name);
                    serial_write_string("'\n");
                    return -EMBK_ENOEXEC;
                }
                symval = 0;
            }
            val = (type == ELF_RELOC_ABS64) ? symval + (uint64_t)r->r_addend : symval;
        } else {
            kprintf("ELF dynlink: unhandled reloc type %u at index %llu\n",
                    (unsigned)type, (unsigned long long)i);
            return -EMBK_ENOEXEC;
        }
        if (poke_user(pml4, where, val) != EMBK_OK) return -EMBK_EFAULT;
    }
    return EMBK_OK;
}

/* The app is already segment-loaded at `app_bias` (0 for an ET_EXEC app, its
 * randomised exec_base for a PIE). Load /libembk.so at `dylib_base` and perform
 * the two-way link. */
static int dynamic_link(const uint8_t *app_image, uint64_t pml4,
                        uint64_t app_bias, uint64_t dylib_base,
                        const struct elf64_phdr *app_ph, uint16_t app_phnum)
{
    /* libembk.so is the sealed ABI (docs/USERSPACE.md D2 §3.1); it lives under
     * /system/lib now, not the old flat root. This is the loader's ONE hardwired
     * library path -- every dynamic app resolves DT_NEEDED libembk.so here. */
    uint8_t *so_buf = 0; uint64_t so_len = 0;
    int rc = read_file_kbuf("/system/lib/libembk.so", &so_buf, &so_len);
    if (rc < 0) { serial_write_string("ELF dynlink: /system/lib/libembk.so not found\n"); return rc; }

    if (so_len < sizeof(struct elf64_ehdr)) { kfree(so_buf); return -EMBK_EINVAL; }
    const struct elf64_ehdr *soeh = (const struct elf64_ehdr *)so_buf;
    if (soeh->e_ident[0] != 0x7F || soeh->e_ident[1] != 'E' ||
        soeh->e_ident[2] != 'L'  || soeh->e_ident[3] != 'F' ||
        soeh->e_ident[4] != 2 || soeh->e_type != ET_DYN || soeh->e_machine != ELF_ARCH_MACHINE) {
        kfree(so_buf); return -EMBK_ENOEXEC;
    }
    const struct elf64_phdr *so_ph = (const struct elf64_phdr *)(so_buf + soeh->e_phoff);

    rc = load_segments(so_buf, so_len, pml4, dylib_base, so_ph, soeh->e_phnum);
    if (rc != EMBK_OK) { kfree(so_buf); return rc; }

    struct dynmod app, so;
    uint64_t ar_v, ar_sz, aj_v, aj_sz, sr_v, sr_sz, sj_v, sj_sz;
    if (parse_dynamic(app_image, app_ph, app_phnum, app_bias, &app, &ar_v, &ar_sz, &aj_v, &aj_sz, 0) != EMBK_OK ||
        parse_dynamic(so_buf, so_ph, soeh->e_phnum, dylib_base, &so, &sr_v, &sr_sz, &sj_v, &sj_sz, 0) != EMBK_OK) {
        kfree(so_buf); return -EMBK_ENOEXEC;
    }
    struct dynmod mods[2] = { app, so };

    /* app relocs (its toolkit imports -> the .so), then .so relocs (internal
     * RELATIVE + its libc imports -> the app's exported newlib). */
    rc = apply_relocs(pml4, &app, mods, 2, ar_v, ar_sz);
    if (rc == EMBK_OK) rc = apply_relocs(pml4, &app, mods, 2, aj_v, aj_sz);
    if (rc == EMBK_OK) rc = apply_relocs(pml4, &so,  mods, 2, sr_v, sr_sz);
    if (rc == EMBK_OK) rc = apply_relocs(pml4, &so,  mods, 2, sj_v, sj_sz);

    if (rc == EMBK_OK)
        serial_write_string("ELF dynlink: /system/lib/libembk.so linked\n");
    kfree(so_buf);
    return rc;
}

/* A PIE with no DT_NEEDED: nothing to link AGAINST, but its own RELATIVE
 * relocations still have to be applied, because every absolute address the
 * linker could not fold into a PC-relative instruction -- an initialised
 * pointer, a jump table, a .init_array entry -- was written assuming a load
 * address of zero and is wrong by exactly the bias until we fix it.
 *
 * This is the whole of "self-relocation", and it is why a PIE needs no ld.so
 * here: the relocations are RELATIVE, so resolving them needs no symbol table
 * and no second module. The mods array is the app alone, which also means a
 * stray symbol-based relocation in a supposedly self-contained binary fails
 * loudly (UNRESOLVED) instead of silently binding to nothing. */
static int self_relocate(const uint8_t *image, uint64_t pml4, uint64_t bias,
                         const struct elf64_phdr *ph, uint16_t phnum)
{
    struct dynmod app;
    uint64_t r_v, r_sz, j_v, j_sz;
    if (parse_dynamic(image, ph, phnum, bias, &app, &r_v, &r_sz, &j_v, &j_sz, 0) != EMBK_OK)
        return -EMBK_ENOEXEC;
    struct dynmod mods[1] = { app };
    int rc = apply_relocs(pml4, &app, mods, 1, r_v, r_sz);
    if (rc == EMBK_OK) rc = apply_relocs(pml4, &app, mods, 1, j_v, j_sz);
    return rc;
}

/* ------------------------------------------------------------------------- */

/* Load an ELF64 executable already resident at `image` (image_len bytes) into
 * pml4_phys. Static ET_EXEC works unchanged; a dynamically-linked ET_EXEC (has
 * PT_DYNAMIC) additionally loads + links /libembk.so. Writes entry to *out. */
/* WHICH refusal. This function and load_segments() below reject a binary in
 * eight different ways and every one of them returns the same EINVAL, so a
 * user sees "can't run (err 22)" and learns nothing -- there is no way to tell
 * a truncated file from a wrong architecture from a segment that would land in
 * kernel space. One line each, on the serial log, naming the check. */
#define ELF_REFUSE(why) do { \
        kprintf("elf: refusing image: %s\n", (why)); \
        return -EMBK_EINVAL; \
    } while (0)

/* The dylib goes where the PROCESS says, which is now a random place per
 * process (struct user_layout). The old entry points keep the fixed base for
 * any caller that has no process to ask -- there are none in the tree today,
 * and one that appears will get the unrandomised layout rather than a crash. */
int elf_load_at(const uint8_t *image, uint64_t image_len, uint64_t pml4_phys,
                uint64_t exec_base, uint64_t dylib_base, uint64_t *entry_out)
{
    if (!image || !entry_out || !pml4_phys)
        ELF_REFUSE("null image/entry/pml4");
    if (image_len < sizeof(struct elf64_ehdr))
        ELF_REFUSE("shorter than an ELF header");

    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
        ELF_REFUSE("not an ELF (bad magic)");
    if (eh->e_ident[4] != 2)                              /* ELFCLASS64 */
        ELF_REFUSE("not ELFCLASS64");
    if (eh->e_machine != ELF_ARCH_MACHINE)
        ELF_REFUSE("wrong machine for this architecture");
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
        ELF_REFUSE("not ET_EXEC or ET_DYN");
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > image_len)
        ELF_REFUSE("program headers past the end of the image");

    /* THE LOAD BIAS, and why e_type is enough to choose it.
     *
     * An ET_EXEC binary names its own addresses: the linker script put .text at
     * 0x400000 and every absolute reference in the file says 0x400000-something.
     * There is exactly one place it can go, and the bias is 0.
     *
     * An ET_DYN executable -- a PIE -- was linked at zero and carries the
     * relocations needed to move it. We move it, per process, to a base the
     * kernel drew from its CSPRNG. That is the last fixed address in a user
     * address space: the heap, the stacks, mmap, the shared surfaces and the
     * toolkit were already randomised per process, and the executable's own
     * text was the one thing an attacker could still count on -- which is
     * exactly what a ROP chain is built out of.
     *
     * A refusal rather than a silent bias-of-0: an unaligned base would map the
     * image's pages at an offset the relocations do not agree with, and the
     * program would run with every pointer wrong by less than a page. That is a
     * far worse failure than not starting. */
    uint64_t bias = 0;
    if (eh->e_type == ET_DYN) {
        if (exec_base & (PAGE_SIZE_4K - 1))
            ELF_REFUSE("PIE load base is not page-aligned");
        bias = exec_base;
    }

    const struct elf64_phdr *ph = (const struct elf64_phdr *)(image + eh->e_phoff);

    int rc = load_segments(image, image_len, pml4_phys, bias, ph, eh->e_phnum);
    if (rc != EMBK_OK) return rc;

    /* PT_DYNAMIC means SOMETHING has to be relocated -- but what depends on
     * whether the binary imports anything. DT_NEEDED present: it wants the
     * toolkit, so load /system/lib/libembk.so and do the two-way link (which
     * also applies the app's own RELATIVE relocations, so a dynamic PIE needs
     * nothing extra). DT_NEEDED absent: a self-contained PIE, which still has
     * its own relocations to apply against the bias. */
    int has_dynamic = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) { has_dynamic = 1; break; }
    if (has_dynamic) {
        struct dynmod probe;
        uint64_t a, b, c, d;
        int needed = 0;
        if (parse_dynamic(image, ph, eh->e_phnum, bias, &probe, &a, &b, &c, &d, &needed) != EMBK_OK)
            ELF_REFUSE("PT_DYNAMIC without a usable symbol table");
        rc = needed ? dynamic_link(image, pml4_phys, bias, dylib_base, ph, eh->e_phnum)
                    : self_relocate(image, pml4_phys, bias, ph, eh->e_phnum);
        if (rc != EMBK_OK) return rc;
    } else if (eh->e_type == ET_DYN) {
        /* ET_DYN with no PT_DYNAMIC is not a thing a linker produces; refusing
         * it is cheaper than loading something whose relocations we cannot even
         * find and letting it fault at a random address. */
        ELF_REFUSE("ET_DYN with no PT_DYNAMIC");
    }

    *entry_out = bias + eh->e_entry;
    return EMBK_OK;
}

/* Read a user ELF off the filesystem and load it into pml4_phys. */
int elf_load_from_file_at(const char *path, uint64_t pml4_phys,
                          uint64_t exec_base, uint64_t dylib_base, uint64_t *entry_out)
{
    uint8_t *buf = 0; uint64_t total = 0;
    int rc = read_file_kbuf(path, &buf, &total);
    if (rc < 0) {
        serial_write_string("elf_load_from_file: read failed\n");
        return rc;
    }
    rc = elf_load_at(buf, total, pml4_phys, exec_base, dylib_base, entry_out);
    kfree(buf);
    return rc;
}


int elf_load(const uint8_t *image, uint64_t image_len, uint64_t pml4_phys, uint64_t *entry_out)
{
    return elf_load_at(image, image_len, pml4_phys, EXEC_VA_BASE, DYLIB_VA_BASE, entry_out);
}

int elf_load_from_file(const char *path, uint64_t pml4_phys, uint64_t *entry_out)
{
    return elf_load_from_file_at(path, pml4_phys, EXEC_VA_BASE, DYLIB_VA_BASE, entry_out);
}
