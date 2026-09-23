/* partmgr.efi: the partition manager that ships with NESH (decision D21).
 * A full-screen program: screen 1 lists the disks, screen 2 shows the
 * partitions and the free space of one disk in disk order.
 *
 * This first version only reads: changing tables, Write and wipe come with
 * the rest of the interface. */
#include "disks.h"
#include "../pal/pal.h"

const char app_name[] = "partmgr";

#define PARTMGR_VERSION "0.1"

/* EFI text colours */
enum { BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LIGHTGRAY, DARKGRAY, LIGHTBLUE, LIGHTGREEN, LIGHTCYAN,
       LIGHTRED, LIGHTMAGENTA, YELLOW, WHITE };

static int cols = 80, rows = 25;

/* ---- drawing ---- */

static void put(const char *s)
{
    pal_con_write(s, strlen(s));
}

/* Writes TEXT at COL,ROW in the given colours, padded with spaces or cut
 * to WIDTH columns (counted in characters, not bytes). */
static void text_at(int col, int row, int width, int fg, int bg, const char *text)
{
    if (row < 0 || row >= rows || col >= cols)
        return;
    if (col + width > cols)
        width = cols - col;
    if (row == rows - 1 && col + width == cols)
        width--; /* writing the last cell scrolls some consoles */
    pal_con_set_color(fg, bg);
    pal_con_set_cursor(col, row);
    size_t len = strlen(text), cut = utf8_offset(text, len, (size_t)width);
    size_t shown = utf8_len(text, cut);
    pal_con_write(text, cut);
    for (size_t i = shown; i < (size_t)width; i++)
        put(" ");
}

static void textf_at(int col, int row, int width, int fg, int bg, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
static void textf_at(int col, int row, int width, int fg, int bg, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    text_at(col, row, width, fg, bg, buf);
}

static void title_bar(const char *left, const char *right)
{
    char r[128];
    snprintf(r, sizeof(r), "%s ", right);
    int rl = (int)utf8_len(r, strlen(r));
    textf_at(0, 0, cols - rl, WHITE, BLUE, " partmgr %s   %s", PARTMGR_VERSION, left);
    text_at(cols - rl, 0, rl, WHITE, BLUE, r);
}

static void key_bar(const char *keys)
{
    text_at(0, rows - 1, cols, BLACK, LIGHTGRAY, keys);
}

static void clear_body(void)
{
    for (int r = 1; r < rows - 1; r++)
        text_at(0, r, cols, LIGHTGRAY, BLACK, "");
}

/* 512 B, 20.0 KiB, 512.0 MiB, 30.8 GiB, 1.8 TiB */
static void fmt_size(char *out, size_t n, uint64_t bytes)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    int u = 0;
    uint64_t whole = bytes, tenth = 0;
    while (whole >= 1024 && u < 5) {
        tenth = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        u++;
    }
    if (u == 0)
        snprintf(out, n, "%llu B", (unsigned long long)bytes);
    else
        snprintf(out, n, "%llu.%llu %s", (unsigned long long)whole, (unsigned long long)tenth, units[u]);
}

static PalKey wait_key(void)
{
    PalKey k;
    while (!pal_con_read_key(&k, -1))
        ;
    return k;
}

static bool is_enter(const PalKey *k)
{
    return k->ch == '\r' || k->ch == '\n';
}

static bool is_back(const PalKey *k)
{
    return k->scan == KEY_ESC || k->ch == 27;
}

/* ---- screen 2: one disk ---- */

typedef struct {
    bool free;
    const PtPart *p; /* a partition */
    PtFree f;        /* or a free area */
    uint64_t start;
} Row;

