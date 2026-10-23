#!/usr/bin/env python3
"""tools/laptop_test.py -- boot the USB stick the way a laptop boots it.

WHY THIS EXISTS. `make test-uefi` attaches uefi-usb.img as an IDE DISK. A
laptop reads it over USB, through xHCI, with the root filesystem on the stick;
it has no serial port, usually no HPET, and on a recent Intel part an 8254 PIT
that is clock-gated and never counts. The first time that shape was booted --
before anyone put the stick in a real machine -- it found, in one afternoon:

  * a boot that could only hang on real silicon: TSC calibration and every
    busy-wait waited, unbounded, for a PIT that is not counting;
  * the TSC measured at "~2 MHz", so every timeout was 500x too short;
  * uptime frozen at zero with no HPET, under forty-six deadlines;
  * the other cores timing their scheduler ticks against the dead PIT, 5x fast;
  * an infinite loop draining a serial port that does not exist;
  * a kernel fault that printed ONLY to that serial port, leaving the screen
    frozen on the desktop with nothing to photograph;
  * the stick's root filesystem mounted a second time, read-write;
  * Intel VT-d switched on with a 4 GiB identity map on a machine whose RAM
    runs past 4 GiB (which would have stopped the stick being readable).

So the stick is booted here three ways, each asserting what a person holding
the laptop would need to be true:

  laptop    PIT and HPET off, stick on xHCI, an NVMe "internal disk" that is
            hashed before and after -- it must not change. The log must show
            a real clock source, consistent ticks on every core, one mount of
            root, and a desktop.
  no-uart   the same with NO serial port at all and a plain firmware
            framebuffer (ramfb, which no kernel driver claims -- as on a
            laptop). Judged from the screen: the desktop's dock is drawn.
  panic     the kernel is faulted on purpose once the desktop is up. Judged
            from the screen: a red header, i.e. the panic screen and not a
            frozen desktop.
"""
import hashlib, json, os, shutil, socket, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import shell_shot as S

FW_DIRS = [os.path.join(os.path.dirname(shutil.which("qemu-system-x86_64") or "/"), "..", "share", "qemu"),
           "/usr/share/qemu", "/usr/share/OVMF"]


def firmware():
    for d in FW_DIRS:
        code = os.path.join(d, "edk2-x86_64-code.fd")
        vars_ = os.path.join(d, "edk2-i386-vars.fd")
        if os.path.exists(code) and os.path.exists(vars_):
            return code, vars_
    code, vars_ = os.environ.get("OVMF_CODE"), os.environ.get("OVMF_VARS")
    if code and vars_:
        return code, vars_
    raise SystemExit("laptop_test: no OVMF firmware found (set OVMF_CODE / OVMF_VARS)")


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def ink(path, band, thresh=110):
    x0, y0, x1, y1 = band
    w, h, px = S.ppm_pixels(path)
    n = 0
    for y in range(y0, min(y1, h)):
        for x in range(x0, min(x1, w)):
            o = (y * w + x) * 3
            if px[o] > thresh and px[o + 1] > thresh and px[o + 2] > thresh:
                n += 1
    return n


def mean_rgb(path, band):
    x0, y0, x1, y1 = band
    w, h, px = S.ppm_pixels(path)
    r = g = b = n = 0
    for y in range(y0, min(y1, h)):
        for x in range(x0, min(x1, w)):
            o = (y * w + x) * 3
            r += px[o]; g += px[o + 1]; b += px[o + 2]; n += 1
    return (r / n, g / n, b / n) if n else (0, 0, 0)


