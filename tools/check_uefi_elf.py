#!/usr/bin/env python3
"""Check assumptions of the small EFI self-relocator before PE conversion."""
import struct
import sys
from pathlib import Path
raw=Path(sys.argv[1]).read_bytes()
if raw[:6]!=b'\x7fELF\x02\x01':
    raise SystemExit('EFI intermediate must be little-endian ELF64')
h=struct.unpack_from('<16sHHIQQQIHHHHHH',raw)
if h[1]!=3 or h[2]!=62:
    raise SystemExit('EFI intermediate must be x86-64 ET_DYN; pass -Wl,-shared')
sections=[struct.unpack_from('<IIQQQQIIQQ',raw,h[6]+i*h[11]) for i in range(h[12])]
strings=sections[h[13]]
names=raw[strings[4]:strings[4]+strings[5]]
for s in sections:
    name=names[s[0]:].split(b'\0',1)[0].decode()
    if name in ('.text','.rodata','.data','.bss','.got','.rela','.reloc') and s[3]&4095:
        raise SystemExit(f'EFI runtime section {name} is not page aligned')
    if name in ('.data','.rodata') and s[1]!=1:
        raise SystemExit('EFI ordinary data must not be merged with dynamic metadata')
    if s[1]==4:  # SHT_RELA: crt0 supports only R_X86_64_RELATIVE.
        for offset in range(s[4],s[4]+s[5],24):
            _,info,_=struct.unpack_from('<QQq',raw,offset)
            if info!=8:
                raise SystemExit(f'EFI relocation {info:#x} is unsupported by crt0')
print('EFI intermediate: relocatable x86-64, supported relocations')
