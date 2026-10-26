#!/usr/bin/env python3
"""Record Powerlevel10k's configuration wizard, for `prompt configure` to match.

`prompt configure` (shell/prompt/wizard.c) is p10k's wizard: the same
questions in the same order, the same pages, the same configuration file at
the end. This drives the real one -- `p10k configure` in a private zsh in a
private tmux server, in a fixture home -- through answer paths that between
them take every branch: each font answer and the mode it leads to, each style,
charset, colour, separator, head, tail, height, connection, frame, ornament,
spacing, icon, flow and transient answer, a restart, an overwritten
configuration, a quit, a resize too small and back, and the wizard that runs
by itself in a shell with no configuration. Each path runs at window sizes
where every page fits, where the pages drop lines to fit (80x24), and the
smallest p10k allows (47x14).

    tests/prompt/wizard/<path>.json       every page, with the key pressed on
                                          it, and the configuration the wizard
                                          wrote (as edits to its style's
                                          template, third_party/powerlevel10k)

tools/golden_wizard.py drives `prompt configure` (build/wizard_dump) with the
same keys and compares every page cell for cell, and the configuration line
for line; the corpus is committed, so comparing needs no zsh.

The reference runs where EmbLinkOS's wizard does, as far as the wizard can
tell: a 256-colour terminal with 24-bit colour (COLORTERM), an OS it has no
icon for (uname is EmbLinkOS), the configuration at ~/.config/shell/p10k.zsh,
and a .zshrc that already sources it and instant prompt (so the wizard has
nothing to ask about .zshrc). Instant prompt is zsh's; its page is recorded and
marked, and `prompt configure` does not ask it.

    python3 tools/record_wizard.py            # every path
    python3 tools/record_wizard.py user pure  # some

Recording needs macOS with zsh, tmux and Powerlevel10k (Homebrew's
/opt/homebrew/share/powerlevel10k, or POWERLEVEL10K=path).
"""
import difflib, json, os, re, shutil, socket, subprocess, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / 'tests' / 'prompt' / 'wizard'
P10K = Path(os.environ.get('POWERLEVEL10K', '/opt/homebrew/share/powerlevel10k'))
SOCKET = 'emblink-wizard'
HOME = Path('/tmp/emblink-wizard-home')
CONFIG = '.config/shell/p10k.zsh'
CONF = Path(tempfile.gettempdir()) / 'emblink-wizard.tmux.conf'

SIZES = {'wide': (100, 40), 'classic': (80, 24), 'least': (47, 14)}


STYLES = ROOT / 'third_party' / 'powerlevel10k' / 'config'


def config_delta(text):
    """A configuration the wizard wrote, as the template it is based on and
    what changed: its header, and each run of the template's lines replaced
    (`at`, `end`, the new lines; none: deleted)."""
    lines = text.split('\n')
    style = re.search(r'config/p10k-([a-z0-9-]+)\.zsh', lines[1]).group(1)
    base = (STYLES / f'p10k-{style}.zsh').read_text().rstrip('\n').split('\n')
    at = next(i for i, line in enumerate(lines) if line == base[0])
    body = lines[at:]
    edits = [[i1, i2, body[j1:j2]] for tag, i1, i2, j1, j2 in
             difflib.SequenceMatcher(None, base, body, autojunk=False).get_opcodes() if tag != 'equal']
    return dict(style=style, header=lines[:at], edits=edits)


def config_apply(delta):
    """The configuration text again."""
    base = (STYLES / f'p10k-{delta["style"]}.zsh').read_text().rstrip('\n').split('\n')
    out, last = list(delta['header']), 0
    for i1, i2, new in delta['edits']:
        out += base[last:i1] + new
        last = i2
    return '\n'.join(out + base[last:])


def keys(*parts):
    """'ynn', ('resize', 40, 24), '1' -> one step each: a key, or a resize."""
    out = []
    for p in parts:
        out += list(p) if isinstance(p, str) else [p]
    return out


