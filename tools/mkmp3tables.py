#!/usr/bin/env python3
"""mkmp3tables.py -- recover the ISO 11172-3 constants into OUR representation.

WHAT THIS IS AND IS NOT.

A Layer III decoder cannot be written without the constants the standard
defines: 32 Huffman tables, the scalefactor band boundaries for each sample
rate, the 512-point synthesis window, the alias-reduction coefficients. These
are DATA, not design -- every independent decoder contains identical copies,
the same way our JPEG decoder contains the tables from ITU T.81. They cannot
be derived; they were chosen by a committee and written down.

They are also several thousand numbers, which is more than can be reproduced
accurately from memory, so they are extracted here from a public-domain
reference implementation (PDMP3, Unlicense) rather than retyped and hoped for.

The extraction is deliberately NOT a copy. PDMP3 stores each Huffman table as
a binary decode tree in 16-bit words -- (skip_if_0 << 8) | skip_if_1, with
leaves as 0x00XY -- which is PDMP3's design decision, not the standard's data.
This script WALKS those trees to recover what the standard actually tabulates:
for each symbol, its code, that code's length, and the (x, y) pair it means.
What comes out is the ISO table; how our decoder then stores and searches it is
our own business, and mp3_huffman.c makes a different choice.

So: their file is an oracle for the numbers, not a source of the program. The
generated file is committed, so the build never needs the network, and this
script exists to document where the numbers came from and to let anyone
reproduce them.

  python3 tools/mkmp3tables.py /path/to/pdmp3.c > user/audio/mp3/tables.c
"""
import re
import sys


def read_source(path):
    with open(path, "r", errors="replace") as f:
        # Comments are stripped IMMEDIATELY, before anything counts a brace.
        # The reference file writes its table boundaries as commented-out
        # declarations -- `//},g_huffman_table_2[17] = {` -- so a brace matcher
        # run over the raw text pairs a real brace with a commented one and
        # never terminates the array.
        return strip_comments(f.read())


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    s = re.sub(r"//[^\n]*", " ", s)
    return s


def grab_array(src, decl):
    """Text between `decl ... = {` and the matching `};`."""
    i = src.index(decl)
    i = src.index("{", i)
    depth, j = 0, i
    while j < len(src):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i + 1:j]
        j += 1
    raise ValueError("unterminated array for %s" % decl)


# --- the Huffman trees ---------------------------------------------------- #

def parse_tree_words(src):
    body = grab_array(src, "g_huffman_table[]")
    return [int(t, 16) for t in re.findall(r"0x[0-9A-Fa-f]+", body)]


def parse_table_index(src):
    """[(offset, treelen, linbits)] for tables 0..33; offset None when unused."""
    body = grab_array(src, "g_huffman_main [34]")
    out = []
    for m in re.finditer(r"\{\s*(NULL|g_huffman_table\s*(?:\+\s*(\d+))?)\s*,"
                         r"\s*(\d+)\s*,\s*(\d+)\s*\}", body):
        if m.group(1) == "NULL":
            out.append((None, 0, 0))
        else:
            off = int(m.group(2)) if m.group(2) else 0
            out.append((off, int(m.group(3)), int(m.group(4))))
    if len(out) != 34:
        raise ValueError("expected 34 table entries, got %d" % len(out))
    return out


def hop(words, base, idx, right):
    """Follow one branch, honouring the CHAINED-SKIP escape.

    A skip byte is 8 bits, so it cannot address the 512-entry tables on its
    own. The encoding escapes: a value >= 250 is not the destination but a hop
    towards it, and you keep hopping while the byte you land on is also >= 250
    before taking one final step.

    Missing this is why tables 24-31 first came out with a Kraft sum of 0.875.
    Treating a 250+ escape as an ordinary offset lands on a node that still
    parses as a node, so the walk produced a plausible table that was quietly
    missing an eighth of its code space -- which is the entire reason the Kraft
    check is in this script rather than trusting the extraction."""
    while True:
        w = words[base + idx]
        skip = (w & 0xFF) if right else (w >> 8)
        idx += skip
        if skip < 250:
            return idx


