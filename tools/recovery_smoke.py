#!/usr/bin/env python3
"""Install only onto a newly-created disposable QEMU disk, then boot it alone."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time
from app_shot import Qmp

ROOT=Path(__file__).resolve().parents[1]

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda:f.read(1048576),b''): h.update(block)
    return h.hexdigest()

class Guest:
    def __init__(self,out,disk,target=None,menu=1):
        self.out=out;out.mkdir();self.seen=bytearray();self.menu=menu;self.selected=False
        firmware=Path(os.environ.get('EMBLINK_OVMF_DIR','/usr/share/OVMF'))
        shutil.copyfile(firmware/'OVMF_VARS_4M.fd',out/'vars.fd')
        cmd=['qemu-system-x86_64','-cpu','max','-m','1024','-smp','2','-accel','tcg,thread=multi',
             '-display','none','-vga','none','-device','virtio-vga,xres=800,yres=600','-nic','none',
             '-drive',f'if=pflash,format=raw,readonly=on,file={firmware / "OVMF_CODE_4M.fd"}',
             '-drive',f'if=pflash,format=raw,file={out / "vars.fd"}',
             '-drive',f'format=raw,file={disk},if=ide,index=0',
             '-serial',f'unix:{out / "serial.sock"},server=on,wait=off',
             '-qmp',f'unix:{out / "qmp.sock"},server=on,wait=off','-no-reboot','-no-shutdown']
        if target:
            cmd+=['-drive',f'format=raw,file={target},if=none,id=target',
                  '-device','nvme,drive=target,serial=EMBLINKINSTALL']
        self.err=(out/'qemu.log').open('wb');self.log=(out/'serial.log').open('wb')
        self.p=subprocess.Popen(cmd,cwd=ROOT,stdout=self.err,stderr=self.err)
        self.serial=socket.socket(socket.AF_UNIX)
        for _ in range(200):
            try:self.serial.connect(str(out/'serial.sock'));break
            except (FileNotFoundError,ConnectionRefusedError):time.sleep(.1)
        else:self.close();raise TimeoutError('serial socket missing')
        self.serial.settimeout(.1);self.qmp=Qmp(str(out/'qmp.sock'))
    def pump(self,seconds=.2):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            if self.p.poll() is not None:raise RuntimeError('guest exited')
            try:
                data=self.serial.recv(65536)
                if not data:raise RuntimeError('serial closed')
                self.seen.extend(data);self.log.write(data);self.log.flush()
            except TimeoutError:pass
            if not self.selected and b'EmbBoot' in self.seen:
                self.selected=True
                if self.menu!=1:
                    self.qmp.cmd('human-monitor-command',**{'command-line':f'sendkey {self.menu}'})
                    time.sleep(.2)
                    self.qmp.cmd('human-monitor-command',**{'command-line':'sendkey ret'})
    def wait(self,needle,timeout=180,start=0):
        end=time.monotonic()+timeout
        while needle not in self.seen[start:]:
            if time.monotonic()>end:raise TimeoutError(f'missing {needle!r}; see {self.out}')
            self.pump()
    def command(self,text,expect,timeout=180):
        start=len(self.seen);self.serial.sendall((text+'\n').encode());self.wait(expect,timeout,start)
        return self.seen[start:].decode(errors='replace')
    def close(self):
        if hasattr(self,'p') and self.p.poll() is None:
            self.p.terminate()
            try:self.p.wait(timeout=5)
            except subprocess.TimeoutExpired:self.p.kill();self.p.wait(timeout=5)
        for attr in ('serial','log','err'):
            obj=getattr(self,attr,None)
            if obj:obj.close()


def main():
    out=Path(tempfile.mkdtemp(prefix='emblink-install-'));print(f'Logs: {out}',flush=True)
    source=out/'source.img';target=out/'installed.img'
    shutil.copyfile(ROOT/'uefi-usb.img',source)
    image_size=source.stat().st_size
    # A dd-written USB is larger than the image's declared GPT geometry.
    # The destination may be smaller than that USB while still fitting the OS.
    with source.open('r+b') as f:f.truncate(image_size+64*1048576)
    with target.open('wb') as f:f.truncate(image_size+16*1048576)
    before=digest(source)
    guest=Guest(out/'recovery',source,target,menu=2)
    try:
        guest.wait(b'recovery: ready')
        guest.command('recovery check',b'integrity OK',240)
        listing=guest.command('install list',b'[running source disk]')
        guest.pump(.5);listing=guest.seen.decode(errors='replace')
        disks=re.findall(r'install: (sd\w+): \d+ MiB[^\n]*',listing)
        assert 'EMBLINKINSTALL' in listing,'installer did not show target identity'
        running=re.search(r'install: (sd\w+):[^\n]*\[running source disk\]',listing).group(1)
        other=[d for d in disks if d!=running];assert len(other)==1,(listing,other)
        target_name=other[0]
        guest.command(f'install plan {running}',b'select a different whole disk')
        plan=guest.command(f'install plan {target_name}',b'reviewed plan:')
        guest.pump(.5);plan=guest.seen.decode(errors='replace')
        confirm=re.search(r'install: reviewed plan: (install erase [^\r\n]+)',plan).group(1)
        guest.command(confirm,b'verified installation complete',300)
        guest.command('recovery report',b'report complete')
        assert b'init: launched' not in guest.seen
        guest.qmp.screendump(str(out/'recovery.ppm'))
    finally:guest.close()
    assert before==digest(source),'recovery or installer changed source disk'
    # With the source media absent, the installed target must boot its own EFI
    # loader, mount its copied root writable and run first-account setup.
    guest=Guest(out/'installed-boot',target)
    try:
        guest.wait(b'init: no account found; first-boot setup started')
        guest.wait(b'EmbLink OS Setup: first frame presented')
        guest.command('mounts',b'embkfs')
        guest.command('recovery check',b'integrity OK',240)
        assert b'-- MISMATCH' not in guest.seen
        assert b'RESOURCE_CREATE_2D failed' not in guest.seen
        guest.qmp.screendump(str(out/'setup.ppm'))
    finally:guest.close()
    print('PASS: padded USB source unchanged + recovery integrity + expanded NVMe install + target boots alone into account setup',flush=True)

if __name__=='__main__':main()
