#!/usr/bin/env python3
"""Select GNU objcopy with PE support, preserving an explicit Make override."""
import shutil
import subprocess
for name in ('x86_64-elf-objcopy', 'gobjcopy', 'objcopy'):
    executable=shutil.which(name)
    if not executable:continue
    result=subprocess.run([executable,'--info'],capture_output=True,text=True)
    if not result.returncode and 'pei-x86-64' in result.stdout:
        print(executable)
        break
else:raise SystemExit('Install GNU binutils with PE support or set UEFI_OBJCOPY.')
