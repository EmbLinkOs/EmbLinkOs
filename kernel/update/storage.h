#ifndef EMBLINK_UPDATE_STORAGE_H
#define EMBLINK_UPDATE_STORAGE_H
/* Boot-only mount composition. Returns zero for legacy and managed layouts,
 * negative if a managed data partition cannot be mounted safely. */
int update_storage_mount(void);
#endif
