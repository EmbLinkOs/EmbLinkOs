#!/usr/bin/env python3
"""caret_shot.py -- can you edit the MIDDLE of a text field on the real machine?

    python3 tools/caret_shot.py

THE CLAIM UNDER TEST. Until the caret existed, a text field in this system
could only be appended to: typing went on the end, backspace came off the end,
and the thing drawn as a caret was a box emitted after the whole string. Fixing
a typo three characters back meant deleting everything after it -- in every
filename box, every search box and every setting in the OS.

ui/kit/kit_test.c proves the EDITING on the host, exhaustively and in a second.
Two things it cannot reach, and they are the two this measures: that the caret
is DRAWN where the model says it is, and that a real keyboard and a real
pointer put it there. So this drives Settings' Keyboard pane -- whose "Try it"
field exists precisely to show what the keyboard produced -- with QMP key and
tablet events.

HOW IT JUDGES, without reading text off a screenshot. It compares INK COLUMNS:
for each x, whether any pixel in the field's text band differs from the field's
background. That turns "what does it say" into "where is there writing", which
is enough to tell the two behaviours apart:

  * type "world", then Home and "hello " --
    an INSERT AT THE FRONT changes the leftmost glyphs (w -> h) and pushes the
    ink further right. An APPEND, which is all the old field could do, would
    leave the leftmost glyphs alone. So the left edge MUST change.

  * click between two letters and type one --
    the ink LEFT of the click must be untouched and the ink right of it must
    move. A caret that ignored the click and went to the end would leave the
    left unchanged AND the middle unchanged, and only grow the right end.
"""
import os, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A
import shell_shot as S

SCREEN_W, SCREEN_H = 1024, 768
SET_SLOT, SLOTS = 2, 5                 # the Settings tile of the default dock

# Measured off build/caret-pane.ppm: the sidebar row that opens the Keyboard
# pane, and the "Try it" field's well.
KEYBOARD_ROW = (160, 163)
FIELD        = (615, 339)
FIELD_X0     = 340                     # the well's left padding: where text starts
FIELD_X1     = 900
BAND         = (330, 350)              # y range of the field's text
CLICK_X      = 365                     # inside "hello world", after the "hell"

# QMP qcode names for the characters this test types. Letters are their own
# name but must be LOWERCASE -- "Z" is not a key name, and sending it types
# nothing at all while looking exactly like a caret that ignored the click.
NAMES = {"/": "slash", ".": "dot", "\n": "ret", " ": "spc", "-": "minus",
         "\x02": "home", "\x05": "end", "\x11": "left", "\x12": "right"}


