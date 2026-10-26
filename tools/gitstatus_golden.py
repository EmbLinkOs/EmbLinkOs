#!/usr/bin/env python3
"""Hold EmbLinkOS's gitstatus daemon to gitstatusd itself.

The prompt's git counts come from user/gitstatus, a daemon that speaks
gitstatusd's protocol (requests `id US dir US 0 RS`, responses of 27 fields).
This builds the same repositories every time -- fixed names and dates, so the
same commits and hashes -- in every state gitstatusd reports: dirty in each
way, ignored files and untracked directories, conflicts, a merge, rebase,
cherry-pick, revert, bisect and am under way, detached heads and tags, an
upstream and a push remote ahead and behind, stashes, packed objects and refs,
index v4, skip-worktree and assume-unchanged, symlinks and the exec bit,
linked worktrees, submodules, nested repositories, unborn branches -- and asks
each one under several sets of flags; then one repository changed between
questions to the same daemon, which must notice every change.

    python3 tools/gitstatus_golden.py record   # gitstatusd's answers -> tests/gitstatus/golden.json
    python3 tools/gitstatus_golden.py          # the host build's answers, held to them

Recording needs gitstatusd (Powerlevel10k ships it; GITSTATUSD=path). Both
need git to build the repositories. Paths in answers are written relative to
the fixture root, so the recording travels.
"""
import json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / 'tests' / 'gitstatus' / 'golden.json'
OURS = Path(os.environ.get('GITSTATUS_HOST', ROOT / 'build' / 'gitstatus_host'))
GITSTATUSD = os.environ.get('GITSTATUSD', next(iter(sorted(Path('/opt/homebrew/Cellar').glob(
    'powerlevel10k/*/share/powerlevel10k/gitstatus/usrbin/gitstatusd-*'))), ''))
FIELDS = ['workdir', 'commit', 'branch', 'upstream', 'remote', 'remote_url', 'action', 'index_size', 'staged',
          'unstaged', 'conflicted', 'untracked', 'ahead', 'behind', 'stashes', 'tag', 'unstaged_deleted',
          'staged_new', 'staged_deleted', 'push_remote', 'push_remote_url', 'push_ahead', 'push_behind',
          'skip_worktree', 'assume_unchanged', 'encoding', 'summary']
FLAGS = {
    'all': ['-s', '-1', '-u', '-1', '-c', '-1', '-d', '-1', '-m', '-1'],
    'all-e': ['-s', '-1', '-u', '-1', '-c', '-1', '-d', '-1', '-m', '-1', '-e'],
    'capped': [],
    'dirty-max': ['-s', '-1', '-u', '-1', '-c', '-1', '-d', '-1', '-m', '2'],
    'flags-UWD': ['-s', '-1', '-u', '-1', '-c', '-1', '-d', '-1', '-m', '-1', '-U', '-W', '-D'],
}
ENV = {'GIT_AUTHOR_NAME': 'Fixture', 'GIT_COMMITTER_NAME': 'Fixture',
       'GIT_AUTHOR_EMAIL': 'fixture@example.invalid', 'GIT_COMMITTER_EMAIL': 'fixture@example.invalid',
       'GIT_AUTHOR_DATE': '2026-01-01T00:00:00Z', 'GIT_COMMITTER_DATE': '2026-01-01T00:00:00Z',
       'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': '/dev/null', 'GIT_EDITOR': 'true',
       'GIT_SEQUENCE_EDITOR': 'true', 'GIT_MERGE_AUTOEDIT': 'no', 'LC_ALL': 'C'}


def git(cwd, *args, check=True, env=None):
    r = subprocess.run(['git', *args], cwd=cwd, capture_output=True, text=True,
                       env={**os.environ, **ENV, **(env or {})})
    if check and r.returncode:
        raise SystemExit(f'git {" ".join(args)} in {cwd}: {r.stderr.strip()}')
    return r.stdout


