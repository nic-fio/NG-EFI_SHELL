/* pttool: drives partmgr's partition table code on a disk image file, so
 * that tests/partmgr/run-tests.py can check it against sfdisk and parted.
 * Linux only. The commands run in order on the table read from the image;
 * at the end the table in memory is printed, one key=value line per item.
 *
 *     pttool IMAGE [-b BLOCKSIZE] [COMMAND...]
 *
 *     new gpt|mbr|none              an empty table (none: delete the table)
 *     add primary|logical START SIZE TYPE NAME
 *                                   TYPE: a hex byte (MBR) or a GUID (GPT);
 *                                   NAME: the GPT name, - for none
 *     del N | type N TYPE | name N NAME | active N on|off
 *     free                          print the free areas
 *     write                         write the table to the image
 *     reread                        read the table again from the image
 *     backup FILE | restore FILE
 */
#include "../../src/partmgr/ptable.h"
#include "../../src/pal/pal.h"

void rt_fatal(const char *msg)
{
    fprintf(stderr, "pttool: %s\n", msg);
    exit(2);
}

typedef struct {
    FILE *f;
    uint32_t bsize;
} Img;

static int img_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    Img *im = ctx;
    if (fseeko(im->f, (off_t)(lba * im->bsize), SEEK_SET))
        return PAL_EIO;
    return fread(buf, im->bsize, count, im->f) == count ? 0 : PAL_EIO;
}

static int img_write(void *ctx, uint64_t lba, uint32_t count, const void *buf)
{
    Img *im = ctx;
    if (fseeko(im->f, (off_t)(lba * im->bsize), SEEK_SET))
        return PAL_EIO;
    return fwrite(buf, im->bsize, count, im->f) == count && !fflush(im->f) ? 0 : PAL_EIO;
}

static void img_random(void *ctx, void *buf, size_t n)
{
    FILE *r = fopen("/dev/urandom", "rb");
    if (!r || fread(buf, 1, n, r) != n)
        rt_fatal("no /dev/urandom");
    fclose(r);
}

static void fail(const char *msg)
{
    printf("error=%s\n", msg);
    exit(1);
}

static void check(const char *msg)
{
    if (msg)
        fail(msg);
}

static void dump(const PtTable *t)
{
    static const char *kinds[] = { "none", "dos", "gpt" };
    static const char *roles[] = { "primary", "extended", "logical" };
    printf("label=%s\n", kinds[t->kind]);
    char g[37];
    if (t->kind == PT_MBR)
        printf("id=0x%08x\n", t->mbr_sig);
    if (t->kind == PT_GPT) {
        pt_guid_str(t->disk_guid, g);
        printf("id=%s\nfirstlba=%llu\nlastlba=%llu\nprimary=%s\nbackup=%s\nhybrid=%s\n", g,
               (unsigned long long)t->first_usable, (unsigned long long)t->last_usable,
               t->primary_ok ? "ok" : "bad", t->backup_ok ? "ok" : "bad", t->hybrid ? "yes" : "no");
    }
    for (int i = 0; i < t->nparts; i++) {
        const PtPart *p = &t->parts[i];
        const char *tn = pt_type_name(t, p);
        printf("part num=%d start=%llu size=%llu", p->num, (unsigned long long)p->start,
               (unsigned long long)p->size);
        if (t->kind == PT_GPT) {
            char u[37];
            pt_guid_str(p->type_guid, g);
            pt_guid_str(p->guid, u);
            printf(" type=%s uuid=%s attrs=0x%llx name=%s", g, u, (unsigned long long)p->attrs, p->name);
        } else
            printf(" type=%x role=%s bootable=%s", p->mbr_type, roles[p->role], p->active ? "yes" : "no");
        printf(" typename=%s\n", tn ? tn : "?");
    }
    for (int i = 0; i < t->nnotes; i++)
        printf("note=%s\n", t->notes[i]);
}

