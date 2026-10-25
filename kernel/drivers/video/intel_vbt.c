/* intel_vbt.c -- parse the VBT. See intel_vbt.h. Block numbers, struct
 * offsets and the BDB-version rules are Linux's i915 (intel_bios.c,
 * intel_vbt_defs.h); tools/intel_display_dump.py decodes the same fields
 * for a person to read. */
#include "drivers/video/intel_vbt.h"
#include "drivers/video/intel_regs.h"
#include "display/edid_modes.h"

static uint16_t u16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int match(const uint8_t *p, const char *s)
{
    for (unsigned i = 0; s[i]; i++) if (p[i] != (uint8_t)s[i]) return 0;
    return 1;
}

struct bdb {
    const uint8_t *base;      /* the BDB header: block 41's offsets count from here */
    uint32_t size;
    uint16_t version;
};

/* A block's payload and size, or NULL. */
static const uint8_t *block(const struct bdb *b, unsigned id, uint32_t *size)
{
    uint32_t off = u16le(b->base + 18);       /* header size */
    while (off + 3 <= b->size) {
        unsigned bid = b->base[off];
        uint32_t len = u16le(b->base + off + 1), start = off + 3;
        if (bid == 53 && start < b->size && b->base[start] >= 3 && start + 5 <= b->size)
            len = u32le(b->base + start + 1) + 5;      /* MIPI sequence v3+: 32-bit size */
        if (bid == 0 || len > b->size - start) return 0;
        if (bid == id) { *size = len; return b->base + start; }
        off = start + len;
    }
    return 0;
}

static unsigned child_size_for(unsigned version)
{
    if (version < 106) return 22;
    if (version < 111) return 27;
    if (version < 195) return 33;
    if (version == 195) return 37;
    if (version <= 215) return 38;
    if (version <= 255) return 39;
    return 40;
}

/* dvo_port codes (intel_vbt_defs.h DVO_PORT_*) to DDI index. */
static unsigned dvo_to_ddi(unsigned port)
{
    switch (port) {
    case 0: case 10: return 0;            /* HDMI-A, DP-A (eDP) */
    case 1: case 7:  return 1;            /* HDMI-B, DP-B */
    case 2: case 8:  return 2;
    case 3: case 9:  return 3;
    case 11: case 12: return 4;           /* DP-E, HDMI-E */
    case 13: case 14: return 5;           /* DP-F, HDMI-F */
    default: return 0xff;
    }
}

static unsigned aux_ch(unsigned code)
{
    switch (code) {
    case 0x40: return 0; case 0x10: return 1; case 0x20: return 2; case 0x30: return 3;
    default: return INTEL_NO_AUX;
    }
}

/* VBT DDC bus numbers to GMBUS pins (i915 cnp_ddc_pin_map; on Sunrise Point
 * the VBT gives GMBUS pins directly). */
static unsigned gmbus_pin(unsigned ddc, unsigned pch)
{
    if (pch == INTEL_PCH_CNP) {
        switch (ddc) {
        case 1: return 1;                 /* DDI B */
        case 2: return 2;                 /* DDI C */
        case 3: return 4;                 /* DDI D -- sic */
        case 4: return 3;                 /* DDI F -- sic */
        default: return 0;
        }
    }
    return ddc <= 7 ? ddc : 0;
}

static unsigned kind_of(uint16_t type, unsigned ddi)
{
    /* The DEVICE_TYPE_* bits (intel_vbt_defs.h). */
    unsigned dp = type & (1u << 2), tmds = type & (1u << 4), not_hdmi = type & (1u << 11),
             internal = type & (1u << 12);
    if (dp && internal) return INTEL_OUT_EDP;
    if (dp) return INTEL_OUT_DP;
    if (tmds) return not_hdmi ? INTEL_OUT_DVI : INTEL_OUT_HDMI;
    if (type && ddi == 0) return INTEL_OUT_EDP;
    return INTEL_OUT_NONE;
}

