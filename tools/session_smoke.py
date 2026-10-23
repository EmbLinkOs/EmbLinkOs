#!/usr/bin/env python3
"""Exercise account setup, login and lock with real pointer/keyboard input."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
from recovery_smoke import Guest,ROOT

out=Path(tempfile.mkdtemp(prefix='emblink-session-'));print(f'Logs: {out}',flush=True)
disk=out/'session.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
shutil.copyfile(ROOT/'kernel/kernel.elf',out/'symbols.elf')
g=None

def inspect(expression):
    result=subprocess.run(['gdb','-q','-nx','-batch',str(out/'symbols.elf'),
        '-ex','target remote '+str(g.out/'gdb.sock'),'-ex',expression,'-ex','detach'],
        capture_output=True,text=True,timeout=20,check=True)
    return result.stdout

def ready_debug():
    g.qmp.cmd('human-monitor-command',**{'command-line':'gdbserver unix:'+str(g.out/'gdb.sock')+',server=on,wait=off'})

def pointer():
    s=inspect('printf "POINTER %d %d\\n", \'kernel/drivers/input/mouse.c\'::g_x, \'kernel/drivers/input/mouse.c\'::g_y')
    m=re.search(r'POINTER (\d+) (\d+)',s);assert m,s
    return tuple(map(int,m.groups()))

def click(x,y):
    px,py=pointer()
    for _ in range(30):
        if (px,py)==(x,y):break
        dx=max(-80,min(80,x-px));dy=max(-80,min(80,y-py))
        g.qmp.cmd('human-monitor-command',**{'command-line':f'mouse_move {dx} {dy}'})
        g.pump(.12);px,py=pointer()
    else:raise TimeoutError('pointer did not arrive')
    for b in (1,0):
        g.qmp.cmd('human-monitor-command',**{'command-line':f'mouse_button {b}'})
        g.pump(.2)

def keys(text):
    mapping={' ':'spc','/':'slash','.':'dot','-':'minus','\n':'ret','"':'shift-apostrophe'}
    for c in text:
        key=mapping.get(c,('shift-'+c.lower()) if c.isupper() else c)
        assert c.isalnum() or c in mapping,c
        g.qmp.cmd('human-monitor-command',**{'command-line':'sendkey '+key})
        g.pump(.13)

def shot(name):
    g.pump(1);g.qmp.screendump(str(out/(name+'.ppm')))

def session_flags():
    s=inspect('printf "SESSION %u %u\\n", \'kernel/process/session.c\'::flags, \'kernel/process/session.c\'::gate')
    m=re.search(r'SESSION (\d+) (\d+)',s);assert m,s;return tuple(map(int,m.groups()))

try:
    g=Guest(out/'setup',disk);g.wait(b'EmbLink OS Setup: first frame presented');ready_debug()
    click(150,305);keys('alex');click(150,347);keys('laptop123');click(150,389);keys('laptop123')
    shot('setup-filled');click(180,430)
    g.wait(b'EmbLink OS Login: first frame presented',180)
    shot('login');print('session: account created and login displayed',flush=True)
    # Login panel has one fewer field than Setup.
    click(150,305);keys('alex');click(150,347);keys('laptop123');click(180,389)
    g.wait(b'home: desktop ready',180);g.wait(b'TopBar: first frame presented',180)
    assert session_flags()[0]&4 and not session_flags()[0]&1
    shot('desktop');print('session: authenticated desktop ready',flush=True)
    g.qmp.cmd('human-monitor-command',**{'command-line':'sendkey ctrl-alt-l'})
    g.wait(b'EmbLink OS Login: first frame presented',120,start=g.seen.rfind(b'home: desktop ready'))
    shot('locked');state,owner=session_flags();assert state&1 and owner not in (0,0xffffffff)
    print('session: lock gate and greeter active',flush=True)
    # Unlock displays a fixed account name instead of an editable username.
    click(150,347);keys('wrongpass');click(180,389);g.pump(5)
    assert session_flags()[0]&1
    shot('wrong-password');print('session: wrong password leaves session locked',flush=True)
    click(150,347)
    g.qmp.cmd('human-monitor-command',**{'command-line':'sendkey ctrl-a'});g.pump(.2);keys('laptop123');click(180,389)
    g.wait(b'init: session unlocked',180);assert not session_flags()[0]&1
    shot('unlocked');print('session: correct password restores desktop',flush=True)
    click(287,553);g.wait(b'Files: first frame presented');g.pump(1)
    file_focus=inspect('printf "FOCUS %u\\n", \'kernel/gfx/compositor.c\'::g_top_id')
    click(345,553);g.wait(b'Terminal: first frame presented');g.pump(1)
    terminal_focus=inspect('printf "FOCUS %u\\n", \'kernel/gfx/compositor.c\'::g_top_id')
    assert re.search(r'FOCUS (\d+)',file_focus).group(1)!=re.search(r'FOCUS (\d+)',terminal_focus).group(1)
    g.qmp.cmd('human-monitor-command',**{'command-line':'sendkey alt-tab'});g.pump(1)
    switched=inspect('printf "FOCUS %u\\n", \'kernel/gfx/compositor.c\'::g_top_id')
    assert re.search(r'FOCUS (\d+)',switched).group(1)==re.search(r'FOCUS (\d+)',file_focus).group(1)
    g.qmp.cmd('human-monitor-command',**{'command-line':'sendkey alt-shift-tab'});g.pump(1)
    keys('/data/apps/notify/notify.elf Build done\n');g.pump(3)
    result=inspect('printf "NOTICES %u\\n", \'kernel/ipc/notify.c\'::count')
    assert re.search(r'NOTICES (\d+)',result).group(1)=='1',result
    click(430,13);shot('notification-center');click(430,13)
    shot('apps-and-notification');print('session: Files/Terminal switching and session notification OK',flush=True)
    click(345,553);keys('/data/apps/devicecheck/devicecheck.elf "--session-writer"\n')
    g.wait(b'.logout-write-check');g.pump(1)
    mark=len(g.seen);click(75,13);shot('logout-menu');click(110,139)
    g.wait(b'init: desktop exited',180,start=mark)
    g.wait(b'EmbLink OS Login: first frame presented',180,start=mark)
    shot('logged-out')
    # Every former session member must be dead, including both app windows.
    epoch_text=inspect('printf "EPOCH %u\\n", \'kernel/process/session.c\'::epoch')
    epoch=int(re.search(r'EPOCH (\d+)',epoch_text).group(1))
    terms=[f"(('kernel/process/process.c'::process_table[{i}].session_id=={epoch}) && ('kernel/process/process.c'::process_table[{i}].live_thread_count>0))" for i in range(64)]
    result=inspect('printf "MEMBERS %u\\n", '+'+'.join(terms))
    assert re.search(r'MEMBERS (\d+)',result).group(1)=='0',result
    g.command('recovery check',b'integrity OK',240)
    print('session: logout drained disk writes, terminated the entire epoch and left a valid filesystem',flush=True)
    g.close();g=Guest(out/'reboot',disk);g.wait(b'EmbLink OS Login: first frame presented');ready_debug()
    assert b'first-boot setup started' not in g.seen
    click(150,305);keys('alex');click(150,347);keys('laptop123');click(180,389)
    g.wait(b'home: desktop ready',180);g.wait(b'TopBar: first frame presented',180)
    result=inspect('printf "NOTICES %u\\n", \'kernel/ipc/notify.c\'::count')
    assert re.search(r'NOTICES (\d+)',result).group(1)=='0',result
    shot('persistent-login');print('session: account and login survive a new boot; notification history cleared',flush=True)

finally:
    if g:
        if g.p.poll() is None:
            try:g.qmp.screendump(str(out/'last.ppm'))
            except (OSError,RuntimeError):pass
        g.close()
print('PASS: account persistence, login/lock/unlock, application switching, notifications and complete logout',flush=True)