def write(path, text, mode=None):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    if mode is not None:
        path.chmod(mode)


def repo(root, name, branch='main'):
    r = root / name
    r.mkdir(parents=True)
    git(r, 'init', '-q', '-b', branch)
    return r


def commit(r, files, message, env=None):
    for name, text in files.items():
        if text is None:
            git(r, 'rm', '-q', name)
        else:
            write(r / name, text)
            git(r, 'add', name)
    git(r, 'commit', '-q', '--allow-empty', '-m', message, env=env)


# --- the repositories --------------------------------------------------------------

def s_not_a_repo(root):
    d = root / 'plain'
    write(d / 'file.txt', 'not tracked by anything\n')
    return [d]


def s_unborn(root):
    r = repo(root, 'unborn')
    write(r / 'staged.txt', 'staged before the first commit\n')
    git(r, 'add', 'staged.txt')
    write(r / 'loose.txt', 'untracked\n')
    return [r]


def s_clean(root):
    r = repo(root, 'clean')
    commit(r, {'README.md': '# clean\n', 'src/main.c': 'int main(void) { return 0; }\n'}, 'Initial')
    commit(r, {'src/util.c': '/* util */\n'}, 'Add util\n\nA longer body\nthat is not the summary.')
    return [r, r / 'src']


def s_dirty(root):
    r = repo(root, 'dirty')
    commit(r, {'a.txt': 'a\n', 'b.txt': 'b\n', 'c.txt': 'c\n', 'd.txt': 'd\n', 'gone.txt': 'g\n',
               'lib/x.c': 'x\n', 'lib/y.c': 'y\n', '.gitignore': '*.log\nbuild/\n!keep.log\n',
               'docs/.gitignore': 'draft*\n'}, 'Initial')
    write(r / 'a.txt', 'a changed, unstaged\n')                       # unstaged
    write(r / 'b.txt', 'b changed, staged\n'); git(r, 'add', 'b.txt')  # staged
    write(r / 'c.txt', 'c staged\n'); git(r, 'add', 'c.txt')
    write(r / 'c.txt', 'c staged, then changed again\n')               # staged and unstaged
    (r / 'd.txt').unlink()                                             # unstaged deleted
    git(r, 'rm', '-q', 'gone.txt')                                     # staged deleted
    write(r / 'new.txt', 'new, staged\n'); git(r, 'add', 'new.txt')    # staged new
    write(r / 'lib/x.c', 'x changed\n')
    write(r / 'untracked.txt', 'u\n')                                  # untracked
    write(r / 'more/one.txt', '1\n'); write(r / 'more/deeper/two.txt', '2\n')   # an untracked dir
    write(r / 'debug.log', 'ignored\n'); write(r / 'keep.log', 'not ignored\n')
    write(r / 'build/out.o', 'ignored dir\n')
    write(r / 'docs/draft1.md', 'ignored by docs/.gitignore\n'); write(r / 'docs/final.md', 'untracked\n')
    write(r / 'only-ignored/a.log', 'a dir holding only ignored files\n')
    write(r / '.git/info/exclude', 'secret.txt\n'); write(r / 'secret.txt', 'excluded\n')
    (r / 'empty-dir').mkdir()
    return [r, r / 'lib', r / 'more']


def s_conflict(root):
    r = repo(root, 'conflict')
    commit(r, {'f.txt': 'base\n', 'g.txt': 'base\n', 'h.txt': 'base\n'}, 'Base')
    git(r, 'checkout', '-q', '-b', 'other')
    commit(r, {'f.txt': 'other\n', 'g.txt': 'other\n', 'h.txt': 'base\nmore\n'}, 'Other')
    git(r, 'checkout', '-q', 'main')
    commit(r, {'f.txt': 'main\n', 'g.txt': 'main\n'}, 'Main')
    git(r, 'merge', '-q', 'other', check=False)                        # f and g conflict; h merges
    return [r]


