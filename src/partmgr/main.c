/* partmgr.efi: the partition manager that ships with NESH (decision D21).
 * A full-screen program: this screen lists the disks, diskview.c shows one
 * disk and changes its partition table. */
#include "partmgr.h"

const char app_name[] = "partmgr";

const char *pm_table_name(int kind)
{
    return kind == PT_GPT ? "GPT" : kind == PT_MBR ? "MBR" : "no table";
}

static void draw(PmDisk *d, int n, int sel)
{
    char title[64];
    snprintf(title, sizeof(title), "partmgr %s", PARTMGR_VERSION);
    ui_title(title, "Select a disk");
    ui_clear_body();
    ui_textf(0, 2, ui_cols, WHITE, BLACK, "  %-7s %-6s %-11s %-9s %s", "Disk", "Type", "Size", "Table", "Partitions");
    if (!n)
        ui_text(2, 4, ui_cols - 2, LIGHTGRAY, BLACK, "No disks found.");
    for (int i = 0; i < n && 3 + i < ui_rows - 3; i++) {
        PtTable t;
        char size[32], parts[64] = "";
        pm_fmt_size(size, sizeof(size), d[i].size);
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
                 kind < 0 ? "unreadable" : pm_table_name(kind), parts,
                 d[i].boot ? "started from here: read only" : d[i].readonly ? "write-protected" : "");
        bool hl = i == sel;
        ui_text(0, 3 + i, ui_cols, hl ? BLACK : LIGHTGRAY, hl ? CYAN : BLACK, line);
    }
    ui_keys(1, "");
    ui_keys(0, " ↑↓ Move   Enter Open   R Rescan   Q Quit");
}

int app_main(int argc, char **argv)
{
    (void)argc, (void)argv;
    pal_con_show_cursor(false);
    int sel = 0;
    for (;;) {
        ui_init();
        PmDisk *d;
        int n = pm_disks(&d);
        if (sel >= n)
            sel = n ? n - 1 : 0;
        draw(d, n, sel);
        PalKey k = ui_key();
        if (k.ch == 'q' || k.ch == 'Q' || ui_is_esc(&k))
            break;
        if (k.scan == KEY_UP && sel > 0)
            sel--;
        else if (k.scan == KEY_DOWN && sel + 1 < n)
            sel++;
        else if (k.scan == KEY_HOME)
            sel = 0;
        else if (k.scan == KEY_END && n)
            sel = n - 1;
        else if (ui_is_enter(&k) && n)
            pm_disk_screen(&d[sel]);
        /* any other key, R included, reads the disks again */
    }
    pal_con_reset_color();
    pal_con_clear();
    pal_con_show_cursor(true);
    return 0;
}
