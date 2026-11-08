#!/usr/bin/env python3
"""Hold `prompt configure` (shell/prompt/wizard.c) to Powerlevel10k's own
wizard, page for page and line for line.

tests/prompt/wizard/<path>.json holds what `p10k configure` drew under tmux
for a path of answers at each window size, the key pressed on each page, and
the configuration it wrote (tools/record_wizard.py). For every session this
runs build/wizard_dump with the same keys (and resizes), lays out what it
writes on a screen of that size, and compares the screen each key is pressed
on with the recorded page, cell for cell -- character, colours, bold -- and
the cursor. Instant prompt's page, which p10k asks and `prompt configure` does
not, is left out with its key.

Then the configuration, line for line. On purpose, the header differs in two
ways: its options leave out instant_prompt (not asked), and it says to type
`prompt configure`. The date line is not compared.

    python3 tools/golden_wizard.py              # every path
    python3 tools/golden_wizard.py user -v      # some, every mismatch shown
"""
import json, os, re, subprocess, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from golden_prompt import Pen, SGR, cells, same, show, width   # noqa: E402
from record_wizard import config_apply                          # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / 'tests' / 'prompt' / 'wizard'
DUMP = Path(os.environ.get('WIZARD_DUMP', ROOT / 'build' / 'wizard_dump'))
OSC = re.compile(r'\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)')
PAGE = '\x1b]7777;page\x07'
BLANK = (' ', None, None, False, False, False)


class Screen:
    """The terminal the wizard writes to: a main screen and an alternate one,
    the escapes the wizard uses, characters by their cell width."""

    def __init__(self, cols, rows):
        self.cols, self.rows = cols, rows
        self.main, self.alt = self.blank(), self.blank()
        self.on_alt, self.x, self.y, self.saved = False, 0, 0, (0, 0)
        self.pen = Pen()

    def blank(self):
        return [[BLANK] * self.cols for _ in range(self.rows)]

    @property
    def grid(self):
        return self.alt if self.on_alt else self.main

    def resize(self, cols, rows):
        def fit(g):
            g = [row[:cols] + [BLANK] * (cols - len(row)) for row in g]
            return (g + [[BLANK] * cols for _ in range(rows - len(g))])[-rows:] if len(g) < rows else g[:rows]
        self.main, self.alt = fit(self.main), fit(self.alt)
        self.cols, self.rows = cols, rows
        self.x, self.y = min(self.x, cols - 1), min(self.y, rows - 1)

    def newline(self):
        self.y += 1
        if self.y == self.rows:
            g = self.grid
            g.pop(0)
            g.append([BLANK] * self.cols)
            self.y -= 1

    def feed(self, text):
        i = 0
        while i < len(text):
            m = SGR.match(text, i)
            if m:
                self.pen.apply(m.group(1))
                i = m.end()
                continue
            m = OSC.match(text, i)
            if m:
                i = m.end()
                continue
            m = re.compile(r'\x1b\[(\??)([0-9;]*)([A-Za-z])').match(text, i)
            if m:
                private, args, final = m.groups()
                i = m.end()
                if private and final in 'hl' and args == '1049':
                    if final == 'h':
                        self.saved, self.on_alt, self.alt = (self.x, self.y), True, self.blank()
                    else:
                        self.on_alt, (self.x, self.y) = False, self.saved
                elif not private and final == 'H':
                    self.x = self.y = 0
                elif not private and final == 'J' and args == '2':
                    g = self.grid
                    for r in range(self.rows):
                        g[r] = [BLANK] * self.cols
                continue
            ch = text[i]
            i += 1
            if ch == '\r':
                self.x = 0
            elif ch == '\n':
                self.newline()
            elif ch >= ' ':
                w = width(ch)
                if w == 0:
                    continue
                if self.x + w > self.cols:
                    self.x = 0
                    self.newline()
                p = self.pen
                cell = (p.fg, p.bg, p.bold, p.underline, p.inverse)
                self.grid[self.y][self.x] = (ch, *cell)
                if w == 2 and self.x + 1 < self.cols:
                    self.grid[self.y][self.x + 1] = ('', *cell)
                self.x += w

    def snapshot(self):
        return [list(row) for row in self.grid], [self.x, self.y]


def recorded(lines, cols):
    """A recorded page as rows of cells (tmux's pen goes on from line to line)."""
    rows, pen = [], Pen()
    for line in lines:
        row, _, pen = cells(OSC.sub('', line), cols, pen=pen)
        rows.append(row)
    return rows


def run(golden, session):
    """Our pages for a recorded session, each as (rows, cursor), and the
    configuration, if the answers finished it."""
    steps = []
    for shot in session['shots']:
        if shot.get('zsh_only'):
            continue
        steps.append(f'{shot["resize"][0]}x{shot["resize"][1]}' if 'resize' in shot else shot['key'])
    flags = ([] if golden['force'] else ['--auto']) + (['--exists'] if golden['existing'] else [])
    with tempfile.TemporaryDirectory() as tmp:
        config = Path(tmp) / 'p10k.zsh'
        r = subprocess.run([str(DUMP), str(session['cols']), str(session['rows']), *flags, '--config', str(config), *steps],
                           capture_output=True, cwd=ROOT)
        if r.returncode == 2:
            raise RuntimeError(r.stderr.decode())
        text = config.read_text() if config.exists() else None
    out = r.stdout.decode('utf-8', 'replace').split(PAGE)
    screen, pages = Screen(session['cols'], session['rows']), []
    for chunk, step in zip(out, steps):
        screen.feed(chunk)
        pages.append(screen.snapshot())
        if 'x' in step and len(step) > 1:
            c, rr = map(int, step.split('x'))
            screen.resize(c, rr)
    return pages, text


