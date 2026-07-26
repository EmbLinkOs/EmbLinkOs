#ifndef _EMBBOOT_MENU_H_
#define _EMBBOOT_MENU_H_

/* The EmbBoot menu entries. Only MENU_BOOT_EMBLINKOS is live in M1; the rest are
 * shown disabled ("(soon)") until their milestones land (see docs/EMBBOOT_Design.md). */
enum menu_entry {
    MENU_BOOT_EMBLINKOS = 0,
    MENU_RECOVERY,
    MENU_DIAGNOSTICS,
    MENU_FIRMWARE_UPDATE,
    MENU_BOOT_MANAGER,
    MENU_SECURE_BOOT,
    MENU_ENTRY_COUNT
};

/* Draw the menu and drive it: arrow keys / number keys to move, Enter to
 * select, and a countdown that auto-selects the default entry on timeout.
 * Returns the chosen menu_entry. */
int menu_run(void);

#endif /* _EMBBOOT_MENU_H_ */
