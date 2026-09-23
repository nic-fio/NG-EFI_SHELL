#!/usr/bin/env python3
"""partmgr.efi inside QEMU/OVMF: the program as firmware runs it.

Boots NESH from a virtual FAT disk that also holds partmgr.efi, attaches test
disks made with sfdisk (a GPT one and an MBR one with logical partitions),
starts partmgr from the NESH prompt and drives it with keys sent through the
QEMU monitor. What partmgr draws reaches the serial console too; the test
waits there for the lines it expects.

1. Looking: the disks numbered as NESH's map numbers them, the disk partmgr
   was started from marked read-only, the partitions and free areas of each
   disk, the return to the NESH prompt - and the disks unchanged, byte for
   byte.
2. Changing: the boot disk refuses changes; on the GPT disk a new partition,
   a rename and a delete, leaving with changes asks first, Write asks for the
   disk name; on the MBR disk a logical partition and the active flag, a
   backup to a file on a writable FAT disk, Write, delete the table, restore.
   Afterwards sfdisk, outside QEMU, must find exactly the result: the GPT
   disk changed as asked, the MBR disk as it was before (the backup was taken
   before anything was written).

    tests/partmgr/qemu-test.py OVMF.fd build/nesh.efi build/partmgr.efi
"""
import hashlib
import json
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
KEYS = {' ': 'spc', '\n': 'ret', '\\': 'backslash', '.': 'dot', ':': 'shift-semicolon', '-': 'minus'}


def check(name, cond, detail=""):
    global count
    count += 1
    if not cond:
        failures.append("%s: %s" % (name, detail))


