/* authtest.c -- the account store and the session policy, against a scratch
 * store: every operation the greeter offers, the upgrade of an old hash, the
 * failure throttle, and the clamp init puts on a session profile.
 *
 * Exit code = the number of claims that did not hold. `test accounts` runs
 * it; the store is /data/authtest, rebuilt from nothing each run. */
#include <stdio.h>
#include <string.h>
#include "embk.h"
#include "auth.h"
#include "session_policy.h"

static int fails;
#define CHECK(cond, ...) do { int ok_ = (cond); printf("  [%s] ", ok_ ? "ok" : "FAIL"); \
    printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

#define DIR "/data/authtest"

static int file_has(const char *leaf, const char *needle) {
    char path[64]; snprintf(path, sizeof path, "%s/%s", DIR, leaf);
    char buf[4096]; int fd = (int)embk_open(path, EMBK_O_RDONLY, 0);
    if (fd < 0) return 0;
    int64_t n = embk_read(fd, buf, sizeof buf - 1); embk_close(fd);
    if (n < 0) return 0;
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}
static int exists(const char *leaf) {
    char path[64]; struct embk_stat st; snprintf(path, sizeof path, "%s/%s", DIR, leaf);
    return embk_stat(path, &st) >= 0;
}
static uint64_t now_ms(void) { return embk_uptime_ms(); }

