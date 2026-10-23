/* kernel/block/scsi.c -- see scsi.h. The commands, once. */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "block/scsi.h"

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

uint8_t scsi_cdb_read10(uint8_t *cdb, uint32_t lba, uint16_t blocks) {
    memset(cdb, 0, SCSI_CDB_MAX);
    cdb[0] = SCSI_READ10;
    be32(cdb + 2, lba);
    cdb[7] = (uint8_t)(blocks >> 8);
    cdb[8] = (uint8_t)blocks;
    return 10;
}
uint8_t scsi_cdb_write10(uint8_t *cdb, uint32_t lba, uint16_t blocks) {
    memset(cdb, 0, SCSI_CDB_MAX);
    cdb[0] = SCSI_WRITE10;
    be32(cdb + 2, lba);
    cdb[7] = (uint8_t)(blocks >> 8);
    cdb[8] = (uint8_t)blocks;
    return 10;
}
uint8_t scsi_cdb_inquiry(uint8_t *cdb, uint8_t alloc_len) {
    memset(cdb, 0, SCSI_CDB_MAX);
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = alloc_len;
    return 6;
}
uint8_t scsi_cdb_read_capacity10(uint8_t *cdb) {
    memset(cdb, 0, SCSI_CDB_MAX);
    cdb[0] = SCSI_READ_CAPACITY10;
    return 10;
}
uint8_t scsi_cdb_test_unit_ready(uint8_t *cdb) {
    memset(cdb, 0, SCSI_CDB_MAX);
    cdb[0] = SCSI_TEST_UNIT_READY;
    return 6;
}
uint8_t scsi_cdb_request_sense(uint8_t *cdb, uint8_t alloc_len) {
    memset(cdb, 0, SCSI_CDB_MAX);
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = alloc_len;
    return 6;
}

static void trim_copy(char *dst, const uint8_t *src, int n) {
    int i = n;
    while (i > 0 && (src[i - 1] == ' ' || src[i - 1] == 0)) i--;
    for (int k = 0; k < i; k++) dst[k] = (char)src[k];
    dst[i] = 0;
}

bool scsi_probe(struct scsi_device *sd) {
    if (!sd || !sd->exec) return false;
    uint8_t cdb[SCSI_CDB_MAX];
    static uint8_t buf[64];

    /* INQUIRY first, and not only for the name: byte 0's low five bits are
     * the PERIPHERAL DEVICE TYPE, which is how a disk is told from an optical
     * drive. Treating a CD as a disk means READ CAPACITY in 512-byte units on
     * a device whose blocks are 2048, and every read lands in the wrong
     * place. */
    memset(buf, 0, sizeof buf);
    if (!sd->exec(sd->ctx, cdb, scsi_cdb_inquiry(cdb, 36), buf, 36, false))
        return false;
    uint8_t devtype = buf[0] & 0x1F;
    sd->is_optical = (devtype == 0x05);
    sd->removable  = (buf[1] & 0x80) != 0;
    trim_copy(sd->vendor, buf + 8, 8);
    trim_copy(sd->product, buf + 16, 16);

    /* WAIT FOR IT TO BE READY, clearing sense in between. A device that has
     * just been powered or plugged answers NOT READY for a while and a driver
     * that takes the first answer as final reports an empty drive. */
    for (int tries = 0; tries < 10; tries++) {
        if (sd->exec(sd->ctx, cdb, scsi_cdb_test_unit_ready(cdb), NULL, 0, false))
            break;
        sd->exec(sd->ctx, cdb, scsi_cdb_request_sense(cdb, 18), buf, 18, false);
        for (volatile int d = 0; d < 200000; d++) { }
    }

    memset(buf, 0, sizeof buf);
    if (!sd->exec(sd->ctx, cdb, scsi_cdb_read_capacity10(cdb), buf, 8, false))
        return false;
    uint32_t max_lba = rd_be32(buf);
    sd->block_size = rd_be32(buf + 4);
    /* READ CAPACITY(10) returns the LAST addressable block, not the count.
     * Off by one here is a filesystem that cannot read its own last block. */
    sd->blocks = (uint64_t)max_lba + 1;
    if (sd->block_size == 0 || sd->block_size > 4096) return false;
    return true;
}

int scsi_read(struct scsi_device *sd, uint64_t lba, uint32_t count,
              void *buf, uint32_t max_blocks) {
    uint8_t cdb[SCSI_CDB_MAX];
    uint8_t *p = buf;
    while (count) {
        uint32_t n = count < max_blocks ? count : max_blocks;
        if (!sd->exec(sd->ctx, cdb, scsi_cdb_read10(cdb, (uint32_t)lba, (uint16_t)n),
                      p, n * sd->block_size, false))
            return -EMBK_EIO;
        p += n * sd->block_size;
        lba += n;
        count -= n;
    }
    return EMBK_OK;
}

int scsi_write(struct scsi_device *sd, uint64_t lba, uint32_t count,
               const void *buf, uint32_t max_blocks) {
    uint8_t cdb[SCSI_CDB_MAX];
    const uint8_t *p = buf;
    while (count) {
        uint32_t n = count < max_blocks ? count : max_blocks;
        if (!sd->exec(sd->ctx, cdb, scsi_cdb_write10(cdb, (uint32_t)lba, (uint16_t)n),
                      (void *)p, n * sd->block_size, true))
            return -EMBK_EIO;
        p += n * sd->block_size;
        lba += n;
        count -= n;
    }
    return EMBK_OK;
}
