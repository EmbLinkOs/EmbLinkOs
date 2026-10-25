#!/usr/bin/env python3
"""intel_display_dump.py -- decode /EMBKDISP.BIN, the kernel's read-only record
of an Intel laptop's display (kernel/drivers/video/intel_capture.h).

    python3 tools/intel_display_dump.py /Volumes/EMBLINK/EMBKDISP.BIN
    python3 tools/intel_display_dump.py EMBKDISP.BIN --json           # machine-readable
    python3 tools/intel_display_dump.py EMBKDISP.BIN --vbt-out vbt.bin

Register meanings are Gen9 display (Skylake to Comet Lake), from Intel's
programmer's reference and Linux's i915 (i915_reg.h, intel_vbt_defs.h,
intel_bios.c). The report says what the firmware set up -- which transcoder
drives which DDI, at what timing, link rate and lane count, from which PLL,
scanning out which plane -- because that is exactly what the native
modesetter has to reproduce.
"""
import argparse
import json
import struct
import sys

TAGS = {1: "PCI", 2: "BARS", 3: "BOOTFB", 4: "OPREGION", 5: "RVDA", 6: "MMIO", 7: "DPCD",
        8: "AUXEDID", 9: "GMBUSEDID", 10: "PCODE", 11: "GTT", 12: "NOTE"}
ERRNO = {0: "ok", -5: "EIO (NACK)", -6: "ENXIO (nothing answered)", -16: "EBUSY",
         -22: "EINVAL", -110: "ETIMEDOUT (no sink)"}


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def s32(b, o): return struct.unpack_from("<i", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]
def bits(v, hi, lo): return (v >> lo) & ((1 << (hi - lo + 1)) - 1)


# ---- the container ---------------------------------------------------------
def parse(data):
    if len(data) < 16 or data[:8] != b"EMBKDISP":
        raise ValueError("not an EMBKDISP capture")
    version, count = u32(data, 8), u32(data, 12)
    if version != 1:
        raise ValueError("capture version %d, this decoder reads 1" % version)
    out, off = [], 16
    while off < len(data):
        if len(data) - off < 8:
            raise ValueError("truncated section header at %d" % off)
        tag, plen = u32(data, off), u32(data, off + 4)
        padded = (plen + 3) & ~3
        if off + 8 + padded > len(data):
            raise ValueError("section %d overruns the file" % len(out))
        out.append((tag, data[off + 8: off + 8 + plen]))
        off += 8 + padded
    if len(out) != count:
        raise ValueError("header says %d sections, file holds %d" % (count, len(out)))
    return out


class Regs:
    """The MMIO sections, as one sparse register file."""
    def __init__(self):
        self.values = {}

    def add(self, payload):
        start, count = u32(payload, 0), u32(payload, 4)
        for i in range(count):
            self.values[start + i * 4] = u32(payload, 8 + i * 4)

    def __contains__(self, off): return off in self.values
    def __getitem__(self, off): return self.values.get(off, 0)


# ---- EDID ------------------------------------------------------------------
def decode_dtd(d):
    clock = u16(d, 0) * 10                                  # kHz
    if not clock:
        return None
    hact = d[2] | (d[4] >> 4) << 8
    hbl = d[3] | (d[4] & 0xf) << 8
    vact = d[5] | (d[7] >> 4) << 8
    vbl = d[6] | (d[7] & 0xf) << 8
    hso = d[8] | ((d[11] >> 6) & 3) << 8
    hsw = d[9] | ((d[11] >> 4) & 3) << 8
    vso = (d[10] >> 4) | ((d[11] >> 2) & 3) << 4
    vsw = (d[10] & 0xf) | (d[11] & 3) << 4
    flags = d[17]
    htotal, vtotal = hact + hbl, vact + vbl
    return {
        "clock_khz": clock, "hactive": hact, "hsync_start": hact + hso, "hsync_end": hact + hso + hsw,
        "htotal": htotal, "vactive": vact, "vsync_start": vact + vso, "vsync_end": vact + vso + vsw,
        "vtotal": vtotal, "refresh_hz": round(clock * 1000.0 / (htotal * vtotal), 3) if htotal and vtotal else 0,
        "hsync_positive": bool(flags & 2), "vsync_positive": bool(flags & 4), "interlaced": bool(flags & 0x80),
        "width_mm": d[12] | (d[14] >> 4) << 8, "height_mm": d[13] | (d[14] & 0xf) << 8,
    }


def decode_edid(e):
    if len(e) < 128 or e[:8] != b"\x00\xff\xff\xff\xff\xff\xff\x00":
        return {"valid": False}
    m = e[8] << 8 | e[9]
    mfg = "".join(chr(((m >> s) & 0x1f) + 64) for s in (10, 5, 0))
    out = {"valid": True, "checksum_ok": sum(e[:128]) & 0xff == 0, "manufacturer": mfg,
           "product": "0x%04x" % u16(e, 10), "serial": u32(e, 12), "week": e[16], "year": 1990 + e[17],
           "version": "%d.%d" % (e[18], e[19]), "digital": bool(e[20] & 0x80),
           "size_cm": [e[21], e[22]], "extensions": e[126], "timings": [], "name": None}
    if e[18] == 1 and e[19] >= 4 and e[20] & 0x80:
        out["bpc"] = {1: 6, 2: 8, 3: 10, 4: 12, 5: 14, 6: 16}.get((e[20] >> 4) & 7)
        out["interface"] = {1: "DVI", 2: "HDMI-a", 3: "HDMI-b", 4: "MDDI", 5: "DisplayPort"}.get(e[20] & 0xf)
    for o in (54, 72, 90, 108):
        d = e[o:o + 18]
        t = decode_dtd(d)
        if t:
            out["timings"].append(t)
        elif d[3] in (0xfc, 0xfe, 0xff):
            text = bytes(d[5:18]).split(b"\n")[0].decode("ascii", "replace").strip()
            out[{0xfc: "name", 0xfe: "text", 0xff: "serial_text"}[d[3]]] = text
        elif d[3] == 0xfd:
            out["range"] = {"v_hz": [d[5], d[6]], "h_khz": [d[7], d[8]], "max_clock_mhz": d[9] * 10}
    return out