def type_text(q, text):
    for ch in text:
        k = NAMES.get(ch, ch)
        if len(k) == 1 and k.isalpha():
            k = k.lower()
        q.cmd("input-send-event", events=[
            {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": k}}}])
        time.sleep(0.05)
        q.cmd("input-send-event", events=[
            {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": k}}}])
        time.sleep(0.09)


def ink_columns(path):
    """For each x in the field, is there writing there?

    The well is a flat fill, so "writing" is simply "some pixel in this column
    differs from the column's own darkest tone by more than the fill varies".
    Comparing against the field's own background rather than a fixed colour
    keeps this working when the theme or accent changes."""
    w, h, px = S.ppm_pixels(path)
    y0, y1 = BAND
    bg = None
    cols = {}
    for x in range(FIELD_X0, min(FIELD_X1, w)):
        best = 0
        base = None
        for y in range(y0, min(y1, h)):
            o = (y * w + x) * 3
            v = (px[o], px[o + 1], px[o + 2])
            if base is None or sum(v) < sum(base):
                base = v
        for y in range(y0, min(y1, h)):
            o = (y * w + x) * 3
            d = abs(px[o] - base[0]) + abs(px[o + 1] - base[1]) + abs(px[o + 2] - base[2])
            if d > best:
                best = d
        cols[x] = best > 60
        if bg is None:
            bg = base
    return cols


def ink_differs(a, b, x0, x1):
    """Fraction of columns in [x0,x1) whose ink state changed.

    x1 SHOULD BE THE END OF THE WRITING, not the end of the field. The first
    version of this compared out to the field's right edge, where several
    hundred columns are blank in both shots and identical by definition -- so a
    perfectly good one-character insertion, which moves every column after it,
    scored 5% and read as a failure. The blank tail is not evidence."""
    n = diff = 0
    for x in range(x0, x1):
        if x in a and x in b:
            n += 1
            if a[x] != b[x]:
                diff += 1
    return diff / float(n or 1)


def last_ink(cols):
    xs = [x for x, on in cols.items() if on]
    return max(xs) if xs else 0


def main():
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("caret_shot: another qemu-system is running; refusing to start a second one")
        return 2

    p, ser, qmp = A.boot("caret", os.path.join(A.BUILD, "shot-caret.img"))
    try:
        s = A.connect(ser)
        q = A.Qmp(qmp)
        log = open(os.path.join(A.BUILD, "caret-serial.log"), "wb")

        def drain():
            while True:
                try:
                    b = s.recv(4096)
                except OSError:
                    return
                if not b:
                    return
                log.write(b); log.flush()
        threading.Thread(target=drain, daemon=True).start()

        t0 = time.time()
        while time.time() - t0 < 200:
            if b"first frame presented" in open(log.name, "rb").read():
                break
            time.sleep(1)
        time.sleep(8)

        S.move(q, S.slot_center(SET_SLOT, SLOTS), S.dock_center_y())
        S.click(q)
        time.sleep(14)
        S.move(q, *KEYBOARD_ROW); S.click(q)
        time.sleep(2.5)

        S.move(q, *FIELD); S.click(q)
        time.sleep(1.5)
        type_text(q, "world")
        time.sleep(1)
        S.move(q, SCREEN_W / 2.0, 200); time.sleep(1.5)
        a = os.path.join(A.BUILD, "caret-typed.ppm"); q.screendump(a)

        type_text(q, "\x02")                      # Home
        type_text(q, "hello ")
        time.sleep(1)
        S.move(q, SCREEN_W / 2.0, 200); time.sleep(1.5)
        b = os.path.join(A.BUILD, "caret-inserted.ppm"); q.screendump(b)

        S.move(q, CLICK_X, FIELD[1]); S.click(q)
        time.sleep(1.5)
        type_text(q, "z")
        time.sleep(1)
        S.move(q, SCREEN_W / 2.0, 200); time.sleep(1.5)
        c = os.path.join(A.BUILD, "caret-clicked.ppm"); q.screendump(c)

        ca, cb, cc = ink_columns(a), ink_columns(b), ink_columns(c)
        left_changed  = ink_differs(ca, cb, FIELD_X0, FIELD_X0 + 40)
        grew          = last_ink(cb) - last_ink(ca)
        written       = max(last_ink(cb), last_ink(cc)) + 4
        before_click  = ink_differs(cb, cc, FIELD_X0, CLICK_X - 6)
        after_click   = ink_differs(cb, cc, CLICK_X + 6, written)

        print("caret_shot: typed ink ends at x=%d, after the front-insert x=%d"
              % (last_ink(ca), last_ink(cb)))
        print("caret_shot: front-insert changed %.0f%% of the leftmost columns"
              % (left_changed * 100))
        print("caret_shot: the click-and-type changed %.0f%% left of the caret, "
              "%.0f%% right of it" % (before_click * 100, after_click * 100))

        fails = []
        if last_ink(ca) == 0:
            fails.append("nothing was typed into the field at all")
        if left_changed < 0.15:
            fails.append("Home + typing left the FRONT of the text alone -- it appended")
        if grew <= 0:
            fails.append("the text did not grow")
        if before_click > 0.10:
            fails.append("typing after a click disturbed the text LEFT of the caret")
        if after_click < 0.15:
            fails.append("typing after a click did not change the text right of it -- "
                         "the click did not place the caret")

        for f in fails:
            print("caret_shot: FAIL %s" % f)
        print("caret_shot: %s" % ("OK" if not fails else "FAILED"))
        return 1 if fails else 0
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()


if __name__ == "__main__":
    sys.exit(main())
