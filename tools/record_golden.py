#!/usr/bin/env python3
"""Record golden terminal streams: what real programs write, and what a mature
emulator shows for it.

Each case runs a real program inside a private tmux server (its own socket, no
user configuration, TERM=xterm-256color) under `script`, which records every
byte the program writes. tmux answers the program's queries and is the
reference emulator: when the case reaches its moment, the program is frozen
(SIGSTOP, so nothing more is written), the screen is captured with its
attributes (`capture-pane -e -N`), and the cursor and screen mode are noted.

    tests/tty/golden/<case>/stream.bin    the bytes, exactly as written
    tests/tty/golden/<case>/screen.txt    tmux's screen, with SGR attributes
    tests/tty/golden/<case>/meta.json     size, cursor, alternate screen, how

tools/golden_terminal.py replays stream.bin into EmbLink's screen model and
compares cell for cell. Recording needs tmux, script, zsh, vim, less, git and
htop; the corpus is committed, so replaying needs none of them.

    python3 tools/record_golden.py              # every case
    python3 tools/record_golden.py vim less     # some

The p10k case runs your own zsh configuration with an EMPTY history file
(autosuggestions draw history entries, and your history is not test data) and
oh-my-zsh's window titles off (they carry user@host).
htop watches one dummy process with the USER column removed, so the stream
holds no other process's command line. Read a new stream before committing it.
"""
import json, os, re, shutil, signal, socket, subprocess, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / 'tests' / 'tty' / 'golden'
SOCKET = 'emblink-golden'
DEMO = Path('/tmp/emblink-golden-demo')


def tmux(*args, check=True):
    return subprocess.run(['tmux', '-L', SOCKET, '-f', str(CONF), *args],
                          capture_output=True, text=True, check=check).stdout


def capture(target):
    return tmux('capture-pane', '-p', '-e', '-N', '-t', target)


def pane_pid(target):
    return int(tmux('display', '-p', '-t', target, '#{pane_pid}').strip())


def descendants(pid):
    out = subprocess.run(['ps', '-A', '-o', 'pid=,ppid=,comm='], capture_output=True, text=True).stdout
    children = {}
    for line in out.splitlines():
        p, pp, comm = line.split(None, 2)
        children.setdefault(int(pp), []).append((int(p), comm.strip()))
    found, todo = [], [pid]
    while todo:
        for child in children.get(todo.pop(), []):
            found.append(child)
            todo.append(child[0])
    return found


def settle(target, quiet=0.8, limit=20.0):
    """Wait until the screen stops changing for `quiet` seconds."""
    end, last, since = time.monotonic() + limit, None, time.monotonic()
    while time.monotonic() < end:
        now = capture(target)
        if now != last:
            last, since = now, time.monotonic()
        elif time.monotonic() - since >= quiet:
            return
        time.sleep(0.1)


def send(target, keys, literal=True):
    tmux('send-keys', '-t', target, *(['-l'] if literal else []), keys)


