/* Partition tables of partmgr: reading GPT and MBR (extended and logical
 * partitions included) into one description of the disk.
 *
 * GPT (UEFI Specification 2.10, chapter 5): a protective MBR in block 0, the
 * primary header in block 1 with its entry array, and a backup header in the
 * last block with its own array before it. Both copies are checked (signature,
 * header CRC, position, entry array CRC); when the primary one is damaged the
 * backup is used and a note says so.
 *
 * MBR: four entries in block 0. An extended partition (type 05, 0F or 85)
 * holds a chain of extended boot records, each describing one logical
 * partition (relative to itself) and the next record (relative to the start
 * of the extended partition). Logical partitions are numbered from 5, as
 * Linux and sfdisk do. */
#include "ptint.h"
#include "../lib/crc32.h"
#include "../pal/pal.h"

static void note(PtTable *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void note(PtTable *t, const char *fmt, ...)
{
    if (t->nnotes >= PT_MAX_NOTES)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->notes[t->nnotes++], sizeof(t->notes[0]), fmt, ap);
    va_end(ap);
}

static PtPart *add_part(PtTable *t)
{
    t->parts = xrealloc(t->parts, (t->nparts + 1) * sizeof(PtPart));
    PtPart *p = &t->parts[t->nparts++];
    memset(p, 0, sizeof(*p));
    return p;
}

static uint8_t *read_blocks(const PtDev *d, uint64_t lba, uint32_t count)
{
    if (!count || lba >= d->nblocks || count > d->nblocks - lba)
        return NULL;
    uint8_t *b = xmalloc((size_t)count * d->bsize);
    if (d->read(d->ctx, lba, count, b)) {
        free(b);
        return NULL;
    }
    return b;
}