def sfdisk_json(path):
    out = subprocess.run([SFDISK, "--json", path], capture_output=True, text=True, check=True)
    t = json.loads(out.stdout)["partitiontable"]
    t.pop("device", None)
    for p in t.get("partitions", []):
        p["node"] = p["node"][len(path):]
    return t


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
    def __init__(self, name, disks):
        esp = os.path.join(WORK, name)
        os.makedirs(os.path.join(esp, "EFI", "BOOT"))
        shutil.copy(NESH, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
        shutil.copy(PARTMGR, os.path.join(esp, "partmgr.efi"))
        self.log = os.path.join(WORK, name + ".log")
        mon = os.path.join(WORK, name + ".monitor")
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
        """The serial output so far, without the terminal control sequences and
        with every run of spaces and line breaks made one space, so that a
        dialog text wrapped over two lines still matches."""
        try:
            raw = open(self.log, "rb").read().decode("utf-8", "replace")
        except FileNotFoundError:
            return ""
        return re.sub(r"\s+", " ", re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", " ", raw))

    def wait(self, what, timeout=30):
        """Waits until WHAT (a regular expression) appears after the last match.
        The screen is drawn top to bottom: rows of the list come before the
        message line below them."""
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
            self.key("shift-" + ch.lower() if ch.isupper() else KEYS.get(ch, ch))

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
    q = Qemu("look", [gpt, mbr])
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
        check("mbr free outside", q.wait(r"- +351\.0 MiB +161\.0 MiB +free space(?! \(for)"), "")
        # back to the list, quit, and NESH is there again
        q.key("esc")
        q.key("q")
        q.type("ver\n")
        check("back to NESH", q.wait(r"NESH \d+\.\d+\.\d+"), "the NESH prompt did not come back")
    finally:
        q.stop()
    # looking does not write: the disks are identical, byte for byte
    for d in (gpt, mbr):
        check("untouched", digest(d) == before[d], os.path.basename(d) + " changed")

    # ------------------------------------------------------------ changing
    mbr_before = sfdisk_json(mbr)
    work = os.path.join(WORK, "work.img")          # a writable FAT volume for the backup file
    mkfs = shutil.which("mkfs.fat") or "/sbin/mkfs.fat"
    subprocess.run([mkfs, "-C", "-n", "WORK", work, "65536"], check=True, stdout=subprocess.DEVNULL)
    q = Qemu("change", [gpt, mbr, work])
    try:
        check("NESH starts again", q.wait(r"New EFI Shell", 90), "")
        q.type("\nfs0:\\partmgr.efi\n")
        check("list", q.wait(r"Select a disk"), "")
        # the boot disk (first row) cannot be changed
        q.key("ret")
        check("boot disk opened", q.wait(r"blk1 +disk"), "")
        q.key("z")
        check("boot disk refuses", q.wait(r"cannot be changed"), "no refusal")
        q.key("spc")
        q.key("esc")
        # GPT disk: a new partition in the free space at the end
        q.key("down")
        q.key("ret")
        check("gpt opened", q.wait(r"blk3 +disk +512\.0 MiB +GPT"), "")
        q.key("end")
        q.key("n")
        check("start asked", q.wait(r"New partition: start"), "")
        q.key("ret")                                  # the proposed start, 317 MiB
        check("size asked", q.wait(r"New partition: size"), "")
        q.type("20M\n")
        check("type asked", q.wait(r"Partition type"), "")
        for _ in range(7):                            # EFI system ... Linux filesystem
            q.key("down")
        q.key("ret")
        check("name asked", q.wait(r"New partition: name"), "")
        q.type("Test part\n")
        check("marked", q.wait(r"4\* +317\.0 MiB +20\.0 MiB +Linux filesystem +Test part"), "no * on the new partition")
        check("added", q.wait(r"Partition 4 added"), "")
        # rename partition 3, delete partition 2
        q.key("up")
        q.key("r")
        check("rename asked", q.wait(r"Rename"), "")
        q.type("Root\n")
        check("renamed", q.wait(r"Partition 3 renamed"), "")
        q.key("up")
        q.key("d")
        check("deleted", q.wait(r"Partition 2 deleted"), "")
        # leaving with changes asks; No stays
        q.key("esc")
        check("leave asks", q.wait(r"Throw them away\?"), "")
        q.key("n")
        # Write: a wrong name does nothing, the right one writes
        q.key("ret")
        check("write asks", q.wait(r"Type blk3 and press Enter"), "")
        q.type("blk4\n")
        check("wrong name refused", q.wait(r"not the name of this disk"), "")
        q.key("spc")
        q.key("ret")
        check("write asks again", q.wait(r"Type blk3 and press Enter"), "")
        q.type("blk3\n")
        check("written", q.wait(r"Written\."), "")
        q.key("esc")
        # MBR disk: a logical partition in the free space after partition 5
        check("list again", q.wait(r"Select a disk"), "")
        q.key("down")
        q.key("ret")
        check("mbr opened", q.wait(r"blk7 +disk +512\.0 MiB +MBR"), "")
        for _ in range(3):                            # 1, 2 (extended), 5, free
            q.key("down")
        q.key("n")
        check("mbr start asked", q.wait(r"New partition: start"), "")
        q.key("ret")
        q.type("30M\n")
        check("mbr type asked", q.wait(r"Partition type"), "")
        q.key("down")
        q.key("down")                                 # FAT32 (LBA), NTFS/exFAT, Linux
        q.key("ret")
        check("renumbered", q.wait(r"7\* +194\.0 MiB +40\.0 MiB +Linux swap +logical"), "the old 6 is not 7")
        check("logical added", q.wait(r"Partition 6 added"), "")
        # partition 5 becomes the active one
        q.key("up")
        q.key("a")
        check("active", q.wait(r"Partition 5 is the active one"), "")
        # backup of the disk as it is now (before writing), to the writable FAT disk
        q.key("b")
        check("backup asks", q.wait(r"Volumes: fs0 \(read-only\), fs1"), "")
        q.key("ret")
        check("backup saved", q.wait(r"is saved in fs1:\\partmgr-blk7\.bin \(\d+ bytes\)"), "")
        q.key("ret")
        q.type("blk7\n")
        check("mbr written", q.wait(r"Written\."), "")
        # delete the table, write, then restore the backup
        q.key("x")
        check("table gone", q.wait(r"The partition table is gone"), "")
        q.key("ret")
        check("delete asks", q.wait(r"will be deleted"), "")
        q.type("blk7\n")
        check("no table shown", q.wait(r"No partition table"), "")
        check("deleted written", q.wait(r"Written\."), "")
        q.key("s")
        check("restore asks for the file", q.wait(r"The backup file to write back"), "")
        q.key("ret")
        check("restore warns", q.wait(r"will be replaced now"), "")
        q.type("blk7\n")
        check("restored", q.wait(r"restored from fs1:\\partmgr-blk7\.bin"), "")
        q.key("esc")
        q.key("q")
        q.type("ver\n")
        check("back to NESH again", q.wait(r"NESH \d+\.\d+\.\d+"), "")
    finally:
        q.stop()
    # the GPT disk as asked
    g = sfdisk_json(gpt)
    got = [(p["node"], p["start"], p["size"], p.get("name", "")) for p in g["partitions"]]
    check("gpt result", got == [("1", 2048, 204800, "EFI system"), ("3", 239616, 409600, "Root"),
                                ("4", 649216, 40960, "Test part")], str(got))
    check("gpt type", g["partitions"][2]["type"].upper() == "0FC63DAF-8483-4772-8E79-3D69D8477DE4", "")
    for d in (gpt, mbr):
        out = subprocess.run([SFDISK, "--verify", d], capture_output=True, text=True)
        check("verify " + os.path.basename(d), out.returncode == 0 and "No errors detected" in out.stdout,
              out.stdout + out.stderr)
    # the MBR disk as it was: the backup was made before writing
    after = sfdisk_json(mbr)
    check("mbr restored", after == mbr_before, "\n%s\n%s" % (mbr_before, after))
finally:
    shutil.rmtree(WORK, ignore_errors=True)

for f in failures:
    print("FAIL " + f)
print("partmgr qemu tests: %d checks, %d failed" % (count, len(failures)))
sys.exit(1 if failures else 0)
