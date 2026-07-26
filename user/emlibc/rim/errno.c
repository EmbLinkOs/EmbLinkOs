/* errno.c — emlibc's errno cell and the kernel→emlibc code map.
 *
 * The kernel returns small negative -EMBK_E* values (Linux-style numbering).
 * emlibc's own <errno.h> adopts that numbering verbatim, so the map is
 * identity-valued today — but it is spelled out BY NAME so the seam is real:
 * the day the kernel or emlibc renumbers, only this switch changes, and every
 * caller keeps using emlibc's E* names. (docs/EMLIBC_Requirements.md §5.) */

#include <errno.h>
#include <stdint.h>

int errno = 0;

int embk_errno_from_kernel(int64_t ret)
{
    switch ((int)(-ret)) {
    case 1:   return EPERM;
    case 2:   return ENOENT;
    case 5:   return EIO;
    case 6:   return ENXIO;
    case 8:   return ENOEXEC;
    case 9:   return EBADF;
    case 10:  return ECHILD;
    case 11:  return EAGAIN;
    case 12:  return ENOMEM;
    case 13:  return EACCES;
    case 14:  return EFAULT;
    case 16:  return EBUSY;
    case 17:  return EEXIST;
    case 18:  return EXDEV;
    case 19:  return ENODEV;
    case 20:  return ENOTDIR;
    case 21:  return EISDIR;
    case 22:  return EINVAL;
    case 23:  return ENFILE;
    case 24:  return EMFILE;
    case 29:  return ESPIPE;
    case 30:  return EROFS;
    case 31:  return EMLINK;
    case 32:  return EPIPE;
    case 34:  return ERANGE;
    case 35:  return EDEADLK;
    case 36:  return ENAMETOOLONG;
    case 37:  return ENOLCK;
    case 38:  return ENOSYS;
    case 39:  return ENOTEMPTY;
    case 40:  return ELOOP;
    case 61:  return ENODATA;
    case 71:  return EPROTO;
    case 75:  return EOVERFLOW;
    case 84:  return EILSEQ;
    case 90:  return EMSGSIZE;
    case 95:  return ENOTSUP;
    case 110: return ETIMEDOUT;
    case 111: return ECONNREFUSED;
    case 125: return ECANCELED;
    default:  return EINVAL;   /* an unknown kernel code is a contract gap;
                                 * surface it as EINVAL, never as success */
    }
}
