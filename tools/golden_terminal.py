#!/usr/bin/env python3
"""Replay the golden terminal corpus into EmbLink's screen model and compare.

tests/tty/golden/<case>/ holds what a real program wrote (stream.bin) and what
tmux showed for it (screen.txt, meta.json); tools/record_golden.py made them.
Each stream is fed to the model through tests/tty/vt_dump.c, and the shown
screen is compared with tmux's cell by cell: text, colours (palette indices
resolved through the model's own palette, so the comparison is about which
colour a program asked for, not how a theme paints it), attributes, cursor and
which screen is up.

tests/tty/golden/xfail.txt names the cases the model is known to get wrong,
each with the reason. It only shrinks: a listed case that now passes fails the
run until it is taken off the list, and an unlisted case that fails fails it.

    python3 tools/golden_terminal.py build/vt_dump [case ...] [-v]
"""
import json, re, subprocess, sys, unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / 'tests' / 'tty' / 'golden'


def width(ch):
    if unicodedata.combining(ch) or unicodedata.category(ch) in ('Me', 'Mn', 'Cf'):
        return 0
    if 0xfe00 <= ord(ch) <= 0xfe0f:
        return 0
    return 2 if unicodedata.east_asian_width(ch) in ('W', 'F') else 1


def colour(spec, palette):
    if spec is None:
        return -1
    kind, value = spec
    return palette[value] if kind == 'p' else value


def sgr(params, st):
    """Apply one SGR parameter list (as tmux's capture writes it) to `st`."""
    items = params.split(';') if params else ['0']
    i = 0
    while i < len(items):
        p = items[i]
        sub = p.split(':')
        head = sub[0]
        if p in ('', '0'):
            st.update(fg=None, bg=None, ul=None, attrs=set(), ulstyle=0)
        elif p == '5:3':                       # tmux writes overline (53) like this
            st['attrs'].add('O')
        elif head == '4' and len(sub) > 1:
            st['ulstyle'] = int(sub[1] or 0)
        elif head in ('38', '48', '58'):
            key = {'38': 'fg', '48': 'bg', '58': 'ul'}[head]
            if len(sub) > 1:                   # colon form: 38:5:n / 38:2::r:g:b
                vals = [v for v in sub[1:]]
                if vals[0] == '5':
                    st[key] = ('p', int(vals[1]))
                elif vals[0] == '2':
                    rgb = [int(v or 0) for v in vals[-3:]]
                    st[key] = ('rgb', rgb[0] << 16 | rgb[1] << 8 | rgb[2])
            elif items[i + 1] == '5':
                st[key] = ('p', int(items[i + 2])); i += 2
            elif items[i + 1] == '2':
                r, g, b = (int(x) for x in items[i + 2:i + 5])
                st[key] = ('rgb', r << 16 | g << 8 | b); i += 4
        else:
            n = int(head)
            letters = {1: 'B', 2: 'D', 3: 'I', 5: 'K', 7: 'R', 8: 'H', 9: 'S', 53: 'O'}
            if n in letters:
                st['attrs'].add(letters[n])
            elif n == 4:
                st['ulstyle'] = 1
            elif n == 21:
                st['ulstyle'] = 2
            elif n == 22:
                st['attrs'] -= {'B', 'D'}
            elif n == 23:
                st['attrs'].discard('I')
            elif n == 24:
                st['ulstyle'] = 0
            elif n == 25:
                st['attrs'].discard('K')
            elif n == 27:
                st['attrs'].discard('R')
            elif n == 28:
                st['attrs'].discard('H')
            elif n == 29:
                st['attrs'].discard('S')
            elif n == 55:
                st['attrs'].discard('O')
            elif n == 59:
                st['ul'] = None
            elif 30 <= n <= 37:
                st['fg'] = ('p', n - 30)
            elif 90 <= n <= 97:
                st['fg'] = ('p', n - 82)
            elif n == 39:
                st['fg'] = None
            elif 40 <= n <= 47:
                st['bg'] = ('p', n - 40)
            elif 100 <= n <= 107:
                st['bg'] = ('p', n - 92)
            elif n == 49:
                st['bg'] = None
        i += 1


def attrs_of(st, palette):
    """Attribute letters in vt_dump's order: B D I U<style> R H S K O, then
    C<rrggbb> for an underline's own colour."""
    out = ''.join(c for c in 'BDI' if c in st['attrs'])
    if st['ulstyle']:
        out += f"U{st['ulstyle']}"
    out += ''.join(c for c in 'RHSKO' if c in st['attrs'])
    if st['ul'] is not None and st['ulstyle']:
        out += 'C%06x' % colour(st['ul'], palette)
    return out