def s_actions(root):
    out = []
    # rebase, interactive, stopped on an edit
    r = repo(root, 'rebase-i')
    commit(r, {'f.txt': '1\n'}, 'One'); commit(r, {'f.txt': '2\n'}, 'Two'); commit(r, {'f.txt': '3\n'}, 'Three')
    git(r, 'rebase', '-q', '-i', 'HEAD~2', env={'GIT_SEQUENCE_EDITOR': "sed -i.bak 's/^pick/edit/'"})
    out.append(r)
    # rebase, stopped on a conflict
    r = repo(root, 'rebase-m')
    commit(r, {'f.txt': 'base\n'}, 'Base')
    git(r, 'checkout', '-q', '-b', 'topic'); commit(r, {'f.txt': 'topic\n'}, 'Topic')
    git(r, 'checkout', '-q', 'main'); commit(r, {'f.txt': 'main\n'}, 'Main')
    git(r, 'checkout', '-q', 'topic'); git(r, 'rebase', '-q', 'main', check=False)
    out.append(r)
    # cherry-pick and revert, each stopped on a conflict
    for kind in ('cherry-pick', 'revert'):
        r = repo(root, kind)
        commit(r, {'f.txt': 'base\n'}, 'Base')
        git(r, 'checkout', '-q', '-b', 'side'); commit(r, {'f.txt': 'side\n'}, 'Side')
        git(r, 'checkout', '-q', 'main'); commit(r, {'f.txt': 'main\n'}, 'Main')
        if kind == 'cherry-pick':
            git(r, 'cherry-pick', 'side', check=False)
        else:
            commit(r, {'f.txt': 'main again\n'}, 'Again')
            git(r, 'revert', '--no-edit', 'HEAD~1', check=False)
        out.append(r)
    # cherry-pick of a range, stopped on the first: a sequence
    r = repo(root, 'cherry-seq')
    commit(r, {'f.txt': 'base\n'}, 'Base')
    git(r, 'checkout', '-q', '-b', 'side'); commit(r, {'f.txt': 'side 1\n'}, 'S1'); commit(r, {'g.txt': 's2\n'}, 'S2')
    git(r, 'checkout', '-q', 'main'); commit(r, {'f.txt': 'main\n'}, 'Main')
    git(r, 'cherry-pick', 'main..side', check=False)
    out.append(r)
    # rebase with the apply backend, stopped on a conflict
    r = repo(root, 'rebase-apply')
    commit(r, {'f.txt': 'base\n'}, 'Base')
    git(r, 'checkout', '-q', '-b', 'topic'); commit(r, {'f.txt': 'topic\n'}, 'Topic')
    git(r, 'checkout', '-q', 'main'); commit(r, {'f.txt': 'main\n'}, 'Main')
    git(r, 'checkout', '-q', 'topic'); git(r, 'rebase', '-q', '--apply', 'main', check=False)
    out.append(r)
    # revert of a range, stopped on the first: a sequence
    r = repo(root, 'revert-seq')
    commit(r, {'f.txt': 'base\n'}, 'Base'); commit(r, {'f.txt': 'one\n'}, 'One'); commit(r, {'f.txt': 'two\n'}, 'Two')
    write(r / 'f.txt', 'local\n'); git(r, 'commit', '-q', '-am', 'Local')
    git(r, 'revert', '--no-edit', 'HEAD~3..HEAD~1', check=False)
    out.append(r)
    # bisect
    r = repo(root, 'bisect')
    for i in range(4):
        commit(r, {'f.txt': f'{i}\n'}, f'C{i}')
    git(r, 'bisect', 'start'); git(r, 'bisect', 'bad'); git(r, 'bisect', 'good', 'HEAD~3')
    out.append(r)
    # am, stopped on a patch that does not apply
    r = repo(root, 'am')
    commit(r, {'f.txt': 'base\n'}, 'Base')
    git(r, 'checkout', '-q', '-b', 'side'); commit(r, {'f.txt': 'side\n'}, 'Side')
    patch = git(r, 'format-patch', '--stdout', 'HEAD~1')
    git(r, 'checkout', '-q', 'main'); commit(r, {'f.txt': 'main\n'}, 'Main')
    write(root / 'am.patch', patch)
    git(r, 'am', str(root / 'am.patch'), check=False)
    out.append(r)
    return out


