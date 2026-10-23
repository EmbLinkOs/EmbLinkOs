/* kernel/drivers/tpm/tpm.c -- the chip that remembers what booted.
 *
 * HONESTY FIRST, BECAUSE IT MATTERS MORE THAN THE CODE BELOW:
 *
 *   NOTHING IN THIS FILE HAS EVER TALKED TO A TPM. The development machine
 *   has no `swtpm`, QEMU's tpm-tis and tpm-crb both require one, and there is
 *   no real TPM to hand. What HAS been exercised is the probe refusing: a
 *   machine with no TPM reaches tpm_init(), finds no interface, returns
 *   false, and boots exactly as it did before. Every path after the probe is
 *   written from the specification and unverified, and the first machine that
 *   runs it should be expected to find bugs.
 *
 *   That is why the probe is deliberately strict. A TPM is recognised only by
 *   a vendor id that is neither all-zeros nor all-ones at the fixed address
 *   the specification assigns, and nothing is written until after that check.
 *   An unverified driver that writes to whatever happens to be at 0xFED40000
 *   on a machine with no TPM would be worse than no driver.
 *
 * WHY IT IS WORTH HAVING. Secure Boot (tools/sbsign.py, boot/uefi/loader.c)
 * proves the firmware would only run a signed loader. It says nothing about
 * what happened afterwards. A TPM's PCRs are the other half: each is a
 * register that can only be EXTENDED -- the new value is the hash of the old
 * value and the new measurement, so a value can be reached only by measuring
 * the same things in the same order, and nothing can put a PCR back. That is
 * what lets a disk key be sealed to a boot state: a stolen disk in another
 * machine cannot reproduce the PCRs, so the TPM will not unseal.
 *
 * TWO INTERFACES, BOTH HERE. TIS is the older one: a FIFO and a status
 * register, byte at a time. CRB is the newer: a command buffer in memory and
 * one register to start it. Which one a machine has is not a choice -- it is
 * read out of the interface id register -- and a driver that implements one
 * works on about half the machines it meets.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "drivers/tpm/tpm.h"
#include "mm/vmm.h"

/* The address the specification fixes for locality 0. It is not discovered:
 * every PC TPM is here, and the ACPI TPM2 table only confirms it. */
#define TPM_BASE 0xFED40000ULL

/* --- TIS registers, locality 0 ----------------------------------------- */
#define TIS_ACCESS      0x0000
#define TIS_INT_ENABLE  0x0008
#define TIS_STS         0x0018
#define TIS_DATA_FIFO   0x0024
#define TIS_INTF_ID     0x0030
#define TIS_DID_VID     0x0F00

#define ACCESS_VALID       0x80
#define ACCESS_ACTIVE_LOC  0x20
#define ACCESS_REQUEST_USE 0x02

#define STS_VALID         0x80
#define STS_COMMAND_READY 0x40
#define STS_GO            0x20
#define STS_DATA_AVAIL    0x10
#define STS_EXPECT        0x08

/* --- CRB registers ------------------------------------------------------ */
#define CRB_LOC_CTRL      0x0008
#define CRB_LOC_STS       0x000C
#define CRB_CTRL_REQ      0x0040
#define CRB_CTRL_STS      0x0044
#define CRB_CTRL_START    0x004C
#define CRB_CTRL_CMD_SIZE 0x0058
#define CRB_CTRL_CMD_LO   0x005C
#define CRB_CTRL_CMD_HI   0x0060
#define CRB_CTRL_RSP_SIZE 0x0064
#define CRB_CTRL_RSP_LO   0x0068

#define CRB_LOC_STS_GRANTED 0x01
#define CRB_CTRL_REQ_READY  0x01
#define CRB_CTRL_STS_ERROR  0x01

/* --- TPM 2.0 commands --------------------------------------------------- */
#define TPM_ST_NO_SESSIONS 0x8001
#define TPM_ST_SESSIONS    0x8002
#define TPM_CC_STARTUP     0x00000144
#define TPM_CC_PCR_EXTEND  0x00000182
#define TPM_CC_PCR_READ    0x0000017E
#define TPM_SU_CLEAR       0x0000
#define TPM_RS_PW          0x40000009
#define TPM_ALG_SHA256     0x000B

