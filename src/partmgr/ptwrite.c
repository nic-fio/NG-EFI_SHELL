/* partmgr: writing partition tables, backups and restores.
 *
 * GPT: the protective MBR in block 0 (boot code as read, none for a new
 * table), the primary header in block 1 with its entry array from block 2,
 * the backup array and header at the end of the disk. Both copies are always
 * written, so writing also repairs a damaged copy and moves the backup to the
 * end of an enlarged disk. A hybrid MBR is left as it is.
 *
 * MBR: block 0 with up to four entries, and for the logical partitions a
 * chain of extended boot records in disk order, the first one at the start
 * of the extended partition. A GPT signature left by an earlier table is
 * wiped, or firmware and Linux would still find the old GPT.
 *
 * Wipe: two passes over a partition, random data then zeros, in chunks of
 * 4 MiB. The random data comes from xoshiro256**, seeded from dev->random:
 * the point is to overwrite every byte, not to be unpredictable.
 *
 * Backup file: "PARTMGR1", version, block size, disk size, number of extents,
 * then for each extent its first block, its length and the blocks; a CRC-32
 * of everything before it closes the file. All numbers little-endian. */
#include "ptint.h"
#include "../lib/crc32.h"
#include "../pal/pal.h"

static int wr(const PtDev *d, uint64_t lba, uint32_t count, const void *buf)
{
    if (!d->write)
        return PAL_EROFS;
    return d->write(d->ctx, lba, count, buf);
}

static uint8_t *rd(const PtDev *d, uint64_t lba, uint32_t count)
{
    uint8_t *b = xmalloc((size_t)count * d->bsize);
    if (lba + count > d->nblocks || d->read(d->ctx, lba, count, b)) {
        free(b);
        return NULL;
    }
    return b;
}

static bool inside_partition(const PtTable *t, uint64_t lba)
{
    for (int i = 0; i < t->nparts; i++)
        if (lba >= t->parts[i].start && lba < t->parts[i].start + t->parts[i].size)
            return true;
    return false;
}

/* Clears a GPT header at LBA if there is one and it is not inside a
 * partition of the new table. */
static int wipe_gpt_header(const PtDev *d, const PtTable *t, uint64_t lba)
{
    if (lba >= d->nblocks || inside_partition(t, lba))
        return 0;
    uint8_t *b = rd(d, lba, 1);
    int rc = 0;
    if (b && !memcmp(b, "EFI PART", 8)) {
        memset(b, 0, d->bsize);
        rc = wr(d, lba, 1, b);
    }
    free(b);
    return rc;
}

/* CHS address of an LBA with the usual 255 heads and 63 sectors; beyond
 * cylinder 1023 the maximum, which tells old systems to use the LBA. */
static void chs(uint8_t *p, uint64_t lba)
{
    uint64_t c = lba / (255 * 63);
    if (c > 1023) {
        p[0] = 0xFE, p[1] = 0xFF, p[2] = 0xFF;
        return;
    }
    p[0] = (uint8_t)(lba / 63 % 255);
    p[1] = (uint8_t)((lba % 63 + 1) | (c >> 2 & 0xC0));
    p[2] = (uint8_t)c;
}

static void mbr_entry(uint8_t *e, bool active, uint8_t type, uint64_t start, uint64_t size, uint64_t abs_start)
{
    e[0] = active ? 0x80 : 0;
    chs(e + 1, abs_start);
    e[4] = type;
    chs(e + 5, abs_start + size - 1);
    put32(e + 8, (uint32_t)start);
    put32(e + 12, (uint32_t)size);
}

/* ---- MBR ---- */

