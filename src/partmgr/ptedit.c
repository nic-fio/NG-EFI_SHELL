/* partmgr: changes to a partition table in memory. Nothing here touches the
 * disk; pt_write (ptwrite.c) writes the result.
 *
 * Partitions start on 1 MiB boundaries (pt_align). On MBR disks each logical
 * partition needs an extended boot record in a free block before it, and the
 * first record of the chain is always the first block of the extended
 * partition. So inside the extended partition the first block is reserved,
 * each logical partition occupies the block just before it as well, and a
 * new logical partition needs a free block before its own start. A logical
 * partition added where no extended partition exists creates one over the
 * whole free area, starting at that area and holding the first record.
 * ptwrite.c places the records by the same rule. */
#include "ptint.h"

#define MBR_LIMIT 0xFFFFFFFFull /* last block an MBR can address (2 TiB with 512-byte blocks) */

uint64_t pt_align(const PtTable *t)
{
    return 1024 * 1024 / t->bsize;
}

static uint64_t round_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) / a * a;
}

static uint64_t last_of(const PtPart *p)
{
    return p->start + p->size - 1;
}

PtPart *pt_find(PtTable *t, int num)
{
    for (int i = 0; i < t->nparts; i++)
        if (t->parts[i].num == num)
            return &t->parts[i];
    return NULL;
}

static PtPart *extended(PtTable *t)
{
    for (int i = 0; i < t->nparts; i++)
        if (t->parts[i].role == PT_EXTENDED)
            return &t->parts[i];
    return NULL;
}

static void random_guid(const PtDev *dev, uint8_t g[16])
{
    dev->random(dev->ctx, g, 16);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40); /* version 4: random */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80); /* RFC 4122 variant */
}

void pt_new(PtTable *t, const PtDev *dev, int kind)
{
    pt_free(t);
    memset(t, 0, sizeof(*t));
    t->kind = kind;
    t->bsize = dev->bsize;
    t->nblocks = dev->nblocks;
    t->changed = true;
    if (kind == PT_MBR) {
        while (!t->mbr_sig)
            dev->random(dev->ctx, &t->mbr_sig, sizeof(t->mbr_sig));
    } else if (kind == PT_GPT) {
        random_guid(dev, t->disk_guid);
        t->max_entries = 128;
        t->entry_size = 128;
        uint64_t arr = (128 * 128 + t->bsize - 1) / t->bsize;
        t->first_usable = 2 + arr;
        t->last_usable = t->nblocks > 2 * arr + 3 ? t->nblocks - 2 - arr : 0;
        t->primary_entries_lba = 2;
        t->backup_lba = t->nblocks - 1;
        t->backup_entries_lba = t->nblocks - 1 - arr;
        t->primary_ok = t->backup_ok = true;
    }
}

/* ---- free space ---- */

typedef struct {
    uint64_t first, last; /* inclusive */
} Span;

static int span_cmp(const void *a, const void *b)
{
    uint64_t x = ((const Span *)a)->first, y = ((const Span *)b)->first;
    return x < y ? -1 : x > y;
}

/* The gaps of [lo, hi] not covered by SPANS, raw (not aligned). */
static int gaps(uint64_t lo, uint64_t hi, Span *spans, int n, Span *out)
{
    qsort(spans, n, sizeof(Span), span_cmp);
    int k = 0;
    uint64_t at = lo;
    for (int i = 0; i <= n && at <= hi; i++) {
        uint64_t end = i < n ? spans[i].first : hi + 1; /* the gap ends before this span */
        if (end > at)
            out[k++] = (Span){ at, MIN(end - 1, hi) };
        if (i < n && spans[i].last + 1 > at)
            at = spans[i].last + 1;
    }
    return k;
}

/* Raw gaps at the top level (GPT: the usable area; MBR: outside the
 * extended partition) and inside the extended partition. */
static int top_gaps(PtTable *t, Span *out)
{
    Span *s = xcalloc(t->nparts + 1, sizeof(Span));
    int n = 0;
    for (int i = 0; i < t->nparts; i++)
        if (t->parts[i].role != PT_LOGICAL)
            s[n++] = (Span){ t->parts[i].start, last_of(&t->parts[i]) };
    int k = t->kind == PT_GPT ? gaps(t->first_usable, t->last_usable, s, n, out)
                              : gaps(1, MIN(t->nblocks, MBR_LIMIT + 1) - 1, s, n, out);
    free(s);
    return k;
}

static int logical_gaps(PtTable *t, const PtPart *ext, Span *out)
{
    Span *s = xcalloc(t->nparts + 2, sizeof(Span));
    int n = 0;
    s[n++] = (Span){ ext->start, ext->start }; /* the first record of the chain */
    for (int i = 0; i < t->nparts; i++)
        if (t->parts[i].role == PT_LOGICAL)
            s[n++] = (Span){ t->parts[i].start - 1, last_of(&t->parts[i]) };
    int k = gaps(ext->start, last_of(ext), s, n, out);
    free(s);
    return k;
}

