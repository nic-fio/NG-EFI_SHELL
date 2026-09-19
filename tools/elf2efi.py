#!/usr/bin/env python3
"""Convert an x86_64 ELF shared object (linked with tools/efi.lds) into a
PE32+ EFI application with native base relocations.

Only R_X86_64_RELATIVE dynamic relocations are accepted: the image is built
with -fPIC -fvisibility=hidden -Bsymbolic, so every other relocation type
indicates a build problem and is reported as an error.
"""
import struct
import sys

SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4
SHT_PROGBITS, SHT_RELA, SHT_NOBITS = 1, 4, 8
R_X86_64_RELATIVE = 8

SECTION_ALIGN = 0x1000
FILE_ALIGN = 0x200

SCN_CODE = 0x00000020
SCN_IDATA = 0x00000040
SCN_UDATA = 0x00000080
SCN_DISCARDABLE = 0x02000000
SCN_EXEC = 0x20000000
SCN_READ = 0x40000000
SCN_WRITE = 0x80000000

# ELF sections that carry the image; everything else is dropped.
KEEP = {".text": ".text", ".rodata": ".rdata", ".data": ".data", ".bss": ".bss"}


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def die(msg):
    sys.exit("elf2efi: " + msg)


def main(src, dst, subsystem=10):
    elf = bytearray(open(src, "rb").read())
    if elf[:4] != b"\x7fELF" or elf[4] != 2 or struct.unpack_from("<H", elf, 18)[0] != 62:
        die("not an ELF64 x86_64 file")
    e_entry, = struct.unpack_from("<Q", elf, 24)
    e_shoff, = struct.unpack_from("<Q", elf, 40)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", elf, 58)

    shdrs = []
    for i in range(e_shnum):
        shdrs.append(struct.unpack_from("<IIQQQQIIQQ", elf, e_shoff + i * e_shentsize))
    stroff = shdrs[e_shstrndx][4]

    def name(sh):
        end = elf.index(b"\0", stroff + sh[0])
        return elf[stroff + sh[0]:end].decode()

    # Collect image sections (ELF VA == PE RVA, the linker script uses base 0).
    secs = []
    for sh in shdrs:
        n = name(sh)
        if n not in KEEP or not sh[2] & SHF_ALLOC or sh[5] == 0:
            continue
        if sh[3] % SECTION_ALIGN:
            die(f"section {n} not page aligned")
        data = bytearray() if sh[1] == SHT_NOBITS else bytearray(elf[sh[4]:sh[4] + sh[5]])
        secs.append({"elf": n, "name": KEEP[n], "va": sh[3], "vsize": sh[5], "data": data,
                     "flags": sh[2], "nobits": sh[1] == SHT_NOBITS})
    secs.sort(key=lambda s: s["va"])

    def section_at(rva):
        for s in secs:
            if s["va"] <= rva < s["va"] + s["vsize"]:
                return s
        die(f"relocation target {rva:#x} outside image sections")

    # Apply RELATIVE relocations and record them as PE DIR64 fixups.
    fixups = []
    for sh in shdrs:
        if sh[1] != SHT_RELA or not sh[2] & SHF_ALLOC:
            continue
        for off in range(sh[4], sh[4] + sh[5], 24):
            r_off, r_info, r_add = struct.unpack_from("<QQq", elf, off)
            if r_info & 0xFFFFFFFF != R_X86_64_RELATIVE:
                die(f"unsupported relocation type {r_info & 0xFFFFFFFF} at {r_off:#x}")
            s = section_at(r_off)
            if s["nobits"]:
                die(f"relocation in bss at {r_off:#x}")
            struct.pack_into("<Q", s["data"], r_off - s["va"], r_add)
            fixups.append(r_off)

    # Build .reloc: one block per 4K page, entries padded to 4-byte blocks.
    reloc = bytearray()
    pages = {}
    for f in sorted(set(fixups)):
        pages.setdefault(f & ~0xFFF, []).append(f & 0xFFF)
    for page, offs in sorted(pages.items()):
        ents = [(10 << 12) | o for o in offs]
        if len(ents) % 2:
            ents.append(0)  # IMAGE_REL_BASED_ABSOLUTE padding
        reloc += struct.pack("<II", page, 8 + 2 * len(ents))
        reloc += struct.pack(f"<{len(ents)}H", *ents)
    if not reloc:  # keep the image relocatable even without fixups
        reloc = bytearray(struct.pack("<IIHH", 0, 12, 0, 0))
    end = max(s["va"] + s["vsize"] for s in secs)
    secs.append({"name": ".reloc", "va": align(end, SECTION_ALIGN), "vsize": len(reloc),
                 "data": reloc, "flags": 0, "nobits": False, "elf": None})

    # Layout.
    nsec = len(secs)
    hdr_size = align(0x40 + 4 + 20 + 240 + 40 * nsec, FILE_ALIGN)
    fpos = hdr_size
    size_code = size_idata = size_udata = 0
    base_code = 0
    for s in secs:
        if s["name"] == ".text":
            ch = SCN_CODE | SCN_EXEC | SCN_READ
        elif s["name"] == ".rdata":
            ch = SCN_IDATA | SCN_READ
        elif s["name"] == ".data":
            ch = SCN_IDATA | SCN_READ | SCN_WRITE
        elif s["name"] == ".bss":
            ch = SCN_UDATA | SCN_READ | SCN_WRITE
        else:
            ch = SCN_IDATA | SCN_READ | SCN_DISCARDABLE
        s["ch"] = ch
        s["rawsize"] = align(len(s["data"]), FILE_ALIGN)
        s["rawptr"] = fpos if s["rawsize"] else 0
        fpos += s["rawsize"]
        if ch & SCN_CODE:
            size_code += s["rawsize"]
            base_code = base_code or s["va"]
        elif ch & SCN_UDATA:
            size_udata += align(s["vsize"], FILE_ALIGN)
        else:
            size_idata += s["rawsize"]
    size_image = align(secs[-1]["va"] + secs[-1]["vsize"], SECTION_ALIGN)
    reloc_sec = secs[-1]

    out = bytearray(fpos)
    struct.pack_into("<2s58xI", out, 0, b"MZ", 0x40)
    struct.pack_into("<4s", out, 0x40, b"PE\0\0")
    # COFF header: EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE | LINE_NUMS/LOCAL_SYMS stripped | DEBUG_STRIPPED
    struct.pack_into("<HHIIIHH", out, 0x44, 0x8664, nsec, 0, 0, 0, 240, 0x022E)
    opt = 0x58
    struct.pack_into("<HBBIIIII", out, opt, 0x20B, 2, 44, size_code, size_idata, size_udata,
                     e_entry, base_code)
    struct.pack_into("<QIIHHHHHHIIIIHHQQQQII", out, opt + 24,
                     0,                      # ImageBase (relocated by the loader)
                     SECTION_ALIGN, FILE_ALIGN,
                     0, 0, 0, 0, 0, 0,       # OS / image / subsystem versions
                     0,                      # Win32VersionValue
                     size_image, hdr_size, 0,
                     subsystem,
                     0x0140,                 # DYNAMIC_BASE | NX_COMPAT
                     0, 0, 0, 0, 0, 16)
    # Data directory 5: base relocations.
    struct.pack_into("<II", out, opt + 112 + 5 * 8, reloc_sec["va"], reloc_sec["vsize"])

    sh = opt + 240
    for s in secs:
        struct.pack_into("<8sIIIIIIHHI", out, sh, s["name"].encode(), s["vsize"], s["va"],
                         s["rawsize"], s["rawptr"], 0, 0, 0, 0, s["ch"])
        sh += 40
        if s["rawsize"]:
            out[s["rawptr"]:s["rawptr"] + len(s["data"])] = s["data"]

    open(dst, "wb").write(out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: elf2efi.py input.so output.efi")
    main(sys.argv[1], sys.argv[2])
