"""Read the native screen model without adding a test interface to Terminal."""
import json,struct,subprocess
from recovery_smoke import ROOT
class TerminalState:
    def __init__(self,desktop):
        self.d=desktop;probe=desktop.out/'vt-probe'
        subprocess.run(['cc','-std=c11','-Iuser/terminal','tools/vt_probe.c','-o',str(probe)],cwd=ROOT,check=True)
        self.o=json.loads(subprocess.check_output([str(probe)]))
    def integer(self,w,name,n=4):return int.from_bytes(self.d.read('term',w['pid'],name,n),'little',signed=n>1)
    def state(self,w,primary=False):
        o=self.o
        dimensions=self.d.read('term',w['pid'],'g_vt',9,o['vt_cols'])
        cols,rows=struct.unpack_from('<ii',dimensions);alt=bool(dimensions[8])
        offset=o['vt_primary'] if primary or not alt else o['vt_alternate']
        data=self.d.read('term',w['pid'],'g_vt',o['screen_size'],offset)
        order=struct.unpack_from('<60I',data,o['vt_screen_order']);texts=[];cells=[]
        for y in range(rows):
            row=[];text=''
            for x in range(cols):
                p=o['vt_screen_lines']+order[y]*o['line_size']+x*o['cell_size']
                rune=struct.unpack_from('<I',data,p+o['vt_cell_rune'])[0]
                combining=struct.unpack_from('<2I',data,p+o['vt_cell_combining'])
                width=data[p+o['vt_cell_width']]
                fg,bg=struct.unpack_from('<II',data,p+o['vt_cell_fg'])
                row.append({'rune':rune,'width':width,'fg':fg,'bg':bg})
                if width:text+=chr(rune)+''.join(chr(c) for c in combining if c)
            cells.append(row);texts.append(text.rstrip())
        cursor=struct.unpack_from('<ii',data,o['vt_screen_cursor'])
        return {'cols':cols,'rows':rows,'alt':alt,'text':texts,'cells':cells,'cursor':cursor}
    def text(self,w,primary=False):return '\n'.join(self.state(w,primary)['text'])
