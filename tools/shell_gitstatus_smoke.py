#!/usr/bin/env python3
"""The prompt's git status, live: /system/bin/gitstatus.elf answering the
shell about ~/Projects/hello, the sample repository on the image.

Driven over QMP like a person would. rc.esh carries the Mac's configuration
(shell_p10k_smoke.SETTINGS); a new tab's shell starts the daemon on its first
prompt in the repository. The prompt then shows what the daemon reads from the
repository's own files -- the branch one commit ahead of origin/main (⇡1),
clean; an edit to a tracked file is !1, a new file ?1, a file under an
ignored directory nothing -- in the colours of an answer (loading draws all
of it grey).
"""
from pathlib import Path
import os,re,shutil,tempfile,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_terminal import TerminalState
from lock_test import keyname
from shell_p10k_smoke import SETTINGS


def main():
    out=Path(tempfile.mkdtemp(prefix='shell-gitstatus-',dir='/workspace/.setup' if os.path.isdir('/workspace/.setup') else None));print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800));d.sock=d.out/'gdb.sock';d.symbols={}
    def eventually(check,message,seconds=60):
        end=time.monotonic()+seconds
        while not check():
            if time.monotonic()>end:raise AssertionError(message)
            d.g.pump(.4)
    def type_text(value):
        for c in value:d.key(keyname(c))
    def run(line,what):
        serial=screen.probe(w,text=False)['prompt_serial']
        type_text(line+'\n')
        eventually(lambda:screen.at_prompt(w) and screen.probe(w,text=False)['prompt_serial']!=serial,what)
    def lines():return [l.rstrip() for l in screen.text(w).split('\n')]
    def first_row():
        st=screen.state(w);y=st['cursor'][1]
        return st['text'][y-1].rstrip() if y else ''
    def fg_of(word):
        """The colour of `word`'s first character on the prompt's first row."""
        st=screen.state(w);y=st['cursor'][1]-1;row=st['text'][y]
        x=row.find(word)
        return st['cells'][y][x]['fg'] if x>=0 else None
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone')
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        screen=TerminalState(d);w=d.launch('Terminal','Terminal');eventually(lambda:screen.at_prompt(w),'initial prompt')
        run('mkdir ".config/shell"','mkdir')
        for i,setting in enumerate(SETTINGS):
            quoted=setting.replace('\\','\\\\').replace('"','\\"')
            run(f'echo "{quoted}" out{">" if i==0 else ">>"} ".config/shell/rc.esh"','rc.esh: '+setting)
        d.key('meta_l-t')
        eventually(lambda:screen.state(w)['tabs']==2 and screen.at_prompt(w),'a second tab')
        run('cd "Projects/hello"','cd into the sample repository')
        # the daemon starts; its answer comes in time, or redraws the prompt when it does
        eventually(lambda:re.search(r'~/Projects/hello main ⇡\s*1?\b',first_row()) is not None,
                   'main ⇡1: one commit ahead of origin/main, from the daemon',90)
        print('clean:',repr(first_row()),flush=True)
        run('echo "more" out>> "README.md"','edit a tracked file')
        eventually(lambda:'!1' in first_row(),'!1: a changed file',60)
        run('echo "notes" out> "notes.txt"','a new file')
        eventually(lambda:'?1' in first_row(),'?1: an untracked file',60)
        run('mkdir "build"','an ignored directory')
        run('echo "x" out> "build/hello.o"','a file in it')
        d.g.pump(3)
        row=first_row()
        assert '?1' in row and '?2' not in row,('build/ is ignored (.gitignore)',row)
        assert fg_of('main')!=fg_of('!1'),('an answer, not loading: loading draws it all grey',fg_of('main'),fg_of('!1'))
        print('dirty:',repr(row),flush=True)
        d.snap('gitstatus-hello')
        print('PASS: gitstatus.elf answered the prompt -- ⇡1 from the packed history, !1, ?1, the ignore rules',flush=True)
    except Exception:
        try:
            d.snap('last');print('\n'.join(lines()[-14:]),flush=True)
        except Exception as e:print('no last capture:',e)
        raise
    finally:
        d.g.close()

if __name__=='__main__':main()
