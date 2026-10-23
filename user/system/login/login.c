/* login.c -- the greeter: the only program on the machine that can reach the
 * account store (/etc), and so the only place accounts are managed.
 *
 * init runs it in the SYSTEM session with a narrowed capability set; on a
 * successful sign-in it writes the user's name to fd 3 and exits 0, and init
 * opens that user's session. Four pages:
 *
 *   sign in          name, password; Return opens the session
 *   change password  name, current password, new twice -- anyone, for themselves
 *   administrator    an administrator's name and password, to get to...
 *   accounts         create (optionally as an administrator), reset a
 *                    password, remove -- the last administrator excepted
 *
 * Wrong passwords are throttled across every page that takes one: free for
 * two, then 1 s doubling to 30 s (embk_auth_throttle_ms). Non-blocking -- the
 * screen stays live and says how long is left. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth.h"
#include "embk.h"
#include "em.h"

#define SESSION_FD 3

enum page { SIGN_IN, CHANGE, ADMIN_AUTH, ACCOUNTS };
static enum page g_page = SIGN_IN;

static char username[EMBK_AUTH_USERNAME_MAX + 1];
static char password[EMBK_AUTH_PASSWORD_MAX + 1];
static char newpw[EMBK_AUTH_PASSWORD_MAX + 1];
static char confirm[EMBK_AUTH_PASSWORD_MAX + 1];
static char target[EMBK_AUTH_USERNAME_MAX + 1];      /* the account being managed */
static bool make_admin;
static char status[128] = "Enter your account details.";

static unsigned g_failures;
static uint64_t g_locked_until;                      /* uptime ms */
static char     g_admin[EMBK_AUTH_USERNAME_MAX + 1]; /* who unlocked ACCOUNTS */

static void wipe(void) {
    memset(password, 0, sizeof password);
    memset(newpw, 0, sizeof newpw);
    memset(confirm, 0, sizeof confirm);
}

static void go(enum page p, const char *msg) {
    wipe();
    g_page = p;
    snprintf(status, sizeof status, "%s", msg);
}

/* True if a password may be tried now; otherwise says how long to wait. */
static bool may_try(void) {
    uint64_t now = embk_uptime_ms();
    if (now >= g_locked_until) return true;
    snprintf(status, sizeof status, "Too many attempts. Try again in %u s.",
             (unsigned)((g_locked_until - now + 999) / 1000));
    return false;
}

static void failed(const char *what, const char *who) {
    g_failures++;
    g_locked_until = embk_uptime_ms() + embk_auth_throttle_ms(g_failures);
    memset(password, 0, sizeof password);
    snprintf(status, sizeof status, "%s", what);
    /* The console keeps a record of refusals -- the name, never the password. */
    printf("login: %s refused for '%s' (%u in a row)\n",
           g_page == SIGN_IN ? "sign-in" : g_page == CHANGE ? "password change" : "administrator check",
           who, g_failures);
}

static void succeeded(void) { g_failures = 0; g_locked_until = 0; }

static void sign_in(void) {
    if (!may_try()) return;
    snprintf(status, sizeof status, "Checking...");
    if (!embk_auth_verify(username, password)) {
        failed("Incorrect username or password.", username);
        return;
    }
    succeeded();
    size_t len = strlen(username);
    if (embk_write(SESSION_FD, username, len) != (int64_t)len) {
        snprintf(status, sizeof status, "Could not start the session.");
        return;
    }
    wipe();
    em_app_request_exit(0);
}