int main(void) {
    (void)embk_mkdir(DIR);
    embk_unlink(DIR "/passwd"); embk_unlink(DIR "/shadow");
    embk_auth_set_dir(DIR);
    printf("authtest: a scratch account store in %s, work factor %u\n", DIR, embk_auth_iterations());

    CHECK(!embk_auth_has_accounts(), "an empty store has no accounts");

    uint64_t t0 = now_ms();
    int rc = embk_auth_create("alice", "correct horse");
    uint64_t t_create = now_ms() - t0;
    CHECK(rc == 0 && embk_auth_is_admin("alice"), "the first account is created and administers the machine (rc %d)", rc);
    rc = embk_auth_create("bob", "battery staple");
    CHECK(rc == 0 && !embk_auth_is_admin("bob"), "the second is an ordinary user (rc %d)", rc);
    CHECK(embk_auth_create("alice", "another one!") == EMBK_AUTH_EEXIST, "a name already taken is refused");
    CHECK(embk_auth_create("Bad Name", "long enough") == EMBK_AUTH_EBADNAME, "a name that is not [a-z0-9_-] is refused");
    CHECK(embk_auth_create("carol", "short") == EMBK_AUTH_EWEAK, "a password under 8 characters is refused");
    CHECK(file_has("passwd", "alice:x:1000:10:") && file_has("passwd", "bob:x:1001:100:"),
          "passwd: uids 1000/1001, gid 10 for the administrator, 100 for the user");

    t0 = now_ms();
    int good = embk_auth_verify("alice", "correct horse");
    uint64_t t_verify = now_ms() - t0;
    CHECK(good, "the right password opens alice's account");
    printf("  [info] one hash at %u iterations: %llu ms to create, %llu ms to verify\n",
           embk_auth_iterations(), (unsigned long long)t_create, (unsigned long long)t_verify);
    CHECK(!embk_auth_verify("alice", "correct horsE"), "a wrong password does not");
    CHECK(!embk_auth_verify("nobody", "correct horse"), "nor does an account that does not exist");

    CHECK(embk_auth_change_password("bob", "not his password", "new password 1") == EMBK_AUTH_EDENIED,
          "changing a password needs the old one");
    CHECK(embk_auth_change_password("bob", "battery staple", "new password 1") == 0 &&
          !embk_auth_verify("bob", "battery staple") && embk_auth_verify("bob", "new password 1"),
          "with it, the old password stops working and the new one works");
    CHECK(embk_auth_set_password("bob", "reset password 2") == 0 && embk_auth_verify("bob", "reset password 2"),
          "an administrator's reset sets a new password without the old one");

    CHECK(embk_auth_remove("alice") == EMBK_AUTH_ELASTADMIN, "the last administrator cannot be removed");
    CHECK(embk_auth_remove("bob") == 0 && !embk_auth_verify("bob", "reset password 2") &&
          !file_has("passwd", "bob:"), "removing bob: his password stops working and his entry is gone");
    char names[8][EMBK_AUTH_USERNAME_MAX + 1];
    int n = embk_auth_list(names, 8);
    CHECK(n == 1 && !strcmp(names[0], "alice"), "the store lists exactly who is left (%d)", n);

    /* THE UPGRADE, against a record this library did not write: standard
     * PBKDF2-HMAC-SHA256 at 4096 iterations, computed by Python's hashlib.
     * Accepting it proves the arithmetic is the standard one (and so matches
     * every hash the old code wrote); the rewrite proves the upgrade. */
    {
        int fd = (int)embk_open(DIR "/shadow", EMBK_O_WRONLY, 0);
        embk_lseek(fd, 0, EMBK_SEEK_END);
        const char *legacy = "legacy:$pbkdf2-sha256$4096$000102030405060708090a0b0c0d0e0f$"
                             "bedff5d5111abd726f950c7376c7fb354f42a4a264e2c21eba455858f42c2824:0:0:99999:7:::\n";
        embk_write(fd, legacy, strlen(legacy));
        embk_close(fd);
    }
    CHECK(embk_auth_verify("legacy", "legacy password"), "a standard PBKDF2 record from elsewhere (hashlib, 4096) verifies");
    char want[32]; snprintf(want, sizeof want, "legacy:$pbkdf2-sha256$%u$", embk_auth_iterations());
    CHECK(file_has("shadow", want) && !file_has("shadow", "$4096$"),
          "and the login rehashed it at today's work factor");
    CHECK(embk_auth_verify("legacy", "legacy password"), "the upgraded record still opens the account");

    CHECK(!exists("shadow.new") && !exists("passwd.new"), "every rewrite renamed into place (no .new left)");

    CHECK(embk_auth_throttle_ms(0) == 0 && embk_auth_throttle_ms(2) == 0 && embk_auth_throttle_ms(3) == 1000 &&
          embk_auth_throttle_ms(4) == 2000 && embk_auth_throttle_ms(8) == 30000 && embk_auth_throttle_ms(50) == 30000,
          "the throttle: free for two, then 1 s doubling to a 30 s cap");

    /* The clamp init puts on every session profile. */
    struct { const char *u; int adm; const char *p; int ro; int want; } G[] = {
        { "alice", 0, "/system", 1, 1 },            { "alice", 0, "/system", 0, 0 },
        { "alice", 0, "/data/apps", 1, 1 },         { "alice", 0, "/data/apps", 0, 0 },
        { "alice", 1, "/data/apps", 0, 1 },         { "alice", 0, "/run", 0, 1 },
        { "alice", 0, "/home/alice", 0, 1 },        { "alice", 0, "/home/alice/Documents", 0, 1 },
        { "al",    0, "/home/alice", 0, 0 },        { "alice", 0, "/home/bob", 0, 0 },
        { "alice", 0, "/home", 0, 0 },              { "alice", 0, "/etc", 1, 0 },
        { "alice", 1, "/etc", 1, 0 },               { "alice", 0, "/home/alice/../bob", 0, 0 },
        { "alice", 0, "/home//alice", 0, 0 },       { "alice", 0, "/home/alice/", 0, 0 },
        { "alice", 0, "/data", 0, 0 },              { "alice", 1, "/data", 0, 1 },
        { "alice", 0, "/", 1, 0 },                  { "alice", 0, "system", 1, 0 },
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof G / sizeof G[0]; i++) {
        int got = embk_session_grant_ok(G[i].u, G[i].adm, G[i].p, G[i].ro);
        if (got != G[i].want) {
            printf("    policy wrong: %s%s %s %s -> %d\n", G[i].u, G[i].adm ? "(admin)" : "",
                   G[i].ro ? "ro" : "rw", G[i].p, got);
            bad++;
        }
    }
    CHECK(bad == 0, "the session policy: %u grants judged, %d wrong", (unsigned)(sizeof G / sizeof G[0]), bad);

    embk_unlink(DIR "/passwd"); embk_unlink(DIR "/shadow");
    printf("authtest: %d failure(s)\n", fails);
    return fails;
}
