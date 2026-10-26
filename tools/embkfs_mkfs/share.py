"""/system/share on the image, and the dev user's sample project: one list
for both image builders.

mkfs_embkfs.py (x86) and mkfs_arm64.py both install exactly these.

    /system/share/powerlevel10k/p10k-<style>.zsh
        the configurations `p10k configure` starts from, which the shell's
        `prompt style NAME` loads (docs/SHELL.md, "The prompt"); MIT, licence
        in system/licenses/powerlevel10k-MIT.txt
    /home/<dev user>/Projects/hello
        a small git repository -- history packed, a tag, one commit ahead of
        origin/main -- for the prompt's git status to show (and to try git on)
"""

import hashlib, os, shutil, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

P10K_STYLES = ("lean", "lean-8colors", "classic", "rainbow", "pure")


def share_files():
    """[(image path bytes, data)]. A missing file is a broken checkout and
    stops the build."""
    out = []
    for style in P10K_STYLES:
        name = f"p10k-{style}.zsh"
        with open(os.path.join(ROOT, "third_party", "powerlevel10k", "config", name), "rb") as fh:
            out.append((("system/share/powerlevel10k/" + name).encode(), fh.read()))
    return out


GIT_ENV = {'GIT_AUTHOR_NAME': 'EmbLink', 'GIT_COMMITTER_NAME': 'EmbLink',
           'GIT_AUTHOR_EMAIL': 'hello@example.com', 'GIT_COMMITTER_EMAIL': 'hello@example.com',
           'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': '/dev/null', 'LC_ALL': 'C'}
HELLO = [  # (date, message, {path: text})
    ('2026-01-05T10:00:00Z', 'Hello, EmbLink', {
        'README.md': '# hello\n\nA first program for EmbLinkOS.\n',
        'src/main.c': '#include <stdio.h>\n\nint main(void) {\n    puts("hello");\n    return 0;\n}\n',
        '.gitignore': 'build/\n*.o\n'}),
    ('2026-01-06T10:00:00Z', 'Say who is greeted', {
        'src/main.c': '#include <stdio.h>\n\nint main(int argc, char **argv) {\n'
                      '    printf("hello, %s\\n", argc > 1 ? argv[1] : "world");\n    return 0;\n}\n'}),
    ('2026-01-07T10:00:00Z', 'Build with make', {
        'Makefile': 'build/hello: src/main.c\n\tmkdir -p build\n\tcc -o $@ $<\n'}),
]


def _git(cwd, *args, date=None):
    env = {**os.environ, **GIT_ENV}
    if date:
        env['GIT_AUTHOR_DATE'] = env['GIT_COMMITTER_DATE'] = date
    return subprocess.run(['git', *args], cwd=cwd, env=env, check=True, capture_output=True, text=True).stdout


def _plain_index(data):
    """The index with its stat fields zeroed and its checksum made again: the
    same image every build (gitstatus compares contents when stat data does
    not agree)."""
    if data[:4] != b'DIRC' or struct.unpack('>I', data[4:8])[0] != 2:
        return data
    count, out, pos = struct.unpack('>I', data[8:12])[0], bytearray(data[:12]), 12
    for _ in range(count):
        entry = bytearray(data[pos:pos + 62])
        entry[0:24] = bytes(24)                    # ctime, mtime, dev, ino
        entry[28:36] = bytes(8)                    # uid, gid
        flags = struct.unpack('>H', entry[60:62])[0]
        end = data.index(b'\0', pos + 62)
        size = (end - pos + 8) & ~7
        out += entry + data[pos + 62:pos + size]
        pos += size
    out += data[pos:len(data) - 20]                # extensions as they were
    return bytes(out) + hashlib.sha1(bytes(out)).digest()


def sample_files(home):
    """[(image path bytes, data)] for the sample project in `home` (b"home/yves").
    Needs git on the host; without it there is no sample, and it says so."""
    try:
        subprocess.run(['git', '--version'], check=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError):
        print('share: no git on this host -- the image has no sample repository', file=sys.stderr)
        return []
    tmp = tempfile.mkdtemp(prefix='emblink-hello-')
    try:
        repo = os.path.join(tmp, 'hello')
        _git(tmp, 'init', '-q', '--template=', '-b', 'main', repo)
        _git(repo, 'config', 'remote.origin.url', 'https://example.com/emblink/hello.git')
        _git(repo, 'config', 'remote.origin.fetch', '+refs/heads/*:refs/remotes/origin/*')
        _git(repo, 'config', 'branch.main.remote', 'origin')
        _git(repo, 'config', 'branch.main.merge', 'refs/heads/main')
        for i, (date, message, files) in enumerate(HELLO):
            for path, text in files.items():
                full = os.path.join(repo, path)
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, 'w') as f:
                    f.write(text)
            _git(repo, 'add', '-A')
            _git(repo, 'commit', '-q', '-m', message, date=date)
            if i == 0:
                _git(repo, 'tag', '-a', 'v0.1', '-m', 'The first greeting', date=date)
            if i == len(HELLO) - 2:                # origin has all but the last
                _git(repo, 'update-ref', 'refs/remotes/origin/main', 'HEAD')
                _git(repo, '-c', 'pack.threads=1', 'gc', '-q', '--prune=now', date=date)
        out = []
        for root, dirs, files in os.walk(repo):
            dirs.sort()
            for name in sorted(files):
                full = os.path.join(root, name)
                rel = os.path.relpath(full, repo).replace(os.sep, '/')
                if rel.startswith('.git/logs/') or rel in ('.git/ORIG_HEAD', '.git/COMMIT_EDITMSG'):
                    continue                       # dated by the host's clock: not the same twice
                with open(full, 'rb') as f:
                    data = f.read()
                if rel == '.git/index':
                    data = _plain_index(data)
                out.append((home + b'/Projects/hello/' + rel.encode(), data))
        return out
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
