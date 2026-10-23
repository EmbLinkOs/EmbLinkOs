/* kernel/block/scsi.h -- the command set four different buses all speak.
 *
 * SCSI is not a bus here; it is a LANGUAGE. The same command descriptor block
 * that a USB stick answers over Bulk-Only Transport is what virtio-scsi puts
 * in a request, what a SAS controller DMAs to a target, and what an optical
 * drive answers over ATAPI. Only the envelope differs.
 *
 * So the commands are built once, here, and each transport supplies one
 * function: "run this CDB, move this many bytes, tell me if it worked". That
 * is the whole seam. Before this file the command bytes were written out
 * inline in usb_core.c, which was correct and would have become four copies.
 */
#ifndef _EMBK_SCSI_H_
#define _EMBK_SCSI_H_

#include <stdint.h>
#include "include/types.h"

/* The opcodes this kernel issues. Named, because `cdb[0] = 0x2A` at a call
 * site is a number nobody can check. */
#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE   0x03
#define SCSI_INQUIRY         0x12
#define SCSI_START_STOP      0x1B
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2A
#define SCSI_SYNC_CACHE10    0x35
#define SCSI_READ_TOC        0x43   /* optical */
#define SCSI_GET_CONFIG      0x46   /* optical */
#define SCSI_READ12          0xA8

#define SCSI_CDB_MAX 16

/* How a transport runs one command. `cdb`/`cdb_len` is the command, `data`
 * the buffer (NULL when there is none), `len` its length, `to_dev` the
 * direction. Returns true when the target reported success.
 *
 * ONE FUNCTION IS THE ENTIRE PORTING SURFACE. A new controller implements
 * this and gets INQUIRY, capacity, read, write and the block-device glue
 * below for nothing. */
typedef bool (*scsi_exec_fn)(void *ctx, const uint8_t *cdb, uint8_t cdb_len,
                             void *data, uint32_t len, bool to_dev);

/* What a device said about itself. */
struct scsi_device {
    scsi_exec_fn exec;
    void    *ctx;
    uint64_t blocks;
    uint32_t block_size;
    char     vendor[9];
    char     product[17];
    bool     removable;
    bool     is_optical;        /* peripheral type 5: an ATAPI/optical drive */
};

/* Build the common commands. `cdb` must be SCSI_CDB_MAX bytes. Returns the
 * CDB length. */
uint8_t scsi_cdb_read10(uint8_t *cdb, uint32_t lba, uint16_t blocks);
uint8_t scsi_cdb_write10(uint8_t *cdb, uint32_t lba, uint16_t blocks);
uint8_t scsi_cdb_inquiry(uint8_t *cdb, uint8_t alloc_len);
uint8_t scsi_cdb_read_capacity10(uint8_t *cdb);
uint8_t scsi_cdb_test_unit_ready(uint8_t *cdb);
uint8_t scsi_cdb_request_sense(uint8_t *cdb, uint8_t alloc_len);

/* Bring a target up: INQUIRY, wait for it to be ready, READ CAPACITY. Fills
 * everything in `sd` except exec/ctx, which the caller sets first. */
bool scsi_probe(struct scsi_device *sd);

/* Read/write through the device's own exec, splitting at `max_blocks` per
 * command. Returns 0 or a negative EMBK_* code. */
int scsi_read(struct scsi_device *sd, uint64_t lba, uint32_t count,
              void *buf, uint32_t max_blocks);
int scsi_write(struct scsi_device *sd, uint64_t lba, uint32_t count,
               const void *buf, uint32_t max_blocks);

#endif