def record(case, cols, rows, command, steps, freeze, env=None, title=False):
    out = GOLDEN / case
    out.mkdir(parents=True, exist_ok=True)
    stream = out / 'stream.bin'
    stream.unlink(missing_ok=True)
    recorder = ['script', '-q', str(stream), *command]
    # The pane outlives the program, so a final screen stays to be captured.
    shell = ' '.join(f"'{a}'" for a in recorder) + '; exec sleep 100000'
    target = f'{case}:0.0'
    exports = [a for k, v in (env or {}).items() for a in ('-e', f'{k}={v}')]
    tmux('new-session', '-d', '-s', case, '-x', str(cols), '-y', str(rows), *exports, '-c', str(DEMO), shell)
    try:
        time.sleep(0.5)
        for step in steps:
            kind, value = step[0], step[1] if len(step) > 1 else None
            if kind == 'keys':
                send(target, value)
            elif kind == 'key':
                send(target, value, literal=False)
            elif kind == 'sleep':
                time.sleep(value)
            elif kind == 'settle':
                settle(target, quiet=value or 0.8)
        program = [p for p, comm in descendants(pane_pid(target)) if Path(comm).name not in ('script', 'sleep', 'sh')]
        if freeze:
            # Freeze everything the program started, so nothing writes after
            # the screen is taken; then let the bytes already sent land.
            for p in program:
                os.kill(p, signal.SIGSTOP)
            time.sleep(0.6)
        else:
            settle(target)
        screen = capture(target)
        meta = dict(zip(('cursor_x', 'cursor_y', 'alternate', 'cursor_visible'),
                        map(int, tmux('display', '-p', '-t', target,
                                      '#{cursor_x} #{cursor_y} #{alternate_on} #{cursor_flag}').split())))
        if title:   # only where the case sets one: tmux's default title is the host name
            meta['title'] = tmux('display', '-p', '-t', target, '#{pane_title}').rstrip('\n')
    finally:
        # End the program but not `script`: it writes its file out when its
        # child goes, and a killed `script` would lose the recording's tail.
        root = pane_pid(target)
        for p, comm in descendants(root):
            if Path(comm).name != 'script':
                try:
                    os.kill(p, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        end = time.monotonic() + 5
        while time.monotonic() < end and any(Path(c).name == 'script' for _, c in descendants(root)):
            time.sleep(0.1)
        tmux('kill-session', '-t', case, check=False)
    # A shell reports its directory as file://HOST/path (OSC 7). The terminal
    # takes that in silently, so the host is replaced; nothing shown changes.
    data = stream.read_bytes()
    data = re.sub(rb'(\x1b\]7;file://)[^/\x07\x1b]*', rb'\1golden.invalid', data)
    stream.write_bytes(data)
    private = [w for w in (os.environ.get('USER'), socket.gethostname().split('.')[0],
                           subprocess.run(['git', 'config', 'user.email'], capture_output=True, text=True).stdout.strip())
               if w]
    for word in private:
        if word.encode() in data or word in screen:
            stream.unlink()
            sys.exit(f'record_golden: {case} recorded {word!r}; refusing to keep it')
    (out / 'screen.txt').write_text(screen)
    meta.update(cols=cols, rows=rows, command=command, frozen=freeze,
                recorded=time.strftime('%Y-%m-%d'), reference=tmux('-V').strip())
    (out / 'meta.json').write_text(json.dumps(meta, indent=2) + '\n')
    print(f'{case}: {stream.stat().st_size} bytes, cursor ({meta["cursor_x"]},{meta["cursor_y"]}), '
          f'{"alternate" if meta["alternate"] else "primary"} screen')


def demo_tree():
    shutil.rmtree(DEMO, ignore_errors=True)
    DEMO.mkdir(parents=True)
    run = lambda *a: subprocess.run(a, cwd=DEMO, check=True, capture_output=True)
    run('git', 'init', '-q', '-b', 'main')
    run('git', 'config', 'user.name', 'Golden'); run('git', 'config', 'user.email', 'golden@example.invalid')
    shutil.copyfile(ROOT / 'user/terminal/vt.h', DEMO / 'vt.h')
    (DEMO / 'README.md').write_text('# demo\n\nA repository for golden terminal streams.\n')
    run('git', 'add', '.'); run('git', 'commit', '-q', '-m', 'Initial demo')
    for i in range(1, 31):
        (DEMO / 'log.txt').write_text(f'revision {i}\n')
        run('git', 'add', 'log.txt'); run('git', 'commit', '-q', '-m', f'Revision {i}: adjust the log')
    with open(DEMO / 'README.md', 'a') as f:
        f.write('A change nobody committed.\n')
    (DEMO / 'notes.txt').write_text('untracked\n')
    subprocess.run(f'git --no-pager log --color=always --graph --decorate --stat -n 40 > {DEMO}/history.txt',
                   shell=True, cwd=DEMO, check=True)
    # htop 3.5 takes its columns from `screen:Main`; this one has no USER.
    (DEMO / 'htoprc').write_text('htop_version=3.5.3-3.5.3\nconfig_reader_min_version=3\n'
                                 'fields=0 17 18 38 39 2 46 47 49 1\nhide_userland_threads=1\n'
                                 'show_program_path=0\nscreen_tabs=0\ncolor_scheme=0\n'
                                 'screen:Main=PID PRIORITY NICE M_VIRT M_RESIDENT STATE PERCENT_CPU '
                                 'PERCENT_MEM TIME Command\n.sort_key=PERCENT_CPU\n.tree_sort_key=PID\n'
                                 '.tree_view=0\n.sort_direction=-1\n.tree_sort_direction=1\n')
    (DEMO / 'sgr.py').write_text(SGR_SCRIPT)
    (DEMO / 'osc.py').write_text(OSC_SCRIPT)
    (DEMO / 'unicode.txt').write_text(UNICODE_TEXT)


SGR_SCRIPT = r'''
import sys
w = sys.stdout.write
w("\x1b[1mbold\x1b[0m \x1b[2mdim\x1b[0m \x1b[3mitalic\x1b[0m \x1b[4munderline\x1b[0m \x1b[5mblink\x1b[0m "
  "\x1b[7minverse\x1b[0m \x1b[8mhidden\x1b[0m \x1b[9mstrike\x1b[0m \x1b[53moverline\x1b[0m\r\n")
w("\x1b[4:3mcurly\x1b[0m \x1b[4:2mdouble\x1b[0m \x1b[4:4mdotted\x1b[0m \x1b[4:5mdashed\x1b[0m "
  "\x1b[21mdouble21\x1b[0m \x1b[4;58;2;255;0;0mred line\x1b[0m \x1b[4;58:5:33mpalette line\x1b[0m\r\n")
for base in (30, 90):
    w("".join(f"\x1b[{base + i}m{base + i:3d}" for i in range(8)) + "\x1b[0m  ")
    w("".join(f"\x1b[{base + 10 + i}m{base + 10 + i:4d}" for i in range(8)) + "\x1b[0m\r\n")
for row in range(16, 256, 24):
    w("".join(f"\x1b[48;5;{i}m{i:3d}" for i in range(row, min(row + 24, 256))) + "\x1b[0m\r\n")
w("".join(f"\x1b[38;2;{i * 8};{255 - i * 8};128m#" for i in range(32)) + "\x1b[0m ")
w("".join(f"\x1b[48:2::{i * 8}:64:{255 - i * 8}m " for i in range(32)) + "\x1b[0m\r\n")
w("\x1b[1;31mbold red\x1b[22m normal red\x1b[39m default \x1b[38;5;208morange\x1b[0m\r\n")
'''

OSC_SCRIPT = r'''
import sys
w = sys.stdout.write
w("\x1b]0;golden title zero\x07")
w("\x1b]2;golden window title\x1b\\")
w("\x1b]7;file://golden.invalid/tmp/emblink-golden-demo\x07")
w("before \x1b]8;;https://example.invalid/doc\x1b\\link text\x1b]8;;\x1b\\ after\r\n")
w("\x1b]133;A\x07prompt$ \x1b]133;B\x07command\r\n\x1b]133;C\x07output\r\n\x1b]133;D;0\x07")
w("\x1b]10;?\x07\x1b]11;?\x07query answered\r\n")
# No APC here: tmux takes ESC _ ... as a title (screen's convention); xterm ignores it.
w("\x1bP+q544e\x1b\\\x1b^hidden pm\x1b\\done\r\n")
sys.stdout.flush()
'''

UNICODE_TEXT = ('ASCII and Latin: café naïve Ωμέγα Привет\n'
                'Combining: é ä ñ कि\n'
                'Wide: 漢字かなカナ 한국어 ｆｕｌｌ\n'
                'Emoji: \U0001F600 \U0001F680 ❤️ \U0001F44D\U0001F3FD\n'
                'Box: ┌──┬──┐ ╔══╗ ▁▂▃▄▅▆▇█ ░▒▓\n'
                'Powerline:   branch\n'
                'Braille: ⠁⠃⠉⠙⠑⠋⠛⠓\n')

CASES = {
    'p10k': dict(cols=120, rows=30, command=['zsh', '-i'], freeze=True,
                 env={'HISTFILE': str(DEMO / 'zsh_history'), 'DISABLE_AUTO_UPDATE': 'true',
                      'DISABLE_AUTO_TITLE': 'true'},
                 steps=[('settle', 2.0), ('keys', 'cd /tmp/emblink-golden-demo'), ('key', 'Enter'), ('settle', 1.5),
                        ('keys', 'ls'), ('key', 'Enter'), ('settle', 1.5),
                        ('keys', 'git status --short'), ('key', 'Enter'), ('settle', 1.5),
                        ('keys', 'false'), ('key', 'Enter'), ('settle', 1.5),
                        ('keys', 'sleep 4'), ('key', 'Enter'), ('sleep', 4.5), ('settle', 1.5),
                        ('keys', 'echo done'), ('key', 'Enter'), ('settle', 1.5),
                        ('keys', 'git sta'), ('settle', 1.5)]),
    'vim': dict(cols=80, rows=24, command=['vim', '-u', 'NONE', '-i', 'NONE', '-N', '-c', 'syntax on',
                                           '-c', 'set number laststatus=2 ruler', 'vt.h'], freeze=True,
                steps=[('settle', 1.5), ('keys', '/vt_cell'), ('key', 'Enter'), ('keys', 'jjw'), ('settle', 1.0)]),
    'vim-exit': dict(cols=80, rows=24, command=['sh', '-c', 'head -8 README.md vt.h; exec vim -u NONE -i NONE -N '
                                                '-c "syntax on" vt.h'],
                     freeze=False, steps=[('settle', 1.5), ('keys', ':q!'), ('key', 'Enter'), ('settle', 1.0)]),
    'less': dict(cols=80, rows=24, command=['less', '-R', 'history.txt'], freeze=True,
                 steps=[('settle', 1.0), ('key', 'Space'), ('settle', 1.0), ('keys', '/Revision 1'), ('key', 'Enter'),
                        ('settle', 1.0)]),
    'htop': dict(cols=100, rows=30, command=['sh', '-c', 'sleep 100000 & HTOPRC=htoprc exec htop -p $!'], freeze=True,
                 steps=[('sleep', 3.0)]),
    'git-log': dict(cols=100, rows=40, command=['git', '--no-pager', 'log', '--color=always', '--graph', '--decorate',
                                                '--stat', '-n', '6'], freeze=False, steps=[('settle', 1.0)]),
    'sgr': dict(cols=100, rows=24, command=['python3', 'sgr.py'], freeze=False, steps=[('settle', 1.0)]),
    'osc': dict(cols=80, rows=12, command=['python3', 'osc.py'], freeze=False, title=True, steps=[('settle', 1.0)]),
    'unicode': dict(cols=80, rows=12, command=['cat', 'unicode.txt'], freeze=False, steps=[('settle', 1.0)]),
}

CONF = Path(tempfile.gettempdir()) / 'emblink-golden.tmux.conf'


def main():
    if sys.platform != 'darwin':
        sys.exit('record_golden: record on macOS -- util-linux script writes headers into the recording')
    for tool in ('tmux', 'script', 'zsh', 'vim', 'less', 'git', 'htop'):
        if not shutil.which(tool):
            sys.exit(f'record_golden: {tool} is required to record (the committed corpus replays without it)')
    CONF.write_text('set -g default-terminal xterm-256color\nset -g status off\nset -sg escape-time 0\n'
                    'set -g history-limit 0\nset -g remain-on-exit off\n')
    demo_tree()
    wanted = sys.argv[1:] or list(CASES)
    try:
        for case in wanted:
            spec = CASES[case]
            record(case, spec['cols'], spec['rows'], spec['command'], spec['steps'], spec['freeze'], spec.get('env'),
                   spec.get('title', False))
    finally:
        tmux('kill-server', check=False)


if __name__ == '__main__':
    main()
