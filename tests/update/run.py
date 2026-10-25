#!/usr/bin/env python3
"""Cross-implementation release signatures and publisher/firmware journal ABI."""
import ctypes,importlib.util,subprocess,sys,tempfile,uuid
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from cryptography.hazmat.primitives import hashes
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/update'))
from protocol import encode,empty_slot
from publish import descriptor

def main():
    with tempfile.TemporaryDirectory(prefix='emblink-update-host-') as tmp:
        tmp=Path(tmp);binary=tmp/'protocol'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Ikernel','tests/update/protocol_host.c','kernel/update/protocol.c','-o',str(binary)],cwd=ROOT,check=True)
        subprocess.run([str(binary)],check=True)
        libpath=tmp/'bridge.so'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O1','-fPIC','-shared','-Ikernel','-Iuser/lib','-Iuser/update','-Iuser/lib/tls/crypto','tests/update/host_bridge.c','kernel/update/protocol.c','user/update/release.c','kernel/crypto/sha256.c','user/lib/tls/crypto/ecdsa.c','user/lib/tls/crypto/bignum.c','-o',str(libpath)],cwd=ROOT,check=True)
        lib=ctypes.CDLL(str(libpath));checks=0
        ids=[uuid.uuid4().bytes_le for _ in range(4)];slot=empty_slot(ids[1]);slot.update(release=1,root_bytes=4096,efi_bytes=512,version='1.0.0',root_sha256=b'r'*32,efi_sha256=b'e'*32)
        state=dict(revision=1,active=0,esp_guid=ids[0],data_guid=ids[3],slots=[slot,empty_slot(ids[2])])
        wire=encode(state);out=ctypes.create_string_buffer(512)
        assert lib.journal_roundtrip(wire,out)==0 and out.raw==wire;checks+=1
        for i in range(512):
            damaged=bytearray(wire);damaged[i]^=1
            assert lib.journal_roundtrip(bytes(damaged),out)<0;checks+=1
        root=tmp/'os.img';root.write_bytes(b'x'*4096);efi=tmp/'boot.efi';efi.write_bytes(b'EFI fixture')
        private=ec.generate_private_key(ec.SECP256R1());pub=private.public_key().public_numbers();key=pub.x.to_bytes(32,'big')+pub.y.to_bytes(32,'big')
        body=descriptor(root,efi,2,'1.1.0','Signed test release')
        r,s=decode_dss_signature(private.sign(body,ec.ECDSA(hashes.SHA256())))
        signed=body+r.to_bytes(32,'big')+s.to_bytes(32,'big')
        assert lib.release_verify(signed,len(signed),key)==0;checks+=1
        for n in [0,1,383,384,447,449,512]:assert lib.release_verify(signed,n,key)<0;checks+=1
        for i in range(0,448,7):
            damaged=bytearray(signed);damaged[i]^=1
            assert lib.release_verify(bytes(damaged),448,key)<0;checks+=1
        other=ec.generate_private_key(ec.SECP256R1()).public_key().public_numbers();wrong=other.x.to_bytes(32,'big')+other.y.to_bytes(32,'big')
        assert lib.release_verify(signed,448,wrong)<0;checks+=1
        print(f'PASS: {checks} publisher/journal ABI and signed release checks using the production parser and ECDSA verifier')
if __name__=='__main__':main()
