#!/usr/bin/env python3
"""Adversarial update transport and real firmware fallback on a disposable disk.
Run after update_smoke.py with its closed, confirmed-slot-A disk. Uses an
external temporary fixture signing key; no production or repository dev key.
"""
from pathlib import Path
import argparse,functools,hashlib,http.server,os,re,subprocess,sys,tempfile,threading,time
from recovery_smoke import Guest,ROOT
sys.path.insert(0,str(ROOT/'tools/update'))
from publish import descriptor
from protocol import decode,CONTROL_OFFSETS
from cryptography.hazmat.primitives.serialization import load_pem_private_key
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from cryptography.hazmat.primitives import hashes
import json

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('fixture',type=Path);p.add_argument('disk',type=Path);args=p.parse_args()
    fixture=args.fixture;out=Path(tempfile.mkdtemp(prefix='update-failures-',dir='/workspace/.setup' if os.path.isdir('/workspace/.setup') else None));print(out,flush=True)
    disk=out/'live.img';subprocess.run(['cp','--sparse=always',str(args.disk),str(disk)],check=True)
    meta=json.loads((fixture/'base.json').read_text());esp=meta['partitions'][0]['start']*512
    original=(fixture/'current').resolve();private=load_pem_private_key((fixture/'private.pem').read_bytes(),password=None)
    def release(name,efi=None,bad_signature=False,short_root=False,bad_root=False,release_id=3):
        path=fixture/name;path.mkdir(exist_ok=False)
        root=original/'os.img';boot=efi or original/'boot.efi'
        body=descriptor(root,boot,release_id,'1.3.0' if release_id==4 else '1.2.0','Recovery acceptance fixture')
        r,s=decode_dss_signature(private.sign(body,ec.ECDSA(hashes.SHA256())))
        record=body+r.to_bytes(32,'big')+s.to_bytes(32,'big')
        if bad_signature:record=record[:-1]+bytes([record[-1]^1])
        (path/'release.emu').write_bytes(record)
        if short_root or bad_root:
            subprocess.run(['cp','--sparse=always',str(root),str(path/'os.img')],check=True)
            with (path/'os.img').open('r+b') as f:
                if short_root:f.truncate(root.stat().st_size-512)
                else:f.seek(65536);b=f.read(1);f.seek(65536);f.write(bytes([b[0]^1]))
        else:os.link(root,path/'os.img')
        os.link(boot,path/'boot.efi');return path
    def select(path):
        temp=fixture/'current-failure-next';temp.symlink_to(path.name);os.replace(temp,fixture/'current')
    untrusted=release('failure-untrusted-'+out.name,bad_signature=True)
    short=release('failure-short-'+out.name,short_root=True)
    corrupt=release('failure-corrupt-'+out.name,bad_root=True)
    invalid_efi=out/'not-an-efi.bin';invalid_efi.write_bytes(b'bad executable'.ljust(512,b'\0'))
    badboot=release('failure-boot-'+out.name,efi=invalid_efi)
    slow=release('failure-interrupted-'+out.name)
    trial=release('failure-unconfirmed-'+out.name,release_id=4)
    class Handler(http.server.SimpleHTTPRequestHandler):
        def copyfile(self,source,output):
            if self.path.endswith('/os.img') and (fixture/'current').resolve()==slow.resolve():
                try:
                    while data:=source.read(32768):output.write(data);output.flush();time.sleep(.04)
                except (BrokenPipeError,ConnectionResetError):pass
            else:
                try:super().copyfile(source,output)
                except (BrokenPipeError,ConnectionResetError):pass
    server=http.server.ThreadingHTTPServer(('0.0.0.0',18080),functools.partial(Handler,directory=str(fixture)))
    threading.Thread(target=server.serve_forever,daemon=True).start();g=None
    def boot(name):
        guest=Guest(out/name,disk,menu=4,boot_nvme=True,resolution=(1280,800),nic='e1000e')
        try:guest.wait(b'home: time zone',240);guest.wait(b'TopBar: first frame presented');guest.pump(1)
        except BaseException:guest.close();raise
        return guest
    def reap(text):
        match=re.search(r'\[run\] started .* as pid (\d+)',text);assert match,text
        pid=match[1]
        g.command('wait '+pid,('[wait] pid '+pid+' exited with code 0').encode(),60)
    def status():
        text=g.command('run /data/apps/updatectl/updatectl.elf status',b'UPDATE request 0 result 0')
        row=re.search(r'UPDATE phase=(\d+) flags=(\d+) job=(\d+) release=(\d+).*? progress=(\d+) error=(-?\d+)',text)
        assert row,text;reap(text);return tuple(map(int,row.groups()))
    def request(action):
        text=g.command('run /data/apps/updatectl/updatectl.elf '+action,b'UPDATE request')
        assert re.search(r'UPDATE request \d+ result 0',text),text
        reap(text)
    def wait_phase(phase,timeout=420):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            row=status()
            if row[0]==phase and row[2]==0:return row
            g.pump(3)
        raise AssertionError(f'phase {phase} missing; {status()}')
    def journal():
        with disk.open('rb') as f:
            records=[]
            for offset in CONTROL_OFFSETS:
                f.seek(esp+offset)
                try:records.append(decode(f.read(512)))
                except ValueError:pass
        assert records;return max(records,key=lambda s:s['revision'])
    def os_hash(slot):
        part=meta['partitions'][1+slot];h=hashlib.sha256()
        with disk.open('rb') as f:
            f.seek(part['start']*512);remaining=part['sectors']*512
            while remaining:
                data=f.read(min(1<<20,remaining));assert data;h.update(data);remaining-=len(data)
        return h.hexdigest()
    before=os_hash(0)
    try:
        g=boot('reject-and-packages')
        for command in ['test pkg','test pkgbuild','test pkgstanza','test spawn-compat','test session']:
            text=g.command(command,('[cmd] '+command+':').encode(),timeout=240)
            assert re.search(r'\[cmd\] '+re.escape(command)+r':[^\n]*\bOK\b',text) and 'FAIL' not in text,text
            print('PASS: '+command,flush=True)
        select(untrusted);request('check');assert wait_phase(8)[5]!=0;assert journal()['pending']==255
        print('PASS: untrusted catalog refused before staging',flush=True)
        select(short);request('install');assert wait_phase(8)[5]!=0;assert journal()['pending']==255
        print('PASS: truncated response refused; confirmed slot unchanged',flush=True)
        select(corrupt);request('install');assert wait_phase(8)[5]!=0;assert journal()['pending']==255
        print('PASS: altered payload refused; confirmed slot unchanged',flush=True)
        select(slow);request('install')
        end=time.monotonic()+60
        while time.monotonic()<end:
            row=status()
            if row[0]==6 and row[4]>0:break
            g.pump(.2)
        else:raise AssertionError('download did not start')
        assert journal()['flags']&2
        g.p.kill();g.p.wait();g.close();g=None
        g=boot('after-interruption');assert status()[3]==1;assert journal()['pending']==255
        assert journal()['last_error']==3
        print('PASS: hard interruption while writing returns to confirmed slot A',flush=True)
        select(badboot);request('install');wait_phase(7)
        assert journal()['pending']==1
        g.close();g=None
        g=boot('bad-efi-fallback');assert status()[3]==1
        assert b'trial image unavailable; reverting' in g.seen
        assert journal()['pending']==255 and journal()['last_error']==2
        assert os_hash(0)==before,'confirmed OS changed during rejected or interrupted updates'
        g.qmp.screendump(str(out/'failed-update-restored.ppm'))
        print('PASS: signed but unloadable trial EFI rejected by real firmware; confirmed OS hash unchanged across all failures',flush=True)
        select(trial);request('install');wait_phase(7)
        g.close();g=None
        g=Guest(out/'unconfirmed-trial',disk,menu=1,boot_nvme=True,resolution=(1280,800),nic='e1000e')
        g.wait(b'update: managed release 4 1.3.0',240);g.wait(b'EmbLink OS Login: first frame presented')
        assert b'healthy desktop confirmed' not in g.seen
        g.p.kill();g.p.wait();g.close();g=None
        g=boot('unconfirmed-fallback');assert status()[3]==1
        assert journal()['pending']==255 and journal()['last_error']==1
        assert os_hash(0)==before
        print('PASS: a valid but unconfirmed trial gets one attempt, then firmware restores A on the next boot',flush=True)
    finally:
        if g:g.close()
        server.shutdown();server.server_close();select(original)
if __name__=='__main__':main()
