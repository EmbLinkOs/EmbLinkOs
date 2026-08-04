#ifndef EMBK_AUTH_H
#define EMBK_AUTH_H

#include <stddef.h>

#define EMBK_AUTH_USERNAME_MAX 31
#define EMBK_AUTH_PASSWORD_MAX 127
#define EMBK_AUTH_PASSWD "/etc/passwd"
#define EMBK_AUTH_SHADOW "/etc/shadow"

int embk_auth_valid_username(const char *username);
int embk_auth_has_accounts(void);
int embk_auth_create(const char *username, const char *password);
int embk_auth_verify(const char *username, const char *password);

#endif
