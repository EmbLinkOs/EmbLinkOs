#!/usr/bin/env python3
"""Hold shell/prompt's engine to Powerlevel10k itself, cell for cell.

tests/prompt/golden/<config>.json holds what Powerlevel10k drew under tmux at
each step of short sessions at three widths (tools/record_prompts.py): the screen with its
attributes, the cursor, and the state the prompt was drawn from. For every
step this renders the same state with the engine (build/prompt_dump, from
tests/prompt/prompt_dump.c) and compares:

    the prompt        its rows, ending at the cursor's row; the right prompt
                      ZLE_RPROMPT_INDENT (1) cells from the right edge
    the transient     the row the last command was typed on: the previous
                      step's transient prompt and the command
    the empty line    PROMPT_ADD_NEWLINE's, above the prompt

A cell is its character, foreground and bold when it shows a character, and
its background, underline and inverse always.

    python3 tools/golden_prompt.py                 # every configuration
    python3 tools/golden_prompt.py user lean -v    # some, every mismatch shown
"""
import json, os, re, subprocess, sys, tempfile, unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / 'tests' / 'prompt' / 'golden'
DUMP = Path(os.environ.get('PROMPT_DUMP', ROOT / 'build' / 'prompt_dump'))
SGR = re.compile(r'\x1b\[([0-9;:]*)m')
IND = 1  # ZLE_RPROMPT_INDENT


def width(ch):
    if unicodedata.combining(ch) or ch in '​‍️':
        return 0
    return 2 if unicodedata.east_asian_width(ch) in ('W', 'F') else 1


class Pen:
    def __init__(self):
        self.reset()

    def reset(self):
        self.fg = self.bg = None
        self.bold = self.underline = self.inverse = False

    def apply(self, params):
        p = [int(x) if x else 0 for x in re.split('[;:]', params)] if params else [0]
        i = 0
        while i < len(p):
            n = p[i]
            if n == 0: self.reset()
            elif n == 1: self.bold = True
            elif n == 22: self.bold = False
            elif n == 4: self.underline = True
            elif n == 24: self.underline = False
            elif n == 7: self.inverse = True
            elif n == 27: self.inverse = False
            elif 30 <= n <= 37: self.fg = n - 30
            elif 90 <= n <= 97: self.fg = n - 90 + 8
            elif n == 39: self.fg = None
            elif 40 <= n <= 47: self.bg = n - 40
            elif 100 <= n <= 107: self.bg = n - 100 + 8
            elif n == 49: self.bg = None
            elif n in (38, 48) and i + 1 < len(p):
                if p[i + 1] == 5 and i + 2 < len(p):
                    c, i = p[i + 2], i + 2
                elif p[i + 1] == 2 and i + 4 < len(p):
                    c, i = '#%02x%02x%02x' % tuple(p[i + 2:i + 5]), i + 4
                else:
                    c = None
                if n == 38: self.fg = c
                else: self.bg = c
            i += 1


def cells(text, cols, row=None, at=0, pen=None):
    """`text` (SGR and characters) drawn into `row` from column `at`."""
    row = row if row is not None else [(' ', None, None, False, False, False)] * cols
    row = list(row)
    pen = pen or Pen()
    x, i = at, 0
    while i < len(text):
        m = SGR.match(text, i)
        if m:
            pen.apply(m.group(1))
            i = m.end()
            continue
        if text[i] == '\x1b':          # another escape: none expected in a prompt
            i += 1
            continue
        ch = text[i]
        i += 1
        w = width(ch)
        if w == 0:
            continue
        if x < cols:
            row[x] = (ch, pen.fg, pen.bg, pen.bold, pen.underline, pen.inverse)
            if w == 2 and x + 1 < cols:
                row[x + 1] = ('', pen.fg, pen.bg, pen.bold, pen.underline, pen.inverse)
        x += w
    return row, x, pen


def wrapped(text, cols):
    """`text` as the terminal lays it out: rows of `cols` cells, a character
    that does not fit starting the next row. The rows, and the column after
    the last character."""
    blank = (' ', None, None, False, False, False)
    rows, pen, x, i = [[blank] * cols], Pen(), 0, 0
    while i < len(text):
        m = SGR.match(text, i)
        if m:
            pen.apply(m.group(1))
            i = m.end()
            continue
        ch, i = text[i], i + 1
        w = width(ch) if ch != '\x1b' else 0
        if w == 0:
            continue
        if x + w > cols:
            rows.append([blank] * cols)
            x = 0
        cell = (pen.fg, pen.bg, pen.bold, pen.underline, pen.inverse)
        rows[-1][x] = (ch, *cell)
        if w == 2:
            rows[-1][x + 1] = ('', *cell)
        x += w
    return rows, x


def text_width(s):
    return sum(width(c) for c in SGR.sub('', s))


def same(a, b):
    if a[0].strip() or b[0].strip():
        return a[0] == b[0] and a[1] == b[1] and a[3] == b[3] and a[2] == b[2]
    return a[2] == b[2] and a[4] == b[4] and a[5] == b[5]


