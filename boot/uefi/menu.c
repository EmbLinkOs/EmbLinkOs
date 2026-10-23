#include "menu.h"
#include "console.h"

/* Seconds to auto-boot the default entry if the user does nothing. The first
 * keypress cancels the countdown (standard boot-menu behaviour). */
#define MENU_TIMEOUT_S 5
#define MENU_DEFAULT   MENU_BOOT_EMBLINKOS

/* IMPORTANT: labels are stored INLINE (char[][N]), not as `const char *`
 * pointers. This EFI app runs with ZERO relocations (fully RIP-relative, no
 * dynamic relocator for data), so a table of pointers would hold link-time
 * (base-0) addresses at runtime and read as garbage. Inline char arrays are
 * addressed RIP-relative from the array base -- no stored pointer, no reloc.
 * Every future table in EmbBoot must follow this rule. */
#define LABEL_MAX 28
struct entry { char label[LABEL_MAX]; int enabled; };

/* M1: only "Boot EmbLinkOS" is live; the rest are shown so the whole tree is
 * visible, marked "(soon)" and inert until their milestones land. */
static const struct entry entries[MENU_ENTRY_COUNT] = {
    { "Boot EmbLinkOS",           1 },
    { "Recovery",                 0 },
    { "Diagnostics",              0 },
    { "Firmware Update",          0 },
    { "Boot Manager",             0 },
    { "Secure Boot Verification", 0 },
};

/* --- frame builder: assemble the whole menu into one buffer, then emit it in a
 * single con_print. One OutputString per frame = no flicker, and it stays well
 * under OVMF's per-call console limits (its GraphicsConsole faults after many
 * OutputString calls when there is no real display). --- */
static void ap(char *b, int *n, const char *s) { while (*s) b[(*n)++] = *s++; }

/* NO COLOUR, AND THAT IS A DECISION. This used to emit ANSI escapes, on the
 * reasoning that OutputString passes ESC straight through to a terminal -- and
 * under QEMU the terminal is a serial console that interprets them. A real
 * machine's console is the firmware's GRAPHICS console, which does not: the
 * first boot of the stick on a laptop-shaped machine drew "[0;36;40m" and
 * "[2J[H" across the menu. The firmware's own SetAttribute/ClearScreen are not
 * the fix either (console.c records why: OVMF's graphics console faults on
 * them when there is no display). Plain text is correct on both. */
static void ap_sgr(char *b, int *n, UINTN a) { (void)b; (void)n; (void)a; }

static void draw(int sel, int countdown) {
    char b[1024];
    int n = 0;

    ap(b, &n, "\n");
    ap_sgr(b, &n, EFI_CYAN);      ap(b, &n, "\n    EmbBoot\n");
    ap_sgr(b, &n, EFI_DARKGRAY);  ap(b, &n, "    -------\n\n");

    for (int i = 0; i < MENU_ENTRY_COUNT; i++) {
        if (i == sel) { ap_sgr(b, &n, EFI_WHITE | EFI_BG(EFI_CYAN)); ap(b, &n, "  > "); }
        else          { ap_sgr(b, &n, entries[i].enabled ? EFI_LIGHTGRAY : EFI_DARKGRAY); ap(b, &n, "    "); }
        b[n++] = ' '; b[n++] = (char)('1' + i); b[n++] = '.'; b[n++] = ' ';
        ap(b, &n, entries[i].label);
        if (!entries[i].enabled) { ap_sgr(b, &n, EFI_DARKGRAY); ap(b, &n, "  (soon)"); }
        ap_sgr(b, &n, EFI_LIGHTGRAY); ap(b, &n, "\n");
    }

    ap_sgr(b, &n, EFI_DARKGRAY);
    ap(b, &n, "\n");
    if (countdown >= 0) {
        ap(b, &n, "    Booting default in ");
        b[n++] = (char)('0' + countdown);
        ap(b, &n, "s -- press any key to stop.\n");
    }
    ap(b, &n, "    Up/Down or 1-6 to move, Enter to select.\n");
    ap_sgr(b, &n, EFI_LIGHTGRAY);
    b[n] = 0;

    con_print(b);
}

int menu_run(void) {
    int sel = MENU_DEFAULT;

    /* Draw ONCE, then wait up to the timeout for the first key. Deliberately no
     * per-second redraw: OVMF's GraphicsConsole faults after a handful of frames
     * when there is no display backing (headless -display none), so the
     * unattended auto-boot path must draw a single frame. The countdown is shown
     * statically ("press any key to stop"). Interactive redraws below happen
     * only once a key is pressed -- i.e. with a real console attached. */
    draw(sel, MENU_TIMEOUT_S);

    EFI_INPUT_KEY key;
    if (!con_read_key(&key, MENU_TIMEOUT_S * 1000))
        return MENU_DEFAULT;              /* timed out -> boot the default */

    for (;;) {
        if (key.ScanCode == SCAN_UP) {
            sel = (sel + MENU_ENTRY_COUNT - 1) % MENU_ENTRY_COUNT;
            draw(sel, -1);
        } else if (key.ScanCode == SCAN_DOWN) {
            sel = (sel + 1) % MENU_ENTRY_COUNT;
            draw(sel, -1);
        } else if (key.UnicodeChar >= '1' &&
                   key.UnicodeChar <= (CHAR16)('0' + MENU_ENTRY_COUNT)) {
            sel = key.UnicodeChar - '1';
            draw(sel, -1);
        } else if (key.UnicodeChar == CHAR_CR) {
            if (entries[sel].enabled)
                return sel;
            con_attr(EFI_YELLOW);
            con_print("\n    That entry isn't built yet.\n");
            con_attr(EFI_LIGHTGRAY);
            ST->BootServices->Stall(900000);   /* 0.9 s */
            draw(sel, -1);
        }
        con_read_key(&key, -1);           /* block for the next key */
    }
}
