/* partmgr: drawing on the text console and the dialogs. Texts are UTF-8;
 * widths are counted in characters. A dialog is a box in the middle of the
 * screen: title bar, text (lines split at '\n' and wrapped), and for
 * questions and fields a last line to answer on. */
#include "ui.h"

int ui_cols = 80, ui_rows = 25;

void ui_init(void)
{
    pal_con_size(&ui_cols, &ui_rows);
    if (ui_cols < 40)
        ui_cols = 40;
    if (ui_rows < 12)
        ui_rows = 12;
}

void ui_text(int col, int row, int width, int fg, int bg, const char *text)
{
    if (row < 0 || row >= ui_rows || col >= ui_cols || width <= 0)
        return;
    if (col + width > ui_cols)
        width = ui_cols - col;
    if (row == ui_rows - 1 && col + width == ui_cols)
        width--; /* writing the last cell scrolls some consoles */
    pal_con_set_color(fg, bg);
    pal_con_set_cursor(col, row);
    size_t len = strlen(text), cut = utf8_offset(text, len, (size_t)width);
    size_t shown = utf8_len(text, cut);
    pal_con_write(text, cut);
    static const char spaces[] = "                                                                ";
    for (size_t left = (size_t)width - shown; left;) {
        size_t k = MIN(left, sizeof(spaces) - 1);
        pal_con_write(spaces, k);
        left -= k;
    }
}

void ui_textf(int col, int row, int width, int fg, int bg, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_text(col, row, width, fg, bg, buf);
}

void ui_title(const char *left, const char *right)
{
    char r[160];
    snprintf(r, sizeof(r), "%s ", right);
    int rl = (int)utf8_len(r, strlen(r));
    ui_textf(0, 0, ui_cols - rl, WHITE, BLUE, " %s", left);
    ui_text(ui_cols - rl, 0, rl, YELLOW, BLUE, r);
}

void ui_keys(int line, const char *keys)
{
    /* both lines one column short: the last cell of the screen cannot be written */
    ui_text(0, ui_rows - 1 - line, ui_cols - 1, BLACK, LIGHTGRAY, keys);
}

void ui_clear_body(void)
{
    for (int r = 1; r < ui_rows - 2; r++)
        ui_text(0, r, ui_cols, LIGHTGRAY, BLACK, "");
}

PalKey ui_key(void)
{
    PalKey k;
    while (!pal_con_read_key(&k, -1))
        ;
    return k;
}

bool ui_is_enter(const PalKey *k)
{
    return k->ch == '\r' || k->ch == '\n';
}

bool ui_is_esc(const PalKey *k)
{
    return k->scan == KEY_ESC || k->ch == 27;
}

/* ---- dialogs ---- */

#define MAXLINES 16

typedef struct {
    int col, row, w, h;
    int fg, bg;
} Box;

/* Splits TEXT into lines of at most WIDTH characters (at spaces when possible). */
static int wrap(const char *text, int width, char lines[MAXLINES][160])
{
    int n = 0;
    const char *p = text;
    while (*p && n < MAXLINES) {
        const char *nl = strchr(p, '\n');
        size_t para = nl ? (size_t)(nl - p) : strlen(p);
        size_t off = 0;
        do {
            size_t rest = para - off;
            size_t take = utf8_offset(p + off, rest, (size_t)width);
            if (take < rest) {
                size_t sp = take;
                while (sp > 0 && p[off + sp] != ' ')
                    sp--;
                if (sp > 0)
                    take = sp;
            }
            snprintf(lines[n++], 160, "%.*s", (int)take, p + off);
            off += take;
            while (off < para && p[off] == ' ')
                off++;
        } while (off < para && n < MAXLINES);
        if (!nl)
            break;
        p = nl + 1;
        if (!*p && n < MAXLINES)
            break;
    }
    return n;
}

/* Draws the box with its title and text; returns it (the answer line is
 * box.row + box.h - 2). */
static Box draw_box(bool warn, const char *title, const char *text, int extra)
{
    Box b;
    b.w = MIN(ui_cols - 4, 70);
    char lines[MAXLINES][160];
    int n = wrap(text, b.w - 4, lines);
    b.h = n + 2 + extra + 1;
    b.col = (ui_cols - b.w) / 2;
    b.row = MAX(1, (ui_rows - b.h) / 2);
    b.fg = warn ? WHITE : BLACK;
    b.bg = warn ? RED : LIGHTGRAY;
    ui_textf(b.col, b.row, b.w, warn ? YELLOW : WHITE, warn ? RED : BLUE, " %s", title);
    ui_text(b.col, b.row + 1, b.w, b.fg, b.bg, "");
    for (int i = 0; i < n; i++)
        ui_textf(b.col, b.row + 2 + i, b.w, b.fg, b.bg, "  %s", lines[i]);
    for (int i = 0; i <= extra; i++)
        ui_text(b.col, b.row + 2 + n + i, b.w, b.fg, b.bg, "");
    b.h = n + 3 + extra;
    return b;
}

void ui_message(bool warn, const char *title, const char *text)
{
    Box b = draw_box(warn, title, text, 1);
    ui_text(b.col + 2, b.row + b.h - 1, b.w - 4, b.fg, b.bg, "Press a key.");
    ui_key();
}