# Each path: the keys pressed, page by page (the last answers instant prompt);
# force: `p10k configure` typed (False: the wizard a shell with no
# configuration runs by itself); existing: a configuration is there already.
PATHS = {
    # the answers in the header of the configuration on the Mac this system
    # is modelled on: powerline, lean, 12h, two lines, left frame, darkest
    'user': dict(keys=keys('ynn', '1111212421y1'), sizes=('wide', 'classic', 'least')),
    # nerdfont-v3 (the second arrow, and the width), large icons, classic, every
    # nerd-font separator, head and tail
    'classic': dict(keys=keys('yyny1', 'n', '213243523412', '2n1'), sizes=('wide', 'classic', 'least')),
    # nerdfont-complete (the first arrow), small icons, rainbow
    'rainbow': dict(keys=keys('yyy', 'y', '311323223221', '1y1'), sizes=('wide', 'classic', 'least')),
    # pure, its original colours, the right prompt
    'pure': dict(keys=keys('yyny1', 'y', '41222', '1n1'), sizes=('wide', 'classic')),
    # lean in 8 colours, ASCII, one line, fluent
    'ascii-charset': dict(keys=keys('yyny1', 'n', '122n11', '2y1'), sizes=('wide', 'classic')),
    # awesome-fontconfig with the python logo, vertical separators, flat
    # heads, blurred tails, one line
    'awesome-fontconfig': dict(keys=keys('yynn', 'y', 'n', '211n21212', '21n1'), sizes=('wide', 'classic')),
    # no font answers yes: ascii
    'ascii': dict(keys=keys('nnnn', '2212211', 'y1'), sizes=('wide', 'classic')),
    # the quotes: compatible
    'compatible': dict(keys=keys('nnny', '312122112', '2n1'), sizes=('wide', 'classic')),
    # the second lock without a diamond: flat; lean in 8 colours, solid, full
    # frame
    'flat': dict(keys=keys('nny', 'y', '1122234112', '2y1'), sizes=('wide', 'classic')),
    # the second lock with a diamond: awesome-patched, no python; rainbow in
    # ASCII, the connection's colour alone
    'awesome-patched': dict(keys=keys('yny', 'n', 'n', '32122321', 'n1'), sizes=('wide', 'classic')),
    # the wizard a new shell runs by itself (no configuration): its greeting;
    # pure in snazzy colours on the left, one line
    'auto': dict(keys=keys('ny', 'y', '4211', '12', 'y1'), sizes=('wide', 'classic'), force=False),
    # the width question, a restart from the style page, then python, and a
    # configuration to overwrite
    'restart': dict(keys=keys('yyny2', 'n', 'y', 'r', 'yyny3', 'y', 'y', '11121111', 'n1', 'y'), sizes=('wide', 'classic'),
                    existing=True),
    # quit at the charset page
    'quit': dict(keys=keys('yyyy', '1', 'q'), sizes=('wide',)),
    # quit when the wizard ran by itself: how it says to stop it
    'quit-auto': dict(keys=keys('q'), sizes=('wide',), force=False),
    # too narrow, too short, then room again, on the style page
    'resize': dict(keys=keys('ynn', ('resize', 40, 24), ('resize', 80, 12), ('resize', 100, 40), '1111212421y1'),
                   sizes=('classic',)),
}

ZSHRC = '''\
# Enable Powerlevel10k instant prompt. Should stay close to the top of ~/.zshrc.
if [[ -r "${XDG_CACHE_HOME:-$HOME/.cache}/p10k-instant-prompt-${(%):-%n}.zsh" ]]; then
  source "${XDG_CACHE_HOME:-$HOME/.cache}/p10k-instant-prompt-${(%):-%n}.zsh"
fi
HISTFILE=/dev/null
unsetopt correct correct_all
# the OS the wizard runs on has no icon of its own in p10k, as EmbLinkOS has none
function uname() { print -r -- EmbLinkOS }
AUTO
source P10K/powerlevel10k.zsh-theme
[[ ! -f "$POWERLEVEL9K_CONFIG_FILE" ]] || source "$POWERLEVEL9K_CONFIG_FILE"
'''


