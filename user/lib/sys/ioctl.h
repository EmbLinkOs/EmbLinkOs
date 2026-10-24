/* sys/ioctl.h -- EmbLink override header (newlib ships none).
 *
 * PTY dimensions are measured by Terminal and shared with child programs.
 * The legacy machine console has no winsize protocol and refuses queries. */

#ifndef _EMBK_SYS_IOCTL_H
#define _EMBK_SYS_IOCTL_H

#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif /* _EMBK_SYS_IOCTL_H */
