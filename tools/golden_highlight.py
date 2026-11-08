#!/usr/bin/env python3
"""Hold the shell's line editor to zsh-syntax-highlighting and
zsh-autosuggestions, cell for cell.

tests/edit/highlight.json holds the row zsh drew with both plugins for each
line typed (tools/record_highlight.py): the same keys go to the editor with
the shell's own highlighter (build/highlight_dump, in a directory holding the
same files, with the same history), and the prompt's row is compared --
character, colour, bold, underline -- and the cursor (but after Enter, when
zsh has run the line and its output moved the cursor on).

    python3 tools/golden_highlight.py        # -v: every difference
"""
import json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from golden_prompt import cells   # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / 'tests' / 'edit' / 'highlight.json'
DUMP = Path(os.environ.get('HIGHLIGHT_DUMP', ROOT / 'build' / 'highlight_dump'))
KEYS = {'Right': '\x1b[C', 'Left': '\x1b[D', 'Up': '\x1b[A', 'Down': '\x1b[B', 'BSpace': '\x7f', 'Enter': '\r',
        'C-e': '\x05', 'C-f': '\x06', 'C-c': '\x03', 'M-f': '\x1bf'}


def cell(c):
    """A cell as compared: its character, and when it shows one, its colour
    and bold; underline either way."""
    text, fg, bold, ul = c
    return (text, fg, bold, ul) if text.strip() else (' ', None, False, ul)


def main():
    verbose = '-v' in sys.argv
    if not DUMP.exists():
        sys.exit(f'golden_highlight: build {DUMP} first (make shell-test)')
    golden = json.loads(GOLDEN.read_text())
    cols = golden['cols']
    home = Path(tempfile.mkdtemp(prefix='emblink-highlight-'))
    try:
        for f in golden['files']:
            (home / f).parent.mkdir(parents=True, exist_ok=True)
            (home / f).write_text(f + '\n')
        failed = []
        for line in golden['lines']:
            history = [a for h in golden['history'] for a in ('-h', h)]
            keys = [KEYS[p[1:-1]] if p.startswith('[') and p.endswith(']') else p for p in line['typed']]
            got = json.loads(subprocess.run([str(DUMP), str(cols), *history, *keys], capture_output=True, text=True,
                                            cwd=home, check=True).stdout)
            row, _, _ = cells(line['row'], cols)
            want = [cell((c[0], c[1], c[3], c[4])) for c in row]
            ours = [cell(tuple(c)) if c else ('', None, False, False) for c in got['row']]
            bad = [(x, w, o) for x, (w, o) in enumerate(zip(want, ours)) if w != o]
            ran = line['typed'][-1] == '[Enter]'          # zsh ran it: where its output left the cursor
            if bad or (not ran and got['cursor'] != line['cursor']):
                failed.append(line['name'])
                print(f'{line["name"]}: {len(bad)} cells differ, cursor {got["cursor"]} (zsh {line["cursor"]})')
                print('    zsh:  ' + ''.join(c[0] for c in want).rstrip())
                print('    ours: ' + ''.join(c[0] or '' for c in ours).rstrip())
                for x, w, o in bad[:None if verbose else 4]:
                    print(f'    col {x}: zsh {w} / ours {o}')
    finally:
        shutil.rmtree(home, ignore_errors=True)
    if failed:
        sys.exit(f'golden_highlight: {len(failed)} of {len(golden["lines"])} lines differ from zsh: {", ".join(failed)}')
    print(f'highlight: {len(golden["lines"])} lines match zsh-syntax-highlighting and zsh-autosuggestions '
          f'({golden["reference"]})')


if __name__ == '__main__':
    main()