class Machine:
    def __init__(self, tag, work, serial=True, display="VGA", console_cmd=None):
        self.tag, self.work = tag, work
        code, vars_src = firmware()
        self.stick = os.path.join(work, "stick-%s.img" % tag)
        shutil.copyfile(os.path.join(ROOT, "uefi-usb.img"), self.stick)
        vars_ = os.path.join(work, "vars-%s.fd" % tag)
        shutil.copyfile(vars_src, vars_)
        # TWO internal disks, because laptops ship both: NVMe on anything
        # recent, SATA behind AHCI on the rest. Both are hashed; both must come
        # out of the boot byte-for-byte as they went in.
        self.internal = os.path.join(work, "internal-%s.img" % tag)
        self.sata = os.path.join(work, "sata-%s.img" % tag)
        for path in (self.internal, self.sata):
            with open(path, "wb") as f:
                f.write(os.urandom(32 * 1024 * 1024))
        self.internal_sha = sha(self.internal)
        self.sata_sha = sha(self.sata)
        self.qmp_path = "/tmp/embk-lt-qmp-%d-%s.sock" % (os.getpid(), tag)
        self.ser_path = "/tmp/embk-lt-ser-%d-%s.sock" % (os.getpid(), tag)
        for p in (self.qmp_path, self.ser_path):
            if os.path.exists(p):
                os.remove(p)
        argv = ["qemu-system-x86_64", "-machine", "q35,pit=off,hpet=off", "-cpu", "max",
                "-smp", "4", "-m", "4G", "-nodefaults", "-no-reboot", "-display", "none",
                "-drive", "if=pflash,format=raw,readonly=on,file=%s" % code,
                "-drive", "if=pflash,format=raw,file=%s" % vars_,
                "-device", display,
                "-device", "qemu-xhci,id=xhci",
                "-drive", "if=none,id=stick,format=raw,file=%s" % self.stick,
                "-device", "usb-storage,bus=xhci.0,drive=stick,removable=on,bootindex=1",
                "-device", "usb-kbd,bus=xhci.0", "-device", "usb-tablet,bus=xhci.0",
                "-drive", "if=none,id=ssd,format=raw,file=%s" % self.internal,
                "-device", "nvme,serial=laptopssd,drive=ssd",
                "-device", "ahci,id=ahci",
                "-drive", "if=none,id=sata,format=raw,file=%s" % self.sata,
                "-device", "ide-hd,drive=sata,bus=ahci.0",
                "-qmp", "unix:%s,server,nowait" % self.qmp_path]
        if serial:
            argv += ["-serial", "unix:%s,server" % self.ser_path]
        self.log = bytearray()
        self.p = subprocess.Popen(argv, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        self.ser = None
        if serial:
            self.ser = self._connect(self.ser_path)
            threading.Thread(target=self._drain, daemon=True).start()
        q = self._connect(self.qmp_path)
        self.f = q.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def _connect(self, path):
        for _ in range(200):
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(path)
                return s
            except OSError:
                time.sleep(0.1)
        raise SystemExit("laptop_test: QEMU never opened %s" % path)

    def _drain(self):
        while True:
            try:
                d = self.ser.recv(4096)
            except OSError:
                return
            if not d:
                return
            self.log.extend(d)

    def text(self):
        return bytes(self.log).decode("utf-8", "replace")

    def wait_for(self, needle, secs):
        dl = time.time() + secs
        while time.time() < dl:
            if needle in self.text():
                return True
            if self.p.poll() is not None:
                return False
            time.sleep(0.5)
        return False

    def cmd(self, c, **a):
        msg = {"execute": c}
        if a:
            msg["arguments"] = a
        self.f.write(json.dumps(msg) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "event" not in r:
                return r

    def screendump(self, name):
        path = os.path.join(self.work, "%s-%s.ppm" % (self.tag, name))
        if os.path.exists(path):
            os.remove(path)
        self.cmd("screendump", filename=path)
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                break
            time.sleep(0.2)
        return path

    def send(self, line):
        self.ser.sendall((line + "\n").encode())

    def stop(self):
        try:
            self.cmd("quit")
        except Exception:
            pass
        try:
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()
        for p in (self.qmp_path, self.ser_path):
            if os.path.exists(p):
                os.remove(p)

    def internal_untouched(self):
        return sha(self.internal) == self.internal_sha and sha(self.sata) == self.sata_sha


def main():
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("laptop_test: another qemu-system is running; refusing to start a second one")
        return 2
    if not os.path.exists(os.path.join(ROOT, "uefi-usb.img")):
        print("laptop_test: uefi-usb.img is missing (make uefi-usb.img)")
        return 2

    work = tempfile.mkdtemp(prefix="laptop_test.")
    fails = []

    def check(ok, what):
        print("laptop_test: [%s] %s" % ("ok" if ok else "FAIL", what))
        if not ok:
            fails.append(what)

    try:
        # ---- laptop: the log ------------------------------------------------
        m = Machine("laptop", work)
        try:
            up = m.wait_for("home: desktop ready", 240)
            check(up, "laptop: the desktop came up from a USB stick with no PIT and no HPET")
            t = m.text()
            tsc = [l for l in t.splitlines() if l.startswith("TSC: ")]
            check(bool(tsc) and "GUESSED" not in tsc[0],
                  "laptop: the TSC's frequency came from a real reference (%s)"
                  % (tsc[0] if tsc else "no TSC line"))
            ticks = []
            for l in t.splitlines():
                if l.startswith("LAPIC timer: ") and " ticks in 10ms" in l:
                    try:
                        ticks.append(int(l.split()[2]))
                    except ValueError:
                        pass
            spread = (max(ticks) / min(ticks)) if ticks and min(ticks) else 0
            check(len(ticks) >= 4 and spread < 2.0,
                  "laptop: every core's scheduler tick agrees within 2x (%d cores, spread %.2f)"
                  % (len(ticks), spread))
            check("is EMBKFS -- mounted at /media/" not in t,
                  "laptop: the stick's root filesystem is mounted once, not again at /media")
            check("Exception" not in t and "halted" not in t, "laptop: no kernel fault")
        finally:
            m.stop()
        check(m.internal_untouched(), "laptop: neither internal disk (NVMe, SATA) was written")

        # ---- no-uart: the screen --------------------------------------------
        m = Machine("nouart", work, serial=False, display="ramfb")
        try:
            time.sleep(140)
            shot = m.screendump("desktop")
            w, h, _ = S.ppm_pixels(shot)
            # the dock pill: bottom-centre of whatever resolution the firmware chose
            band = (w // 2 - 120, h - 80, w // 2 + 120, h - 20)
            dock = ink(shot, band)
            check(dock > 150, "no-uart: with no serial port at all, the desktop's dock is drawn "
                              "(%d lit pixels at %dx%d)" % (dock, w, h))
        finally:
            m.stop()
        check(m.internal_untouched(), "no-uart: neither internal disk (NVMe, SATA) was written")

        # ---- panic: the screen after a fault --------------------------------
        m = Machine("panic", work)
        try:
            up = m.wait_for("home: desktop ready", 240)
            check(up, "panic: the desktop came up")
            time.sleep(4)
            m.send("test panicnow")
            check(m.wait_for("kernel-mode fault -- system halted", 60), "panic: the kernel faulted on request")
            time.sleep(6)                       # long enough for any other core to repaint
            shot = m.screendump("panic")
            w, h, _ = S.ppm_pixels(shot)
            r, g, b = mean_rgb(shot, (0, 0, w, 40))
            check(r > 90 and g < 60 and b < 60,
                  "panic: the screen shows the panic header, not the desktop (top band RGB %.0f/%.0f/%.0f)"
                  % (r, g, b))
        finally:
            m.stop()
    finally:
        if not fails:
            shutil.rmtree(work, ignore_errors=True)
        else:
            print("laptop_test: screenshots and images kept in %s" % work)

    print("=== test-laptop: %s" % ("FAIL (%d)" % len(fails) if fails else "OK"))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
