/* checkout_host -- gitclone's writing half on the host: a packfile unpacked
 * and written out as a repository (objects, refs, working tree, index), for
 * tools/gitclone_index_test.py to hold to git itself.
 *
 *   checkout_host PACK DIR BRANCH COMMIT */
#include "pack.h"
#include "repo.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: checkout_host PACK DIR BRANCH COMMIT\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *pack = malloc((size_t)n);
    if (!pack || fread(pack, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 1; }
    fclose(f);
    struct pack_obj *objs;
    int count = pack_unpack(pack, (size_t)n, &objs);
    if (count < 0) { fprintf(stderr, "checkout_host: bad pack (%d)\n", count); return 1; }
    if (repo_init(argv[2]) || repo_write_objects(argv[2], objs, count) != count ||
        repo_write_refs(argv[2], argv[3], argv[4]) || repo_checkout(argv[2], objs, count, argv[4]) < 0) {
        fprintf(stderr, "checkout_host: writing %s failed\n", argv[2]);
        return 1;
    }
    for (int i = 0; i < count; i++) free(objs[i].data);
    free(objs);
    free(pack);
    return 0;
}
