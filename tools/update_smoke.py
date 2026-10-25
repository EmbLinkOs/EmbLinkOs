#!/usr/bin/env python3
"""Real managed-NVMe upgrade through Settings, account login and cold reboot.
Build fixture/base.img with a temporary publisher key and source
http://10.0.2.2:18080/current, and serve fixture/current/release.emu + assets.
The host only observes via GDB; interactions use mouse, keyboard and serial.
Do not rebuild kernel/apps/libembk.so while this guest is active.
"""
from pathlib import Path
import argparse,functools,http.server,json,re,subprocess,tempfile,threading,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_ui import NativeUI
from native_terminal import TerminalState
from lock_test import keyname

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('fixture',type=Path);p.add_argument('--resume-disk',type=Path);args=p.parse_args()
    out=Path(tempfile.mkdtemp(prefix='update-acceptance-',dir='/workspace/.setup'));print(out,flush=True)
    handler=functools.partial(http.server.SimpleHTTPRequestHandler,directory=str(args.fixture))
    server=http.server.ThreadingHTTPServer(('0.0.0.0',18080),handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    disk=out/'live.img';subprocess.run(['cp','--sparse=always',str(args.resume_disk or args.fixture/'base.img'),str(disk)],check=True)
    metadata=json.loads((args.fixture/'base.json').read_text())
    def desktop(name,setup=False):
        d=Desktop.__new__(Desktop);d.out=out/name;d.g=Guest(d.out,disk,boot_nvme=True,resolution=(1280,800),nic='e1000e');d.sock=d.out/'gdb.sock';d.symbols={}
        try:d.login(setup=setup)
        except BaseException:d.g.close();raise
        return d
    def eventually(d,check,label,timeout=240):
        end=time.monotonic()+timeout
        while not check():
            if time.monotonic()>end:raise AssertionError(label)
            d.g.pump(.5)
    def status(d):
        s=d.g.command('run /data/apps/updatectl/updatectl.elf status',b'UPDATE request 0 result 0');print(s[-700:],flush=True);return s
    d=None
    try:
        d=desktop('before',setup=not args.resume_disk)
        status(d)
        terminal=d.launch('Terminal','Terminal');screen=TerminalState(d)
        eventually(d,lambda:screen.integer(terminal,'g_at_prompt',1)==1,'terminal prompt')
        def type_shell(d,text):
            for c in text:d.key({'>' :'shift-dot','/':'slash','|':'shift-backslash'}.get(c) or keyname(c))
        type_shell(d,'echo UPDATE-PROOF-KEEP-MY-FILES | save Documents/update-proof.txt\n')
        eventually(d,lambda:screen.integer(terminal,'g_at_prompt',1)==1,'saved personal file')
        settings=d.launch('Settings','Settings');ui=NativeUI(d);ui.click_text(settings,'Paper');d.g.pump(2)
        preferences=d.read('settings',settings['pid'],'g_cfg',64)
        ui.click_text(settings,'Software Update');d.snap('update-ready-to-check')
        ui.click_text(settings,'Check for Updates')
        eventually(d,lambda:any(r['text']=='Install Update…' for r in ui.labels(settings)),'verified release did not appear')
        d.snap('verified-update-available')
        ui.click_text(settings,'Install Update…');ui.click_text(settings,'Install')
        d.g.pump(4);d.snap('installing-update')
        eventually(d,lambda:int.from_bytes(d.read('settings',settings['pid'],'g_update',4,4),'little')==5,
                   'readback verification phase missing',360)
        ui.click_text(settings,'System');ui.click_text(settings,'Software Update')
        d.snap('verifying-installed-update')
        d.g.wait(b'update: verified release 2 staged in slot B',360)
        eventually(d,lambda:any(r['text']=='Restart to finish…' for r in ui.labels(settings)),'restart action missing')
        d.snap('update-ready-to-restart');assert 'phase=7' in status(d)
        d.g.close();d=None
        d=desktop('after')
        d.g.wait(b'update: healthy desktop confirmed release 2 in slot B',120)
        assert 'release=2 version=1.1.0' in status(d)
        settings=d.launch('Settings','Settings');ui=NativeUI(d)
        assert d.read('settings',settings['pid'],'g_cfg',64)==preferences,'preferences changed across upgrade'
        ui.click_text(settings,'Software Update');d.snap('update-completed')
        ui.click_text(settings,'Restore Previous System…');ui.click_text(settings,'Select Previous System');d.g.pump(2)
        assert 'phase=7' in status(d);d.snap('previous-system-selected')
        d.g.close();d=None
        d=desktop('rollback')
        d.g.wait(b'update: healthy desktop confirmed release 1 in slot A',120)
        assert 'release=1 version=1.0.0' in status(d)
        terminal=d.launch('Terminal','Terminal');screen=TerminalState(d)
        eventually(d,lambda:screen.integer(terminal,'g_at_prompt',1)==1,'rollback terminal prompt')
        type_shell(d,'cat Documents/update-proof.txt\n')
        eventually(d,lambda:'UPDATE-PROOF-KEEP-MY-FILES' in screen.text(terminal).splitlines(),'personal file lost across upgrade/rollback')
        d.snap('personal-files-preserved')
        assert b'PANIC' not in d.g.seen
        print('PASS: real administrator login; Settings checks signed feed, streams and verifies OS/EFI to B; cold reboot confirms B; Settings rollback and cold reboot confirms A; account remains usable',flush=True)
    finally:
        if d:d.g.close()
        server.shutdown();server.server_close()
if __name__=='__main__':main()