def s_detached_tags(root):
    r = repo(root, 'tags')
    commit(r, {'f.txt': '1\n'}, 'One')
    git(r, 'tag', 'v1.0'); git(r, 'tag', '-a', 'v1.0-annotated', '-m', 'Annotated'); git(r, 'tag', 'alpha')
    commit(r, {'f.txt': '2\n'}, 'Two')
    git(r, 'tag', '-a', 'zeta', '-m', 'Annotated, last in order')
    git(r, 'checkout', '-q', '--detach', 'HEAD~1')
    r2 = repo(root, 'tags-on-branch')
    commit(r2, {'f.txt': '1\n'}, 'One')
    git(r2, 'tag', 'release-2'); git(r2, 'tag', 'release-10')
    return [r, r2]


def s_upstream(root):
    remote = root / 'remote.git'
    git(root, 'init', '-q', '--bare', '-b', 'main', str(remote))
    fork = root / 'fork'
    git(root, 'clone', '-q', str(remote), 'fork')
    commit(fork, {'a.txt': '1\n'}, 'One'); git(fork, 'push', '-q', 'origin', 'main')
    other = root / 'other-clone'
    git(root, 'clone', '-q', str(remote), 'other-clone')
    commit(other, {'b.txt': '2\n'}, 'Two'); commit(other, {'c.txt': '3\n'}, 'Three')
    git(other, 'push', '-q', 'origin', 'main')
    commit(fork, {'d.txt': '4\n'}, 'Four')
    git(fork, 'fetch', '-q')
    write(fork / 'a.txt', 'stash me\n'); git(fork, 'stash', '-q')
    write(fork / 'a.txt', 'stash me too\n'); git(fork, 'stash', '-q')
    # remote.pushDefault: a push remote of its own, further behind
    mirror = root / 'mirror.git'
    git(root, 'init', '-q', '--bare', '-b', 'main', str(mirror))
    pushy = root / 'push-default'
    git(root, 'clone', '-q', str(remote), 'push-default')
    git(pushy, 'remote', 'add', 'mirror', str(mirror))
    git(pushy, 'push', '-q', 'mirror', 'main~1:refs/heads/main')
    git(pushy, 'fetch', '-q', 'mirror')
    git(pushy, 'config', 'remote.pushDefault', 'mirror')
    commit(pushy, {'e.txt': '5\n'}, 'Five')
    # branch.<name>.pushRemote, its branch there, and an upstream of another name
    branchy = root / 'push-branch'
    git(root, 'clone', '-q', str(remote), 'push-branch')
    git(branchy, 'checkout', '-q', '-b', 'feature', '--track', 'origin/main')
    commit(branchy, {'f.txt': '6\n'}, 'Six')
    git(branchy, 'push', '-q', 'origin', 'feature')
    commit(branchy, {'g.txt': '7\n'}, 'Seven')
    git(branchy, 'config', 'branch.feature.pushRemote', 'origin')
    # a pushRemote whose branch is not there
    nowhere = root / 'push-missing'
    git(root, 'clone', '-q', str(remote), 'push-missing')
    git(nowhere, 'checkout', '-q', '-b', 'topic', '--track', 'origin/main')
    git(nowhere, 'config', 'branch.topic.pushRemote', 'origin')
    # an upstream configured, its remote-tracking branch never fetched
    lost = root / 'upstream-missing'
    git(root, 'clone', '-q', str(remote), 'upstream-missing')
    git(lost, 'checkout', '-q', '-b', 'orphaned')
    git(lost, 'config', 'branch.orphaned.remote', 'origin')
    git(lost, 'config', 'branch.orphaned.merge', 'refs/heads/never-pushed')
    return [fork, pushy, branchy, nowhere, lost]


