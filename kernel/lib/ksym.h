#ifndef __KSYM_H__
#define __KSYM_H__

#include <stdint.h>
#include <stddef.h>

/* The kernel panic symbolizer (EMBDBG_Specification.md §7). Loads the kernel's
 * OWN .embdbg (a LINE+FUNCS+STRTAB sidecar produced by `embdbg kernel.elf
 * emit-kernel`) and turns a raw kernel address into func+off (file:line). No
 * debugger, no userspace, no producer beyond the format's cheapest tables —
 * this is the most self-contained consumer of the .embdbg format, and it makes
 * isr_handler's dump read `isr_handler+0x0 (isr.c:63)` instead of a bare hex
 * RIP. Reuses the exact seekable binary-searchable arrays the spec designed. */

/* Point the symbolizer at an in-memory .embdbg image. Returns 0 on a valid
 * image (magic + a FUNCS section), -1 otherwise. The bytes must outlive use
 * (loaded once at boot into a kernel buffer that is never freed). */
int ksym_load(const void *data, uint32_t len);

int ksym_ready(void);

/* Write "func+0xN (file:line)" (or "func+0xN", or "0x… ?") for addr into out. */
void ksym_symbolize(uint64_t addr, char *out, size_t n);

#endif /* __KSYM_H__ */
