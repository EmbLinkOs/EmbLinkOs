#!/usr/bin/env python3
"""Check installed terminal headers against the declarations applications build with."""
from pathlib import Path
import sys,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/embkfs_mkfs'))
import mkfs_embkfs as fs

with tempfile.TemporaryDirectory(prefix='terminal-sdk-') as directory:
    fake=Path(directory);(fake/'sys').mkdir()
    (fake/'termios.h').write_text('#include <sys/termios.h>\n')
    (fake/'sys/ioctl.h').write_text('broken target declaration\n')
    previous=fs.NEWLIB_INC;fs.NEWLIB_INC=directory
    try:objects=fs.discover_userland_objects()
    finally:fs.NEWLIB_INC=previous
    for relative in ['termios.h','sys/ioctl.h','embk_terminal.h','embk.h','embk_syscall.h']:
        path=b'system/abi/include/'+relative.encode()
        matches=[obj for obj in objects if obj[0]==path]
        assert len(matches)==1,(relative,len(matches))
        assert matches[0][3]==(ROOT/'user/lib'/relative).read_bytes(),relative
    for filename in (ROOT/'user/lib').glob('embk_*.h'):
        path=b'data/src/user/lib/'+filename.name.encode()
        matches=[obj for obj in objects if obj[0]==path]
        assert len(matches)==1 and matches[0][3]==filename.read_bytes(),filename
print('PASS: installed terminal SDK overrides, unique header paths and shared kernel source declarations')
