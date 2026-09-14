#ifndef _EMAUTH_H_
#define _EMAUTH_H_

/* user/lib/emauth.h -- "is this the person who logged in?", asked from inside
 * a session that cannot possibly answer it itself.
 *
 * THE PROBLEM THIS SOLVES. A screen lock has to check a password, and checking
 * a password means reading /etc/shadow. No session can: the namespace init
 * grants a desktop is `ro /system, ro /data/apps, rw /home/<user>, rw /run`,
 * and that is deliberate -- an account store a session can read is an account
 * store every program you run can read, and the whole point of hashing it is
 * that stealing the file should not be enough.
 *
 * So the check happens somewhere else. init -- which is outside every session
 * and is where the authority to see /etc already lives -- starts one authd per
 * session and tells it WHOSE session it is. authd will answer about that one
 * account and no other, which is the property that matters: a compromised
 * session cannot use this endpoint to test passwords against the
 * administrator's account, because authd will not accept the name.
 *
 * WHAT IT DOES NOT DO. It does not hand back a token, a capability, or
 * anything else that could be replayed: the answer is one boolean about one
 * password, and everything that follows from it is the caller's own decision.
 * A caller that lies to itself about the answer was already able to do
 * whatever it wanted -- it is inside the session. */

#include "emsvc.h"
#include "auth.h"

#define EMSVC_T_AUTH  0x03

/* What is being asked. STATUS is "could this session be locked at all" --
 * a machine whose only account was made by the development auto-login has no
 * password to unlock with, and a lock screen there is a locked door with no
 * key. Asking first is how the desktop avoids putting one up. */
#define EMAUTH_OP_STATUS  0u
#define EMAUTH_OP_VERIFY  1u

#define EMAUTH_OK         1     /* that is the password */
#define EMAUTH_DENIED     0     /* it is not */
#define EMAUTH_NOACCOUNT (-1)   /* this session's user has no account to check */

struct emauth_req {
    uint32_t op;                                  /* EMAUTH_OP_* */
    char     password[EMBK_AUTH_PASSWORD_MAX + 1];
};

struct emauth_rep {
    int32_t  verdict;                             /* EMAUTH_* */
    uint32_t wait_ms;                             /* how long the last refusal cost */
    char     user[EMBK_AUTH_USERNAME_MAX + 1];    /* whose session authd is serving */
};

/* Both calls BLOCK, and a verify blocks for as long as the service decides to
 * make a wrong password cost -- that delay is the point of it. Call from a
 * thread that is not drawing. */
static inline int emauth_call(uint32_t op, const char *password,
                              struct emauth_rep *out) {
    struct emauth_req req;
    unsigned i = 0;
    req.op = op;
    if (password)
        for (; password[i] && i < sizeof req.password - 1; i++)
            req.password[i] = password[i];
    for (unsigned k = i; k < sizeof req.password; k++) req.password[k] = 0;

    struct emauth_rep rep;
    for (unsigned k = 0; k < sizeof rep; k++) ((unsigned char *)&rep)[k] = 0;
    int n = emsvc_call("auth", EMSVC_T_AUTH, &req, sizeof req, &rep, sizeof rep);
    if (n < (int)sizeof rep) return n < 0 ? n : -EMBK_EINVAL;
    if (out) *out = rep;
    return rep.verdict;
}

static inline int emauth_status(struct emauth_rep *out) {
    return emauth_call(EMAUTH_OP_STATUS, "", out);
}
static inline int emauth_verify(const char *password, struct emauth_rep *out) {
    return emauth_call(EMAUTH_OP_VERIFY, password, out);
}

#endif /* _EMAUTH_H_ */
