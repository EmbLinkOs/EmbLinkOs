"""Read what Terminal shows through its probe (user/apps/term/term_probe.h).

Terminal keeps one fixed-layout symbol, g_term_probe, current for exactly this
reader; nothing here knows the screen model's own structs, so the model can
change without the acceptance tools following it. The offsets below mirror the
header's _Static_asserts. A probe whose magic or version differs is refused
rather than misread, and a probe caught mid-update (odd serial, or a serial
that moved while it was being read) is read again.
"""
import struct

MAGIC, VERSION = 0x504d5254, 3
TEXT, PRIMARY, CELLS, CELL = 1368, 66904, 132440, 16
HEADER = struct.Struct('<4I4i4B5i3I2i3I')
# version 3: tabs, the selection, find, the font, the link under the pointer
V3, V3_SIZE, TABS = 394584, 400204 - 394584, 16


class TerminalState:
    def __init__(self, desktop):
        self.d = desktop

    def _raw(self, w, length, offset=0):
        return self.d.read('term', w['pid'], 'g_term_probe', length, offset)

    def probe(self, w, text=True, cells=False):
        for _ in range(20):
            head = self._raw(w, 88 + 256 + 1024)
            (magic, version, size, serial, cols, rows, cx, cy, at_prompt, dead, alt, visible,
             view, shell, master, session, sessions, prompt_serial, history, frames,
             input_len, caret, text_len, primary_len, flags) = HEADER.unpack_from(head)
            if magic != MAGIC or version != VERSION:
                raise RuntimeError(f'Terminal probe magic/version {magic:#x}/{version}, expected {MAGIC:#x}/{VERSION}')
            if serial & 1:
                self.d.g.pump(.05)
                continue
            p = {'cols': cols, 'rows': rows, 'cursor': (cx, cy), 'at_prompt': bool(at_prompt),
                 'dead': bool(dead), 'alt': bool(alt), 'cursor_visible': bool(visible), 'view': view,
                 'shell': shell, 'master': master, 'session': session, 'sessions': sessions,
                 'prompt_serial': prompt_serial, 'history_count': history, 'frames': frames,
                 'input': head[344:344 + max(0, input_len)].decode('utf-8', 'replace'), 'caret': caret,
                 'local_edit': bool(flags & 1), 'edit_shell': bool(flags & 2), 'inband_size': bool(flags & 4),
                 'cwd': head[88:344].split(b'\0', 1)[0].decode('utf-8', 'replace')}
            if text:
                v3 = self._raw(w, V3_SIZE, V3)
                cstr = lambda b: b.split(b'\0', 1)[0].decode('utf-8', 'replace')
                tabs, tab = struct.unpack_from('<2I', v3, 0)
                shells = struct.unpack_from(f'<{TABS}i', v3, 8)
                sel_len, = struct.unpack_from('<I', v3, 395680 - V3)
                find_open, find_found, find_row, find_col = struct.unpack_from('<2I2i', v3, 399780 - V3)
                font_px, cell_w, cell_h, link_len = struct.unpack_from('<f2iI', v3, 399924 - V3)
                origin = struct.unpack_from('<2i', v3, 400196 - V3)
                p.update({'tabs': tabs, 'tab': tab, 'tab_shells': list(shells[:tabs]),
                          'tab_titles': [cstr(v3[72 + 64 * i:72 + 64 * (i + 1)]) for i in range(tabs)],
                          'selection': v3[395684 - V3:395684 - V3 + sel_len].decode('utf-8', 'replace'),
                          'find_open': bool(find_open), 'find_found': bool(find_found), 'find_at': (find_row, find_col),
                          'find': cstr(v3[399796 - V3:399796 - V3 + 128]), 'font_px': font_px, 'cell': (cell_w, cell_h),
                          'link': v3[399940 - V3:399940 - V3 + link_len].decode('utf-8', 'replace'),
                          'origin': origin})
                p['text'] = self._raw(w, text_len, TEXT).decode('utf-8', 'replace').split('\n') if text_len else ['']
                p['primary'] = self._raw(w, primary_len, PRIMARY).decode('utf-8', 'replace').split('\n') if primary_len else ['']
            if cells:
                n = min(cols * rows, 16384)
                data = self._raw(w, n * CELL, CELLS)
                p['cells'] = [[dict(zip(('rune', 'fg', 'bg', 'width', 'flags'),
                                        struct.unpack_from('<3I2B', data, (y * cols + x) * CELL)))
                               for x in range(cols)] for y in range(rows) if (y + 1) * cols <= n]
            if struct.unpack_from('<4I', self._raw(w, 16))[3] == serial:
                return p
        raise RuntimeError('Terminal probe kept changing while it was read')

    def state(self, w, primary=False):
        p = self.probe(w, cells=True)
        if primary:
            p['text'] = p['primary']
        return p

    def text(self, w, primary=False):
        p = self.probe(w)
        return '\n'.join(p['primary'] if primary else p['text'])

    def at_prompt(self, w):
        return self.probe(w, text=False)['at_prompt']
