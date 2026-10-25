#!/usr/bin/env python3
"""Build a managed ESP + OS-A + OS-B + shared Data image. Never opens a device."""
import argparse,importlib.util,json,os,struct,sys,tempfile,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/embkfs_mkfs'),str(Path(__file__).resolve().parent)]
import mkuefidisk as disk
import mkfs_embkfs as mkfs
from protocol import encode,empty_slot,digest,CONTROL_OFFSETS,FACTORY_OFFSET,EFI_CAPACITY

def append_files(image,files):
    """Append bounded 8.3 paths to an existing FAT image at build time."""
    p=bytearray(image);bps=struct.unpack_from('<H',p,11)[0];spc=p[13];reserved=struct.unpack_from('<H',p,14)[0];nfats=p[16];spf=struct.unpack_from('<I',p,36)[0]
    assert bps==512 and spc==1
    fat=bytearray(p[reserved*512:(reserved+spf)*512]);data=(reserved+nfats*spf)*512
    def pos(cluster):return data+(cluster-2)*512
    def alloc(count):
        free=[]
        for c in range(2,len(fat)//4):
            if not struct.unpack_from('<I',fat,c*4)[0] and pos(c)+512<=len(p):
                free.append(c)
                if len(free)==count:break
        if len(free)!=count:raise ValueError('ESP has insufficient space')
        for a,b in zip(free,free[1:]+[0x0fffffff]):struct.pack_into('<I',fat,a*4,b)
        return free
    def find(parent,name):
        packed=disk._fat_name(name)
        for at in range(pos(parent),pos(parent)+512,32):
            if p[at:at+11]==packed:return ((struct.unpack_from('<H',p,at+20)[0]<<16)|struct.unpack_from('<H',p,at+26)[0])
        return None
    def entry(parent,name,kind,cluster,size):
        for at in range(pos(parent),pos(parent)+512,32):
            if p[at] in (0,0xe5):p[at:at+32]=disk._dirent(name,kind,cluster,size);return
        raise ValueError('too many entries in fixture directory')
    for path,(payload,capacity) in files.items():
        components=path.split('/');parent=2
        for name in components[:-1]:
            child=find(parent,name)
            if child is None:
                child=alloc(1)[0];entry(parent,name,0x10,child,0)
                p[pos(child):pos(child)+64]=disk._dirent('.',0x10,child,0)+disk._dirent('..',0x10,parent if parent!=2 else 0,0)
            parent=child
        if find(parent,components[-1]) is not None:raise ValueError('duplicate ESP file')
        if not 0<=len(payload)<=capacity:raise ValueError('invalid preallocation')
        chain=alloc(max(1,(capacity+511)//512));entry(parent,components[-1],0x20,chain[0],capacity)
        for i,c in enumerate(chain):
            chunk=payload[i*512:(i+1)*512]
            if chunk:p[pos(c):pos(c)+len(chunk)]=chunk
    for i in range(nfats):p[(reserved+i*spf)*512:(reserved+(i+1)*spf)*512]=fat
    allocated=sum(struct.unpack_from('<I',fat,i*4)[0]!=0 for i in range(2,(len(p)-data)//512+2))
    struct.pack_into('<I',p,512+488,(len(p)-data)//512-allocated);struct.pack_into('<I',p,512+492,0xffffffff)
    return p

def sparse_write(path,data):
    with open(path,'wb') as f:
        f.truncate(len(data))
        for offset in range(0,len(data),1<<16):
            chunk=data[offset:offset+(1<<16)]
            if chunk.strip(b'\0'):f.seek(offset);f.write(chunk)
        f.flush();os.fsync(f.fileno())

def make_data(path,size):
    prefixes=(b'etc',b'home',b'data')
    objects=[o for o in mkfs.discover_userland_objects()
             if any(o[0]==p or o[0].startswith(p+b'/') for p in prefixes)
             and o[0]!=b'data/apps' and not o[0].startswith(b'data/apps/')]
    names={o[0] for o in objects}
    for name in (b'data',b'home',b'etc',b'data/apps',b'data/pkg',b'data/pkg/apps',b'data/update'):
        if name not in names:objects.append((name,mkfs.L.DT_DIR,mkfs.L.S_IFDIR|0o755,b''))
    mkfs.make_image(str(path),size,objects=objects)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('efi',type=Path);p.add_argument('root',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--data',type=Path);p.add_argument('--data-mib',type=int,default=128)
    p.add_argument('--version',default='0.1.0');p.add_argument('--release',type=int,default=1)
    args=p.parse_args()
    if not 1<=args.release<1<<64 or args.data_mib<64:p.error('invalid release or data size')
    root=args.root.read_bytes();efi=args.efi.read_bytes()
    if len(root)%512 or len(efi)>EFI_CAPACITY:p.error('invalid OS/EFI size')
    with tempfile.TemporaryDirectory(prefix='emblink-managed-') as temporary:
        data_path=args.data or Path(temporary)/'data.img'
        if not args.data:make_data(data_path,args.data_mib<<20)
        data=data_path.read_bytes()
        ids=[uuid.uuid4() for _ in range(4)];wire=[disk._guid(i) for i in ids]
        slot=empty_slot(wire[1]);slot.update(release=args.release,root_bytes=len(root),efi_bytes=len(efi),version=args.version,efi_sha256=digest(args.efi),root_sha256=digest(args.root))
        state=dict(revision=1,active=0,data_guid=wire[3],esp_guid=wire[0],slots=[slot,empty_slot(wire[2])])
        record=encode(state)
        esp=append_files(disk.fat32(64*2048,efi,reserved_sectors=128),{'EFI/EMBLINK/SLOT0.EFI':(efi,EFI_CAPACITY),'EFI/EMBLINK/SLOT1.EFI':(b'',EFI_CAPACITY)})
        esp[3:11]=b'EMBUPD1 ';esp[6*512+3:6*512+11]=b'EMBUPD1 '
        for offset in (*CONTROL_OFFSETS,FACTORY_OFFSET):esp[offset:offset+512]=record
        parts=[];payloads=[];start=disk.FIRST_LBA
        for name,kind,length,identity,payload in [('EFI System',disk.ESP_TYPE,len(esp),ids[0],esp),('EmbLinkOS A',disk.DATA_TYPE,len(root),ids[1],root),('EmbLinkOS B',disk.DATA_TYPE,len(root),ids[2],None),('EmbLink Data',disk.DATA_TYPE,len(data),ids[3],data)]:
            sectors=length//512;parts.append((start,start+sectors-1,kind,name,identity))
            if payload is not None:payloads.append((start,payload))
            start+=sectors
        image=disk.gpt_disk(parts,start+2048,payloads);sparse_write(args.output,image)
        metadata={k:v for k,v in state.items() if k not in ('slots','data_guid','esp_guid')}
        metadata.update(version=args.version,release=args.release,partitions=[dict(name=t[3],guid=str(t[4]),start=t[0],sectors=t[1]-t[0]+1) for t in parts],root_sha256=slot['root_sha256'].hex(),efi_sha256=slot['efi_sha256'].hex())
        args.output.with_suffix('.json').write_text(json.dumps(metadata,indent=2)+'\n')
        print(f'Managed image: {args.output}; ESP, OS-A, OS-B, shared Data; release {args.release} {args.version}')
if __name__=='__main__':main()
