/* user/tests/notifytest/notifytest.c -- post a banner and leave.
 *
 * The whole point of a notification is that it appears when the program that
 * sent it is not what you are looking at -- so the witness for one is a
 * program that says something and EXITS. If the banner is still on screen
 * after this process is gone, the service owns it, which is the claim.
 */
#include <stdio.h>
#include "embk.h"
#include "emnotify.h"

int main(int argc, char **argv) {
    const char *title = argc > 1 ? argv[1] : "Build finished";
    const char *body  = argc > 2 ? argv[2] : "notifytest posted this and exited.";
    int rc = embk_notify(title, body);
    char b[96];
    snprintf(b, sizeof b, "notifytest: embk_notify -> %d\n", rc);
    embk_puts(1, b);
    return rc == 0 ? 0 : 1;
}
