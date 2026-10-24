#!/usr/bin/env python3
"""Native PTY and Terminal acceptance; actions use serial commands or QMP keys.

GDB only reads app buffers and window ownership. Keep build binaries unchanged
while the guest is running. Captures preserve the actual rendered pixels.
"""
from pathlib import Path
import re,shutil,tempfile,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from lock_test import keyname
from native_terminal import TerminalState

def main():
    out=Path(tempfile.mkdtemp(prefix='terminal-native-',dir='/workspace/.setup')); print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800));d.sock=d.out/'gdb.sock';d.symbols={}
    def eventually(check,message,timeout=30):
        end=time.monotonic()+timeout
        while not check():
            if time.monotonic()>end:raise AssertionError(message)
            d.g.pump(.4)
    def integer(w,name,n=4):return int.from_bytes(d.read('term',w['pid'],name,n),'little',signed=n>1)
    def prompt(w):return integer(w,'g_at_prompt',1)==1
    def transcript(w):return screen.text(w)
    def text(w,needle):return needle in transcript(w)
    def type_text(value):
        extra={'/':'slash','"':'shift-apostrophe','{':'shift-bracket_left','}':'shift-bracket_right'}
        for c in value:d.key(extra[c] if c in extra else keyname(c))
    def command(w,value,needle):
        eventually(lambda:prompt(w),'shell did not regain its prompt')
        type_text(value+'\n');eventually(lambda:needle in transcript(w).splitlines(),'missing command output '+needle)
        eventually(lambda:prompt(w),'command did not finish')
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone')
        for cmd in ['test terminal','test tty','test posix','test channel']:
            result=d.g.command(cmd,('[cmd] '+cmd+':').encode(),timeout=120)
            assert re.search(r'\[cmd\] '+re.escape(cmd)+r':[^\r\n]*OK',result),result
            print(result[result.rfind('[cmd]'):].strip(),flush=True)
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        screen=TerminalState(d)
        w=d.launch('Terminal','Terminal');eventually(lambda:prompt(w),'initial PTY prompt')
        assert b'\r' not in d.read('term',w['pid'],'g_cwd',160).split(b'\0',1)[0]
        type_text('echo 123');d.key('backspace');type_text('4\n')
        eventually(lambda:prompt(w) and text(w,'echo 124'),'native prompt editing')
        d.key('up');assert d.read('term',w['pid'],'g_in',256).split(b'\0',1)[0]==b'echo 124'
        d.key('ctrl-u');command(w,'echo 73421','73421');d.snap('terminal-prompt')
        type_text('/data/apps/ttyprobe/ttyprobe.elf\n')
        eventually(lambda:text(w,'Canonical input: type a line'),'canonical program start')
        assert not prompt(w)
        cols,rows=integer(w,'g_cols'),integer(w,'g_rows')
        assert text(w,f'{cols} columns, {rows} rows'),'program did not see measured terminal dimensions'
        type_text('boardx');d.key('backspace');type_text('42\n')
        eventually(lambda:text(w,'Raw input: press one key'),'canonical input was not delivered')
        assert text(w,'Received: board42'),'canonical erase did not edit the line'
        d.snap('terminal-raw-input')
        type_text('z')
        eventually(lambda:text(w,'Received immediately: 0x7a'),'raw key required Enter')
        eventually(lambda:text(w,'Canonical input restored.'),'program did not restore input settings')
        d.snap('terminal-direct-input')
        d.key('ctrl-d');eventually(lambda:prompt(w),'EOF did not return to shell')
        assert text(w,'Terminal probe complete.')
        type_text('/data/apps/ttyprobe/ttyprobe.elf "--keys"\n')
        eventually(lambda:text(w,'Raw key check:'),'raw key witness did not start')
        for key in ['ctrl-b','ctrl-s','ctrl-h','ret','left']:d.key(key)
        eventually(lambda:text(w,'Raw key sequence verified.'),'control keys were mistaken for navigation')
        eventually(lambda:prompt(w),'raw key witness did not finish')
        command(w,'echo 98317','98317')
        type_text('/data/apps/ttyprobe/ttyprobe.elf\n')
        # A second invocation has no raw-mode transition until a line is read.
        eventually(lambda:not prompt(w),'second command did not start');d.g.pump(2)
        d.key('ctrl-c');eventually(lambda:prompt(w),'Ctrl+C did not stop the foreground child')
        command(w,'echo 62514','62514')
        # Shell builtins catch their own interrupt, independently of children.
        type_text('while true { }\n');eventually(lambda:not prompt(w),'loop did not start');d.key('ctrl-c')
        eventually(lambda:prompt(w),'Ctrl+C did not return from a shell loop')
        command(w,'echo 47218','47218');d.snap('terminal-interrupt')
        # A separate emulator must not steal input or console interruption.
        old={r['pid'] for r in d.state()['windows'] if r['title']=='Terminal'}
        d.g.serial.sendall(b'run /data/apps/term/term.elf\n')
        def second():return [r for r in d.state()['windows'] if r['title']=='Terminal' and r['pid'] not in old]
        eventually(lambda:bool(second()),'second terminal did not open');other=second()[0]
        eventually(lambda:prompt(other),'second terminal prompt');d.g.pump(2)
        d.click(other['x']+other['cw']//2,other['y']+18)
        command(other,'echo 85296','85296');assert not text(w,'85296'),'input crossed terminal sessions'
        # Closing an active foreground read hangs up its PTY and reaps the shell.
        type_text('/data/apps/ttyprobe/ttyprobe.elf\n');eventually(lambda:not prompt(other),'hangup command did not start');d.g.pump(1)
        shell_handle=integer(other,'g_shell')
        shell_pid=d.debug(f"import gdb,json\nps=gdb.parse_and_eval(\"'kernel/process/process.c'::process_table\")\np=next(ps[i] for i in range(ps.type.range()[1]+1) if int(ps[i]['pid'])=={other['pid']})\nprint('RESULT '+json.dumps(int(p['handles'][{shell_handle}]['pid'])))")
        d.key('meta_l-w')
        eventually(lambda:not any(r['pid']==other['pid'] for r in d.state()['windows']),'terminal close stalled')
        def gone():
            return d.debug(f"import gdb,json\nps=gdb.parse_and_eval(\"'kernel/process/process.c'::process_table\")\nprint('RESULT '+json.dumps(not any(int(ps[i]['pid']) in [{other['pid']},{shell_pid}] and int(ps[i]['live_thread_count']) for i in range(ps.type.range()[1]+1))))")
        eventually(gone,'closed Terminal or its shell remained alive')
        # Complete first shell with EOF, rather than terminating its window.
        now=next(r for r in d.state()['windows'] if r['pid']==w['pid'])
        d.click(now['x']+now['cw']//2,now['y']+18)
        command(w,'echo 31957','31957')
        print('EOF state: '+str({k:integer(w,k,1 if k in ['g_at_prompt','g_dead'] else 4) for k in ['g_at_prompt','g_dead','g_in_n','g_master']}),flush=True)
        d.key('ctrl-d')
        eventually(lambda:integer(w,'g_dead',1)==1,'Ctrl+D at an empty prompt did not close the shell')
        assert b'PANIC' not in d.g.seen
        print('PASS: native PTY checks, console/POSIX/channel regressions, prompt editing/history, measured dimensions, cooked/raw input, EOF, scoped Ctrl+C, independent terminals and active-close hangup',flush=True)
    except Exception:
        if 'w' in locals():
            print('Failure state: '+str({k:integer(w,k,1 if k in ['g_at_prompt','g_dead'] else 4) for k in ['g_at_prompt','g_dead','g_in_n','g_master']}),flush=True)
        raise
    finally:
        try:d.snap('last')
        finally:d.g.close()

if __name__=='__main__':main()
