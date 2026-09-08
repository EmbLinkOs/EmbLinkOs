/* user/lib/appauth.h -- an application's DECLARED AUTHORITY.
 *
 * An app ships next to its binary a statement of what it needs, and the session
 * that launches it grants exactly that and no more. Two independent questions,
 * two sidecars, because they are genuinely different axes (docs/USERSPACE_v2.md:
 * permission = nameable AND capable):
 *
 *   <name>.ns     what the app may NAME   -- namespace prefixes and their mode
 *   <name>.caps   what the app may DO     -- the capability classes it needs
 *
 * Both only ever ATTENUATE. The kernel resolves a namespace prefix in the
 * GRANTOR's namespace and refuses a capability mask that is not a subset of the
 * grantor's own, so a manifest asking for more than the session holds fails the
 * spawn rather than widening anything. A manifest is a request, never a grant.
 *
 * Absent sidecar = full inherit, which is the pre-manifest behaviour and keeps
 * every undeclared app working exactly as before.
 *
 * This lives in user/lib rather than inside the desktop because reading an
 * app's declaration is not the desktop's business specifically -- any launcher
 * (home, the shell, a future session manager) asks the same question.
 */
#ifndef _EMBLINK_APPAUTH_H_
#define _EMBLINK_APPAUTH_H_

#include <stddef.h>
#include "embk.h"

/* Parse "<path minus .elf>.ns" into NS_BIND spawn actions.
 * Returns the number of actions written (0 = no manifest, or an empty one =>
 * the caller should grant nothing special and let the child inherit).
 * `desc`, if non-NULL, gets a short human summary like "ro /system, rw /run". */
int appauth_load_ns(const char *elf_path, struct embk_spawn_file_action *acts,
                    int max, char *desc, size_t desc_cap);

/* Parse "<path minus .elf>.caps" into a capability mask.
 * One capability name per line; '#' comments and blank lines ignored. The
 * recognised names are the EMBK_CAP_* classes, lowercased:
 *
 *     filesystem  network  gpu  audio  camera  usb  serial  rawdisk  kernel_ext
 *
 * The single word `none` declares an empty set, which is how an app says "I
 * need no capabilities at all" as opposed to "I did not say" -- the two are
 * very different and a file that could only express the latter would be a
 * worse manifest than no manifest. Requiring the WORD also means a .caps
 * containing only comments (someone documenting why an app is unrestricted)
 * reads as "no manifest" rather than silently stripping the app of everything.
 *
 * Returns 1 if a manifest was present (mask written to *out_mask, possibly 0),
 * 0 if there is none (caller inherits). `desc` gets "network, filesystem". */
int appauth_load_caps(const char *elf_path, unsigned *out_mask,
                      char *desc, size_t desc_cap);

/* The name of a capability id, for logging. "?" if unknown. */
const char *appauth_cap_name(int cap_id);

#endif /* _EMBLINK_APPAUTH_H_ */
