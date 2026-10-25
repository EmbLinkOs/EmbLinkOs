#!/usr/bin/env python3
"""ppm2png.py -- a QEMU screendump (binary PPM, P6) to a lossless PNG, pixel
for pixel: the docs gallery's captures are guest pixels, never re-rendered.

    python3 tools/ppm2png.py in.ppm out.png
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkpictures import write_png  # noqa: E402


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b"P6" or int(fields[3]) != 255:
        raise ValueError("%s: not an 8-bit binary PPM" % path)
    w, h = int(fields[1]), int(fields[2])
    pixels = data[pos + 1:pos + 1 + w * h * 3]
    if len(pixels) != w * h * 3:
        raise ValueError("%s: truncated" % path)
    return w, h, pixels


def convert(src, dst):
    w, h, px = read_ppm(src)
    write_png(dst, w, h, [px[y * w * 3:(y + 1) * w * 3] for y in range(h)])
    return w, h


if __name__ == "__main__":
    print("%dx%d" % convert(sys.argv[1], sys.argv[2]))
