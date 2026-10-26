/* Where Terminal's profiles live: ~/.config/terminal/terminal.conf names the
 * default one, ~/.config/terminal/profiles/<name>.conf holds each. A preset
 * has no file until it is changed; deleting that file restores it. Writes
 * are atomic (a temporary file, fsync, rename), so another Terminal window
 * reading at the same moment sees the old file or the new one. */
#ifndef TERM_PROFILE_STORE_H
#define TERM_PROFILE_STORE_H

#include "profile.h"

#define PROFILE_LIST_MAX 32

struct profile_store { char dir[200], profiles[216], conf[224]; };   /* HOME is cut at 180 bytes */

/* The paths under `home`; the directories are made if missing. */
bool profile_store_init(struct profile_store *st, const char *home);

/* The default profile's name: terminal.conf's, or Clear Dark. */
void profile_store_default(const struct profile_store *st, char *out, size_t cap);
bool profile_store_set_default(const struct profile_store *st, const char *name);

/* Every profile: the presets, then the files that are not presets, by name. */
int profile_store_list(const struct profile_store *st, char names[][PROFILE_NAME_MAX], int max);

/* A profile by name: its file over its preset (or over the defaults); false
 * if neither exists. */
bool profile_store_load(const struct profile_store *st, const char *name, struct term_profile *p);
/* Written to its file, keeping what that file had that `p` does not know. */
bool profile_store_save(const struct profile_store *st, const struct term_profile *p);
/* The file removed: a preset is itself again, any other profile is gone. */
bool profile_store_remove(const struct profile_store *st, const char *name);
bool profile_store_has_file(const struct profile_store *st, const char *name);

/* A hash of terminal.conf and `name`'s file: it changes when either does,
 * whoever wrote it. */
uint64_t profile_store_signature(const struct profile_store *st, const char *name);

#endif
