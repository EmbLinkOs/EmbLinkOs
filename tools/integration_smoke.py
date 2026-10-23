#!/usr/bin/env python3
"""Exercise the merged kernel and userspace through one disposable UEFI boot."""
import os
from pathlib import Path
import re
import shutil
import tempfile
import time
from recovery_smoke import Guest,ROOT

out=Path(tempfile.mkdtemp(prefix='emblink-integration-'))
print(f'Logs: {out}',flush=True)
disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
guest=Guest(out/'live',disk,menu=4,boot_nvme=True)
results=[]
try:
    guest.wait(b'TopBar: first frame presented')
    guest.qmp.screendump(str(out/'desktop.ppm'))
    for command in ['test devices','test devices preferences','test devices files',
                    'test ipc cancel','test channel','test compositor','test session',
                    'test kill io','test posix']:
        start=len(guest.seen);guest.serial.sendall((command+'\n').encode())
        deadline=time.monotonic()+240
        pattern=re.compile(rb'\[cmd\] '+re.escape(command.encode())+rb':[^\r\n]*(?:OK|FAIL)')
        while True:
            guest.pump()
            output=bytes(guest.seen[start:]);match=pattern.search(output)
            if match:
                line=match.group().decode(errors='replace');print(line,flush=True)
                if not line.endswith('OK'):raise RuntimeError(line)
                results.append(line);break
            if b'PANIC' in output:raise RuntimeError('kernel panic during '+command)
            if time.monotonic()>deadline:raise TimeoutError(command)
    guest.qmp.screendump(str(out/'desktop-tested.ppm'))
    (out/'results.txt').write_text('\n'.join(results)+'\n')
    print('PASS: UEFI/NVMe native device gates, preferences, files, IPC, sessions, kill-I/O and POSIX',flush=True)
finally:
    guest.close()
