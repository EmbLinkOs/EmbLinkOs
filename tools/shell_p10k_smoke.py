#!/usr/bin/env python3
"""Powerlevel10k in the shell, live: a person's configuration, its repository
and transient prompt, and every style p10k configure starts from.

Driven over QMP like a person would. The first tab makes a directory holding a
repository (its .git/HEAD) and writes ~/.config/shell/rc.esh: the lean style
and the settings the wizard put on top of it in the configuration recorded on
a Mac (tests/prompt/configs/user.p10k.zsh -- the host's golden test holds the
engine to that recording cell for cell; here the same prompt is drawn by the
shell on the OS). Super+T opens a tab whose shell reads it: the frame, the
12-hour clock, the branch, a slow command's duration, a failure's red prompt
character, Ctrl+C's INT, a background job's ≡, and accepted lines kept as
❯ alone. Then `prompt style` draws classic, rainbow, pure and lean, and
`prompt style plain` puts the plain prompt back.
"""
from pathlib import Path
import os,re,shutil,tempfile,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_terminal import TerminalState
from lock_test import keyname

# lean, and what the wizard changed: the configuration in tests/prompt/configs
SETTINGS=[
    "prompt style lean",
    "prompt set mode powerline",
    "prompt set right status duration background_jobs time",
    r"prompt set multiline.first.prompt.prefix '%238F\u256D\u2500'",
    r"prompt set multiline.newline.prompt.prefix '%238F\u251C\u2500'",
    r"prompt set multiline.last.prompt.prefix '%238F\u2570\u2500'",
    "prompt set left.prompt.first.segment.start.symbol ' '",
    "prompt set ruler.fg 238",
    "prompt set multiline.first.prompt.gap.fg 238",
    r"prompt set lock.icon '\u2205'",
    "prompt set dir.classes ''",
    "prompt set vcs.visual.identifier.expansion ''",
    "prompt set command.execution.time.visual.identifier.expansion ''",
    r"prompt set background.jobs.visual.identifier.expansion '\u2261'",
    "prompt set time.format 12h",
    "prompt set time.visual.identifier.expansion ''",
    "prompt set transient always",
]
CLOCK=re.compile(r'\d\d:\d\d:\d\d [AP]M$')
PROMPT_CHAR='\u276f'

def main():
    out=Path(tempfile.mkdtemp(prefix='shell-p10k-',dir='/workspace/.setup' if os.path.isdir('/workspace/.setup') else None));print(out,flush=True)
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
    def rows():
        st=screen.state(w);y=st['cursor'][1];t=st['text']
        return t[y-1].rstrip() if y else '',t[y].rstrip(),st
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone')
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        screen=TerminalState(d);w=d.launch('Terminal','Terminal');eventually(lambda:screen.at_prompt(w),'initial prompt')
        # a repository to stand in, and rc.esh
        for line in ('mkdir "src"','mkdir "src/emblink"','mkdir "src/emblink/.git"',
                     'echo "ref: refs/heads/main" out> "src/emblink/.git/HEAD"','mkdir ".config/shell"'):
            run(line,line)
        for i,setting in enumerate(SETTINGS):
            quoted=setting.replace('\\','\\\\').replace('"','\\"')
            run(f'echo "{quoted}" out{">" if i==0 else ">>"} ".config/shell/rc.esh"','rc.esh: '+setting)
        # a new tab: a new shell, which reads rc.esh
        d.key('meta_l-t')
        eventually(lambda:screen.state(w)['tabs']==2 and screen.at_prompt(w),'a second tab')
        eventually(lambda:rows()[1].startswith('\u2570\u2500'+PROMPT_CHAR),'the prompt in two lines: ╰─❯')
        first,second,st=rows();cols=st['cols']
        print('first prompt:',repr(first),repr(second),flush=True)
        assert first.startswith('\u256d\u2500 ~') and CLOCK.search(first),('╭─ ~ and the 12-hour clock',first)
        assert len(first)==cols-1,('the clock ends one column in from the edge',len(first),cols)
        assert st['cursor'][0]==4,('the typing starts after ╰─❯ and a space',st['cursor'])
        run('cd "src/emblink"','cd into the repository')
        first,second,st=rows()
        assert re.match(r'\u256d\u2500 ~/src/emblink main\s',first),('the directory and the branch',first)
        ok_fg=st['cells'][st['cursor'][1]][2]['fg']
        run('sleep 3500','a slow command')         # milliseconds; 3.5 s and more rounds to 4s, as p10k rounds
        assert re.search(r' [34]s \d\d:\d\d:\d\d [AP]M$',rows()[0]),('its duration before the clock',rows()[0])
        run('assert 1 == 2','a failing command')
        first,second,st=rows()
        err_fg=st['cells'][st['cursor'][1]][2]['fg']
        assert err_fg!=ok_fg,('the prompt character red after a failure',ok_fg,err_fg)
        run('sleep 600000 &','a background job')   # ten minutes: it outlasts the test
        eventually(lambda:'\u2261' in rows()[0],'≡ for the background job')
        type_text('sleep 30000\n');d.g.pump(2)
        d.key('ctrl-c')
        eventually(lambda:screen.at_prompt(w) and re.search(r'\u2718 INT \u2261 \d\d:\d\d:\d\d [AP]M$',rows()[0]),
                   'Ctrl+C stops the sleep: ✘ INT, the job, the clock')
        kept=[l for l in lines() if l.startswith(PROMPT_CHAR+' ')]
        assert any('cd "src/emblink"' in l for l in kept) and any('sleep 3500' in l for l in kept),\
            ('accepted lines keep ❯ alone',lines()[-12:])
        print('user prompt:',repr(rows()[0]),flush=True)
        d.snap('p10k-user')
        # the styles, as `prompt style` draws them (the settings above cleared first)
        run('prompt reset','prompt reset')
        for style,mark in (('classic','\ue0b0'),('rainbow','\ue0b0'),('pure','src/emblink'),('lean','src/emblink')):
            run(f'prompt style {style}',f'prompt style {style}')
            run(f'echo "{style}"',f'a line under {style}')
            first,second,st=rows()
            print(f'{style}:',repr(first),repr(second),flush=True)
            assert mark in first+second,(f'{style} drawn',first,second)
            d.snap(f'p10k-{style}')
        run('prompt style plain','prompt style plain')
        assert rows()[1].startswith('yves@emblink:~/src/emblink$'),('the plain prompt again',rows())
        print('PASS: a person\'s Powerlevel10k configuration in rc.esh (frame, 12-hour clock, branch, duration, '
              'red prompt character, INT, ≡, transient), and the classic, rainbow, pure and lean styles',flush=True)
    except Exception:
        try:
            d.snap('last');print('\n'.join(lines()[-14:]),flush=True)
        except Exception as e:print('no last capture:',e)
        raise
    finally:
        d.g.close()

if __name__=='__main__':main()
