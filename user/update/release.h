#ifndef EMBLINK_RELEASE_H
#define EMBLINK_RELEASE_H
#include <stddef.h>
#include <stdint.h>
#include "embk_update.h"
#define RELEASE_BYTES 448u
#define RELEASE_SIGNED_BYTES 384u
int update_release_parse(const uint8_t *,size_t,struct embk_update_image *,char notes[256]);
#endif
