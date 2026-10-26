#!/usr/bin/env python3
"""The screen model, live: ttyprobe --xterm inside Terminal, driven over QMP.

What a program sees of the terminal it runs in -- TERM, the attributes and
colours it can draw, the answers to the questions programs ask (DA1, DA2,
XTVERSION, DECRQM, OSC 10, XTGETTCAP, CSI 18 t), mouse and focus reports --
and what a resize does to text already written: the long line and the
scrollback re-wrap at the new width.
"""
from pathlib import Path
import math,os,shutil,tempfile,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_terminal import TerminalState
from lock_test import keyname

REFLOW='Reflow: '+''.join(f'<{i:02d}-456789>' for i in range(20))+'<END>'
UNDERLINE,BOLD=0x08,0x01

def main():
    out=Path(tempfile.mkdtemp(prefix='terminal-xterm-',dir='/workspace/.setup' if os.path.isdir('/workspace/.setup') else None));print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800));d.sock=d.out/'gdb.sock';d.symbols={}
    def eventually(check,message,seconds=40):
        end=time.monotonic()+seconds
        while not check():
            if time.monotonic()>end:raise AssertionError(message)
            d.g.pump(.4)
    def type_text(value):
        extra={'/':'slash','"':'shift-apostrophe'}
        for c in value:d.key(extra[c] if c in extra else keyname(c))
    def lines(w):return screen.text(w).split('\n')
    def has(w,s):return any(s in line for line in lines(w))
    def hmp(command):d.g.qmp.cmd('human-monitor-command',**{'command-line':command});d.g.pump(.6)
    def focus_id():return d.debug("import gdb,json\nprint('RESULT '+json.dumps(int(gdb.parse_and_eval(\"'kernel/gfx/compositor.c'::g_top_id\"))))")
    def toggle_fill():
        current=d.window('Terminal');d.click(current['x']+84,current['y']+18)
    def reflowed(w):
        """The long line, read back off the screen, and how many rows it takes."""
        rows=lines(w)
        for i,line in enumerate(rows):
            if line.startswith('Reflow: '):
                joined,used='',0
                for row in rows[i:]:
                    joined+=row;used+=1
                    if '<END>' in row:break
                return joined,used
        return None,0
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone')
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        screen=TerminalState(d);w=d.launch('Terminal','Terminal');eventually(lambda:screen.at_prompt(w),'initial prompt')
        narrow=screen.state(w)['cols']
        toggle_fill();eventually(lambda:screen.state(w)['cols']>100 and screen.at_prompt(w),'fill the work area')
        type_text('/data/apps/ttyprobe/ttyprobe.elf "--xterm"\n')
        eventually(lambda:has(w,'Terminal replies verified:'),'terminal replies')
        rows=lines(w)
        assert any('Terminal replies verified: 7/7' in line for line in rows),'\n'.join(rows)
        assert any('TERM=xterm-256color COLORTERM=truecolor' in line for line in rows),'TERM and COLORTERM'
        eventually(lambda:has(w,'Mouse and focus:'),'event loop')
        # what the card asked for is what the cells hold
        state=screen.state(w);text=state['text']
        row=lambda prefix:next(i for i,line in enumerate(text) if line.startswith(prefix))
        cells=state['cells']
        assert cells[row('Attributes:')][12]['flags']&BOLD,'bold'
        assert all(cells[row('Underlines:')][x]['flags']&UNDERLINE for x in (12,19,26,32,39)),'underline styles'
        assert not cells[row('Underlines:')][18]['flags']&UNDERLINE,'the space between is plain'
        tc=cells[row('Truecolour:')]
        assert tc[12]['bg']!=tc[58]['bg'] and 0xffffffff not in (tc[12]['bg'],tc[58]['bg']),'truecolour gradient'
        # the profile's sixteen: Clear Dark's black and bright red (user/terminal/profile.c)
        assert cells[row('16 colours:')][12]['bg']==0x35424c and cells[row('16 colours:')][12+3*9]['bg']==0xdf6c5a,'16-colour palette'
        wide=cells[row('Wide, marks:')];assert wide[13]['width']==2 and wide[14]['width']==0,'wide character'
        d.snap('xterm-card')
        # mouse: left and right clicks, the wheel both ways; focus out and back
        win=d.window('Terminal');cx,cy=win['x']+win['cw']//2,win['y']+win['ch']//2
        d.click(cx,cy)
        eventually(lambda:has(w,'Mouse: left pressed at column') and has(w,'Mouse: left released at column'),'left click reported')
        d.hover(cx+40,cy+20);hmp('mouse_button 2');hmp('mouse_button 0')
        eventually(lambda:has(w,'Mouse: right pressed') and has(w,'Mouse: right released'),'right click reported')
        hmp('mouse_move 0 0 1');hmp('mouse_move 0 0 -1')
        eventually(lambda:has(w,'Mouse: wheel up') and has(w,'Mouse: wheel down'),'wheel reported both ways')
        reported=[line for line in lines(w) if line.startswith('Mouse: left pressed')][0].split()
        col,rw=int(reported[5].rstrip(',')),int(reported[7])
        cols,rws=screen.state(w)['cols'],screen.state(w)['rows']
        assert 1<=col<=cols and 1<=rw<=rws and abs(col-cols/2)<cols/4,f'click at column {col} row {rw} of {cols}x{rws}'
        editor=d.launch('Editor','Editor')
        eventually(lambda:has(w,'Focus: lost'),'focus lost')
        d.key('meta_l-tab');eventually(lambda:focus_id()==w['id'],'focus back to Terminal')
        eventually(lambda:has(w,'Focus: regained'),'focus regained')
        d.snap('xterm-mouse-focus')
        d.key('q');eventually(lambda:has(w,'xterm check done.') and screen.at_prompt(w),'back at the prompt')
        # reflow: the long line at the wide width, then at the narrow one
        joined,used=reflowed(w);wide_cols=screen.state(w)['cols']
        assert joined==REFLOW and used==math.ceil(len(REFLOW)/wide_cols),(joined,used,wide_cols)
        toggle_fill();eventually(lambda:screen.state(w)['cols']==narrow and screen.at_prompt(w),'restore the window')
        d.g.pump(2)
        state=screen.state(w)
        joined,used=reflowed(w)
        if joined is not None:     # still on the screen: re-wrapped there
            assert joined==REFLOW and used==math.ceil(len(REFLOW)/narrow),(joined,used,narrow)
        assert state['history_count']>0,'the card went into the scrollback'
        d.snap('xterm-reflow-narrow')
        # the scrollback re-wrapped too: page back to the card's top
        for _ in range(8):
            d.key('shift-pgup')
            if screen.probe(w,text=False)['view']>=state['history_count']:break
        eventually(lambda:screen.probe(w,text=False)['view']>0,'paging back')
        d.snap('xterm-scrollback')
        for _ in range(8):d.key('shift-pgdn')
        eventually(lambda:screen.probe(w,text=False)['view']==0,'back to the live screen')
        assert b'PANIC' not in d.g.seen
        print('PASS: TERM=xterm-256color, the card drawn as asked (attributes, underline styles, 16/256/truecolour, wide cells), '
              '7/7 terminal replies, mouse (left, right, wheel) and focus reports, reflow wide and narrow, scrollback re-wrapped',flush=True)
    finally:
        try:d.snap('last')
        finally:d.g.close()

if __name__=='__main__':main()
