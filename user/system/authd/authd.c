/* user/system/authd/authd.c -- the one thing in a session's reach that can
 * read /etc/shadow, and the least it could possibly do with it.
 *
 * Started by init, NOT by the desktop: it lives outside the session it serves,
 * so the session's namespace does not contain it and the session's teardown
 * does not decide when it stops -- init kills it when the desktop goes.
 *
 * ONE ACCOUNT, NAMED AT STARTUP. argv[1] is whose session this is, and it is
 * the only account this process will ever answer about. That is what keeps the
 * endpoint from being a password oracle: a program inside the session can ask
 * "is this the password for the person who is already logged in", which it
 * could find out anyway by watching them type it, and cannot ask anything
 * about the administrator's account or anyone else's.
 *
 * WRONG ANSWERS COST TIME, and the cost is not this file's invention:
 * embk_auth_throttle_ms() is the same escalation the greeter uses -- nothing
 * for the first two, then a second doubling to thirty. A service that answers
 * instantly forever is an offline attack with a keyboard.
 *
 * It has no window. Everything it says goes to the console, where init's own
 * lines go, because the only people who read it are looking at a machine that
 * is misbehaving. */
#include <stdio.h>
#include <string.h>

#include "embk.h"
#include "emsvc.h"
#include "emauth.h"
#include "auth.h"

static char g_user[EMBK_AUTH_USERNAME_MAX + 1];
static unsigned g_failures;

/* Is there an account to check against? The development auto-login opens a
 * session for a user the store has never heard of, and a lock screen on that
 * machine would be a door that cannot be opened. */
static int account_exists(void) {
    static char names[16][EMBK_AUTH_USERNAME_MAX + 1];
    int n = embk_auth_list(names, 16);
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], g_user) == 0) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        embk_puts(1, "authd: started with no account to serve; refusing\n");
        return 1;
    }
    snprintf(g_user, sizeof g_user, "%s", argv[1]);

    int lh = emsvc_listen("auth");
    if (lh < 0) {
        char b[96];
        snprintf(b, sizeof b, "authd: cannot listen at /run/emlink.auth (%d)\n", lh);
        embk_puts(1, b);
        return 1;
    }
    { char b[96];
      snprintf(b, sizeof b, "authd: serving /run/emlink.auth for '%s'%s\n", g_user,
               account_exists() ? "" : " (no account -- nothing to unlock with)");
      embk_puts(1, b); }

    for (;;) {
        uint32_t type = 0;
        struct emauth_req req;
        unsigned len = 0;
        int ch = emsvc_accept(lh, &type, &req, sizeof req, &len);
        if (ch < 0) { embk_sleep_ms(100); continue; }

        struct emauth_rep rep;
        memset(&rep, 0, sizeof rep);
        snprintf(rep.user, sizeof rep.user, "%s", g_user);
        rep.verdict = EMAUTH_DENIED;

        if (type != EMSVC_T_AUTH || len < sizeof req) {
            /* Answered anyway, with a refusal: a service that closes on a
             * malformed request leaves the caller unable to tell "you asked
             * wrongly" from "nothing is there". */
            emsvc_reply(ch, type, &rep, sizeof rep);
            continue;
        }

        req.password[sizeof req.password - 1] = 0;

        if (!account_exists()) {
            rep.verdict = EMAUTH_NOACCOUNT;
        } else if (req.op == EMAUTH_OP_STATUS) {
            /* "There is someone to check against" -- and deliberately NOT a
             * verdict on a password, so an empty password can never be an
             * accidental yes. */
            rep.verdict = EMAUTH_DENIED;
        } else if (embk_auth_verify(g_user, req.password)) {
            g_failures = 0;
            rep.verdict = EMAUTH_OK;
            embk_puts(1, "authd: unlocked\n");
        } else {
            /* THE WAIT HAPPENS BEFORE THE ANSWER, not after it: a caller that
             * hangs up on a slow reply has still spent the time, because it
             * cannot ask again until this one is served. */
            unsigned ms = embk_auth_throttle_ms(++g_failures);
            rep.wait_ms = ms;
            if (ms) embk_sleep_ms(ms);
            char b[96];
            snprintf(b, sizeof b, "authd: refused (%u in a row, %u ms)\n", g_failures, ms);
            embk_puts(1, b);
        }
        emsvc_reply(ch, EMSVC_T_AUTH, &rep, sizeof rep);
    }
}
