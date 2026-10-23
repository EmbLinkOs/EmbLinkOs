/* setup.c -- first boot: create the machine's first account, which administers
 * it. init runs this when the account store is empty; it exits 0 once an
 * account exists, and init then shows the greeter. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth.h"
#include "embk.h"
#include "em.h"

static char username[EMBK_AUTH_USERNAME_MAX + 1];
static char password[EMBK_AUTH_PASSWORD_MAX + 1];
static char confirm[EMBK_AUTH_PASSWORD_MAX + 1];
static char status[128] = "Choose the account that will administer this machine.";

static void create_account(void) {
    if (!embk_auth_valid_username(username)) {
        snprintf(status, sizeof status, "Use lowercase letters, numbers, - or _.");
        return;
    }
    if (strlen(password) < EMBK_AUTH_PASSWORD_MIN) {
        snprintf(status, sizeof status, "The password needs at least %d characters.", EMBK_AUTH_PASSWORD_MIN);
        return;
    }
    if (strcmp(password, confirm) != 0) {
        snprintf(status, sizeof status, "The two passwords do not match.");
        memset(confirm, 0, sizeof confirm);
        return;
    }
    snprintf(status, sizeof status, "Creating the account...");
    int rc = embk_auth_create_as(username, password, 1);
    memset(password, 0, sizeof password);
    memset(confirm, 0, sizeof confirm);
    if (rc != 0) {
        snprintf(status, sizeof status, "Account creation failed (%d).", rc);
        return;
    }
    if (embk_auth_provision(username) != 0) {
        snprintf(status, sizeof status, "The account exists but its home could not be made.");
        return;
    }
    printf("setup: account '%s' created (administrator)\n", username);
    em_app_request_exit(0);
}

static void view(void) {
    float panel = em_viewport_width() * 0.38f;
    if (panel < 380) panel = 380;
    if (panel > 520) panel = 520;
    float side = em_viewport_width() * 0.07f;
    if (side < 32) side = 32;
    if (side > 120) side = 120;
    Screen(.padding = -1, .justify = Center, .align = Center) {
        BackgroundImage("/system/images/colibri-user.ppm");
        HStack(.width = em_viewport_width(), .height = em_viewport_height(),
               .padding = side, .align = Center) {
            Glass(.width = panel, .spacing = 14, .padding = 32, .align = Fill,
                  .background = { .r = 0.045f, .g = 0.055f, .b = 0.09f, .a = 0.92f },
                  .corner = 20, .border = 1, .shadow = 1) {
                Text("Welcome to EmbLink OS").title();
                Text("Create the first account").heading();
                Text(status).body().secondary();
                /* Opens ready to type; Tab walks the fields; Return in the
                 * last one creates the account. */
                em_field_autofocus();
                TextField(username, sizeof username, "username");
                PasswordField(password, sizeof password, "password");
                bool go = PasswordField(confirm, sizeof confirm, "confirm password").submitted();
                if (Button("Create account").primary().clicked() || go) create_account();
            }
            Spacer();
        }
    }
}

EM_APPLICATION {
    .title = "EmbLink OS Setup",
    .size = { 720, 520 },
    .theme = Dark,
    .chrome = Chromeless,
    .resize = FixedSize,
    .fullscreen = 1,
    .view = view,
};
