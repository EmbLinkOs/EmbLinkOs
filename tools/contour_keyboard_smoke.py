#!/usr/bin/env python3
"""Real QMP keyboard navigation; GDB reads focus geometry and document state only."""
from pathlib import Path
import shutil,tempfile,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_ui import NativeUI

def main():
    out=Path(tempfile.mkdtemp(prefix='contour-keyboard-',dir='/workspace/.setup'));print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800));d.sock=d.out/'gdb.sock';d.symbols={}
    def eventually(check,message):
        end=time.monotonic()+30
        while not check():
            if time.monotonic()>end:raise AssertionError(message)
            d.g.pump(.5)
    def scalar(app,w,name):return int.from_bytes(d.read(app,w['pid'],name,1 if name=='g_reset_confirm' else 4),'little',signed=True)
    def doc(w):return d.read('edit',w['pid'],'g_doc',256).split(b'\0',1)[0].decode()
    def focus(w,label):
        rows=[r for r in ui.labels(w) if r['text']==label]
        if not rows:return False
        target=rows[0]['rect'];chain=ui.labels(w,True)['g_focused']
        if not chain:return False
        x,y,width,height=chain[0]['rect']
        return x<=target[0]+1 and y<=target[1]+1 and x+width>=target[0]+target[2]-1 and y+height>=target[1]+target[3]-1
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone');d.g.pump(2)
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        ui=NativeUI(d);w=d.launch('Settings','Settings');ui.click_text(w,'Paper');d.g.pump(3)
        ui.reveal(w,'Reset appearance…');ui.click_text(w,'Reset appearance…')
        eventually(lambda:focus(w,'Cancel'),'dialog did not focus its safe first action');d.snap('keyboard-dialog')
        d.key('shift-tab');eventually(lambda:focus(w,'Reset'),'reverse Tab did not wrap inside the dialog');d.snap('reverse-dialog')
        d.key('tab');eventually(lambda:focus(w,'Cancel'),'forward Tab escaped the dialog')
        d.type('background');assert scalar('settings',w,'g_reset_confirm')==1
        d.key('ret');eventually(lambda:scalar('settings',w,'g_reset_confirm')==0,'Return did not cancel reset')
        eventually(lambda:focus(w,'Reset appearance…'),'dialog did not restore its initiating control')
        d.key('spc');eventually(lambda:scalar('settings',w,'g_reset_confirm')==1,'restored control did not reopen with Space')
        d.key('esc');eventually(lambda:scalar('settings',w,'g_reset_confirm')==0,'Escape did not cancel the overlay')
        assert d.window('Settings')['id']==w['id'],'Escape closed Settings'
        bar=d.window('TopBar');ui.click_text(bar,'EmbLink');d.key('right');d.g.pump(2)
        eventually(lambda:focus(bar,'✓  Workspace 1'),'Right did not reach the adjacent workspace menu')
        d.key('down');eventually(lambda:focus(bar,'    Workspace 2'),'Down did not select the second workspace');d.snap('keyboard-menu')
        d.key('home');eventually(lambda:focus(bar,'✓  Workspace 1'),'Home did not reach the first menu item')
        d.key('esc');eventually(lambda:d.window('TopBar')['ch']<100,'Escape did not release menu ownership')
        assert d.window('Settings')['id']==w['id']
        edit=d.launch('Editor','Editor');d.click(edit['x']+120,edit['y']+155)
        d.type('Keys stay in this document.');eventually(lambda:doc(edit)=='Keys stay in this document.','Editor typing')
        d.key('meta_l-w');eventually(lambda:scalar('edit',edit,'g_confirm')==1,'dirty close did not open a decision')
        eventually(lambda:focus(edit,'Cancel'),'dirty guard did not focus Cancel');d.snap('document-dialog')
        d.key('ctrl-s');d.g.pump(1)
        assert not any(a['title']=='Save document' for a in d.state()['windows']),'an app shortcut bypassed the dialog'
        d.key('shift-tab');eventually(lambda:focus(edit,'Save changes'),'reverse Tab did not stay in the dirty guard')
        d.key('tab');eventually(lambda:focus(edit,'Cancel'),'Tab failed to return to Cancel')
        d.key('ret');eventually(lambda:scalar('edit',edit,'g_confirm')==0,'keyboard Cancel did not retain the document')
        d.type(' Still here.');eventually(lambda:doc(edit)=='Keys stay in this document. Still here.','document focus was not restored')
        assert b'PANIC' not in d.g.seen
        print('PASS: bidirectional dialog traversal, safe keyboard actions, focus restoration, menu arrows/Home/Escape and dirty-document shortcut isolation',flush=True)
    finally:
        try:d.snap('last')
        finally:d.g.close()

if __name__=='__main__':main()