def show(c):
    ch, fg, bg, bold, ul, inv = c
    attrs = ''.join(k for k, v in (('B', bold), ('U', ul), ('I', inv)) if v)
    return f'{ch!r} fg={fg} bg={bg}{" " + attrs if attrs else ""}'


def plain(row):
    return ''.join(c[0] for c in row).rstrip()


def fixture(golden):
    """The recording's home: the directories truncate_to_unique and the
    folder markers look at."""
    root = Path(tempfile.mkdtemp(prefix='emblink-prompt-fs-'))
    for d in golden['dirs']:
        (root / d).mkdir(parents=True, exist_ok=True)
    for f in golden.get('files', []):
        (root / f).touch()
    return root


def render(golden, cols, state, last_pwd, fsroot):
    home = golden['home']
    pwd = home + state['dir'][1:] if state['dir'].startswith('~') else state['dir']
    configs = golden['config'] if isinstance(golden['config'], list) else [golden['config']]
    args = [str(DUMP), *(str(ROOT / c) for c in configs), '--cols', str(cols), '--home', home, '--pwd', pwd,
            '--fsroot', str(fsroot), '--user', 'yves']
    if last_pwd:
        args += ['--last-pwd', last_pwd]
    if state.get('status') is not None:
        args += ['--status', str(state['status'])]
    if state.get('duration_ms') is not None:
        args += ['--duration', str(state['duration_ms'] / 1000)]
    if state.get('jobs'):
        args += ['--jobs', str(state['jobs'])]
    if state.get('time'):
        args += ['--time', '%d:%d:%d' % tuple(state['time'])]
    if state.get('git'):
        args += ['--git', ','.join(f'{k}={v}' for k, v in state['git'].items())]
    out = subprocess.run(args, capture_output=True, text=True, check=True).stdout
    return json.loads(out), pwd


def compare(label, want, got, cols, problems, verbose):
    bad = [x for x in range(cols) if not same(want[x], got[x])]
    if not bad:
        return True
    problems.append(f'{label}: {len(bad)} cells differ, first at column {bad[0]}')
    problems.append(f'    want: {plain(want)!r}')
    problems.append(f'    got:  {plain(got)!r}')
    for x in bad[:12 if verbose else 3]:
        problems.append(f'    col {x}: want {show(want[x])}, got {show(got[x])}')
    return False


def check(name, verbose):
    golden = json.loads((GOLDEN / f'{name}.json').read_text())
    fsroot = fixture(golden)
    problems, passed, total, ignored = [], 0, 0, ''
    for sess in golden['sessions']:
        cols, label = sess['cols'], f'{name}/{sess["name"]}'
        prev, last_pwd = None, None
        for step in sess['steps']:
            where = f'{label}/{step["step"]}'
            p, pwd = render(golden, cols, step['state'], last_pwd, fsroot)
            if not p['imported']:
                problems.append(f'{name}: import failed: {p["error"]}')
                break
            ignored = p['ignored']
            screen, pen = [], Pen()
            for r in step['screen'][:sess['rows']]:    # tmux carries attributes from row to row
                row, _, pen = cells(r, cols, None, 0, pen)
                screen.append(row)
            cx, cy = step['cursor']
            lines = p['left'].split('\n')
            drawn = [wrapped(line, cols) for line in lines]
            top = cy - sum(len(rows) for rows, _ in drawn) + 1
            ok, y = True, top
            for i, (rows, x) in enumerate(drawn):
                if i == len(lines) - 1 and p['right']:
                    rows[-1], _, _ = cells(p['right'], cols, rows[-1], cols - IND - text_width(p['right']))
                if i == len(lines) - 1 and x != cx:
                    problems.append(f'{where}: cursor after the prompt at {x}, want {cx}')
                    ok = False
                for k, row in enumerate(rows):
                    part = f' row {k + 1}' if len(rows) > 1 else ''
                    ok &= compare(f'{where} prompt line {i + 1}{part}', screen[y], row, cols, problems, verbose)
                    y += 1
            if step['typed'] and not step.get('output'):
                for k in range(1, p['add_newline'] + 1):
                    ok &= compare(f'{where} empty line', screen[top - k], cells('', cols)[0], cols, problems, verbose)
            if prev and step['typed'] and prev['transient']:
                want_row = next((r for r in reversed(screen[:top]) if step['typed'] in plain(r)), None)
                if want_row is None:
                    problems.append(f'{where}: no row holds {step["typed"]!r}')
                    ok = False
                else:
                    row, x, pen = cells(prev['transient'], cols)
                    row, _, _ = cells(step['typed'], cols, row, x, Pen())
                    ok &= compare(f'{where} transient', want_row, row, cols, problems, verbose)
            total += 1
            passed += ok
            prev, last_pwd = p, pwd
    print(f'{name}: {passed}/{total} steps match' + (f' (ignored segments: {len(ignored.split())})' if ignored else ''))
    for line in problems:
        print('  ' + line)
    return passed == total and not problems


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    verbose = '-v' in sys.argv
    if not DUMP.exists():
        sys.exit(f'golden_prompt: {DUMP} missing (make prompt-test builds it)')
    names = args or sorted(p.stem for p in GOLDEN.glob('*.json'))
    results = [check(n, verbose) for n in names]
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
