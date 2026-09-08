#!/usr/bin/env python3
"""mp3_check.py -- our MP3 decoder against a reference, sample by sample.

An MP3 decoder's failure mode is not silence or a crash. A wrong Huffman
table, a sign error in the IMDCT, a scalefactor exponent off by a factor of
two: every one of them produces confident, plausible audio. Listening cannot
separate "correct" from "close", and neither can peak level or duration -- the
first version of this decoder matched the reference's peak EXACTLY while
getting the waveform wrong would have looked identical on that measure.

So the reference decoder (ffmpeg) decodes the same file and the two waveforms
are compared point for point.

ALIGNMENT is the one subtlety. Layer III has an inherent decoder delay (529
samples) and encoders add padding, and ffmpeg TRIMS both using the LAME/Xing
gapless tags while we emit everything we decode. So the streams start at
different points in the music, and comparing them as-is reports total garbage
for a perfectly correct decoder. The offset is found by search rather than
assumed, then reported -- if it is not a plausible small number, that is
itself the finding.

  python3 tools/mp3_check.py build/mp3ref/tone440.mp3
"""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_pcm(path):
    with open(path, "rb") as f:
        raw = f.read()
    n = len(raw) // 2
    return struct.unpack("<%dh" % n, raw[:n * 2])


def rms(xs):
    if not xs:
        return 0.0
    return (sum(float(x) * x for x in xs) / len(xs)) ** 0.5


def best_offset(ours, ref, channels, limit=4000, window=30000):
    """Where in `ours` does `ref` begin?

    ALIGN ON THE ONSET, and step by one.

    The first version skipped 5000 samples into the reference "to land on
    signal", then searched coarsely and refined locally. On a sine it chose an
    offset 4 periods wrong and reported 6.8% error for a decoder that is
    actually accurate to 0.004%. Two mistakes, both worth keeping written down:

      * A periodic waveform matches itself at EVERY period, so steady state
        cannot identify the alignment -- 440 Hz at 44100 gives a false minimum
        every 100 samples. The only unambiguous feature is where the sound
        STARTS, and skipping the lead-in threw away precisely that.
      * Coarse-then-local-refine finds a local minimum. With a comb of near
        ties, the one it lands in is arbitrary.

    So: window from sample 0 (the onset is in it), and sweep every offset. It
    is a decimated compare over a few thousand candidates -- cheap, and it
    cannot miss the global minimum."""
    a = ours[0::channels]
    b = ref[0::channels]
    win = min(window, len(b), max(0, len(a) - limit))
    if win <= 0:
        return 0, 0.0

    target = b[:win]
    tr = rms(target) or 1.0

    best, best_err = 0, None
    for off in range(0, limit):
        if off + win > len(a):
            break
        err = rms([a[off + i] - target[i] for i in range(0, win, 32)])
        if best_err is None or err < best_err:
            best_err, best = err, off
    return best, best_err / tr


def main():
    mp3 = sys.argv[1] if len(sys.argv) > 1 else "build/mp3ref/tone440.mp3"
    base = os.path.splitext(mp3)[0]
    ours_p, ref_p = base + ".ours.pcm", base + ".ref.pcm"

    if subprocess.call([os.path.join(ROOT, "build", "mp3dec"), mp3, ours_p],
                       stderr=subprocess.DEVNULL) != 0:
        print("  %s: FAIL -- our decoder produced nothing" % os.path.basename(mp3))
        return 1
    subprocess.check_call(["ffmpeg", "-hide_banner", "-loglevel", "error",
                           "-i", mp3, "-f", "s16le", "-acodec", "pcm_s16le",
                           ref_p, "-y"])

    ours, ref = read_pcm(ours_p), read_pcm(ref_p)
    # Channel count from the file sizes is unreliable; ask ffprobe once.
    ch = int(subprocess.check_output(
        ["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
         "stream=channels", "-of", "csv=p=0", mp3]).strip())

    off, _ = best_offset(ours, ref, ch)

    # Compare the overlapping span, one channel at a time.
    errs, refs, worst = [], [], 0
    for c in range(ch):
        a = ours[off * ch + c::ch]
        b = ref[c::ch]
        n = min(len(a), len(b))
        # Ignore the last frame: we emit padding the reference trimmed.
        n = max(0, n - 1152)
        if n <= 0:
            continue
        d = [a[i] - b[i] for i in range(n)]
        errs.append(rms(d))
        refs.append(rms(b[:n]))
        worst = max(worst, max(abs(x) for x in d))

    e, r = rms(errs), rms(refs)
    pct = 100.0 * e / r if r else 0.0
    print("  %-16s offset %4d  err %.2f%% of signal  worst sample %d"
          % (os.path.basename(mp3), off, pct, worst))

    # 1% RMS. Two correct decoders differ a little -- rounding, float order,
    # ours emits samples ffmpeg trims -- but a real bug is not a rounding
    # difference: a wrong table or sign error lands in the tens of percent.
    if pct > 1.0:
        print("     FAIL: %.2f%% error is a decoding bug, not rounding" % pct)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
