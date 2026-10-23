#ifndef EMBK_AUTH_H
#define EMBK_AUTH_H

#include <stddef.h>
#include <stdint.h>

/* The account store: who may open a session on this machine.
 *
 *   /etc/passwd   name:x:uid:gid:name:/home/name:/system/bin/shell.elf
 *                 gid 10 is an ADMINISTRATOR (may manage accounts at the
 *                 greeter), gid 100 an ordinary user. Kept in the classic
 *                 layout because ported software (git, anything with getpwnam)
 *                 reads it; the kernel does not -- identity there is a session.
 *   /etc/shadow   name:$pbkdf2-sha256$<iterations>$<salt>$<hash>:...
 *
 * Only the greeter (login.elf) and first-boot setup can reach /etc: no
 * user's session is given it (init's session policy). Every change rewrites
 * the file whole and renames it into place, which the filesystem does in one
 * commit -- a crash leaves the old file or the new one, never half of each. */

#define EMBK_AUTH_USERNAME_MAX 31
#define EMBK_AUTH_PASSWORD_MAX 127
#define EMBK_AUTH_PASSWORD_MIN 8
#define EMBK_AUTH_GID_ADMIN    10
#define EMBK_AUTH_GID_USER     100

/* Errors (negative). */
#define EMBK_AUTH_EBADNAME   (-1)   /* not [a-z0-9_-], 1..31 */
#define EMBK_AUTH_EEXIST     (-2)   /* that account already exists */
#define EMBK_AUTH_EENTROPY   (-3)   /* no randomness for a salt */
#define EMBK_AUTH_EIO        (-4)   /* the store could not be read or written */
#define EMBK_AUTH_EWEAK      (-5)   /* password shorter than EMBK_AUTH_PASSWORD_MIN */
#define EMBK_AUTH_ENOUSER    (-6)   /* no such account */
#define EMBK_AUTH_EDENIED    (-7)   /* wrong password */
#define EMBK_AUTH_ELASTADMIN (-8)   /* would leave the machine with no administrator */

/* Where the store lives. "/etc" unless a test points it at a scratch dir. */
void embk_auth_set_dir(const char *dir);

int embk_auth_valid_username(const char *username);
int embk_auth_has_accounts(void);

/* Create an account. The FIRST account on a machine is its administrator. */
int embk_auth_create(const char *username, const char *password);

/* The same, saying whether the new account administers the machine (the
 * first one always does). For the greeter's account manager. */
int embk_auth_create_as(const char *username, const char *password, int admin);

/* Give an account a place to live: /home/<user> and its standard folders,
 * and its SESSION PROFILE -- <store>/sessions/<user>.ns, the namespace init
 * grants the user's desktop (clamped by user/lib/session_policy.h). In the
 * store's directory, not the user's home: a profile the user can edit is a
 * profile the user can widen. 0, or EMBK_AUTH_EIO. */
int embk_auth_provision(const char *username);

/* 1 if `password` is right for `username`, else 0. A right password whose
 * stored hash is older and weaker than today's is rehashed on the spot, so a
 * store upgrades itself one login at a time. */
int embk_auth_verify(const char *username, const char *password);

/* Change your password: the old one is required. */
int embk_auth_change_password(const char *username, const char *old_pw, const char *new_pw);

/* Set a password WITHOUT the old one -- an administrator's reset. The CALLER
 * decides whether it may (the greeter asks for an administrator's password
 * first); this library only does it. */
int embk_auth_set_password(const char *username, const char *new_pw);

/* Remove an account (its home directory is left; that is data, not access).
 * Refused if it is the last administrator. */
int embk_auth_remove(const char *username);

int embk_auth_is_admin(const char *username);

/* Up to `max` account names, in store order. Returns the count. */
int embk_auth_list(char names[][EMBK_AUTH_USERNAME_MAX + 1], int max);

/* The work factor new hashes get (PBKDF2-HMAC-SHA256 iterations), and a way
 * to set it -- a machine that wants more, or a test that needs an old, weak
 * record to prove the upgrade. Never below 1000. */
uint32_t embk_auth_iterations(void);
void     embk_auth_set_iterations(uint32_t iterations);

/* How long to make someone wait after `failures` wrong passwords in a row:
 * nothing for the first two, then 1 s doubling to a 30 s cap. A greeter that
 * answers instantly forever is an offline attack with a keyboard. */
unsigned embk_auth_throttle_ms(unsigned failures);

#endif
