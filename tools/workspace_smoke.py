#!/usr/bin/env python3
"""Exercise real document windows, common dialogs, recovery and a cold reboot.

Uses a disposable NVMe image and a real authenticated account. GDB only reads
kernel window ownership and user buffers; all app actions use mouse/keyboard.
Keep the kernel/app binaries unchanged while the guest runs.
"""
from pathlib import Path
import json
import re
import shutil
import subprocess
import tempfile
import time
from recovery_smoke import Guest, ROOT
from lock_test import keyname
import shell_shot as S

class Desktop:
    def __init__(self,out,disk):
        self.out=out;self.g=Guest(out,disk,boot_nvme=True,resolution=(1280,800))
        self.sock=out/'gdb.sock';self.symbols={}
    def login(self,setup=False):
        g=self.g
        if setup:
            g.wait(b'EmbLink OS Setup: first frame presented')
            self.type('tester\tcorrect horse 1\tcorrect horse 1\n')
            g.wait(b"setup: account 'tester' created")
        g.wait(b'EmbLink OS Login: first frame presented')
        self.type('tester\tcorrect horse 1\n')
        g.wait(b"init: authenticated session 'tester'")
        g.wait(b'TopBar: first frame presented');g.wait(b'home: time zone');g.pump(2)
        g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{self.sock},server=on,wait=off'})
    def key(self,key):
        self.g.qmp.cmd('human-monitor-command',**{'command-line':'sendkey '+key});self.g.pump(.2)
    def type(self,text):
        for c in text:self.key(keyname(c))
    def debug(self,body):
        script=self.out/'inspect.gdb'
        script.write_text('set pagination off\ntarget remote '+str(self.sock)+'\npython\n'+body+'\nend\ndetach\nquit\n')
        p=subprocess.run(['gdb','-q','-nx','-batch',str(ROOT/'kernel/kernel.elf'),'-x',str(script)],capture_output=True,text=True,timeout=20,check=True)
        match=re.search(r'RESULT (.*)',p.stdout)
        if not match:raise RuntimeError(p.stdout+p.stderr)
        return json.loads(match.group(1))
    def state(self):
        return self.debug('''import gdb,json
w=gdb.parse_and_eval("'kernel/gfx/compositor.c'::g_wins")
rows=[]
for i in range(w.type.range()[1]+1):
    a=w[i]
    if int(a['used']):
        rows.append({k:int(a[k]) for k in ['id','pid','x','y','cw','ch','visible','presented','asked_close_ms','pending_action'] } | {'title':a['title'].string()})
x=int(gdb.parse_and_eval("'kernel/drivers/input/mouse.c'::g_x"))
y=int(gdb.parse_and_eval("'kernel/drivers/input/mouse.c'::g_y"))
print('RESULT '+json.dumps({'windows':rows,'mouse':[x,y]}))''')
    def window(self,title):
        matches=[w for w in self.state()['windows'] if w['title']==title and w['visible']]
        assert matches,f'missing {title} window';return matches[-1]
    def wait_window(self,title):
        end=time.monotonic()+60
        while time.monotonic()<end:
            try:
                w=self.window(title)
                if w['presented']:return w
            except AssertionError:pass
            self.g.pump(.5)
        raise TimeoutError(title)
    def read(self,app,pid,symbol,length,offset=0):
        if app not in self.symbols:
            p=subprocess.run(['x86_64-elf-nm','-S','--defined-only',str(ROOT/f'build/{app}.elf')],capture_output=True,text=True,check=True)
            # nm has address, size, type, name; retain local app symbols too.
            self.symbols[app]={line.split()[3]:int(line.split()[0],16) for line in p.stdout.splitlines() if len(line.split())==4}
        address=self.symbols[app][symbol]+offset
        return bytes.fromhex(self.debug(f'''import gdb,json
ps=gdb.parse_and_eval("'kernel/process/process.c'::process_table")
cr3=next(int(ps[i]['pml4_phys']) for i in range(ps.type.range()[1]+1) if int(ps[i]['pid'])=={pid})
inf=gdb.selected_inferior();base=0xffff800000000000

def phys(va):
    table=cr3
    for shift in [39,30,21,12]:
        entry=int.from_bytes(inf.read_memory(base+table+((va>>shift)&511)*8,8).tobytes(),'little')
        if not entry&1:raise RuntimeError('unmapped app page')
        if shift in [30,21] and entry&128:return (entry&0x000ffffffffff000&~((1<<shift)-1))+(va&((1<<shift)-1))
        table=entry&0x000ffffffffff000
    return table+(va&4095)
va={address};left={length};data=bytearray()
while left:
    n=min(left,4096-(va&4095));data.extend(inf.read_memory(base+phys(va),n));left-=n;va+=n
print('RESULT '+json.dumps(data.hex()))'''))
    def click(self,x,y):
        end=time.monotonic()+30
        while True:
            ax,ay=self.state()['mouse']
            if (ax,ay)==(x,y):break
            if time.monotonic()>end:raise TimeoutError('pointer')
            dx=max(-40,min(40,x-ax));dy=max(-40,min(40,y-ay))
            self.g.qmp.cmd('human-monitor-command',**{'command-line':f'mouse_move {dx} {dy}'});self.g.pump(.08)
        self.g.qmp.cmd('human-monitor-command',**{'command-line':'mouse_button 1'});self.g.pump(.8)
        self.g.qmp.cmd('human-monitor-command',**{'command-line':'mouse_button 0'});self.g.pump(1)
    def snap(self,name):
        self.g.pump(1);p=self.out.parent/(name+'.ppm');self.g.qmp.screendump(str(p));print('Capture: '+name,flush=True);return p
    def launch(self,name,title):
        self.key('meta_l-spc');self.g.pump(2)
        start=len(self.g.seen);self.type(name+'\n');w=self.wait_window(title)
        self.g.wait((title+': first frame presented').encode(),start=start)
        self.g.pump(1);return w

