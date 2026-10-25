#!/usr/bin/env python3
"""Check real system-menu appearance, anchoring, Escape and live preferences.

Build FB_W=1280 FB_H=800 first. Uses a disposable UEFI live disk; never writes
host devices or existing account preferences. Retains original QMP captures.
"""
from pathlib import Path
import shutil
import re
import subprocess
import time
import tempfile
from recovery_smoke import Guest,ROOT
import shell_shot as S


def main():
    out=Path(tempfile.mkdtemp(prefix='emblink-menu-'))
    print(f'Logs and captures: {out}',flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    g=Guest(out/'boot',disk,menu=4,resolution=(1280,800),boot_nvme=True)
    def snap(name):
        g.pump(2);p=out/(name+'.ppm');g.qmp.screendump(str(p));return S.ppm_pixels(str(p))
    def click(x,y):
        # Read actual coordinates: a slow software-rendered menu can delay PS/2
        # packets, so assuming every relative event arrived makes aiming drift.
        deadline=time.monotonic()+30
        while True:
            p=subprocess.run(['gdb','-q','-nx','-batch',str(ROOT/'kernel/kernel.elf'),
                '-ex','target remote '+str(g.out/'gdb.sock'),
                '-ex','printf "POINTER %d %d\\n", \'kernel/drivers/input/mouse.c\'::g_x, \'kernel/drivers/input/mouse.c\'::g_y',
                '-ex','detach'],capture_output=True,text=True,timeout=15,check=True)
            m=re.search(r'POINTER (\d+) (\d+)',p.stdout)
            if not m:raise RuntimeError(p.stdout+p.stderr)
            ax,ay=map(int,m.groups())
            if (ax,ay)==(x,y):break
            if time.monotonic()>deadline:raise TimeoutError('pointer did not reach control')
            dx=max(-40,min(40,x-ax));dy=max(-40,min(40,y-ay))
            g.qmp.cmd('human-monitor-command',**{'command-line':f'mouse_move {dx} {dy}'})
            g.pump(.1)
        S.click(g.qmp);g.pump(1)
    def escape():
        g.qmp.cmd('human-monitor-command',**{'command-line':'sendkey esc'});g.pump(2)
    def panel_lightness(frame):
        w,h,p=frame
        assert (w,h)==(1280,800),'build FB_W=1280 FB_H=800'
        v=sorted(sum(p[(y*w+x)*3:(y*w+x)*3+3])/3
                 for y in range(35,190) for x in range(48,225))
        return v[len(v)//2]
    def panel_left(a,b):
        w,h,p=a;_,_,q=b
        changed=[]
        for x in range(0,310):
            n=sum(max(abs(p[(y*w+x)*3+i]-q[(y*w+x)*3+i]) for i in range(3))>15
                  for y in range(35,190))
            if n>30:changed.append(x)
        assert changed,'menu must change visible pixels'
        return min(changed)
    def panel_difference(a,b):
        w,h,p=a;_,_,q=b
        return sum(max(abs(p[(y*w+x)*3+i]-q[(y*w+x)*3+i]) for i in range(3))>15
                   for y in range(30,225) for x in range(20,300))
    try:
        g.wait(b'TopBar: first frame presented');g.wait(b'home: time zone');g.pump(3)
        g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{g.out / "gdb.sock"},server=on,wait=off'})
        baseline=snap('desktop')
        click(50,12);dark=snap('dark-menu-left')
        assert panel_lightness(dark)<90,'dark desktop must have a dark menu'
        left=panel_left(baseline,dark)
        escape();closed=snap('escape-dismissed')
        # The panel must disappear, not merely lose its hovered row.
        assert panel_difference(baseline,dark)>3000,'menu must visibly open'
        assert panel_difference(baseline,closed)<300,'Escape left the panel visible'
        assert b'TopBar: exit' not in g.seen,'Escape quit the system bar'
        click(95,12);again=snap('dark-menu-right')
        assert abs(panel_left(baseline,again)-left)<=2,'menu anchor moved with click location'
        click(650,500);outside=snap('outside-dismissed')
        assert panel_difference(baseline,outside)<300,'outside click left the panel visible'
        click(668,745);g.wait(b'Settings: first frame presented');g.pump(2)
        click(916,195);snap('light-appearance')
        click(230,94);g.pump(3)
        click(75,12);light=snap('light-menu')
        assert panel_lightness(light)>160,'live light preference did not reach the system menu'
        escape()
        click(668,745);g.pump(3)
        click(988,195);snap('dark-appearance')
        click(230,94);g.pump(3)
        click(75,12);dark=snap('dark-menu')
        assert panel_lightness(dark)<90,'live dark preference did not reach the system menu'
        # A chosen item still dispatches through the native notification service.
        click(130,45);g.wait(b'Notifications: first frame presented');snap('about-banner')
        assert g.seen.count(b'TopBar: font loaded')==1,'the menu bar restarted during interaction'
        assert b'PANIC' not in g.seen
    finally:
        try:g.qmp.screendump(str(out/'last.ppm'))
        finally:g.close()
    print('PASS: dark/light menus, stable label anchor, Escape without bar exit, outside dismissal and native action dispatch',flush=True)


if __name__=='__main__':main()
