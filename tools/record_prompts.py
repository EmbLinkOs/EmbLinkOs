#!/usr/bin/env python3
"""Record what Powerlevel10k itself draws, for EmbLink's prompt engine to match.

For each configuration -- the four styles `p10k configure` starts from
(third_party/powerlevel10k/config), a person's own ~/.p10k.zsh
(tests/prompt/configs/user.p10k.zsh), and lean with each variant in
tests/prompt/configs/variants on top -- a private zsh with nothing but
Powerlevel10k and that configuration runs in a private tmux server, in a
fixture home with a deep directory and a git repository whose state is known.
Each runs three sessions -- 100, 60 and 36 columns wide -- so the directory is
shortened to fit and the right prompt gives way. At each step the screen is
captured with its attributes (tmux is the reference
emulator, as for tools/record_golden.py), with the cursor and the facts the
prompt was drawn from: the directory, the last status, the clock it showed.

    tests/prompt/golden/<config>.json     every step's screen and state

tools/golden_prompt.py renders the same states with shell/prompt and compares
cell for cell; the corpus is committed, so comparing needs no zsh.

    python3 tools/record_prompts.py              # every configuration
    python3 tools/record_prompts.py lean user    # some

Recording needs macOS with zsh, tmux, git and Powerlevel10k (Homebrew's
/opt/homebrew/share/powerlevel10k, or POWERLEVEL10K=path). The user name,
host name and git e-mail are refused in any recording.
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / 'tests' / 'prompt' / 'golden'
P10K = Path(os.environ.get('POWERLEVEL10K', '/opt/homebrew/share/powerlevel10k'))
SOCKET = 'emblink-prompt'
HOME = Path('/tmp/emblink-prompt-home')
DEEP = HOME / 'src' / 'operating-systems' / 'emblink' / 'user' / 'terminal' / 'renderer' / 'fonts'
DEEPER = DEEP / 'with-a-rather-long-name' / 'and-another-one-below-it'
REPO = HOME / 'projects' / 'emblink'
REPO_DEEP = REPO / 'kernel' / 'drivers' / 'video'
# Repositories in the states gitstatus reports: ahead of and behind its
# upstream with a stash and a wip commit, a detached HEAD, a merge stopped by
# a conflict. Fixed dates make the commits, and so the hashes, the same each
# time.
FORK, DETACHED, MERGING = (HOME / 'projects' / n for n in ('fork', 'detached', 'merging'))
REMOTE = Path('/tmp/emblink-prompt-remote.git')
FORK_GIT, DETACHED_GIT, MERGING_GIT = {}, {}, {}
# Neighbours that make truncate_to_unique keep more than a letter: src/sources,
# operating-systems/other, emblink/emacs, terminal/term. A folder marker half
# way down the deep directory: user/package.json.
SIBLINGS = [HOME / 'sources', HOME / 'src' / 'other', HOME / 'src' / 'operating-systems' / 'emacs',
            DEEP.parent.parent.parent / 'term', HOME / 'projects' / 'embedded']
MARKER = DEEP.parent.parent.parent / 'package.json'
CONF = Path(tempfile.gettempdir()) / 'emblink-prompt.tmux.conf'

STYLES = ROOT / 'third_party/powerlevel10k/config'
LEAN = STYLES / 'p10k-lean.zsh'
VARIANTS = ROOT / 'tests/prompt/configs/variants'
# Each configuration is files sourced in turn: a style, or a style and a
# variant on top of it (tests/prompt/configs/variants, whose first line names
# the style) -- each shortening strategy, the directory's styles, the
# segments' other forms, the glue between them, icons, a ruler.
CONFIGS = {
    'lean': [LEAN],
    'classic': [ROOT / 'third_party/powerlevel10k/config/p10k-classic.zsh'],
    'rainbow': [ROOT / 'third_party/powerlevel10k/config/p10k-rainbow.zsh'],
    'pure': [ROOT / 'third_party/powerlevel10k/config/p10k-pure.zsh'],
    'user': [ROOT / 'tests/prompt/configs/user.p10k.zsh'],
    **{v.stem: [STYLES / re.search(r'On top of (p10k-[a-z0-9-]+\.zsh)', v.read_text()).group(1), v]
       for v in sorted(VARIANTS.glob('*.zsh'))},
}

def rel(path):
    return '~/' + str(path.relative_to(HOME))

GIT = dict(branch='main', staged=1, unstaged=1, untracked=2)

# Sessions at three widths. Each step: what is typed (a string, or keys to
# send one after another with a pause), and what the prompt after it is drawn
# from. `output`: the command prints something (^C, a job number), so the
# line above the prompt is not PROMPT_ADD_NEWLINE's empty one.
SESSIONS = [
    ('wide', 100, 16, [
        dict(step='home', typed=None, state=dict(dir='~', status=None)),
        dict(step='deep', typed=f'cd {DEEP}', state=dict(dir=rel(DEEP), status=0)),
        dict(step='error', typed='false', state=dict(dir=rel(DEEP), status=1)),
        dict(step='deeper', typed='cd $deeper', state=dict(dir=rel(DEEPER), status=0)),
        dict(step='git', typed=f'cd {REPO}', state=dict(dir=rel(REPO), status=0, git=GIT)),
        dict(step='slow', typed='sleep 3.2', state=dict(dir=rel(REPO), status=0, duration_ms=3200, git=GIT)),
        dict(step='jobs', typed='sleep 100 &', output=True, state=dict(dir=rel(REPO), status=0, jobs=1, git=GIT)),
        dict(step='fork', typed='cd $fork', state=dict(dir=rel(FORK), status=0, jobs=1, git=FORK_GIT)),
        dict(step='detached', typed='cd $detached', state=dict(dir=rel(DETACHED), status=0, jobs=1, git=DETACHED_GIT)),
        dict(step='merging', typed='cd $merging', state=dict(dir=rel(MERGING), status=0, jobs=1, git=MERGING_GIT)),
    ]),
    ('narrow', 60, 16, [
        dict(step='home', typed=None, state=dict(dir='~', status=None)),
        dict(step='deep', typed='cd $deep', state=dict(dir=rel(DEEP), status=0)),
        dict(step='interrupt', typed='sleep 30', keys=['C-c'], output=True, state=dict(dir=rel(DEEP), status=130)),
        dict(step='repo', typed='cd $repodeep', state=dict(dir=rel(REPO_DEEP), status=0, git=GIT)),
    ]),
    ('tiny', 36, 12, [
        dict(step='home', typed=None, state=dict(dir='~', status=None)),
        dict(step='deep', typed='cd $deep', state=dict(dir=rel(DEEP), status=0)),
    ]),
]


def tmux(*args, check=True):
    return subprocess.run(['tmux', '-L', SOCKET, '-f', str(CONF), *args],
                          capture_output=True, text=True, check=check).stdout


def settle(target, quiet=1.0, limit=20.0):
    end, last, since = time.monotonic() + limit, None, time.monotonic()
    while time.monotonic() < end:
        now = tmux('capture-pane', '-p', '-e', '-N', '-t', target)
        if now != last:
            last, since = now, time.monotonic()
        elif time.monotonic() - since >= quiet:
            return now
        time.sleep(0.1)
    return last


GIT_ENV = {**os.environ, 'GIT_CONFIG_NOSYSTEM': '1', 'GIT_AUTHOR_NAME': 'Prompt', 'GIT_COMMITTER_NAME': 'Prompt',
           'GIT_AUTHOR_EMAIL': 'prompt@example.invalid', 'GIT_COMMITTER_EMAIL': 'prompt@example.invalid',
           'GIT_AUTHOR_DATE': '2026-01-01T00:00:00Z', 'GIT_COMMITTER_DATE': '2026-01-01T00:00:00Z'}


def git(cwd, *args, check=True):
    return subprocess.run(['git', *args], cwd=cwd, check=check, capture_output=True, text=True,
                          env={**GIT_ENV, 'HOME': str(HOME)}).stdout


def commit(repo, name, text, message):
    (repo / name).write_text(text)
    git(repo, 'add', name)
    git(repo, 'commit', '-q', '-m', message)


def status(repo):
    """What gitstatus reports of `repo`, read with git itself."""
    st = dict(staged=0, unstaged=0, untracked=0, conflicted=0)
    for line in git(repo, 'status', '--porcelain=v2', '--branch').splitlines():
        kind = line[:2]
        if kind in ('1 ', '2 '):
            st['staged'] += line[2] != '.'
            st['unstaged'] += line[3] != '.'
        elif kind == 'u ':
            st['conflicted'] += 1
        elif kind == '? ':
            st['untracked'] += 1
    branch = git(repo, 'symbolic-ref', '-q', '--short', 'HEAD', check=False).strip()
    if branch:
        st['branch'] = branch
    else:
        st['commit'] = git(repo, 'rev-parse', 'HEAD').strip()
    upstream = git(repo, 'for-each-ref', '--format=%(upstream:short)', f'refs/heads/{branch}').strip() if branch else ''
    if upstream:
        ahead, behind = git(repo, 'rev-list', '--left-right', '--count', 'HEAD...@{upstream}').split()
        st.update(remote_branch=upstream.split('/', 1)[1], ahead=int(ahead), behind=int(behind),
                  remote_url=git(repo, 'remote', 'get-url', upstream.split('/', 1)[0]).strip())
    stashes = len(git(repo, 'stash', 'list').splitlines())
    if stashes:
        st['stashes'] = stashes
    if (Path(repo) / '.git' / 'MERGE_HEAD').exists():
        st['action'] = 'merge'
    st['summary'] = git(repo, 'log', '-1', '--format=%s').strip()
    return {k: v for k, v in st.items() if v not in (0, '')}


def fixtures():
    shutil.rmtree(HOME, ignore_errors=True)
    shutil.rmtree(REMOTE, ignore_errors=True)
    for d in (DEEPER, REPO_DEEP, *SIBLINGS, DETACHED, MERGING):
        d.mkdir(parents=True)
    MARKER.write_text('{"name": "user"}\n')
    git(REPO, 'init', '-q', '-b', 'main')
    (REPO / 'kernel.c').write_text('int main(void) { return 0; }\n')
    commit(REPO, 'README.md', '# emblink\n', 'Initial')
    git(REPO, 'add', 'kernel.c'); git(REPO, 'commit', '-q', '-m', 'The kernel')
    (REPO / 'README.md').write_text('# emblink\n\nChanged, not staged.\n')        # unstaged: 1
    (REPO / 'shell.c').write_text('/* staged */\n'); git(REPO, 'add', 'shell.c')   # staged: 1
    (REPO / 'notes.txt').write_text('untracked\n')                                 # untracked: 2
    (REPO / 'todo.txt').write_text('untracked\n')
    # fork: 2 ahead of origin/main, 1 behind, a stash, a wip commit last
    git(HOME, 'init', '-q', '--bare', '-b', 'main', str(REMOTE))
    git(HOME / 'projects', 'clone', '-q', str(REMOTE), 'fork')
    commit(FORK, 'a.txt', 'one\n', 'One'); git(FORK, 'push', '-q', 'origin', 'main')
    other = Path(tempfile.mkdtemp(prefix='emblink-prompt-other-'))
    git(other, 'clone', '-q', str(REMOTE), 'o')
    commit(other / 'o', 'b.txt', 'two\n', 'Two'); git(other / 'o', 'push', '-q', 'origin', 'main')
    shutil.rmtree(other)
    commit(FORK, 'c.txt', 'three\n', 'Three'); commit(FORK, 'd.txt', 'four\n', 'wip: four')
    git(FORK, 'fetch', '-q')
    (FORK / 'a.txt').write_text('stashed\n'); git(FORK, 'stash', '-q')
    # detached: HEAD on the first of two commits
    git(DETACHED, 'init', '-q', '-b', 'main')
    commit(DETACHED, 'a.txt', 'one\n', 'One'); commit(DETACHED, 'a.txt', 'two\n', 'Two')
    git(DETACHED, 'checkout', '-q', '--detach', 'HEAD~1')
    # merging: main and other both changed f.txt; the merge stops on it
    git(MERGING, 'init', '-q', '-b', 'main')
    commit(MERGING, 'f.txt', 'base\n', 'Base')
    git(MERGING, 'checkout', '-q', '-b', 'other'); commit(MERGING, 'f.txt', 'other\n', 'Other')
    git(MERGING, 'checkout', '-q', 'main'); commit(MERGING, 'f.txt', 'main\n', 'Main')
    git(MERGING, 'merge', '-q', 'other', check=False)
    for repo, into in ((FORK, FORK_GIT), (DETACHED, DETACHED_GIT), (MERGING, MERGING_GIT)):
        into.clear()
        into.update(status(repo))


def listing():
    """The fixture's directories and files (inside .git, only .git itself),
    for tools/golden_prompt.py to stand up again."""
    dirs, files = [], []
    for root, sub, names in os.walk(HOME):
        here = Path(root)
        if here.name == '.git':
            sub[:] = []
            continue
        dirs += [str((here / d).relative_to(HOME)) for d in sub]
        files += [str((here / f).relative_to(HOME)) for f in names]
    return sorted(dirs), sorted(files)


def zshrc(configs, zdotdir):
    zdotdir.mkdir(parents=True, exist_ok=True)
    (zdotdir / '.zshrc').write_text(
        'HISTFILE=/dev/null\nunsetopt correct correct_all\n'
        'typeset -g POWERLEVEL9K_INSTANT_PROMPT=off\n'
        'typeset -g POWERLEVEL9K_DISABLE_HOT_RELOAD=true\n'
        f'deep={DEEP} deeper={DEEPER} repodeep={REPO_DEEP} fork={FORK} detached={DETACHED} merging={MERGING}\n'
        f'source {P10K}/powerlevel10k.zsh-theme\n' +
        ''.join(f'source {c}\n' for c in configs))


def clock(screen):
    """The time the current prompt showed -- the last on the screen, as older
    prompts may still be there: 24 hours or 12 with AM/PM, seconds or not."""
    found = re.findall(r'\b(\d\d):(\d\d)(?::(\d\d))?( [AP]M)?\b', re.sub(r'\x1b\[[0-9;:]*m', '', screen))
    if not found:
        return None
    h, m, sec, ampm = found[-1]
    h = int(h)
    if ampm:
        h = h % 12 + (12 if ampm == ' PM' else 0)
    return [h, int(m), int(sec or 0)]


def session(name, config, label, cols, rows, steps):
    target = f'{name}-{label}:0.0'
    zdotdir = Path(tempfile.mkdtemp(prefix='emblink-prompt-zdot-'))
    zshrc(config, zdotdir)
    env = {'HOME': str(HOME), 'ZDOTDIR': str(zdotdir), 'TZ': 'UTC', 'TERM': 'xterm-256color',
           'LANG': 'en_US.UTF-8', 'USER': 'yves', 'LOGNAME': 'yves', 'GIT_CONFIG_NOSYSTEM': '1'}
    exports = [a for k, v in env.items() for a in ('-e', f'{k}={v}')]
    tmux('new-session', '-d', '-s', f'{name}-{label}', '-x', str(cols), '-y', str(rows), *exports, '-c', str(HOME),
         'zsh -i; exec sleep 100000')
    shots = []
    try:
        screen = settle(target, quiet=2.0)
        for step in steps:
            if step['typed']:
                tmux('send-keys', '-t', target, '-l', step['typed'])
                tmux('send-keys', '-t', target, 'Enter')
                for key in step.get('keys', []):
                    time.sleep(0.5)
                    tmux('send-keys', '-t', target, key)
                if step['step'] == 'slow':
                    time.sleep(3.5)
                screen = settle(target)
            x, y = map(int, tmux('display', '-p', '-t', target, '#{cursor_x} #{cursor_y}').split())
            shot = dict(step=step['step'], typed=step['typed'], state={**step['state'], 'time': clock(screen)},
                        cursor=[x, y], screen=screen.split('\n'))
            if step.get('output'):
                shot['output'] = True
            shots.append(shot)
    finally:
        tmux('kill-session', '-t', f'{name}-{label}', check=False)
        shutil.rmtree(zdotdir, ignore_errors=True)
    return dict(name=label, cols=cols, rows=rows, steps=shots)


def record(name, config, fixture):
    sessions = [session(name, config, *s) for s in SESSIONS]
    text = json.dumps(sessions)
    private = [w for w in (os.environ.get('USER'), socket.gethostname().split('.')[0],
                           subprocess.run(['git', 'config', 'user.email'], capture_output=True, text=True).stdout.strip())
               if w and w != 'yves']
    for word in private:
        if word in text:
            sys.exit(f'record_prompts: {name} recorded {word!r}; refusing to keep it')
    GOLDEN.mkdir(parents=True, exist_ok=True)
    (GOLDEN / f'{name}.json').write_text(json.dumps(dict(
        config=[str(c.relative_to(ROOT)) for c in config], home=str(HOME), dirs=fixture[0], files=fixture[1],
        recorded=time.strftime('%Y-%m-%d'), reference=f"Powerlevel10k {P10K.name} under {tmux('-V').strip()}",
        sessions=sessions), indent=1, ensure_ascii=False) + '\n')
    print(f'{name}: ' + ', '.join(f'{s["name"]} {len(s["steps"])} steps' for s in sessions), flush=True)


def main():
    if sys.platform != 'darwin':
        sys.exit('record_prompts: record on macOS, where zsh and Powerlevel10k are')
    for tool in ('tmux', 'zsh', 'git'):
        if not shutil.which(tool):
            sys.exit(f'record_prompts: {tool} is required')
    if not (P10K / 'powerlevel10k.zsh-theme').exists():
        sys.exit(f'record_prompts: no Powerlevel10k at {P10K} (set POWERLEVEL10K)')
    CONF.write_text('set -g default-terminal xterm-256color\nset -g status off\nset -sg escape-time 0\n'
                    'set -g history-limit 0\n')
    fixtures()
    fixture = listing()
    names = sys.argv[1:] or list(CONFIGS)
    for name in names:
        record(name, CONFIGS[name], fixture)
    tmux('kill-server', check=False)


if __name__ == '__main__':
    main()