static void type_arg(const PtTable *t, const char *s, uint8_t *mbr, uint8_t guid[16])
{
    memset(guid, 0, 16);
    *mbr = 0;
    if (t->kind == PT_GPT) {
        if (!pt_guid_parse(s, guid))
            fail("bad GUID");
    } else
        *mbr = (uint8_t)strtoul(s, NULL, 16);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: pttool IMAGE [-b BLOCKSIZE] [COMMAND...]\n");
        return 2;
    }
    int a = 2;
    uint32_t bsize = 512;
    if (argc > 3 && !strcmp(argv[2], "-b")) {
        bsize = (uint32_t)atoi(argv[3]);
        a = 4;
    }
    Img im = { fopen(argv[1], "r+b"), bsize };
    if (!im.f) {
        perror(argv[1]);
        return 2;
    }
    fseeko(im.f, 0, SEEK_END);
    PtDev dev = { img_read, img_write, img_random, &im, bsize, (uint64_t)ftello(im.f) / bsize };
    PtTable t;
    if (pt_read(&dev, &t))
        fail("cannot read the image");
    while (a < argc) {
        const char *c = argv[a++];
#define ARG() (a < argc ? argv[a++] : (fail("missing argument"), ""))
        if (!strcmp(c, "new")) {
            const char *k = ARG();
            pt_new(&t, &dev, !strcmp(k, "gpt") ? PT_GPT : !strcmp(k, "mbr") ? PT_MBR : PT_NONE);
        } else if (!strcmp(c, "add")) {
            PtPart p = { 0 };
            p.role = !strcmp(ARG(), "logical") ? PT_LOGICAL : PT_PRIMARY;
            p.start = strtoull(ARG(), NULL, 10);
            p.size = strtoull(ARG(), NULL, 10);
            type_arg(&t, ARG(), &p.mbr_type, p.type_guid);
            const char *name = ARG();
            if (strcmp(name, "-"))
                snprintf(p.name, sizeof(p.name), "%s", name);
            check(pt_add(&t, &dev, &p));
        } else if (!strcmp(c, "del")) {
            check(pt_delete(&t, atoi(ARG())));
        } else if (!strcmp(c, "type")) {
            int n = atoi(ARG());
            uint8_t mbr, guid[16];
            type_arg(&t, ARG(), &mbr, guid);
            check(pt_set_type(&t, n, mbr, guid));
        } else if (!strcmp(c, "name")) {
            int n = atoi(ARG());
            check(pt_set_name(&t, n, ARG()));
        } else if (!strcmp(c, "active")) {
            int n = atoi(ARG());
            check(pt_set_active(&t, n, !strcmp(ARG(), "on")));
        } else if (!strcmp(c, "free")) {
            PtFree *f;
            int n = pt_free_space(&t, &f);
            for (int i = 0; i < n; i++)
                printf("free start=%llu size=%llu logical=%s\n", (unsigned long long)f[i].start,
                       (unsigned long long)f[i].size, f[i].logical ? "yes" : "no");
            free(f);
        } else if (!strcmp(c, "write")) {
            if (pt_write(&dev, &t))
                fail("the table could not be written");
        } else if (!strcmp(c, "reread")) {
            pt_free(&t);
            if (pt_read(&dev, &t))
                fail("cannot read the image");
        } else if (!strcmp(c, "backup")) {
            uint8_t *data;
            size_t len;
            if (pt_backup(&dev, &t, &data, &len))
                fail("nothing to back up");
            FILE *o = fopen(ARG(), "wb");
            if (!o || fwrite(data, 1, len, o) != len || fclose(o))
                fail("cannot write the backup file");
            free(data);
        } else if (!strcmp(c, "restore")) {
            FILE *in = fopen(ARG(), "rb");
            if (!in)
                fail("cannot open the backup file");
            fseek(in, 0, SEEK_END);
            size_t len = (size_t)ftell(in);
            fseek(in, 0, SEEK_SET);
            uint8_t *data = xmalloc(len ? len : 1);
            if (fread(data, 1, len, in) != len)
                fail("cannot read the backup file");
            fclose(in);
            check(pt_restore(&dev, data, len));
            free(data);
            pt_free(&t);
            if (pt_read(&dev, &t))
                fail("cannot read the image");
        } else
            fail("unknown command");
    }
    dump(&t);
    pt_free(&t);
    fclose(im.f);
    return 0;
}
