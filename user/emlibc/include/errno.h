/* errno.h — emlibc's own error numbering (docs/EMLIBC_Requirements.md §5).
 *
 * The values here are emlibc's; the kernel returns its OWN -EMBK_E* codes and
 * errno.c maps them BY NAME (embk_errno_from_kernel). emlibc adopts the
 * kernel's Linux-style numbering verbatim, so today the map is identity-valued
 * — but the map is the seam: if the two ever diverge, only errno.c changes,
 * not every caller. A libc swap re-opens exactly this file, and nothing else.
 *
 * Only codes the EmbLink kernel can actually return are defined. There is no
 * EFAULT-you-can-catch fantasy, no networking errno a kernel without sockets
 * would never produce beyond the ones the socket layer really returns.
 */
#ifndef _EMLIBC_ERRNO_H
#define _EMLIBC_ERRNO_H

extern int errno;

#define EPERM            1  /* operation not permitted (capability denied)   */
#define ENOENT           2  /* no such file or directory                     */
#define EIO              5  /* I/O error                                     */
#define ENXIO            6  /* no such device or address                     */
#define ENOEXEC          8  /* exec format error (bad ELF/EMBX)              */
#define EBADF            9  /* bad file descriptor                           */
#define ECHILD          10  /* no child processes / handle                   */
#define EAGAIN          11  /* try again / would block                       */
#define ENOMEM          12  /* out of memory                                 */
#define EACCES          13  /* permission denied                             */
#define EFAULT          14  /* bad address                                   */
#define EBUSY           16  /* resource busy                                 */
#define EEXIST          17  /* file exists                                   */
#define EXDEV           18  /* cross-device link                             */
#define ENODEV          19  /* no such device                                */
#define ENOTDIR         20  /* not a directory                               */
#define EISDIR          21  /* is a directory                                */
#define EINVAL          22  /* invalid argument                              */
#define ENFILE          23  /* file table overflow                           */
#define EMFILE          24  /* too many open files                           */
#define ESPIPE          29  /* illegal seek (pipe)                           */
#define EROFS           30  /* read-only file system                         */
#define EMLINK          31  /* too many links                                */
#define EPIPE           32  /* broken pipe                                   */
#define ERANGE          34  /* result out of range                           */
#define EDEADLK         35  /* resource deadlock would occur                 */
#define ENAMETOOLONG    36  /* file name too long                            */
#define ENOLCK          37  /* no locks available                            */
#define ENOSYS          38  /* function not implemented / absent             */
#define ENOTEMPTY       39  /* directory not empty                           */
#define ELOOP           40  /* too many symbolic links                       */
#define ENODATA         61  /* no data available                             */
#define EPROTO          71  /* protocol error                                */
#define EOVERFLOW       75  /* value too large for defined data type         */
#define EILSEQ          84  /* illegal byte sequence                         */
#define EMSGSIZE        90  /* message too long                              */
#define ENOTSUP         95  /* not supported                                 */
#define ETIMEDOUT      110  /* operation timed out                           */
#define ECONNREFUSED   111  /* connection refused                            */
#define ECANCELED      125  /* operation canceled (^C / cancel handle)       */

#endif /* _EMLIBC_ERRNO_H */
