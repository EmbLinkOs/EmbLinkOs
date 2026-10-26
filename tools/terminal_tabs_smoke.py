#!/usr/bin/env python3
"""Terminal's window, live: tabs, the mouse selection, find, links, the menu, zoom.

Driven over QMP like a person would: Super+T opens a tab whose shell keeps its
own history of output, Ctrl+Tab goes between them; a double click selects a
word, a triple click a line, a drag what it covers, and Copy/Paste carry it;
Super+F finds in the scrollback; Super held over a URL marks it and a click
opens it; the right button opens the menu; Super+= and Super+0 zoom; Super+W
closes the tab, its shell reaped and its PTY freed.
"""
from pathlib import Path
import os,shutil,tempfile,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_terminal import TerminalState
from lock_test import keyname

def main():
    out=Path(tempfile.mkdtemp(prefix='terminal-tabs-',dir='/workspace/.setup' if os.path.isdir('/workspace/.setup') else None));print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800));d.sock=d.out/'gdb.sock';d.symbols={}
    def eventually(check,message,seconds=60):
        end=time.monotonic()+seconds
        while not check():
            if time.monotonic()>end:raise AssertionError(message)
            d.g.pump(.4)
    def type_text(value):
        extra={'/':'slash','"':'shift-apostrophe',':':'shift-semicolon'}
        for c in value:d.key(extra[c] if c in extra else keyname(c))
    def hmp(command,pump=.3):
        d.g.qmp.cmd('human-monitor-command',**{'command-line':command})
        if pump:d.g.pump(pump)
    def state():return screen.state(w)
    def text():return screen.text(w).split('\n')
    def row_of(line):return max(i for i,l in enumerate(state()['text']) if l==line)   # the line itself, the last one
    def cell_xy(col,row):
        st=state();win=d.window('Terminal');(cw,ch),(ox,oy)=st['cell'],st['origin']
        return win['x']+ox+col*cw+cw//2,win['y']+oy+row*ch+ch//2
    def clicks(n):                 # a double or triple click, about as fast as a hand makes one
        for _ in range(n):hmp('mouse_button 1',.05);hmp('mouse_button 0',.1)
        d.g.pump(1.2)
    def ptys():return d.debug("import gdb,json\nt=gdb.parse_and_eval(\"'kernel/tty/pty.c'::terminals\")\nprint('RESULT '+json.dumps(sum(1 for i in range(t.type.range()[1]+1) if int(t[i])!=0)))")
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone')
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        screen=TerminalState(d);w=d.launch('Terminal','Terminal');eventually(lambda:screen.at_prompt(w),'initial prompt')
        win=d.window('Terminal');d.click(win['x']+84,win['y']+18)
        eventually(lambda:state()['cols']>100 and screen.at_prompt(w),'fill the work area')
        assert state()['tabs']==1
        base_ptys=ptys()
        type_text('echo "first-tab-42 file:///data/apps"\n')
        eventually(lambda:any(line=='first-tab-42 file:///data/apps' for line in text()),'output in the first tab')
        # tabs: a second one, its own shell; back again, the first kept its screen
        d.key('meta_l-t')
        eventually(lambda:state()['tabs']==2 and state()['tab']==1 and screen.at_prompt(w),'a second tab')
        st=state();assert st['tab_shells'][0]!=st['tab_shells'][1] and all(s>=0 for s in st['tab_shells']),st['tab_shells']   # spawn handles, not pids
        assert not any('first-tab-42' in line for line in st['text']),'the new tab has its own screen'
        type_text('echo second-tab\n')
        eventually(lambda:any(line=='second-tab' for line in text()),'output in the second tab')
        assert ptys()==base_ptys+1
        d.snap('tabs-two')
        d.key('ctrl-tab')
        eventually(lambda:state()['tab']==0 and any(line=='first-tab-42 file:///data/apps' for line in text()),'back to the first tab')
        # selection: a word, a line, a drag
        r=row_of('first-tab-42 file:///data/apps');x,y=cell_xy(3,r)
        d.hover(x,y);clicks(2)
        eventually(lambda:state()['selection']=='first-tab-42','a double click selects the word')
        clicks(3)
        eventually(lambda:state()['selection']=='first-tab-42 file:///data/apps','a triple click selects the line')
        x0,y0=cell_xy(0,r);x1,y1=cell_xy(9,r)
        d.hover(x0-3,y0);hmp('mouse_button 1',.5);d.hover(x1-3,y1);hmp('mouse_button 0',1)
        eventually(lambda:state()['selection']=='first-tab','a drag selects what it covers')
        d.snap('tabs-selection')
        d.key('meta_l-c');d.g.pump(.5)
        d.key('meta_l-v')
        eventually(lambda:text()[state()['cursor'][1]].endswith('first-tab'),'Copy then Paste puts it at the prompt')
        d.key('ctrl-c');eventually(lambda:screen.at_prompt(w),'prompt after Ctrl+C')
        # find in the scrollback: push the line up out of view first
        for _ in range(3):
            type_text('/data/apps/ttyprobe/ttyprobe.elf "--glyphs"\n')
            eventually(lambda:screen.at_prompt(w) and any(line.startswith('Lines: single') for line in text()),'filler output')
        assert not any('first-tab-42' in line for line in text()),'the line went up into the scrollback'
        d.key('meta_l-f')
        eventually(lambda:state()['find_open'],'Super+F opens the find bar')
        type_text('first-tab')
        eventually(lambda:state()['find_found'] and state()['find']=='first-tab' and state()['view']>0,
                   'typing finds the text, and the view goes back to it')
        d.snap('tabs-find')
        d.key('esc');eventually(lambda:not state()['find_open'],'Escape closes the find bar')
        d.key('shift-end');eventually(lambda:state()['view']==0,'Shift+End: back to the live screen')
        type_text('echo "first-tab-42 file:///data/apps"\n')
        eventually(lambda:any(line=='first-tab-42 file:///data/apps' for line in text()) and screen.at_prompt(w),'the URL on the screen again')
        # a link: Super held marks it, a Super-click opens it (a folder: Files)
        r=row_of('first-tab-42 file:///data/apps');lx,ly=cell_xy(20,r)
        d.hover(lx,ly);hmp('sendkey meta_l 7000',.3);d.hover(lx+1,ly)    # a move while Super is down: a frame
        eventually(lambda:state()['link']=='file:///data/apps','Super over a URL finds the link',seconds=8)
        d.snap('tabs-link')
        clicks(1)
        eventually(lambda:any(x['title']=='Files' and x['visible'] for x in d.state()['windows']),'Super-click opens the folder in Files')
        d.g.pump(4)
        d.key('meta_l-tab');eventually(lambda:d.debug("import gdb,json\nprint('RESULT '+json.dumps(int(gdb.parse_and_eval(\"'kernel/gfx/compositor.c'::g_top_id\"))))")==w['id'],'Terminal in front again')
        # the menu (Super long up again: held, the compositor takes the mouse)
        d.g.pump(4)
        mx,my=cell_xy(30,4);d.hover(mx,my);hmp('mouse_button 2',.3);hmp('mouse_button 0',1)
        d.snap('tabs-menu')
        d.key('esc');d.g.pump(1)
        # zoom
        px,cols=state()['font_px'],state()['cols']
        d.key('meta_l-equal');d.key('meta_l-equal')
        eventually(lambda:state()['font_px']==px+2 and state()['cols']<cols,'Super+= zooms in')
        d.snap('tabs-zoom')
        d.key('meta_l-0');eventually(lambda:state()['font_px']==px and state()['cols']==cols,'Super+0 back')
        # closing a tab: its shell goes and its PTY is freed
        d.key('ctrl-tab');eventually(lambda:state()['tab']==1,'to the second tab')
        gone=state()['tab_shells'][1]
        d.key('meta_l-w')
        eventually(lambda:state()['tabs']==1 and state()['tab']==0,'Super+W closes the tab, not the window')
        eventually(lambda:ptys()==base_ptys,'its PTY is freed')
        assert gone not in state()['tab_shells']
        assert any(line=='first-tab-42 file:///data/apps' for line in text()),'the remaining tab kept its screen'
        assert b'PANIC' not in d.g.seen
        print('PASS: tabs with their own shells (Super+T, Ctrl+Tab, Super+W reaping the shell and freeing its PTY), '
              'word/line/drag selection, Copy and Paste, find, link hover and open, the menu, zoom',flush=True)
    finally:
        try:d.snap('last')
        finally:d.g.close()

if __name__=='__main__':main()
