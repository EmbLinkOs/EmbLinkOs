#!/usr/bin/env python3
"""login_test.py -- the login path, end to end, typed on the real keyboard.

Boots a SCRATCH COPY of the image (the account store it creates must not leak
into the next run), and then, from the kernel console and QEMU's keyboard:

  1. the development auto-login opens yves's session; the console ends it --
     a logout -- and init must go to the REAL flow, not log yves back in
  2. the image has no accounts, so first-boot setup appears: an account is
     typed in (name, Tab, password, Tab, confirm, Return)
  3. the greeter appears: a WRONG password is refused, and the console says so
  4. the right one opens tester's session, as an administrator
  5. the kernel lists the session under tester's name
  6. logging tester out returns to the greeter -- setup does not run again

Every key goes through QMP `sendkey` into the PS/2 controller, the path a
person's keyboard takes. Exit status = the number of steps that failed.
"""
import os, sys, time, signal, subprocess, threading, socket, json, random, re, shutil, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

KEYS = {' ': 'spc', '\t': 'tab', '\n': 'ret', '-': 'minus', '_': 'shift-minus', '.': 'dot'}
def keyname(c):
    if c in KEYS: return KEYS[c]
    if c.isdigit() or ('a' <= c <= 'z'): return c
    if 'A' <= c <= 'Z': return 'shift-' + c.lower()
    raise ValueError("no key for %r" % c)

def main():
    if subprocess.run(["pgrep", "-f", "qemu-system"], capture_output=True).returncode == 0:
        print("login_test: another qemu-system is running; refusing to start a second one", file=sys.stderr)
        return 2
    scratch = tempfile.mkdtemp(prefix="login_test.")
    disk = os.path.join(scratch, "embkfs.img")
    shutil.copyfile(os.path.join(ROOT, "embkfs.img"), disk)
    qmp_port = 4900 + random.randint(0, 400)
    argv = ["qemu-system-x86_64", "-cpu", "max",
            "-drive", "format=raw,file=%s/myos.img,if=ide,index=0" % ROOT,
            "-drive", "format=raw,file=%s,if=ide,index=1" % disk,
            "-serial", "stdio", "-no-reboot", "-no-shutdown", "-m", "2G", "-smp", "4",
            "-display", "none", "-qmp", "tcp:127.0.0.1:%d,server,nowait" % qmp_port]
    p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, bufsize=0, cwd=ROOT)
    buf = bytearray(); lock = threading.Lock()
    def reader():
        while True:
            b = p.stdout.read(1)
            if not b: break
            sys.stdout.buffer.write(b); sys.stdout.flush()
            with lock: buf.extend(b)
    threading.Thread(target=reader, daemon=True).start()

    qmp = {}
    def hmp(cmd):
        if 'f' not in qmp:
            for _ in range(50):
                try:
                    sk = socket.create_connection(("127.0.0.1", qmp_port), timeout=3.0); break
                except OSError: time.sleep(0.2)
            f = sk.makefile("rw"); f.readline()
            f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()
            qmp['f'] = f
        f = qmp['f']
        f.write(json.dumps({"execute": "human-monitor-command", "arguments": {"command-line": cmd}}) + "\n")
        f.flush()
        while True:                                  # skip async events
            r = json.loads(f.readline())
            if 'return' in r or 'error' in r: return r

    def type_text(s):
        for c in s:
            hmp("sendkey " + keyname(c))
            time.sleep(0.12)

    mark = [0]
    def wait_for(needle, secs):
        dl = time.time() + secs; nb = needle.encode()
        while time.time() < dl:
            with lock:
                i = buf.find(nb, mark[0])
                if i >= 0:
                    mark[0] = i + len(nb); return True
            time.sleep(0.05)
        return False
    def console(cmd):
        p.stdin.write((cmd + "\n").encode()); p.stdin.flush()
    def text_since(start):
        with lock: return bytes(buf[start:]).decode("utf-8", "replace")

    fails = 0
    shots = os.environ.get("SHOTS")                  # a directory: screenshots on failure
    def step(ok, what):
        nonlocal fails
        print("\nlogin_test: [%s] %s" % ("ok" if ok else "FAIL", what), file=sys.stderr)
        if not ok:
            fails += 1
            if shots:
                path = os.path.join(shots, "login_test_fail%d.ppm" % fails)
                hmp("screendump " + path)
                print("login_test: screen saved to %s" % path, file=sys.stderr)
        return ok

    try:
        if not step(wait_for("TopBar: first frame presented", 240), "the auto-login desktop came up"):
            return 1
        time.sleep(1.0)
        start = len(buf); console("sessions")
        time.sleep(1.5)
        m = re.search(r"session (\d+)\s+user yves", text_since(start))
        if not step(m is not None, "the kernel lists yves's session"):
            return fails
        console("session end %s" % m.group(1))
        step(wait_for("init: 'yves' logged out -- returning to the login screen", 30),
             "ending the session is a LOGOUT: init does not log yves back in")

        if step(wait_for("first-boot setup started", 30) and wait_for("EmbLink OS Setup: first frame presented", 60),
                "no accounts on this image: first-boot setup appeared"):
            time.sleep(2.0)
            type_text("tester\tcorrect horse 1\tcorrect horse 1\n")
            step(wait_for("setup: account 'tester' created (administrator)", 120),
                 "an account typed into setup (Tab between fields, Return to finish) was created, as the administrator")

        if step(wait_for("init: login screen started", 60) and wait_for("EmbLink OS Login: first frame presented", 60),
                "the greeter appeared"):
            # A hostile profile for tester, planted where only the greeter and
            # init can write -- the clamp must hold even if one gets there.
            console("test plant profile tester")
            step(wait_for("[cmd] test plant profile: OK", 30), "a hostile session profile was planted for tester")
            time.sleep(2.0)
            type_text("tester\twrong password\n")
            step(wait_for("login: sign-in refused for 'tester' (1 in a row)", 120),
                 "a wrong password is refused, and the console records it (the name, not the password)")
            time.sleep(1.0)
            type_text("correct horse 1\n")          # focus is still in the password field
            start = len(buf)
            ok = wait_for("init: authenticated session 'tester' (administrator)", 120)
            step(ok, "the right password opens tester's session")
            t = text_since(start)
            step("asks for rw /etc -- not granted" in t and "asks for rw /home -- not granted" in t and
                 "ns[ro /system, ro /data/apps, rw /home/tester, rw /run]" in t,
                 "init refused the profile's rw /etc and rw /home, and granted only what the policy allows")

        if step(wait_for("TopBar: first frame presented", 120), "tester's desktop came up"):
            time.sleep(1.0)
            start = len(buf); console("sessions")
            time.sleep(1.5)
            m = re.search(r"session (\d+)\s+user tester", text_since(start))
            if step(m is not None, "the kernel lists the session under tester's name"):
                start = len(buf)
                console("session end %s" % m.group(1))
                ok = wait_for("init: 'tester' logged out -- returning to the login screen", 30) and \
                     wait_for("init: login screen started", 30)
                step(ok and "first-boot setup" not in text_since(start),
                     "logging tester out returns to the greeter, not to setup")
    finally:
        p.send_signal(signal.SIGTERM)
        try: p.wait(5)
        except Exception: p.kill()
        shutil.rmtree(scratch, ignore_errors=True)
    print("\nlogin_test: %d step(s) failed" % fails, file=sys.stderr)
    return fails

if __name__ == "__main__":
    sys.exit(main())
