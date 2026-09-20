#!/usr/bin/env python3
"""Record the animation shown in the README: boot the disk image in QEMU,
type a short session with the QEMU monitor, grab one screendump per frame
and turn the frames into a GIF.

    make usb                       # build/nesh-usb.img
    tools/record-demo.py build/nesh-usb.img docs/assets/nesh-demo.gif

Needs qemu-system-x86_64, an OVMF image, ffmpeg and Python's PIL. Nothing
in the build or the test suite depends on it: it is run by hand when the
animation has to be made again.
"""
import os, shutil, socket, subprocess, sys, tempfile, time

FPS = 6.0                 # frames per second, both captured and played
TICK = 1.0 / FPS
SCREEN = (800, 600)       # forced on the firmware with an EDID, so that the
CROP = (800, 432)         # text fills the width; the GIF keeps the used part

# what the session types; (seconds to wait before it, action)
SESSION = [
    (0.8, None),
    (0.0, "type:ver\n"),
    (2.0, None),
    (0.0, "type:map\n"),
    (2.0, None),
    (0.0, "type:ls fs0:\\examples\n"),
    (2.2, None),
    (0.0, "type:examples\\menu.nsb\n"),
    (2.6, None),
    (0.0, "key:down"),
    (0.8, "key:ret"),
    (3.0, None),
    (0.0, "key:ret"),      # dismiss the "press a key", back to the menu
    (1.6, None),
    (0.0, "key:esc"),      # leave the menu
    (1.6, None),
]

KEYS = {' ': 'spc', '\n': 'ret', '\\': 'backslash', '-': 'minus', '.': 'dot',
        ',': 'comma', '/': 'slash', ';': 'semicolon', "'": 'apostrophe',
        '=': 'equal', '[': 'bracket_left', ']': 'bracket_right',
        ':': 'shift-semicolon', '$': 'shift-4', '"': 'shift-apostrophe',
        '(': 'shift-9', ')': 'shift-0', '_': 'shift-minus', '*': 'shift-8',
        '?': 'shift-slash', '!': 'shift-1', '>': 'shift-dot', '<': 'shift-comma'}


def keyname(ch):
    if ch in KEYS:
        return KEYS[ch]
    if ch.isdigit() or 'a' <= ch <= 'z':
        return ch
    if 'A' <= ch <= 'Z':
        return 'shift-' + ch.lower()
    raise ValueError("no QEMU key name for %r" % ch)


class Monitor:
    """The QEMU monitor on a unix socket: sendkey and screendump."""

    def __init__(self, path):
        for _ in range(200):
            if os.path.exists(path):
                break
            time.sleep(0.1)
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        time.sleep(0.4)
        self.drain()

    def drain(self):
        self.s.setblocking(False)
        try:
            while self.s.recv(65536):
                pass
        except BlockingIOError:
            pass
        self.s.setblocking(True)

    def cmd(self, line):
        self.s.sendall((line + "\n").encode())
        time.sleep(0.05)
        self.drain()


def find_ovmf():
    if os.environ.get("OVMF"):
        return os.environ["OVMF"]
    for p in ("/usr/share/ovmf/OVMF.fd", "/usr/share/OVMF/OVMF.fd"):
        if os.path.exists(p):
            return p
    sys.exit("no OVMF image found: set OVMF=/path/to/OVMF.fd")


def record(img, frames, ovmf):
    tmp = tempfile.mkdtemp(prefix="nesh-demo-")
    mon_path, log = os.path.join(tmp, "mon"), os.path.join(tmp, "log")
    disk = os.path.join(tmp, "demo.img")        # the image is not modified
    shutil.copyfile(img, disk)

    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-accel", "kvm", "-m", "1024", "-machine", "q35",
        "-bios", ovmf, "-drive", "format=raw,file=" + disk,
        "-net", "none", "-display", "none", "-serial", "file:" + log,
        "-vga", "none",
        "-device", "VGA,edid=on,xres=%d,yres=%d" % SCREEN,
        "-monitor", "unix:%s,server,nowait" % mon_path, "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon = Monitor(mon_path)

    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            if "New EFI Shell" in open(log, "rb").read().decode("utf-8", "replace"):
                break
        except FileNotFoundError:
            pass
        time.sleep(0.2)
    else:
        qemu.kill()
        sys.exit("NESH did not start")
    time.sleep(1.0)

    # clear the firmware logo and the boot messages before recording; the
    # firmware also swallows the first keystroke, hence the leading newline
    for ch in "\ncls\n":
        mon.cmd("sendkey " + keyname(ch))
        time.sleep(0.12)
    time.sleep(1.0)

    n = 0

    def shot():
        nonlocal n
        mon.cmd("screendump %s/%04d.ppm" % (frames, n))
        n += 1

    for delay, action in SESSION:
        end = time.time() + delay
        while time.time() < end:
            t0 = time.time()
            shot()
            time.sleep(max(0, TICK - (time.time() - t0)))
        if action is None:
            continue
        kind, arg = action.split(":", 1)
        if kind == "key":
            mon.cmd("sendkey " + arg)
            shot()
            time.sleep(TICK)
        else:
            for ch in arg:
                mon.cmd("sendkey " + keyname(ch))
                shot()
                time.sleep(max(0, TICK - 0.05))

    qemu.kill()
    shutil.rmtree(tmp, ignore_errors=True)
    return n


def make_gif(frames, out):
    crop = "crop=%d:%d:0:0" % CROP
    palette = os.path.join(frames, "palette.png")
    src = ["-framerate", str(FPS), "-i", os.path.join(frames, "%04d.ppm")]
    subprocess.run(["ffmpeg", "-y", "-v", "error"] + src +
                   ["-vf", crop + ",palettegen=max_colors=64", palette],
                   check=True)
    subprocess.run(["ffmpeg", "-y", "-v", "error"] + src + ["-i", palette,
                   "-lavfi", crop + " [x]; [x][1:v] paletteuse=dither=none",
                    "-loop", "0", out], check=True)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: record-demo.py nesh-usb.img out.gif")
    img, out = sys.argv[1], sys.argv[2]
    frames = tempfile.mkdtemp(prefix="nesh-frames-")
    try:
        n = record(img, frames, find_ovmf())
        make_gif(frames, out)
        print("%s: %d frames, %.1f s, %d bytes"
              % (out, n, n / FPS, os.path.getsize(out)))
    finally:
        shutil.rmtree(frames, ignore_errors=True)


main()
