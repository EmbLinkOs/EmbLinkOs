#!/usr/bin/env python3
"""Terminal's renderer, live: ttyprobe --glyphs and --bench in the real Terminal.

The glyph card (a Powerlevel10k-style framed prompt with Powerline segments
and Nerd Font icons, frames of every weight, blocks, braille, the Powerline
shapes, the four faces and the DejaVu fallback) is checked through the probe
and captured; then the throughput bench says how fast the screen keeps up
and how many repaints it took (the probe's frame count).
"""
from pathlib import Path
import os,shutil,tempfile,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_terminal import TerminalState
from lock_test import keyname

def main():
    out=Path(tempfile.mkdtemp(prefix='terminal-render-',dir='/workspace/.setup' if os.path.isdir('/workspace/.setup') else None));print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800));d.sock=d.out/'gdb.sock';d.symbols={}
    def eventually(check,message,seconds=60,every=.4):
        end=time.monotonic()+seconds
        while not check():
            if time.monotonic()>end:raise AssertionError(message)
            d.g.pump(every)
    def type_text(value):
        extra={'/':'slash','"':'shift-apostrophe'}
        for c in value:d.key(extra[c] if c in extra else keyname(c))
    def lines(w):
        try:return screen.text(w).split('\n')
        except RuntimeError:return []       # still being rewritten (output streaming): not yet
    def at_prompt(w):
        try:return screen.at_prompt(w)
        except RuntimeError:return False
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone')
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        screen=TerminalState(d);w=d.launch('Terminal','Terminal');eventually(lambda:screen.at_prompt(w),'initial prompt')
        win=d.window('Terminal');d.click(win['x']+84,win['y']+18)
        eventually(lambda:screen.state(w)['cols']>100 and screen.at_prompt(w),'fill the work area')
        type_text('/data/apps/ttyprobe/ttyprobe.elf "--glyphs"\n')
        eventually(lambda:any(line.startswith('Lines: single') for line in lines(w)) and screen.at_prompt(w),'the glyph card')
        state=screen.state(w);text=state['text'];cells=state['cells']
        row=lambda prefix:next(i for i,line in enumerate(text) if line.startswith(prefix))
        prompt=row('╭─')
        runes=[c['rune'] for c in cells[prompt]]
        assert 0xe0b0 in runes and 0xe0b6 in runes and 0xe0b4 in runes and 0xf126 in runes,'Powerline segments and icons in the prompt'
        assert cells[row('╰─')][0]['rune']==0x2570,'the frame closes'
        icons=[c['rune'] for c in cells[row('Icons:')]]
        assert 0xf0001 in icons and 0xf113 in icons,'Nerd Font icons, plane 15 included'
        assert cells[row('│  │')][0]['rune']==0x2502,'box drawing'
        # the pointer off the card; a space typed and taken back restarts the
        # cursor's blink, so the capture (1.2 s on) falls in its next on phase
        d.hover(1180,690);d.key('spc');d.key('backspace')
        d.snap('render-glyphs')
        # throughput: 1 MiB of coloured lines, and the repaints it took. Every
        # probe read stops the guest, so it is asked seldom while this runs.
        frames=screen.probe(w,text=False)['frames']
        type_text('/data/apps/ttyprobe/ttyprobe.elf "--bench" 1\n')
        eventually(lambda:any(line.startswith('Bench:') for line in lines(w)) and at_prompt(w),'the bench',seconds=900,every=8)
        bench=next(line for line in lines(w) if line.startswith('Bench:'))
        painted=screen.probe(w,text=False)['frames']-frames
        print(f'{bench}; {painted} repaints',flush=True)
        assert painted>0
        d.snap('render-bench')
        assert b'PANIC' not in d.g.seen
        print('PASS: the glyph card drawn (Powerline prompt, frames, blocks, braille, icons from plane 15, faces, fallback); bench completed',flush=True)
    finally:
        try:d.snap('last')
        finally:d.g.close()

if __name__=='__main__':main()
