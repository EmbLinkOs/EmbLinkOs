/* display_modes_test.c -- every mode an EDID offers: detailed timings, CTA
 * extension video codes and timings, established and standard timings. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "display/edid_modes.h"
#include "intel_fixtures.h"

static uint8_t e[384];

static void checksum(uint8_t *b)
{
    unsigned sum = 0;
    for (unsigned i = 0; i < 127; i++) sum += b[i];
    b[127] = (uint8_t)(256 - (sum & 0xff));
}

static int has(const struct display_mode *m, int n, unsigned w, unsigned h, unsigned hz)
{
    for (int i = 0; i < n; i++) {
        uint32_t r = display_mode_refresh_mhz(&m[i]);
        if (m[i].hdisplay == w && m[i].vdisplay == h && r + 600 > hz * 1000 && r < hz * 1000 + 600) return i;
    }
    return -1;
}

int main(void)
{
    struct display_mode m[32];
    make_edid(e, "DEL", 0xa0b1, 2560, 1440, 24150, "DELL U2717D");
    e[35] = 0x21;                      /* established: 640x480@60, 800x600@60 */
    e[36] = 0x08;                      /* 1024x768@60 */
    e[38] = (1280 / 8) - 31; e[39] = 0x80;               /* standard: 1280x1024@60 (5:4) */
    e[40] = (1680 / 8) - 31; e[41] = 0x00;               /* 1680x1050@60 (16:10) */
    e[42] = (1360 / 8) - 31; e[43] = 0xc0;               /* 1366x768 as EDID spells it */
    e[44] = 1; e[45] = 1;                                 /* unused */
    for (unsigned i = 46; i < 54; i += 2) { e[i] = 1; e[i + 1] = 1; }
    e[126] = 1;
    checksum(e);
    /* A CTA-861 extension: video data block (VIC 16 native, VIC 4, VIC 95, an
     * unknown VIC 200) and one detailed timing (1920x1080@50). */
    uint8_t *x = e + 128;
    memset(x, 0, 128);
    x[0] = 0x02; x[1] = 3;
    x[4] = (2u << 5) | 4; x[5] = 0x80 | 16; x[6] = 4; x[7] = 95; x[8] = 200;
    x[2] = 9;
    uint8_t *d = x + 9;
    put16(d, 14850); d[2] = 0x80; d[3] = 0xd0; d[4] = 0x72; d[5] = 0x38; d[6] = 0x2d; d[7] = 0x40;
    d[8] = 0x10; d[9] = 0x2c; d[10] = 0x45; d[11] = 0x80; d[17] = 0x1e;
    checksum(x);

    int n = edid_modes(e, 256, m, 32);
    assert(n > 0);
    int pref = has(m, n, 2560, 1440, 60);
    assert(pref == 0 && (m[0].flags & DISPLAY_MODE_PREFERRED) && m[0].clock_khz == 241500);
    assert(has(m, n, 640, 480, 60) >= 0 && has(m, n, 800, 600, 60) >= 0 && has(m, n, 1024, 768, 60) >= 0);
    assert(has(m, n, 1280, 1024, 60) >= 0 && has(m, n, 1680, 1050, 60) >= 0 && has(m, n, 1366, 768, 60) >= 0);
    int i1080 = has(m, n, 1920, 1080, 60);
    assert(i1080 >= 0 && m[i1080].vic == 16 && (m[i1080].flags & DISPLAY_MODE_PREFERRED) &&
           (m[i1080].flags & DISPLAY_MODE_HSYNC_POSITIVE));
    assert(has(m, n, 1280, 720, 60) >= 0 && has(m, n, 3840, 2160, 30) >= 0);
    int i50 = has(m, n, 1920, 1080, 50);
    assert(i50 >= 0 && m[i50].htotal == 2640 && m[i50].clock_khz == 148500);
    for (int i = 0; i < n; i++) assert(display_mode_sane(&m[i]));
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
        assert(!(m[i].hdisplay == m[j].hdisplay && m[i].vdisplay == m[j].vdisplay &&
                 display_mode_refresh_mhz(&m[i]) / 1000 == display_mode_refresh_mhz(&m[j]) / 1000));
    /* The list is cut at `max` without overrunning it. */
    assert(edid_modes(e, 256, m, 3) == 3);
    /* An extension with a bad checksum is skipped, the base block still read. */
    x[127] ^= 1;
    int base_only = edid_modes(e, 256, m, 32);
    assert(base_only > 0 && base_only < n && has(m, base_only, 1920, 1080, 50) < 0);
    x[127] ^= 1;
    /* A truncated buffer reads only the blocks it holds. */
    assert(edid_modes(e, 128, m, 32) == base_only);
    e[0] = 1;
    assert(edid_modes(e, 256, m, 32) == -1);
    e[0] = 0;
    e[10] ^= 1;                        /* base checksum broken */
    assert(edid_modes(e, 256, m, 32) == -1);
    /* An eDP panel's EDID: one detailed timing, and that is the panel. */
    make_edid(e, "AUO", 0x40ed, 1920, 1080, 14250, "B156HAN02.1");
    assert(edid_modes(e, 128, m, 32) == 1 && m[0].hdisplay == 1920 && (m[0].flags & DISPLAY_MODE_PREFERRED));
    const struct display_mode *k = edid_known_mode(1366, 768, 60);
    assert(k && k->clock_khz == 85500);
    /* HDMI or DVI: the HDMI vendor-specific block (OUI 00-0C-03) in a CTA
     * extension, and nothing else, makes an HDMI sink. */
    make_edid(e, "SAM", 0x0f00, 1920, 1080, 14850, "SAMSUNG");
    assert(!edid_hdmi(e, 128) && !edid_hdmi(e, 256));
    e[126] = 1; checksum(e);
    x = e + 128; memset(x, 0, 128);
    x[0] = 0x02; x[1] = 3; x[2] = 10;
    x[4] = (3u << 5) | 5; x[5] = 0x03; x[6] = 0x0c; x[7] = 0x00; x[8] = 0x10; x[9] = 0x00;
    checksum(x);
    assert(edid_hdmi(e, 256) == 1 && edid_hdmi(e, 128) == 0);
    x[6] = 0x0d; checksum(x);                          /* another vendor's block */
    assert(edid_hdmi(e, 256) == 0);
    x[6] = 0x0c; checksum(x); x[127] ^= 1;             /* a corrupt extension says nothing */
    assert(edid_hdmi(e, 256) == 0);
    puts("EDID modes: detailed timings, CTA video codes and DTDs, established and standard timings, "
         "preferred flags, de-duplication, bounds, bad checksums and truncation, HDMI vs DVI OK");
    return 0;
}
