#!/usr/bin/env python3
"""Build pip.zip -- pip, repacked for EmbLinkOS's zipimport.

pip ships as a wheel (a DEFLATED zip). zipimport on this OS cannot inflate --
there is no `zlib` module (same constraint as tools/mkpystdlib.py) -- so a wheel
on sys.path fails to import. This unpacks the bundled pip wheel, PRECOMPILES each
module to .pyc (compiling pip's ~450 modules from source under TCG would be
brutally slow, exactly like the stdlib), and rewrites them into a STORED zip that
zipimport reads directly. Add the result to python.elf._pth and `python -m pip`
finds pip/__main__.pyc on sys.path.

The source wheel is CPython's own bundled copy, so pip always matches the
interpreter: Lib/ensurepip/_bundled/pip-*.whl.

usage: mkpip.py <python-src-dir> <out.zip>
"""
import glob
import os
import py_compile
import sys
import tempfile
import zipfile

# In-place source patches applied to wheel members before compile, keyed by the
# path inside the wheel. Each value is a list of (old, new) exact substrings;
# every one MUST match or the build aborts (no silent skips).
PATCHES = {
    # truststore uses the OS-native trust store via a memory-BIO SSLObject --
    # neither of which EmbLinkOS has (our TLS is _embtls verifying against
    # embedded roots). Force `import truststore` to raise ImportError so pip
    # takes its own documented fallback ("platform isn't supported") to the
    # standard certifi + ssl.create_default_context path, which our ssl.py shim
    # serves (proven by `test python https`).
    "pip/_vendor/truststore/__init__.py": [
        ('import sys as _sys\n',
         'import sys as _sys\n'
         'raise ImportError("truststore unsupported on EmbLinkOS: no memory-BIO '
         'SSL / native trust store; _embtls verifies vs embedded roots")\n'),
    ],
}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src_root, out = sys.argv[1], sys.argv[2]

    bundled = sorted(glob.glob(os.path.join(
        src_root, "Lib", "ensurepip", "_bundled", "pip-*.whl")))
    if not bundled:
        print(f"mkpip: no pip-*.whl under {src_root}/Lib/ensurepip/_bundled",
              file=sys.stderr)
        return 1
    whl = bundled[-1]

    n = 0
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    tmp = tempfile.TemporaryDirectory()
    tmpdir = tmp.name
    with zipfile.ZipFile(whl) as src, \
         zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        for name in sorted(src.namelist()):
            if name.endswith("/"):
                continue
            data = src.read(name)
            if name.endswith(".py"):
                patches = PATCHES.get(name)
                if patches:
                    text = data.decode("utf-8")
                    for old, new in patches:
                        if old not in text:
                            print(f"mkpip: patch for {name} did not match "
                                  f"(wheel changed?):\n  {old[:60]}...",
                                  file=sys.stderr)
                            return 1
                        text = text.replace(old, new)
                    data = text.encode("utf-8")
                # Precompile to an adjacent <mod>.pyc (the legacy layout
                # zipimport expects), UNCHECKED_HASH so it loads without the
                # absent source -- identical recipe to mkpystdlib.py.
                srcfile = os.path.join(tmpdir, "s.py")
                with open(srcfile, "wb") as f:
                    f.write(data)
                arc = name[:-3] + ".pyc"
                cfile = os.path.join(tmpdir, "m.pyc")
                py_compile.compile(
                    srcfile, cfile=cfile, dfile="/" + arc, doraise=True,
                    invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH)
                with open(cfile, "rb") as f:
                    data = f.read()
                name = arc
            elif name.endswith((".pyc", ".pyi")):
                continue   # stale bytecode / stubs: rebuilt above / not needed

            zi = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED   # per-entry; STORED is the point
            zi.external_attr = 0o644 << 16
            z.writestr(zi, data)
            n += 1

    size = os.path.getsize(out)
    print(f"mkpip: {n} files from {os.path.basename(whl)} -> {out} "
          f"({size / 1048576:.1f} MB)")

    with zipfile.ZipFile(out) as z:
        assert any(x == "pip/__main__.pyc" for x in z.namelist()), \
            "pip/__main__.pyc missing -- `python -m pip` would not resolve"
        bad = [i.filename for i in z.infolist()
               if i.compress_type != zipfile.ZIP_STORED]
        assert not bad, f"non-STORED entries would need zlib: {bad[:3]}"
    print("mkpip: verified pip/__main__.pyc present and all entries STORED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
