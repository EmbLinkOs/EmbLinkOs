#!/usr/bin/env python3
"""Detect native syscall/capability collisions while integrating SDK changes."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
def numbers(path,prefix):
    return {name:int(value) for name,value in re.findall(r'^#define\s+'+prefix+r'(\w+)\s+(\d+)\b',(root/path).read_text(),re.M)}
kernel=numbers('kernel/include/syscall_nr.h','SYS_')
user=numbers('user/lib/embk_syscall.h','EMBK_SYS_')
assert kernel==user, f'Syscall SDK mismatch: {set(kernel.items()) ^ set(user.items())}'
assert len(set(kernel.values()))==len(kernel), 'Native syscall number reused'
ids=numbers('kernel/process/capabilities.h','EMBK_CAP_')
sdk=numbers('user/lib/embk.h','EMBK_CAP_')
ids.pop('MAX_ID')
assert ids==sdk, f'Capability SDK mismatch: {set(ids.items()) ^ set(sdk.items())}'
assert len(set(ids.values()))==len(ids),'Capability ID reused'
actions=numbers('kernel/process/spawn.h','SPAWN_ACTION_')
public=numbers('user/lib/embk.h','EMBK_SPAWN_ACTION_')
assert all(actions.get(k)==v for k,v in public.items()),'Spawn action SDK mismatch'
limit=numbers('kernel/process/spawn.h','SPAWN_')['ACTIONS_MAX']
assert limit==numbers('user/lib/embk.h','EMBK_SPAWN_')['ACTIONS_MAX'],'Spawn action limit mismatch'
print(f'Native ABI: {len(kernel)} unique syscalls and {len(ids)} matching capability IDs')