static void change_password(void) {
    if (!may_try()) return;
    if (strcmp(newpw, confirm) != 0) { snprintf(status, sizeof status, "The new passwords do not match."); return; }
    int rc = embk_auth_change_password(username, password, newpw);
    if (rc == EMBK_AUTH_EDENIED || rc == EMBK_AUTH_ENOUSER || rc == EMBK_AUTH_EBADNAME) {
        failed("Incorrect username or current password.", username);
        return;
    }
    if (rc == EMBK_AUTH_EWEAK) { snprintf(status, sizeof status, "The new password needs at least %d characters.", EMBK_AUTH_PASSWORD_MIN); return; }
    if (rc != 0) { snprintf(status, sizeof status, "The password could not be changed (%d).", rc); return; }
    succeeded();
    printf("login: password changed for '%s'\n", username);
    go(SIGN_IN, "Password changed. Sign in with the new one.");
}

static void admin_auth(void) {
    if (!may_try()) return;
    if (!embk_auth_verify(username, password) || !embk_auth_is_admin(username)) {
        failed("That is not an administrator's name and password.", username);
        return;
    }
    succeeded();
    snprintf(g_admin, sizeof g_admin, "%s", username);
    target[0] = 0; make_admin = false;
    go(ACCOUNTS, "Manage the accounts on this machine.");
}

static void create_account(void) {
    if (strcmp(newpw, confirm) != 0) { snprintf(status, sizeof status, "The passwords do not match."); return; }
    int rc = embk_auth_create_as(target, newpw, make_admin);
    wipe();
    if (rc == EMBK_AUTH_EBADNAME) { snprintf(status, sizeof status, "Use lowercase letters, numbers, - or _."); return; }
    if (rc == EMBK_AUTH_EEXIST)   { snprintf(status, sizeof status, "'%s' already exists.", target); return; }
    if (rc == EMBK_AUTH_EWEAK)    { snprintf(status, sizeof status, "The password needs at least %d characters.", EMBK_AUTH_PASSWORD_MIN); return; }
    if (rc != 0 || embk_auth_provision(target) != 0) { snprintf(status, sizeof status, "The account could not be created (%d).", rc); return; }
    printf("login: '%s' created account '%s'%s\n", g_admin, target, make_admin ? " (administrator)" : "");
    snprintf(status, sizeof status, "Created '%s'.", target);
}

static void reset_password(void) {
    if (strcmp(newpw, confirm) != 0) { snprintf(status, sizeof status, "The passwords do not match."); return; }
    int rc = embk_auth_set_password(target, newpw);
    wipe();
    if (rc == EMBK_AUTH_ENOUSER || rc == EMBK_AUTH_EBADNAME) { snprintf(status, sizeof status, "There is no account '%s'.", target); return; }
    if (rc == EMBK_AUTH_EWEAK) { snprintf(status, sizeof status, "The password needs at least %d characters.", EMBK_AUTH_PASSWORD_MIN); return; }
    if (rc != 0) { snprintf(status, sizeof status, "The password could not be reset (%d).", rc); return; }
    printf("login: '%s' reset the password of '%s'\n", g_admin, target);
    snprintf(status, sizeof status, "New password set for '%s'.", target);
}

static void remove_account(void) {
    int rc = embk_auth_remove(target);
    if (rc == EMBK_AUTH_ELASTADMIN) { snprintf(status, sizeof status, "'%s' is the last administrator.", target); return; }
    if (rc == EMBK_AUTH_ENOUSER || rc == EMBK_AUTH_EBADNAME) { snprintf(status, sizeof status, "There is no account '%s'.", target); return; }
    if (rc != 0) { snprintf(status, sizeof status, "The account could not be removed (%d).", rc); return; }
    printf("login: '%s' removed account '%s'\n", g_admin, target);
    snprintf(status, sizeof status, "Removed '%s'. Its home folder is kept.", target);
    target[0] = 0;
}

static void accounts_list(void) {
    static char names[16][EMBK_AUTH_USERNAME_MAX + 1];
    static char line[16][48];
    int n = embk_auth_list(names, 16);
    for (int i = 0; i < n; i++) {
        snprintf(line[i], sizeof line[i], "%s%s", names[i], embk_auth_is_admin(names[i]) ? "  (administrator)" : "");
        Text(line[i]).body();
    }
}

