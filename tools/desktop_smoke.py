#!/usr/bin/env python3
"""Native UEFI account, desktop controls, lock and reboot acceptance.

Build with FB_W=1280 FB_H=800. Requires QEMU, OVMF and GDB; optional
EMBLINK_ACPI_TABLE points to the compiled tests/acpi/power.asl fixture.
All storage is a fresh disposable image. Original QMP captures are retained.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
from recovery_smoke import Guest, ROOT
from lock_test import keyname
import shell_shot as S


def main():
    out=Path(tempfile.mkdtemp(prefix='emblink-desktop-'))
    print(f'Logs and captures: {out}',flush=True)
    disk=out/'desktop.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    fixture=os.environ.get('EMBLINK_ACPI_TABLE')
    guest=Guest(out/'first-boot',disk,boot_nvme=True,resolution=(1280,800),audio=True,acpi=fixture)
    def snap(name):
        guest.pump(1);guest.qmp.screendump(str(out/(name+'.ppm')))
    def type_text(text):
        for c in text:
            guest.qmp.cmd('human-monitor-command',**{'command-line':'sendkey '+keyname(c)})
            guest.pump(.1)
    def click(x,y):
        S.move(guest.qmp,x,y);S.click(guest.qmp);guest.pump(1)
    def menu(y):
        click(75,12);click(130,y)
    def debugger():
        sock=guest.out/'gdb.sock'
        guest.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{sock},server=on,wait=off'})
        return sock
    sock=None
    def value(expression):
        p=subprocess.run(['gdb','-q','-nx','-batch',str(ROOT/'kernel/kernel.elf'),
            '-ex','target remote '+str(sock),'-ex','p/u '+expression,'-ex','detach'],
            capture_output=True,text=True,timeout=20,check=True)
        m=re.search(r'\$\d+ = (\d+)',p.stdout)
        if not m:raise RuntimeError(p.stdout+p.stderr)
        return int(m.group(1))
    def eventually(expression,want):
        deadline=time.monotonic()+15
        while True:
            got=value(expression)
            if got==want:return
            if time.monotonic()>deadline:raise AssertionError(f'{expression}: got {got}, wanted {want}')
            guest.pump(.4)
    volume="'kernel/drivers/audio/audio.c'::volume_control"
    brightness="'kernel/acpi/display.c'::cache.percent"
    try:
        guest.wait(b'EmbLink OS Setup: first frame presented');snap('setup')
        assert S.ppm_pixels(str(out/'setup.ppm'))[:2]==(1280,800),'build FB_W=1280 FB_H=800'
        type_text('tester\tcorrect horse 1\tcorrect horse 1\n')
        guest.wait(b"setup: account 'tester' created",180)
        guest.wait(b'EmbLink OS Login: first frame presented');snap('login')
        type_text('tester\tcorrect horse 1\n')
        guest.wait(b"init: authenticated session 'tester'",180)
        guest.wait(b'TopBar: first frame presented');guest.wait(b'home: time zone');snap('desktop')
        sock=debugger();S._pos[0]=None
        click(668,745);guest.wait(b'Settings: first frame presented');snap('appearance')
        assert b'home: spawn /data/apps/settings/settings.elf' in guest.seen
        click(280,180);snap('wifi')
        click(280,202);eventually(volume,100);snap('sound')
        click(1000,306);eventually(volume,356);snap('muted')
        click(1000,306);eventually(volume,100)
        click(735,274);level=value(volume)
        assert 45<=level<=55,level
        snap('volume')
        click(280,225);snap('display')
        if fixture:
            eventually(brightness,60);click(735,375);eventually(brightness,40);snap('brightness')
        click(280,247);snap('power')
        click(280,269);snap('keyboard')
        click(230,94)
        menu(39);snap('banner')
        menu(140);guest.wait(b'Notifications: first frame presented')
        click(640,115);snap('notifications')
        assert value("'kernel/ipc/notify.c'::count")>0,'native banner must enter session history'
        w,h,pixels=S.ppm_pixels(str(out/'notifications.ppm'))
        ink=sum(min(pixels[(y*w+x)*3:(y*w+x)*3+3])>110
                for y in range(150,400) for x in range(420,850))
        assert ink>100,'history content must actually render in its window'
        start=len(guest.seen);menu(81);guest.wait(b'home: screen LOCKED',start=start);snap('locked')
        start=len(guest.seen);type_text('wrong password\n')
        guest.wait(b'authd: refused',start=start);snap('password-rejected')
        assert b'home: screen unlocked' not in guest.seen[start:]
        type_text('correct horse 1\n');guest.wait(b'home: screen unlocked',start=start)
        start=len(guest.seen);menu(110)
        guest.wait(b'EmbLink OS Login: first frame presented',start=start);snap('logged-out')
    finally:guest.close()
    guest=Guest(out/'second-boot',disk,boot_nvme=True,resolution=(1280,800),audio=True,acpi=fixture)
    try:
        guest.wait(b'EmbLink OS Login: first frame presented');snap('persistent-login')
        assert b'first-boot setup started' not in guest.seen
        type_text('tester\tcorrect horse 1\n')
        guest.wait(b"init: authenticated session 'tester'",180)
        guest.wait(b'TopBar: first frame presented');guest.wait(b'home: time zone')
        sock=debugger();eventually(volume,level);snap('persistent-desktop')
        guest.command('recovery check',b'integrity OK',240)
        assert b'PANIC' not in guest.seen
    finally:guest.close()
    print('PASS: native account/login, confined Settings, mute/volume, brightness fixture, history, lock/passwords, logout, persistent account/preferences and cold integrity',flush=True)


if __name__=='__main__':main()
