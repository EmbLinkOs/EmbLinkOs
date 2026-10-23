#!/usr/bin/env python3
"""Reproduce EmbLink's original Contour masters; no external icon dependencies."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]/'icons/masters'
SYMBOLS={
'check':'<path d="m5 12 4 4L19 6"/>',
'close':'<path d="m6 6 12 12M18 6 6 18"/>',
'plus':'<path d="M12 5v14M5 12h14"/>',
'minus':'<path d="M5 12h14"/>',
'chevron-right':'<path d="m9 5 7 7-7 7"/>',
'chevron-left':'<path d="m15 5-7 7 7 7"/>',
'chevron-down':'<path d="m5 9 7 7 7-7"/>',
'chevron-up':'<path d="m5 15 7-7 7 7"/>',
'arrow-right':'<path d="M4 12h16m-6-6 6 6-6 6"/>',
'search':'<circle cx="10.5" cy="10.5" r="6.5"/><path d="m16 16 5 5"/>',
'folder':'<path d="M3 7V5h7l2 3h9v11H3Z"/><path d="M3 10h18"/>',
'document':'<path d="M6 3h8l4 4v14H6Z"/><path d="M14 3v5h4M9 12h6m-6 4h6"/>',
'files':'<path d="M8 3h11v15H8Z"/><path d="M5 7H3v14h12v-1M11 8h5m-5 4h5"/>',
'home':'<path d="m3 10 9-7 9 7M5 9v12h14V9M10 21v-7h4v7"/>',
'grid':'<rect x="4" y="4" width="6" height="6" rx="1"/><rect x="14" y="4" width="6" height="6" rx="1"/><rect x="4" y="14" width="6" height="6" rx="1"/><rect x="14" y="14" width="6" height="6" rx="1"/>',
'list':'<path d="M8 6h13M8 12h13M8 18h13M3 6h.1M3 12h.1M3 18h.1"/>',
'gear':'<path d="M4 6h16M4 12h16M4 18h16"/><circle cx="9" cy="6" r="2"/><circle cx="16" cy="12" r="2"/><circle cx="8" cy="18" r="2"/>',
'clock':'<circle cx="12" cy="12" r="9"/><path d="M12 6v6l4 2"/>',
'bell':'<path d="M5 17h14l-2-3V9a5 5 0 0 0-10 0v5ZM10 21h4"/>',
'info':'<circle cx="12" cy="12" r="9"/><path d="M12 11v6m0-10v.2"/>',
'warning':'<path d="M12 3 22 21H2ZM12 9v5m0 3v.2"/>',
'bolt':'<path d="m14 2-10 12h7l-1 8 10-12h-7Z"/>',
'user':'<circle cx="12" cy="8" r="4"/><path d="M4 21v-2a8 8 0 0 1 16 0v2"/>',
'inbox':'<path d="M3 14 6 5h12l3 9v6H3Z"/><path d="M3 14h5l2 3h4l2-3h5"/>',
'dot':'<circle cx="12" cy="12" r="3" fill="white" stroke="none"/>',
'star':'<path d="m12 3 3 6 7 1-5 5 1 7-6-3-6 3 1-7-5-5 7-1Z"/>',
'heart':'<path d="M12 21 3 12C-2 4 8 0 12 7c4-7 14-3 9 5Z"/>',
'cloud':'<path d="M6 19a5 5 0 0 1-1-10 7 7 0 0 1 14-1 5.5 5.5 0 0 1 0 11Z"/>',
'reload':'<path d="M20 8A9 9 0 1 0 21 15M20 3v6h-6"/>',
'network':'<path d="M3 8a14 14 0 0 1 18 0M6 12a9 9 0 0 1 12 0m-9 4a4.5 4.5 0 0 1 6 0M12 20h.1"/>',
'volume':'<path d="M3 9h4l5-5v16l-5-5H3ZM16 8a6 6 0 0 1 0 8m3-11a10 10 0 0 1 0 14"/>',
'battery':'<rect x="2" y="6" width="18" height="12" rx="2"/><path d="M23 10v4M6 10v4m4-4v4m4-4v4"/>',
'trash':'<path d="M3 6h18M9 6V3h6v3M6 6l1 15h10l1-15M10 10v7m4-7v7"/>',
'window':'<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M3 9h18m-12 0v11"/>',
'display':'<rect x="2" y="3" width="20" height="14" rx="2"/><path d="M12 17v4m-5 0h10"/>',
'keyboard':'<rect x="2" y="5" width="20" height="14" rx="2"/><path d="M6 9h.2m4 0h.2m4 0h.2m4 0h.2M6 13h.2m4 0h.2m4 0h.2m4 0h.2M7 16h10"/>',
'palette':'<path d="M12 3a9 9 0 1 0 0 18h2a2 2 0 0 0 0-4h-1a2 2 0 0 1 0-4h5a3 3 0 0 0 3-3 9 9 0 0 0-9-7Z"/><path d="M7 9h.2m4-3h.2m5 2h.2M6 14h.2"/>',
'cursor':'<path d="M5 2v18l5-5 4 7 3-2-4-7h7Z"/>',
'terminal':'<path d="m5 6 6 6-6 6m8 0h7"/>',
'calculator':'<rect x="4" y="2" width="16" height="20" rx="2"/><path d="M7 8h10M8 12h.2m7 0h.2M8 16h.2m7 0h.2M8 19h.2m7 0h.2"/>',
'photo':'<rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="15" cy="8" r="2"/><path d="m3 18 6-7 5 6 3-3 4 5"/>',
'music':'<path d="M9 18V5l11-3v13M9 8l11-3"/><ellipse cx="6" cy="18" rx="3" ry="3"/><ellipse cx="17" cy="15" rx="3" ry="3"/>',
'edit':'<path d="m4 16-1 5 5-1L21 7l-4-4ZM14 6l4 4"/>',
'monitor':'<path d="M2 13h4l3-8 5 15 3-10h5"/>',
'launcher':'<path d="M4 5h7l3 7-3 7H4l3-7ZM15 5h5v14h-5"/>'}

def svg(body,size=24,bg=None):
    tile='' if bg is None else f'<rect x="2" y="2" width="60" height="60" rx="12" fill="{bg}"/><path d="M14 3h36" stroke="white" stroke-opacity=".2"/>'
    transform='' if bg is None else ' transform="translate(12 12) scale(1.666667)"'
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {size} {size}">{tile}<g{transform} fill="none" stroke="white" stroke-width="1.65" stroke-linecap="round" stroke-linejoin="round">{body}</g></svg>\n'

def main():
    for name,body in SYMBOLS.items():
        (ROOT/f'ui-{name}.svg').write_text(svg(body))
    apps={'files':('folder','#347d8b'),'terminal':('terminal','#334548'),'editor':('edit','#447c70'),'notepp':('document','#867044'),'setting':('gear','#607885'),'photos':('photo','#84657d'),'music':('music','#a15e59'),'monitor':('monitor','#477564'),'calc':('calculator','#8f7354'),'vellum':('window','#5b709c'),'netsurf':('network','#5b709c'),'file':('document','#657579'),'notifications':('bell','#607885'),'launcher':('launcher','#447c70'),'app_launcher':('grid','#447c70')}
    for name,symbol in [('list','list'),('camara','photo'),('partly_cloudy','cloud'),('zipfloder','folder')]:
        (ROOT/'svg'/f'{name}.svg').write_text(svg(SYMBOLS[symbol]))
    for name,(symbol,bg) in apps.items():(ROOT/f'{name}.svg').write_text(svg(SYMBOLS[symbol],64,bg))

if __name__=='__main__':main()