def plain(row):
    return ''.join(c[0] for c in row).rstrip()


def compare_page(label, want_lines, want_cursor, got, cols, problems, verbose):
    rows, cursor = got
    want = recorded(want_lines, cols)
    bad = []
    for y in range(max(len(want), len(rows))):
        a = want[y] if y < len(want) else [BLANK] * cols
        b = rows[y] if y < len(rows) else [BLANK] * cols
        for x in range(cols):
            if not same(a[x], b[x]):
                bad.append((y, x, a[x], b[x]))
    if bad:
        y = bad[0][0]
        msg = [f'{label}: {len(bad)} cells differ, first at row {y}',
               f'    p10k: {plain(want[y] if y < len(want) else [])!r}',
               f'    ours: {plain(rows[y] if y < len(rows) else [])!r}']
        for (y, x, a, b) in bad[:12 if verbose else 3]:
            msg.append(f'    row {y} col {x}: p10k {show(a)} / ours {show(b)}')
        problems.append('\n'.join(msg))
    elif cursor != want_cursor:
        problems.append(f'{label}: cursor at {cursor}, p10k at {want_cursor}')


def wrap_options(options):
    """The header's options lines, as generate_config wraps them."""
    out, line = [], f'# Wizard options: {options[0]}'
    for opt in options[1:]:
        if len(line) + len(opt) > 85:
            out.append(line + ',')
            line = f'# {opt}'
        else:
            line += f', {opt}'
    return out + [line + '.']


def expected_config(golden):
    """p10k's configuration, but for what `prompt configure` writes on purpose."""
    lines = config_apply(golden['config']).split('\n')
    at = next(i for i, l in enumerate(lines) if l.startswith('# Wizard options: '))
    end = next(i for i in range(at, len(lines)) if lines[i].endswith('.'))
    words = ' '.join(l[2:] for l in lines[at:end + 1])[len('Wizard options: '):].rstrip('.').split(', ')
    words = [w for w in words if not w.startswith('instant_prompt=')]
    lines[at:end + 1] = wrap_options(words)
    return '\n'.join(lines).replace('# Type `p10k configure` to generate another config.',
                                    '# Type `prompt configure` to generate another config.', 1)


def check(name, verbose):
    golden = json.loads((GOLDEN / f'{name}.json').read_text())
    problems, pages, configs = [], 0, set()
    for session in golden['sessions']:
        got, text = run(golden, session)
        want = [s for s in session['shots'] if not s.get('zsh_only')]
        if len(got) != len(want):
            problems.append(f'{name}/{session["name"]}: {len(got)} pages, p10k {len(want)}')
        cols = session['cols']
        for i, (shot, page) in enumerate(zip(want, got)):
            label = f'{name}/{session["name"]} page {i + 1} (then {shot.get("key") or shot.get("resize")})'
            compare_page(label, shot['screen'], shot['cursor'], page, cols, problems, verbose)
            if 'resize' in shot:
                cols = shot['resize'][0]
            pages += 1
        configs.add(text)
    if golden['config']:
        want = expected_config(golden).split('\n')[1:]
        for text in configs:
            got = (text or '').split('\n')[1:]
            if got != want:
                diff = next((i for i, (a, b) in enumerate(zip(want, got)) if a != b), min(len(want), len(got)))
                problems.append(f'{name}: configuration line {diff + 2} differs\n'
                                f'    p10k: {want[diff] if diff < len(want) else None!r}\n'
                                f'    ours: {got[diff] if diff < len(got) else None!r}')
    elif any(configs - {None}):
        problems.append(f'{name}: a configuration was written; p10k wrote none')
    return pages, problems


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    verbose = '-v' in sys.argv
    if not DUMP.exists():
        sys.exit(f'golden_wizard: build {DUMP} first (make prompt-test)')
    names = args or sorted(p.stem for p in GOLDEN.glob('*.json'))
    total, failed = 0, []
    for name in names:
        pages, problems = check(name, verbose)
        total += pages
        if problems:
            failed.append(name)
            print(f'{name}: {len(problems)} problems', flush=True)
            for p in problems[:None if verbose else 6]:
                print('  ' + p.replace('\n', '\n  '))
    if failed:
        sys.exit(f'golden_wizard: {len(failed)} of {len(names)} paths differ from Powerlevel10k: {", ".join(failed)}')
    print(f'wizard: {total} pages and {sum(1 for n in names if json.loads((GOLDEN / f"{n}.json").read_text())["config"])} '
          f'configurations match Powerlevel10k\'s wizard ({len(names)} paths)')


if __name__ == '__main__':
    main()
