#!/usr/bin/env python3
"""The shell's prompt, live: rc.esh, the prompt builtin, the right prompt and
the transient prompt.

Driven over QMP like a person would. The first tab writes
~/.config/shell/rc.esh -- the right side shows the last command's status,
its duration and the time; accepted lines keep only the prompt character --
and Super+T opens a tab whose shell reads it. There a slow command shows
its duration, a failing one its status, and every line above the prompt is
drawn the short way. `prompt` lists the options; `prompt set transient off`
from the prompt itself puts the full prompt back.
"""
from pathlib import Path
import os,re,shutil,tempfile,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_terminal import TerminalState
from lock_test import keyname

CLOCK=re.compile(r'\d\d:\d\d:\d\d$')

def main():
    out=Path(tempfile.mkdtemp(prefix='shell-prompt-',dir='/workspace/.setup' if os.path.isdir('/workspace/.setup') else None));print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800));d.sock=d.out/'gdb.sock';d.symbols={}
    def eventually(check,message,seconds=60):
        end=time.monotonic()+seconds
        while not check():
            if time.monotonic()>end:raise AssertionError(message)
            d.g.pump(.4)
    def type_text(value):
        for c in value:d.key(keyname(c))
    def lines():return [l.rstrip() for l in screen.text(w).split('\n')]
    def prompt_row():
        st=screen.state(w);return st['text'][st['cursor'][1]].rstrip()
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone')
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        screen=TerminalState(d);w=d.launch('Terminal','Terminal');eventually(lambda:screen.at_prompt(w),'initial prompt')
        assert not CLOCK.search(prompt_row()),'no right side before rc.esh says so'
        type_text('mkdir ".config/shell"\n');eventually(lambda:screen.at_prompt(w),'mkdir')
        type_text('echo "prompt set right status duration time" out> ".config/shell/rc.esh"\n')
        type_text('echo "prompt set transient always" out>> ".config/shell/rc.esh"\n')
        eventually(lambda:screen.at_prompt(w),'rc.esh written')
        d.g.pump(1)
        # a new tab: a new shell, which reads rc.esh
        d.key('meta_l-t')
        eventually(lambda:screen.state(w)['tabs']==2 and screen.at_prompt(w),'a second tab')
        eventually(lambda:CLOCK.search(prompt_row()) is not None,'the time on the right of the prompt')
        row=prompt_row();print('first prompt:',repr(row),flush=True)
        cols=screen.state(w)['cols']
        assert len(row)==cols-1,('the right side ends one column in from the edge',len(row),cols)
        type_text('sleep 4000\n')                  # milliseconds
        eventually(lambda:'4s' in prompt_row() and '\u2714' in prompt_row(),'a slow command: ✔ and its duration',30)
        assert '$ sleep 4000' in lines(),('the accepted line keeps only the prompt character',lines()[-6:])
        assert not any(l.startswith('yves@') and 'sleep 4000' in l for l in lines()),'no full prompt left in front of it'
        type_text('assert 1 == 2\n')
        eventually(lambda:'\u2718 1' in prompt_row(),'a failing one: ✘ and its status')
        d.snap('prompt-right')
        print('after a failure:',repr(prompt_row()),flush=True)
        type_text('prompt\n')
        eventually(lambda:any('transient' in l and 'always' in l for l in lines()),'prompt: the options as a table')
        type_text('prompt set transient off\n')
        eventually(lambda:screen.at_prompt(w),'transient off')
        type_text('echo kept\n')
        eventually(lambda:'kept' in lines(),'a command after it')
        assert any(l.startswith('yves@emblink:~$ echo kept') for l in lines()),('the full prompt stays again',lines()[-5:])
        d.snap('prompt-transient')
        print('PASS: rc.esh read by a new shell, the right prompt (status, duration, time), the transient prompt, '
              'the prompt builtin',flush=True)
    except Exception:
        try:
            d.snap('last');print('\n'.join(lines()[-14:]),flush=True)
        except Exception as e:print('no last capture:',e)
        raise
    finally:
        d.g.close()

if __name__=='__main__':main()
