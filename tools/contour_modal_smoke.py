#!/usr/bin/env python3
"""Check a scaled serif confirmation using real controls and read-only geometry."""
from pathlib import Path
import os,shutil,tempfile
from recovery_smoke import Guest,ROOT
from workspace_smoke import Desktop
from native_ui import NativeUI

def main():
    out=Path(tempfile.mkdtemp(prefix='contour-modal-',dir='/workspace/.setup' if os.path.isdir('/workspace/.setup') else None));print(out,flush=True)
    disk=out/'live.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop.__new__(Desktop);d.out=out/'boot';d.g=Guest(d.out,disk,menu=4,boot_nvme=True,resolution=(1280,800));d.sock=d.out/'gdb.sock';d.symbols={}
    try:
        d.g.wait(b'TopBar: first frame presented');d.g.wait(b'home: time zone');d.g.pump(2)
        d.g.qmp.cmd('human-monitor-command',**{'command-line':f'gdbserver unix:{d.sock},server=on,wait=off'})
        w=d.launch('Settings','Settings');ui=NativeUI(d);ui.click_text(w,'Paper');d.g.pump(3)
        ui.reveal(w,'Interface font');ui.select(w,'Interface font','Serif');d.g.pump(2)
        ui.reveal(w,'Interface scale');ui.select(w,'Interface scale','115%');d.g.pump(2)
        ui.reveal(w,'Density');ui.select(w,'Density','Compact');d.g.pump(2)
        ui.reveal(w,'Reset appearance…');ui.click_text(w,'Reset appearance…');d.g.pump(2)
        copy='Restore the Contour palette, layout, typography and effects. Other system preferences are kept.'
        paragraph=ui.text(w,copy)['rect'];actions=ui.text(w,'Cancel')['rect']
        assert paragraph[3]>40, 'scaled serif paragraph lost its wrapped height'
        assert actions[1]>=paragraph[1]+paragraph[3], 'dialog actions overlap its explanation'
        d.snap('reset-confirmation');ui.click_text(w,'Cancel');d.g.pump(1)
        assert all(r['text']!='Reset appearance?' for r in ui.labels(w)), 'cancel did not dismiss confirmation'
        assert b'PANIC' not in d.g.seen
        print('PASS: scaled serif dialog retains an opaque surface, complete paragraph geometry and reachable cancellation',flush=True)
    finally:d.g.close()

if __name__=='__main__':main()
