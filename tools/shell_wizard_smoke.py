#!/usr/bin/env python3
"""`prompt configure`, live: Powerlevel10k's wizard on EmbLinkOS, from the
first time a person opens the Terminal to the prompt it leaves them with.

A normal boot of a fresh image, driven over QMP like a person would: the
first-boot setup makes an account, the account logs in, and the Terminal is
opened. Its shell finds no prompt configuration, so the wizard starts by
itself -- the greeting says why -- on the alternate screen. The answers are the
ones in the header of the configuration on the Mac this system is modelled on
(powerline, lean, 12-hour time, two lines, a left frame, darkest ornaments,
sparse, concise, transient), page by page; each page is captured. Then:

  - the prompt is that configuration's (the frame, the clock on the right)
  - ~/.config/shell/p10k.zsh and prompt.esh are written
  - a new tab -- a new shell -- reads prompt.esh: the same prompt, no wizard
  - `prompt configure` again greets as p10k configure does, and q leaves it
    saying how to run it again

    python3 tools/shell_wizard_smoke.py
"""
from pathlib import Path
import os, re, shutil, tempfile, time
from recovery_smoke import ROOT
from workspace_smoke import Desktop
from native_terminal import TerminalState
from lock_test import keyname

# the page each answer is typed on, and the answer
ANSWERS = [('diamond', 'y'), ('a lock?', 'n'), ("Let's try another one.", 'n'), ('Prompt Style', '1'),
           ('Character Set', '1'), ('Prompt Colors', '1'), ('Show current time?', '1'), ('Prompt Height', '2'),
           ('Prompt Connection', '1'), ('Prompt Frame', '2'), ('Frame Color', '4'), ('Prompt Spacing', '2'),
           ('Prompt Flow', '1'), ('Enable Transient Prompt?', 'y')]


def main():
    out = Path(tempfile.mkdtemp(prefix='shell-wizard-', dir='/workspace/.setup' if os.path.isdir('/workspace/.setup') else None))
    print(out, flush=True)
    disk = out / 'live.img'
    shutil.copyfile(ROOT / 'uefi-usb.img', disk)
    d = Desktop(out / 'boot', disk)

    def eventually(check, message, seconds=60):
        end = time.monotonic() + seconds
        while not check():
            if time.monotonic() > end:
                raise AssertionError(message)
            d.g.pump(.4)

    def type_text(value):
        for c in value:
            d.key(keyname(c))

    def text():
        return screen.text(w)

    def run(line, what):
        serial = screen.probe(w, text=False)['prompt_serial']
        type_text(line + '\n')
        eventually(lambda: screen.at_prompt(w) and screen.probe(w, text=False)['prompt_serial'] != serial, what)

    def rows():
        return [l.rstrip() for l in text().split('\n')]

    try:
        d.login(setup=True)
        screen = TerminalState(d)
        w = d.launch('Terminal', 'Terminal')
        # no prompt configuration: the wizard, by itself
        eventually(lambda: 'You are seeing it because' in text(), 'the wizard starts by itself in a new shell', 90)
        assert screen.probe(w, text=False)['alt'], 'the wizard draws on the alternate screen'
        for i, (page, answer) in enumerate(ANSWERS):
            eventually(lambda: page in text() and 'Choice [' in text(), f'page {i + 1}: {page}')
            d.g.pump(.5)
            d.snap(f'wizard-{i + 1:02d}')
            print(f'page {i + 1}: {page} -> {answer}', flush=True)
            type_text(answer)
            if i + 1 < len(ANSWERS):
                eventually(lambda: page not in text() or ANSWERS[i + 1][0] in text(), f'past page {i + 1}')
        # done: the main screen again, and the prompt the answers made
        eventually(lambda: screen.at_prompt(w), 'a prompt after the wizard', 60)
        main_text = text()
        assert 'New config: ~/.config/shell/p10k.zsh.' in main_text, main_text
        assert not screen.probe(w, text=False)['alt'], 'the wizard left the alternate screen'
        r = rows()
        y = screen.probe(w, text=False)['cursor'][1]
        assert r[y].startswith('\u2570\u2500\u276f'), ('the last line: the frame and the prompt character', r[y])
        assert r[y - 1].startswith('\u256d\u2500') and re.search(r'\d\d:\d\d:\d\d [AP]M$', r[y - 1]), \
            ('the first line: the frame, the directory, the 12-hour clock on the right', r[y - 1])
        d.snap('wizard-done')
        print('prompt:', repr(r[y - 1]), repr(r[y]), flush=True)
        # what it wrote, and what loads it
        run('cat ".config/shell/prompt.esh"', 'prompt.esh')
        esh = text()
        assert 'Wizard options: powerline, unicode, lean, 12h time, 2 lines, disconnected,' in esh, esh
        assert 'let p10k = $(prompt import-p10k "~/.config/shell/p10k.zsh")' in esh, esh
        run('clear', 'clear')
        run('prompt style', 'the base')
        assert '/home/tester/.config/shell/p10k.zsh' in text(), text()
        # a new shell reads prompt.esh: the same prompt, no wizard
        d.key('meta_l-t')
        eventually(lambda: screen.probe(w)['tabs'] == 2 and screen.at_prompt(w), 'a second tab')
        r = rows()
        y = screen.probe(w, text=False)['cursor'][1]
        assert 'You are seeing it because' not in text() and r[y].startswith('\u2570\u2500\u276f'), r[max(0, y - 3):y + 1]
        d.snap('wizard-new-tab')
        # prompt configure, typed: p10k configure's greeting; q leaves it
        serial = screen.probe(w, text=False)['prompt_serial']
        type_text('prompt configure\n')
        eventually(lambda: 'It will ask you a few questions' in text() and 'Choice [' in text(), 'prompt configure')
        assert 'You are seeing it because' not in text()
        d.snap('wizard-configure')
        type_text('q')
        eventually(lambda: screen.at_prompt(w) and screen.probe(w, text=False)['prompt_serial'] != serial, 'q')
        assert 'Powerlevel10k configuration wizard has been aborted. To run it again, type:' in text(), text()
        assert 'prompt configure' in text()
        d.snap('wizard-quit')
        print('PASS: the wizard ran by itself in the first shell, its answers made the Mac\'s prompt, prompt.esh '
              'loads it in a new shell, and prompt configure greets and quits as p10k configure does', flush=True)
    except Exception:
        try:
            d.snap('last')
            print('\n'.join(rows()[-20:]), flush=True)
        except Exception as e:
            print('no last capture:', e)
        raise
    finally:
        d.g.close()


if __name__ == '__main__':
    main()