def walk(words, base, length):
    """Recover (code, bits, x, y) for every leaf of one tree.

    A node is (skip_if_0 << 8) | skip_if_1; a leaf has a zero high byte and
    carries its value in the low byte's nibbles. Verified by hand against ISO
    table 1, whose codes are 1 -> (0,0), 01 -> (1,0), 001 -> (0,1),
    000 -> (1,1)."""
    out = []
    stack = [(0, 0, 0)]                       # (index, code so far, bits)
    while stack:
        idx, code, bits = stack.pop()
        if idx >= length or bits > 24:
            continue
        w = words[base + idx]
        if (w & 0xFF00) == 0:                 # leaf
            out.append((code, bits, (w >> 4) & 0xF, w & 0xF))
            continue
        stack.append((hop(words, base, idx, False), (code << 1) | 0, bits + 1))
        stack.append((hop(words, base, idx, True), (code << 1) | 1, bits + 1))
    return sorted(out, key=lambda e: (e[1], e[0]))


def check_kraft(entries, name):
    """A prefix code over a complete alphabet satisfies sum(2^-len) == 1.
    A transcription error almost always breaks this, so it is checked here
    rather than discovered as noise in the output."""
    total = sum(2.0 ** -bits for _code, bits, _x, _y in entries)
    return abs(total - 1.0) < 1e-9, total


# --- the plain tables ------------------------------------------------------ #

def parse_floats(src, decl, count):
    body = grab_array(src, decl)
    vals = [float(t) for t in re.findall(r"-?\d+\.\d+(?:[eE][-+]?\d+)?", body)]
    if len(vals) != count:
        raise ValueError("%s: expected %d floats, got %d" % (decl, count, len(vals)))
    return vals


def parse_sf_bands(src):
    body = grab_array(src, "g_sf_band_indices[3")
    groups = re.findall(r"\{([^{}]*)\}", body)
    if len(groups) != 6:
        raise ValueError("expected 3 rates x (long, short), got %d" % len(groups))
    out = []
    for i in range(3):
        lo = [int(t) for t in re.findall(r"\d+", groups[i * 2])]
        sh = [int(t) for t in re.findall(r"\d+", groups[i * 2 + 1])]
        out.append((lo, sh))
    return out


# How many symbols each ISO table has: the product of its (x, y) ranges, or 16
# for the two count1 tables that code quadruples. Checked rather than assumed,
# because the extraction is only as trustworthy as what it can verify.
EXPECTED = {1: 4, 2: 9, 3: 9, 5: 16, 6: 16, 7: 36, 8: 36, 9: 36,
            10: 64, 11: 64, 12: 64, 13: 256, 15: 256,
            32: 16, 33: 16}
for _t in range(16, 32):
    EXPECTED[_t] = 256


def fix_index(words, index):
    """Correct the reference's own table index where it is wrong.

    PDMP3 points table 33 at offset 2261, which lands inside the region tables
    24-31 share, on a word whose high byte is zero -- a LEAF. Decoding with it
    returns a value without consuming a single bit, so any frame using count1
    table B comes out wrong. The real data is the last 31 words of the array: a
    balanced depth-4 tree whose leaves are 0..15, which is exactly what the
    standard specifies for that table (a fixed 4-bit code).

    Recorded here rather than silently patched because it is the reason this
    script validates instead of trusting: the symbol-count check below is what
    turned "table 33: 1 code" into a bug report about somebody else's file."""
    out = list(index)
    tail = len(words) - 31
    if words[tail] == 0x1001:                 # the balanced root, as expected
        off, length, linbits = out[33]
        if off != tail:
            sys.stderr.write("  NOTE: reference indexes table 33 at %d (a leaf); "
                             "using %d, the real data\n" % (off, tail))
            out[33] = (tail, 31, linbits)
    return out


