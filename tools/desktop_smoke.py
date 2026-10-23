#!/usr/bin/env python3
"""Exercise real top-bar IPC and Settings sound controls in a disposable VM.
Requires QEMU and GDB. Coordinates target the current 800x600 desktop; screenshots
are retained so layout failures are reviewable. No mock device status is used.
"""
import os
from pathlib import Path
import re
import subprocess
import time
import app_shot

root = Path(__file__).resolve().parents[1]
name = 'desktop-controls'
scratch = str(root / 'build' / f'shot-{name}.img')
q, serial_path, qmp_path = app_shot.boot(name, scratch, str(root / 'build' / f'shot-{name}.wav'))
seen = bytearray()
serial = None
qmp = None
try:
    serial = app_shot.connect(serial_path)
    serial.settimeout(.1)
    qmp = app_shot.Qmp(qmp_path)
    def pump(seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                data = serial.recv(65536)
                if not data:
                    raise RuntimeError('guest serial closed')
                seen.extend(data)
            except TimeoutError:
                pass
    deadline = time.monotonic() + 120
    while b'home: desktop ready' not in seen or b'TopBar: first frame presented' not in seen:
        if time.monotonic() > deadline:
            raise TimeoutError('desktop did not start')
        pump(.5)
    socket = str(root / 'build' / f'{name}-gdb.sock')
    Path(socket).unlink(missing_ok=True)
    qmp.cmd('human-monitor-command', **{'command-line': 'gdbserver unix:' + socket + ',server=on,wait=off'})
    def inspect(expression):
        result = subprocess.run(['gdb', '-q', '-nx', '-batch', str(root/'kernel/kernel.elf'),
            '-ex', 'target remote ' + socket, '-ex', expression, '-ex', 'detach'],
            cwd=root, capture_output=True, text=True, timeout=15, check=False)
        if result.returncode:
            raise RuntimeError("GDB state check failed: " + result.stdout + result.stderr)
        return result.stdout
    def state():
        expression = ('printf "STATE %d %d %u %u\\n", '
            "'kernel/drivers/input/mouse.c'::g_x, "
            "'kernel/drivers/input/mouse.c'::g_y, "
            "'kernel/drivers/audio/audio.c'::volume, "
            "'kernel/drivers/audio/audio.c'::muted")
        output = inspect(expression)
        match = re.search(r'STATE (\d+) (\d+) (\d+) (\d+)', output)
        if not match:
            raise RuntimeError('GDB did not return device state: ' + output)
        return tuple(map(int,match.groups()))
    def brightness():
        output = inspect('printf "BRIGHTNESS %u\\n", \'kernel/acpi/display.c\'::cache.percent')
        match = re.search(r'BRIGHTNESS (\d+)',output)
        if not match:
            raise RuntimeError('GDB did not return brightness: ' + output)
        return int(match.group(1))
    def mouse(x, y, click=True):
        current_x, current_y, _, _ = state()
        deadline = time.monotonic() + 20
        while current_x != x or current_y != y:
            if time.monotonic() > deadline:
                raise TimeoutError("pointer did not reach the requested control")
            dx = max(-80,min(80,x-current_x))
            dy = max(-80,min(80,y-current_y))
            qmp.cmd('human-monitor-command', **{'command-line': f'mouse_move {dx} {dy}'})
            pump(.1)
            current_x, current_y, _, _ = state()
        if click:
            qmp.cmd('human-monitor-command', **{'command-line':'mouse_button 1'})
            pump(.15)
            qmp.cmd('human-monitor-command', **{'command-line':'mouse_button 0'})
            pump(.5)
    before = str(root/'build'/f'shot-{name}-desktop.ppm')
    qmp.screendump(before)
    w,h,_ = app_shot.read_ppm(before)
    if (w,h) != (800,600):
        raise RuntimeError(f'expected 800x600 test layout, got {w}x{h}')
    print('desktop controls: desktop ready', flush=True)
    mouse(75,13)
    qmp.screendump(str(root/'build'/f'shot-{name}-menu.ppm'))
    mouse(90,65)
    pump(3)
    if b'home: spawn /data/apps/settings/settings.elf' not in seen or b'Settings: first frame presented' not in seen:
        raise RuntimeError('top-bar Settings action did not launch through the desktop')
    print('desktop controls: top bar -> desktop -> confined Settings OK',flush=True)
    mouse(88,160)
    pump(1)
    qmp.screendump(str(root/'build'/f'shot-{name}-sound-before.ppm'))
    if state()[2:] != (100,0):
        raise RuntimeError('unexpected initial sound state')
    mouse(744,263)
    pump(1)
    if state()[3] != 1:
        raise RuntimeError('Settings mute did not reach the PCM sink')
    qmp.screendump(str(root/'build'/f'shot-{name}-muted.ppm'))
    mouse(744,263)
    pump(1)
    if state()[3]:
        raise RuntimeError('Settings unmute did not reach the PCM sink')
    mouse(500,232)
    pump(1)
    if not 45 <= state()[2] <= 55:
        raise RuntimeError('Settings slider did not reach the PCM sink')
    qmp.screendump(str(root/'build'/f'shot-{name}-sound.ppm'))
    mouse(88,182)
    pump(1)
    if brightness()!=60:
        raise RuntimeError('unexpected fixture brightness')
    mouse(500,332)
    pump(1)
    if brightness()!=40:
        raise RuntimeError('Settings brightness slider did not reach firmware')
    qmp.screendump(str(root/'build'/f'shot-{name}-display.ppm'))
    mouse(88,205)
    pump(1)
    qmp.screendump(str(root/'build'/f'shot-{name}-power.ppm'))
    print('desktop controls: mute, unmute, volume, firmware brightness and power navigation OK',flush=True)
finally:
    if qmp:
        try:
            qmp.screendump(str(root/'build'/f'shot-{name}-last.ppm'))
        except Exception:
            pass
    (root/'build'/f'shot-{name}-serial.log').write_bytes(seen)
    if serial:
        serial.close()
    q.terminate()
    try:
        q.wait(timeout=10)
    except subprocess.TimeoutExpired:
        q.kill()
        q.wait(timeout=10)
