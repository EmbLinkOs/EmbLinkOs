#ifndef EMBK_TERMINAL_H
#define EMBK_TERMINAL_H
#include <stdint.h>

/* Fixed-width native ABI. These are descriptors, never global terminal IDs.
 * Slave descriptors can be passed with SPAWN_ACTION_INHERIT_FD. */
#define EMBK_TERMINAL_OPEN 0
#define EMBK_TERMINAL_GET_ATTR 1
#define EMBK_TERMINAL_SET_ATTR 2
#define EMBK_TERMINAL_SET_ATTR_FLUSH 3
#define EMBK_TERMINAL_GET_SIZE 4
#define EMBK_TERMINAL_SET_SIZE 5
#define EMBK_TERMINAL_FOREGROUND 6
#define EMBK_TERMINAL_SELF (-2)
#define EMBK_TERMINAL_NONE (-1)

#define EMBK_TIF_ICRNL 0x100u
#define EMBK_TOF_OPOST 1u
#define EMBK_TOF_ONLCR 4u
#define EMBK_TCF_CS8 0x30u
#define EMBK_TLF_ISIG 1u
#define EMBK_TLF_ICANON 2u
#define EMBK_TLF_ECHO 8u
#define EMBK_TLF_ECHOE 16u
#define EMBK_TLF_ECHOK 32u
#define EMBK_TCC_INTR 0
#define EMBK_TCC_ERASE 2
#define EMBK_TCC_KILL 3
#define EMBK_TCC_EOF 4
#define EMBK_TCC_TIME 5
#define EMBK_TCC_MIN 6
#define EMBK_TCC_COUNT 8

struct embk_terminal_attr {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t cc[EMBK_TCC_COUNT];
    uint32_t reserved[2];
};
struct embk_terminal_size { uint16_t rows, cols, xpixel, ypixel; };
#endif