static int row_cmp(const void *a, const void *b)
{
    const Row *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static const char *table_name(int kind)
{
    return kind == PT_GPT ? "GPT" : kind == PT_MBR ? "MBR" : "no table";
}

static void show_disk(PmDisk *d)
{
    PtTable t;
    char size[32];
    if (pt_read(&d->dev, &t)) {
        clear_body();
        text_at(2, 3, cols - 4, WHITE, RED, " The disk could not be read. Press a key. ");
        wait_key();
        return;
    }
    PtFree *fr;
    int nf = pt_free_space(&t, &fr);
    Row *rw = xcalloc(t.nparts + nf + 1, sizeof(Row));
    int n = 0;
    for (int i = 0; i < t.nparts; i++)
        rw[n++] = (Row){ false, &t.parts[i], { 0 }, t.parts[i].start };
    for (int i = 0; i < nf; i++)
        rw[n++] = (Row){ true, NULL, fr[i], fr[i].start };
    qsort(rw, n, sizeof(Row), row_cmp);

    int sel = 0, top = 0;
    uint64_t bs = t.bsize;
    for (;;) {
        fmt_size(size, sizeof(size), d->size);
        char left[160];
        snprintf(left, sizeof(left), "%s  %s  %s  %s", d->name, d->kind, size, table_name(t.kind));
        title_bar(left, d->boot ? "started from here: read only" : d->readonly ? "write-protected" : "");
        clear_body();
        bool gpt = t.kind == PT_GPT;
        textf_at(0, 2, cols, WHITE, BLACK, "  %-3s %-11s %-11s %-22s %-9s %s", "#", "Start", "Size", "Type",
                 gpt ? "" : "Flags", gpt ? "Name" : "");
        int first_row = 3, notes = t.nnotes ? t.nnotes + 1 : 0;
        int visible = rows - 1 - first_row - notes;
        if (visible < 1)
            visible = 1;
        if (sel < top)
            top = sel;
        if (sel >= top + visible)
            top = sel - visible + 1;
        if (!n)
            text_at(2, first_row, cols - 2, LIGHTGRAY, BLACK,
                    t.kind == PT_NONE ? "This disk has no partition table." : "This disk has no free space.");
        for (int i = top; i < n && i < top + visible; i++) {
            const Row *r = &rw[i];
            char st[32], sz[32], line[256];
            bool hl = i == sel;
            int fg = hl ? BLACK : LIGHTGRAY, bg = hl ? CYAN : BLACK;
            if (r->free) {
                fmt_size(st, sizeof(st), r->f.start * bs);
                fmt_size(sz, sizeof(sz), r->f.size * bs);
                snprintf(line, sizeof(line), "  %-3s %-11s %-11s %s", "-", st, sz,
                         r->f.logical ? "free space (for logical partitions)" : "free space");
                if (!hl)
                    fg = DARKGRAY;
            } else {
                const PtPart *p = r->p;
                const char *tn = pt_type_name(&t, p);
                char type[40], flags[16] = "";
                if (tn)
                    snprintf(type, sizeof(type), "%s", tn);
                else if (gpt)
                    pt_guid_str(p->type_guid, type), type[18] = 0;
                else
                    snprintf(type, sizeof(type), "type %02X", p->mbr_type);
                if (!gpt)
                    snprintf(flags, sizeof(flags), "%s", p->active ? "active" : p->role == PT_LOGICAL ? "logical" : "");
                fmt_size(st, sizeof(st), p->start * bs);
                fmt_size(sz, sizeof(sz), p->size * bs);
                snprintf(line, sizeof(line), "  %-3d %-11s %-11s %-22s %-9s %s", p->num, st, sz, type, flags,
                         gpt ? p->name : "");
            }
            text_at(0, first_row + i - top, cols, fg, bg, line);
        }
        for (int i = 0; i < t.nnotes; i++)
            textf_at(0, rows - 1 - t.nnotes + i, cols, YELLOW, BLACK, "  Note: %s", t.notes[i]);
        key_bar(" ↑↓ Move   Esc Back      (this version of partmgr only reads)");
        PalKey k = wait_key();
        if (is_back(&k))
            break;
        if (k.scan == KEY_UP && sel > 0)
            sel--;
        else if (k.scan == KEY_DOWN && sel + 1 < n)
            sel++;
        else if (k.scan == KEY_HOME)
            sel = 0;
        else if (k.scan == KEY_END && n)
            sel = n - 1;
    }
    free(rw);
    free(fr);
    pt_free(&t);
}

/* ---- screen 1: the disks ---- */

int app_main(int argc, char **argv)
{
    (void)argc, (void)argv;
    pal_con_show_cursor(false);
    int sel = 0;
    bool quit = false;
    while (!quit) {
        pal_con_size(&cols, &rows);
        PmDisk *d;
        int n = pm_disks(&d);
        if (sel >= n)
            sel = n ? n - 1 : 0;
        title_bar("", "Select a disk");
        clear_body();
        textf_at(0, 2, cols, WHITE, BLACK, "  %-7s %-6s %-11s %-9s %s", "Disk", "Type", "Size", "Table", "Partitions");
        if (!n)
            text_at(2, 4, cols - 2, LIGHTGRAY, BLACK, "No disks found.");
        for (int i = 0; i < n; i++) {
            PtTable t;
            char size[32], parts[64] = "";
            fmt_size(size, sizeof(size), d[i].size);
            int kind = -1;
            if (!pt_read(&d[i].dev, &t)) {
                kind = t.kind;
                int np = 0;
                for (int j = 0; j < t.nparts; j++)
                    np += t.parts[j].role != PT_EXTENDED;
                if (t.kind != PT_NONE)
                    snprintf(parts, sizeof(parts), "%d partition%s", np, np == 1 ? "" : "s");
                pt_free(&t);
            }
            char line[256];
            snprintf(line, sizeof(line), "  %-7s %-6s %-11s %-9s %-14s %s", d[i].name, d[i].kind, size,
                     kind < 0 ? "unreadable" : table_name(kind), parts,
                     d[i].boot ? "started from here: read only" : d[i].readonly ? "write-protected" : "");
            bool hl = i == sel;
            text_at(0, 3 + i, cols, hl ? BLACK : LIGHTGRAY, hl ? CYAN : BLACK, line);
        }
        key_bar(" ↑↓ Move   Enter Open   R Rescan   Q Quit");
        PalKey k = wait_key();
        if (k.ch == 'q' || k.ch == 'Q' || is_back(&k))
            quit = true;
        else if (k.scan == KEY_UP && sel > 0)
            sel--;
        else if (k.scan == KEY_DOWN && sel + 1 < n)
            sel++;
        else if (is_enter(&k) && n)
            show_disk(&d[sel]);
    }
    pal_con_reset_color();
    pal_con_clear();
    pal_con_show_cursor(true);
    return 0;
}
