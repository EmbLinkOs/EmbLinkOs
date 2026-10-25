#!/usr/bin/env python3
"""Observe native custom colours, monochrome silhouettes and power status."""
from pathlib import Path
import json,re,shutil,tempfile,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_ui import NativeUI

def main():
    out=Path(tempfile.mkdtemp(prefix='contour-shell-',dir='/workspace/.setup'));print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800));d.sock=d.out/'gdb.sock';d.symbols={}
    fields=re.findall(r'^    X\((\w+),',(ROOT/'user/lib/oscfg.h').read_text(),re.M)
    def cfg(app,pid,name):return int.from_bytes(d.read(app,pid,'g_cfg',4,4*fields.index(name)),'little',signed=True)
    def eventually(check,message):
        end=time.monotonic()+30
        while not check():
            if time.monotonic()>end:raise AssertionError(message)
            d.g.pump(.5)
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone');d.g.pump(2)
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        home=d.debug("import gdb,json\nw=gdb.parse_and_eval(\"'kernel/gfx/compositor.c'::g_wins\")\nprint('RESULT '+json.dumps(next(int(w[i]['pid']) for i in range(w.type.range()[1]+1) if int(w[i]['used']) and int(w[i]['desktop']))))")
        w=d.launch('Settings','Settings');ui=NativeUI(d);ui.click_text(w,'Paper');d.g.pump(3)
        ui.click_text(w,'Desktop & Dock');ui.reveal(w,'Application icons');ui.select(w,'Application icons','Monochrome')
        eventually(lambda:cfg('home',home,'icon_theme')==1,'monochrome preference did not reach shell');d.g.pump(3)
        capture=d.snap('monochrome-settings')
        from PIL import Image
        with Image.open(capture) as im:
            bright=sum(min(im.getpixel((x,y)))>170 for y in range(54,94) for x in range(35,75))
        assert bright>20,'monochrome desktop glyph lost contrast against the dark wallpaper'
        d.key('meta_l-spc');d.g.pump(2)
        desktop=next(a for a in d.state()['windows'] if a['pid']==home)
        assert abs(ui.text(desktop,'Calculator')['rect'][1]-ui.text(desktop,'Settings')['rect'][1])<2, 'a missing icon collapsed its library cell'
        d.snap('monochrome-library');d.key('meta_l-spc');d.g.pump(1)
        ui.click_text(w,'Appearance');ui.select(w,'Surfaces','Custom')
        current=d.read('settings',w['pid'],'g_surface_hex',8).split(b'\0',1)[0].decode()
        ui.click_text(w,current);d.key('meta_l-a');d.type('E4E8DF');ui.select(w,'Surface colour','Use colour')
        eventually(lambda:cfg('settings',w['pid'],'custom_surface')==0xE4E8DF,'custom surface did not save')
        ui.reveal(w,'Custom accent')
        current=d.read('settings',w['pid'],'g_accent_hex',8).split(b'\0',1)[0].decode()
        ui.reveal(w,current);ui.click_text(w,current);d.key('meta_l-a');d.type('D66F52');ui.select(w,'Custom accent','Use colour')
        eventually(lambda:cfg('home',home,'accent_custom')==1 and cfg('home',home,'custom_accent')==0xD66F52,'custom accent did not reach shell')
        d.g.pump(3);d.snap('custom-palette')
        d.click(1127,17);d.g.pump(2)
        ui.text(d.window('TopBar'),'No battery reported');d.snap('power-unavailable');d.key('esc')
        assert b'PANIC' not in d.g.seen
        print('PASS: original monochrome silhouettes, live custom surface/accent values and honest unavailable-power status',flush=True)
    finally:
        try:d.snap('last')
        finally:d.g.close()

if __name__=='__main__':main()