bool ui_yesno(bool warn, const char *title, const char *text)
{
    Box b = draw_box(warn, title, text, 1);
    ui_text(b.col + 2, b.row + b.h - 1, b.w - 4, b.fg, b.bg, "Y Yes    N No");
    for (;;) {
        PalKey k = ui_key();
        if (k.ch == 'y' || k.ch == 'Y')
            return true;
        if (k.ch == 'n' || k.ch == 'N' || ui_is_esc(&k))
            return false;
    }
}

bool ui_input(bool warn, const char *title, const char *text, char *buf, size_t n)
{
    Box b = draw_box(warn, title, text, 2);
    int frow = b.row + b.h - 2, fcol = b.col + 2, fw = b.w - 4;
    ui_text(fcol, b.row + b.h - 1, fw, b.fg, b.bg, "Enter OK    Esc Cancel");
    bool fresh = true; /* the first typed character replaces the default */
    size_t cur = strlen(buf);
    pal_con_show_cursor(true);
    for (;;) {
        size_t len = strlen(buf);
        /* the field, scrolled so that the cursor is visible */
        size_t cpos = utf8_len(buf, cur), first = cpos >= (size_t)fw ? cpos - fw + 1 : 0;
        size_t off = utf8_offset(buf, len, first);
        ui_text(fcol, frow, fw, fresh ? WHITE : BLACK, fresh ? BLUE : WHITE, buf + off);
        pal_con_set_cursor(fcol + (int)(cpos - first), frow);
        PalKey k = ui_key();
        if (ui_is_enter(&k)) {
            pal_con_show_cursor(false);
            return true;
        }
        if (ui_is_esc(&k)) {
            pal_con_show_cursor(false);
            return false;
        }
        if (k.scan == KEY_LEFT && cur > 0) {
            do
                cur--;
            while (cur > 0 && (buf[cur] & 0xC0) == 0x80);
        } else if (k.scan == KEY_RIGHT && cur < len) {
            do
                cur++;
            while (cur < len && (buf[cur] & 0xC0) == 0x80);
        } else if (k.scan == KEY_HOME)
            cur = 0;
        else if (k.scan == KEY_END)
            cur = len;
        else if ((k.ch == 8 || k.ch == 127) && cur > 0) {
            size_t s = cur;
            do
                s--;
            while (s > 0 && (buf[s] & 0xC0) == 0x80);
            memmove(buf + s, buf + cur, len - cur + 1);
            cur = s;
        } else if (k.scan == KEY_DELETE && cur < len) {
            size_t e = cur;
            do
                e++;
            while (e < len && (buf[e] & 0xC0) == 0x80);
            memmove(buf + cur, buf + e, len - e + 1);
        } else if (k.ch >= 0x20 && k.ch != 127) {
            if (fresh) {
                buf[0] = 0;
                cur = len = 0;
            }
            char u[4];
            int ul = utf8_encode(k.ch, u);
            if (len + (size_t)ul < n) {
                memmove(buf + cur + ul, buf + cur, len - cur + 1);
                memcpy(buf + cur, u, (size_t)ul);
                cur += (size_t)ul;
            }
        } else
            continue;
        fresh = false;
    }
}

int ui_menu(const char *title, const char *const *items, int n, int sel)
{
    int w = 0;
    for (int i = 0; i < n; i++)
        w = MAX(w, (int)utf8_len(items[i], strlen(items[i])));
    static const char footer[] = "  Enter Choose   Esc Cancel";
    w = MIN(MAX(MAX(w + 6, (int)utf8_len(title, strlen(title)) + 4), (int)sizeof(footer) + 1), ui_cols - 4);
    int h = MIN(n, ui_rows - 6);
    int col = (ui_cols - w) / 2, row = MAX(1, (ui_rows - h - 3) / 2), top = 0;
    if (sel < 0 || sel >= n)
        sel = 0;
    for (;;) {
        if (sel < top)
            top = sel;
        if (sel >= top + h)
            top = sel - h + 1;
        ui_textf(col, row, w, WHITE, BLUE, " %s", title);
        for (int i = 0; i < h; i++) {
            bool hl = top + i == sel;
            ui_textf(col, row + 1 + i, w, hl ? WHITE : BLACK, hl ? BLUE : LIGHTGRAY, "  %s", items[top + i]);
        }
        ui_text(col, row + 1 + h, w, BLACK, LIGHTGRAY, footer);
        PalKey k = ui_key();
        if (ui_is_enter(&k))
            return sel;
        if (ui_is_esc(&k))
            return -1;
        if (k.scan == KEY_UP && sel > 0)
            sel--;
        else if (k.scan == KEY_DOWN && sel + 1 < n)
            sel++;
        else if (k.scan == KEY_PGUP)
            sel = MAX(0, sel - h);
        else if (k.scan == KEY_PGDN)
            sel = MIN(n - 1, sel + h);
        else if (k.scan == KEY_HOME)
            sel = 0;
        else if (k.scan == KEY_END)
            sel = n - 1;
    }
}
