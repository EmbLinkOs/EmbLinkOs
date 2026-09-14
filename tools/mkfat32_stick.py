#!/usr/bin/env python3
"""mkfat32_stick.py -- the 32 MiB FAT32 stick the USB tests read from.

tools/usb_hotplug.py builds this as a side effect of running; `make
test-usb-all` needs it without running that, so the one line that makes it
lives here and both call it. Same file, same marker, one definition."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfat32 import build_fat32

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STICK = os.path.join(ROOT, "build", "usbstick.img")
MARKER = "HOTPLUG.TXT"

if __name__ == "__main__":
    build_fat32(STICK, 32, {MARKER: b"plugged in while it was running\n"},
                label="EMBSTICK")
    print("usbstick: %s (32 MiB FAT32, holding %s)"
          % (os.path.relpath(STICK, ROOT), MARKER))
