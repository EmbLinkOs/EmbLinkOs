#!/usr/bin/env python3
"""gitclone's checkout, held to git: a repository with directories, an
executable, a link and a submodule is packed as a server would send it,
written out by user/git (tests/git/checkout_host.c, the code gitclone runs),
and then git itself must find it whole and clean -- `git fsck`, and nothing
in `git status` -- as must the gitstatus daemon.

    python3 tools/gitclone_index_test.py     # make gitclone-index-test builds the helpers
"""
import os, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / 'build' / 'checkout_host'
GITSTATUS = ROOT / 'build' / 'gitstatus_host'
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gitstatus_golden import ENV, git, write, commit, repo  # noqa: E402


def main():
    root = Path(tempfile.mkdtemp(prefix='emblink-checkout-')).resolve()
    try:
        lib = repo(root, 'lib')
        commit(lib, {'lib.c': '/* lib */\n'}, 'Lib')
        src = repo(root, 'src')
        commit(src, {'README.md': '# r\n', 'a/b/c.txt': 'deep\n', 'a/d.txt': 'd\n', 'z.txt': 'z\n'}, 'Files')
        write(src / 'run.sh', '#!/bin/sh\necho hi\n', mode=0o755)
        os.symlink('a/d.txt', src / 'link')
        git(src, 'add', 'run.sh', 'link')
        git(src, '-c', 'protocol.file.allow=always', 'submodule', '-q', 'add', '../lib', 'vendor/lib')
        git(src, 'commit', '-q', '-m', 'Script, link, submodule')
        head = git(src, 'rev-parse', 'HEAD').strip()
        pack = root / 'all.pack'
        with open(pack, 'wb') as f:
            subprocess.run(['git', 'pack-objects', '--stdout', '--revs', '-q'], cwd=src, input=b'HEAD\n', stdout=f,
                           check=True, env={**os.environ, **ENV})
        out = root / 'clone'
        subprocess.run([str(HOST), str(pack), str(out), 'main', head], check=True)
        problems = []
        fsck = subprocess.run(['git', 'fsck', '--no-progress'], cwd=out, capture_output=True, text=True,
                              env={**os.environ, **ENV})
        if fsck.returncode:
            problems.append(f'git fsck: {fsck.stdout}{fsck.stderr}')
        status = git(out, 'status', '--porcelain', '--ignore-submodules=none')
        if status.strip():
            problems.append(f'git status is not clean:\n{status}')
        if (out / 'link').is_symlink() is False:
            problems.append('link is not a link')
        if GITSTATUS.exists():
            ans = subprocess.run([str(GITSTATUS), '-s', '-1', '-u', '-1', '-c', '-1', '-d', '-1'], input=f'1\x1f{out}\x1f0\x1e',
                                 capture_output=True, text=True).stdout.split('\x1f')
            counts = dict(zip(('staged', 'unstaged', 'conflicted', 'untracked'), ans[10:14]))
            if any(v != '0' for v in counts.values()):
                problems.append(f'gitstatus: {counts}')
        for p in problems:
            print('FAIL', p)
        print('gitclone checkout: ' + ('clean, as git and gitstatus read it' if not problems else f'{len(problems)} problems'))
        sys.exit(1 if problems else 0)
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == '__main__':
    main()