void pt_guid_str(const uint8_t g[16], char out[37])
{
    snprintf(out, 37, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", le32(g), le16(g + 4), le16(g + 6),
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

bool pt_guid_parse(const char *s, uint8_t g[16])
{
    static const int pos[16] = { 6, 4, 2, 0, 11, 9, 16, 14, 19, 21, 24, 26, 28, 30, 32, 34 };
    if (strlen(s) != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
        return false;
    for (int i = 0; i < 16; i++) {
        char h[3] = { s[pos[i]], s[pos[i] + 1], 0 };
        if (!isxdigit((uint8_t)h[0]) || !isxdigit((uint8_t)h[1]))
            return false;
        g[i] = (uint8_t)strtoul(h, NULL, 16);
    }
    return true;
}

static bool guid_zero(const uint8_t g[16])
{
    for (int i = 0; i < 16; i++)
        if (g[i])
            return false;
    return true;
}

/* ---- MBR ---- */

/* A file system written on the whole disk, without a partition table (a
 * "superfloppy"): its boot sector also ends in 55 AA. */
static bool fs_boot_sector(const uint8_t *b)
{
    if (b[0] != 0xEB && b[0] != 0xE9)
        return false;
    return !memcmp(b + 3, "NTFS    ", 8) || !memcmp(b + 3, "EXFAT   ", 8) || !memcmp(b + 0x36, "FAT", 3) ||
           !memcmp(b + 0x52, "FAT32", 5);
}

static void mbr_logicals(const PtDev *d, PtTable *t, const PtPart *ext)
{
    uint64_t ebr = ext->start, end = ext->start + ext->size;
    int num = 5;
    for (int guard = 0;; guard++) {
        if (guard >= 128) {
            note(t, "the chain of logical partitions is too long; stopped after 128");
            return;
        }
        uint8_t *b = read_blocks(d, ebr, 1);
        if (!b) {
            note(t, "cannot read the extended boot record at block %llu", (unsigned long long)ebr);
            return;
        }
        if (b[510] != 0x55 || b[511] != 0xAA) {
            note(t, "invalid extended boot record at block %llu", (unsigned long long)ebr);
            free(b);
            return;
        }
        const uint8_t *e1 = b + 446, *e2 = b + 462;
        if (e1[4] && le32(e1 + 12)) {
            PtPart *p = add_part(t);
            p->num = num++;
            p->role = PT_LOGICAL;
            p->mbr_type = e1[4];
            p->active = e1[0] == 0x80;
            p->start = ebr + le32(e1 + 8);
            p->size = le32(e1 + 12);
            p->ebr_lba = ebr;
            if (p->start + p->size > end)
                note(t, "logical partition %d goes beyond its extended partition", p->num);
        }
        uint64_t next = pt_mbr_extended(e2[4]) && le32(e2 + 12) ? ext->start + le32(e2 + 8) : 0;
        free(b);
        if (!next)
            return;
        if (next <= ebr || next >= end) {
            note(t, "the chain of logical partitions points outside its extended partition");
            return;
        }
        ebr = next;
    }
}

static void read_mbr(const PtDev *d, PtTable *t, const uint8_t *b)
{
    t->kind = PT_MBR;
    t->mbr_sig = le32(b + 440);
    int next = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = b + 446 + i * 16;
        if (!e[4] || !le32(e + 12))
            continue;
        PtPart *p = add_part(t);
        p->num = i + 1;
        p->mbr_type = e[4];
        p->active = e[0] == 0x80;
        p->start = le32(e + 8);
        p->size = le32(e + 12);
        p->role = pt_mbr_extended(e[4]) ? PT_EXTENDED : PT_PRIMARY;
        if (p->start + p->size > d->nblocks)
            note(t, "partition %d goes beyond the end of the disk", p->num);
        if (p->role == PT_EXTENDED && next++)
            note(t, "more than one extended partition; only the first one is read");
    }
    /* logical partitions after the primary ones, as in the kernel's numbering */
    int n = t->nparts;
    for (int i = 0; i < n; i++)
        if (t->parts[i].role == PT_EXTENDED) {
            PtPart ext = t->parts[i];
            mbr_logicals(d, t, &ext);
            break;
        }
}

/* ---- GPT ---- */

/* Reads and checks the header at LBA and its entry array. On success returns
 * true with the header (one block) and the array in *hdr and *ents. */
static bool gpt_copy(const PtDev *d, uint64_t lba, uint8_t **hdr, uint8_t **ents, const char **why)
{
    *hdr = *ents = NULL;
    uint8_t *h = read_blocks(d, lba, 1);
    if (!h) {
        *why = "cannot be read";
        return false;
    }
    uint32_t hsize = le32(h + 12);
    *why = "has no GPT signature";
    if (memcmp(h, "EFI PART", 8))
        goto bad;
    *why = "has an invalid size";
    if (hsize < 92 || hsize > d->bsize)
        goto bad;
    uint32_t crc = le32(h + 16);
    memset(h + 16, 0, 4);
    uint32_t calc = crc32_update(0, h, hsize);
    h[16] = crc, h[17] = crc >> 8, h[18] = crc >> 16, h[19] = crc >> 24;
    *why = "has a wrong checksum";
    if (calc != crc)
        goto bad;
    *why = "is not where it says it is";
    if (le64(h + 24) != lba)
        goto bad;
    uint64_t first = le64(h + 40), last = le64(h + 48), elba = le64(h + 72);
    uint32_t n = le32(h + 80), esize = le32(h + 84);
    *why = "describes an impossible layout";
    if (first > last || last >= d->nblocks || esize < 128 || esize % 8 || n == 0 ||
        (uint64_t)n * esize > 1024 * 1024)
        goto bad;
    uint32_t eblocks = (uint32_t)(((uint64_t)n * esize + d->bsize - 1) / d->bsize);
    uint8_t *e = read_blocks(d, elba, eblocks);
    *why = "has an unreadable partition array";
    if (!e)
        goto bad;
    *why = "has a partition array with a wrong checksum";
    if (crc32_update(0, e, (size_t)n * esize) != le32(h + 88)) {
        free(e);
        goto bad;
    }
    *hdr = h;
    *ents = e;
    return true;
bad:
    free(h);
    return false;
}

static void read_gpt(const PtDev *d, PtTable *t)
{
    uint8_t *ph, *pe, *bh = NULL, *be = NULL;
    const char *pwhy, *bwhy = "cannot be found";
    t->primary_ok = gpt_copy(d, 1, &ph, &pe, &pwhy);
    uint64_t alt = t->primary_ok ? le64(ph + 32) : d->nblocks - 1;
    t->backup_ok = alt < d->nblocks && gpt_copy(d, alt, &bh, &be, &bwhy);
    if (!t->primary_ok && !t->backup_ok) {
        t->kind = PT_NONE;
        note(t, "the protective MBR announces a GPT, but both GPT copies are damaged");
        return;
    }
    t->kind = PT_GPT;
    if (!t->primary_ok)
        note(t, "the primary GPT %s; the backup copy is used", pwhy);
    if (!t->backup_ok)
        note(t, "the backup GPT %s", bwhy);
    else if (alt != d->nblocks - 1)
        note(t, "the backup GPT is not in the last block (the disk was enlarged?)");
    const uint8_t *h = t->primary_ok ? ph : bh, *e = t->primary_ok ? pe : be;
    if (t->primary_ok && t->backup_ok &&
        (memcmp(ph + 56, bh + 56, 16) || le32(ph + 88) != le32(bh + 88) || le32(ph + 80) != le32(bh + 80)))
        note(t, "the primary and backup GPT differ; the primary one is shown");
    memcpy(t->disk_guid, h + 56, 16);
    t->first_usable = le64(h + 40);
    t->last_usable = le64(h + 48);
    t->max_entries = le32(h + 80);
    t->entry_size = le32(h + 84);
    t->primary_entries_lba = t->primary_ok ? le64(ph + 72) : 2;
    t->backup_lba = alt;
    t->backup_entries_lba = t->backup_ok ? le64(bh + 72) : 0;
    for (uint32_t i = 0; i < t->max_entries; i++) {
        const uint8_t *en = e + (size_t)i * t->entry_size;
        if (guid_zero(en))
            continue;
        PtPart *p = add_part(t);
        p->num = (int)i + 1;
        memcpy(p->type_guid, en, 16);
        memcpy(p->guid, en + 16, 16);
        uint64_t first = le64(en + 32), last = le64(en + 40);
        p->start = first;
        p->size = last >= first ? last - first + 1 : 0;
        p->attrs = le64(en + 48);
        uint16_t name[37];
        for (int k = 0; k < 36; k++)
            name[k] = le16(en + 56 + 2 * k);
        name[36] = 0;
        char *u = ucs2_to_utf8(name, (size_t)-1);
        snprintf(p->name, sizeof(p->name), "%s", u);
        free(u);
        if (last < first || first < t->first_usable || last > t->last_usable)
            note(t, "partition %d lies outside the usable area of the disk", p->num);
    }
    free(ph), free(pe), free(bh), free(be);
}

/* ---- the disk ---- */

int pt_read(const PtDev *d, PtTable *t)
{
    memset(t, 0, sizeof(*t));
    t->bsize = d->bsize;
    t->nblocks = d->nblocks;
    if (d->bsize < 512 || d->nblocks < 1)
        return PAL_EINVAL;
    uint8_t *b = read_blocks(d, 0, 1);
    if (!b)
        return PAL_EIO;
    memcpy(t->lba0, b, 512);
    bool sig = b[510] == 0x55 && b[511] == 0xAA;
    bool valid = sig, protective = false, others = false;
    for (int i = 0; i < 4 && sig; i++) {
        const uint8_t *e = b + 446 + i * 16;
        if (e[0] != 0 && e[0] != 0x80)
            valid = false;
        if (e[4] == 0xEE)
            protective = true;
        else if (e[4])
            others = true;
    }
    if (valid && protective) {
        t->hybrid = others;
        read_gpt(d, t);
        if (t->hybrid && t->kind == PT_GPT)
            note(t, "hybrid MBR: block 0 also describes partitions for legacy systems");
    } else if (valid && !fs_boot_sector(b)) {
        read_mbr(d, t, b);
    } else {
        /* no MBR: a GPT whose protective MBR was wiped is still a GPT */
        uint8_t *h = read_blocks(d, 1, 1);
        bool gpt = h && !memcmp(h, "EFI PART", 8);
        free(h);
        if (gpt) {
            read_gpt(d, t);
            if (t->kind == PT_GPT)
                note(t, "the protective MBR is missing");
        } else if (sig && fs_boot_sector(b))
            note(t, "no partition table: a file system covers the whole disk");
        else if (sig)
            note(t, "block 0 ends like an MBR, but its partition entries are invalid");
    }
    free(b);
    return 0;
}

void pt_free(PtTable *t)
{
    free(t->parts);
    t->parts = NULL;
    t->nparts = 0;
}

/* ---- type names ---- */

static const struct {
    const char *guid;
    const char *name;
} gpt_types[] = {
    { "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI system" },
    { "21686148-6449-6E6F-744E-656564454649", "BIOS boot" },
    { "E3C9E316-0B5C-4DB8-817D-F92DF00215AE", "Microsoft reserved" },
    { "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "Microsoft basic data" },
    { "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", "Windows recovery" },
    { "5808C8AA-7E8F-42E0-85D2-E1E90434CFB3", "Windows LDM metadata" },
    { "AF9B60A0-1431-4F62-BC68-3311714A69AD", "Windows LDM data" },
    { "0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux filesystem" },
    { "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709", "Linux root (x86-64)" },
    { "933AC7E1-2EB4-4F13-B844-0E14E2AEF915", "Linux home" },
    { "BC13C2FF-59E6-4262-A352-B275FD6F7172", "Linux extended boot" },
    { "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "Linux swap" },
    { "E6D6D379-F507-44C2-A23C-238F2A3DF928", "Linux LVM" },
    { "A19D880F-05FC-4D3B-A006-743F0F84911E", "Linux RAID" },
    { "48465300-0000-11AA-AA11-00306543ECAC", "Apple HFS+" },
    { "7C3457EF-0000-11AA-AA11-00306543ECAC", "Apple APFS" },
};

static const struct {
    uint8_t type;
    const char *name;
} mbr_types[] = {
    { 0x01, "FAT12" },           { 0x04, "FAT16 <32M" },       { 0x05, "Extended" },
    { 0x06, "FAT16" },           { 0x07, "NTFS/exFAT" },       { 0x0B, "FAT32" },
    { 0x0C, "FAT32 (LBA)" },     { 0x0E, "FAT16 (LBA)" },      { 0x0F, "Extended (LBA)" },
    { 0x11, "Hidden FAT12" },    { 0x14, "Hidden FAT16 <32M" }, { 0x16, "Hidden FAT16" },
    { 0x17, "Hidden NTFS" },     { 0x1B, "Hidden FAT32" },     { 0x1C, "Hidden FAT32 (LBA)" },
    { 0x1E, "Hidden FAT16 (LBA)" }, { 0x27, "Windows recovery" }, { 0x42, "Windows LDM" },
    { 0x82, "Linux swap" },      { 0x83, "Linux" },            { 0x85, "Linux extended" },
    { 0x8E, "Linux LVM" },       { 0xA5, "FreeBSD" },          { 0xA6, "OpenBSD" },
    { 0xAF, "Apple HFS+" },      { 0xEE, "GPT protective" },   { 0xEF, "EFI system" },
    { 0xFD, "Linux RAID" },
};

const char *pt_type_name(const PtTable *t, const PtPart *p)
{
    if (t->kind == PT_GPT) {
        char g[37];
        pt_guid_str(p->type_guid, g);
        for (size_t i = 0; i < ARRAY_SIZE(gpt_types); i++)
            if (!strcmp(g, gpt_types[i].guid))
                return gpt_types[i].name;
        return NULL;
    }
    for (size_t i = 0; i < ARRAY_SIZE(mbr_types); i++)
        if (mbr_types[i].type == p->mbr_type)
            return mbr_types[i].name;
    return NULL;
}
