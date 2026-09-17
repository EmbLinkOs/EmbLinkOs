#include "console.h"

EFI_SYSTEM_TABLE *ST;

void con_init(EFI_SYSTEM_TABLE *st) { ST = st; }

/* Chunk-flush so ANY length prints in as few OutputString calls as possible
 * (a whole menu frame fits in one 512-char chunk => one firmware call). '\n' is
 * expanded to CRLF. */
void con_print(const char *s) {
    CHAR16 buf[520];
    while (*s) {
        unsigned i = 0;
        while (*s && i < 512) {
            if (*s == '\n') buf[i++] = '\r';
            buf[i++] = (CHAR16)(uint8_t)*s++;
        }
        buf[i] = 0;
        ST->ConOut->OutputString(ST->ConOut, buf);
    }
}

void con_printhex(uint64_t v) {
    static const char hx[] = "0123456789ABCDEF";
    char s[19];
    s[0] = '0'; s[1] = 'x';
    for (int i = 0; i < 16; i++) s[2 + i] = hx[(v >> ((15 - i) * 4)) & 0xF];
    s[18] = 0;
    con_print(s);
}

/* Drive the console with ANSI escapes through OutputString rather than the
 * firmware's ClearScreen/SetAttribute/SetCursorPosition entry points: OVMF's
 * graphics-console implementations of those fault on a serial-only console
 * (-display none), and ANSI is the more portable path for a boot menu anyway.
 * OutputString passes the ESC (0x1B) bytes straight through to the terminal. */
void con_clear(void) { con_print("\n"); }

/* Colour and cursor positioning are no-ops, for the reason menu.c gives: the
 * ANSI escapes these used to print are shown LITERALLY by a real machine's
 * graphics console. */
void con_attr(UINTN a) { (void)a; }

void con_setpos(UINTN c, UINTN r) { (void)c; (void)r; }

int con_read_key(EFI_INPUT_KEY *key, long timeout_ms) {
    long waited = 0;
    for (;;) {
        EFI_STATUS s = ST->ConIn->ReadKeyStroke(ST->ConIn, key);
        if (!EFI_ERROR(s)) return 1;              /* got a key */
        if (timeout_ms >= 0 && waited >= timeout_ms) return 0;   /* timed out */
        ST->BootServices->Stall(10000);           /* 10 ms */
        waited += 10;
    }
}

void con_die(const char *why) {
    con_print("\nEmbBoot FATAL: ");
    con_print(why);
    con_print("\n");
    for (;;) __asm__ volatile ("cli; hlt");
}
