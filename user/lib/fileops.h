#ifndef EMBLINK_FILEOPS_H
#define EMBLINK_FILEOPS_H
#include <stddef.h>
/* POSIX convention: 0 on success, -1 with errno on failure. No overwrites. */
int embk_file_copy(const char *source,const char *destination);
int embk_file_move(const char *source,const char *destination);
int embk_file_trash(const char *source,const char *home,char *record,size_t capacity);
int embk_file_restore(const char *record,const char *home);
int embk_file_valid_name(const char *);
#endif
