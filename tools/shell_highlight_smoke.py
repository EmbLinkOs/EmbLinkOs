#!/usr/bin/env python3
"""Suggestions and colours as the line is typed, live in the Terminal.

Driven over QMP like a person would, in a Live session. rc.esh carries the
Mac's Powerlevel10k configuration (shell_p10k_smoke.SETTINGS) and a new tab
reads it. Two lines are run, so history has them; then, without running
anything more:

  - `ec` brings the newest line that starts with it, after the cursor, in
    another colour; Right takes it, and the string in it is yellow
  - a line with a builtin, a directory, a pipe, quoted text and a comment:
    the commands green, the directory underlined, the string yellow, the
    comment bold
  - a name that is no command, red and bold
  - a word on its way to a file's name, underlined while it ends the line
"""
from pathlib import Path
import os, shutil, tempfile, time
from recovery_smoke import Guest, ROOT
from workspace_smoke import Desktop
from native_terminal import TerminalState
from lock_test import keyname
from shell_p10k_smoke import SETTINGS

BOLD, UNDERLINE = 0x01, 0x08
DEFAULT = 0xffffffff


def main():
    out = Path(tempfile.mkdtemp(prefix='shell-highlight-', dir='/workspace/.setup' if os.path.isdir('/workspace/.setup') else None))
    print(out, flush=True)
    disk = out / 'live.img'
    shutil.copyfile(ROOT / 'uefi-usb.img', disk)
    d = Desktop.__new__(Desktop)
    d.out = out / 'boot'
    d.g = Guest(d.out, disk, menu=4, boot_nvme=True, resolution=(1280, 800))
    d.sock = d.out / 'gdb.sock'
    d.symbols = {}

    def eventually(check, message, seconds=60):
        end = time.monotonic() + seconds
        while not check():
            if time.monotonic() > end:
                raise AssertionError(message)
            d.g.pump(.4)

    def type_text(value):
        for c in value:
            d.key(keyname(c))

    def run(line, what):
        serial = screen.probe(w, text=False)['prompt_serial']
        type_text(line + '\n')
        eventually(lambda: screen.at_prompt(w) and screen.probe(w, text=False)['prompt_serial'] != serial, what)

    def typed():
        """The row the typing is on: its text and its cells."""
        st = screen.state(w)
        y = st['cursor'][1]
        return st['text'][y], st['cells'][y]

    def span(word):
        text, cells = typed()
        x = text.find(word)
        assert x >= 0, (word, text)
        return cells[x:x + len(word)]

    def clear_line():
        d.key('ctrl-u')
        eventually(lambda: typed()[0].rstrip().endswith('❯'), 'the line cleared')

    try:
        d.g.wait(b'TopBar: first frame presented')
        d.g.wait(b'home: time zone')
        d.g.qmp.cmd('human-monitor-command', **{'command-line': f'gdbserver unix:{d.sock},server=on,wait=off'})
        screen = TerminalState(d)
        w = d.launch('Terminal', 'Terminal')
        eventually(lambda: screen.at_prompt(w), 'initial prompt')
        run('mkdir ".config/shell"', 'mkdir')
        for i, setting in enumerate(SETTINGS):
            quoted = setting.replace('\\', '\\\\').replace('"', '\\"')
            run(f'echo "{quoted}" out{">" if i == 0 else ">>"} ".config/shell/rc.esh"', 'rc.esh: ' + setting)
        d.key('meta_l-t')
        eventually(lambda: screen.probe(w)['tabs'] == 2 and screen.at_prompt(w), 'a second tab')
        run('echo "hello, world"', 'a line for the history')
        run('ls /system | count', 'another')
        run('clear', 'clear')

        # a suggestion from history
        type_text('ec')
        eventually(lambda: 'ec' + 'ho "hello, world"' in typed()[0], 'the suggestion after the typing')
        typing, rest = span('ec'), span('ho "hello, world"')
        assert all(c['fg'] != typing[0]['fg'] for c in rest if c['rune'] > 32), ('the suggestion in its own colour', rest)
        d.snap('highlight-suggestion')
        d.key('right')
        eventually(lambda: span('"hello, world"')[0]['fg'] != rest[0]['fg'], 'Right takes it')
        echo, string = span('echo'), span('"hello, world"')
        assert echo[0]['fg'] not in (DEFAULT, string[0]['fg']), ('echo is a command, the string another colour', echo, string)
        d.snap('highlight-accepted')
        clear_line()

        # a whole line, not run
        line = 'ls /system/bin | where name =~ "shell" # the shell'
        type_text(line)
        eventually(lambda: line in typed()[0], 'the line typed')
        ls, path, where, string, comment = (span('ls'), span('/system/bin'), span('where'), span('"shell"'),
                                            span('# the shell'))
        assert ls[0]['fg'] == where[0]['fg'] != DEFAULT and not ls[0]['flags'] & BOLD, ('commands green', ls, where)
        assert all(c['flags'] & UNDERLINE for c in path), ('the directory underlined', path)
        assert string[0]['fg'] not in (DEFAULT, ls[0]['fg']), ('the string yellow', string)
        assert all(c['flags'] & BOLD for c in comment if c['rune'] > 32), ('the comment bold', comment)
        d.snap('highlight-line')
        clear_line()

        type_text('frobnicate --now')
        eventually(lambda: 'frobnicate' in typed()[0], 'an unknown name')
        bad = span('frobnicate')
        assert all(c['flags'] & BOLD for c in bad) and bad[0]['fg'] not in (DEFAULT, ls[0]['fg']), ('red, bold', bad)
        clear_line()

        type_text('cat read')
        eventually(lambda: 'cat read' in typed()[0], 'a word on its way to a file')
        assert all(c['flags'] & UNDERLINE for c in span('read')), 'readme.txt is there: read is on its way to it'
        d.snap('highlight-path')
        type_text(' ')
        eventually(lambda: not span('read')[0]['flags'] & UNDERLINE, 'once the word is done, it names nothing')
        print('PASS: a suggestion from history taken with Right, commands green, a directory underlined, quoted '
              'text and a comment, an unknown name red and bold, a word on its way to a file', flush=True)
    except Exception:
        try:
            d.snap('last')
            print('\n'.join(l.rstrip() for l in screen.text(w).split('\n')[-14:]), flush=True)
        except Exception as e:
            print('no last capture:', e)
        raise
    finally:
        d.g.close()


if __name__ == '__main__':
    main()