def s_packed(root):
    r = repo(root, 'packed')
    text = ''.join(f'line {i}\n' for i in range(200))
    for i in range(6):
        text = text.replace(f'line {i * 7}\n', f'line {i * 7} edited {i}\n')
        commit(r, {'big.txt': text, f'dir{i % 2}/f{i}.txt': f'{i}\n'}, f'Edit {i}')
    git(r, 'tag', '-a', 'packed-tag', '-m', 'Tagged before gc')
    git(r, 'checkout', '-q', '-b', 'side'); commit(r, {'side.txt': 's\n'}, 'Side'); git(r, 'checkout', '-q', 'main')
    git(r, 'gc', '-q', '--aggressive', '--prune=now')
    write(r / 'big.txt', text + 'and an unstaged line\n')
    write(r / 'dir0/new.txt', 'untracked\n')
    return [r]


def s_index_flags(root):
    r = repo(root, 'index-v4')
    commit(r, {f'd/f{i}.txt': f'{i}\n' for i in range(5)}, 'Five files')
    git(r, 'update-index', '--index-version', '4')
    write(r / 'd/f1.txt', 'changed\n')
    r2 = repo(root, 'flags')
    commit(r2, {'skip.txt': 's\n', 'assume.txt': 'a\n', 'plain.txt': 'p\n'}, 'Three')
    git(r2, 'update-index', '--skip-worktree', 'skip.txt')
    git(r2, 'update-index', '--assume-unchanged', 'assume.txt')
    write(r2 / 'skip.txt', 'changed under skip-worktree\n')
    write(r2 / 'assume.txt', 'changed under assume-unchanged\n')
    write(r2 / 'plain.txt', 'changed\n')
    return [r, r2]


def s_links_modes(root):
    r = repo(root, 'links')
    write(r / 'target.txt', 't\n')
    os.symlink('target.txt', r / 'link')
    write(r / 'run.sh', '#!/bin/sh\n', mode=0o644)
    git(r, 'add', '.'); git(r, 'commit', '-q', '-m', 'Links and scripts')
    os.remove(r / 'link'); os.symlink('elsewhere.txt', r / 'link')    # the link's target changed
    (r / 'run.sh').chmod(0o755)                                        # the exec bit
    return [r]


def s_worktrees(root):
    r = repo(root, 'main-tree')
    commit(r, {'f.txt': '1\n'}, 'One')
    git(r, 'worktree', 'add', '-q', '-b', 'linked', str(root / 'linked-tree'))
    write(root / 'linked-tree' / 'f.txt', 'changed in the linked tree\n')
    write(root / 'linked-tree' / 'new.txt', 'untracked there\n')
    return [r, root / 'linked-tree']


def s_submodules(root):
    lib = repo(root, 'sublib')
    commit(lib, {'lib.c': '/* lib */\n'}, 'Lib')
    r = repo(root, 'super')
    commit(r, {'top.txt': 't\n'}, 'Top')
    git(r, '-c', 'protocol.file.allow=always', 'submodule', '-q', 'add', '../sublib', 'vendor/lib')
    git(r, 'commit', '-q', '-m', 'Add the submodule')
    r2 = repo(root, 'super-dirty')
    commit(r2, {'top.txt': 't\n'}, 'Top')
    git(r2, '-c', 'protocol.file.allow=always', 'submodule', '-q', 'add', '../sublib', 'vendor/lib')
    git(r2, 'commit', '-q', '-m', 'Add the submodule')
    write(r2 / 'vendor/lib/lib.c', '/* changed inside the submodule */\n')
    r3 = repo(root, 'super-moved')
    commit(r3, {'top.txt': 't\n'}, 'Top')
    git(r3, '-c', 'protocol.file.allow=always', 'submodule', '-q', 'add', '../sublib', 'vendor/lib')
    git(r3, 'commit', '-q', '-m', 'Add the submodule')
    commit(r3 / 'vendor/lib', {'more.c': '/* a new commit in the submodule */\n'}, 'More')
    nested = r / 'nested-repo'
    nested.mkdir()
    git(nested, 'init', '-q')
    write(nested / 'inside.txt', 'in a repository of its own\n')
    return [r, r / 'vendor' / 'lib', r2, r3]


