#!/usr/bin/env python3
"""BIOS/UEFI hardware checks on disposable or explicitly retained disk images."""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time
import tempfile
from concurrent.futures import ThreadPoolExecutor

root = Path(__file__).resolve().parents[1]
test_command = os.environ.get('EMBLINK_SMOKE_COMMAND', 'test posix')
wait_desktop = (test_command.startswith(('test power', 'test persist', 'test devices'))
                or test_command in ('poweroff', 'reboot', 'test posix'))
success = os.environ.get('EMBLINK_SMOKE_EXPECT', '[cmd] test posix: exit=0 -> OK').encode()
out = Path(tempfile.mkdtemp(prefix='emblink-hardware-'))
print(f'Logs: {out}', flush=True)
out.mkdir(exist_ok=True)
serial = out / 'smoke-serial.sock'
serial.unlink(missing_ok=True)
scratch = out / 'smoke-data.img'
uefi = os.environ.get('EMBLINK_FIRMWARE') == 'uefi'
source_image = root / ('uefi-usb.img' if uefi else 'embkfs.img')
if os.environ.get('EMBLINK_PERSIST_IMAGE'):
    scratch = Path(os.environ['EMBLINK_PERSIST_IMAGE']).resolve()
    if scratch == source_image.resolve():
        raise ValueError('use a disposable persistence image, not the build image')
if not scratch.exists():
    shutil.copyfile(source_image, scratch)
command = [
    'qemu-system-x86_64', '-cpu', 'max', '-m', '1024', '-smp', os.environ.get('EMBLINK_CPUS', '1'),
    '-accel', 'tcg,thread=multi', '-display', 'none',
    '-vga', 'none', '-device', 'virtio-vga,xres=800,yres=600',
    '-serial', f'unix:{serial},server=on,wait=off',
    '-gdb', f'unix:{out / "smoke-gdb.sock"},server=on,wait=off',
    '-no-reboot', '-no-shutdown',
]
if os.environ.get('EMBLINK_LIVE', '1') == '1':
    command += ['-fw_cfg', 'name=opt/emblink/live,string=1']
if uefi:
    firmware = Path(os.environ.get('EMBLINK_OVMF_DIR', '/usr/share/OVMF'))
    shutil.copyfile(firmware / 'OVMF_VARS_4M.fd', out / 'vars.fd')
    command += ['-drive', f'if=pflash,format=raw,readonly=on,file={firmware / "OVMF_CODE_4M.fd"}',
                '-drive', f'if=pflash,format=raw,file={out / "vars.fd"}']
else:
    boot_copy=out / 'boot.img'
    shutil.copyfile(root / 'myos.img',boot_copy)
    command += ['-drive', f'format=raw,file={boot_copy},if=ide,index=0']
screenshot = os.environ.get('EMBLINK_SCREENSHOT') == '1'
power_test = os.environ.get('EMBLINK_POWER_TEST')
if power_test:
    command.remove('-no-shutdown')
if power_test or screenshot:
    command += ['-qmp', f'unix:{out / "qmp.sock"},server=on,wait=off']
if os.environ.get('EMBLINK_ACPI_TABLE'):
    command += ['-acpitable', 'file=' + os.environ['EMBLINK_ACPI_TABLE']]
audio = os.environ.get('EMBLINK_AUDIO')
if audio:
    if audio == 'hda':
        command += ['-device', 'intel-hda,id=sound0', '-device', 'hda-duplex,bus=sound0.0,audiodev=snd0']
    elif audio == 'ac97':
        command += ['-device', 'AC97,audiodev=snd0']
    else:
        raise ValueError('EMBLINK_AUDIO must be hda or ac97')
    command += ['-audiodev', f'wav,id=snd0,path={out / "audio.wav"}']
nic = os.environ.get('EMBLINK_NIC')
if nic:
    command += ['-netdev', 'user,id=net0' + os.environ.get('EMBLINK_NETDEV_OPTIONS', ''), '-device', f'{nic},netdev=net0']
    command += ['-object', f'filter-dump,id=netdump,netdev=net0,file={out / "network.pcap"}']
else:
    command += ['-nic', 'none']
if os.environ.get('EMBLINK_DISK') == 'nvme':
    sector = os.environ.get('EMBLINK_NVME_SECTOR', '512')
    command += ['-drive', f'format=raw,file={scratch},if=none,id=smokedata',
                '-device', 'nvme,id=nvme0,serial=EMBLINKSMOKE',
                '-device', f'nvme-ns,drive=smokedata,bus=nvme0,nsid=1,logical_block_size={sector},physical_block_size={sector}']
elif os.environ.get('EMBLINK_DISK') == 'ahci':
    command += ['-device', 'ich9-ahci,id=sata',
                '-drive', f'format=raw,file={scratch},if=none,id=smokedata',
                '-device', 'ide-hd,drive=smokedata,bus=sata.0']
else:
    command += ['-drive', f'format=raw,file={scratch},if=ide,index=1']
if os.environ.get('EMBLINK_NO_HPET') == '1':
    command += ['-machine', 'hpet=off']
if os.environ.get('EMBLINK_TRACE_IO') == '1':
    command += ['-trace', f'enable=ide_bus_exec_cmd,file={out / "smoke-io.trace"}']