static void form(void) {
    switch (g_page) {
    case SIGN_IN: {
        Text("Sign in").heading();
        Text(status).body().secondary();
        em_field_autofocus();
        TextField(username, sizeof username, "username");
        bool enter = PasswordField(password, sizeof password, "password").submitted();
        if (Button("Open session").primary().clicked() || enter) sign_in();
        if (Button("Change password").clicked()) go(CHANGE, "Change the password of your account.");
        if (Button("Manage accounts").clicked()) go(ADMIN_AUTH, "An administrator's name and password.");
        break;
    }
    case CHANGE: {
        Text("Change password").heading();
        Text(status).body().secondary();
        em_field_autofocus();
        TextField(username, sizeof username, "username");
        PasswordField(password, sizeof password, "current password");
        PasswordField(newpw, sizeof newpw, "new password");
        bool enter = PasswordField(confirm, sizeof confirm, "confirm new password").submitted();
        if (Button("Change password").primary().clicked() || enter) change_password();
        if (Button("Back").clicked()) go(SIGN_IN, "Enter your account details.");
        break;
    }
    case ADMIN_AUTH: {
        Text("Manage accounts").heading();
        Text(status).body().secondary();
        em_field_autofocus();
        TextField(username, sizeof username, "administrator");
        bool enter = PasswordField(password, sizeof password, "password").submitted();
        if (Button("Continue").primary().clicked() || enter) admin_auth();
        if (Button("Back").clicked()) go(SIGN_IN, "Enter your account details.");
        break;
    }
    case ACCOUNTS: {
        Text("Accounts").heading();
        accounts_list();
        Text(status).body().secondary();
        em_field_autofocus();
        TextField(target, sizeof target, "account name");
        PasswordField(newpw, sizeof newpw, "password");
        PasswordField(confirm, sizeof confirm, "confirm password");
        Checkbox("Administrator", &make_admin);
        HStack(.spacing = 8) {
            if (Button("Create").primary().clicked()) create_account();
            if (Button("Reset password").clicked()) reset_password();
            if (Button("Remove").clicked()) remove_account();
        }
        if (Button("Done").clicked()) { g_admin[0] = 0; go(SIGN_IN, "Enter your account details."); }
        break;
    }
    }
}

static void view(void) {
    float panel = em_viewport_width() * 0.34f;
    if (panel < 380) panel = 380;
    if (panel > 500) panel = 500;
    float side = em_viewport_width() * 0.07f;
    if (side < 32) side = 32;
    if (side > 120) side = 120;
    Screen(.padding = -1, .justify = Center, .align = Center) {
        BackgroundImage("/system/images/colibri-user.ppm");
        HStack(.width = em_viewport_width(), .height = em_viewport_height(),
               .padding = side, .align = Center) {
            Glass(.width = panel, .spacing = 12, .padding = 32, .align = Fill,
                  .background = { .r = 0.045f, .g = 0.055f, .b = 0.09f, .a = 0.92f },
                  .corner = 20, .border = 1, .shadow = 1) {
                Text("EmbLink OS").title();
                /* KEYED BY PAGE, so a page change is a new set of widgets and
                 * not the old ones relabelled: identity is positional otherwise,
                 * and the sign-in page's password field would hand its focus
                 * -- and its place -- to whatever field the next page has
                 * there. A new page opens with focus on its first field. */
                static const char *const page_key[] = { "signin", "change", "adminauth", "accounts" };
                VStack(.key = page_key[g_page], .spacing = 12, .align = Fill) {
                    form();
                }
            }
            Spacer();
        }
    }
}

EM_APPLICATION {
    .title = "EmbLink OS Login",
    .size = { 680, 520 },
    .theme = Dark,
    .chrome = Chromeless,
    .resize = FixedSize,
    .fullscreen = 1,
    .refresh_ms = 500,           /* the throttle countdown is live */
    .view = view,
};