static int free_cmp(const void *a, const void *b)
{
    uint64_t x = ((const PtFree *)a)->start, y = ((const PtFree *)b)->start;
    return x < y ? -1 : x > y;
}

int pt_free_space(const PtTable *ct, PtFree **out)
{
    PtTable *t = (PtTable *)ct;
    *out = NULL;
    if (t->kind == PT_NONE)
        return 0;
    uint64_t al = pt_align(t);
    Span *g = xcalloc(2 * t->nparts + 2, sizeof(Span));
    PtFree *f = xcalloc(2 * t->nparts + 2, sizeof(PtFree));
    int nf = 0;
    int n = top_gaps(t, g);
    for (int i = 0; i < n; i++) {
        uint64_t s = round_up(g[i].first, al);
        if (s <= g[i].last && g[i].last - s + 1 >= al)
            f[nf++] = (PtFree){ s, g[i].last - s + 1, false };
    }
    PtPart *ext = extended(t);
    if (ext) {
        n = logical_gaps(t, ext, g);
        for (int i = 0; i < n; i++) {
            uint64_t s = round_up(g[i].first + 1, al); /* the record goes in g[i].first */
            if (s <= g[i].last && g[i].last - s + 1 >= al)
                f[nf++] = (PtFree){ s, g[i].last - s + 1, true };
        }
    }
    free(g);
    qsort(f, nf, sizeof(PtFree), free_cmp);
    *out = f;
    return nf;
}

/* ---- adding and deleting ---- */

static int part_cmp(const void *a, const void *b)
{
    const PtPart *x = a, *y = b;
    /* primary and extended by number, logical ones after them in disk order */
    if ((x->role == PT_LOGICAL) != (y->role == PT_LOGICAL))
        return x->role == PT_LOGICAL ? 1 : -1;
    if (x->role == PT_LOGICAL)
        return x->start < y->start ? -1 : x->start > y->start;
    return x->num - y->num;
}

/* Logical partitions are numbered 5, 6... in disk order, as Linux does. */
static void renumber(PtTable *t)
{
    qsort(t->parts, t->nparts, sizeof(PtPart), part_cmp);
    int n = 5;
    for (int i = 0; i < t->nparts; i++) {
        if (t->parts[i].role != PT_LOGICAL)
            continue;
        if (t->parts[i].num != n) {
            t->parts[i].num = n;
            t->parts[i].changed = true;
        }
        n++;
    }
}

static bool overlaps(uint64_t a1, uint64_t a2, uint64_t b1, uint64_t b2)
{
    return a1 <= b2 && b1 <= a2;
}

static PtPart *append(PtTable *t, const PtPart *p)
{
    t->parts = xrealloc(t->parts, (t->nparts + 1) * sizeof(PtPart));
    t->parts[t->nparts] = *p;
    t->parts[t->nparts].changed = true;
    t->changed = true;
    return &t->parts[t->nparts++];
}

static int free_primary_slot(PtTable *t)
{
    for (int n = 1; n <= 4; n++)
        if (!pt_find(t, n))
            return n;
    return 0;
}

static const char *add_gpt(PtTable *t, const PtDev *dev, const PtPart *req, uint64_t last)
{
    static uint8_t zero[16];
    if (!memcmp(req->type_guid, zero, 16))
        return "choose a partition type";
    if (req->start < t->first_usable || last > t->last_usable)
        return "the partition lies outside the usable area of the disk";
    for (int i = 0; i < t->nparts; i++)
        if (overlaps(req->start, last, t->parts[i].start, last_of(&t->parts[i])))
            return "the partition overlaps another one";
    int num = 0;
    for (uint32_t n = 1; n <= t->max_entries && !num; n++)
        if (!pt_find(t, (int)n))
            num = (int)n;
    if (!num)
        return "the partition table is full";
    size_t units;
    uint16_t *u = utf8_to_ucs2(req->name, &units);
    free(u);
    if (units > 36)
        return "the name is too long (at most 36 characters)";
    PtPart *p = append(t, req);
    p->num = num;
    p->role = PT_PRIMARY;
    p->ebr_lba = 0;
    random_guid(dev, p->guid);
    return NULL;
}

