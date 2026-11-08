#!/usr/bin/env python3
"""Record zsh-syntax-highlighting and zsh-autosuggestions, for the shell's
editor to match.

The line editor (shell/edit) suggests from history and colours the line as
the two zsh plugins a Mac's shell is set up with do, with their default
settings. This types into the real ones -- a private zsh with both, in a
private tmux server, in a fixture directory -- lines that mean the same in
both shells: commands both have (echo, wc, head, uniq), one neither has,
quotes and escapes, a pipe, a separator, a comment, a command substitution,
files that exist and one that does not, a word on its way to being a file's
name, a `for` loop's first words; and, against a history both are given, a
suggestion shown, typed into, taken by Right or Ctrl+E, kept while the cursor
moves, dropped by Up and by Enter, and none when nothing matches.

    tests/edit/highlight.json    each line: the keys, and the row they leave

tools/golden_highlight.py types the same keys into the editor
(build/highlight_dump, with the shell's own highlighter) and compares the row
cell for cell; the corpus is committed, so comparing needs no zsh.

    python3 tools/record_highlight.py

Recording needs macOS with zsh, tmux, and Homebrew's zsh-syntax-highlighting
and zsh-autosuggestions (or HIGHLIGHTING= / AUTOSUGGESTIONS= paths).
"""
import json, os, shutil, subprocess, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'tests' / 'edit' / 'highlight.json'
SOCKET = 'emblink-highlight'
HOME = Path('/tmp/emblink-highlight')
CONF = Path(tempfile.gettempdir()) / 'emblink-highlight.tmux.conf'
HIGHLIGHTING = Path(os.environ.get('HIGHLIGHTING', '/opt/homebrew/share/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh'))
AUTOSUGGESTIONS = Path(os.environ.get('AUTOSUGGESTIONS',
                                      '/opt/homebrew/share/zsh-autosuggestions/zsh-autosuggestions.zsh'))
COLS, ROWS = 80, 6

# the history both shells are given, oldest first
HISTORY = ['echo first', 'head src/main.c', 'echo hello world', 'wc README.md']
# the fixture directory's files
FILES = ['README.md', 'notes.txt', 'src/main.c']

# each: a name, then what is typed: text, or one of tmux's key names in [ ]
LINES = [
    ('command', ['echo hello world']),
    ('unknown', ['frobnicate --verbose']),
    ('quotes', ["echo 'single quoted' \"double quoted\""]),
    ('escape', ['echo "a \\" b"']),
    ('pipe', ['echo hi | wc']),
    ('separator', ['echo ok; frobnicate']),
    ('comment', ['echo hi # a comment']),
    ('path', ['head README.md notes.txt']),
    ('path-dir', ['head src/']),
    ('path-quoted', ['head "README.md"']),
    ('path-prefix', ['uniq READ']),
    ('path-missing', ['head nothere.txt']),
    ('unterminated', ['echo "unterminated']),
    ('substitution', ['echo $(wc)']),
    ('for', ['for x in']),
    ('suggest', ['ec']),
    ('suggest-typed', ['ech']),
    ('suggest-backspace', ['ech', '[BSpace]']),
    ('suggest-none', ['xyz']),
    ('suggest-path', ['head s']),
    ('suggest-right', ['ec', '[Right]']),
    ('suggest-end-of-line', ['ec', '[C-e]']),
    ('suggest-left', ['ec', '[Left]']),
    ('suggest-left-right', ['ec', '[Left]', '[Right]']),
    ('suggest-up', ['ec', '[Up]']),
    ('suggest-enter', ['ec', '[Enter]']),
]


def tmux(*args, check=True):
    return subprocess.run(['tmux', '-L', SOCKET, '-f', str(CONF), *args],
                          capture_output=True, text=True, check=check).stdout


def settle(target, quiet=0.6, limit=15.0):
    end, last, since = time.monotonic() + limit, None, time.monotonic()
    while time.monotonic() < end:
        now = tmux('capture-pane', '-p', '-e', '-t', target)
        if now != last:
            last, since = now, time.monotonic()
        elif time.monotonic() - since >= quiet:
            return now
        time.sleep(0.05)
    return last


def fixture():
    shutil.rmtree(HOME, ignore_errors=True)
    for f in FILES:
        (HOME / f).parent.mkdir(parents=True, exist_ok=True)
        (HOME / f).write_text(f + '\n')
    zdot = HOME.parent / 'emblink-highlight-zdot'
    shutil.rmtree(zdot, ignore_errors=True)
    zdot.mkdir()
    (zdot / '.zshrc').write_text(
        "PS1='$ '\nRPS1=\nHISTFILE=/dev/null\nHISTSIZE=100\nSAVEHIST=0\n"
        'setopt interactive_comments no_prompt_sp\nunsetopt correct correct_all\n'
        'PATH=/usr/bin:/bin\n' +
        ''.join(f"print -s {json.dumps(h)}\n" for h in HISTORY) +
        f'source {AUTOSUGGESTIONS}\nsource {HIGHLIGHTING}\n')
    return zdot


def record_line(zdot, name, typed):
    target = f'{name}:0.0'
    env = {'HOME': str(HOME), 'ZDOTDIR': str(zdot), 'TERM': 'xterm-256color', 'LANG': 'en_US.UTF-8'}
    exports = [a for k, v in env.items() for a in ('-e', f'{k}={v}')]
    tmux('new-session', '-d', '-s', name, '-x', str(COLS), '-y', str(ROWS), *exports, '-c', str(HOME),
         'zsh -i; exec sleep 100000')
    try:
        settle(target, quiet=1.0)
        y0 = int(tmux('display', '-p', '-t', target, '#{cursor_y}'))
        for part in typed:
            if part.startswith('[') and part.endswith(']'):
                tmux('send-keys', '-t', target, part[1:-1])
            else:
                tmux('send-keys', '-t', target, '-l', part)
            settle(target, quiet=0.4)
        screen = settle(target, quiet=0.8).split('\n')
        x, y = map(int, tmux('display', '-p', '-t', target, '#{cursor_x} #{cursor_y}').split())
        return dict(name=name, typed=typed, row=screen[y0], cursor=[x, y - y0])
    finally:
        tmux('kill-session', '-t', name, check=False)


def main():
    if sys.platform != 'darwin':
        sys.exit('record_highlight: record on macOS, where zsh and the plugins are')
    for p in (HIGHLIGHTING, AUTOSUGGESTIONS):
        if not p.exists():
            sys.exit(f'record_highlight: no {p}')
    CONF.write_text('set -g default-terminal xterm-256color\nset -g status off\nset -sg escape-time 0\n'
                    'set -g history-limit 0\n')
    zdot = fixture()
    lines = [record_line(zdot, name, typed) for name, typed in LINES]
    version = lambda p, pat: next((l for l in p.read_text().split('\n') if pat in l), '').strip('# ')
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(dict(
        cols=COLS, prompt='$ ', history=HISTORY, files=FILES, recorded=time.strftime('%Y-%m-%d'),
        reference=f"zsh-syntax-highlighting {(HIGHLIGHTING.parent / '.version').read_text().strip()}, "
                  f"zsh-autosuggestions {version(AUTOSUGGESTIONS, '# v')}, under {tmux('-V').strip()}",
        lines=lines), indent=1, ensure_ascii=False) + '\n')
    tmux('kill-server', check=False)
    shutil.rmtree(HOME, ignore_errors=True)
    shutil.rmtree(zdot, ignore_errors=True)
    for l in lines:
        print(f'{l["name"]}: {l["row"]!r} cursor {l["cursor"]}')


if __name__ == '__main__':
    main()