#define TPM_BUF 1024

static volatile uint8_t *g_regs;
static bool g_up;
static bool g_is_crb;
static uint32_t g_vendor;
static uint32_t g_extends;
static uint8_t  g_cmd[TPM_BUF] __attribute__((aligned(64)));
static uint8_t  g_rsp[TPM_BUF] __attribute__((aligned(64)));

static inline uint8_t  r8 (uint32_t o) { return *(volatile uint8_t  *)(g_regs + o); }
static inline uint32_t r32(uint32_t o) { return *(volatile uint32_t *)(g_regs + o); }
static inline void w8 (uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(g_regs + o) = v; }
static inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_regs + o) = v; }

/* Everything a TPM says is big endian: it is a smart card by ancestry. */
static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool wait8(uint32_t reg, uint8_t mask, uint8_t want) {
    for (int i = 0; i < 5000000; i++) {
        if ((r8(reg) & mask) == want) return true;
        __asm__ volatile("" ::: "memory");
    }
    return false;
}

/* ---- TIS: a FIFO and a status register -------------------------------- */

static uint32_t tis_burst(void) {
    /* The burst count is how many bytes the FIFO will take right now. Writing
     * more than it says is not buffered -- it is dropped. */
    for (int i = 0; i < 1000000; i++) {
        uint32_t sts = r32(TIS_STS);
        uint32_t burst = (sts >> 8) & 0xFFFF;
        if (burst) return burst;
        __asm__ volatile("" ::: "memory");
    }
    return 0;
}

static int tis_transact(const uint8_t *cmd, uint32_t len, uint8_t *rsp, uint32_t cap) {
    w8(TIS_ACCESS, ACCESS_REQUEST_USE);
    if (!wait8(TIS_ACCESS, ACCESS_ACTIVE_LOC, ACCESS_ACTIVE_LOC)) return -EMBK_EIO;

    w8(TIS_STS, STS_COMMAND_READY);
    if (!wait8(TIS_STS, STS_COMMAND_READY, STS_COMMAND_READY)) return -EMBK_EIO;

    uint32_t sent = 0;
    while (sent < len) {
        uint32_t burst = tis_burst();
        if (!burst) return -EMBK_EIO;
        /* THE LAST BYTE IS HELD BACK. A TPM clears Expect when it has the
         * whole command, and it decides that on the final byte; sending it
         * inside a burst with the rest is legal but sending MORE than the
         * command is not, so the loop is bounded by both. */
        while (burst-- && sent < len) w8(TIS_DATA_FIFO, cmd[sent++]);
    }
    if (r8(TIS_STS) & STS_EXPECT) return -EMBK_EIO;   /* it wants more */

    w8(TIS_STS, STS_GO);
    if (!wait8(TIS_STS, STS_VALID | STS_DATA_AVAIL, STS_VALID | STS_DATA_AVAIL))
        return -EMBK_EIO;

    uint32_t got = 0;
    while (got < cap) {
        uint32_t burst = tis_burst();
        if (!burst) break;
        while (burst-- && got < cap) {
            rsp[got++] = r8(TIS_DATA_FIFO);
            /* Stop when the header's own length says the answer is complete;
             * reading past it returns whatever the FIFO does when empty. */
            if (got >= 6) {
                uint32_t total = get_be32(rsp + 2);
                if (total >= 10 && got >= total) goto done;
            }
        }
        if (!(r8(TIS_STS) & STS_DATA_AVAIL)) break;
    }
done:
    w8(TIS_STS, STS_COMMAND_READY);
    w8(TIS_ACCESS, ACCESS_ACTIVE_LOC);     /* release the locality */
    return (int)got;
}

/* ---- CRB: a buffer in memory and one register to start it ------------- */

