#!/usr/bin/env python3
"""Tests of partmgr's partition table code (src/partmgr), on disk image files.

Reading: images are built with sfdisk (util-linux) and read by
build/tests/pttool; the reader must find what sfdisk itself reports (sfdisk
--json): table type, disk identifier, usable area, and every partition with
its start, size, type, identifier, name, attributes and boot flag. Then the
images are damaged on purpose - a broken primary GPT, a wiped protective MBR,
a chain of logical partitions that loops - and the reader must cope and say
what it found.

Writing: tables made or changed by partmgr are read back by sfdisk (which
must find what was asked, and no errors with --verify) and by parted.
Backups are restored over a changed disk, which must then be identical,
byte for byte, to the original image.

    tests/partmgr/run-tests.py build/tests/pttool
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

PTTOOL = os.path.abspath(sys.argv[1])
PARTED = shutil.which("parted") or next((p for p in ("/sbin/parted", "/usr/sbin/parted") if os.path.exists(p)), None)
SFDISK = shutil.which("sfdisk") or next(
    (p for p in ("/sbin/sfdisk", "/usr/sbin/sfdisk") if os.path.exists(p)), None)
WORK = tempfile.mkdtemp(prefix="ptest-")
failures = []
skipped = []
count = 0


def sector_opt(bsize):
    """--sector-size exists only in newer sfdisk (not in util-linux 2.39); 512 is the default."""
    return [] if bsize == 512 else ["--sector-size", str(bsize)]


def sfdisk_knows_sector_size():
    out = subprocess.run([SFDISK, "--help"], capture_output=True, text=True)
    return "--sector-size" in out.stdout


def image(name, size_mb, script=None, bsize=512):
    path = os.path.join(WORK, name + ".img")
    with open(path, "wb") as f:
        f.truncate(size_mb * 1024 * 1024)
    if script is not None:
        subprocess.run([SFDISK, "-q"] + sector_opt(bsize) + [path], input=script.encode(),
                       check=True, stdout=subprocess.DEVNULL)
    return path


def pttool(path, *cmds, bsize=512, error=False):
    """Runs pttool; with error=True the commands must fail and the message is returned."""
    args = [PTTOOL, path] + (["-b", str(bsize)] if bsize != 512 else []) + [str(c) for c in cmds]
    out = subprocess.run(args, capture_output=True, text=True)
    if error:
        if out.returncode != 1 or not out.stdout.startswith("error="):
            raise RuntimeError("pttool %s: expected an error, got %r" % (cmds, out.stdout + out.stderr))
        return out.stdout[6:].strip()
    if out.returncode:
        raise RuntimeError("pttool %s failed: %s" % (cmds, (out.stdout + out.stderr).strip()))
    res = {"parts": [], "notes": [], "free": []}
    for line in out.stdout.splitlines():
        if line.startswith("part "):
            fields = {}
            rest = line[5:]
            # name= and typename= may contain spaces: they are the last two fields
            rest, _, typename = rest.partition(" typename=")
            if " name=" in rest:
                rest, _, fields["name"] = rest.partition(" name=")
            for kv in rest.split():
                k, _, v = kv.partition("=")
                fields[k] = v
            fields["typename"] = typename
            res["parts"].append(fields)
        elif line.startswith("free "):
            res["free"].append(dict(kv.split("=") for kv in line[5:].split()))
        elif line.startswith("note="):
            res["notes"].append(line[5:])
        else:
            k, _, v = line.partition("=")
            res[k] = v
    return res


def ptdump(path, bsize=512):
    return pttool(path, bsize=bsize)


def sfdisk_json(path, bsize=512):
    out = subprocess.run([SFDISK, "--json"] + sector_opt(bsize) + [path],
                         capture_output=True, text=True, check=True)
    return json.loads(out.stdout)["partitiontable"]


ATTR_BITS = {"RequiredPartition": 0, "NoBlockIOProtocol": 1, "LegacyBIOSBootable": 2}


def attrs_value(text):
    v = 0
    for word in text.split():
        if word.startswith("GUID:"):
            for b in word[5:].split(","):
                v |= 1 << int(b)
        else:
            v |= 1 << ATTR_BITS[word]
    return v


def check(name, cond, detail=""):
    global count
    count += 1
    if not cond:
        failures.append("%s: %s" % (name, detail))


def same_as_sfdisk(name, path, bsize=512):
    """The reader finds exactly what sfdisk reports."""
    ours, ref = ptdump(path, bsize), sfdisk_json(path, bsize)
    check(name, ours["label"] == ref["label"], "label %s, sfdisk %s" % (ours["label"], ref["label"]))
    check(name, ours["id"].lower() == ref["id"].lower(), "id %s, sfdisk %s" % (ours["id"], ref["id"]))
    if ref["label"] == "gpt":
        check(name, int(ours["firstlba"]) == ref["firstlba"] and int(ours["lastlba"]) == ref["lastlba"],
              "usable area %s-%s, sfdisk %s-%s" % (ours["firstlba"], ours["lastlba"], ref["firstlba"], ref["lastlba"]))
    rparts = ref.get("partitions", [])
    check(name, len(ours["parts"]) == len(rparts),
          "%d partitions, sfdisk %d" % (len(ours["parts"]), len(rparts)))
    for p, r in zip(ours["parts"], rparts):
        num = int(r["node"][len(path):])
        what = "%s partition %s" % (name, p["num"])
        check(what, int(p["num"]) == num, "number, sfdisk %d" % num)
        check(what, int(p["start"]) == r["start"] and int(p["size"]) == r["size"],
              "start/size %s/%s, sfdisk %s/%s" % (p["start"], p["size"], r["start"], r["size"]))
        if ref["label"] == "gpt":
            check(what, p["type"] == r["type"].upper(), "type %s, sfdisk %s" % (p["type"], r["type"]))
            check(what, p["uuid"] == r["uuid"].upper(), "uuid")
            check(what, p.get("name", "") == r.get("name", ""), "name %r, sfdisk %r" % (p.get("name"), r.get("name")))
            check(what, int(p["attrs"], 16) == attrs_value(r.get("attrs", "")),
                  "attrs %s, sfdisk %r" % (p["attrs"], r.get("attrs")))
        else:
            check(what, int(p["type"], 16) == int(r["type"], 16), "type %s, sfdisk %s" % (p["type"], r["type"]))
            check(what, (p["bootable"] == "yes") == bool(r.get("bootable")), "boot flag")
    check(name, not ours["notes"], "unexpected notes: %s" % ours["notes"])
    return ours


def copy(src, dst):
    """Copies an image keeping it sparse: the images are mostly empty."""
    subprocess.run(["cp", "--sparse=always", src, dst], check=True)


def patch(path, offset, data):
    with open(path, "r+b") as f:
        f.seek(offset)
        f.write(data)


def read(path, offset, n):
    with open(path, "rb") as f:
        f.seek(offset)
        return f.read(n)


def has_note(name, res, words):
    check(name, any(words in n for n in res["notes"]), "no note with %r in %s" % (words, res["notes"]))


def sfdisk_verify(name, path, bsize=512):
    """sfdisk --verify finds no problem in a table partmgr wrote."""
    out = subprocess.run([SFDISK, "--verify"] + sector_opt(bsize) + [path], capture_output=True, text=True)
    check(name, out.returncode == 0 and "No errors detected" in out.stdout,
          "sfdisk --verify: %s" % (out.stdout + out.stderr).strip().replace("\n", " | "))


def parted_ok(name, path):
    """parted reads the table without warnings or errors."""
    if not PARTED:
        return
    out = subprocess.run([PARTED, "-s", "-m", path, "unit", "s", "print"], capture_output=True, text=True)
    check(name, out.returncode == 0 and not out.stderr.strip(), "parted: %s" % out.stderr.strip())


def identical(name, a, b):
    out = subprocess.run(["cmp", "-s", a, b])
    check(name, out.returncode == 0, "%s and %s differ" % (os.path.basename(a), os.path.basename(b)))


def gpt_sig(path, lba, bsize=512):
    return read(path, lba * bsize, 8) == b"EFI PART"


def chs(lba):
    """CHS of an LBA with 255 heads and 63 sectors, as the three bytes of an MBR entry."""
    c = lba // (255 * 63)
    if c > 1023:
        return bytes([0xFE, 0xFF, 0xFF])
    return bytes([(lba // 63) % 255, (lba % 63 + 1) | ((c >> 2) & 0xC0), c & 0xFF])


def mbr_structure(name, path):
    """Checks, independently of the reader, what partmgr wrote in an MBR: every
    entry's CHS fields, and the chain of extended boot records - the first at
    the start of the extended partition, each describing its logical partition
    relative to itself and linking to the next with type 05, relative to the
    extended partition, with the size from that record to the end of the next
    logical partition."""
    b = read(path, 0, 512)
    ext = None
    for i in range(4):
        e = b[446 + 16 * i:462 + 16 * i]
        if not e[4]:
            continue
        start, size = struct.unpack("<II", e[8:16])
        check(name + " CHS", e[1:4] == chs(start) and e[5:8] == chs(start + size - 1),
              "entry %d: %s %s" % (i + 1, e[1:4].hex(), e[5:8].hex()))
        if e[4] in (0x05, 0x0F, 0x85):
            ext = (start, size)
    if not ext:
        return
    ebr, seen = ext[0], 0
    while True:
        r = read(path, ebr * 512, 512)
        check(name + " chain", r[510:512] == b"\x55\xaa", "record at %d has no signature" % ebr)
        e1, e2 = r[446:462], r[462:478]
        rel, size = struct.unpack("<II", e1[8:16])
        if e1[4]:
            check(name + " chain", e1[1:4] == chs(ebr + rel) and e1[5:8] == chs(ebr + rel + size - 1),
                  "CHS of the logical partition at %d" % (ebr + rel))
        if not e2[4]:
            break
        nrel, nsize = struct.unpack("<II", e2[8:16])
        nxt = ext[0] + nrel
        r2 = read(path, nxt * 512, 512)
        lrel, lsize = struct.unpack("<II", r2[446 + 8:446 + 16])
        check(name + " chain", e2[4] == 0x05 and nsize == lrel + lsize and nxt > ebr and
              e2[1:4] == chs(nxt) and e2[5:8] == chs(nxt + nsize - 1),
              "link from %d to %d: type %02x size %d, expected %d" % (ebr, nxt, e2[4], nsize, lrel + lsize))
        ebr, seen = nxt, seen + 1
        if seen > 64:
            check(name + " chain", False, "loops")
            break


ESP = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
MSR = "E3C9E316-0B5C-4DB8-817D-F92DF00215AE"
BDP = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
LNX = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"


# ---------------------------------------------------------------- the images

if not SFDISK:
    print("partmgr tests: sfdisk not found (package fdisk, see tools/setup-dev.sh)")
    sys.exit(1)

GPT3 = """label: gpt
size=100M, type=U, name="EFI system partition"
size=16M, type=E3C9E316-0B5C-4DB8-817D-F92DF00215AE
size=200M, type=L, name="Données été", attrs="RequiredPartition,GUID:60,GUID:63"
size=50M, type=0657FD6D-A4AB-43C4-84E5-0933C84B4F4F
"""

try:
    # GPT, 512-byte blocks: names with accents, attributes, a type the reader has no name for
    g = image("gpt", 512, GPT3 + "size=8M, type=11111111-2222-3333-4444-555555555555\n")
    res = same_as_sfdisk("gpt", g)
    check("gpt", res["primary"] == "ok" and res["backup"] == "ok", "copies %s/%s" % (res["primary"], res["backup"]))
    names = [p["typename"] for p in res["parts"]]
    check("gpt type names", names[:4] == ["EFI system", "Microsoft reserved", "Linux filesystem", "Linux swap"],
          str(names))

    # GPT with 4096-byte blocks and a longer entry array
    if sfdisk_knows_sector_size():
        g4 = image("gpt4k", 512, "label: gpt\ntable-length: 200\n" + GPT3.split("\n", 1)[1], bsize=4096)
        same_as_sfdisk("gpt 4K", g4, 4096)
    else:
        skipped.append("GPT with 4096-byte blocks (this sfdisk has no --sector-size)")

    # GPT with gaps between partitions and entries not in disk order
    gg = image("gptgaps", 256, "label: gpt\n3 : start=100MiB, size=20MiB, type=L\n"
               "1 : start=10MiB, size=10MiB, type=U\n7 : start=200MiB, size=10MiB, type=L\n")
    same_as_sfdisk("gpt gaps", gg)

    # MBR: primary partitions, boot flag, types
    m = image("mbr", 256, "label: dos\nsize=50M, type=ef, bootable\nsize=50M, type=7\ntype=83\n")
    res = same_as_sfdisk("mbr", m)
    check("mbr type names", [p["typename"] for p in res["parts"]] == ["EFI system", "NTFS/exFAT", "Linux"],
          str([p["typename"] for p in res["parts"]]))

    # MBR with an extended partition, logical partitions and a gap inside it
    me = image("mbrext", 512, "label: dos\n"
               "1 : start=2048, size=102400, type=c\n"
               "2 : start=104448, size=614400, type=5\n"
               "3 : start=720896, size=40960, type=b\n"
               "5 : start=106496, size=204800, type=83\n"
               "6 : start=313344, size=61440, type=83\n"
               "7 : start=397312, size=81920, type=82\n")
    res = same_as_sfdisk("mbr extended", me)
    roles = [(p["num"], p["role"]) for p in res["parts"]]
    check("mbr extended roles", roles[:2] == [("1", "primary"), ("2", "extended")] and
          all(r == "logical" for _, r in roles[3:]), str(roles))

    # MBR with the extended partition at LBA (0F) and no logicals yet
    mf = image("mbrext0", 64, "label: dos\nsize=20M, type=83\ntype=f\n")
    same_as_sfdisk("mbr empty extended", mf)

    # an empty MBR and an empty GPT
    same_as_sfdisk("mbr empty", image("mbr0", 16, "label: dos\n"))
    same_as_sfdisk("gpt empty", image("gpt0", 16, "label: gpt\n"))

    # ------------------------------------------------------------ damaged disks

    # nothing at all
    res = ptdump(image("blank", 16))
    check("blank", res["label"] == "none" and not res["parts"] and not res["notes"], str(res))

    # a FAT file system on the whole disk ("superfloppy")
    sf = image("superfloppy", 32)
    mkfs = shutil.which("mkfs.fat") or "/sbin/mkfs.fat"
    subprocess.run([mkfs, "-F", "16", sf], check=True, capture_output=True)
    res = ptdump(sf)
    check("superfloppy", res["label"] == "none" and not res["parts"], str(res))
    has_note("superfloppy", res, "file system covers the whole disk")

    # primary GPT header broken: the backup is used, and gives the same partitions
    ref = ptdump(g)
    bad = image("gpt-badprimary", 512)
    copy(g, bad)
    patch(bad, 512 + 60, b"\xff")                 # inside the disk GUID: header CRC fails
    res = ptdump(bad)
    check("gpt bad primary", res["label"] == "gpt" and res["primary"] == "bad" and res["backup"] == "ok", str(res))
    check("gpt bad primary", res["parts"] == ref["parts"], "partitions differ from the intact disk")
    has_note("gpt bad primary", res, "primary GPT has a wrong checksum")

    # primary entry array broken (header intact): same result
    bad = os.path.join(WORK, "gpt-badarray.img")
    copy(g, bad)
    patch(bad, 2 * 512 + 40, b"\x00")             # inside entry 1: array CRC fails
    res = ptdump(bad)
    check("gpt bad array", res["primary"] == "bad" and res["parts"] == ref["parts"], str(res))
    has_note("gpt bad array", res, "partition array with a wrong checksum")

    # backup header broken
    bad = os.path.join(WORK, "gpt-badbackup.img")
    copy(g, bad)
    patch(bad, os.path.getsize(bad) - 512, b"XXXX")
    res = ptdump(bad)
    check("gpt bad backup", res["primary"] == "ok" and res["backup"] == "bad" and res["parts"] == ref["parts"],
          str(res))
    has_note("gpt bad backup", res, "backup GPT has no GPT signature")

    # both copies broken: no usable table
    patch(bad, 512 + 60, b"\xff")
    res = ptdump(bad)
    check("gpt both bad", res["label"] == "none" and not res["parts"], str(res))
    has_note("gpt both bad", res, "both GPT copies are damaged")

    # protective MBR wiped: still a GPT
    bad = os.path.join(WORK, "gpt-nopmbr.img")
    copy(g, bad)
    patch(bad, 0, bytes(512))
    res = ptdump(bad)
    check("gpt no protective MBR", res["label"] == "gpt" and res["parts"] == ref["parts"], str(res))
    has_note("gpt no protective MBR", res, "protective MBR is missing")

    # hybrid MBR: a second entry next to the protective one
    bad = os.path.join(WORK, "gpt-hybrid.img")
    copy(g, bad)
    patch(bad, 446 + 16, bytes([0x80, 0, 0, 0, 0x0C, 0, 0, 0]) + struct.pack("<II", 2048, 204800))
    res = ptdump(bad)
    check("gpt hybrid", res["label"] == "gpt" and res["hybrid"] == "yes" and res["parts"] == ref["parts"], str(res))
    has_note("gpt hybrid", res, "hybrid MBR")

    # a GPT copy moved away from the last block (disk enlarged after partitioning)
    big = os.path.join(WORK, "gpt-enlarged.img")
    copy(g, big)
    with open(big, "r+b") as f:
        f.truncate(os.path.getsize(g) + 64 * 1024 * 1024)
    res = ptdump(big)
    check("gpt enlarged", res["label"] == "gpt" and res["backup"] == "ok" and res["parts"] == ref["parts"], str(res))
    has_note("gpt enlarged", res, "not in the last block")

    # a chain of logical partitions that loops back on itself
    loop = os.path.join(WORK, "mbr-loop.img")
    copy(me, loop)
    mref = ptdump(me)
    ext = next(p for p in mref["parts"] if p["role"] == "extended")
    first_ebr = int(ext["start"]) * 512
    # the link of the first EBR points to the first EBR itself
    patch(loop, first_ebr + 462 + 8, struct.pack("<I", 0))
    res = ptdump(loop)
    check("mbr loop", len([p for p in res["parts"] if p["role"] == "logical"]) == 1, str(res["parts"]))
    has_note("mbr loop", res, "points outside its extended partition")

    # an EBR without the 55 AA signature
    brk = os.path.join(WORK, "mbr-badebr.img")
    copy(me, brk)
    patch(brk, first_ebr + 510, b"\0\0")
    res = ptdump(brk)
    check("mbr bad ebr", not [p for p in res["parts"] if p["role"] == "logical"], str(res["parts"]))
    has_note("mbr bad ebr", res, "invalid extended boot record")

    # a partition beyond the end of the disk (the image was shrunk)
    small = os.path.join(WORK, "mbr-shrunk.img")
    copy(m, small)
    with open(small, "r+b") as f:
        f.truncate(128 * 1024 * 1024)
    res = ptdump(small)
    has_note("mbr shrunk", res, "beyond the end of the disk")

    # entries with an invalid status byte are not taken for an MBR
    junk = image("junk", 16, "label: dos\ntype=83\n")
    patch(junk, 446, b"\x42")
    res = ptdump(junk)
    check("mbr invalid", res["label"] == "none", str(res))
    has_note("mbr invalid", res, "entries are invalid")

    # the GPT header CRC is the one zlib computes (the reader's CRC-32 is right)
    hdr = bytearray(read(g, 512, 92))
    crc = struct.unpack("<I", hdr[16:20])[0]
    hdr[16:20] = b"\0\0\0\0"
    check("crc32", zlib.crc32(bytes(hdr)) == crc, "sfdisk header CRC does not match zlib")

    # ================================================================ writing

    # the independent MBR check agrees with sfdisk's own CHS fields and chains
    mbr_structure("sfdisk mbr", m)
    mbr_structure("sfdisk mbr extended", me)

    # a new GPT on an empty disk: the usable area is the one sfdisk would choose
    w = image("w-gpt", 512)
    ref = sfdisk_json(image("w-gpt-ref", 512, "label: gpt\n"))
    res = pttool(w, "new", "gpt", "free")
    check("new gpt free space", res["free"] == [{"start": "2048", "size": str(ref["lastlba"] - 2048 + 1),
                                                 "logical": "no"}], str(res["free"]))
    # the first usable block is 34 (the minimum, as Windows and gdisk do); sfdisk says 2048
    check("new gpt usable area", res["firstlba"] == "34" and int(res["lastlba"]) == ref["lastlba"],
          "%s-%s, sfdisk %s-%s" % (res["firstlba"], res["lastlba"], ref["firstlba"], ref["lastlba"]))
    res = pttool(w, "new", "gpt", "add", "primary", 2048, 204800, ESP, "EFI system",
                 "add", "primary", 206848, 32768, MSR, "-",
                 "add", "primary", 239616, 400000, BDP, "Windows \u00e9t\u00e9", "write", "reread")
    asked = [("1", "2048", "204800", ESP, "EFI system"), ("2", "206848", "32768", MSR, ""),
             ("3", "239616", "400000", BDP, "Windows \u00e9t\u00e9")]
    got = [(p["num"], p["start"], p["size"], p["type"], p.get("name", "")) for p in res["parts"]]
    check("new gpt", got == asked, str(got))
    check("new gpt", res["primary"] == "ok" and res["backup"] == "ok" and not res["notes"], str(res))
    same_as_sfdisk("new gpt (sfdisk)", w)
    sfdisk_verify("new gpt", w)
    parted_ok("new gpt", w)
    check("new gpt identifiers", len({p["uuid"] for p in res["parts"]} | {res["id"]}) == 4, "not unique")
    check("new gpt boot code", read(w, 0, 440) == bytes(440), "block 0 has boot code")

    # the same with 4096-byte blocks
    w4 = image("w-gpt4k", 512)
    res = pttool(w4, "new", "gpt", "add", "primary", 256, 25600, ESP, "EFI", "write", "reread", bsize=4096)
    check("new gpt 4K", res["firstlba"] == "6" and res["primary"] == "ok" and res["backup"] == "ok" and
          [(p["start"], p["size"]) for p in res["parts"]] == [("256", "25600")] and not res["notes"], str(res))
    if sfdisk_knows_sector_size():
        same_as_sfdisk("new gpt 4K (sfdisk)", w4, 4096)
        sfdisk_verify("new gpt 4K", w4, 4096)
    else:
        skipped.append("new GPT with 4096-byte blocks read back by sfdisk (no --sector-size)")

    # a new MBR: a primary partition, then logical ones, which create the extended partition
    wm = image("w-mbr", 1024)
    res = pttool(wm, "new", "mbr", "add", "primary", 2048, 204800, "ef", "-", "active", 1, "on", "free")
    check("new mbr free", res["free"] == [{"start": "206848", "size": str(2097152 - 206848), "logical": "no"}],
          str(res["free"]))
    msg = pttool(wm, "new", "mbr", "add", "primary", 2048, 204800, "ef", "-",
                 "add", "logical", 206848, 102400, "83", "-", error=True)
    check("logical needs room", "1 MiB into the free area" in msg, msg)
    res = pttool(wm, "new", "mbr", "add", "primary", 2048, 204800, "ef", "-", "active", 1, "on",
                 "add", "logical", 208896, 102400, "83", "-", "free")
    check("extended created", [(p["num"], p["role"], p["type"], p["start"]) for p in res["parts"]] ==
          [("1", "primary", "ef", "2048"), ("2", "extended", "5", "206848"), ("5", "logical", "83", "208896")],
          str(res["parts"]))
    check("free inside extended", res["free"] == [{"start": "313344", "size": str(2097152 - 313344),
                                                   "logical": "yes"}], str(res["free"]))
    res = pttool(wm, "new", "mbr", "add", "primary", 2048, 204800, "ef", "-", "active", 1, "on",
                 "add", "logical", 208896, 102400, "83", "-", "add", "logical", 313344, 102400, "82", "-",
                 "add", "logical", 419840, 102400, "7", "-", "write", "reread")
    check("new mbr", [(p["num"], p["role"]) for p in res["parts"]] ==
          [("1", "primary"), ("2", "extended"), ("5", "logical"), ("6", "logical"), ("7", "logical")] and
          not res["notes"], str(res))
    same_as_sfdisk("new mbr (sfdisk)", wm)
    sfdisk_verify("new mbr", wm)
    mbr_structure("new mbr", wm)
    parted_ok("new mbr", wm)
    check("new mbr boot flag", sfdisk_json(wm)["partitions"][0].get("bootable") is True, "partition 1 not active")

    # refusals
    msg = pttool(wm, "add", "primary", 1000000, 2048, "83", "-", error=True)
    check("refuse overlap", "overlaps" in msg, msg)
    msg = pttool(wm, "new", "mbr", "add", "primary", 2048, 2048, "83", "-", "add", "primary", 4096, 2048, "83", "-",
                 "add", "primary", 6144, 2048, "83", "-", "add", "primary", 8192, 2048, "83", "-",
                 "add", "primary", 10240, 2048, "83", "-", error=True)
    check("refuse fifth primary", "at most four" in msg, msg)
    msg = pttool(w, "name", 1, "x" * 37, error=True)
    check("refuse long name", "too long" in msg, msg)
    msg = pttool(wm, "type", 2, "83", error=True)
    check("refuse extended type change", "extended" in msg, msg)
    msg = pttool(wm, "active", 2, "on", error=True)
    check("refuse active extended", "cannot be active" in msg, msg)
    huge = image("w-huge", 3 * 1024 * 1024)
    msg = pttool(huge, "new", "mbr", "add", "primary", 2048, 5000000000, "83", "-", error=True)
    check("refuse MBR beyond 2 TiB", "2 TiB" in msg, msg)

    # an extended partition beyond 8 GB gets the LBA type 0F
    big = image("w-big", 20 * 1024)
    res = pttool(big, "new", "mbr", "add", "logical", 4096, 204800, "83", "-", "write", "reread")
    check("extended 0F", [p["type"] for p in res["parts"]] == ["f", "83"], str(res["parts"]))
    sfdisk_verify("extended 0F", big)
    mbr_structure("extended 0F", big)

    # changing a GPT made by sfdisk: identifiers of untouched partitions stay
    e = os.path.join(WORK, "w-edit.img")
    copy(g, e)
    before = {p["num"]: p for p in ptdump(e)["parts"]}
    res = pttool(e, "del", 2, "free")
    gap = next(f for f in res["free"] if int(f["start"]) == int(before["2"]["start"]))
    res = pttool(e, "del", 2, "add", "primary", gap["start"], 20480, LNX, "new one", "name", 3, "Renamed",
                 "type", 4, LNX, "write", "reread")
    after = {p["num"]: p for p in res["parts"]}
    check("edit gpt", after["1"] == before["1"] and after["5"] == before["5"], "untouched partitions changed")
    check("edit gpt", after["2"]["uuid"] != before["2"]["uuid"] and after["2"]["name"] == "new one" and
          after["2"]["size"] == "20480", str(after["2"]))
    check("edit gpt", after["3"]["name"] == "Renamed" and after["3"]["uuid"] == before["3"]["uuid"] and
          after["4"]["type"] == LNX, str(after))
    check("edit gpt", res["id"] == ptdump(g)["id"] and not res["notes"], "disk GUID changed or notes")
    same_as_sfdisk("edit gpt (sfdisk)", e)
    sfdisk_verify("edit gpt", e)
    parted_ok("edit gpt", e)

    # changing an MBR made by sfdisk: delete the first logical, then fill the gap it left
    em = os.path.join(WORK, "w-editmbr.img")
    copy(me, em)
    before = ptdump(em)["parts"]
    res = pttool(em, "del", 5, "write", "reread")
    logs = [(p["num"], p["start"], p["size"]) for p in res["parts"] if p["role"] == "logical"]
    check("delete first logical", logs == [("5", "313344", "61440"), ("6", "397312", "81920")], str(logs))
    same_as_sfdisk("delete first logical (sfdisk)", em)
    sfdisk_verify("delete first logical", em)
    mbr_structure("delete first logical", em)
    res = pttool(em, "free")
    first = next(f for f in res["free"] if f["logical"] == "yes")
    check("gap of the first logical", first["start"] == "106496", str(res["free"]))
    res = pttool(em, "add", "logical", 106496, 100000, "83", "-", "write", "reread")
    check("refill first logical", [p["start"] for p in res["parts"] if p["role"] == "logical"] ==
          ["106496", "313344", "397312"], str(res["parts"]))
    same_as_sfdisk("refill first logical (sfdisk)", em)
    sfdisk_verify("refill first logical", em)
    mbr_structure("refill first logical", em)
    parted_ok("refill first logical", em)
    res = pttool(em, "del", 2, "write", "reread")
    check("delete extended", [p["num"] for p in res["parts"]] == ["1", "3"], str(res["parts"]))
    sfdisk_verify("delete extended", em)
    mbr_structure("delete extended", em)

    # boot code: kept when a table is changed, cleared when a new table is made
    bc = os.path.join(WORK, "w-bootcode.img")
    copy(m, bc)
    code = bytes((i * 7 + 3) & 0xFF for i in range(440))
    patch(bc, 0, code)
    sig = read(bc, 440, 4)
    pttool(bc, "active", 2, "on", "write")
    check("boot code kept", read(bc, 0, 440) == code and read(bc, 440, 4) == sig, "changed")
    check("active moved", [p.get("bootable", False) for p in sfdisk_json(bc)["partitions"]] == [False, True, False],
          "boot flags")
    pttool(bc, "new", "mbr", "add", "primary", 2048, 2048, "83", "-", "write")
    check("boot code cleared", read(bc, 0, 440) == bytes(440) and read(bc, 440, 4) not in (sig, bytes(4)),
          "new table kept the boot code or has no signature")

    # a hybrid MBR is left alone
    hy = os.path.join(WORK, "w-hybrid.img")
    copy(os.path.join(WORK, "gpt-hybrid.img"), hy)
    lba0 = read(hy, 0, 512)
    pttool(hy, "name", 1, "changed", "write")
    check("hybrid kept", read(hy, 0, 512) == lba0 and ptdump(hy)["parts"][0]["name"] == "changed", "block 0 changed")

    # GPT -> MBR: the old GPT headers are wiped
    gm = os.path.join(WORK, "w-gpt2mbr.img")
    copy(g, gm)
    res = pttool(gm, "new", "mbr", "add", "primary", 2048, 100000, "83", "-", "write", "reread")
    check("gpt to mbr", res["label"] == "dos" and not res["notes"] and not gpt_sig(gm, 1) and
          not gpt_sig(gm, os.path.getsize(gm) // 512 - 1), str(res))
    sfdisk_verify("gpt to mbr", gm)
    mbr_structure("gpt to mbr", gm)
    parted_ok("gpt to mbr", gm)

    # MBR -> GPT
    mg = os.path.join(WORK, "w-mbr2gpt.img")
    copy(me, mg)
    res = pttool(mg, "new", "gpt", "add", "primary", 2048, 100000, LNX, "-", "write", "reread")
    check("mbr to gpt", res["label"] == "gpt" and not res["notes"], str(res))
    sfdisk_verify("mbr to gpt", mg)
    parted_ok("mbr to gpt", mg)

    # deleting the table
    dt = os.path.join(WORK, "w-delete.img")
    copy(g, dt)
    res = pttool(dt, "new", "none", "write", "reread")
    check("delete table", res["label"] == "none" and not res["parts"] and not res["notes"], str(res))
    out = subprocess.run([SFDISK, "--json", dt], capture_output=True, text=True)
    check("delete table (sfdisk)", "partitiontable" not in out.stdout, out.stdout[:80])

    # writing repairs a damaged primary GPT and moves the backup to the end of an enlarged disk
    rp = os.path.join(WORK, "w-repair.img")
    copy(os.path.join(WORK, "gpt-badprimary.img"), rp)
    res = pttool(rp, "write", "reread")
    check("repair", res["primary"] == "ok" and res["backup"] == "ok" and not res["notes"] and
          res["parts"] == ptdump(g)["parts"], str(res))
    sfdisk_verify("repair", rp)
    en = os.path.join(WORK, "w-enlarged.img")
    copy(os.path.join(WORK, "gpt-enlarged.img"), en)
    res = pttool(en, "write", "reread")
    check("enlarged", not res["notes"] and int(res["lastlba"]) == sfdisk_json(en)["lastlba"] and
          int(res["lastlba"]) == os.path.getsize(en) // 512 - 34, str(res))
    sfdisk_verify("enlarged", en)

    # backup and restore: after restoring, the disk is identical to the original, byte for byte
    for name, src in (("gpt", g), ("mbr extended", me)):
        orig = os.path.join(WORK, "w-orig.img")
        work = os.path.join(WORK, "w-work.img")
        bk = os.path.join(WORK, "w-backup.bin")
        copy(src, orig)
        copy(src, work)
        pttool(work, "backup", bk)
        pttool(work, "new", "gpt" if name != "gpt" else "mbr", "add", "primary", 4096, 8192,
               LNX if name != "gpt" else "83", "-", "write")
        check("restore " + name, ptdump(work)["parts"] != ptdump(orig)["parts"], "the disk was not changed")
        res = pttool(work, "restore", bk)
        check("restore " + name, res["parts"] == ptdump(orig)["parts"] and not res["notes"], str(res))
        identical("restore " + name, work, orig)
    msg = pttool(image("w-other", 256), "restore", bk, error=True)
    check("restore on another disk", "different size" in msg, msg)
    data = bytearray(open(bk, "rb").read())
    data[40] ^= 1
    open(bk, "wb").write(bytes(data))
    msg = pttool(work, "restore", bk, error=True)
    check("restore damaged backup", "damaged" in msg, msg)
    msg = pttool(image("w-blank", 16), "backup", bk, error=True)
    check("backup without a table", "nothing to back up" in msg, msg)
finally:
    shutil.rmtree(WORK, ignore_errors=True)

for f in failures:
    print("FAIL " + f)
for s in skipped:
    print("SKIPPED " + s)
print("partmgr tests: %d checks, %d failed" % (count, len(failures)))
sys.exit(1 if failures else 0)
