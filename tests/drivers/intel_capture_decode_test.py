#!/usr/bin/env python3
"""Decode the capture tests/drivers/intel_capture_test.c builds from its
simulated Comet Lake laptop, and check tools/intel_display_dump.py reads back
exactly what the simulation holds."""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
capture = sys.argv[1]
out = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "intel_display_dump.py"), capture, "--json"],
                     check=True, capture_output=True, text=True).stdout
rep = json.loads(out)
text = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "intel_display_dump.py"), capture],
                      check=True, capture_output=True, text=True).stdout


def check(cond, what):
    if not cond:
        sys.exit("intel capture decode: FAILED: " + what)


check(rep["pci"][0]["id"] == "8086:9b41" and rep["pci"][1]["role"] == "PCH", "PCI functions")
check(rep["bars"]["pch_layout"] == "CNP/CMP" and rep["boot_framebuffer"]["width"] == 1920, "BARs and boot framebuffer")
check(rep["opregion"]["version"] == "2.1" and rep["opregion"]["vbt_in"] == "mailbox 4", "OpRegion %s" % rep["opregion"])
v = rep["vbt"]
check(v["valid"] and v["checksum_ok"] and v["bdb_version"] == 228 and v["panel_type"] == 2, "VBT header")
kinds = [(c["kind"], c["ddi"], c.get("aux_channel"), c["ddc_pin"], c.get("usb_type_c")) for c in v["children"]]
check(kinds == [("eDP", "A", "A", 0, False), ("HDMI", "B", None, 1, False), ("DP", "C", "C", 2, True)], "VBT children %s" % kinds)
check(v["edp_power_sequence_ms"] == {"t1_t3": 200.0, "t8": 50.0, "t9": 50.0, "t10": 50.0, "t11_t12": 500.0}, "eDP power sequence")
check(v["backlight"]["type"] == "PWM" and v["backlight"]["pwm_hz"] == 200, "VBT backlight")
check(v["lfp_timing"]["hactive"] == 1920 and v["lfp_timing"]["clock_khz"] == 142500, "LFP timing")
a = rep["dpcd"]["A"]
check(a["revision"] == "1.2" and a["max_link_rate_gbps"] == 2.7 and a["max_lanes"] == 4, "DPCD caps")
check(a["edp_revision"] == "1.4" and a["edp_link_rates_gbps"] == [1.62, 2.16, 2.7], "eDP DPCD")
check(a["errors"] == [{"address": "0x00080", "rc": "EIO (NACK)"}], "the NACKed range is reported, not hidden")
check("extended_revision" not in a, "no extended caps claimed when 0x00e says none")
check(rep["dpcd"]["B"]["errors"][0]["rc"] == "ETIMEDOUT (no sink)", "empty port")
e = rep["aux_edid"]["A"]
check(e["manufacturer"] == "AUO" and e["name"] == "B156HAN02.1" and e["checksum_ok"], "eDP EDID")
check(rep["gmbus_edid"]["1"]["name"] == "DELL U2717D" and rep["gmbus_edid"]["2"]["rc"].startswith("ENXIO"), "HDMI EDID")
check(rep["pcode"]["memory_latency_us"] == [2, 4, 8, 10, 15, 20, 25, 30], "pcode latencies")
check(rep["gtt"]["all_valid"] and rep["gtt"]["contiguous"] and rep["gtt"]["framebuffer_pages"] == 2025, "GTT")
r = rep["registers"]
check(r["cdclk"]["mhz"] == 337.5, "CDCLK %s" % r["cdclk"])
t = r["transcoders"]["eDP"]
check(t["enabled"] and t["mode"] == "DP SST" and t["lanes"] == 4 and t["timing"]["htotal"] == 2080, "eDP transcoder")
check(r["dplls"][0]["dp_link_rate_gbps"] == 2.7, "DPLL0 link rate")
check(rep["firmware_mode"] == ["transcoder eDP -> DDI A, DP SST, 4 lanes, 8 bpc, pipe A (always on): 1920x1080 "
                               "(total 2080x1110), 142.5 MHz, 61.72 Hz, DPLL0 at 2.70 Gbps"], "firmware mode line %s" % rep["firmware_mode"])
check("What the firmware put on screen" in text and "hotplug live: none" in text, "text report")
print("Intel capture decoder: VBT children/power sequence/backlight/LFP timing, DPCD and eDP link rates, "
      "EDIDs over AUX and GMBUS, pcode, GTT, registers and the firmware mode line OK")
