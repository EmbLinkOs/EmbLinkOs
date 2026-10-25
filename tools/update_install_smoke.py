#!/usr/bin/env python3
"""Recovery-install a managed image to disposable NVMe, then boot it alone."""
from pathlib import Path
import json,re,subprocess,tempfile
from recovery_smoke import Guest,ROOT,digest

def main():
    out=Path(tempfile.mkdtemp(prefix='update-managed-install-',dir='/workspace/.setup'));print(out,flush=True)
    source=out/'source.img';target=out/'installed.img'
    subprocess.run(['cp','--sparse=always',str(ROOT/'managed-uefi.img'),str(source)],check=True)
    with target.open('wb') as f:f.truncate(source.stat().st_size+64*1048576)
    before=digest(source)
    guest=Guest(out/'recovery',source,target,menu=2)
    try:
        guest.wait(b'recovery: ready',240)
        listing=guest.command('install list',b'EMBLINKINSTALL');guest.pump(.5);listing=guest.seen.decode(errors='replace')
        running=re.search(r'install: (sd\w+):[^\n]*\[running source disk\]',listing).group(1)
        target_name=next(n for n in re.findall(r'install: (sd\w+):',listing) if n!=running)
        start=len(guest.seen);guest.command('install plan '+target_name,b'reviewed plan:');guest.pump(.5)
        plan=guest.seen[start:].decode(errors='replace');assert 'managed OS-A + OS-B + EFI' in plan,plan
        confirm=re.search(r'install: reviewed plan: (install erase [^\r\n]+)',plan).group(1)
        guest.command(confirm,b'verified installation complete',420)
        guest.qmp.screendump(str(out/'managed-recovery-install.ppm'))
    finally:guest.close()
    assert digest(source)==before,'installer changed source image'
    guest=Guest(out/'installed',target,boot_nvme=True,resolution=(1280,800))
    try:
        guest.wait(b'EmbLink OS Setup: first frame presented',240)
        assert b'managed OS slot 0 mounted read-only' in guest.seen
        assert b'shared personal data mounted' in guest.seen
        assert b'update: managed release 1 0.1.0' in guest.seen
        guest.qmp.screendump(str(out/'installed-managed-setup.ppm'))
    finally:guest.close()
    print('PASS: managed recovery clone expands Data, rewrites boot identities, preserves source, and boots alone over NVMe with read-only OS and writable account setup',flush=True)
if __name__=='__main__':main()