static int logical_cmp(const void *a, const void *b)
{
    const PtPart *x = *(const PtPart *const *)a, *y = *(const PtPart *const *)b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static int write_mbr(const PtDev *d, const PtTable *t)
{
    uint8_t *b = xcalloc(1, d->bsize);
    memcpy(b, t->lba0, 440); /* the boot code, zero for a new table */
    put32(b + 440, t->mbr_sig);
    const PtPart *ext = NULL;
    const PtPart **logs = xcalloc(t->nparts + 1, sizeof(PtPart *));
    int nl = 0;
    for (int i = 0; i < t->nparts; i++) {
        const PtPart *p = &t->parts[i];
        if (p->role == PT_LOGICAL) {
            logs[nl++] = p;
            continue;
        }
        if (p->role == PT_EXTENDED)
            ext = p;
        mbr_entry(b + 446 + (p->num - 1) * 16, p->active, p->mbr_type, p->start, p->size, p->start);
    }
    b[510] = 0x55, b[511] = 0xAA;
    int rc = wipe_gpt_header(d, t, 1);
    if (!rc)
        rc = wipe_gpt_header(d, t, d->nblocks - 1);
    if (!rc)
        rc = wr(d, 0, 1, b);
    free(b);
    if (rc || !ext) {
        free(logs);
        return rc;
    }
    /* the chain: record i at ebr[i] describes logical i and links to record i+1 */
    qsort(logs, nl, sizeof(PtPart *), logical_cmp);
    uint64_t *ebr = xcalloc(nl + 1, sizeof(uint64_t));
    for (int i = 0; i < nl; i++) {
        /* the first record at the start of the extended partition; the others
         * where they were, if that is still between the two partitions, or
         * else in the block just before their partition (kept free by ptedit.c) */
        uint64_t prev_end = i ? logs[i - 1]->start + logs[i - 1]->size - 1 : 0, at = logs[i]->ebr_lba;
        ebr[i] = i == 0 ? ext->start : at > prev_end && at < logs[i]->start ? at : logs[i]->start - 1;
    }
    uint8_t *e = xcalloc(1, d->bsize);
    if (!nl) {
        /* an extended partition without logical ones: an empty record */
        e[510] = 0x55, e[511] = 0xAA;
        rc = wr(d, ext->start, 1, e);
    }
    for (int i = 0; i < nl && !rc; i++) {
        memset(e, 0, d->bsize);
        const PtPart *p = logs[i];
        mbr_entry(e + 446, p->active, p->mbr_type, p->start - ebr[i], p->size, p->start);
        if (i + 1 < nl) {
            uint64_t next_end = logs[i + 1]->start + logs[i + 1]->size;
            mbr_entry(e + 462, false, 0x05, ebr[i + 1] - ext->start, next_end - ebr[i + 1], ebr[i + 1]);
        }
        e[510] = 0x55, e[511] = 0xAA;
        rc = wr(d, ebr[i], 1, e);
    }
    free(e);
    free(ebr);
    free(logs);
    return rc;
}

/* ---- GPT ---- */

static void gpt_header(uint8_t *h, const PtTable *t, uint64_t my, uint64_t alt, uint64_t entries, uint64_t last_usable,
                       uint32_t array_crc)
{
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x00010000);
    put32(h + 12, 92);
    put64(h + 24, my);
    put64(h + 32, alt);
    put64(h + 40, t->first_usable);
    put64(h + 48, last_usable);
    memcpy(h + 56, t->disk_guid, 16);
    put64(h + 72, entries);
    put32(h + 80, t->max_entries);
    put32(h + 84, t->entry_size);
    put32(h + 88, array_crc);
    put32(h + 16, crc32_update(0, h, 92));
}

static int write_gpt(const PtDev *d, const PtTable *t)
{
    size_t asize = (size_t)t->max_entries * t->entry_size;
    uint32_t ablocks = (uint32_t)((asize + d->bsize - 1) / d->bsize);
    uint64_t last_usable = d->nblocks - 2 - ablocks;
    if (t->first_usable < 2 + ablocks || t->first_usable > last_usable)
        return PAL_EINVAL;
    for (int i = 0; i < t->nparts; i++)
        if (t->parts[i].start < t->first_usable || t->parts[i].start + t->parts[i].size - 1 > last_usable)
            return PAL_EINVAL;
    uint8_t *arr = xcalloc(ablocks, d->bsize);
    for (int i = 0; i < t->nparts; i++) {
        const PtPart *p = &t->parts[i];
        uint8_t *en = arr + (size_t)(p->num - 1) * t->entry_size;
        memcpy(en, p->type_guid, 16);
        memcpy(en + 16, p->guid, 16);
        put64(en + 32, p->start);
        put64(en + 40, p->start + p->size - 1);
        put64(en + 48, p->attrs);
        size_t units;
        uint16_t *u = utf8_to_ucs2(p->name, &units);
        for (size_t k = 0; k < units && k < 36; k++)
            put16(en + 56 + 2 * k, u[k]);
        free(u);
    }
    uint32_t acrc = crc32_update(0, arr, asize);
    uint64_t back = d->nblocks - 1, back_entries = back - ablocks;
    uint8_t *h = xcalloc(1, d->bsize);
    int rc = 0;
    if (!t->hybrid) {
        memcpy(h, t->lba0, 440);
        uint64_t n = d->nblocks - 1;
        mbr_entry(h + 446, false, 0xEE, 1, n > 0xFFFFFFFFull ? 0xFFFFFFFFull : n, 1);
        h[447] = 0x00, h[448] = 0x02, h[449] = 0x00; /* CHS of block 1, as the specification gives it */
        h[510] = 0x55, h[511] = 0xAA;
        rc = wr(d, 0, 1, h);
    }
    if (!rc)
        rc = wr(d, 2, ablocks, arr);
    if (!rc) {
        memset(h, 0, d->bsize);
        gpt_header(h, t, 1, back, 2, last_usable, acrc);
        rc = wr(d, 1, 1, h);
    }
    if (!rc)
        rc = wr(d, back_entries, ablocks, arr);
    if (!rc) {
        memset(h, 0, d->bsize);
        gpt_header(h, t, back, 1, back_entries, last_usable, acrc);
        rc = wr(d, back, 1, h);
    }
    free(h);
    free(arr);
    return rc;
}