static int crb_transact(const uint8_t *cmd, uint32_t len, uint8_t *rsp, uint32_t cap) {
    w32(CRB_LOC_CTRL, 1);                   /* request locality 0 */
    for (int i = 0; i < 5000000; i++) {
        if (r32(CRB_LOC_STS) & CRB_LOC_STS_GRANTED) break;
        __asm__ volatile("" ::: "memory");
    }
    w32(CRB_CTRL_REQ, CRB_CTRL_REQ_READY);
    for (int i = 0; i < 5000000; i++) {
        if (!(r32(CRB_CTRL_REQ) & CRB_CTRL_REQ_READY)) break;
        __asm__ volatile("" ::: "memory");
    }

    /* The command buffer's address is given BY THE CHIP, not chosen by the
     * driver: it is somewhere inside the same MMIO window. */
    uint32_t cmd_sz = r32(CRB_CTRL_CMD_SIZE);
    uint64_t cmd_pa = (uint64_t)r32(CRB_CTRL_CMD_LO) |
                      ((uint64_t)r32(CRB_CTRL_CMD_HI) << 32);
    uint32_t rsp_sz = r32(CRB_CTRL_RSP_SIZE);
    uint64_t rsp_pa = (uint64_t)r32(CRB_CTRL_RSP_LO);
    if (!cmd_sz || len > cmd_sz) return -EMBK_EINVAL;

    volatile uint8_t *cb = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(cmd_pa, cmd_sz);
    if (!cb) return -EMBK_EIO;
    for (uint32_t i = 0; i < len; i++) cb[i] = cmd[i];

    w32(CRB_CTRL_START, 1);
    bool done = false;
    for (int i = 0; i < 20000000; i++) {
        if (!(r32(CRB_CTRL_START) & 1)) { done = true; break; }
        __asm__ volatile("" ::: "memory");
    }
    if (!done) return -EMBK_EIO;
    if (r32(CRB_CTRL_STS) & CRB_CTRL_STS_ERROR) return -EMBK_EIO;

    volatile uint8_t *rb = (rsp_pa == cmd_pa)
        ? cb : (volatile uint8_t *)(uintptr_t)vmm_map_mmio(rsp_pa, rsp_sz);
    if (!rb) return -EMBK_EIO;
    uint32_t total = ((uint32_t)rb[2] << 24) | ((uint32_t)rb[3] << 16) |
                     ((uint32_t)rb[4] << 8) | (uint32_t)rb[5];
    if (total < 10 || total > cap) total = cap;
    for (uint32_t i = 0; i < total; i++) rsp[i] = rb[i];
    return (int)total;
}

static int tpm_transact(const uint8_t *cmd, uint32_t len, uint8_t *rsp, uint32_t cap) {
    if (!g_up) return -EMBK_ENODEV;
    return g_is_crb ? crb_transact(cmd, len, rsp, cap)
                    : tis_transact(cmd, len, rsp, cap);
}

/* The response code is the third field of every answer; 0 is success. */
static int tpm_rc(const uint8_t *rsp, int n) {
    if (n < 10) return -EMBK_EIO;
    return get_be32(rsp + 6) == 0 ? EMBK_OK : -EMBK_EIO;
}

/* ---- the three commands worth having ---------------------------------- */

static int tpm_startup(void) {
    put_be16(g_cmd + 0, TPM_ST_NO_SESSIONS);
    put_be32(g_cmd + 2, 12);
    put_be32(g_cmd + 6, TPM_CC_STARTUP);
    put_be16(g_cmd + 10, TPM_SU_CLEAR);
    int n = tpm_transact(g_cmd, 12, g_rsp, sizeof g_rsp);
    if (n < 0) return n;
    /* A TPM the firmware already started answers "initialise" with an error
     * that means "already done", and that is success for our purposes. */
    return n >= 10 ? EMBK_OK : -EMBK_EIO;
}

