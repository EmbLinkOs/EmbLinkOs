#!/usr/bin/env python3
"""Run shared observable cases on the explicit-spawn backend and a reference sh."""
import json,os,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[2]
binary=Path(os.environ.get('POSIXSH_BINARY',str(root/'build/posixsh-host')));probe=root/'build/shprobe-host'
cases=json.loads((root/'tests/posixsh/cases.json').read_text())
subprocess.run(['cc','-std=c11','-O1','-g','-DSH_PROBE_HELPER',str(root/'user/tests/shprobe/shprobe.c'),'-o',str(probe)],check=True)
failed=0
reference_checks=0
with tempfile.TemporaryDirectory(prefix='posixsh-host-') as directory:
 d=Path(directory);(d/'nested').mkdir()
 for name,text in [('alpha.txt', 'A\n'), ('beta.txt', 'B\n'), ('.hidden.txt', 'hidden\n'), ('source.sh', 'sourced=sourced-value\n'), ('file-script.sh', 'printf \'file:%s\\n\' "$1"\n')]: (d/name).write_text(text)
 for c in cases:
  script=c['script'].replace('@ROOT@',directory).replace('@PROBE@',str(probe))
  expected=c['stdout'].replace('@ROOT@',directory)
  tail=[script,'argument'] if c['option']=='--file' else ([c['option']] if c['option'] else [])+['-c',script,'case-sh']
  args=[str(binary)]+tail
  try:r=subprocess.run(args,cwd=d,env={'HOME':directory,'PATH':'/usr/bin:/bin'},text=True,capture_output=True,timeout=15)
  except subprocess.TimeoutExpired:
   failed+=1;print('TIMEOUT:',c['name'],flush=True);continue
  if not c['name'].startswith('unsupported ') and c.get('reference',True):
   reference=subprocess.run(['/bin/sh']+tail,cwd=d,env={'HOME':directory,'PATH':'/usr/bin:/bin'},text=True,capture_output=True,timeout=15)
   assert reference.returncode==c['status'] and reference.stdout==expected,(c['name'],'reference mismatch',reference.returncode,reference.stdout,reference.stderr)
   reference_checks+=1
  if r.returncode!=c['status'] or r.stdout!=expected:
   failed+=1;print('FAIL:',c['name'],r.returncode,repr(r.stdout),repr(r.stderr),'expected',c['status'],repr(expected),flush=True)
 assert not list(d.glob('.sh-*')),'worker context file leaked'
print(f'Host shell cases: {len(cases)-failed}/{len(cases)} passed; {reference_checks} also checked against /bin/sh',flush=True)
raise SystemExit(bool(failed))
