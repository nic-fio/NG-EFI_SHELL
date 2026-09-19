/* Interactive selection menu (MENU function). */
#include "shell.h"

static void goto_line(int start_row, int line, int total_up)
{
    if (pal_con_ansi()) {
        (void)start_row;
        (void)line;
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "\x1b[%dA\r", total_up);
        pal_con_write(buf, (size_t)n);
    } else {
        pal_con_set_cursor(0, start_row + line);
    }
}

static void draw(const char *title, char **items, int n, int sel, int countdown, int cols)
{
    int fg, bg;
    pal_con_get_color(&fg, &bg);
    pal_con_set_color(C_WHITE, bg);
    pal_con_write(title, strlen(title));
    pal_con_set_color(fg, bg);
    pal_con_write("\n", 1);
    for (int i = 0; i < n; i++) {
        char *line = xasprintf(" %2d. %s", i + 1, items[i]);
        size_t w = utf8_len(line, strlen(line));
        if (i == sel)
            pal_con_set_color(C_BLACK, C_LIGHTGRAY);
        pal_con_write(line, strlen(line));
        for (int k = (int)w; k < cols - 2 && k < 60; k++)
            pal_con_write(" ", 1);
        if (i == sel)
            pal_con_set_color(fg, bg);
        pal_con_write("\n", 1);
        free(line);
    }
    char *hint = countdown > 0
        ? xasprintf("Up/Down + Enter to choose, ESC to cancel. Default in %d s   ", countdown)
        : xstrdup("Up/Down + Enter to choose, ESC to cancel.                       ");
    pal_con_set_color(C_DARKGRAY, bg);
    pal_con_write(hint, strlen(hint));
    pal_con_set_color(fg, bg);
    free(hint);
}

int ui_menu(const char *title, char **items, int n, int timeout_s, int def)
{
    if (n <= 0)
        return 0;
    if (!pal_con_interactive())
        return def;
    int cols, rows;
    pal_con_size(&cols, &rows);
    int sel = def - 1;
    int countdown = timeout_s;
    pal_con_raw(true);
    pal_con_show_cursor(false);
    draw(title, items, n, sel, countdown, cols);
    int col, row;
    pal_con_get_cursor(&col, &row);
    int start_row = row - (n + 1);
    int result = 0;
    uint64_t next_tick = pal_ticks_ms() + 1000;
    for (;;) {
        PalKey k;
        int wait = countdown > 0 ? (int)(next_tick > pal_ticks_ms() ? next_tick - pal_ticks_ms() : 0) : -1;
        bool got = con_get_key(&k, wait);
        if (!got) {
            if (countdown > 0 && --countdown == 0) {
                result = sel + 1;
                break;
            }
            next_tick += 1000;
        } else {
            countdown = 0;
            if (k.scan == KEY_UP && sel > 0)
                sel--;
            else if (k.scan == KEY_DOWN && sel < n - 1)
                sel++;
            else if (k.scan == KEY_HOME)
                sel = 0;
            else if (k.scan == KEY_END)
                sel = n - 1;
            else if (k.ch == '\r' || k.ch == '\n') {
                result = sel + 1;
                break;
            } else if (k.scan == KEY_ESC || k.ch == 27) {
                result = 0;
                break;
            } else if (k.ch == 3) {
                result = -1;
                break;
            } else if (k.ch >= '1' && k.ch <= '9' && (int)(k.ch - '0') <= n) {
                result = (int)(k.ch - '0');
                sel = result - 1;
                goto_line(start_row, 0, n + 1);
                draw(title, items, n, sel, 0, cols);
                break;
            }
        }
        goto_line(start_row, 0, n + 1);
        draw(title, items, n, sel, countdown, cols);
    }
    pal_con_write("\n", 1);
    pal_con_show_cursor(true);
    pal_con_raw(false);
    return result;
}
