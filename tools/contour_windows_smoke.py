#!/usr/bin/env python3
"""Native minimum-size, maximize/restore and layout-board acceptance."""
from pathlib import Path
import os,shutil,tempfile,time
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_ui import NativeUI

def main():
    out=Path(tempfile.mkdtemp(prefix='contour-windows-',dir='/workspace/.setup' if os.path.isdir('/workspace/.setup') else None));print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800));d.sock=d.out/'gdb.sock';d.symbols={}
    def eventually(check,message):
        end=time.monotonic()+30
        while not check():
            if time.monotonic()>end:raise AssertionError(message)
            d.g.pump(.5)
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone');d.g.pump(2)
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        w=d.launch('Settings','Settings');ui=NativeUI(d)
        ui.move(w['x']+w['cw']-10,w['y']+w['ch']-10)
        d.g.qmp.cmd('human-monitor-command',**{'command-line':'mouse_button 1'});d.g.pump(.5)
        ui.move(w['x']+600,w['y']+300)
        d.g.qmp.cmd('human-monitor-command',**{'command-line':'mouse_button 0'});d.g.pump(2)
        eventually(lambda:(d.window('Settings')['cw'],d.window('Settings')['ch'])==(640,420),'resize did not respect the application minimum')
        d.snap('compact-settings')
        w=d.window('Settings');normal=(w['x'],w['y'],w['cw'],w['ch'])
        # Both click paths: arrive directly, then deliberately hover the board.
        d.click(w['x']+84,w['y']+18)
        eventually(lambda:d.window('Settings')['cw']==1280,'maximize did not fill work area');d.snap('window-fill')
        w=d.window('Settings');ui.move(w['x']+84,w['y']+18);d.g.pump(1)
        d.click(w['x']+84,w['y']+18)
        eventually(lambda:tuple(d.window('Settings')[k] for k in ['x','y','cw','ch'])==normal,'maximize did not restore original geometry')
        d.snap('window-restored')
        w=d.window('Settings');ui.move(w['x']+84,w['y']+18);d.g.pump(1)
        ui.click_text(w,'Right')
        eventually(lambda:d.window('Settings')['x']==640 and d.window('Settings')['cw']==640,'right tiling failed')
        d.snap('window-tiled')
        assert b'PANIC' not in d.g.seen
        print('PASS: compact resize minimums, fill/restore through the hover board, and right-half tiling',flush=True)
    finally:
        try:d.snap('last')
        finally:d.g.close()

if __name__=='__main__':main()