/* No table: block 0 and both GPT headers are cleared, which is what every
 * system looks at; the partitions' contents are not touched. */
static int erase(const PtDev *d)
{
    uint8_t *z = xcalloc(1, d->bsize);
    int rc = wr(d, 0, 1, z);
    if (!rc && d->nblocks > 2)
        rc = wr(d, 1, 1, z);
    if (!rc && d->nblocks > 2)
        rc = wr(d, d->nblocks - 1, 1, z);
    free(z);
    return rc;
}

int pt_write(const PtDev *d, const PtTable *t)
{
    if (t->bsize != d->bsize || t->nblocks != d->nblocks)
        return PAL_EINVAL;
    switch (t->kind) {
    case PT_GPT:
        return write_gpt(d, t);
    case PT_MBR:
        return write_mbr(d, t);
    default:
        return erase(d);
    }
}

/* ---- backup and restore ---- */

#define BK_MAGIC "PARTMGR1"
#define BK_VERSION 1

typedef struct {
    uint64_t lba;
    uint32_t count;
} Extent;

static int add_extent(Extent *x, int n, uint64_t lba, uint32_t count, uint64_t nblocks)
{
    if (!count || lba >= nblocks)
        return n;
    if (count > nblocks - lba)
        count = (uint32_t)(nblocks - lba);
    for (int i = 0; i < n; i++)
        if (x[i].lba == lba)
            return n;
    x[n].lba = lba;
    x[n].count = count;
    return n + 1;
}

int pt_backup(const PtDev *d, const PtTable *t, uint8_t **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    if (t->kind == PT_NONE)
        return PAL_ENOENT;
    Extent *x = xcalloc(t->nparts + 8, sizeof(Extent));
    int n = add_extent(x, 0, 0, 1, d->nblocks);
    if (t->kind == PT_GPT) {
        uint32_t ab = (uint32_t)(((uint64_t)t->max_entries * t->entry_size + d->bsize - 1) / d->bsize);
        n = add_extent(x, n, 1, 1, d->nblocks);
        n = add_extent(x, n, t->primary_entries_lba, ab, d->nblocks);
        n = add_extent(x, n, t->backup_lba, 1, d->nblocks);
        if (t->backup_entries_lba)
            n = add_extent(x, n, t->backup_entries_lba, ab, d->nblocks);
    } else {
        for (int i = 0; i < t->nparts; i++)
            if (t->parts[i].role == PT_LOGICAL)
                n = add_extent(x, n, t->parts[i].ebr_lba, 1, d->nblocks);
            else if (t->parts[i].role == PT_EXTENDED)
                n = add_extent(x, n, t->parts[i].start, 1, d->nblocks);
    }
    Sbuf b;
    sb_init(&b);
    uint8_t h[28];
    memcpy(h, BK_MAGIC, 8);
    put32(h + 8, BK_VERSION);
    put32(h + 12, d->bsize);
    put64(h + 16, d->nblocks);
    put32(h + 24, (uint32_t)n);
    sb_add(&b, (const char *)h, sizeof(h));
    int rc = 0;
    for (int i = 0; i < n && !rc; i++) {
        uint8_t *blk = rd(d, x[i].lba, x[i].count);
        if (!blk) {
            rc = PAL_EIO;
            break;
        }
        uint8_t eh[12];
        put64(eh, x[i].lba);
        put32(eh + 8, x[i].count);
        sb_add(&b, (const char *)eh, sizeof(eh));
        sb_add(&b, (const char *)blk, (size_t)x[i].count * d->bsize);
        free(blk);
    }
    free(x);
    if (rc) {
        sb_free(&b);
        return rc;
    }
    uint8_t c[4];
    put32(c, crc32_update(0, b.s, b.len));
    sb_add(&b, (const char *)c, 4);
    *len = b.len;
    *data = (uint8_t *)sb_steal(&b);
    return 0;
}

