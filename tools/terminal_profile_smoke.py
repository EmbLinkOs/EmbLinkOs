#!/usr/bin/env python3
"""Terminal's profiles, live: Clear Dark by default, Preferences, a profile
file changed under a running window, the default profile for a new window.

Driven over QMP like a person would. The first window wears Clear Dark (the
Mac profile it follows): its colours, 95% over a blur of the desktop, the
window sized to its 120 x 30. Super+, opens Preferences beside the screen; a
swatch opens the colour picker. The profile's file is then written from the
shell -- as another window's Preferences or an editor would -- and the window
takes it within a second: at 60% the desktop shows through, blurred. Last,
terminal.conf names Solarized Light and a new window opens in it, its bars
light to match.

    terminal_profile_smoke.py [--virgl]   (--virgl: composited by the GPU)
"""
from pathlib import Path
import os,shutil,sys,tempfile,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_terminal import TerminalState
from lock_test import keyname
from PIL import Image

def main():
    virgl='--virgl' in sys.argv
    out=Path(tempfile.mkdtemp(prefix='terminal-profile-',dir='/workspace/.setup' if os.path.isdir('/workspace/.setup') else None));print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot'
    d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800),**({'graphics':'virgl'} if virgl else {}))
    d.sock=d.out/'gdb.sock';d.symbols={}
    tag='-virgl' if virgl else ''
    def eventually(check,message,seconds=60):
        end=time.monotonic()+seconds
        while not check():
            if time.monotonic()>end:raise AssertionError(message)
            d.g.pump(.4)
    def type_text(value):
        for c in value:d.key(keyname(c))
    def state():return screen.state(w)
    def text():return screen.text(w).split('\n')
    def grab(name):                 # a screendump, or over VNC when the GPU composes
        d.g.pump(1);p=d.g.capture(out/f'{name}.ppm');print('Capture: '+name,flush=True);return p
    def pixel(x,y):return Image.open(grab('probe')).convert('RGB').getpixel((x,y))
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone')
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        screen=TerminalState(d);w=d.launch('Terminal','Terminal');eventually(lambda:screen.at_prompt(w),'initial prompt')
        st=state()
        print('profile',st['profile'],'see-through',st['see_through'],'opacity',st['opacity'],'blur',st['blur'],
              'grid',st['cols'],st['rows'],'cell',st['cell'],'origin',st['origin'],flush=True)
        assert st['profile']=='Clear Dark' and st['see_through'] and st['opacity']==0.95 and st['blur']==16,st
        assert (st['cols'],st['rows'])==(120,30),(st['cols'],st['rows'])
        win=d.window('Terminal');print('window',win,flush=True)
        type_text('/data/apps/ttyprobe/ttyprobe.elf "--glyphs"\n')
        eventually(lambda:any('MesloLGS Nerd Font Mono' in line for line in text()),'the glyph card')
        d.g.pump(1.5)
        grab(f'profile-clear-dark{tag}')
        # the background: the profile's colour at 95% over the desktop
        win=d.window('Terminal');st=state();(cw,ch),(ox,oy)=st['cell'],st['origin']
        blank=(win['x']+ox+60*cw,win['y']+oy+(st['rows']-2)*ch)
        r,g,b=pixel(*blank)
        assert abs(r-0x21)<=3 and abs(g-0x27)<=3 and abs(b-0x34)<=3,('the ground',(r,g,b))
        # Preferences, beside the screen; a swatch opens the colour picker
        d.key('meta_l-comma')
        eventually(lambda:state()['prefs_open'],'Preferences opens on Super+,')
        d.g.pump(1.5)
        prefs=Image.open(grab(f'profile-prefs{tag}')).convert('RGB')
        panel_x=win['x']+win['cw']-312
        swatch=next(((x,y) for y in range(win['y'],win['y']+win['ch']) for x in range(panel_x,win['x']+win['cw'])
                     if prefs.getpixel((x,y))==(0x35,0x42,0x4c)),None)
        assert swatch,'the swatch of colour 0 in the panel'
        d.click(swatch[0]+8,swatch[1]+6)
        eventually(lambda:state()['prefs_edit']==0,'a swatch opens the picker for its colour')
        d.hover(panel_x+150,win['y']+win['ch']//2)          # the picker is below the swatches: scroll to it
        for _ in range(6):d.g.qmp.cmd('human-monitor-command',**{'command-line':'mouse_move 0 0 -1'});d.g.pump(.3)
        d.g.pump(1.5)
        grab(f'profile-colours{tag}')
        d.key('meta_l-comma')
        eventually(lambda:not state()['prefs_open'],'Super+, closes Preferences')
        # the profile's file, written by someone else: the window takes it
        type_text('echo "name = Clear Dark" out> ".config/terminal/profiles/clear-dark.conf"\n')
        type_text('echo "opacity = 0.6" out>> ".config/terminal/profiles/clear-dark.conf"\n')
        type_text('lines in< ".config/terminal/profiles/clear-dark.conf"\n')
        eventually(lambda:state()['opacity']==0.6,'the window reads its changed file')
        d.g.pump(2)
        r2,g2,b2=pixel(*blank)
        print('ground at 95%',(r,g,b),'at 60%',(r2,g2,b2),flush=True)
        assert max(abs(r2-r),abs(g2-g),abs(b2-b))>=5,('the desktop shows through at 60%',(r,g,b),(r2,g2,b2))
        # and it is the desktop, not some flat colour: the wallpaper's light
        # left and dark right show through a blank row, blurred but there
        row=Image.open(grab('probe')).convert('RGB')
        left,right=row.getpixel((win['x']+ox+8*cw,blank[1])),row.getpixel((win['x']+ox+110*cw,blank[1]))
        print('a blank row at 60%: left',left,'right',right,flush=True)
        assert max(abs(left[i]-right[i]) for i in range(3))>=6,('the wallpaper shows through',left,right)
        # a window behind it: Files, opened and then put behind by raising the
        # Terminal from the dock -- its folders show through, blurred
        files=d.launch('Files','Files')
        d.g.pump(2)
        d.click(611,746)
        front=lambda:d.debug("import gdb,json\nprint('RESULT '+json.dumps(int(gdb.parse_and_eval(\"'kernel/gfx/compositor.c'::g_top_id\"))))")
        eventually(lambda:front()==d.window('Terminal')['id'],'the dock raises the Terminal over Files')
        d.g.pump(3)
        r3,g3,b3=pixel(*blank)
        print('with Files behind',(r3,g3,b3),flush=True)
        grab(f'profile-translucent{tag}')
        # the default profile, for a new window
        type_text('echo "default_profile = Solarized Light" out> ".config/terminal/terminal.conf"\n')
        d.g.pump(1)
        d.key('meta_l-q')
        eventually(lambda:not any(x['title']=='Terminal' and x['visible'] for x in d.state()['windows']),'the window closes',30)
        w=d.launch('Terminal','Terminal');eventually(lambda:screen.at_prompt(w),'a new window')
        st=state()
        assert st['profile']=='Solarized Light' and not st['see_through'] and st['opacity']==1,st['profile']
        type_text('/data/apps/ttyprobe/ttyprobe.elf "--glyphs"\n')
        eventually(lambda:any('MesloLGS Nerd Font Mono' in line for line in text()),'the glyph card, light')
        d.g.pump(1.5)
        grab(f'profile-solarized-light{tag}')
        print('PASS: Clear Dark by default (95% over a blur, 120 x 30), Preferences and its colour picker, '
              'a profile file changed under the window, a new window in the default profile',flush=True)
    except Exception:
        try:
            grab('last');print('\n'.join(text()[-12:]),flush=True)
        except Exception as e:print('no last capture:',e)
        raise
    finally:
        d.g.close()

if __name__=='__main__':main()
