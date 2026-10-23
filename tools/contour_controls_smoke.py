#!/usr/bin/env python3
"""Drive Contour customization through actual Settings and QMP input.

A disposable live UEFI image; NativeUI only observes labels and C geometry.
The assertions read compositor state, without modifying guest memory.
"""
from pathlib import Path
import json,shutil,tempfile,time
from workspace_smoke import Desktop
from recovery_smoke import Guest,ROOT
from native_ui import NativeUI

def main():
    out=Path(tempfile.mkdtemp(prefix='contour-controls-',dir='/workspace/.setup'));print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800));d.sock=d.out/'gdb.sock';d.symbols={}
    def state():
        return d.debug('''import gdb,json
s=gdb.parse_and_eval("'kernel/gfx/compositor.c'::g_desktop")
print('RESULT '+json.dumps({k:int(s[k]) for k in ['top','bottom','left','right','cursor_style','cursor_size','flags','blur','opacity','shortcut_modifier','workspace_count']}))''')
    def eventually(check,message):
        end=time.monotonic()+30
        while not check():
            if time.monotonic()>end:raise AssertionError(message)
            d.g.pump(.5)
    def dock_ready(side):
        import struct
        owner=d.debug("import gdb,json\nw=gdb.parse_and_eval(\"'kernel/gfx/compositor.c'::g_wins\")\nprint('RESULT '+json.dumps(next(int(w[i]['pid']) for i in range(w.type.range()[1]+1) if int(w[i]['used']) and int(w[i]['desktop']))))")
        x0,y0,x1,y1=struct.unpack('<4f',d.read('home',owner,'g_dockr',16))
        return x1-x0<100 and (x0<100 if side=='left' else x0>1000)
    def cfg(w,name):
        # The declaration order comes from the single schema, including old fields.
        text=(ROOT/'user/lib/oscfg.h').read_text();import re
        fields=re.findall(r'^    X\((\w+),',text,re.M)
        fields+=['volume','muted','idle_dim_seconds','idle_lock_seconds','lid_lock','power_profile','accent','dark','dock_size','dock_dots','ui_scale','keymap','wallpaper','tz','bar_24h','bar_seconds','bar_date','bar_cpu']
        return int.from_bytes(d.read('settings',w['pid'],'g_cfg',4,4*fields.index(name)),'little',signed=True)
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone');d.g.pump(2)
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        w=d.launch('Settings','Settings');ui=NativeUI(d)
        ui.click_text(w,'Paper');d.g.pump(2)
        ui.click_text(w,'Desktop & Dock');ui.select(w,'Placement','Left')
        eventually(lambda:state()['left']>0 and dock_ready('left'),'left dock geometry');d.g.pump(2);d.snap('left-dock')
        ui.select(w,'Placement','Right');eventually(lambda:state()['right']>0 and dock_ready('right'),'right dock geometry');d.g.pump(2);d.snap('right-dock')
        ui.select(w,'Placement','Bottom');eventually(lambda:state()['right']==0 and state()['bottom']>0,'bottom dock insets')
        ui.toggle(w,'Auto-hide');eventually(lambda:cfg(w,'dock_autohide')==1,'dock auto-hide preference')
        d.g.pump(2);d.snap('dock-hidden');ui.move(640,790);d.g.pump(2);d.snap('dock-revealed')
        ui.toggle(w,'Auto-hide');d.g.pump(2)
        ui.select(w,'Screen edge','Bottom');eventually(lambda:state()['top']==0,'bottom command strip')
        eventually(lambda:d.window('TopBar')['y']>700,'bottom strip position');d.snap('bottom-strip')
        d.click(70,784);eventually(lambda:d.window('TopBar')['ch']==800,'bottom menu canvas')
        bar=d.window('TopBar');row=ui.text(bar,'Lock Screen')['rect'];assert 0<row[1]<750,row
        d.snap('bottom-menu');d.key('esc');d.g.pump(1)
        ui.select(w,'Screen edge','Top');eventually(lambda:state()['top']>0,'restore top strip')
        eventually(lambda:d.window('Settings')['y']>=state()['top'],'window respects restored top work area')
        ui.toggle(w,'Auto-hide strip');d.g.pump(2)
        eventually(lambda:d.window('TopBar')['y']<0,'strip auto-hide');d.snap('strip-hidden')
        ui.move(60,0);eventually(lambda:d.window('TopBar')['y']==0,'strip edge reveal');d.snap('strip-revealed')
        ui.toggle(w,'Auto-hide strip');d.g.pump(2)
        ui.click_text(w,'Accessibility');ui.select(w,'Pointer colour','Accent');ui.select(w,'Pointer size','Large')
        eventually(lambda:state()['cursor_style']==2 and state()['cursor_size']==1,'large accent cursor')
        ui.toggle(w,'Reduce motion');ui.click_text(w,'Use opaque surfaces')
        eventually(lambda:state()['flags']&1 and state()['opacity']==255 and state()['blur']==0,'reduced effects')
        d.snap('reduced-effects')
        ui.select(w,'System modifier','Ctrl + Alt');eventually(lambda:state()['shortcut_modifier']==1,'custom system modifier')
        d.key('ctrl-alt-2');eventually(lambda:d.debug('''import gdb,json
print('RESULT '+json.dumps(int(gdb.parse_and_eval("'kernel/gfx/compositor.c'::g_desktop.workspace"))))''')==1,'Ctrl+Alt workspace shortcut')
        d.key('ctrl-alt-1');d.g.pump(1)
        ui.select(w,'System modifier','Super');d.g.pump(1)
        ui.click_text(w,'Appearance');ui.select(w,'Interface font','Serif');eventually(lambda:cfg(w,'font')==2,'font preference');d.snap('serif-appearance')
        ui.scroll(w,4)
        import struct
        assert struct.unpack('<f',d.read('settings',w['pid'],'g_scroll',4))[0]>0,'wheel-down must reveal lower controls'
        d.snap('type-controls')
        ui.select(w,'Interface scale','115%');eventually(lambda:cfg(w,'ui_scale')==115,'interface scale')
        ui.reveal(w,'Density');ui.select(w,'Density','Compact');eventually(lambda:cfg(w,'density')==0,'density');d.snap('compact-scaled')
        ui.reveal(w,'Reset appearance…');ui.click_text(w,'Reset appearance…');d.snap('reset-confirmation')
        ui.click_text(w,'Cancel');assert cfg(w,'ui_scale')==115,'reset cancellation changed preferences'
        ui.click_text(w,'Reset appearance…');ui.click_text(w,'Reset')
        eventually(lambda:cfg(w,'ui_scale')==100 and cfg(w,'font')==0,'reset appearance')
        eventually(lambda:state()['cursor_style']==0 and state()['cursor_size']==0 and state()['opacity']<255,'reset compositor')
        d.snap('reset-complete')
        (out/'final-state.json').write_text(json.dumps(state(),indent=2))
        print('PASS: side docks, edge reveal, bottom menus, cursor styles, reduced effects, alternate shortcuts, font/scale/density and confirmed/cancelled reset',flush=True)
    finally:
        try:d.snap('last')
        finally:d.g.close()

if __name__=='__main__':main()
