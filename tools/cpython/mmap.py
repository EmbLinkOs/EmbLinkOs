"""mmap -- stub for EmbLinkOS, which has no memory-mapped files.

Normally a C builtin; EmbLinkOS doesn't provide the mmap() file mapping the real
module wraps. This exists only so code that `import mmap` UNCONDITIONALLY at
module load (e.g. pip's vendored cachecontrol.filewrapper) can import. Actually
constructing an mmap raises -- and the only caller that would, cachecontrol's
disk cache, is off under `pip install --no-cache-dir`. Honest: the capability is
genuinely absent, so use fails loudly rather than silently misbehaving.
"""

ACCESS_DEFAULT = 0
ACCESS_READ = 1
ACCESS_WRITE = 2
ACCESS_COPY = 3

PROT_READ = 1
PROT_WRITE = 2
PROT_EXEC = 4

MAP_SHARED = 1
MAP_PRIVATE = 2
MAP_ANONYMOUS = 32
MAP_ANON = 32

PAGESIZE = 4096
ALLOCATIONGRANULARITY = 4096


class error(OSError):
    pass


class mmap:
    def __init__(self, *args, **kwargs):
        raise error("mmap is not supported on EmbLinkOS")