const char *pt_restore(const PtDev *d, const uint8_t *data, size_t len)
{
    if (len < 32 || memcmp(data, BK_MAGIC, 8))
        return "this is not a partmgr backup file";
    if (crc32_update(0, data, len - 4) != le32(data + len - 4))
        return "the backup file is damaged (wrong checksum)";
    if (le32(data + 8) != BK_VERSION)
        return "the backup file was made by a newer partmgr";
    if (le32(data + 12) != d->bsize)
        return "the backup was made on a disk with a different block size";
    if (le64(data + 16) != d->nblocks)
        return "the backup was made on a disk of a different size";
    /* check every extent before writing anything */
    uint32_t n = le32(data + 24);
    size_t at = 28;
    for (uint32_t i = 0; i < n; i++) {
        if (at + 12 > len - 4)
            return "the backup file is damaged (truncated)";
        uint64_t lba = le64(data + at);
        uint32_t count = le32(data + at + 8);
        size_t bytes = (size_t)count * d->bsize;
        if (!count || lba >= d->nblocks || count > d->nblocks - lba || at + 12 + bytes > len - 4)
            return "the backup file is damaged (bad block range)";
        at += 12 + bytes;
    }
    if (at != len - 4)
        return "the backup file is damaged (extra data)";
    PtTable cur;
    if (pt_read(d, &cur))
        return "the disk could not be read";
    /* an MBR backup over a disk that now has a GPT: the GPT headers must go */
    bool gpt = false;
    at = 28;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t count = le32(data + at + 8);
        if (le64(data + at) == 1 && !memcmp(data + at + 12, "EFI PART", 8))
            gpt = true;
        at += 12 + (size_t)count * d->bsize;
    }
    const char *err = NULL;
    at = 28;
    for (uint32_t i = 0; i < n && !err; i++) {
        uint64_t lba = le64(data + at);
        uint32_t count = le32(data + at + 8);
        if (wr(d, lba, count, data + at + 12))
            err = "the disk could not be written";
        at += 12 + (size_t)count * d->bsize;
    }
    /* an MBR backup over a disk that has a GPT now: the GPT goes, headers and
     * entry arrays, except where the restored partitions are */
    if (!err && !gpt && cur.kind == PT_GPT) {
        PtTable now;
        if (pt_read(d, &now))
            err = "the disk could not be read";
        uint32_t ab = (uint32_t)(((uint64_t)cur.max_entries * cur.entry_size + d->bsize - 1) / d->bsize);
        uint64_t ranges[2][2] = { { 1, cur.primary_entries_lba + ab - 1 }, { cur.backup_lba - ab, cur.backup_lba } };
        uint8_t *z = xcalloc(1, d->bsize);
        for (int r = 0; r < 2 && !err; r++)
            for (uint64_t lba = ranges[r][0]; lba <= ranges[r][1] && lba < d->nblocks && !err; lba++)
                if (!inside_partition(&now, lba) && wr(d, lba, 1, z))
                    err = "the disk could not be written";
        free(z);
        pt_free(&now);
    }
    pt_free(&cur);
    return err;
}

/* ---- wipe ---- */

#define WIPE_CHUNK (4u * 1024 * 1024)

static uint64_t rotl(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

/* xoshiro256** (Blackman and Vigna) */
static uint64_t next_random(uint64_t s[4])
{
    uint64_t r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return r;
}

int pt_wipe(const PtDev *d, uint64_t start, uint64_t count, PtProgressFn progress, void *ctx)
{
    if (!d->write)
        return PAL_EROFS;
    if (!count || start >= d->nblocks || count > d->nblocks - start)
        return PAL_EINVAL;
    uint32_t per = WIPE_CHUNK / d->bsize; /* blocks per chunk */
    uint64_t *buf = xmalloc(WIPE_CHUNK);
    uint64_t seed[4];
    do
        d->random(d->ctx, seed, sizeof(seed));
    while (!(seed[0] | seed[1] | seed[2] | seed[3]));
    int rc = 0;
    for (int pass = 1; pass <= 2 && !rc; pass++) {
        if (pass == 2)
            memset(buf, 0, WIPE_CHUNK);
        if (progress && !progress(ctx, pass, 0, count)) {
            rc = PAL_EABORT;
            break;
        }
        for (uint64_t done = 0; done < count && !rc;) {
            uint32_t n = (uint32_t)MIN((uint64_t)per, count - done);
            if (pass == 1)
                for (size_t i = 0; i < (size_t)n * d->bsize / 8; i++)
                    buf[i] = next_random(seed);
            rc = wr(d, start + done, n, buf);
            done += n;
            if (!rc && progress && !progress(ctx, pass, done, count))
                rc = PAL_EABORT;
        }
    }
    free(buf);
    return rc;
}
