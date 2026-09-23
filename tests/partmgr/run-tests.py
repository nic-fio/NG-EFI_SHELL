#!/usr/bin/env python3
"""Tests of partmgr's partition table reader (src/partmgr/ptable.c).

Disk images are built with sfdisk (util-linux) and read by build/tests/ptdump;
for every image the reader must find what sfdisk itself reports (sfdisk --json):
table type, disk identifier, usable area, and every partition with its start,
size, type, identifier, name, attributes and boot flag. Then the images are
damaged on purpose - a broken primary GPT, a wiped protective MBR, a chain of
logical partitions that loops - and the reader must cope and say what it found.

    tests/partmgr/run-tests.py build/tests/ptdump
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

PTDUMP = os.path.abspath(sys.argv[1])
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


def ptdump(path, bsize=512):
    out = subprocess.run([PTDUMP, path, str(bsize)], capture_output=True, text=True)
    if out.returncode:
        raise RuntimeError("ptdump failed: " + out.stderr.strip())
    res = {"parts": [], "notes": []}
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
        elif line.startswith("note="):
            res["notes"].append(line[5:])
        else:
            k, _, v = line.partition("=")
            res[k] = v
    return res


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
finally:
    shutil.rmtree(WORK, ignore_errors=True)

for f in failures:
    print("FAIL " + f)
for s in skipped:
    print("SKIPPED " + s)
print("partmgr tests: %d checks, %d failed" % (count, len(failures)))
sys.exit(1 if failures else 0)