# ---- DPCD ------------------------------------------------------------------
LINK_RATE = {0x06: 1.62, 0x0a: 2.7, 0x14: 5.4, 0x1e: 8.1}
EDP_REV = {0: "1.1", 1: "1.2", 2: "1.3", 3: "1.4", 4: "1.4a", 5: "1.4b", 6: "1.5"}


def decode_dpcd(mem):
    """mem: {address: byte} for one sink."""
    g = lambda a: mem.get(a)
    if g(0) is None:
        return None
    out = {"revision": "%d.%d" % (g(0) >> 4, g(0) & 0xf)}
    if g(1) is not None:
        out["max_link_rate_gbps"] = LINK_RATE.get(g(1), "code 0x%02x" % g(1))
    if g(2) is not None:
        out["max_lanes"] = g(2) & 0x1f
        out["tps3"] = bool(g(2) & 0x40)
        out["enhanced_framing"] = bool(g(2) & 0x80)
    if g(3) is not None:
        out["downspread"] = bool(g(3) & 1)
    if g(0xe) is not None:
        out["training_aux_interval"] = g(0xe) & 0x7f
        out["extended_caps"] = bool(g(0xe) & 0x80)
    rates = []
    for i in range(8):
        lo, hi = g(0x10 + 2 * i), g(0x11 + 2 * i)
        if lo is None or hi is None:
            break
        v = lo | hi << 8
        if not v:
            break
        rates.append(round(v * 200 / 1e6, 4))              # 200 kHz units -> Gbps
    if rates:
        out["edp_link_rates_gbps"] = rates
    if g(0x70) is not None:
        out["psr_version"] = g(0x70)
    if g(0x700) is not None:
        out["edp_revision"] = EDP_REV.get(g(0x700), "0x%02x" % g(0x700))
        out["edp_general_caps"] = ["0x%02x" % (g(a) or 0) for a in (0x701, 0x702, 0x703)]
    if g(0x100) is not None:
        out["link_bw_set"] = LINK_RATE.get(g(0x100), g(0x100))
        out["lane_count_set"] = (g(0x101) or 0) & 0x1f
        out["link_rate_set_index"] = g(0x115)
    if g(0x202) is not None:
        lanes = (g(0x202) or 0) | (g(0x203) or 0) << 8
        out["lane_status"] = ["%s%s%s" % ("C" if s & 1 else "-", "E" if s & 2 else "-", "S" if s & 4 else "-")
                              for s in ((lanes >> (4 * i)) & 0xf for i in range(4))]
        out["interlane_aligned"] = bool((g(0x204) or 0) & 1)
    if g(0x600) is not None:
        out["power_state"] = {1: "D0", 2: "D3", 5: "D3 (aux on)"}.get(g(0x600), g(0x600))
    if g(0x2200) is not None and out.get("extended_caps"):
        out["extended_revision"] = "%d.%d" % (g(0x2200) >> 4, g(0x2200) & 0xf)
        out["extended_max_link_rate_gbps"] = LINK_RATE.get(g(0x2201), g(0x2201))
    return out


# ---- VBT -------------------------------------------------------------------
DVO_PORT = {0: "HDMI-A", 1: "HDMI-B", 2: "HDMI-C", 3: "HDMI-D", 4: "LVDS", 5: "TV", 6: "CRT",
            7: "DP-B", 8: "DP-C", 9: "DP-D", 10: "DP-A (eDP)", 11: "DP-E", 12: "HDMI-E",
            13: "DP-F", 14: "HDMI-F"}
DVO_DDI = {0: "A", 1: "B", 2: "C", 3: "D", 7: "B", 8: "C", 9: "D", 10: "A", 11: "E", 12: "E", 13: "F", 14: "F"}
AUX_CH = {0x40: "A", 0x10: "B", 0x20: "C", 0x30: "D", 0x50: "E", 0x60: "F"}
DEVICE_TYPE = {0x78c6: "eDP", 0x68c6: "DP", 0x60d6: "DP dual mode (DP++)", 0x60d2: "HDMI", 0x68d2: "DVI",
               0x1022: "internal LFP", 0x1806: "eDP (legacy)"}


def child_size_for(version):
    if version < 106: return 22
    if version < 111: return 27
    if version < 195: return 33
    if version == 195: return 37
    if version <= 215: return 38
    if version <= 255: return 39
    return 40