static void parse_children(const struct bdb *b, unsigned pch, struct intel_vbt *v)
{
    uint32_t len;
    const uint8_t *g = block(b, 2, &len);
    if (!g || len < 5) return;
    unsigned csize = g[4] ? g[4] : child_size_for(b->version);
    if (csize < 27) return;
    for (uint32_t off = 5; off + csize <= len && v->child_count < INTEL_VBT_MAX_CHILDREN; off += csize) {
        const uint8_t *c = g + off;
        uint16_t type = u16le(c + 2);
        if (!type) continue;
        struct intel_vbt_child *ch = &v->children[v->child_count];
        ch->device_type = type;
        ch->ddi = (uint8_t)dvo_to_ddi(c[16]);
        ch->kind = (uint8_t)kind_of(type, ch->ddi);
        if (ch->kind == INTEL_OUT_NONE || ch->ddi == 0xff) continue;
        ch->gmbus_pin = (uint8_t)gmbus_pin(c[19], pch);
        ch->aux_ch = INTEL_NO_AUX;
        ch->hdmi_level = 0xff;
        /* DP++ (i915 intel_bios_encoder_supports_dp_dual_mode): TMDS and DP
         * signalling, none of the other output kinds. */
        ch->dual_mode = (type & 0x163du) == 0x0014u;
        if (b->version >= 158 && csize > 25) {
            ch->lane_reversal = (c[23] >> 1) & 1;
            ch->iboost = (c[23] >> 3) & 1;
            ch->hpd_invert = (c[23] >> 4) & 1;
            ch->aux_ch = (uint8_t)aux_ch(c[25]);
        }
        if (b->version >= 158 && csize > 8)
            ch->hdmi_level = c[7] & 0x1fu;                  /* hdmi_level_shifter_value; 0 is a level */
        if (b->version >= 195 && csize > 33) {
            ch->usb_type_c = c[33] & 1;
            ch->thunderbolt = (c[33] >> 1) & 1;
        }
        if (b->version >= 196 && csize > 37) {
            ch->dp_iboost_level = c[37] & 0xf;
            ch->hdmi_iboost_level = c[37] >> 4;
        }
        if (b->version >= 216 && csize > 38) ch->dp_max_link_rate = c[38] & 7;
        /* An eDP child is always on AUX A unless the VBT says otherwise. */
        if (ch->aux_ch == INTEL_NO_AUX && (ch->kind == INTEL_OUT_EDP || ch->kind == INTEL_OUT_DP))
            ch->aux_ch = ch->ddi < 4 ? ch->ddi : INTEL_NO_AUX;
        v->child_count++;
    }
}

/* VBT block 42 holds a struct lvds_dvo_timing: an EDID detailed timing in
 * every byte but the last, whose flags mean something else -- bit 7 is
 * NON-interlaced and the sync polarities are bits 6 (H) and 5 (V). Read as an
 * EDID timing, every real panel would come out interlaced. */
static int parse_dvo_timing(const uint8_t *d, struct display_mode *m)
{
    if (edid_parse_dtd(d, m)) return -1;
    m->flags = 0;
    if (d[17] & 0x40) m->flags |= DISPLAY_MODE_HSYNC_POSITIVE;
    if (d[17] & 0x20) m->flags |= DISPLAY_MODE_VSYNC_POSITIVE;
    return 0;
}

static void parse_panel(const struct bdb *b, struct intel_vbt *v)
{
    uint32_t len;
    const uint8_t *o = block(b, 40, &len);
    v->panel_type = -1;
    if (o && len >= 1 && o[0] < 16) v->panel_type = o[0];
    if (v->panel_type < 0) return;
    unsigned p = (unsigned)v->panel_type;

    const uint8_t *e = block(b, 27, &len);
    if (e && len >= (p + 1) * 10) {
        const uint8_t *s = e + p * 10;
        v->pps.t1_t3 = u16le(s); v->pps.t8 = u16le(s + 2); v->pps.t9 = u16le(s + 4);
        v->pps.t10 = u16le(s + 6); v->pps.t11_t12 = u16le(s + 8);
        v->pps.valid = 1;
    }
    if (e && len >= 164) {
        static const uint8_t bpp[4] = { 18, 24, 30, 36 };
        v->edp_bpp = bpp[(u32le(e + 160) >> (p * 2)) & 3];
    }
    if (e && len >= 164 + 32) {
        const uint8_t *f = e + 164 + p * 2;
        static const uint8_t lanes[4] = { 1, 2, 0, 4 };
        v->edp_fast_link.rate_code = f[0] & 0xf;
        v->edp_fast_link.lanes = lanes[(f[0] >> 4) & 3];
        v->edp_fast_link.valid = v->edp_fast_link.lanes != 0 && v->edp_fast_link.rate_code <= 2;
    }
    if (e && b->version >= 173 && len >= 204 + 8) {
        unsigned sel = (unsigned)((u32le(e + 204 + (p / 8) * 4) >> ((p % 8) * 4)) & 0xf);
        v->edp_low_vswing = sel == 0;
    }

    const uint8_t *bl = block(b, 43, &len);
    if (bl && len >= 1) {
        unsigned esize = bl[0];
        if (esize >= 3 && len >= 1 + (p + 1) * esize) {
            const uint8_t *ent = bl + 1 + p * esize;
            v->backlight.pwm = (ent[0] & 3) == 2;
            v->backlight.active_low = (ent[0] >> 2) & 1;
            v->backlight.pwm_hz = u16le(ent + 1);
            v->backlight.min_brightness = esize >= 4 ? ent[3] : 0;
            v->backlight.valid = 1;
        }
    }

    const uint8_t *ptrs = block(b, 41, &len);
    if (ptrs && len >= 1 + (p + 1) * 9) {
        uint16_t off = u16le(ptrs + 1 + p * 9 + 3);
        uint8_t size = ptrs[1 + p * 9 + 5];
        if (size >= 18 && off + 18u <= b->size) {
            struct display_mode m;
            if (!parse_dvo_timing(b->base + off, &m) && display_mode_sane(&m)) v->lfp_mode = m;
        }
    }
}

