#!/usr/bin/env python3
"""A real full-screen child, driven through Terminal with QMP input only."""
from pathlib import Path
import re,shutil,tempfile,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_terminal import TerminalState
from lock_test import keyname

def main():
    out=Path(tempfile.mkdtemp(prefix='terminal-screen-',dir='/workspace/.setup'));print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800));d.sock=d.out/'gdb.sock';d.symbols={}
    def eventually(check,message):
        end=time.monotonic()+40
        while not check():
            if time.monotonic()>end:raise AssertionError(message)
            d.g.pump(.4)
    def type_text(value):
        extra={'/':'slash','"':'shift-apostrophe'}
        for c in value:d.key(extra[c] if c in extra else keyname(c))
    def prompt(w):return screen.integer(w,'g_at_prompt',1)==1
    def text(w,s):return s in screen.text(w)
    def edit_text(w):return d.read('edit',w['pid'],'g_doc',32768).split(b'\0',1)[0].decode()
    def focus_id():return d.debug("import gdb,json\nprint('RESULT '+json.dumps(int(gdb.parse_and_eval(\"'kernel/gfx/compositor.c'::g_top_id\"))))")
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone')
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        screen=TerminalState(d);w=d.launch('Terminal','Terminal');eventually(lambda:prompt(w),'initial prompt')
        initial=screen.state(w);assert initial['cursor']==(0,1) and initial['text'][0].startswith('EmbLink shell'),initial['text']
        type_text('echo 48126\n');eventually(lambda:prompt(w) and '48126' in screen.text(w).splitlines(),'native command result')
        d.snap('screen-native-prompt')
        type_text('/data/apps/ttyprobe/ttyprobe.elf "--screen"\n')
        eventually(lambda:screen.state(w)['alt'] and text(w,'EmbLink terminal screen check'),'alternate screen')
        assert '48126' in screen.text(w,True),'entering alternate screen damaged primary output'
        state=screen.state(w);assert state['cells'][4][0]['bg']==0x385f59
        d.key('down');eventually(lambda:screen.state(w)['text'][5].startswith(' > Scrolling region'),'application cursor Down')
        for _ in range(12):d.key('ret')
        eventually(lambda:text(w,'Log 012:'),'scroll-region output')
        state=screen.state(w);assert state['text'][0]=='EmbLink terminal screen check' and state['text'][-1]=='Ready for raw keys; no Enter required.'
        assert state['cells'][5][0]['bg']==0x385f59 and 'café' in state['text'][7] and 'e\u0301' in state['text'][7]
        capture=d.snap('screen-scroll-region')
        from PIL import Image
        with Image.open(capture) as im:assert sum(pixel==(56,95,89) for pixel in im.convert('RGB').getdata())>1500,'selected-row background was not painted'
        d.key('r');eventually(lambda:text(w,'Cursor reply verified: row 4, column 10'),'DSR round trip')
        d.snap('screen-cursor-reply')
        normal=d.window('Terminal');dimensions=(normal['cw'],normal['ch'])
        d.click(normal['x']+84,normal['y']+18)
        eventually(lambda:d.window('Terminal')['cw']==1280,'fill work area')
        eventually(lambda:screen.state(w)['cols']>100 and text(w,'Live PTY dimensions: '+str(screen.state(w)['cols'])),'live resized screen')
        d.snap('screen-resized')
        current=d.window('Terminal');d.click(current['x']+84,current['y']+18)
        eventually(lambda:(d.window('Terminal')['cw'],d.window('Terminal')['ch'])==dimensions,'restore window')
        eventually(lambda:screen.state(w)['cols']<100,'restore terminal dimensions')
        editor=d.launch('Editor','Editor');d.click(editor['x']+120,editor['y']+155)
        d.type('first line\nsecond line');eventually(lambda:edit_text(editor)=='first line\nsecond line','editor clipboard source')
        d.key('meta_l-a');d.key('meta_l-c');d.key('meta_l-tab');eventually(lambda:focus_id()==w['id'],'return focus to Terminal')
        d.key('meta_l-v');eventually(lambda:text(w,'Bracketed paste verified: 22 bytes, 1 newline(s)'),'bracketed paste delivery')
        d.snap('screen-bracketed-paste')
        d.key('meta_l-c');d.key('meta_l-tab');eventually(lambda:focus_id()==editor['id'],'focus Editor for screen text')
        d.key('meta_l-a');d.key('meta_l-v');eventually(lambda:'Bracketed paste verified' in edit_text(editor),'copy full rendered screen text')
        assert '\x1b' not in edit_text(editor),'copy included escape sequences'
        assert int.from_bytes(d.read('edit',editor['pid'],'g_cur',4),'little')==len(edit_text(editor).encode()),'paste caret did not reach the end'
        d.snap('screen-editor-copy')
        d.key('meta_l-tab');eventually(lambda:focus_id()==w['id'],'focus terminal to exit screen child')
        d.key('q');eventually(lambda:prompt(w) and not screen.state(w)['alt'],'alternate-screen exit')
        state=screen.state(w);assert '48126' in '\n'.join(state['text']) and 'Alternate screen restored.' in '\n'.join(state['text'])
        assert 'EmbLink terminal screen check' not in '\n'.join(state['text']),'alternate pixels escaped into primary output'
        d.snap('screen-primary-restored')
        d.key('meta_l-v');d.g.pump(1)
        assert prompt(w) and screen.integer(w,'g_in_n')==0,'oversized paste executed or partially inserted'
        # Build a >8 KiB clipboard using real editor commands, then exercise
        # Note++'s custom hook as well as the shared TextEditor paste path.
        d.key('meta_l-tab');eventually(lambda:focus_id()==editor['id'],'focus Editor for long clipboard')
        while len(edit_text(editor).encode())<9000:
            old=edit_text(editor)
            d.key('meta_l-a');d.key('meta_l-c');d.key('right');d.key('meta_l-v')
            eventually(lambda:edit_text(editor)==old+old,'long multiline clipboard paste')
        long_text=edit_text(editor)
        d.key('meta_l-a');d.key('meta_l-c')
        note=d.launch('Note++','Note++');d.key('meta_l-v')
        eventually(lambda:int.from_bytes(d.read('notepp',note['pid'],'ED',4,16),'little')==len(long_text.encode()),'Note++ long clipboard paste')
        d.snap('screen-long-editor-paste')
        assert b'PANIC' not in d.g.seen
        print('PASS: native VT screen, application-cursor keys, confined scrolling, Unicode/colors, DSR replies, live resize, cross-app bracketed paste/copy, primary-screen restoration and long Editor/Note++ paste',flush=True)
    finally:
        try:d.snap('last')
        finally:d.g.close()

if __name__=='__main__':main()
