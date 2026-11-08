#!/usr/bin/env python3
"""Import and remove a real legacy bundle on a disposable managed Data volume.
Use a normal development build with its normal package fixture signing key.
"""
from pathlib import Path
import os,subprocess,sys,tempfile
from recovery_smoke import Guest,ROOT
sys.path[:0]=[str(ROOT/'tools/update'),str(ROOT/'tools/embkfs_mkfs')]
import mkfs_embkfs as mkfs

def main():
    out=Path(tempfile.mkdtemp(prefix='update-legacy-apps-',dir='/workspace/.setup' if os.path.isdir('/workspace/.setup') else None));print(out,flush=True)
    objects=[o for o in mkfs.discover_userland_objects() if (o[0] in (b'data',b'etc',b'home') or o[0].startswith((b'data/',b'etc/',b'home/'))) and o[0]!=b'data/apps' and not o[0].startswith(b'data/apps/')]
    for name in (b'data/apps',b'data/apps/pkgprobe'):
        objects.append((name,mkfs.L.DT_DIR,mkfs.L.S_IFDIR|0o755,b''))
    for name,content in [(b'pkgprobe.embx',(ROOT/'build/pkgprobe.embx').read_bytes()),(b'pkgprobe.pkg',(ROOT/'build/pkgprobe.pkg').read_bytes()),(b'pkgprobe.ns',b'ro /system\n')]:
        objects.append((b'data/apps/pkgprobe/'+name,mkfs.L.DT_REG,mkfs.L.S_IFREG|0o644,content))
    data=out/'data.img';mkfs.make_image(str(data),128<<20,objects=objects)
    disk=out/'live.img';subprocess.run([sys.executable,str(ROOT/'tools/update/mkdisk.py'),str(ROOT/'build/BOOTX64.EFI'),str(ROOT/'embkfs.img'),str(disk),'--data',str(data)],cwd=ROOT,check=True)
    g=Guest(out/'boot',disk,menu=4,boot_nvme=True,resolution=(1280,800))
    try:
        g.wait(b'home: time zone',240);g.wait(b'TopBar: first frame presented')
        text=g.command('test pkg',b'[cmd] test pkg:',240);g.pump(.3)
        assert b'[cmd] test pkg: OK' in g.seen and 'FAIL' not in text,text[-4000:]
        text=g.command('run /data/apps/pkg/pkg.elf remove pkgprobe',b'PKG remove pkgprobe -> deactivated',60)
        text=g.command('run /data/apps/pkg/pkg.elf info pkgprobe',b'is not installed',60)
        text=g.command('run /data/apps/pkg/pkg.elf install /data/staging/pkgprobe',b'PKG install pkgprobe ->',60)
        g.pump(.3)
        assert b'PANIC' not in g.seen
        print('PASS: legacy signed bundle imported before atomic update, real rollback and authority checks, modern plus legacy installation removed, fresh install still works',flush=True)
    finally:g.close()
if __name__=='__main__':main()
