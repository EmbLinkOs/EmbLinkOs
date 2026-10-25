"""Publisher-side encoding of kernel/update/protocol.h's 512-byte journal."""
import hashlib,re,struct,zlib
RECORD_BYTES=512
CONTROL_OFFSETS=(16<<10,32<<10)
FACTORY_OFFSET=48<<10
EFI_CAPACITY=16<<20
NO_SLOT=255

def empty_slot(guid):
    return dict(root_guid=guid,release=0,root_bytes=0,efi_bytes=0,version='',efi_sha256=bytes(32),root_sha256=bytes(32))

def encode(state):
    p=bytearray(RECORD_BYTES)
    struct.pack_into('<8sIIQI4B',p,0,b'EMUPST1\0',1,512,state['revision'],state.get('flags',0),state['active'],state.get('pending',NO_SLOT),state.get('previous',NO_SLOT),state.get('attempts',0))
    for offset,key in [(32,'data_guid'),(48,'esp_guid')]:
        assert len(state[key])==16;p[offset:offset+16]=state[key]
    assert len(state['slots'])==2
    for i,slot in enumerate(state['slots']):
        version=slot['version'].encode('ascii')
        if len(version)>31 or (version and not re.fullmatch(rb'[A-Za-z0-9._-]+',version)):raise ValueError('invalid release version')
        assert len(slot['root_guid'])==16 and len(slot['efi_sha256'])==len(slot['root_sha256'])==32
        struct.pack_into('<16sQQQ32s32s32s',p,64+136*i,slot['root_guid'],slot['release'],slot['root_bytes'],slot['efi_bytes'],version,slot['efi_sha256'],slot['root_sha256'])
    struct.pack_into('<I',p,336,state.get('last_error',0));struct.pack_into('<I',p,508,zlib.crc32(p[:508]))
    return bytes(p)

def decode(p):
    if len(p)!=512 or p[:8]!=b'EMUPST1\0' or struct.unpack_from('<II',p,8)!=(1,512) or struct.unpack_from('<I',p,508)[0]!=zlib.crc32(p[:508]) or any(p[340:508]):raise ValueError('invalid journal')
    revision,flags,active,pending,previous,attempts=struct.unpack_from('<QI4B',p,16)
    slots=[]
    for i in range(2):
        guid,release,root,efi,version,ehash,rhash=struct.unpack_from('<16sQQQ32s32s32s',p,64+136*i)
        slots.append(dict(root_guid=guid,release=release,root_bytes=root,efi_bytes=efi,version=version.split(b'\0',1)[0].decode('ascii'),efi_sha256=ehash,root_sha256=rhash))
    return dict(revision=revision,flags=flags,active=active,pending=pending,previous=previous,attempts=attempts,data_guid=p[32:48],esp_guid=p[48:64],slots=slots,last_error=struct.unpack_from('<I',p,336)[0])

def digest(path):
    h=hashlib.sha256()
    with open(path,'rb') as f:
        for data in iter(lambda:f.read(1<<20),b''):h.update(data)
    return h.digest()