def tmux(*args, check=True):
    return subprocess.run(['tmux', '-L', SOCKET, '-f', str(CONF), *args],
                          capture_output=True, text=True, check=check).stdout


def capture(target):
    return tmux('capture-pane', '-p', '-e', '-N', '-t', target)


def trim(lines):
    """The lines without the blank cells at their ends that carry no colour.
    tmux writes a change of colour only where one happens, so the pen goes
    on from one line to the next."""
    bg = inverse = underline = False
    out = []
    for line in lines:
        for m in re.finditer(r'\x1b\[([0-9;:]*)m', line):
            p = [int(x) if x else 0 for x in re.split('[;:]', m.group(1))] if m.group(1) else [0]
            i = 0
            while i < len(p):
                n = p[i]
                if n == 0: bg = inverse = underline = False
                elif n == 7: inverse = True
                elif n == 27: inverse = False
                elif n == 4: underline = True
                elif n == 24: underline = False
                elif 40 <= n <= 47 or 100 <= n <= 107: bg = True
                elif n == 49: bg = False
                elif n in (38, 48):
                    if n == 48: bg = True
                    i += 2 if i + 1 < len(p) and p[i + 1] == 5 else 4
                i += 1
        out.append(line if bg or inverse or underline else line.rstrip(' '))
    return out


def settle(target, want=None, quiet=0.4, limit=20.0):
    """The screen once it has stopped changing (and shows `want`)."""
    end, last, since = time.monotonic() + limit, None, time.monotonic()
    while time.monotonic() < end:
        now = capture(target)
        if now != last:
            last, since = now, time.monotonic()
        elif time.monotonic() - since >= quiet and (want is None or want in now):
            return now
        time.sleep(0.05)
    raise RuntimeError(f'record_wizard: {target}: no {want!r} on the screen:\n{last}')


def fresh_home(existing, force):
    shutil.rmtree(HOME, ignore_errors=True)
    (HOME / '.config' / 'shell').mkdir(parents=True)
    # an option set keeps the wizard from starting by itself, so `p10k
    # configure` is what starts it; the prompt before it names no one
    auto = ('typeset -g POWERLEVEL9K_DISABLE_CONFIGURATION_WIZARD=true POWERLEVEL9K_CONTEXT_TEMPLATE=ctx' if force
            else '# no option: the wizard starts by itself')
    (HOME / '.zshrc').write_text(ZSHRC.replace('P10K', str(P10K)).replace('AUTO', auto))
    if existing:
        (HOME / CONFIG).write_text('# a configuration from before\n')


def session(name, path, label):
    cols, rows = SIZES[label]
    fresh_home(path.get('existing', False), path.get('force', True))
    target = f'{name}-{label}:0.0'
    env = {'HOME': str(HOME), 'ZDOTDIR': str(HOME), 'TZ': 'UTC', 'TERM': 'xterm-256color', 'COLORTERM': 'truecolor',
           'LANG': 'en_US.UTF-8', 'USER': 'yves', 'LOGNAME': 'yves',
           'POWERLEVEL9K_CONFIG_FILE': str(HOME / CONFIG)}
    exports = [a for k, v in env.items() for a in ('-e', f'{k}={v}')]
    tmux('new-session', '-d', '-s', f'{name}-{label}', '-x', str(cols), '-y', str(rows), *exports, '-c', str(HOME),
         'zsh -i; exec sleep 100000')
    shots = []
    try:
        if path.get('force', True):
            settle(target, quiet=1.0)
            tmux('send-keys', '-t', target, '-l', 'p10k configure')
            tmux('send-keys', '-t', target, 'Enter')
        for step in path['keys']:
            want = 'Choice [' if not shots or 'resize' not in shots[-1] or shots[-1]['ok'] else None
            screen = settle(target, want, quiet=1.5 if shots and 'resize' in shots[-1] else 0.4)
            x, y = map(int, tmux('display', '-p', '-t', target, '#{cursor_x} #{cursor_y}').split())
            shot = dict(cursor=[x, y], screen=trim(screen.split('\n')))
            if 'Instant Prompt Mode' in screen:
                shot['zsh_only'] = True
            if isinstance(step, tuple):
                _, c, r = step
                tmux('resize-window', '-t', f'{name}-{label}', '-x', str(c), '-y', str(r))
                shot.update(resize=[c, r], ok=c >= 47 and r >= 14)
            else:
                shot['key'] = step
                tmux('send-keys', '-t', target, '-l', step)
            shots.append(shot)
        # the wizard is done when the alternate screen is gone
        end = time.monotonic() + 20
        while tmux('display', '-p', '-t', target, '#{alternate_on}').strip() == '1':
            if time.monotonic() > end:
                raise RuntimeError(f'record_wizard: {target}: the wizard did not finish')
            time.sleep(0.1)
        after = settle(target, quiet=1.0)
    finally:
        tmux('kill-session', '-t', f'{name}-{label}', check=False)
    for s in shots:
        s.pop('ok', None)
    config = HOME / CONFIG
    written = config.read_text() if config.exists() and 'from before' not in config.read_text() else None
    return dict(name=label, cols=cols, rows=rows, shots=shots, after=trim(after.split('\n'))), written


