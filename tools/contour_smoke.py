#!/usr/bin/env python3
"""Contour's native UX acceptance. Read-only GDB observations; actions use QMP.

A fresh disposable UEFI/NVMe image, actual account and confined applications.
Captures retain original pixels. Build binaries before starting this test.
"""
from pathlib import Path
import json,shutil,tempfile,time
from workspace_smoke import Desktop
from recovery_smoke import ROOT
from native_ui import NativeUI

def main():
    out=Path(tempfile.mkdtemp(prefix='contour-',dir='/workspace/.setup'))
    print(f'Artifacts: {out}',flush=True)
    disk=out/'desktop.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop(out/'first',disk)
    def desktop():
        return d.debug('''import gdb,json
s=gdb.parse_and_eval("'kernel/gfx/compositor.c'::g_desktop")
print('RESULT '+json.dumps({k:int(s[k]) for k in ['workspace','workspace_count','top','bottom','left','right','background','cursor_style','cursor_size','flags']}))''')
    def eventually(check,message):
        end=time.monotonic()+30
        while not check():
            if time.monotonic()>end:raise AssertionError(message)
            d.g.pump(.5)
    def scalar(app,w,symbol):return int.from_bytes(d.read(app,w['pid'],symbol,4),'little',signed=True)
    def text(app,w,symbol):return d.read(app,w['pid'],symbol,256).split(b'\0',1)[0].decode()
    def close(w):
        d.click(w['x']+20,w['y']+18)
        d.g.pump(1)
        if any(a['id']==w['id'] for a in d.state()['windows']):
            (out/('close-'+w['title']+'.json')).write_text(json.dumps(NativeUI(d).labels(w,True),indent=2))
        eventually(lambda:all(a['id']!=w['id'] for a in d.state()['windows']),'close click did not close '+w['title'])
    try:
        d.g.wait(b'EmbLink OS Setup: first frame presented');d.snap('setup')
        d.type('tester\tcorrect horse 1\tcorrect horse 1\n')
        d.g.wait(b"setup: account 'tester' created")
        d.g.wait(b'EmbLink OS Login: first frame presented');d.snap('login')
        d.login();d.snap('desktop')
        assert desktop()['workspace_count']==4
        edit=d.launch('Editor','Editor');d.click(edit['x']+120,edit['y']+155)
        d.type('A workspace of our own.');d.g.pump(1)
        eventually(lambda:text('edit',edit,'g_doc')=='A workspace of our own.','Editor input')
        d.snap('editor')
        d.key('meta_l-2');eventually(lambda:desktop()['workspace']==1,'switch to workspace 2')
        d.snap('workspace-empty')
        calc=d.launch('Calculator','Calculator');d.type('2');d.key('shift-equal');d.type('3\n')
        d.g.wait(b'calc: = 5');d.snap('calculator')
        d.key('meta_l-1');eventually(lambda:desktop()['workspace']==0,'return to workspace 1')
        assert text('edit',edit,'g_doc')=='A workspace of our own.'
        d.snap('workspace-return')
        d.key('meta_l-shift-2');d.g.pump(1)
        assert desktop()['workspace']==0
        d.type('hidden');assert text('edit',edit,'g_doc')=='A workspace of our own.','hidden workspace received keyboard input'
        d.key('meta_l-2');eventually(lambda:desktop()['workspace']==1,'reach moved Editor')
        d.snap('workspace-moved')
        settings=d.launch('Settings','Settings');d.snap('appearance-dark')
        # Paper preset: the real settings file, desktop palette and open apps update.
        d.click(settings['x']+365,settings['y']+163)
        eventually(lambda:desktop()['background']>0xc0c0c0,'Paper preset did not reach compositor')
        d.snap('appearance-paper')
        close(settings);d.g.pump(2);d.snap('apps-paper')
        settings=d.launch('Settings','Settings')
        assert scalar('settings',settings,'g_pane')==0
        # Workspace controls are in the third navigation row.
        d.click(settings['x']+110,settings['y']+173)
        eventually(lambda:scalar('settings',settings,'g_pane')==2,'workspace settings navigation')
        d.snap('windows-settings')
        d.click(settings['x']+110,settings['y']+206)
        eventually(lambda:scalar('settings',settings,'g_pane')==3,'accessibility navigation')
        d.snap('accessibility')
        # Each application is opened through the actual searchable launcher.
        close(settings)
        for name,title in [('Terminal','Terminal'),('Files','Files'),('Photos','Photos'),('Music','Music'),('Activity Monitor','Activity Monitor'),('Notifications','Notifications')]:
            w=d.launch(name,title);d.snap(name.lower().replace(' ','-'));close(w)
        # Fullscreen system shortcut continues to launch and dismiss the library.
        d.key('meta_l-spc');d.g.pump(2);d.snap('application-library')
        d.key('meta_l-spc');d.g.pump(1)
        assert b'PANIC' not in d.g.seen
        (out/'desktop-state.json').write_text(json.dumps(desktop(),indent=2))
    finally:
        try:d.snap('last')
        finally:d.g.close()
    # Cold boot proves that the curated preset survives outside all live caches.
    d=Desktop(out/'second',disk)
    try:
        d.login();eventually(lambda:desktop()['background']>0xc0c0c0,'Paper preset lost after reboot')
        d.snap('paper-after-reboot')
        ui=NativeUI(d);bar=d.window('TopBar');ui.click_text(bar,'EmbLink');d.g.pump(1)
        ui.click_text(bar,'Lock Screen')
        def locked():
            return d.debug("import gdb,json\nprint('RESULT '+json.dumps(int(gdb.parse_and_eval(\"'kernel/process/session.c'::gate\"))))")
        eventually(lambda:locked()>0,'lock did not acquire screen ownership');d.snap('lock-paper')
        before=desktop()['workspace'];d.key('meta_l-2');d.g.pump(1)
        assert desktop()['workspace']==before,'workspace shortcut bypassed lock'
        d.type('correct horse 1\n');eventually(lambda:locked()==0,'password did not unlock');d.snap('unlocked-paper')
        d.g.command('recovery check',b'integrity OK',240)
    finally:d.g.close()
    print('PASS: authenticated Contour shell, keyboard launch, real workspaces, hidden-input isolation, live Paper preset, application surfaces and cold preference persistence',flush=True)

if __name__=='__main__':main()
