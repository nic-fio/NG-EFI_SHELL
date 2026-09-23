/* ptdump: prints what partmgr's table reader finds in a disk image file, one
 * key=value line per item. Linux only, used by tests/partmgr/run-tests.py to
 * compare the reader with sfdisk.
 *
 *     ptdump IMAGE [BLOCKSIZE]
 */
#include "../../src/partmgr/ptable.h"
#include "../../src/pal/pal.h"

void rt_fatal(const char *msg)
{
    fprintf(stderr, "ptdump: %s\n", msg);
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

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: ptdump IMAGE [BLOCKSIZE]\n");
        return 2;
    }
    Img im = { fopen(argv[1], "rb"), argc > 2 ? (uint32_t)atoi(argv[2]) : 512 };
    if (!im.f) {
        perror(argv[1]);
        return 2;
    }
    fseeko(im.f, 0, SEEK_END);
    PtDev dev = { img_read, &im, im.bsize, (uint64_t)ftello(im.f) / im.bsize };
    PtTable t;
    if (pt_read(&dev, &t)) {
        fprintf(stderr, "ptdump: cannot read %s\n", argv[1]);
        return 1;
    }
    static const char *kinds[] = { "none", "dos", "gpt" };
    static const char *roles[] = { "primary", "extended", "logical" };
    printf("label=%s\n", kinds[t.kind]);
    char g[37];
    if (t.kind == PT_MBR)
        printf("id=0x%08x\n", t.mbr_sig);
    if (t.kind == PT_GPT) {
        pt_guid_str(t.disk_guid, g);
        printf("id=%s\nfirstlba=%llu\nlastlba=%llu\nprimary=%s\nbackup=%s\nhybrid=%s\n", g,
               (unsigned long long)t.first_usable, (unsigned long long)t.last_usable, t.primary_ok ? "ok" : "bad",
               t.backup_ok ? "ok" : "bad", t.hybrid ? "yes" : "no");
    }
    for (int i = 0; i < t.nparts; i++) {
        const PtPart *p = &t.parts[i];
        const char *tn = pt_type_name(&t, p);
        printf("part num=%d start=%llu size=%llu", p->num, (unsigned long long)p->start,
               (unsigned long long)p->size);
        if (t.kind == PT_GPT) {
            char u[37];
            pt_guid_str(p->type_guid, g);
            pt_guid_str(p->guid, u);
            printf(" type=%s uuid=%s attrs=0x%llx name=%s", g, u, (unsigned long long)p->attrs, p->name);
        } else
            printf(" type=%x role=%s bootable=%s", p->mbr_type, roles[p->role], p->active ? "yes" : "no");
        printf(" typename=%s\n", tn ? tn : "?");
    }
    for (int i = 0; i < t.nnotes; i++)
        printf("note=%s\n", t.notes[i]);
    pt_free(&t);
    fclose(im.f);
    return 0;
}
