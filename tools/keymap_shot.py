#!/usr/bin/env python3
"""keymap_shot.py -- does choosing a keyboard layout actually change the keys?

    python3 tools/keymap_shot.py

THE CLAIM, and the reason this exists. `sys_kbd_layout` tested the result of
copy_string_from_user against EMBK_OK -- which is 0, while that function
returns the string's LENGTH on success. Every non-empty layout name was
therefore rejected with -EFAULT, so nothing in userspace could change the
layout: Settings' AZERTY/Dvorak switch and the saved keymap the desktop applies
at login both failed silently, on an OS that ships three layouts and a pane to
pick them with.

`test keymap` did not catch it because it calls keyboard_set_layout() DIRECTLY
in the kernel. The only path a person can use had no coverage at all, which is
exactly how a bug lives for a long time. This is that coverage.

HOW IT JUDGES. QMP sends key codes by POSITION, so the same six keypresses
spell "qwerty" on a QWERTY layout and "azerty" on a French one -- which is what
those layouts are named after. The test types them before and after the switch
and requires the field to differ. Comparing the field against ITSELF avoids
having to recognise glyphs.
"""
import os, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A
import shell_shot as S

SCREEN_W, SCREEN_H = 1024, 768
SETTINGS_SLOT, SLOTS = 2, 5
KEYBOARD_ROW = (160, 163)          # the sidebar row, measured off a screendump
AZERTY_SEG   = (522, 235)          # the "AZERTY (French)" segment
FIELD        = (615, 339)          # the "Try it" field
BAND         = (330, 352)          # its text band
FIELD_X0, FIELD_X1 = 340, 878      # stops before the scroll gutter


def type_text(q, text):
    for ch in text:
        q.cmd("input-send-event", events=[{"type": "key", "data": {"down": True,
              "key": {"type": "qcode", "data": ch}}}])
        time.sleep(0.05)
        q.cmd("input-send-event", events=[{"type": "key", "data": {"down": False,
              "key": {"type": "qcode", "data": ch}}}])
        time.sleep(0.08)


def ink(path):
    w, h, px = S.ppm_pixels(path)
    y0, y1 = BAND
    cols = {}
    for x in range(FIELD_X0, min(FIELD_X1, w)):
        base = None
        for y in range(y0, min(y1, h)):
            o = (y * w + x) * 3
            v = (px[o], px[o+1], px[o+2])
            if base is None or sum(v) < sum(base):
                base = v
        best = 0
        for y in range(y0, min(y1, h)):
            o = (y * w + x) * 3
            d = abs(px[o]-base[0]) + abs(px[o+1]-base[1]) + abs(px[o+2]-base[2])
            best = max(best, d)
        cols[x] = best > 60
    return cols


def differs(a, b, upto):
    n = d = 0
    for x in range(FIELD_X0, upto):
        if x in a and x in b:
            n += 1
            if a[x] != b[x]:
                d += 1
    return d / float(n or 1)


def last_ink(c):
    xs = [x for x, on in c.items() if on]
    return max(xs) if xs else FIELD_X0


def main():
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("keymap_shot: another qemu-system is running; refusing to start a second one")
        return 2

    p, ser, qmp = A.boot("keymap", os.path.join(A.BUILD, "shot-keymap.img"))
    try:
        s = A.connect(ser)
        q = A.Qmp(qmp)
        log = open(os.path.join(A.BUILD, "keymap-serial.log"), "wb")

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
        while time.time() - t0 < 220:
            if b"first frame presented" in open(log.name, "rb").read():
                break
            time.sleep(1)
        time.sleep(8)

        S.move(q, S.slot_center(SETTINGS_SLOT, SLOTS), S.dock_center_y())
        S.click(q)
        time.sleep(14)
        S.move(q, *KEYBOARD_ROW); S.click(q)
        time.sleep(3)

        # QWERTY first: these six keys spell what the layout is named after.
        S.move(q, *FIELD); S.click(q)
        time.sleep(1.5)
        type_text(q, "qwerty")
        time.sleep(1)
        S.move(q, SCREEN_W / 2.0, 200); time.sleep(1.5)
        before = os.path.join(A.BUILD, "keymap-qwerty.ppm"); q.screendump(before)

        # Switch to AZERTY, clear the field, type the same six keys again.
        S.move(q, *AZERTY_SEG); S.click(q)
        time.sleep(3)
        S.move(q, *FIELD); S.click(q)
        time.sleep(1.5)
        for _ in range(8):
            type_text(q, "backspace")
        type_text(q, "qwerty")
        time.sleep(1)
        S.move(q, SCREEN_W / 2.0, 200); time.sleep(1.5)
        after = os.path.join(A.BUILD, "keymap-azerty.ppm"); q.screendump(after)

        a, b = ink(before), ink(after)
        upto = max(last_ink(a), last_ink(b)) + 4
        changed = differs(a, b, upto)
        print("keymap_shot: the same six keys drew %.0f%% different ink after "
              "switching to AZERTY" % (changed * 100))

        fails = []
        if last_ink(a) <= FIELD_X0 + 2:
            fails.append("nothing was typed at all")
        if changed < 0.10:
            fails.append("the layout switch changed nothing -- the same keys still "
                         "produce the same letters")
        for f in fails:
            print("keymap_shot: FAIL %s" % f)
        print("keymap_shot: %s" % ("OK" if not fails else "FAILED"))
        return 1 if fails else 0
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()


if __name__ == "__main__":
    sys.exit(main())