int intel_vbt_parse(const uint8_t *vbt, uint32_t len, unsigned pch_layout, struct intel_vbt *v)
{
    for (unsigned i = 0; i < sizeof *v; i++) ((uint8_t *)v)[i] = 0;
    v->panel_type = -1;
    if (!vbt || len < 48 || !match(vbt, "$VBT")) return -1;
    uint32_t vbt_size = u16le(vbt + 24), bdb_off = u32le(vbt + 32);
    if (vbt_size > len || vbt_size < 48 || bdb_off + 22 > vbt_size) return -1;
    unsigned sum = 0;
    for (uint32_t i = 0; i < vbt_size; i++) sum += vbt[i];
    if (sum & 0xff) return -1;                /* a corrupt VBT is ignored, not half-believed */
    struct bdb b = { vbt + bdb_off, vbt_size - bdb_off, 0 };
    if (!match(b.base, "BIOS_DATA_BLOCK ")) return -1;
    b.version = u16le(b.base + 16);
    uint32_t bdb_size = u16le(b.base + 20);
    if (bdb_size < b.size) b.size = bdb_size;
    v->bdb_version = b.version;
    parse_children(&b, pch_layout, v);
    parse_panel(&b, v);
    v->valid = 1;
    return 0;
}

const struct intel_vbt_child *intel_vbt_child_for_ddi(const struct intel_vbt *v, unsigned ddi)
{
    for (unsigned i = 0; v && i < v->child_count; i++)
        if (v->children[i].ddi == ddi) return &v->children[i];
    return 0;
}

int intel_vbt_locate(const volatile uint8_t *op, uint64_t phys, uint32_t *offset,
                     uint64_t *rvda_phys, uint32_t *len)
{
    static const char sig[] = "IntelGraphicsMem";
    for (unsigned i = 0; i < 16; i++) if (op[i] != (uint8_t)sig[i]) return 0;
    uint32_t over = (uint32_t)op[0x14] | (uint32_t)op[0x15] << 8 | (uint32_t)op[0x16] << 16 | (uint32_t)op[0x17] << 24;
    uint32_t mbox = (uint32_t)op[0x58] | (uint32_t)op[0x59] << 8;
    unsigned major = over >> 24, minor = (over >> 16) & 0xffu;
    uint64_t rvda = 0;
    for (unsigned i = 0; i < 8; i++) rvda |= (uint64_t)op[0x3ba + i] << (8 * i);
    uint32_t rvds = (uint32_t)op[0x3c2] | (uint32_t)op[0x3c3] << 8 | (uint32_t)op[0x3c4] << 16 | (uint32_t)op[0x3c5] << 24;
    if (major >= 2 && (mbox & (1u << 2)) && rvda && rvds) {
        *rvda_phys = (major > 2 || minor >= 1) ? phys + rvda : rvda;
        *len = rvds;
        return 2;
    }
    if (op[0x400] == '$' && op[0x401] == 'V' && op[0x402] == 'B' && op[0x403] == 'T') {
        *offset = 0x400;
        uint32_t n = (uint32_t)op[0x400 + 24] | (uint32_t)op[0x400 + 25] << 8;
        *len = n <= 0x1800 ? n : 0x1800;
        return 1;
    }
    return 0;
}