def s_config(root):
    out = []
    for name, key in (('show-untracked-no', 'status.showUntrackedFiles=no'),
                      ('bash-untracked-false', 'bash.showUntrackedFiles=false'),
                      ('bash-dirty-false', 'bash.showDirtyState=false')):
        r = repo(root, name)
        commit(r, {'f.txt': '1\n'}, 'One')
        k, v = key.split('=')
        git(r, 'config', k, v)
        write(r / 'f.txt', 'changed\n'); write(r / 'u.txt', 'untracked\n')
        write(r / 'g.txt', 'staged\n'); git(r, 'add', 'g.txt')
        out.append(r)
    return out


def s_encoding(root):
    r = repo(root, 'encoding')
    git(r, 'config', 'i18n.commitEncoding', 'ISO-8859-1')
    write(r / 'f.txt', '1\n'); git(r, 'add', 'f.txt')
    git(r, 'commit', '-q', '-m', '  A summary   on\n  two lines\n\nand a body')
    return [r]


SCENARIOS = [s_not_a_repo, s_unborn, s_clean, s_dirty, s_conflict, s_actions, s_detached_tags, s_upstream,
             s_packed, s_index_flags, s_links_modes, s_worktrees, s_submodules, s_config, s_encoding]


def build(root):
    """Every repository; [(label, dir)] to ask about."""
    asks = []
    for s in SCENARIOS:
        for d in s(root):
            asks.append((str(d.relative_to(root)), d))
    return asks


def ask(binary, flags, dirs, home):
    """One daemon, every dir: [response fields]. Its HOME is empty: no global
    git configuration (an excludes file, say) reaches either daemon."""
    req = ''.join(f'{i}\x1f{d}\x1f0\x1e' for i, (_, d) in enumerate(dirs))
    env = {k: v for k, v in os.environ.items() if not k.startswith(('GIT_', 'XDG_'))}
    env['HOME'] = str(home)
    r = subprocess.run([str(binary), *flags, '-v', 'FATAL'], input=req, capture_output=True, text=True,
                       timeout=120, env=env)
    out = [rec.split('\x1f') for rec in r.stdout.split('\x1e') if rec]
    if len(out) != len(dirs):
        raise SystemExit(f'{binary}: {len(out)} answers for {len(dirs)} questions\n{r.stderr}')
    return out


# One daemon, one repository, a change between questions: what it keeps
# (the HEAD tree, files' names, directory listings) must follow.
def change_steps(r):
    yield 'clean'
    write(r / 'src/new.txt', 'untracked in a listed directory\n'); yield 'untracked in src'
    write(r / 'src/a.c', 'A\n'); yield 'same size, new contents'
    git(r, 'add', 'src/a.c'); yield 'staged'
    git(r, 'commit', '-q', '-m', 'Second'); yield 'committed'
    (r / 'src/new.txt').unlink(); yield 'untracked gone'
    write(r / 'logs/x.log', 'x\n'); yield 'untracked directory'
    write(r / '.gitignore', 'logs/\n'); yield 'now ignored'
    write(r / 'src/a.c', 'B\n'); yield 'changed again, same size'
    git(r, 'stash', '-q'); yield 'stashed'
    git(r, 'checkout', '-q', '-b', 'topic'); yield 'another branch'


