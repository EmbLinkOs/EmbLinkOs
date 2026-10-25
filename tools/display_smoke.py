#!/usr/bin/env python3
"""Real UEFI/NVMe display acceptance via QMP input and read-only GDB inspection.

Build 1280x800 images and uefi-usb.img before running. Never rebuild binaries
while this guest is running. All accounts, preferences and disks are disposable.
Pass --graphics bochs for the independent DISPI backend.
"""
import argparse,json,shutil,tempfile,time
from pathlib import Path
from recovery_smoke import ROOT
from workspace_smoke import Desktop
from native_ui import NativeUI


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--graphics',choices=['virtio','bochs'],default='virtio')
    args=parser.parse_args();out=Path(tempfile.mkdtemp(prefix='emblink-display-'+args.graphics+'-'))
    print('Logs and captures: '+str(out),flush=True)
    disk=out/'display.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop(out/'first-boot',disk,graphics=args.graphics)
    def monitor():
        return d.debug('''import gdb,json
s=gdb.parse_and_eval("'kernel/display/control.c'::state")
f=gdb.parse_and_eval("'kernel/drivers/video/framebuffer.c'::fb")
print('RESULT '+json.dumps({k:int(s[k]) for k in ['phase','generation','owner','token','error']} | {k:int(f[k]) for k in ['width','height']} | {'focus':int(gdb.parse_and_eval("'kernel/gfx/compositor.c'::g_top_id"))}))''')
    def until(test,description,seconds=20):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            if test():return
            d.g.pump(.4)
        raise AssertionError(description+'; '+str(monitor()))
    def geometry(w,h):
        s=monitor();return (s['width'],s['height'])==(w,h)
    def reflow(w,h):
        rows=d.state()['windows'];desktop=next(x for x in rows if x['desktop'])
        bar=next(x for x in rows if x['title']=='TopBar')
        return desktop['cw']==w and desktop['ch']==h and bar['cw']==w
    def change(w,h):
        settings=d.window('Settings');ui=NativeUI(d)
        ui.reveal(settings,'Resolution')
        # The current mode is the dropdown label; opening it reveals actual choices.
        labels=ui.labels(settings)
        current=next(r for r in labels if ' × ' in r['text'] and not r['text'].endswith('mm'))['text']
        ui.click_text(settings,current);ui.reveal(settings,f'{w} × {h}')
        ui.click_text(settings,f'{w} × {h}');ui.reveal(settings,'Apply Resolution');ui.click_text(settings,'Apply Resolution')
        until(lambda:monitor()['phase']==3,'trial did not start')
        until(lambda:geometry(w,h) and reflow(w,h),'shell did not reflow')
        assert d.window('Settings')['id']==settings['id'],'reflow hid Settings'
        assert monitor()['focus']==settings['id'],'reflow stole Settings focus'
        return settings,NativeUI(d)
    try:
        d.login(setup=True);d.snap('desktop-before')
        editor=d.launch('Editor','Editor');d.click(editor['x']+100,editor['y']+130);d.type('Display changes must preserve this unsaved document.\n')
        settings=d.launch('Settings','Settings');ui=NativeUI(d);ui.click_text(settings,'Displays')
        d.snap('display-settings')
        old=monitor();old_size=(old['width'],old['height'])
        settings,ui=change(800,600);d.snap('resolution-trial-small')
        ui.click_text(settings,'Revert');until(lambda:geometry(*old_size) and monitor()['phase']==0,'explicit revert failed');until(lambda:reflow(*old_size),'revert reflow failed')
        saved=(1280,800) if old_size==(1024,768) else (1024,768)
        settings,ui=change(*saved);d.snap('resolution-confirmation')
        ui.click_text(settings,'Keep Changes');until(lambda:monitor()['phase']==0,'keep failed');d.snap('resolution-kept')
        assert d.read('edit',editor['pid'],'g_doc',160).split(b'\0',1)[0]==b'Display changes must preserve this unsaved document.\n'
        settings,ui=change(800,600);d.snap('resolution-timeout')
        until(lambda:geometry(*saved) and monitor()['phase']==0,'timeout did not restore',45)
        d.snap('resolution-timeout-restored')
        settings,ui=change(800,600);d.key('meta_l-w')
        until(lambda:geometry(*saved) and monitor()['phase']==0,'requester close did not restore')
        d.snap('resolution-owner-exit-restored')
        settings=d.launch('Settings','Settings');ui=NativeUI(d);ui.click_text(settings,'Displays')
        settings,ui=change(800,600)
        bar=d.window('TopBar');ui.click_text(bar,'EmbLink');ui.click_text(bar,'Lock Screen')
        d.g.wait(b'home: screen LOCKED')
        until(lambda:geometry(*saved) and monitor()['phase']==0,'session lock did not restore')
        until(lambda:reflow(*saved),'locked desktop did not reflow')
        home=next(w for w in d.state()['windows'] if w['desktop'])
        until(lambda:int.from_bytes(d.read('home',home['pid'],'g_full_present',4),'little')==0,'locked desktop did not finish repainting')
        d.g.pump(2);d.snap('resolution-lock-restored')
        assert b'PANIC' not in d.g.seen and b'used ring timeout' not in d.g.seen
    finally:d.g.close()
    # A cold boot must restore the confirmed user preference only after login.
    d=Desktop(out/'cold-boot',disk,graphics=args.graphics)
    try:
        d.login();until(lambda:geometry(*saved) and monitor()['phase']==0,'confirmed mode did not survive cold reboot',35)
        until(lambda:reflow(*saved),'restored shell geometry')
        d.snap('resolution-cold-boot');assert b'PANIC' not in d.g.seen
    finally:d.g.close()
    (out/'results.json').write_text(json.dumps({'graphics':args.graphics,'checks':['live mode switch','shell reflow','keep','explicit revert','timeout','requester exit','session lock','unsaved document preserved','confirmed preference cold boot']},indent=2)+'\n')
    print('PASS: '+args.graphics+' modesetting, shell/app reflow, confirmation, explicit/timeout/exit rollback and cold-boot persistence',flush=True)

if __name__=='__main__':main()
