#!/usr/bin/env python3
"""Two monitors on a real UEFI/NVMe guest: extend, arrange, mirror, persist.

QEMU's virtio-vga with two heads (graphics="virtio-dual" in recovery_smoke.Guest).
Headless QEMU reports only head 0 as connected, so fw_cfg tells the guest's
driver that both are plugged in. Everything else is the real path: the kernel
extends the desktop when the second head appears, Settings arranges the
displays through QMP pointer input, the compositor and shell follow, and the
kept arrangement comes back after a cold boot and password login. Layout,
window and pointer state are read through read-only GDB; screenshots of each
head are unmodified guest pixels.

Monitors plugged in or pulled out while running cannot be driven from
headless QEMU; tests/drivers/display_control_test.c covers that path against
the real controller.

Build uefi-usb.img first. Never rebuild binaries while the guest runs.
Set EMBLINK_OVMF_DIR to a directory holding OVMF_CODE_4M.fd/OVMF_VARS_4M.fd.
"""
import json,os,shutil,tempfile,time
from pathlib import Path
from recovery_smoke import ROOT
from workspace_smoke import Desktop
from native_ui import NativeUI

ZOOM=(84,16)         # the toolkit's zoom light in an app's own title bar (em.c TL_HIT cases after the AppBar inset)