def record(name):
    path = PATHS[name]
    sessions, configs = [], []
    for label in path['sizes']:
        s, written = session(name, path, label)
        sessions.append(s)
        configs.append(written)
    # the configuration does not depend on the window: one, but the date line
    if len({'\n'.join(c.split('\n')[1:]) if c else None for c in configs}) != 1:
        sys.exit(f'record_wizard: {name}: the configuration differs between window sizes')
    private = [w for w in (os.environ.get('USER'), socket.gethostname().split('.')[0]) if w and w != 'yves']
    # p10k's own prompt after a quit with no configuration names the user and
    # the host (context): kept as the fixture's
    for s in sessions:
        for word, mask in zip(private, ('yves', 'host')):
            s['after'] = [line.replace(word, mask) for line in s['after']]
    text = json.dumps(sessions)
    for word in private:
        if word in text or (configs[0] and word in configs[0]):
            sys.exit(f'record_wizard: {name} recorded {word!r}; refusing to keep it')
    delta = None
    if configs[0]:
        delta = config_delta(configs[0])
        if config_apply(delta) != configs[0]:
            sys.exit(f'record_wizard: {name}: the configuration does not rebuild from its template')
    GOLDEN.mkdir(parents=True, exist_ok=True)
    (GOLDEN / f'{name}.json').write_text(json.dumps(dict(
        path=name, keys=[list(k) if isinstance(k, tuple) else k for k in path['keys']],
        force=path.get('force', True), existing=path.get('existing', False),
        file=CONFIG, config=delta, recorded=time.strftime('%Y-%m-%d'),
        reference=f"Powerlevel10k {P10K.resolve().parent.name if P10K.name == 'powerlevel10k' else P10K.name} "
                  f"under {tmux('-V').strip()}",
        sessions=sessions), indent=1, ensure_ascii=False) + '\n')
    print(f'{name}: ' + ', '.join(f'{s["name"]} {len(s["shots"])} pages' for s in sessions) +
          ('' if configs[0] else ', no configuration'), flush=True)


def main():
    if sys.platform != 'darwin':
        sys.exit('record_wizard: record on macOS, where zsh and Powerlevel10k are')
    for tool in ('tmux', 'zsh'):
        if not shutil.which(tool):
            sys.exit(f'record_wizard: {tool} is required')
    if not (P10K / 'powerlevel10k.zsh-theme').exists():
        sys.exit(f'record_wizard: no Powerlevel10k at {P10K} (set POWERLEVEL10K)')
    CONF.write_text('set -g default-terminal xterm-256color\nset -g status off\nset -sg escape-time 0\n'
                    'set -g history-limit 0\nset -g window-size manual\n')
    names = sys.argv[1:] or list(PATHS)
    for name in names:
        if name not in PATHS:
            sys.exit(f'record_wizard: no path {name!r} ({", ".join(PATHS)})')
        record(name)
    tmux('kill-server', check=False)
    shutil.rmtree(HOME, ignore_errors=True)


if __name__ == '__main__':
    main()
