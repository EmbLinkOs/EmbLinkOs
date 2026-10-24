#!/usr/bin/env python3
"""Run shell scripts and interactive workflows through the real desktop.
All actions use serial commands or QMP input; GDB only observes state.
Keep x86 kernel/application binaries unchanged while this guest is running.
"""
from pathlib import Path
import re,shutil,tempfile,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_terminal import TerminalState
from lock_test import keyname

def main():
    out=Path(tempfile.mkdtemp(prefix='posix-shell-',dir='/workspace/.setup'));print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800));d.sock=d.out/'gdb.sock';d.symbols={}
    def eventually(check,message,timeout=180):
        deadline=time.monotonic()+timeout
        while not check():
            if time.monotonic()>deadline:raise AssertionError(message)
            d.g.pump(.4)
    mapping={'/':'slash','"':'shift-apostrophe',"'":'apostrophe','$':'shift-4',';':'semicolon',':':'shift-semicolon','=':'equal','|':'shift-backslash','(':'shift-9',')':'shift-0','*':'shift-8','&':'shift-7','>':'shift-dot','<':'shift-comma','[':'bracket_left',']':'bracket_right','{':'shift-bracket_left','}':'shift-bracket_right','!':'shift-1','%':'shift-5','\\':'backslash'}
    def type_text(value):
        for c in value:d.key(mapping[c] if c in mapping else keyname(c))
    def shells():
        return d.debug('''import gdb,json
ps=gdb.parse_and_eval("'kernel/process/process.c'::process_table")
print('RESULT '+json.dumps([int(ps[i]['pid']) for i in range(ps.type.range()[1]+1) if int(ps[i]['pid']) and int(ps[i]['live_thread_count']) and ps[i]['exec_name'].string()=='sh.elf']))''')
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone')
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        for command in ['test posix-shell','test posix-shell','test spawn-compat','test process-compat','test handle reap','test sched wait','test terminal','test tty','test posix','test channel','test capgate']:
            result=d.g.command(command,('[cmd] '+command+':').encode(),timeout=240)
            line=re.search(r'\[cmd\] '+re.escape(command)+r':[^\r\n]*',result)
            assert line and line.group().endswith('OK') and 'FAIL' not in result,result
            print(line.group(),flush=True)
        screen=TerminalState(d);w=d.launch('Terminal','Terminal')
        prompt=lambda:screen.integer(w,'g_at_prompt',1)==1
        eventually(prompt,'native prompt')
        type_text('/data/apps/shprobe/shprobe.elf\n')
        eventually(lambda:'shprobe:' in screen.text(w) and prompt(),'graphical shell script checks')
        text=screen.text(w);match=re.search(r'shprobe: (\d+)/(\d+) checks passed',text)
        assert match and match[1]==match[2],text
        d.snap('posix-shell-script-checks');print(match.group(),flush=True)
        type_text('sh\n');eventually(lambda:bool(shells()) and prompt(),'interactive sh start')
        interactive=shells()[0]
        type_text('for n in 1 2 3; do echo sample-$n; done\n')
        eventually(lambda:'sample-3' in screen.text(w).splitlines() and prompt(),'interactive for loop')
        assert screen.text(w).count('for n in 1 2 3; do echo sample-$n; done')==1,'command was echoed twice'
        type_text('echo calculation=$((6*7))\n')
        eventually(lambda:'calculation=42' in screen.text(w).splitlines() and prompt(),'interactive arithmetic')
        type_text('echo "$(echo sensor-ready)"\n')
        eventually(lambda:'sensor-ready' in screen.text(w).splitlines() and prompt(),'interactive command substitution')
        type_text('/data/apps/shprobe/shprobe.elf --emit | /data/apps/shprobe/shprobe.elf --upper | /data/apps/shprobe/shprobe.elf --count\n')
        eventually(lambda:'66049' in screen.text(w).splitlines() and prompt(),'interactive three-stage native pipeline')
        d.snap('posix-shell-interactive');print('Interactive loops, arithmetic, substitution and three-stage pipeline: OK',flush=True)
        type_text('sleep 30 &\n')
        eventually(lambda:len(shells())>=2 and prompt(),'background worker')
        background=set(shells())-{interactive}
        type_text('/data/apps/shprobe/shprobe.elf --hold\n')
        eventually(lambda:'Foreground hold: ready for Ctrl+C' in screen.text(w),'noncooperative foreground program')
        d.key('ctrl-c')
        eventually(prompt,'native Ctrl+C returns sh prompt',timeout=15)
        assert interactive in shells(),'interrupt terminated interactive shell'
        assert background.intersection(shells()),'foreground cancellation terminated a background worker'
        type_text('wait; echo background-completed\n')
        eventually(lambda:'background-completed' in screen.text(w).splitlines() and prompt(),'background wait after foreground interruption',timeout=45)
        type_text('echo resumed-after-interrupt\n')
        eventually(lambda:'resumed-after-interrupt' in screen.text(w).splitlines() and prompt(),'shell after interruption')
        type_text('exit\n');eventually(lambda:interactive not in shells() and prompt(),'return to structured shell')
        type_text('echo 73921\n');eventually(lambda:'73921' in screen.text(w).splitlines() and prompt(),'structured shell still usable')
        assert b'PANIC' not in d.g.seen
        print('PASS: native shell scripts, isolated workers, POSIX pipeline, interactive prompt, native interruption and structured-shell return',flush=True)
    finally:
        try:d.snap('last')
        finally:d.g.close()

if __name__=='__main__':main()