static const char *add_logical(PtTable *t, const PtPart *req, uint64_t last)
{
    PtPart *ext = extended(t);
    uint64_t ebr;
    Span g[512];
    if (!ext) {
        /* a new extended partition over the whole free area, holding the first record */
        int slot = free_primary_slot(t);
        if (!slot)
            return "no room for an extended partition: the MBR already has four partitions";
        if (2 * t->nparts + 2 > (int)ARRAY_SIZE(g))
            return "too many partitions";
        int n = top_gaps(t, g);
        const Span *in = NULL;
        for (int i = 0; i < n && !in; i++)
            if (req->start >= g[i].first && req->start <= g[i].last)
                in = &g[i];
        if (!in || last > in->last)
            return "the partition does not fit in a free area";
        uint64_t es = round_up(in->first, pt_align(t));
        if (req->start <= es)
            return "a logical partition must start at least 1 MiB into the free area";
        PtPart e = { 0 };
        e.num = slot;
        e.role = PT_EXTENDED;
        e.mbr_type = in->last > 0xFFFFFFull ? 0x0F : 0x05; /* 0F beyond the reach of CHS (8 GB) */
        e.start = es;
        e.size = in->last - es + 1;
        ext = append(t, &e);
        ebr = es;
    } else {
        if (req->start <= ext->start || last > last_of(ext))
            return "the partition does not fit in the extended partition";
        if (2 * t->nparts + 2 > (int)ARRAY_SIZE(g))
            return "too many partitions";
        int n = logical_gaps(t, ext, g);
        const Span *in = NULL;
        for (int i = 0; i < n && !in; i++)
            if (req->start > g[i].first && req->start <= g[i].last)
                in = &g[i];
        if (!in || last > in->last)
            return "the partition overlaps another one, or leaves no room for its table";
        ebr = in->first;
    }
    PtPart *p = append(t, req);
    p->role = PT_LOGICAL;
    p->ebr_lba = ebr;
    p->num = 99;
    renumber(t);
    return NULL;
}

const char *pt_add(PtTable *t, const PtDev *dev, const PtPart *req)
{
    if (t->kind == PT_NONE)
        return "the disk has no partition table: create one first";
    if (!req->size)
        return "the size is zero";
    uint64_t last = req->start + req->size - 1;
    if (last < req->start || last >= t->nblocks)
        return "the partition goes beyond the end of the disk";
    if (t->kind == PT_GPT)
        return add_gpt(t, dev, req, last);
    if (!req->mbr_type)
        return "choose a partition type";
    if (pt_mbr_extended(req->mbr_type))
        return "the extended partition is created by adding a logical partition";
    if (last > MBR_LIMIT)
        return "an MBR cannot address beyond 2 TiB: use GPT";
    if (!req->start)
        return "block 0 holds the partition table";
    if (req->role == PT_LOGICAL)
        return add_logical(t, req, last);
    for (int i = 0; i < t->nparts; i++)
        if (t->parts[i].role != PT_LOGICAL && overlaps(req->start, last, t->parts[i].start, last_of(&t->parts[i])))
            return "the partition overlaps another one";
    int slot = free_primary_slot(t);
    if (!slot)
        return "an MBR holds at most four primary partitions, the extended one included";
    PtPart *p = append(t, req);
    p->num = slot;
    p->role = PT_PRIMARY;
    p->ebr_lba = 0;
    renumber(t);
    return NULL;
}

static void remove_at(PtTable *t, int i)
{
    memmove(&t->parts[i], &t->parts[i + 1], (t->nparts - i - 1) * sizeof(PtPart));
    t->nparts--;
    t->changed = true;
}

const char *pt_delete(PtTable *t, int num)
{
    PtPart *p = pt_find(t, num);
    if (!p)
        return "there is no such partition";
    bool ext = p->role == PT_EXTENDED;
    remove_at(t, (int)(p - t->parts));
    if (ext)
        for (int i = t->nparts - 1; i >= 0; i--)
            if (t->parts[i].role == PT_LOGICAL)
                remove_at(t, i);
    renumber(t);
    return NULL;
}

/* ---- changing a partition ---- */

const char *pt_set_type(PtTable *t, int num, uint8_t mbr_type, const uint8_t type_guid[16])
{
    PtPart *p = pt_find(t, num);
    if (!p)
        return "there is no such partition";
    if (t->kind == PT_GPT) {
        static uint8_t zero[16];
        if (!memcmp(type_guid, zero, 16))
            return "choose a partition type";
        memcpy(p->type_guid, type_guid, 16);
    } else {
        if (!mbr_type)
            return "choose a partition type";
        if (pt_mbr_extended(mbr_type) != (p->role == PT_EXTENDED))
            return p->role == PT_EXTENDED ? "the extended partition can only have an extended type"
                                          : "only the extended partition can have an extended type";
        p->mbr_type = mbr_type;
    }
    p->changed = t->changed = true;
    return NULL;
}

const char *pt_set_name(PtTable *t, int num, const char *name)
{
    PtPart *p = pt_find(t, num);
    if (!p)
        return "there is no such partition";
    if (t->kind != PT_GPT)
        return "only GPT partitions have a name";
    size_t units;
    uint16_t *u = utf8_to_ucs2(name, &units);
    free(u);
    if (units > 36)
        return "the name is too long (at most 36 characters)";
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->changed = t->changed = true;
    return NULL;
}

const char *pt_set_active(PtTable *t, int num, bool on)
{
    PtPart *p = pt_find(t, num);
    if (!p)
        return "there is no such partition";
    if (t->kind != PT_MBR)
        return "only MBR partitions have the active flag";
    if (p->role == PT_EXTENDED)
        return "the extended partition cannot be active";
    for (int i = 0; i < t->nparts && on; i++)
        if (t->parts[i].active && &t->parts[i] != p) {
            t->parts[i].active = false;
            t->parts[i].changed = true;
        }
    p->active = on;
    p->changed = t->changed = true;
    return NULL;
}