with (out / 'smoke-qemu.log').open('wb') as errors:
    process = subprocess.Popen(command, cwd=root, stdout=errors, stderr=errors)
    connection = socket.socket(socket.AF_UNIX)
    try:
        deadline = time.monotonic() + 180
        while True:
            if process.poll() is not None:
                raise RuntimeError('QEMU exited; see smoke-qemu.log')
            try:
                connection.connect(str(serial))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() >= deadline:
                    raise TimeoutError('serial socket did not appear')
                time.sleep(0.1)
        if power_test or screenshot:
            import json
            import threading
            qmp = socket.socket(socket.AF_UNIX)
            qmp.connect(str(out / 'qmp.sock'))
            qmp_file = qmp.makefile('rwb', buffering=0)
            qmp_file.readline()
            qmp_file.write(b'{"execute":"qmp_capabilities"}\n')
            def qmp_events():
                with (out / 'qmp-events.log').open('wb') as events:
                    while True:
                        line = qmp_file.readline()
                        if not line:
                            break
                        events.write(line)
                        events.flush()
            qmp_reader = threading.Thread(target=qmp_events, daemon=True)
            qmp_reader.start()
        connection.settimeout(1)
        seen = b''
        sent = False
        transferred = False
        def verify_power_exit():
            if process.wait(timeout=10) or not (b'power: flushed ' in seen and b'block device cache(s) flushed' in seen):
                raise RuntimeError('guest exited without storage writeback and device flush markers')
            qmp_reader.join(timeout=2)
            expected_reason = 'guest-reset' if power_test == 'reboot' else 'guest-shutdown'
            if expected_reason not in (out / 'qmp-events.log').read_text():
                raise RuntimeError(f'missing QMP power reason {expected_reason}')
            print(f'PASS: {expected_reason} after storage writeback and device flush')
        with (out / 'smoke-serial.log').open('wb') as log:
            while time.monotonic() < deadline:
                if power_test and process.poll() is not None:
                    verify_power_exit()
                    break
                if transferred and http_result.done():
                    http_result.result()
                try:
                    chunk = connection.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    if power_test:
                        verify_power_exit()
                        break
                    raise RuntimeError('guest serial connection closed')
                log.write(chunk)
                log.flush()
                seen += chunk
                if b'EmbBoot FATAL:' in seen:
                    raise RuntimeError('UEFI loader failed; see smoke-serial.log')
                if test_command.startswith('test devices') and b'[cmd] test devices: exit=' in seen and b'-> FAIL' in seen:
                    raise RuntimeError('device ABI acceptance failed; see smoke-serial.log')
                if (not sent and b'init: launched /system/bin/init.elf' in seen
                        and (not wait_desktop or (b'TopBar: first frame presented' in seen
                                                 and b'home: time zone' in seen))):
                    connection.sendall((test_command + '\n').encode())
                    sent = True
                    deadline = time.monotonic() + 240
                if (os.environ.get('EMBLINK_HTTP_VERIFY') == '1' and not transferred
                        and b'synthetic bytes' in seen):
                    # Keep draining serial while TCP runs: a full UART socket
                    # otherwise stalls guest console writers inside net locks.
                    def verify_http():
                        time.sleep(1)  # let the guest return from its readiness printf
                        data = subprocess.check_output([
                            'curl', '--noproxy', '*', '--fail', '--silent', '--show-error',
                            '--max-time', '30', 'http://127.0.0.1:18080/'])
                        expected = bytes(ord('A') + i % 26 for i in range(65536))
                        if data != expected:
                            raise RuntimeError(f'HTTP payload mismatch: {len(data)} bytes')
                    http_pool = ThreadPoolExecutor(max_workers=1)
                    http_result = http_pool.submit(verify_http)
                    transferred = True
                if transferred and http_result.done():
                    http_result.result()
                if not power_test and success in seen:
                    if screenshot:
                        if b'first frame presented' not in seen:
                            continue
                        request={'execute':'screendump','arguments':{'filename':str(out / 'screen.ppm')},'id':'screenshot'}
                        qmp.sendall((json.dumps(request)+'\n').encode())
                        screen_deadline=time.monotonic()+5
                        while True:
                            lines=(out / 'qmp-events.log').read_text().splitlines()
                            replies=[json.loads(line) for line in lines if line.strip()]
                            reply=next((r for r in replies if r.get('id')=='screenshot'),None)
                            if reply is not None:
                                if 'error' in reply:
                                    raise RuntimeError(reply['error'])
                                break
                            if time.monotonic()>screen_deadline:
                                raise TimeoutError('QMP screendump did not finish')
                            time.sleep(.05)
                    if os.environ.get('EMBLINK_HTTP_VERIFY') == '1':
                        if not transferred:
                            raise RuntimeError('guest exited without HTTP payload verification')
                        http_result.result(timeout=35)
                        http_pool.shutdown()
                    if test_command == 'test posix' and b'ALL PASS' not in seen:
                        raise RuntimeError('POSIX command completed without suite success marker')
                    if os.environ.get('EMBLINK_POWER_FIXTURE') == '1':
                        for reading in (b'remaining=20000 mWh (50%)', b'remaining=1250 mAh (25%)', b'remaining=unknown', b'ac0: online'):
                            if reading not in seen:
                                raise RuntimeError(f'missing firmware reading: {reading!r}')
                    print(f'PASS: {"UEFI" if uefi else "BIOS"} boot, init launch, {test_command}: {success.decode()}')
                    break
                prefix = f'[cmd] {test_command}:'.encode()
                if prefix in seen and b'\n' in seen.split(prefix, 1)[1]:
                    raise RuntimeError('guest selftest did not report success; see smoke-serial.log')
            else:
                raise TimeoutError('guest did not complete boot/POSIX test; see smoke-serial.log')
    finally:
        connection.close()
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        serial.unlink(missing_ok=True)
