#!/usr/bin/env python3
"""Publish an immutable signed system release. The private key stays external.

Serve the output directory at the read-only image's configured update source.
No production key, channel URL or private signing material is supplied here.
"""
import argparse,hashlib,os,re,shutil,struct
from pathlib import Path
from cryptography.hazmat.primitives.serialization import load_pem_private_key
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from cryptography.hazmat.primitives import hashes

def descriptor(root,efi,release,version,notes):
    v=version.encode('ascii');note=notes.encode('ascii')
    if not 1<=release<1<<64 or not re.fullmatch(rb'[A-Za-z0-9._-]{1,31}',v):raise ValueError('invalid release/version')
    if len(note)>223 or any(c<32 or c>126 for c in note):raise ValueError('notes must be at most 223 printable ASCII bytes')
    rn=root.stat().st_size;en=efi.stat().st_size
    if not rn or rn%512 or rn>8<<30 or not 0<en<=16<<20:raise ValueError('invalid asset sizes')
    def digest(path):
        h=hashlib.sha256()
        with path.open('rb') as f:
            for b in iter(lambda:f.read(1<<20),b''):h.update(b)
        return h.digest()
    p=bytearray(384);struct.pack_into('<8sIIQQQII16s32s32s32s224s',p,0,b'EMUREL1\0',1,448,release,rn,en,1,1,b'x86_64',v,digest(root),digest(efi),note)
    return bytes(p)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root',type=Path,required=True);p.add_argument('--efi',type=Path,required=True)
    p.add_argument('--key',type=Path,required=True);p.add_argument('--release',type=int,required=True)
    p.add_argument('--version',required=True);p.add_argument('--notes',default='');p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();key=load_pem_private_key(args.key.read_bytes(),password=None)
    if not isinstance(key,ec.EllipticCurvePrivateKey) or not isinstance(key.curve,ec.SECP256R1):p.error('signing key must be P-256')
    data=descriptor(args.root,args.efi,args.release,args.version,args.notes)
    r,s=decode_dss_signature(key.sign(data,ec.ECDSA(hashes.SHA256())))
    args.output.mkdir(parents=True,exist_ok=False)
    for name,source in [('os.img',args.root),('boot.efi',args.efi)]:
        with source.open('rb') as src,(args.output/name).open('xb') as dst:
            shutil.copyfileobj(src,dst);dst.flush();os.fsync(dst.fileno())
    with (args.output/'release.emu').open('xb') as f:f.write(data+r.to_bytes(32,'big')+s.to_bytes(32,'big'));f.flush();os.fsync(f.fileno())
    public=key.public_key().public_numbers()
    (args.output/'public-key.bin').write_bytes(public.x.to_bytes(32,'big')+public.y.to_bytes(32,'big'))
    print(f'Published signed release {args.release} ({args.version}) to {args.output}; configure only the public key in OS images')
if __name__=='__main__':main()