def emit(src):
    words = parse_tree_words(src)
    index = fix_index(words, parse_table_index(src))
    sfb = parse_sf_bands(src)
    synth = parse_floats(src, "g_synth_dtbl[512]", 512)

    p = print
    p("/* user/audio/mp3/tables.c -- GENERATED by tools/mkmp3tables.py.")
    p(" *")
    p(" * The constants ISO 11172-3 defines: Huffman codes, scalefactor band")
    p(" * boundaries, the synthesis window. DATA, not design -- every decoder")
    p(" * holds identical copies, as our JPEG decoder holds T.81's tables.")
    p(" * Recovered from a public-domain reference (PDMP3, Unlicense) by walking")
    p(" * its decode trees back into the codes the standard tabulates; the")
    p(" * script says more about why that is an extraction and not a copy.")
    p(" *")
    p(" * DO NOT EDIT. Re-run the script.")
    p(" */")
    p('#include "tables.h"')
    p("")

    # Huffman: one flat array of entries, plus a per-table span.
    p("/* Every code of every table, ordered by (length, code) so a decoder can")
    p(" * walk lengths upward and stop at the first match -- which is the whole")
    p(" * search, and needs no tree. */")
    p("const Mp3HuffEntry mp3_huff_entries[] = {")
    spans = []
    cursor = 0
    for t, (off, length, linbits) in enumerate(index):
        if off is None or length == 0:
            spans.append((0, 0, linbits))
            continue
        entries = walk(words, off, length)
        good, total = check_kraft(entries, "table %d" % t)
        if not good:
            raise ValueError("table %d: Kraft sum %.9f, expected 1.0 -- the "
                             "code is incomplete, so codes were lost walking it"
                             % (t, total))
        want = EXPECTED.get(t)
        if want is not None and len(entries) != want:
            raise ValueError("table %d: %d symbols, expected %d"
                             % (t, len(entries), want))
        p("    /* table %d: %d codes, linbits %d */" % (t, len(entries), linbits))
        for code, bits, x, y in entries:
            p("    { 0x%04X, %2d, %2d, %2d }," % (code, bits, x, y))
        spans.append((cursor, len(entries), linbits))
        cursor += len(entries)
    p("};")
    p("")

    p("/* Where each table's codes live in the array above, and its linbits --")
    p(" * the extra magnitude bits an escape value of 15 is followed by. */")
    p("const Mp3HuffTable mp3_huff_tables[34] = {")
    for t, (start, count, linbits) in enumerate(spans):
        p("    { %5d, %3d, %2d },   /* table %2d */" % (start, count, linbits, t))
    p("};")
    p("")

    names = ("44100", "48000", "32000")
    p("/* Scalefactor band boundaries, by sample rate. The bands are where the")
    p(" * standard puts the psychoacoustic model's resolution, so requantisation")
    p(" * and stereo processing both walk these rather than raw coefficients. */")
    p("const Mp3SfBands mp3_sf_bands[3] = {")
    for i, (lo, sh) in enumerate(sfb):
        p("    {   /* %s Hz */" % names[i])
        p("        { %s }," % ", ".join(str(v) for v in lo))
        p("        { %s }," % ", ".join(str(v) for v in sh))
        p("    },")
    p("};")
    p("")

    p("/* The polyphase synthesis window. 512 coefficients the standard simply")
    p(" * states; the filterbank that uses them is ours. */")
    p("const float mp3_synth_window[512] = {")
    for i in range(0, 512, 4):
        p("    " + " ".join("%14.10ff," % v for v in synth[i:i + 4]))
    p("};")

    sys.stderr.write("  %d huffman codes across %d tables, %d sf-band sets, "
                     "%d window points\n"
                     % (cursor, sum(1 for s in spans if s[1]), len(sfb), len(synth)))


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    emit(read_source(sys.argv[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
