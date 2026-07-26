/* emlibc_demo.c -- the house-rule proof for emlibc phase 1: a real program
 * that links emlibc INSTEAD of newlib (built -nostdinc, no -lc, no newlib
 * crt0/syscalls) and runs on EmbLinkOS.
 *
 * It exercises the three layers that make emlibc real: string.h (agnostic
 * bulk), stdlib.h (malloc/qsort/strtol/getenv), and stdio.h (the printf family
 * + a buffered file stream over the EmbLink rim). Everything it prints goes
 * through emlibc's OWN formatter and write() path.
 *
 * Ring-3 stdout lands on the screen, not the serial log, so the machine-checkable
 * witness is written to a file with emlibc's fopen/fprintf and read back by
 * `test emlibc`; the exit code (42) is the second, independent witness.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "emlibc.h"

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("emlibc: %s\n", EMLIBC_VERSION_STR);

    /* --- string.h --- */
    char buf[64];
    strcpy(buf, "EmbLink");
    strcat(buf, "OS");
    printf("string: len=%zu strcmp=%d strstr=%s memchr=%s\n",
           strlen(buf), strcmp(buf, "EmbLinkOS"),
           strstr(buf, "Link"), (char *)memchr(buf, 'O', 9));

    /* --- stdlib.h: malloc + qsort + strtol --- */
    int *v = malloc(5 * sizeof *v);
    v[0] = 42; v[1] = 7; v[2] = 99; v[3] = 1; v[4] = 23;
    qsort(v, 5, sizeof *v, cmp_int);
    printf("qsort: %d %d %d %d %d\n", v[0], v[1], v[2], v[3], v[4]);
    free(v);

    long n = strtol("0x2a", NULL, 0);      /* 42, base auto-detected */

    /* --- stdio.h: printf conversion coverage --- */
    printf("fmt: |%5d|%-5d|%05d|%+d|%#x|%c|%.3s|%p|\n",
           42, 42, 42, 42, 255, 'Q', "abcdef", (void *)buf);

    /* --- the environment crt0 published (HOME passed at spawn) --- */
    char *home = getenv("HOME");
    printf("env: HOME=%s\n", home ? home : "(unset)");

    /* --- a buffered FILE stream over the rim: the serial-checkable witness.
     * Needs the FILESYSTEM capability; best-effort so the demo still exits 42
     * either way (the exit code is the primary witness). --- */
    FILE *f = fopen("/data/tmp/emlibc.out", "w");
    if (f) {
        fprintf(f, "emlibc wrote %ld via its own stdio\n", n);
        fclose(f);
    }

    printf("emlibc demo OK, exit %ld\n", n);
    return (int)n;                          /* 42 */
}