def reference(case, meta, palette):
    """tmux's capture, as rows of [text, fg, bg, attrs, width] / None."""
    st = dict(fg=None, bg=None, ul=None, attrs=set(), ulstyle=0)
    rows = []
    for line in (GOLDEN / case / 'screen.txt').read_text(encoding='utf-8').split('\n')[:meta['rows']]:
        cells = []
        for m in re.finditer(r'\x1b\[([0-9;:]*)m|\x1b\]8;[^\x07\x1b]*(?:\x07|\x1b\\)|(.)', line, re.S):
            if m.group(1) is not None:
                sgr(m.group(1), st)
                continue
            ch = m.group(2)
            if ch is None:                     # a hyperlink mark: no cell
                continue
            w = width(ch)
            if w == 0 and cells:
                prev = next(c for c in reversed(cells) if c is not None)
                prev[0] += ch
                continue
            cells.append([ch, colour(st['fg'], palette), colour(st['bg'], palette), attrs_of(st, palette), w])
            if w == 2:
                cells.append(None)
        while len(cells) < meta['cols']:
            cells.append([' ', -1, -1, '', 1])
        rows.append(cells[:meta['cols']])
    while len(rows) < meta['rows']:
        rows.append([[' ', -1, -1, '', 1] for _ in range(meta['cols'])])
    return rows


def row_text(cells):
    return ''.join(c[0] for c in cells if c is not None).rstrip()


def compare(case, dump, verbose):
    meta = json.loads((GOLDEN / case / 'meta.json').read_text())
    got = json.loads(subprocess.run([dump, str(meta['cols']), str(meta['rows']), str(GOLDEN / case / 'stream.bin')],
                                    capture_output=True, check=True).stdout)
    want = reference(case, meta, got['palette'])
    problems = []
    if bool(got['alternate']) != bool(meta['alternate']):
        problems.append(f"screen: model shows {'alternate' if got['alternate'] else 'primary'}, "
                        f"tmux {'alternate' if meta['alternate'] else 'primary'}")
    if got['cursor'] != [meta['cursor_x'], meta['cursor_y']]:
        problems.append(f"cursor: model {tuple(got['cursor'])}, tmux {(meta['cursor_x'], meta['cursor_y'])}")
    if bool(got['cursor_visible']) != bool(meta['cursor_visible']):
        problems.append(f"cursor visibility: model {got['cursor_visible']}, tmux {meta['cursor_visible']}")
    if 'title' in meta and got['title'] != meta['title']:
        problems.append(f"title: model {got['title']!r}, tmux {meta['title']!r}")
    for y, (mine, theirs) in enumerate(zip(got['cells'], want)):
        if row_text(mine) != row_text(theirs):
            problems.append(f'row {y} text:\n    model {row_text(mine)!r}\n    tmux  {row_text(theirs)!r}')
            continue
        for x, (a, b) in enumerate(zip(mine, theirs)):
            if (a is None) != (b is None):
                problems.append(f'row {y} col {x}: width differs')
                break
            if a is not None and a[1:4] != b[1:4]:
                problems.append(f'row {y} col {x} {a[0]!r}: model fg {a[1]:#x} bg {a[2]:#x} "{a[3]}", '
                                f'tmux fg {b[1]:#x} bg {b[2]:#x} "{b[3]}"')
                break
    return problems


def main():
    args = [a for a in sys.argv[1:] if a != '-v']
    verbose = '-v' in sys.argv
    if not args:
        sys.exit(__doc__)
    dump, wanted = args[0], args[1:]
    xfail = {}
    for line in (GOLDEN / 'xfail.txt').read_text().splitlines():
        if line.strip() and not line.startswith('#'):
            name, _, why = line.partition(' ')
            xfail[name] = why.strip()
    cases = wanted or sorted(p.name for p in GOLDEN.iterdir() if (p / 'stream.bin').exists())
    bad = 0
    for case in cases:
        problems = compare(case, dump, verbose)
        if problems and case in xfail:
            print(f'xfail {case}: {xfail[case]} ({len(problems)} difference(s))')
            if verbose:
                print('\n'.join('  ' + p for p in problems))
        elif problems:
            bad += 1
            print(f'FAIL  {case}:')
            print('\n'.join('  ' + p for p in problems[:12 if not verbose else None]))
        elif case in xfail:
            bad += 1
            print(f'XPASS {case}: it passes now -- take it off tests/tty/golden/xfail.txt')
        else:
            print(f'ok    {case}')
    print(f'golden terminal: {len(cases) - bad}/{len(cases)} as expected')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