def sequence(binary, root, home):
    r = repo(root, 'changes')
    commit(r, {'src/a.c': 'a\n', 'src/b.c': 'b\n', 'README': 'r\n'}, 'First')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('GIT_', 'XDG_'))}
    env['HOME'] = str(home)
    p = subprocess.Popen([str(binary), *FLAGS['all'], '-v', 'FATAL'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         text=True, env=env)
    out = []
    try:
        for i, step in enumerate(change_steps(r)):
            p.stdin.write(f'{i}\x1f{r}\x1f0\x1e')
            p.stdin.flush()
            buf = ''
            while not buf.endswith('\x1e'):
                ch = p.stdout.read(1)
                if not ch:
                    raise SystemExit(f'{binary}: no answer at {step}')
                buf += ch
            out.append((step, normalise(buf[:-1].split('\x1f'), root)))
    finally:
        p.stdin.close()
        p.wait()
    return out


def normalise(resp, root):
    if resp[1] != '1':
        return {'repo': False}
    d = dict(zip(FIELDS, resp[2:]))
    real = str(Path(root).resolve())
    for k in ('workdir', 'remote_url', 'push_remote_url'):
        d[k] = d[k].replace(real, '{root}').replace(str(root), '{root}')
    d['repo'] = True
    return d


def main():
    record = sys.argv[1:] == ['record']
    root = Path(tempfile.mkdtemp(prefix='emblink-gitstatus-')).resolve()
    try:
        dirs = build(root / 'repos')
        home = root / 'home'
        home.mkdir()
        if record:
            if not GITSTATUSD or not Path(GITSTATUSD).exists():
                sys.exit('gitstatus_golden: no gitstatusd (set GITSTATUSD)')
            version = subprocess.run([GITSTATUSD, '--version'], capture_output=True, text=True).stdout.strip()
            golden = {'reference': f'gitstatusd {version}', 'git': subprocess.run(
                ['git', '--version'], capture_output=True, text=True).stdout.strip(), 'answers': {}}
            for name, flags in FLAGS.items():
                answers = ask(GITSTATUSD, flags, dirs, home)
                golden['answers'][name] = {label: normalise(a, root / 'repos') for (label, _), a in zip(dirs, answers)}
            golden['sequence'] = sequence(GITSTATUSD, root / 'seq', home)
            GOLDEN.parent.mkdir(parents=True, exist_ok=True)
            GOLDEN.write_text(json.dumps(golden, indent=1, sort_keys=True) + '\n')
            print(f'gitstatus_golden: {len(dirs)} directories x {len(FLAGS)} flag sets from {golden["reference"]}')
            return
        if not OURS.exists():
            sys.exit(f'gitstatus_golden: {OURS} missing (make gitstatus-test builds it)')
        golden = json.loads(GOLDEN.read_text())
        bad = total = 0
        for name, flags in FLAGS.items():
            answers = ask(OURS, flags, dirs, home)
            for (label, _), a in zip(dirs, answers):
                want, got = golden['answers'][name][label], normalise(a, root / 'repos')
                total += 1
                diff = {k: (want.get(k), got.get(k)) for k in sorted(set(want) | set(got)) if want.get(k) != got.get(k)}
                if diff:
                    bad += 1
                    print(f'{name} {label}: ' + ', '.join(f'{k} want {w!r} got {g!r}' for k, (w, g) in diff.items()))
        for (step, want), (_, got) in zip(golden['sequence'], sequence(OURS, root / 'seq', home)):
            total += 1
            diff = {k: (want.get(k), got.get(k)) for k in sorted(set(want) | set(got)) if want.get(k) != got.get(k)}
            if diff:
                bad += 1
                print(f'sequence, {step}: ' + ', '.join(f'{k} want {w!r} got {g!r}' for k, (w, g) in diff.items()))
        print(f'gitstatus: {total - bad}/{total} answers match {golden["reference"]}')
        sys.exit(1 if bad else 0)
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == '__main__':
    main()
