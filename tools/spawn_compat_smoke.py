#!/usr/bin/env python3
"""Native POSIX spawn and child/pipe regressions and a real Terminal invocation using QMP input."""
from pathlib import Path
import os,re,shutil,tempfile,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_terminal import TerminalState
from lock_test import keyname

def main():
    out=Path(tempfile.mkdtemp(prefix='spawn-compat-',dir='/workspace/.setup' if os.path.isdir('/workspace/.setup') else None));print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800));d.sock=d.out/'gdb.sock';d.symbols={}
    def eventually(check,message):
        deadline=time.monotonic()+120
        while not check():
            if time.monotonic()>deadline:raise AssertionError(message)
            d.g.pump(.4)
    def type_text(value):
        for c in value:d.key('slash' if c=='/' else keyname(c))
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone')
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        for command in ['test spawn-compat','test spawn-compat','test process-compat','test handle reap','test sched wait','test terminal','test tty','test posix','test channel']:
            result=d.g.command(command,('[cmd] '+command+':').encode(),timeout=180)
            line=re.search(r'\[cmd\] '+re.escape(command)+r':[^\r\n]*',result)
            assert line and line.group().endswith('OK'),result
            assert 'FAIL' not in result,result
            print(line.group(),flush=True)
        screen=TerminalState(d);w=d.launch('Terminal','Terminal')
        eventually(lambda:screen.at_prompt(w),'native prompt')
        type_text('/data/apps/spawnprobe/spawnprobe.elf\n')
        eventually(lambda:'POSIX pipeline: 66049 bytes transformed' in screen.text(w) and screen.at_prompt(w),'graphical POSIX spawn checks')
        text=screen.text(w);match=re.search(r'spawnprobe: (\d+)/(\d+) checks passed',text)
        assert match and match[1]==match[2],text
        d.snap('posix-spawn-and-pipeline')
        type_text('echo 73921\n')
        eventually(lambda:'73921' in screen.text(w).splitlines() and screen.at_prompt(w),'shell remains usable')
        assert b'PANIC' not in d.g.seen
        print('PASS: ordered spawn actions, CLOEXEC, cwd/environment, retained child status, PTY/POSIX/channel regressions and graphical POSIX pipeline with a working shell afterward',flush=True)
    finally:
        try:d.snap('last')
        finally:d.g.close()

if __name__=='__main__':main()