def main():
    out=Path(tempfile.mkdtemp(prefix='emblink-multimonitor-'))
    print('Logs and captures: '+str(out),flush=True)
    disk=out/'dual.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=None

    def layout():
        return d.debug('''import gdb,json
s=gdb.parse_and_eval("'kernel/display/control.c'::shown")
st=gdb.parse_and_eval("'kernel/display/control.c'::state")
f=gdb.parse_and_eval("'kernel/drivers/video/framebuffer.c'::fb")
outs=[{k:int(s['out'][i][k]) for k in ['connected','enabled','primary','x','y','w','h']} for i in range(int(s['count']))]
print('RESULT '+json.dumps({'mirror':int(s['mirror']),'outs':outs,'phase':int(st['phase']),'generation':int(st['generation']),'width':int(f['width']),'height':int(f['height'])}))''')
    def until(test,description,seconds=30):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            if test():return
            d.g.pump(.5)
        raise AssertionError(description+'; '+json.dumps(layout()))
    def windows():return d.state()['windows']
    def titled(title):return next(w for w in windows() if w['title']==title and w['visible'])
    def desktop():return next(w for w in windows() if w['desktop'])
    def settle():
        # The desktop has drawn and presented frames for the layout on screen.
        def ok():
            home=desktop();gen=int.from_bytes(d.read('home',home['pid'],'g_out_gen',4),'little')
            full=int.from_bytes(d.read('home',home['pid'],'g_full_present',4),'little')
            return gen==layout()['generation'] and full==0
        until(ok,'the desktop did not redraw for the layout on screen')
    def shot(name,head):
        settle();d.g.pump(1);p=out/f'{name}.ppm'
        if p.exists():p.unlink()
        d.g.qmp.cmd('screendump',filename=str(p),device='gpu0',head=head)
        for _ in range(50):
            if p.exists() and p.stat().st_size>1024:break
            time.sleep(.2)
        print('Capture: '+p.name,flush=True);return p
    def both(name):shot(name+'-head1',0);shot(name+'-head2',1)
    def drag(x0,y0,x1,y1):
        ui=NativeUI(d);ui.move(x0,y0)
        d.g.qmp.cmd('human-monitor-command',**{'command-line':'mouse_button 1'});d.g.pump(.4)
        end=time.monotonic()+60
        while True:
            ax,ay=d.state()['mouse']
            if (ax,ay)==(x1,y1):break
            if time.monotonic()>end:raise TimeoutError('drag')
            d.g.qmp.cmd('human-monitor-command',**{'command-line':f'mouse_move {max(-60,min(60,x1-ax))} {max(-60,min(60,y1-ay))}'})
            d.g.pump(.08)
        d.g.pump(.4);d.g.qmp.cmd('human-monitor-command',**{'command-line':'mouse_button 0'});d.g.pump(1)
    def settings_displays():
        s=d.launch('Settings','Settings');ui=NativeUI(d);ui.click_text(s,'Displays');d.g.pump(1.5)
        return titled('Settings'),NativeUI(d)
    def apply_and_keep(settings,ui,check):
        ui.reveal(settings,'Apply Arrangement');ui.click_text(settings,'Apply Arrangement')
        until(lambda:layout()['phase']==3,'arrangement trial did not start')
        until(check,'arrangement not shown')
        settings=titled('Settings');ui=NativeUI(d)
        until(lambda:any(r['text']=='Keep this arrangement?' for r in ui.labels(settings)),'no confirmation')
        return settings,ui
    def keep(settings,ui):
        ui.click_text(settings,'Keep Changes')
        until(lambda:layout()['phase']==0,'keep failed')
        d.g.pump(2)
    def bar_on(x,w):
        bar=titled('TopBar');return bar['x']==x and bar['cw']==w
    try:
        d=Desktop(out/'first-boot',disk,graphics='virtio-dual')
        d.login(setup=True)
        # 1. The second monitor extends the desktop to its right; the menu bar
        #    and the dock stay on the first.
        until(lambda:layout()['width']==2304 and layout()['height']==800,'second head did not extend the desktop',60)
        L=layout()['outs']
        assert L[0]['primary'] and (L[0]['x'],L[0]['y'],L[0]['w'],L[0]['h'])==(0,0,1280,800),L
        assert L[1]['enabled'] and not L[1]['primary'] and (L[1]['x'],L[1]['y'],L[1]['w'],L[1]['h'])==(1280,0,1024,768),L
        until(lambda:desktop()['cw']==2304 and bar_on(0,1280),'shell did not follow the extended desktop')
        assert b'display: Virtual-2 connected' in d.g.seen
        both('extended')

        # 2. A window dragged across the seam lands on the second monitor, and
        #    maximizing fills that monitor -- not the desktop, not the first.
        editor=d.launch('Editor','Editor')
        assert editor['x']+editor['cw']<=1280,editor
        d.click(editor['x']+100,editor['y']+130);d.type('Written on the first monitor.\n')
        gx,gy=editor['x']+editor['cw']//2,editor['y']+16                       # the title row
        drag(gx,gy,1280+512,gy+20)
        until(lambda:titled('Editor')['x']>=1280,'Editor did not cross to the second monitor')
        e=titled('Editor');assert e['x']+e['cw']<=2304 and e['y']+e['ch']<=768+1,e
        shot('window-moved-head2',1)
        # Rest on the zoom light: its board of placements opens; Fill fills
        # the monitor the window is on.
        ui=NativeUI(d);ui.move(e['x']+ZOOM[0],e['y']+ZOOM[1]);d.g.pump(1.5)
        ui.click_text(titled('Editor'),'Fill')
        until(lambda:titled('Editor')['x']==1280 and titled('Editor')['cw']==1024,'Fill did not fill the second monitor')
        e=titled('Editor');assert e['y']==0 and e['ch']<=768,e
        shot('window-filled-head2',1)
        d.click(e['x']+ZOOM[0],e['y']+ZOOM[1])
        until(lambda:titled('Editor')['cw']<1024,'Editor did not restore')
        assert d.read('edit',e['pid'],'g_doc',64).split(b'\0',1)[0]==b'Written on the first monitor.\n'

        # 3. Settings arranges them: the second monitor to the LEFT. The panel
        #    keeps the menu bar, and the Editor moves with its monitor.
        settings,ui=settings_displays()
        both('settings-arrangement')
        ui.click_text(settings,'2');d.g.pump(1);settings=titled('Settings');ui=NativeUI(d)
        ui.reveal(settings,'Left');ui.click_text(settings,'Left')
        settings,ui=apply_and_keep(settings,ui,lambda:(lambda L:L[1]['x']==0 and L[0]['x']==1024)(layout()['outs']))
        shot('arrangement-trial',1)
        keep(settings,ui)
        until(lambda:bar_on(1024,1280),'menu bar did not stay on the panel')
        until(lambda:titled('Editor')['x']+titled('Editor')['cw']<=1024,'Editor did not move with its monitor')
        both('arrangement-left')

        # 4. The second monitor becomes primary: the menu bar and dock move to it.
        settings=titled('Settings');ui=NativeUI(d)
        ui.reveal(settings,'Make Primary');ui.click_text(settings,'Make Primary')
        settings,ui=apply_and_keep(settings,ui,lambda:layout()['outs'][1]['primary']==1)
        keep(settings,ui)
        until(lambda:bar_on(0,1024),'menu bar did not move to the new primary')
        both('primary-moved')

        # 5. Mirror: one picture on both, at the size both can show; then a
        #    revert puts the extended arrangement back.
        settings=titled('Settings');ui=NativeUI(d)
        ui.reveal(settings,'Mirror');ui.click_text(settings,'Mirror')
        settings,ui=apply_and_keep(settings,ui,lambda:layout()['mirror']==1 and layout()['width']==1024 and layout()['height']==768)
        both('mirror-trial')
        ui.click_text(settings,'Revert')
        until(lambda:layout()['phase']==0 and layout()['mirror']==0 and layout()['width']==2304,'revert did not restore the arrangement')
        until(lambda:bar_on(0,1024),'menu bar did not return after the revert')
        e=titled('Editor');assert e['x']+e['cw']<=1024,e
        assert b'PANIC' not in d.g.seen and b'used ring timeout' not in d.g.seen
    finally:
        if d:d.g.close()

    # 6. A cold boot: the kept arrangement -- second monitor on the left, and
    #    primary -- comes back after login, for these two monitors.
    d=Desktop(out/'cold-boot',disk,graphics='virtio-dual')
    try:
        d.login()
        d.g.wait(b'home: saved display arrangement restored',120)
        until(lambda:(lambda L:L[1]['primary'] and L[1]['x']==0 and L[0]['x']==1024 and not L[0]['primary'])(layout()['outs']),
              'saved arrangement not restored')
        until(lambda:bar_on(0,1024) and desktop()['cw']==2304,'shell did not follow the restored arrangement')
        both('cold-boot-restored')
        assert b'PANIC' not in d.g.seen
    finally:d.g.close()
    (out/'results.json').write_text(json.dumps({'graphics':'virtio-dual','checks':[
        'second head extends the desktop','menu bar and dock on the primary','window crosses the seam',
        'maximize fills its own monitor','arrangement trial and keep','windows move with their monitor',
        'primary moves the menu bar','mirror trial','revert restores the arrangement','unsaved document preserved',
        'kept arrangement restored after cold boot and login']},indent=2)+'\n')
    print('PASS: two monitors -- extend, cross-monitor windows, arrangement, primary, mirror, revert, cold-boot restore',flush=True)

if __name__=='__main__':main()