def main():
    from native_ui import NativeUI
    out=Path(tempfile.mkdtemp(prefix='emblink-workspace-'));print(f'Logs and captures: {out}',flush=True)
    disk=out/'workspace.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop(out/'first-boot',disk)
    original='Our first daily use document.\nSaved safely on NVMe.\n'
    extra='An unsaved draft survives a crash.\n'
    tab1='Workspace recovery for a real desktop.\n'
    tab2='Second unsaved tab.\n'
    def text(app,w,symbol='g_doc',offset=0):
        return d.read(app,w['pid'],symbol,256,offset).split(b'\0',1)[0].decode()
    def expect_text(app,w,expected,symbol='g_doc',offset=0):
        deadline=time.monotonic()+15
        while True:
            actual=text(app,w,symbol,offset)
            if actual==expected:return
            if time.monotonic()>deadline:raise AssertionError(f'{app} text: {actual!r}, expected {expected!r}')
            d.g.pump(.5)
    def integer(app,w,symbol):return int.from_bytes(d.read(app,w['pid'],symbol,4),'little',signed=True)
    def threads(pid):
        return d.debug(f"import gdb,json\np=gdb.parse_and_eval(\"'kernel/process/process.c'::process_table\")\nprint('RESULT '+json.dumps(next(int(p[i]['live_thread_count']) for i in range(p.type.range()[1]+1) if int(p[i]['pid'])=={pid})))")
    def save_as(name,documents=True):
        panel=d.wait_window('Save');d.g.pump(1)
        ui=NativeUI(d)
        if documents:ui.click_text(panel,'Documents')
        current=text('filepanel',panel,'g_name')
        ui.click_text(panel,current or 'Filename');d.key('meta_l-a');d.type(name)
        ui.click_text(panel,'Save',rightmost=True);d.g.pump(2)
        assert all(w['title']!='Save' for w in d.state()['windows']),'save dialog did not finish'
    try:
        d.login(setup=True);d.snap('desktop')
        w=d.launch('Editor','Editor')
        d.click(w['x']+100,w['y']+150);d.type(original)
        expect_text('edit',w,original)
        d.key('up');d.g.pump(1)
        assert all(v['title']!='Save' for v in d.state()['windows']), 'Up was mistaken for Ctrl+S'
        d.key('ctrl-s');panel=d.wait_window('Save');d.snap('shared-save')
        panel_threads=threads(panel['pid'])
        save_as('Milestone.txt')
        assert text('edit',w,'g_path')=='/home/tester/Documents/Milestone.txt'
        assert text('edit',w,'g_status').startswith('Saved ')
        d.snap('document-saved')
        NativeUI(d).click_text(w,'Save As');panel=d.wait_window('Save')
        assert threads(panel['pid'])==panel_threads,'repeated picker windows leaked appearance workers'
        NativeUI(d).click_text(panel,'Save',rightmost=True);d.g.pump(1)
        assert integer('filepanel',panel,'g_replace')==1,'existing Save As skipped replacement confirmation'
        d.snap('replace-confirmation');d.key('esc');d.key('esc');d.g.pump(1)
        assert text('edit',w,'g_path')=='/home/tester/Documents/Milestone.txt'
        d.click(w['x']+100,w['y']+150);d.key('meta_l-a');d.key('right');d.type(extra)
        expect_text('edit',w,original+extra)
        d.key('meta_l-w');d.g.pump(8)
        assert d.window('Editor')['asked_close_ms']==0
        assert integer('edit',w,'g_confirm')==1,'GUI+W bypassed the dirty guard'
        d.snap('editor-unsaved-close');d.key('esc')
        assert integer('edit',w,'g_confirm')==0
        n=d.launch('Note++','Note++');d.type(tab1)
        expect_text('notepp',n,tab1,'g_text')
        d.key('ctrl-n');d.g.pump(1);d.type(tab2)
        expect_text('notepp',n,tab2,'g_text',256*1024)
        d.key('meta_l-w');d.g.pump(8)
        assert d.window('Note++')['asked_close_ms']==0
        assert integer('notepp',n,'g_confirm_close')==0
        d.snap('workspace-unsaved-close')
        NativeUI(d).click_text(n,'Discard');d.g.pump(1)
        assert integer('notepp',n,'g_confirm_close')==1,'quit did not advance to the next dirty tab'
        d.key('esc');d.key('meta_l-w');d.g.pump(1)
        assert integer('notepp',n,'g_confirm_close')==0,'cancelling quit lost the first tab dirty state'
        d.key('esc');d.g.pump(3);d.snap('workspace-before-crash')
        # Terminate the VM without logout or shutdown: checkpoints and the saved
        # document must already be on the disposable NVMe image.
        d.g.close();d=Desktop(out/'cold-recovery',disk);d.login()
        w=d.launch('Editor','Editor')
        assert text('edit',w)==original+extra,'Editor draft did not survive the cold boot'
        assert text('edit',w,'g_path')=='/home/tester/Documents/Milestone.txt'
        d.snap('editor-recovered');d.key('ctrl-s');d.g.pump(2)
        assert text('edit',w,'g_status').startswith('Saved ')
        d.key('esc');d.g.pump(1)
        assert all(x['title']!='Editor' for x in d.state()['windows'])
        n=d.launch('Note++','Note++')
        recovered={text('notepp',n,'g_text'),text('notepp',n,'g_text',256*1024)}
        assert recovered=={tab1,tab2},'a draft was skipped or overwritten during multi-tab recovery'
        assert b'Note++: recovered unsaved workspace' in d.g.seen
        d.snap('workspace-recovered')
        current=integer('notepp',n,'g_cur');saved_text=text('notepp',n,'g_text',current*256*1024)
        d.key('ctrl-s');save_as('Recovered.txt');d.snap('workspace-saved')
        d.key('esc');d.g.pump(1)
        # One unsaved tab remains. Discard it explicitly; the saved tab must
        # stay in the session history when the process exits.
        assert integer('notepp',n,'g_confirm_close')>=0
        NativeUI(d).click_text(n,'Discard');d.g.pump(2)
        assert all(x['title']!='Note++' for x in d.state()['windows'])
        d.g.close();d=Desktop(out/'saved-reopen',disk);d.login()
        n=d.launch('Note++','Note++')
        assert text('notepp',n,'g_text')==saved_text,'saved workspace failed to reopen after reboot'
        assert text('notepp',n,'g_path_field')=='/home/tester/Documents/Recovered.txt'
        d.snap('workspace-after-reboot')
        d.key('esc');d.g.pump(1)
        # Reopen the persisted original through the system Open panel.
        w=d.launch('Editor','Editor');d.key('ctrl-o');panel=d.wait_window('Open');d.g.pump(1)
        NativeUI(d).click_text(panel,'Documents');d.g.pump(1)
        d.snap('shared-open')
        NativeUI(d).click_text(panel,'Milestone.txt')
        NativeUI(d).click_text(panel,'Open',rightmost=True);d.g.pump(2)
        assert text('edit',w)==original+extra,'shared Open selected the wrong file or reset EDIT_FILE'
        d.snap('document-after-reboot');d.key('esc');d.g.pump(1)
        d.g.command('recovery check',b'integrity OK',240)
        assert b'PANIC' not in d.g.seen
        print('PASS: shared dialogs, durable save, both dirty guards, multi-tab quit cancellation, cold draft recovery, saved workspace reopening and cold filesystem integrity',flush=True)
    finally:
        try:d.snap('last')
        finally:d.g.close()

if __name__=='__main__':main()
