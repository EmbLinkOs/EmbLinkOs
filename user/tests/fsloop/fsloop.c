/* fsloop.c -- live inside the filesystem, so that a kill lands there.
 *
 * Create, write, fsync, close, unlink, forever. fsync is the point: a write
 * only reaches the page cache, but fsync commits a transaction, and the
 * process itself holds the filesystem's lock while it does. `test kill io`
 * kills this program at random moments and then asks whether the filesystem
 * still works -- which it did not, once: a thread killed while asleep inside
 * that lock took it to its grave, and every file operation in the system
 * waited on a dead owner.
 *
 *   run /data/apps/fsloop/fsloop.elf
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int main(void) {
    static char buf[4096];
    memset(buf, 'f', sizeof buf);
    for (unsigned long n = 0; ; n++) {
        int fd = open("/home/yves/fsloop.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { printf("fsloop: open failed\n"); return 1; }
        if (write(fd, buf, sizeof buf) != (long)sizeof buf) { printf("fsloop: write failed\n"); return 2; }
        if (fsync(fd) != 0) { printf("fsloop: fsync failed\n"); return 3; }
        close(fd);
        unlink("/home/yves/fsloop.bin");
    }
}
