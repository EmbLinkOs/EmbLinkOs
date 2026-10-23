"""Read labels and arranged bounds without adding any test API to the OS.

Uses the guest's actual shared-library mapping and host-compiled C offsets.
It only reads physical memory through GDB; all actions still use QMP input.
This helper is for x86_64 native acceptance, like workspace_smoke.Desktop.
"""
from pathlib import Path
import json,subprocess
from recovery_smoke import ROOT

class NativeUI:
    def __init__(self,desktop):
        self.d=desktop
        probe=desktop.out/'ui-probe'
        subprocess.run(['cc','-std=c11','-Iui/declare','-Iui/scene','-Iui/layout','-Iui/reactive','-Iui/backend','-Iuser/lib',
            'tools/ui_probe.c','-o',str(probe)],cwd=ROOT,check=True)
        self.offsets=json.loads(subprocess.check_output([str(probe)]))
        nm=subprocess.check_output(['x86_64-elf-nm','-S','--defined-only',str(ROOT/'build/libembk.so')],text=True)
        symbols={a[3]:int(a[0],16) for line in nm.splitlines() if len(a:=line.split())==4}
        self.symbols={k:symbols[k] for k in ['g_inst','g_inst_hw','g_sa','g_hovered','g_clicked','g_active','g_focused','g_ptr_down','g_zoom_open','g_tip_visible','g_zoom_overlay','g_menu_scrim']}
    def labels(self,window,diagnostic=False):
        current=next(r for r in self.d.state()["windows"] if r["id"]==window["id"])
        window.update(current)
        o=self.offsets;s=self.symbols
        return self.d.debug(f'''import gdb,json,struct
pid={window['pid']};offsets={o!r};symbols={s!r}
ps=gdb.parse_and_eval("'kernel/process/process.c'::process_table")
p=next(ps[i] for i in range(ps.type.range()[1]+1) if int(ps[i]['pid'])==pid)
cr3=int(p['pml4_phys']);bias=int(p['layout']['dylib_base']);inf=gdb.selected_inferior();direct=0xffff800000000000
physical={{}};cache={{}}
def phys_read(address,n):
    key=(address,n)
    if key not in physical:physical[key]=inf.read_memory(direct+address,n).tobytes()
    return physical[key]
def translate(va):
    table=cr3
    for shift in [39,30,21,12]:
        entry=int.from_bytes(phys_read(table+((va>>shift)&511)*8,8),'little')
        if not entry&1:raise RuntimeError('unmapped UI page')
        if shift in [30,21] and entry&128:return (entry&0x000ffffffffff000&~((1<<shift)-1))+(va&((1<<shift)-1))
        table=entry&0x000ffffffffff000
    return table+(va&4095)
def read(va,n):
    out=bytearray()
    while n:
        page=va&~4095
        if page not in cache:cache[page]=phys_read(translate(page),4096)
        k=min(n,4096-(va&4095));out.extend(cache[page][va&4095:(va&4095)+k]);va+=k;n-=k
    return bytes(out)
def uint(b,offset):return struct.unpack_from('<I',b,offset)[0]
def f32(b,offset):return struct.unpack_from('<f',b,offset)[0]
count=uint(read(bias+symbols['g_inst_hw'],4),0)
if not 0<count<4096:raise RuntimeError('unexpected native instance count '+str(count))
instances=read(bias+symbols['g_inst'],count*offsets['instance_size'])
arena=int.from_bytes(read(bias+symbols['g_sa'],8),'little');pages=struct.unpack('<256Q',read(arena,2048))
nodes={{}};worlds={{}}
def node(index):
    if index not in nodes:
        page=pages[index//offsets['page_size']]
        nodes[index]=read(page+(index%offsets['page_size'])*offsets['node_size'],offsets['node_size'])
    return nodes[index]
def world(index,depth=0):
    if index in worlds:return worlds[index]
    if not index or depth>64:return (0,0,1,1,1,False,None)
    a=node(index);parent=uint(a,offsets['n_parent']);x,y,sx,sy,opacity,skip,clip=world(parent,depth+1)
    x+=f32(a,offsets['n_tx'])*sx;y+=f32(a,offsets['n_ty'])*sy
    sx*=f32(a,offsets['n_sx']);sy*=f32(a,offsets['n_sy']);opacity*=f32(a,offsets['n_opacity']);skip=skip or bool(a[offsets['n_pass']])
    value=(x,y,sx,sy,opacity,skip,clip);worlds[index]=value;return value
def bounds(index):
    a=node(index);x,y,sx,sy,opacity,skip,clip=world(index)
    return [x,y,f32(a,offsets['n_width'])*sx,f32(a,offsets['n_height'])*sy]
def instance(index):return instances[index*offsets['instance_size']:(index+1)*offsets['instance_size']]
rows=[]
for i in range(1,count):
    a=instance(i)
    if not a[offsets['i_has_text']]:continue
    index=uint(a,offsets['i_scene'])
    if not index:continue
    n=node(index)
    if uint(n,offsets['n_self'])!=index or uint(n,offsets['n_self']+4)!=uint(a,offsets['i_scene']+4):continue
    x,y,w,h=bounds(index);_,_,_,_,alpha,skip,_=world(index)
    if alpha<.1 or skip or w<=0 or h<=0 or x+w<=0 or y+h<=0 or x>={window['cw']} or y>={window['ch']}:continue
    parent=uint(a,offsets['i_parent']);ancestors=[];valid=True
    for step in range(10):
        if not parent or parent>=count:break
        p=instance(parent);ni=uint(p,offsets['i_scene']);r=bounds(ni);pn=node(ni)
        if pn[offsets['n_clip']] and (x+w<=r[0] or y+h<=r[1] or x>=r[0]+r[2] or y>=r[1]+r[3]):valid=False;break
        ancestors.append(r);parent=uint(p,offsets['i_parent'])
    if valid:rows.append({{'text':a[offsets['i_text']:offsets['i_text']+256].split(b'\\0',1)[0].decode('utf-8','replace'),'rect':[x,y,w,h],'parents':ancestors}})
if {diagnostic!r}:
    states={{}}
    for key in ['g_hovered','g_clicked','g_active','g_focused']:
        index=uint(read(bias+symbols[key],8),0);chain=[]
        while 0<index<count and len(chain)<20:
            a=instance(index);n=uint(a,offsets['i_scene'])
            chain.append({{'index':index,'text':a[offsets['i_text']:offsets['i_text']+256].split(b'\\0',1)[0].decode('utf-8','replace'),'rect':bounds(n),'key':int.from_bytes(a[offsets['i_key']:offsets['i_key']+8],'little'),'layer':node(n)[offsets['n_layer']],'passthrough':bool(node(n)[offsets['n_pass']])}})
            index=uint(a,offsets['i_parent'])
        states[key]=chain
    for key in ['g_ptr_down','g_tip_visible','g_zoom_open']:states[key]=int.from_bytes(read(bias+symbols[key],1 if key=='g_ptr_down' else 4),'little')
    for key in ['g_zoom_overlay','g_menu_scrim']:states[key]=list(struct.unpack('<II',read(bias+symbols[key],8)))
    states['kernel_ptr_pid']=int(gdb.parse_and_eval("'kernel/gfx/compositor.c'::g_ptr_pid"))
    states['kernel_buttons']=int(gdb.parse_and_eval("'kernel/gfx/compositor.c'::g_ptr_buttons"))
    rows=states
print('RESULT '+json.dumps(rows))''')
    def text(self,w,label):
        rows=[r for r in self.labels(w) if r['text']==label]
        if not rows:raise AssertionError('visible label missing: '+label)
        return min(rows,key=lambda r:r["rect"][0])
    def click_text(self,w,label):
        r=self.text(w,label)['rect'];self.d.click(int(w['x']+r[0]+r[2]/2),int(w['y']+r[1]+r[3]/2))
    def select(self,w,title,label):
        rows=self.labels(w);r=next(r for r in rows if r['text']==title)['parents'][1]
        options=[p for p in rows if p['text']==label and r[1]<=p['rect'][1]<r[1]+r[3]]
        if not options:raise AssertionError('choice not visible: '+title+' / '+label)
        p=options[-1]['rect'];self.d.click(int(w['x']+p[0]+p[2]/2),int(w['y']+p[1]+p[3]/2))
    def toggle(self,w,title):
        r=self.text(w,title)['parents'][1]
        self.d.click(int(w['x']+r[0]+r[2]-22),int(w['y']+r[1]+r[3]/2))
    def move(self,x,y):
        import time
        deadline=time.monotonic()+30
        while True:
            ax,ay=self.d.state()['mouse']
            if (ax,ay)==(x,y):return
            if time.monotonic()>deadline:raise TimeoutError('native pointer move')
            self.d.g.qmp.cmd('human-monitor-command',**{'command-line':f'mouse_move {max(-40,min(40,x-ax))} {max(-40,min(40,y-ay))}'})
            self.d.g.pump(.08)
    def scroll(self,w,steps):
        self.move(w['x']+w['cw']-45,w['y']+w['ch']//2)
        button='wheel-down' if steps>0 else 'wheel-up'
        for _ in range(abs(steps)):
            self.d.g.qmp.cmd('input-send-event',events=[{'type':'btn','data':{'down':True,'button':button}},{'type':'btn','data':{'down':False,'button':button}}])
            self.d.g.pump(.2)
        self.d.g.pump(1)
