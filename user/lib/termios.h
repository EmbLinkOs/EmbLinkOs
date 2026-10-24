/* termios.h -- EmbLink override header.
 *
 * newlib SHIPS a termios.h, but it is a one-line `#include <sys/termios.h>`
 * pointing at a file that does not exist for this target -- broken the same
 * way its utime.h is (see user/lib/sys/utime.h). Same cure: override it.
 *
 * Private PTY sessions implement canonical/raw input and echo. The legacy
 * machine console still refuses termios. Unsupported flags and timed reads
 * fail explicitly; see docs/TERMINAL.md for the supported subset. */

#ifndef _EMBK_TERMIOS_H
#define _EMBK_TERMIOS_H

typedef unsigned char  cc_t;
typedef unsigned int   speed_t;
typedef unsigned int   tcflag_t;

#define NCCS 32

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];
};

/* c_lflag bits and c_cc indices callers actually name. */
#define ISIG   0x0001
#define ICANON 0x0002
#define ECHO   0x0008
#define ECHOE  0x0010
#define ECHOK  0x0020
#define ICRNL  0x0100
#define OPOST  0x0001
#define ONLCR  0x0004
#define CS8    0x0030
#define VINTR  0
#define VERASE 2
#define VKILL  3
#define VEOF   4
#define VMIN   6
#define VTIME  5

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#ifdef __cplusplus
extern "C" {
#endif

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int actions, const struct termios *t);
void cfmakeraw(struct termios *t);

#ifdef __cplusplus
}
#endif

#endif /* _EMBK_TERMIOS_H */