def decode_vbt(v):
    if len(v) < 48 or v[:4] != b"$VBT":
        return {"valid": False}
    out = {"valid": True, "signature": v[:20].decode("ascii", "replace").strip(),
           "vbt_version": u16(v, 20), "vbt_size": u16(v, 24),
           "checksum_ok": sum(v[:u16(v, 24)]) & 0xff == 0 if u16(v, 24) <= len(v) else False}
    bdb_off = u32(v, 32)
    bdb = v[bdb_off:]
    if bdb[:16] != b"BIOS_DATA_BLOCK ":
        out["error"] = "no BDB at offset %d" % bdb_off
        return out
    version, hsize, bsize = u16(bdb, 16), u16(bdb, 18), u16(bdb, 20)
    out["bdb_version"] = version
    blocks, off = {}, hsize
    while off + 3 <= min(bsize, len(bdb)):
        bid, size = bdb[off], u16(bdb, off + 1)
        start = off + 3
        if bid == 53 and start < len(bdb) and bdb[start] >= 3:   # MIPI sequence v3+: u32 size
            size = u32(bdb, start + 1) + 5
        if bid == 0 or start + size > len(bdb):
            break
        blocks[bid] = (start, bdb[start:start + size])
        off = start + size
    out["blocks"] = {str(k): len(b) for k, (_, b) in sorted(blocks.items())}

    if 2 in blocks:
        _, g = blocks[2]
        csize = g[4] if len(g) > 4 and g[4] else child_size_for(version)
        children = []
        for i in range((len(g) - 5) // csize):
            c = g[5 + i * csize: 5 + (i + 1) * csize]
            dtype = u16(c, 2)
            if not dtype:
                continue
            child = {"handle": "0x%04x" % u16(c, 0), "device_type": "0x%04x" % dtype,
                     "kind": DEVICE_TYPE.get(dtype, "other"), "dvo_port": DVO_PORT.get(c[16], c[16]),
                     "ddi": DVO_DDI.get(c[16]), "ddc_pin": c[19]}
            if version >= 158 and len(c) > 25:
                child["hdmi_support"] = bool(c[24] & 1)
                child["dp_support"] = bool(c[24] & 2)
                child["aux_channel"] = AUX_CH.get(c[25], c[25]) if c[25] else None
                child["lane_reversal"] = bool(c[23] & 2)
                child["hpd_invert"] = bool(c[23] & 0x10)
                child["iboost"] = bool(c[23] & 8)
            if version >= 195 and len(c) > 33:
                child["usb_type_c"] = bool(c[33] & 1)
                child["thunderbolt"] = bool(c[33] & 2)
            if version >= 196 and len(c) > 37:
                child["dp_iboost_level"] = c[37] & 0xf
                child["hdmi_iboost_level"] = c[37] >> 4
            if version >= 216 and len(c) > 38:
                child["dp_max_link_rate_code"] = c[38] & 7
            if version >= 204 and len(c) > 7:
                child["hdmi_max_data_rate_code"] = c[7] >> 5
            children.append(child)
        out["children"] = children

    panel = None
    if 40 in blocks:
        _, o = blocks[40]
        panel = o[0]
        out["panel_type"] = panel
        if len(o) > 1 and version >= 212:
            out["panel_type2"] = o[1]
    if panel is not None and panel < 16:
        if 27 in blocks:
            _, e = blocks[27]
            if len(e) >= (panel + 1) * 10:
                t1_t3, t8, t9, t10, t11_t12 = struct.unpack_from("<5H", e, panel * 10)
                out["edp_power_sequence_ms"] = {"t1_t3": t1_t3 / 10, "t8": t8 / 10, "t9": t9 / 10,
                                                "t10": t10 / 10, "t11_t12": t11_t12 / 10}
            if len(e) >= 164:
                bpp = (u32(e, 160) >> (panel * 2)) & 3
                out["edp_bpp"] = {0: 18, 1: 24, 2: 30, 3: 36}[bpp]
            if len(e) >= 164 + 32:
                f = e[164 + panel * 2: 166 + panel * 2]
                out["edp_fast_link"] = {"rate_code": f[0] & 0xf, "lanes_code": f[0] >> 4,
                                        "preemphasis": f[1] & 0xf, "vswing": f[1] >> 4}
        if 43 in blocks:
            _, b = blocks[43]
            esize = b[0] if b else 0
            if esize and len(b) >= 1 + (panel + 1) * esize:
                ent = b[1 + panel * esize: 1 + (panel + 1) * esize]
                out["backlight"] = {"type": {0: "none", 2: "PWM"}.get(ent[0] & 3, ent[0] & 3),
                                    "active_low": bool(ent[0] & 4),
                                    "pwm_hz": u16(ent, 1) if esize >= 3 else None,
                                    "min_brightness": ent[3] if esize >= 4 else None}
        if 41 in blocks:
            _, p = blocks[41]
            base = 1 + panel * 9
            if len(p) >= base + 9:
                dvo_off, dvo_size = u16(p, base + 3), p[base + 5]
                if dvo_size >= 18 and dvo_off + 18 <= len(bdb):
                    out["lfp_timing"] = decode_dtd(bdb[dvo_off:dvo_off + 18])
    return out, blocks


# ---- registers -------------------------------------------------------------
TRANSCODERS = [("A", 0x60000, 0x70008), ("B", 0x61000, 0x71008), ("C", 0x62000, 0x72008),
               ("eDP", 0x6f000, 0x7f008)]
PIPES = "ABC"
DDI_MODE = {0: "HDMI", 1: "DVI", 2: "DP SST", 3: "DP MST", 4: "FDI"}
BPC = {0: 8, 1: 10, 2: 6, 3: 12}
EDP_INPUT = {0: "A (always on)", 4: "A", 5: "B", 6: "C"}
DPLL_RATE_KHZ = {0: 540000, 1: 270000, 2: 162000, 3: 324000, 4: 216000, 5: 432000}
PLANE_FMT = {0: "YUV422", 1: "NV12", 2: "RGB2101010", 4: "XRGB8888", 6: "RGB16161616F", 8: "AYUV",
             9: "INDEXED"}
TILING = {0: "linear", 1: "X", 4: "Y", 5: "Yf"}
CDCLK_SEL = {0: "450/432", 1: "540", 2: "337.5/308.57", 3: "675/617.14"}
PW = [("MISC IO", 0), ("DDI A/E", 1), ("DDI B", 2), ("DDI C", 3), ("DDI D", 4), ("PW1", 14), ("PW2", 15)]


def timing(r, base):
    def pair(off):
        v = r[base + off]
        return bits(v, 28, 16) + 1, bits(v, 12, 0) + 1
    htot, hact = pair(0x00)
    hbe, hbs = pair(0x04)
    hse, hss = pair(0x08)
    vtot, vact = pair(0x0c)
    vbe, vbs = pair(0x10)
    vse, vss = pair(0x14)
    return {"hactive": hact, "hsync_start": hss, "hsync_end": hse, "htotal": htot,
            "hblank": [hbs, hbe], "vactive": vact, "vsync_start": vss, "vsync_end": vse,
            "vtotal": vtot, "vblank": [vbs, vbe]}


def decode_registers(r, pch_layout):
    out = {}
    ctl2 = r[0x45404]
    out["power_wells"] = {name: {"on": bool(ctl2 >> (2 * i) & 1),
                                 "bios_request": bool(r[0x45400] >> (2 * i + 1) & 1),
                                 "driver_request": bool(ctl2 >> (2 * i + 1) & 1)} for name, i in PW}
    out["dc_states"] = {"dc5": bool(r[0x45504] & 1), "dc6": bool(r[0x45504] & 2), "raw": "0x%08x" % r[0x45504]}
    out["dbuf"] = {"requested": bool(r[0x45008] >> 31 & 1), "on": bool(r[0x45008] >> 30 & 1)}
    cd = r[0x46000]
    out["cdclk"] = {"mhz": (bits(cd, 10, 0) * 500 + 1000) / 1000, "select": CDCLK_SEL[bits(cd, 27, 26)],
                    "raw": "0x%08x" % cd}

    ctrl1, ctrl2, status = r[0x6c058], r[0x6c05c], r[0x6c060]
    plls = []
    enables = [r[0x46010], r[0x46014], r[0x46040], r[0x46060]]
    cfgcr = [None, (0x6c040, 0x6c044), (0x6c048, 0x6c04c), (0x6c050, 0x6c054)]
    for i in range(4):
        f = (ctrl1 >> (6 * i)) & 0x3f
        pll = {"id": i, "enabled": bool(enables[i] >> 31 & 1), "locked": bool(status >> (8 * i) & 1),
               "override": bool(f & 1), "hdmi_mode": bool(f & 0x20), "ssc": bool(f & 0x10)}
        if f & 1 and not f & 0x20:
            pll["dp_link_rate_gbps"] = DPLL_RATE_KHZ.get(bits(f, 3, 1), 0) / 100000
        if cfgcr[i] and f & 0x20:
            c1, c2 = r[cfgcr[i][0]], r[cfgcr[i][1]]
            dco_int, dco_frac = bits(c1, 8, 0), bits(c1, 23, 9)
            dco = (dco_int + dco_frac / 0x8000) * 24.0
            pdiv = {0: 1, 1: 2, 2: 3, 4: 7}.get(bits(c2, 4, 2), 0)
            kdiv = {0: 5, 1: 2, 2: 3, 3: 1}.get(bits(c2, 6, 5), 0)
            qdiv = bits(c2, 15, 8) if c2 & 0x80 else 1
            pll["hdmi"] = {"dco_mhz": round(dco, 3), "pdiv": pdiv, "kdiv": kdiv, "qdiv": qdiv,
                           "central": {0: 9600, 1: 9000, 3: 8400}.get(bits(c2, 1, 0)),
                           "pixel_mhz": round(dco / (5 * pdiv * kdiv * qdiv), 3) if pdiv and kdiv and qdiv else None}
        plls.append(pll)
    out["dplls"] = plls
    out["ddi_clocks"] = {}
    for port in range(5):
        out["ddi_clocks"]["ABCDE"[port]] = {
            "off": bool(ctrl2 >> (15 + port) & 1), "override": bool(ctrl2 >> (3 * port) & 1),
            "dpll": bits(ctrl2, 3 * port + 2, 3 * port + 1)}

    ddis = {}
    for port in range(5):
        base = 0x64000 + port * 0x100
        buf, tp = r[base], r[base + 0x40]
        ddis["ABCDE"[port]] = {
            "buffer_enabled": bool(buf >> 31 & 1), "idle": bool(buf >> 7 & 1),
            "lanes_field": bits(buf, 3, 1), "a_4_lanes": bool(buf >> 4 & 1) if port == 0 else None,
            "translation_select": bits(buf, 27, 24), "reversal": bool(buf >> 16 & 1),
            "dp_tp_enabled": bool(tp >> 31 & 1), "dp_tp_mst": bool(tp >> 27 & 1),
            "dp_tp_enhanced_framing": bool(tp >> 18 & 1),
            "dp_tp_pattern": {0: "TPS1", 1: "TPS2", 2: "idle", 3: "normal", 4: "TPS3"}.get(bits(tp, 10, 8)),
            "raw": ["0x%08x" % buf, "0x%08x" % tp, "0x%08x" % r[base + 0x44]]}
        trans = [r[0x64e00 + port * 0x60 + i * 4] for i in range(20)]
        if any(trans):
            ddis["ABCDE"[port]]["buffer_translations"] = ["0x%08x/0x%08x" % (trans[2 * i], trans[2 * i + 1])
                                                          for i in range(10)]
    out["ddis"] = ddis

    transcoders = {}
    for name, base, conf_reg in TRANSCODERS:
        fn, conf = r[base + 0x400], r[conf_reg]
        t = {"enabled": bool(conf >> 31 & 1), "active": bool(conf >> 30 & 1),
             "ddi_function_enabled": bool(fn >> 31 & 1), "mode": DDI_MODE.get(bits(fn, 26, 24)),
             "bpc": BPC.get(bits(fn, 22, 20)), "hsync_positive": bool(fn >> 16 & 1),
             "vsync_positive": bool(fn >> 17 & 1),
             "lanes": {0: 1, 1: 2, 3: 4}.get(bits(fn, 3, 1)), "raw_ddi_func_ctl": "0x%08x" % fn,
             "raw_conf": "0x%08x" % conf}
        if name == "eDP":
            t["ddi"] = "A"
            t["pipe"] = EDP_INPUT.get(bits(fn, 14, 12), "code %d" % bits(fn, 14, 12))
        else:
            t["ddi"] = "ABCDE"[bits(fn, 30, 28)] if bits(fn, 30, 28) < 5 else None
            t["pipe"] = name
        if t["enabled"] or t["ddi_function_enabled"]:
            t["timing"] = timing(r, base)
            dm, dn, lm, ln = r[base + 0x30], r[base + 0x34], r[base + 0x40], r[base + 0x44]
            t["m_n"] = {"tu": bits(dm, 30, 25) + 1, "data_m": bits(dm, 23, 0), "data_n": bits(dn, 23, 0),
                        "link_m": bits(lm, 23, 0), "link_n": bits(ln, 23, 0)}
        transcoders[name] = t
    out["transcoders"] = transcoders

    pipes = {}
    for p, letter in enumerate(PIPES):
        src = r[0x6001c + p * 0x1000]
        pipe = {"source": [bits(src, 28, 16) + 1, bits(src, 12, 0) + 1] if src else None,
                "underrun": bool(r[0x44400 + p * 0x10] >> 31 & 1), "planes": [], "scalers": []}
        for plane in range(3):
            b = 0x70180 + p * 0x1000 + plane * 0x100
            ctl = r[b]
            if not ctl >> 31 & 1:
                continue
            size, buf = r[b + 0x10], r[0x7027c + p * 0x1000 + plane * 0x100]
            pipe["planes"].append({
                "plane": plane + 1, "format": PLANE_FMT.get(bits(ctl, 27, 24), bits(ctl, 27, 24)),
                "rgb_order": "RGBX" if ctl >> 20 & 1 else "BGRX", "tiling": TILING.get(bits(ctl, 12, 10)),
                "rotation": bits(ctl, 1, 0) * 90, "stride_bytes": bits(r[b + 0x08], 10, 0) * 64,
                "position": [bits(r[b + 0x0c], 12, 0), bits(r[b + 0x0c], 28, 16)],
                "size": [bits(size, 12, 0) + 1, bits(size, 28, 16) + 1],
                "surface": "0x%08x" % (r[b + 0x1c] & 0xfffff000),
                "offset": [bits(r[b + 0x24], 12, 0), bits(r[b + 0x24], 28, 16)],
                "ddb": [bits(buf, 10, 0), bits(buf, 26, 16)],
                "watermarks": [{"level": lv, "enabled": bool(w >> 31 & 1), "lines": bits(w, 18, 14),
                                "blocks": bits(w, 10, 0)}
                               for lv, w in ((lv, r[0x70240 + p * 0x1000 + plane * 0x100 + lv * 4]) for lv in range(8))],
                "transition_wm": "0x%08x" % r[0x70268 + p * 0x1000 + plane * 0x100],
                "raw_ctl": "0x%08x" % ctl})
        for s in range(2):
            base = 0x68180 + p * 0x800 + s * 0x100
            ctl = r[base]
            if ctl >> 31 & 1:
                pos, sz = r[base - 0x10], r[base - 0x0c]
                pipe["scalers"].append({"id": s, "ctl": "0x%08x" % ctl,
                                        "window": [bits(pos, 28, 16), bits(pos, 12, 0), bits(sz, 28, 16), bits(sz, 12, 0)]})
        cur = r[0x70080 + p * 0x1000]
        pipe["cursor_enabled"] = bool(cur & 0x27)
        pipe["pipe_misc"] = "0x%08x" % r[0x70030 + p * 0x1000]
        pipes[letter] = pipe
    out["pipes"] = pipes

    pp_status, pp_ctl = r[0xc7200], r[0xc7204]
    on, off_ = r[0xc7208], r[0xc720c]
    out["panel_power"] = {
        "on": bool(pp_status >> 31 & 1), "sequence": {0: "idle", 1: "powering up", 2: "powering down"}.get(bits(pp_status, 29, 28)),
        "cycle_delay_active": bool(pp_status >> 27 & 1),
        "target_on": bool(pp_ctl & 1), "backlight_enabled": bool(pp_ctl & 4), "vdd_forced": bool(pp_ctl & 8),
        "power_up_delay_ms": bits(on, 28, 16) / 10, "light_on_delay_ms": bits(on, 12, 0) / 10,
        "power_down_delay_ms": bits(off_, 28, 16) / 10, "light_off_delay_ms": bits(off_, 12, 0) / 10,
        "power_cycle_delay_ms": (bits(pp_ctl, 8, 4) * 100 if pch_layout == 2 else bits(r[0xc7210], 4, 0) * 100),
        "raw": ["0x%08x" % x for x in (pp_status, pp_ctl, on, off_, r[0xc7210])]}
    pwm_ctl, freq, duty = r[0xc8250], r[0xc8254], r[0xc8258]
    if pch_layout == 1:
        freq, duty = freq >> 16, freq & 0xffff
    out["backlight_pwm"] = {"enabled": bool(pwm_ctl >> 31 & 1), "inverted": bool(pwm_ctl >> 29 & 1),
                            "frequency_counts": freq, "duty_counts": duty,
                            "percent": round(100.0 * duty / freq, 1) if freq else None}
    sde = r[0xc4000]
    out["hotplug"] = {"live": {p: bool(sde >> b & 1) for p, b in (("A", 24), ("B", 21), ("C", 22), ("D", 23), ("E", 25))},
                      "shotplug_ctl": "0x%08x" % r[0xc4030], "shotplug_ctl2": "0x%08x" % r[0xc403c],
                      "sfuse_strap": "0x%08x" % r[0xc2014]}
    out["master_irq_enabled"] = bool(r[0x44200] >> 31 & 1)
    return out


def describe_firmware_mode(regs, plls):
    """One readable line per lit output: what the firmware put on screen."""
    lines = []
    for name, t in regs["transcoders"].items():
        if not t["enabled"]:
            continue
        tm, mn = t.get("timing", {}), t.get("m_n", {})
        ddi = t.get("ddi")
        clk = regs["ddi_clocks"].get(ddi, {}) if ddi else {}
        pll = plls[clk["dpll"]] if clk and not clk.get("off") else None
        rate = pll.get("dp_link_rate_gbps") if pll else None
        pixel = None
        if mn.get("link_n") and rate:
            pixel = round(mn["link_m"] / mn["link_n"] * rate * 1e5, 1)      # kHz
        elif pll and pll.get("hdmi"):
            pixel = pll["hdmi"]["pixel_mhz"] * 1000 if pll["hdmi"]["pixel_mhz"] else None
        refresh = round(pixel * 1000 / (tm["htotal"] * tm["vtotal"]), 2) if pixel and tm else None
        lines.append("transcoder %s -> DDI %s, %s, %s lanes, %s bpc, pipe %s: %sx%s (total %sx%s)%s%s, DPLL%s%s" % (
            name, ddi, t["mode"], t["lanes"], t["bpc"], t["pipe"], tm.get("hactive"), tm.get("vactive"),
            tm.get("htotal"), tm.get("vtotal"),
            ", %.1f MHz" % (pixel / 1000) if pixel else "", ", %.2f Hz" % refresh if refresh else "",
            clk.get("dpll") if clk else "?", " at %.2f Gbps" % rate if rate else ""))
    return lines


# ---- the report ------------------------------------------------------------
def decode(data):
    sections = parse(data)
    rep = {"sections": len(sections), "notes": [], "pci": [], "dpcd": {}, "aux_edid": {}, "gmbus_edid": {},
           "pcode": {}, "counts": {}}
    regs = Regs()
    dpcd_mem, vbt_bytes, opregion = {}, None, None
    for tag, p in sections:
        name = TAGS.get(tag, "tag%d" % tag)
        rep["counts"][name] = rep["counts"].get(name, 0) + 1
        if tag == 1:
            cfg = p[4:]
            rep["pci"].append({"bdf": "%02x:%02x.%x" % (p[0], p[1], p[2]),
                               "role": {0: "GPU", 1: "PCH", 2: "other display"}.get(p[3], p[3]),
                               "id": "%04x:%04x" % (u16(cfg, 0), u16(cfg, 2)), "revision": cfg[8],
                               "subsystem": "%04x:%04x" % (u16(cfg, 0x2c), u16(cfg, 0x2e)),
                               "class": "%02x%02x%02x" % (cfg[0xb], cfg[0xa], cfg[9]),
                               "asls": "0x%08x" % u32(cfg, 0xfc), "bdsm": "0x%08x" % u32(cfg, 0x5c),
                               "ggc": "0x%04x" % u16(cfg, 0x50)})
        elif tag == 2:
            rep["bars"] = {"bar0": "0x%x" % u64(p, 0), "bar0_size": u64(p, 8), "bar2": "0x%x" % u64(p, 16),
                           "bar2_size": u64(p, 24), "pch_layout": {0: "unknown", 1: "SPT/KBP", 2: "CNP/CMP"}.get(u32(p, 32)),
                           "pch_layout_code": u32(p, 32)}
        elif tag == 3:
            rep["boot_framebuffer"] = {"address": "0x%x" % u64(p, 0), "width": u32(p, 8), "height": u32(p, 12),
                                       "pitch": u32(p, 16), "bpp": u32(p, 20),
                                       "format": {0: "RGB", 1: "BGR"}.get(u32(p, 24), u32(p, 24)),
                                       "firmware": {1: "BIOS", 2: "UEFI"}.get(u32(p, 28), u32(p, 28))}
            if u32(p, 32) == 128:
                rep["boot_framebuffer"]["gop_edid"] = decode_edid(p[40:168])
        elif tag == 4:
            opregion = p[12:12 + u32(p, 8)]
            over = u32(opregion, 0x14)
            rep["opregion"] = {"phys": "0x%x" % u64(p, 0), "signature_ok": opregion[:16] == b"IntelGraphicsMem",
                               "version": "%d.%d" % (over >> 24, (over >> 16) & 0xff),
                               "mailboxes": "0x%x" % u32(opregion, 0x58)}
            if vbt_bytes is None and opregion[0x400:0x404] == b"$VBT":
                vbt_bytes = opregion[0x400:0x400 + u16(opregion, 0x400 + 24)]
                rep["opregion"]["vbt_in"] = "mailbox 4"
        elif tag == 5:
            vbt_bytes = p[12:12 + u32(p, 8)]
            if "opregion" in rep:
                rep["opregion"]["vbt_in"] = "RVDA at 0x%x" % u64(p, 0)
        elif tag == 6:
            regs.add(p)
        elif tag == 7:
            ch, addr, rc, n = "ABCD"[p[0]], u32(p, 4), s32(p, 8), u32(p, 12)
            mem = dpcd_mem.setdefault(ch, {})
            if rc == 0:
                for i in range(n):
                    mem[addr + i] = p[16 + i]
            else:
                rep["dpcd"].setdefault(ch, {}).setdefault("errors", []).append(
                    {"address": "0x%05x" % addr, "rc": ERRNO.get(rc, rc)})
        elif tag == 8:
            ch, rc = "ABCD"[p[0]], s32(p, 4)
            rep["aux_edid"][ch] = decode_edid(p[12:12 + u32(p, 8)]) if rc == 0 else {"rc": ERRNO.get(rc, rc)}
        elif tag == 9:
            pin, rc = p[0], s32(p, 4)
            rep["gmbus_edid"][str(pin)] = decode_edid(p[12:12 + u32(p, 8)]) if rc == 0 else {"rc": ERRNO.get(rc, rc)}
        elif tag == 10:
            cmd, param, rc, value = u32(p, 0), u32(p, 4), s32(p, 8), u32(p, 12)
            if cmd == 6 and rc == 0:
                rep["pcode"].setdefault("memory_latency_us", [None] * 8)
                for i in range(4):
                    rep["pcode"]["memory_latency_us"][param * 4 + i] = (value >> (8 * i)) & 0xff
            elif rc:
                rep["pcode"]["error_%d_%d" % (cmd, param)] = ERRNO.get(rc, rc)
        elif tag == 11:
            first, pages, count = u32(p, 8), u32(p, 12), u32(p, 16)
            ptes = [u64(p, 20 + i * 8) for i in range(count)]
            addrs = [pte & 0x7ffffff000 for pte in ptes]
            rep["gtt"] = {"gtt_phys": "0x%x" % u64(p, 0), "first_index": first, "framebuffer_pages": pages,
                          "captured": count, "all_valid": all(pte & 1 for pte in ptes),
                          "contiguous": all(addrs[i + 1] == addrs[i] + 4096 for i in range(len(addrs) - 1)),
                          "first_page": "0x%x" % addrs[0] if addrs else None,
                          "pte_low_bits": sorted({"0x%03x" % (pte & 0xfff) for pte in ptes})[:8]}
        elif tag == 12:
            rep["notes"].append(p.decode("ascii", "replace"))

    for ch, mem in dpcd_mem.items():
        d = decode_dpcd(mem)
        if d:
            rep["dpcd"].setdefault(ch, {}).update(d)
    if vbt_bytes is not None:
        vbt, _ = decode_vbt(vbt_bytes)
        rep["vbt"] = vbt
    if regs.values:
        r = decode_registers(regs, rep.get("bars", {}).get("pch_layout_code", 0))
        rep["registers"] = r
        rep["firmware_mode"] = describe_firmware_mode(r, r["dplls"])
    rep["_vbt_bytes"] = vbt_bytes
    return rep


def print_report(rep, out=sys.stdout):
    w = lambda s="": print(s, file=out)
    w("EMBKDISP capture: %d sections" % rep["sections"])
    for f in rep["pci"]:
        w("  %s %-13s %s rev %02x subsystem %s" % (f["bdf"], f["role"], f["id"], f["revision"], f["subsystem"]))
    if "bars" in rep:
        b = rep["bars"]
        w("  BAR0 %s (%d MiB), aperture %s (%d MiB), PCH %s" % (b["bar0"], b["bar0_size"] >> 20, b["bar2"],
                                                              b["bar2_size"] >> 20, b["pch_layout"]))
    if "boot_framebuffer" in rep:
        f = rep["boot_framebuffer"]
        w("  boot framebuffer %s: %dx%d pitch %d, %s, from %s" % (f["address"], f["width"], f["height"],
                                                                  f["pitch"], f["format"], f["firmware"]))
    w()
    w("What the firmware put on screen:")
    for line in rep.get("firmware_mode", []) or ["  (no enabled transcoder)"]:
        w("  " + line)
    r = rep.get("registers")
    if r:
        w("  CDCLK %.1f MHz; power wells on: %s; DC states %s" % (
            r["cdclk"]["mhz"], ", ".join(n for n, s in r["power_wells"].items() if s["on"]) or "none",
            "on" if r["dc_states"]["dc5"] or r["dc_states"]["dc6"] else "off"))
        for letter, pipe in r["pipes"].items():
            for pl in pipe["planes"]:
                w("  pipe %s plane %d: %s %s %s, %dx%d stride %d at %s, DDB %s, WM0 %s" % (
                    letter, pl["plane"], pl["format"], pl["rgb_order"], pl["tiling"], pl["size"][0], pl["size"][1],
                    pl["stride_bytes"], pl["surface"], pl["ddb"],
                    "%d lines/%d blocks" % (pl["watermarks"][0]["lines"], pl["watermarks"][0]["blocks"])))
            if pipe["underrun"]:
                w("  pipe %s: FIFO UNDERRUN latched" % letter)
        pp = r["panel_power"]
        w("  panel power %s, backlight %s; delays up %.1f / on %.1f / down %.1f / off %.1f / cycle %.0f ms" % (
            "on" if pp["on"] else "off", "on" if pp["backlight_enabled"] else "off", pp["power_up_delay_ms"],
            pp["light_on_delay_ms"], pp["power_down_delay_ms"], pp["light_off_delay_ms"], pp["power_cycle_delay_ms"]))
        bl = r["backlight_pwm"]
        w("  backlight PWM %s, %s/%s counts (%s%%)" % ("on" if bl["enabled"] else "off", bl["duty_counts"],
                                                       bl["frequency_counts"], bl["percent"]))
        w("  hotplug live: %s" % (", ".join(p for p, s in r["hotplug"]["live"].items() if s) or "none"))
    w()
    v = rep.get("vbt")
    if v:
        w("VBT %s (BDB %s), %d blocks; panel type %s" % (v.get("signature"), v.get("bdb_version"),
                                                         len(v.get("blocks", {})), v.get("panel_type")))
        for c in v.get("children", []):
            extra = []
            if c.get("aux_channel") is not None: extra.append("AUX %s" % c["aux_channel"])
            if c.get("ddc_pin"): extra.append("DDC pin %d" % c["ddc_pin"])
            if c.get("usb_type_c"): extra.append("USB-C")
            if c.get("hdmi_support"): extra.append("HDMI")
            if c.get("dp_support"): extra.append("DP")
            w("  %-22s DDI %s  %s  (%s)" % (c["kind"], c["ddi"], c["dvo_port"], ", ".join(extra)))
        if "edp_power_sequence_ms" in v:
            w("  eDP power sequence (ms): %s" % v["edp_power_sequence_ms"])
        if "backlight" in v:
            w("  backlight: %s" % v["backlight"])
        if v.get("lfp_timing"):
            t = v["lfp_timing"]
            w("  panel timing: %dx%d @ %.3f Hz, %.2f MHz" % (t["hactive"], t["vactive"], t["refresh_hz"], t["clock_khz"] / 1000))
    for ch, d in sorted(rep["dpcd"].items()):
        w("AUX %s: %s" % (ch, json.dumps(d)))
    for ch, e in sorted(rep["aux_edid"].items()):
        w("AUX %s EDID: %s" % (ch, summarize_edid(e)))
    for pin, e in sorted(rep["gmbus_edid"].items()):
        w("GMBUS pin %s EDID: %s" % (pin, summarize_edid(e)))
    if rep["pcode"]:
        w("pcode: %s" % rep["pcode"])
    if "gtt" in rep:
        g = rep["gtt"]
        w("GTT: %d framebuffer pages from index %d, %s, %s, first page %s" % (
            g["framebuffer_pages"], g["first_index"], "all valid" if g["all_valid"] else "SOME INVALID",
            "contiguous" if g["contiguous"] else "scattered", g["first_page"]))
    for n in rep["notes"]:
        w("note: " + n)


def summarize_edid(e):
    if "rc" in e:
        return e["rc"]
    if not e.get("valid"):
        return "invalid"
    t = e["timings"][0] if e["timings"] else None
    return "%s %s %s%s" % (e["manufacturer"], e["product"], e.get("name") or "",
                           " %dx%d@%.2f %.2f MHz" % (t["hactive"], t["vactive"], t["refresh_hz"], t["clock_khz"] / 1000) if t else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("capture")
    ap.add_argument("--json", action="store_true", help="print the decoded capture as JSON")
    ap.add_argument("--vbt-out", help="write the raw VBT to this file")
    args = ap.parse_args()
    with open(args.capture, "rb") as f:
        rep = decode(f.read())
    vbt = rep.pop("_vbt_bytes")
    if args.vbt_out and vbt is not None:
        with open(args.vbt_out, "wb") as f:
            f.write(vbt)
    if args.json:
        json.dump(rep, sys.stdout, indent=1, sort_keys=True)
        print()
    else:
        print_report(rep)


if __name__ == "__main__":
    main()
