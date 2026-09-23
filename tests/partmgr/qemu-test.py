#!/usr/bin/env python3
"""partmgr.efi inside QEMU/OVMF: the program as firmware runs it.

Boots NESH from a virtual FAT disk that also holds partmgr.efi, attaches two
test disks made with sfdisk (a GPT one and an MBR one with logical
partitions), starts partmgr from the NESH prompt and drives it with keys sent
through the QEMU monitor. What partmgr draws reaches the serial console too;
the test waits there for the lines it expects: the disks numbered as NESH's
map numbers them, the disk partmgr was started from marked read-only, the
partitions and free areas of each disk, and the return to the NESH prompt.

    tests/partmgr/qemu-test.py OVMF.fd build/nesh.efi build/partmgr.efi
"""
import hashlib
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

OVMF, NESH, PARTMGR = sys.argv[1:4]
SFDISK = shutil.which("sfdisk") or "/sbin/sfdisk"
WORK = tempfile.mkdtemp(prefix="pmqemu-")
failures = []
count = 0
KEYS = {' ': 'spc', '\n': 'ret', '\\': 'backslash', '.': 'dot', ':': 'shift-semicolon'}


def check(name, cond, detail=""):
    global count
    count += 1
    if not cond:
        failures.append("%s: %s" % (name, detail))


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def disk(name, mb, script):
    p = os.path.join(WORK, name)
    with open(p, "wb") as f:
        f.truncate(mb * 1024 * 1024)
    subprocess.run([SFDISK, "-q", p], input=script.encode(), check=True, stdout=subprocess.DEVNULL)
    return p


class Qemu:
    def __init__(self, disks):
        esp = os.path.join(WORK, "esp")
        os.makedirs(os.path.join(esp, "EFI", "BOOT"))
        shutil.copy(NESH, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
        shutil.copy(PARTMGR, os.path.join(esp, "partmgr.efi"))
        self.log = os.path.join(WORK, "serial.log")
        mon = os.path.join(WORK, "monitor")
        drives = ["-drive", "if=virtio,format=raw,readonly=on,file=fat:" + esp]
        for d in disks:
            drives += ["-drive", "if=virtio,format=raw,file=" + d]
        kvm = ["-accel", "kvm"] if os.access("/dev/kvm", os.W_OK) else []
        self.proc = subprocess.Popen(
            ["qemu-system-x86_64"] + kvm + ["-m", "512", "-machine", "q35", "-bios", OVMF] + drives +
            ["-net", "none", "-display", "none", "-serial", "file:" + self.log,
             "-monitor", "unix:%s,server,nowait" % mon, "-no-reboot"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(300):
            if os.path.exists(mon):
                break
            time.sleep(0.1)
        self.mon = socket.socket(socket.AF_UNIX)
        self.mon.connect(mon)
        self.mon.setblocking(False)
        self.seen = 0

    def text(self):
        """The serial output so far, without the terminal control sequences."""
        try:
            raw = open(self.log, "rb").read().decode("utf-8", "replace")
        except FileNotFoundError:
            return ""
        return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "\n", raw)

    def wait(self, what, timeout=30):
        """Waits until WHAT (a regular expression) appears after the last match."""
        end = time.time() + timeout
        while time.time() < end:
            t = self.text()
            m = re.search(what, t[self.seen:])
            if m:
                self.seen += m.end()
                return True
            time.sleep(0.2)
        return False

    def key(self, name):
        self.mon.sendall(("sendkey %s\n" % name).encode())
        time.sleep(0.15)
        try:
            while self.mon.recv(65536):
                pass
        except BlockingIOError:
            pass

    def type(self, text):
        for ch in text:
            self.key(KEYS.get(ch, ch))

    def stop(self):
        self.proc.kill()
        self.proc.wait()


try:
    gpt = disk("gpt.img", 512, 'label: gpt\nsize=100M, type=U, name="EFI system"\n'
               'size=16M, type=E3C9E316-0B5C-4DB8-817D-F92DF00215AE\nsize=200M, type=L, name="Linux root"\n')
    mbr = disk("mbr.img", 512, "label: dos\n1 : start=2048, size=102400, type=c, bootable\n"
               "2 : start=104448, size=614400, type=5\n5 : start=106496, size=204800, type=83\n"
               "6 : start=397312, size=81920, type=82\n")
    before = {d: digest(d) for d in (gpt, mbr)}
    q = Qemu([gpt, mbr])
    try:
        check("NESH starts", q.wait(r"New EFI Shell", 90), "no NESH banner on the serial console")
        q.type("\nfs0:\\partmgr.efi\n")
        # screen 1: blk1 is the boot disk (the FAT one), blk3 and blk7 the test disks, as in NESH's map
        check("disk list", q.wait(r"Select a disk"), "no disk list")
        check("boot disk", q.wait(r"blk1 +disk +504\.0 MiB +MBR +1 partition +started from here: read only"),
              "the boot disk is not marked read-only")
        check("gpt disk listed", q.wait(r"blk3 +disk +512\.0 MiB +GPT +3 partitions"), "blk3 missing")
        check("mbr disk listed", q.wait(r"blk7 +disk +512\.0 MiB +MBR +3 partitions"), "blk7 missing")
        # screen 2 on the GPT disk
        q.key("down")
        q.key("ret")
        check("gpt title", q.wait(r"blk3 +disk +512\.0 MiB +GPT"), "no title")
        check("gpt partition 1", q.wait(r"1 +1\.0 MiB +100\.0 MiB +EFI system +EFI system"), "")
        check("gpt partition 2", q.wait(r"2 +101\.0 MiB +16\.0 MiB +Microsoft reserved"), "")
        check("gpt partition 3", q.wait(r"3 +117\.0 MiB +200\.0 MiB +Linux filesystem +Linux root"), "")
        check("gpt free space", q.wait(r"- +317\.0 MiB +194\.9 MiB +free space"), "")
        # screen 2 on the MBR disk
        q.key("esc")
        q.key("down")
        q.key("ret")
        check("mbr title", q.wait(r"blk7 +disk +512\.0 MiB +MBR"), "no title")
        check("mbr active", q.wait(r"1 +1\.0 MiB +50\.0 MiB +FAT32 \(LBA\) +active"), "")
        check("mbr extended", q.wait(r"2 +51\.0 MiB +300\.0 MiB +Extended"), "")
        check("mbr logical 5", q.wait(r"5 +52\.0 MiB +100\.0 MiB +Linux +logical"), "")
        check("mbr free inside", q.wait(r"- +153\.0 MiB +40\.9 MiB +free space \(for logical partitions\)"), "")
        check("mbr logical 6", q.wait(r"6 +194\.0 MiB +40\.0 MiB +Linux swap +logical"), "")
        check("mbr free outside", q.wait(r"- +351\.0 MiB +161\.0 MiB +free space *\n"), "")
        # back to the list, quit, and NESH is there again
        q.key("esc")
        q.key("q")
        q.type("ver\n")
        check("back to NESH", q.wait(r"NESH \d+\.\d+\.\d+"), "the NESH prompt did not come back")
    finally:
        q.stop()
    # this version of partmgr only reads: the disks are identical, byte for byte
    for d in (gpt, mbr):
        check("untouched", digest(d) == before[d], os.path.basename(d) + " changed")
finally:
    shutil.rmtree(WORK, ignore_errors=True)

for f in failures:
    print("FAIL " + f)
print("partmgr qemu tests: %d checks, %d failed" % (count, len(failures)))
sys.exit(1 if failures else 0)
