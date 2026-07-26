#ifndef _EMBBOOT_CONSOLE_H_
#define _EMBBOOT_CONSOLE_H_

#include "uefi.h"

/* Shared console I/O for EmbBoot. ST is set once by con_init() and used across
 * the loader and the menu (it also carries BootServices, ConOut, ConIn). */
extern EFI_SYSTEM_TABLE *ST;

void con_init(EFI_SYSTEM_TABLE *st);

void con_print(const char *s);        /* ASCII; '\n' -> CRLF */
void con_printhex(uint64_t v);        /* 0x-prefixed 16-digit hex */
void con_clear(void);
void con_attr(UINTN attr);            /* SetAttribute (EFI_* colour) */
void con_setpos(UINTN col, UINTN row);

/* Read one keystroke. Blocks up to timeout_ms (polls in 10ms steps); pass a
 * negative timeout to wait forever. Returns 1 and fills *key on a keypress, 0
 * on timeout. */
int con_read_key(EFI_INPUT_KEY *key, long timeout_ms);

/* Print a message and halt the CPU forever (unrecoverable). */
void con_die(const char *why);

#endif /* _EMBBOOT_CONSOLE_H_ */
