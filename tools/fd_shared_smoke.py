#!/usr/bin/env python3
"""Native descriptor acceptance with local TCP/UDP echo fixtures.
Input uses serial/QMP; GDB observes only. Keep x86 binaries unchanged while live.
"""
from pathlib import Path
import json,re,shutil,socket,tempfile,threading,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_terminal import TerminalState
from lock_test import keyname

def main():
    out=Path(tempfile.mkdtemp(prefix='fd-shared-',dir='/workspace/.setup'));print(out,flush=True)
    stop=threading.Event();errors=[];received=bytearray();datagrams=[]
    tcp=socket.socket();tcp.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);tcp.bind(('127.0.0.1',39381));tcp.listen();tcp.settimeout(.2)
    udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);udp.bind(('127.0.0.1',39382));udp.settimeout(.2)
    def stream():
        try:
            while not stop.is_set():
                try:c,_=tcp.accept()
                except socket.timeout:continue
                with c:
                    c.settimeout(.2)
                    pending=bytearray()
                    while not stop.is_set():
                        try:data=c.recv(4096)
                        except socket.timeout:continue
                        if not data:break
                        received.extend(data)
                        pending.extend(data)
                        while b'\n' in pending:
                            line,_,tail=pending.partition(b'\n');pending=bytearray(tail)
                            if line==b'delayed':time.sleep(1)
                            c.sendall(line+b'\n')
        except Exception as e:errors.append(str(e))
    def datagram():
        try:
            while not stop.is_set():
                try:data,peer=udp.recvfrom(2048)
                except socket.timeout:continue
                datagrams.append(data.decode());udp.sendto(data,peer)
        except Exception as e:errors.append(str(e))
    threads=[threading.Thread(target=f,daemon=True) for f in [stream,datagram]]
    for t in threads:t.start()
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800),nic='e1000e');d.sock=d.out/'gdb.sock';d.symbols={}
    def eventually(check,message,timeout=180):
        deadline=time.monotonic()+timeout
        while not check():
            if time.monotonic()>deadline:raise AssertionError(message)
            d.g.pump(.4)
    def command(value):
        result=d.g.command(value,('[cmd] '+value+':').encode(),timeout=240)
        line=re.search(r'\[cmd\] '+re.escape(value)+r':[^\r\n]*',result)
        assert line and re.search(r'\bOK\b',line.group()) and 'FAIL' not in result,result
        print(line.group(),flush=True)
    def type_text(value):
        for c in value:d.key('slash' if c=='/' else keyname(c))
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone')
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        for value in ['test fd-sharing','test fd-sharing','test dhcp','test fd-network','test spawn-compat','test process-compat','test terminal','test handle reap','test sched wait','test tty','test posix','test channel','test capgate','test posix-shell']:
            command(value)
        screen=TerminalState(d);w=d.launch('Terminal','Terminal');prompt=lambda:screen.integer(w,'g_at_prompt',1)==1
        eventually(prompt,'native prompt')
        type_text('/data/apps/fdprobe/fdprobe.elf\n')
        eventually(lambda:'fdprobe:' in screen.text(w) and prompt(),'graphical descriptor checks')
        text=screen.text(w);match=re.search(r'fdprobe: (\d+)/(\d+) checks passed',text)
        assert match and match[1]==match[2],text
        for _ in range(4):d.g.qmp.cmd('human-monitor-command',**{'command-line':'mouse_move 120 120'});d.g.pump(.1)
        d.snap('shared-descriptors-and-append');print(match.group(),flush=True)
        type_text('sh\n');eventually(prompt,'interactive sh prompt')
        type_text('/data/apps/fdprobe/fdprobe.elf --network\n')
        eventually(lambda:'Network: connected TCP' in screen.text(w) and prompt(),'graphical network ownership checks')
        text=screen.text(w);matches=re.findall(r'fdprobe: (\d+)/(\d+) checks passed',text)
        assert matches and all(a==b for a,b in matches),text
        d.snap('shared-socket-traffic')
        type_text('echo 82649\n');eventually(lambda:'82649' in screen.text(w).splitlines() and prompt(),'prompt after socket checks')
        type_text('exit\n');eventually(prompt,'native prompt after sh')
        expected=b'child-net\nparent-net\nafter-kill\n'+b'delayed\n'*8
        assert bytes(received)==expected*2,(received,errors)
        assert datagrams==['datagram','','datagram',''] and not errors,(datagrams,errors)
        assert b'PANIC' not in d.g.seen
        (out/'fixtures.json').write_text(json.dumps({'tcp_bytes':len(received),'tcp':received.decode(),'udp':datagrams,'errors':errors},indent=2)+'\n')
        print('PASS: shared flags/cursors, concurrent append/read, TCP/UDP dup/spawn/exit/kill, native regressions and desktop prompt',flush=True)
    finally:
        try:d.snap('last')
        finally:
            d.g.close();stop.set()
            for t in threads:t.join(timeout=2)
            tcp.close();udp.close()

if __name__=='__main__':main()
