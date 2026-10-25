#!/usr/bin/env python3
"""Exercise malformed private worker descriptors without evaluating shell text."""
import os,struct,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[2]
binary=Path(os.environ.get('POSIXSH_BINARY',str(root/'build/posixsh-host')))
u32=lambda *values:struct.pack('<'+'I'*len(values),*values)
ast=u32(0x53484101,1,0) # empty program, valid and has no side effects
payload=u32(0x53484301,0,0,1,0,1,5)+b'probe'+u32(0,0,0,len(ast))+ast
checks=0
with tempfile.TemporaryDirectory(prefix='sh-worker-') as directory:
 def run(data,expected):
  global checks
  with tempfile.TemporaryFile() as f:
   f.write(data);f.flush();f.seek(0)
   r=subprocess.run([str(binary),'--emblink-worker',str(f.fileno())],pass_fds=(f.fileno(),),cwd=directory,env={'HOME':directory,'PATH':'/usr/bin:/bin'},capture_output=True,timeout=5)
  assert r.returncode==expected,(r.returncode,r.stdout,r.stderr,data[:32])
  checks+=1
 run(payload,0)
 for n in range(len(payload)):run(payload[:n],126)
 for offset,value in [(0,0),(4,1<<6),(4,1<<3),(4,1<<31),(20,0),(20,4097),(20,0xffffffff),(24,0xffffffff),(len(payload)-16,0xffffffff),(len(payload)-8,0xffffffff),(len(payload)-4,0xffffffff)]:
  data=bytearray(payload);struct.pack_into('<I',data,offset,value);run(data,126)
 run(payload+b'extra',126)
 with tempfile.TemporaryFile() as f:
  f.truncate(1024*1024+1)
  r=subprocess.run([str(binary),'--emblink-worker',str(f.fileno())],pass_fds=(f.fileno(),),cwd=directory,env={'HOME':directory,'PATH':'/usr/bin:/bin'},capture_output=True,timeout=5)
  assert r.returncode==126;checks+=1
 for fd in ['0','2','64','-1','not-a-descriptor']:
  r=subprocess.run([str(binary),'--emblink-worker',fd],cwd=directory,env={'HOME':directory,'PATH':'/usr/bin:/bin'},capture_output=True,timeout=5)
  assert r.returncode==126;checks+=1
 assert not list(Path(directory).iterdir()),'malformed worker created files'
print(f'PASS: {checks} worker startup checks: descriptor type/size, truncations, counts, flags and typed roots')