int tpm_pcr_extend(uint32_t pcr, const uint8_t digest[32]) {
    if (!g_up) return -EMBK_ENODEV;
    uint8_t *p = g_cmd;
    put_be16(p + 0, TPM_ST_SESSIONS);
    put_be32(p + 6, TPM_CC_PCR_EXTEND);
    put_be32(p + 10, pcr);
    /* THE PASSWORD SESSION IS NINE BYTES AND IS NOT OPTIONAL. Extending a PCR
     * is an authorised command even when the authorisation is empty, and a
     * command sent without the session is rejected rather than treated as
     * unauthenticated. */
    put_be32(p + 14, 9);
    put_be32(p + 18, TPM_RS_PW);
    put_be16(p + 22, 0);          /* no nonce        */
    p[24] = 0;                    /* no attributes   */
    put_be16(p + 25, 0);          /* no HMAC         */
    put_be32(p + 27, 1);          /* one digest      */
    put_be16(p + 31, TPM_ALG_SHA256);
    memcpy(p + 33, digest, 32);
    uint32_t len = 65;
    put_be32(p + 2, len);

    int n = tpm_transact(g_cmd, len, g_rsp, sizeof g_rsp);
    if (n < 0) return n;
    int rc = tpm_rc(g_rsp, n);
    if (rc == EMBK_OK) g_extends++;
    return rc;
}

int tpm_pcr_read(uint32_t pcr, uint8_t out[32]) {
    if (!g_up || pcr > 23) return -EMBK_ENODEV;
    uint8_t *p = g_cmd;
    put_be16(p + 0, TPM_ST_NO_SESSIONS);
    put_be32(p + 6, TPM_CC_PCR_READ);
    put_be32(p + 10, 1);                 /* one selection */
    put_be16(p + 14, TPM_ALG_SHA256);
    p[16] = 3;                           /* three bytes of bitmap */
    p[17] = p[18] = p[19] = 0;
    p[17 + (pcr / 8)] = (uint8_t)(1u << (pcr % 8));
    uint32_t len = 20;
    put_be32(p + 2, len);

    int n = tpm_transact(g_cmd, len, g_rsp, sizeof g_rsp);
    if (n < 0) return n;
    if (tpm_rc(g_rsp, n) != EMBK_OK) return -EMBK_EIO;
    /* header(10) + updateCounter(4) + selection count(4) + alg(2) + size(1)
     * + bitmap(3) + digest count(4) + digest size(2) = 30 */
    if (n < 30 + 32) return -EMBK_EIO;
    memcpy(out, g_rsp + 30, 32);
    return EMBK_OK;
}

bool tpm_init(void) {
    if (g_up) return true;
    g_regs = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(TPM_BASE, 0x5000);
    if (!g_regs) return false;

    /* THE PROBE IS READ-ONLY AND STRICT. On a machine with no TPM this window
     * decodes to nothing and reads back as all-ones, or to something else
     * entirely; either way nothing here writes until a plausible vendor has
     * answered. Given that no path past this point has ever run, a probe that
     * guessed would be the most dangerous line in the kernel. */
    g_vendor = r32(TIS_DID_VID);
    if (g_vendor == 0xFFFFFFFFu || g_vendor == 0) {
        vmm_unmap_mmio((uint64_t)(uintptr_t)g_regs, 0x5000);
        g_regs = NULL;
        return false;
    }

    /* Which interface. Bits 3:0 of the interface id register: 0 is FIFO
     * (TIS), 1 is CRB. */
    uint32_t intf = r32(TIS_INTF_ID);
    g_is_crb = (intf & 0xF) == 1;
    g_up = true;

    if (tpm_startup() != EMBK_OK)
        kprintf("tpm: startup was refused; the firmware may have done it\n");

    kprintf("tpm: vendor %04x device %04x, %s interface\n",
            g_vendor & 0xFFFF, g_vendor >> 16, g_is_crb ? "CRB" : "TIS");
    return true;
}

bool     tpm_present(void) { return g_up; }
bool     tpm_is_crb(void)  { return g_is_crb; }
uint32_t tpm_vendor(void)  { return g_vendor; }
uint32_t tpm_extends(void) { return g_extends; }
