#ifndef _EMBBOOT_MENU_H_
#define _EMBBOOT_MENU_H_

/* Normal boot and read-only recovery/diagnostics share the embedded kernel. */
enum menu_entry {
    MENU_BOOT_EMBLINKOS = 0,
    MENU_RECOVERY,
    MENU_DIAGNOSTICS,
    MENU_LIVE_DESKTOP,
    MENU_BOOT_MANAGER,
    MENU_SECURE_BOOT,
    MENU_ENTRY_COUNT
};

/* Draw the menu and drive it: arrow keys / number keys to move, Enter to
 * select, and a countdown that auto-selects the default entry on timeout.
 * Returns the chosen menu_entry. */
int menu_run(void);

#endif /* _EMBBOOT_MENU_H_ */
