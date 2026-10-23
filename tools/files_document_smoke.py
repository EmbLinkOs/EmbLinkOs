#!/usr/bin/env python3
"""Drive Files -> create document -> Editor -> save, in a real user session."""
from pathlib import Path
import shutil
import tempfile
import time
from workspace_smoke import Desktop,ROOT

def main():
    out=Path(tempfile.mkdtemp(prefix='emblink-files-document-'))
    print(f'Logs and captures: {out}',flush=True)
    disk=out/'workspace.img';shutil.copyfile(ROOT/'uefi-usb.img',disk)
    d=Desktop(out/'boot',disk)
    def value(app,w,symbol,size=256):
        return d.read(app,w['pid'],symbol,size).split(b'\0',1)[0]
    try:
        d.login(setup=True);d.click(555,745);f=d.wait_window('Files')
        d.g.wait(b'Files: first frame presented');d.snap('files-new-document')
        d.click(f['x']+930,f['y']+90)
        assert value('files',f,'g_naming_document',1)==b'\1'
        d.click(800,164);d.key('meta_l-a');d.type('FromFiles.txt');d.click(982,164)
        e=d.wait_window('Editor');d.g.wait(b'Editor: first frame presented')
        assert value('edit',e,'g_path')==b'/home/tester/FromFiles.txt','Files path: '+repr(value('edit',e,'g_path'))
        d.click(e['x']+100,e['y']+150);d.type('Created from Files.\n')
        deadline=time.monotonic()+15
        while value('edit',e,'g_doc')!=b'Created from Files.\n':
            if time.monotonic()>deadline:raise AssertionError('Editor lost typed document')
            d.g.pump(.5)
        d.key('ctrl-s');d.g.pump(2)
        assert value('edit',e,'g_status').startswith(b'Saved ')
        d.snap('files-created-and-saved');d.key('esc');d.g.pump(1)
        d.click(f['x']+930,f['y']+90);d.click(800,164);d.key('meta_l-a');d.type('FromFiles.txt')
        d.click(982,164);d.g.pump(1)
        assert b'already exists' in value('files',f,'g_status')
        assert all(w['title']!='Editor' for w in d.state()['windows'])
        d.snap('files-name-conflict')
        assert b'PANIC' not in d.g.seen
        print('PASS: Files creates a home document, launches Editor with HOME/EDIT_FILE, saves it and refuses a duplicate filename',flush=True)
    finally:
        try:d.snap('files-last')
        finally:d.g.close()

if __name__=='__main__':main()
