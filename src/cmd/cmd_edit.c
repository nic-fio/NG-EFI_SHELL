/* Full-screen editors: edit (text) and hexedit (files, disk blocks, memory).
 * Keys follow the UEFI Shell editors: F1..F9 or the Ctrl equivalents. */
#include "../core/shell.h"

/* ---- Screen with per-row cache: only changed rows are redrawn ---- */

typedef struct {
    int cols, rows;
    char **cache;
    int *cattr;
} Screen;

static void scr_begin(Screen *s)
{
    pal_con_size(&s->cols, &s->rows);
    if (s->cols < 20)
        s->cols = 80;
    if (s->rows < 6)
        s->rows = 25;
    s->cache = xcalloc((size_t)s->rows, sizeof(char *));
    s->cattr = xcalloc((size_t)s->rows, sizeof(int));
    pal_con_raw(true);
    pal_con_clear();
}

static void scr_end(Screen *s)
{
    for (int i = 0; i < s->rows; i++)
        free(s->cache[i]);
    free(s->cache);
    free(s->cattr);
    pal_con_reset_color();
    pal_con_clear();
    pal_con_show_cursor(true);
    pal_con_raw(false);
}

/* Draws a row (UTF-8 text, padded/clipped to the width). */
static void scr_row(Screen *s, int row, const char *text, int fg, int bg)
{
    if (row < 0 || row >= s->rows)
        return;
    int width = row == s->rows - 1 ? s->cols - 1 : s->cols; /* never write the last cell: it scrolls */
    Sbuf b;
    sb_init(&b);
    size_t n = strlen(text);
    int w = 0;
    for (size_t i = 0; i < n && w < width;) {
        uint32_t cp;
        int k = utf8_decode(text + i, n - i, &cp);
        if (cp < 0x20)
            cp = '.';
        sb_put_cp(&b, cp);
        i += (size_t)k;
        w++;
    }
    while (w++ < width)
        sb_putc(&b, ' ');
    int attr = fg | bg << 4;
    if (s->cache[row] && s->cattr[row] == attr && !strcmp(s->cache[row], b.s)) {
        sb_free(&b);
        return;
    }
    pal_con_set_cursor(0, row);
    pal_con_set_color(fg, bg);
    pal_con_write(b.s, b.len);
    free(s->cache[row]);
    s->cache[row] = sb_steal(&b);
    s->cattr[row] = attr;
}

static void scr_invalidate(Screen *s)
{
    for (int i = 0; i < s->rows; i++) {
        free(s->cache[i]);
        s->cache[i] = NULL;
    }
    pal_con_clear();
}

/* One-line prompt in the status row; returns NULL if cancelled.
 * A default is shown as "[default]" and returned when Enter is pressed alone. */
static char *scr_prompt(Screen *s, const char *question, const char *def)
{
    Sbuf in;
    sb_init(&in);
    for (;;) {
        char *line = def && *def ? xasprintf("%s[%s] %s", question, def, in.s ? in.s : "")
                                 : xasprintf("%s%s", question, in.s ? in.s : "");
        scr_row(s, s->rows - 2, line, C_BLACK, C_LIGHTGRAY);
        int cw = (int)utf8_len(line, strlen(line));
        free(line);
        pal_con_set_cursor(MIN(cw, s->cols - 1), s->rows - 2);
        pal_con_show_cursor(true);
        PalKey k;
        if (!con_get_key(&k, -1))
            continue;
        if (k.ch == '\r' || k.ch == '\n') {
            if (!in.len && def) {
                sb_free(&in);
                return xstrdup(def);
            }
            return sb_steal(&in);
        }
        if (k.scan == KEY_ESC || k.ch == 27 || k.ch == 3 || k.ch == 17) {
            sb_free(&in);
            return NULL;
        }
        if (k.ch == 8 || k.ch == 127) {
            if (in.len) {
                in.len--;
                while (in.len && ((uint8_t)in.s[in.len] & 0xC0) == 0x80)
                    in.len--;
                in.s[in.len] = 0;
            }
        } else if (k.ch >= 0x20 && !k.scan) {
            sb_put_cp(&in, k.ch);
        }
    }
}

static int scr_ask(Screen *s, const char *question) /* 'y', 'n' or 0 (cancel) */
{
    for (;;) {
        scr_row(s, s->rows - 2, question, C_BLACK, C_LIGHTGRAY);
        PalKey k;
        if (!con_get_key(&k, -1))
            continue;
        int c = (int)tolower((int)(k.ch < 128 ? k.ch : 0));
        if (c == 'y' || c == 's')
            return 'y';
        if (c == 'n')
            return 'n';
        if (k.scan == KEY_ESC || c == 27 || c == 'c' || c == 3)
            return 0;
    }
}

/* Maps F-keys and Ctrl-keys to the editor actions. */
enum { A_NONE, A_GOTO, A_SAVE, A_EXIT, A_FIND, A_REPLACE, A_CUT, A_PASTE, A_OPEN, A_TYPE, A_HELP, A_FINDNEXT };

static int action_of(const PalKey *k)
{
    if (k->scan >= KEY_F1 && k->scan <= KEY_F1 + 9) {
        static const int f[] = { A_GOTO, A_SAVE, A_EXIT, A_FIND, A_REPLACE, A_CUT, A_PASTE, A_OPEN, A_TYPE, A_FINDNEXT };
        return f[k->scan - KEY_F1];
    }
    switch (k->ch) {
    case 7: return A_GOTO;     /* Ctrl-G */
    case 19: return A_SAVE;    /* Ctrl-S */
    case 17: return A_EXIT;    /* Ctrl-Q */
    case 6: return A_FIND;     /* Ctrl-F */
    case 18: return A_REPLACE; /* Ctrl-R */
    case 11: return A_CUT;     /* Ctrl-K */
    case 21: return A_PASTE;   /* Ctrl-U */
    case 15: return A_OPEN;    /* Ctrl-O */
    case 20: return A_TYPE;    /* Ctrl-T */
    case 5: return A_HELP;     /* Ctrl-E */
    case 14: return A_FINDNEXT; /* Ctrl-N */
    }
    return A_NONE;
}

/* ---- Text editor ---- */

typedef struct {
    uint32_t *c;
    int len, cap;
} Line;

typedef struct {
    Line *l;
    int n, cap;
    int cx, cy, top, left;
    char *path;
    bool modified, ucs2, crlf, bom;
    Line *clip; /* cut lines: consecutive cuts add to it, any other key starts over */
    int nclip, capclip;
    bool last_cut;
    char *last_find;
    char msg[160];
    Screen s;
} Ed;

static void line_ins(Line *l, int at, uint32_t c)
{
    if (l->len + 1 > l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->c = xrealloc(l->c, sizeof(uint32_t) * (size_t)l->cap);
    }
    memmove(l->c + at + 1, l->c + at, sizeof(uint32_t) * (size_t)(l->len - at));
    l->c[at] = c;
    l->len++;
}

static void line_del(Line *l, int at, int count)
{
    if (at >= l->len)
        return;
    if (at + count > l->len)
        count = l->len - at;
    memmove(l->c + at, l->c + at + count, sizeof(uint32_t) * (size_t)(l->len - at - count));
    l->len -= count;
}

static void line_append(Line *l, const uint32_t *c, int n)
{
    for (int i = 0; i < n; i++)
        line_ins(l, l->len, c[i]);
}

static void ed_insert_line(Ed *e, int at)
{
    if (e->n + 1 > e->cap) {
        e->cap = e->cap ? e->cap * 2 : 64;
        e->l = xrealloc(e->l, sizeof(Line) * (size_t)e->cap);
    }
    memmove(e->l + at + 1, e->l + at, sizeof(Line) * (size_t)(e->n - at));
    memset(&e->l[at], 0, sizeof(Line));
    e->n++;
}

static void ed_delete_line(Ed *e, int at)
{
    free(e->l[at].c);
    memmove(e->l + at, e->l + at + 1, sizeof(Line) * (size_t)(e->n - at - 1));
    e->n--;
    if (!e->n)
        ed_insert_line(e, 0);
}

static void ed_load_text(Ed *e, const char *text, size_t len)
{
    ed_insert_line(e, 0);
    Line *cur = &e->l[0];
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        i += (size_t)utf8_decode(text + i, len - i, &cp);
        if (cp == '\n') {
            ed_insert_line(e, e->n);
            cur = &e->l[e->n - 1];
        } else {
            line_ins(cur, cur->len, cp);
        }
    }
    if (e->n > 1 && !e->l[e->n - 1].len) /* final newline */
        ed_delete_line(e, e->n - 1);
}

static bool has_crlf(const char *d, size_t n)
{
    for (size_t i = 0; i + 1 < n; i++)
        if (d[i] == '\r' && (d[i + 1] == '\n' || (i + 2 < n && d[i + 1] == 0 && d[i + 2] == '\n')))
            return true;
    return false;
}

static bool ed_load(Ed *e, const char *path)
{
    char *raw;
    size_t rlen;
    int err = file_read_all(path, &raw, &rlen);
    if (err == PAL_ENOENT) {
        ed_insert_line(e, 0);
        snprintf(e->msg, sizeof(e->msg), "New file");
        return true;
    }
    if (err) {
        err_printf("edit: %s: %s\n", path, pal_strerror(err));
        return false;
    }
    e->ucs2 = rlen >= 2 && (uint8_t)raw[0] == 0xFF && (uint8_t)raw[1] == 0xFE;
    e->bom = !e->ucs2 && rlen >= 3 && !memcmp(raw, "\xEF\xBB\xBF", 3);
    e->crlf = has_crlf(raw, rlen);
    free(raw);
    char *text;
    size_t len;
    file_read_text(path, &text, &len);
    ed_load_text(e, text, len);
    free(text);
    snprintf(e->msg, sizeof(e->msg), "%d lines read", e->n);
    return true;
}

static bool ed_save(Ed *e)
{
    Sbuf b;
    sb_init(&b);
    const char *nl = e->crlf ? "\r\n" : "\n";
    if (e->bom && !e->ucs2)
        sb_adds(&b, "\xEF\xBB\xBF");
    for (int i = 0; i < e->n; i++) {
        for (int k = 0; k < e->l[i].len; k++)
            sb_put_cp(&b, e->l[i].c[k]);
        if (i + 1 < e->n || e->l[i].len)
            sb_adds(&b, nl);
    }
    int err;
    if (e->ucs2) {
        size_t units;
        uint16_t *u = utf8_to_ucs2(b.s ? b.s : "", &units);
        char *o = xmalloc(units * 2 + 2);
        o[0] = (char)0xFF;
        o[1] = (char)0xFE;
        memcpy(o + 2, u, units * 2);
        err = file_write_all(e->path, o, units * 2 + 2, false);
        free(o);
        free(u);
    } else {
        err = file_write_all(e->path, b.s ? b.s : "", b.len, false);
    }
    sb_free(&b);
    if (err) {
        snprintf(e->msg, sizeof(e->msg), "Save failed: %s", pal_strerror(err));
        return false;
    }
    e->modified = false;
    snprintf(e->msg, sizeof(e->msg), "%d lines written", e->n);
    return true;
}

/* Tabs are kept in the text and shown up to the next multiple of TAB_WIDTH. */
#define TAB_WIDTH 4

/* Screen column of character index cx (e->left is in screen columns too). */
static int ed_vcol(const Line *l, int cx)
{
    int col = 0;
    for (int k = 0; k < cx && k < l->len; k++)
        col += l->c[k] == '\t' ? TAB_WIDTH - col % TAB_WIDTH : 1;
    return col;
}

static void ed_draw(Ed *e)
{
    Screen *s = &e->s;
    int text_rows = s->rows - 3;
    if (e->cy < e->top)
        e->top = e->cy;
    if (e->cy >= e->top + text_rows)
        e->top = e->cy - text_rows + 1;
    int vc = ed_vcol(&e->l[e->cy], e->cx);
    if (vc < e->left)
        e->left = vc;
    if (vc >= e->left + s->cols - 1)
        e->left = vc - s->cols + 2;
    pal_con_show_cursor(false);
    char *title = xasprintf("  NESH edit   %s%s   %s%s", e->path, e->modified ? "  [modified]" : "",
                            e->ucs2 ? "UCS-2" : "UTF-8", e->crlf ? " CRLF" : "");
    scr_row(s, 0, title, C_WHITE, C_BLUE);
    free(title);
    for (int r = 0; r < text_rows; r++) {
        int li = e->top + r;
        Sbuf b;
        sb_init(&b);
        if (li < e->n) {
            Line *l = &e->l[li];
            int col = 0;
            for (int k = 0; k < l->len && col < e->left + s->cols; k++) {
                int w = l->c[k] == '\t' ? TAB_WIDTH - col % TAB_WIDTH : 1;
                for (int j = 0; j < w; j++, col++)
                    if (col >= e->left && col < e->left + s->cols)
                        sb_put_cp(&b, l->c[k] == '\t' ? ' ' : l->c[k]);
            }
        }
        scr_row(s, r + 1, b.s ? b.s : "", C_LIGHTGRAY, C_BLACK);
        sb_free(&b);
    }
    char *st = xasprintf("  Line %d/%d  Col %d   %s", e->cy + 1, e->n, e->cx + 1, e->msg);
    scr_row(s, s->rows - 2, st, C_BLACK, C_LIGHTGRAY);
    free(st);
    scr_row(s, s->rows - 1, "F1 GoTo F2 Save F3 Exit F4 Find F5 Replace F6 Cut F7 Paste F9 Type  (Ctrl-E help)",
            C_WHITE, C_BLUE);
    pal_con_set_cursor(vc - e->left, e->cy - e->top + 1);
    pal_con_show_cursor(true);
}

static bool match_at(const Line *l, int at, const uint32_t *f, int fn)
{
    if (at + fn > l->len)
        return false;
    for (int i = 0; i < fn; i++)
        if (l->c[at + i] != f[i])
            return false;
    return true;
}

static uint32_t *to_cps(const char *s, int *n)
{
    size_t l = strlen(s);
    uint32_t *r = xmalloc(sizeof(uint32_t) * (l + 1));
    *n = 0;
    for (size_t i = 0; i < l;) {
        uint32_t cp;
        i += (size_t)utf8_decode(s + i, l - i, &cp);
        r[(*n)++] = cp;
    }
    return r;
}

/* Finds the text after the cursor (wrapping around). */
static bool ed_find(Ed *e, const char *text, bool from_next)
{
    int fn;
    uint32_t *f = to_cps(text, &fn);
    if (!fn) {
        free(f);
        return false;
    }
    for (int pass = 0; pass <= e->n; pass++) {
        int li = (e->cy + pass) % e->n;
        int start = pass == 0 ? e->cx + (from_next ? 1 : 0) : 0;
        for (int k = start; k + fn <= e->l[li].len; k++) {
            if (match_at(&e->l[li], k, f, fn)) {
                e->cy = li;
                e->cx = k;
                free(f);
                return true;
            }
        }
    }
    free(f);
    return false;
}

static void ed_help(Ed *e)
{
    static const char *lines[] = {
        "NESH edit - keys",
        "",
        "  F1  Ctrl-G   go to line             F6  Ctrl-K   cut the current line",
        "  F2  Ctrl-S   save                   F7  Ctrl-U   paste the cut lines",
        "  F3  Ctrl-Q   exit                   F8  Ctrl-O   open another file",
        "  F4  Ctrl-F   find                   F9  Ctrl-T   switch UTF-8 / UCS-2",
        "  F5  Ctrl-R   find and replace       F10 Ctrl-N   find next",
        "",
        "  Arrows, Home, End, PgUp, PgDn move; Tab inserts 4 spaces.",
        "  Tab characters already in the file are kept.",
        "",
        "  Press any key to go back to the text.",
    };
    for (int r = 1; r < e->s.rows - 2; r++)
        scr_row(&e->s, r, (size_t)(r - 1) < ARRAY_SIZE(lines) ? lines[r - 1] : "", C_WHITE, C_BLACK);
    PalKey k;
    while (!con_get_key(&k, -1))
        ;
}

static bool ed_confirm_discard(Ed *e)
{
    if (!e->modified)
        return true;
    int a = scr_ask(&e->s, "  File modified. Save it? (y = yes, n = no, ESC = cancel)");
    if (a == 'y')
        return ed_save(e);
    return a == 'n';
}

static void ed_free_lines(Ed *e)
{
    for (int i = 0; i < e->n; i++)
        free(e->l[i].c);
    free(e->l);
    e->l = NULL;
    e->n = e->cap = 0;
}

static int cmd_edit(int argc, char **argv)
{
    if (argc != 2)
        return cmd_usage("edit");
    if (!pal_con_interactive())
        return cmd_err("edit", "needs an interactive console");
    Ed *e = xcalloc(1, sizeof(Ed));
    e->path = path_resolve(argv[1]);
    if (!e->path) {
        free(e);
        return cmd_err("edit", "%s: invalid path", argv[1]);
    }
    if (!ed_load(e, e->path)) {
        free(e->path);
        free(e);
        return RC_FAIL;
    }
    scr_begin(&e->s);
    for (;;) {
        if (e->cy >= e->n)
            e->cy = e->n - 1;
        if (e->cx > e->l[e->cy].len)
            e->cx = e->l[e->cy].len;
        ed_draw(e);
        PalKey k;
        if (!con_get_key(&k, -1))
            continue;
        e->msg[0] = 0;
        bool prev_cut = e->last_cut;
        e->last_cut = false;
        Line *l = &e->l[e->cy];
        int text_rows = e->s.rows - 3;
        int act = action_of(&k);
        if (act == A_EXIT || (k.scan == KEY_ESC && !act)) {
            if (ed_confirm_discard(e))
                break;
            continue;
        }
        switch (act) {
        case A_SAVE:
            ed_save(e);
            continue;
        case A_GOTO: {
            char *a = scr_prompt(&e->s, "  Go to line: ", NULL);
            int64_t n;
            if (a && parse_int(a, &n) && n >= 1)
                e->cy = (int)MIN(n, (int64_t)e->n) - 1, e->cx = 0;
            free(a);
            continue;
        }
        case A_FIND:
        case A_FINDNEXT: {
            char *a = act == A_FINDNEXT && e->last_find ? xstrdup(e->last_find)
                                                        : scr_prompt(&e->s, "  Find: ", e->last_find);
            if (a && *a) {
                free(e->last_find);
                e->last_find = xstrdup(a);
                if (!ed_find(e, a, true))
                    snprintf(e->msg, sizeof(e->msg), "Not found: %s", a);
            }
            free(a);
            continue;
        }
        case A_REPLACE: {
            /* one pass from the cursor to the end of the file */
            char *f = scr_prompt(&e->s, "  Replace: ", e->last_find);
            char *r = f && *f ? scr_prompt(&e->s, "  With: ", NULL) : NULL;
            if (r) {
                int fn, rn, count = 0;
                uint32_t *fc = to_cps(f, &fn), *rc = to_cps(r, &rn);
                bool all = false, stop = false;
                /* one full turn of the file, starting at the cursor */
                int start_y = e->cy, start_x = e->cx;
                for (int step = 0; step <= e->n && !stop; step++) {
                    int li = (start_y + step) % e->n;
                    int from = step == 0 ? start_x : 0;
                    int limit = step == e->n ? start_x : e->l[li].len; /* back on the first line */
                    for (int k = from; k + fn <= e->l[li].len && k < limit + (step == e->n ? 0 : 1) && !stop;) {
                        if (!match_at(&e->l[li], k, fc, fn)) {
                            k++;
                            continue;
                        }
                        e->cy = li;
                        e->cx = k;
                        int ans = 'y';
                        if (!all) {
                            ed_draw(e);
                            scr_row(&e->s, e->s.rows - 2, "  Replace? y = yes, n = no, a = all, ESC = stop",
                                    C_BLACK, C_LIGHTGRAY);
                            PalKey q;
                            while (!con_get_key(&q, -1))
                                ;
                            ans = q.ch < 128 ? tolower((int)q.ch) : 0;
                            if (ans == 'a')
                                all = true, ans = 'y';
                            if (ans != 'y' && ans != 'n')
                                stop = true;
                        }
                        if (ans == 'y') {
                            line_del(&e->l[li], k, fn);
                            for (int i = 0; i < rn; i++)
                                line_ins(&e->l[li], k + i, rc[i]);
                            k += rn;
                            count++;
                            e->modified = true;
                        } else {
                            k++;
                        }
                    }
                }
                free(fc);
                free(rc);
                snprintf(e->msg, sizeof(e->msg), "%d replaced", count);
                free(e->last_find);
                e->last_find = xstrdup(f);
            }
            free(f);
            free(r);
            continue;
        }
        case A_CUT:
            if (!prev_cut) { /* a new block of cut lines */
                for (int i = 0; i < e->nclip; i++)
                    free(e->clip[i].c);
                e->nclip = 0;
            }
            if (e->nclip + 1 > e->capclip) {
                e->capclip = e->capclip ? e->capclip * 2 : 16;
                e->clip = xrealloc(e->clip, sizeof(Line) * (size_t)e->capclip);
            }
            e->clip[e->nclip] = *l;
            e->clip[e->nclip].c = xmalloc(sizeof(uint32_t) * (size_t)(l->len + 1));
            memcpy(e->clip[e->nclip].c, l->c, sizeof(uint32_t) * (size_t)l->len);
            e->clip[e->nclip].cap = l->len + 1;
            e->nclip++;
            e->last_cut = true;
            ed_delete_line(e, e->cy);
            e->modified = true;
            continue;
        case A_PASTE:
            for (int i = 0; i < e->nclip; i++) {
                ed_insert_line(e, e->cy + i);
                line_append(&e->l[e->cy + i], e->clip[i].c, e->clip[i].len);
            }
            if (e->nclip)
                e->modified = true;
            continue;
        case A_TYPE:
            e->ucs2 = !e->ucs2;
            e->modified = true;
            continue;
        case A_HELP:
            ed_help(e);
            continue;
        case A_OPEN: {
            if (!ed_confirm_discard(e))
                continue;
            char *a = scr_prompt(&e->s, "  Open file: ", NULL);
            char *p = a && *a ? path_resolve(a) : NULL;
            if (p) {
                ed_free_lines(e);
                free(e->path);
                e->path = p;
                e->cx = e->cy = e->top = e->left = 0;
                e->modified = e->ucs2 = e->crlf = e->bom = false;
                if (!ed_load(e, p))
                    ed_insert_line(e, 0);
                scr_invalidate(&e->s);
            }
            free(a);
            continue;
        }
        default:
            break;
        }
        switch (k.scan) {
        case KEY_UP: if (e->cy) e->cy--; continue;
        case KEY_DOWN: if (e->cy + 1 < e->n) e->cy++; continue;
        case KEY_LEFT:
            if (e->cx) e->cx--;
            else if (e->cy) e->cy--, e->cx = e->l[e->cy].len;
            continue;
        case KEY_RIGHT:
            if (e->cx < l->len) e->cx++;
            else if (e->cy + 1 < e->n) e->cy++, e->cx = 0;
            continue;
        case KEY_HOME: e->cx = 0; continue;
        case KEY_END: e->cx = l->len; continue;
        case KEY_PGUP: e->cy = MAX(0, e->cy - text_rows); continue;
        case KEY_PGDN: e->cy = MIN(e->n - 1, e->cy + text_rows); continue;
        case KEY_DELETE:
            if (e->cx < l->len) {
                line_del(l, e->cx, 1);
            } else if (e->cy + 1 < e->n) {
                line_append(l, e->l[e->cy + 1].c, e->l[e->cy + 1].len);
                ed_delete_line(e, e->cy + 1);
            }
            e->modified = true;
            continue;
        default:
            break;
        }
        if (k.ch == '\r' || k.ch == '\n') {
            ed_insert_line(e, e->cy + 1);
            l = &e->l[e->cy];
            line_append(&e->l[e->cy + 1], l->c + e->cx, l->len - e->cx);
            l->len = e->cx;
            e->cy++;
            e->cx = 0;
            e->modified = true;
        } else if (k.ch == 8 || k.ch == 127) {
            if (e->cx) {
                line_del(l, --e->cx, 1);
            } else if (e->cy) {
                Line *p = &e->l[e->cy - 1];
                e->cx = p->len;
                line_append(p, l->c, l->len);
                ed_delete_line(e, e->cy);
                e->cy--;
            }
            e->modified = true;
        } else if (k.ch == '\t') {
            for (int i = 0; i < 4; i++)
                line_ins(l, e->cx++, ' ');
            e->modified = true;
        } else if (k.ch >= 0x20 && !(k.mods & MOD_ALT)) {
            line_ins(l, e->cx++, k.ch);
            e->modified = true;
        }
    }
    scr_end(&e->s);
    ed_free_lines(e);
    for (int i = 0; i < e->nclip; i++)
        free(e->clip[i].c);
    free(e->clip);
    free(e->last_find);
    free(e->path);
    free(e);
    return RC_OK;
}

/* ---- Hex editor ---- */

/* Disk blocks and memory come from the platform (unsupported on the host). */
int platform_blk_read(const char *dev, uint64_t lba, uint64_t count, uint8_t **buf, size_t *len, uint32_t *bsize);
int platform_blk_write(const char *dev, uint64_t lba, const uint8_t *buf, size_t len);
int platform_mem_read(uint64_t addr, size_t len, uint8_t **buf);
int platform_mem_write(uint64_t addr, const uint8_t *buf, size_t len);

enum { HX_FILE, HX_DISK, HX_MEM };

typedef struct {
    uint8_t *d;
    size_t n, cap;
    size_t cur;
    int nibble;  /* 0 high, 1 low */
    bool ascii;  /* editing the ASCII column */
    size_t top;  /* first shown row (in lines of 16) */
    bool modified;
    int kind;
    char *name;
    uint64_t base; /* address / byte offset of d[0] */
    char dev[32];
    uint64_t lba;
    char msg[160];
    Screen s;
} Hex;

static void hx_draw(Hex *h)
{
    Screen *s = &h->s;
    int rows = s->rows - 3;
    size_t row = h->cur / 16;
    if (row < h->top)
        h->top = row;
    if (row >= h->top + (size_t)rows)
        h->top = row - (size_t)rows + 1;
    pal_con_show_cursor(false);
    static const char *kinds[] = { "file", "disk", "memory" };
    char *title = xasprintf("  NESH hexedit   %s %s%s   %zu bytes", kinds[h->kind], h->name,
                            h->modified ? "  [modified]" : "", h->n);
    scr_row(s, 0, title, C_WHITE, C_BLUE);
    free(title);
    for (int r = 0; r < rows; r++) {
        size_t off = (h->top + (size_t)r) * 16;
        Sbuf b;
        sb_init(&b);
        if (off < h->n || (off == h->n && off == 0)) {
            sb_printf(&b, "%010llX  ", (unsigned long long)(h->base + off));
            for (int k = 0; k < 16; k++) {
                if (off + (size_t)k < h->n)
                    sb_printf(&b, "%02X ", h->d[off + (size_t)k]);
                else
                    sb_adds(&b, "   ");
                if (k == 7)
                    sb_putc(&b, ' ');
            }
            sb_adds(&b, " ");
            for (int k = 0; k < 16 && off + (size_t)k < h->n; k++) {
                uint8_t c = h->d[off + (size_t)k];
                sb_putc(&b, c >= 0x20 && c < 0x7f ? (char)c : '.');
            }
        }
        scr_row(s, r + 1, b.s ? b.s : "", C_LIGHTGRAY, C_BLACK);
        sb_free(&b);
    }
    char *st = xasprintf("  Offset %llX  %s   %s", (unsigned long long)(h->base + h->cur), h->ascii ? "ASCII" : "HEX",
                         h->msg);
    scr_row(s, s->rows - 2, st, C_BLACK, C_LIGHTGRAY);
    free(st);
    scr_row(s, s->rows - 1,
            h->kind == HX_FILE ? "F1 GoTo F2 Save F3 Exit  Tab hex/ASCII  Ins insert  Del delete"
                               : "F1 GoTo F2 Save F3 Exit  Tab hex/ASCII",
            C_WHITE, C_BLUE);
    int col = h->ascii ? 12 + 16 * 3 + 2 + (int)(h->cur % 16) : 12 + (int)(h->cur % 16) * 3 + (h->cur % 16 >= 8) + h->nibble;
    pal_con_set_cursor(col, (int)(row - h->top) + 1);
    pal_con_show_cursor(true);
}

static bool hx_save(Hex *h)
{
    int err;
    if (h->kind == HX_FILE) {
        err = file_write_all(h->name, (const char *)h->d, h->n, false);
    } else if (h->kind == HX_DISK) {
        if (scr_ask(&h->s, "  Write the blocks back to the disk? (y/n)") != 'y')
            return false;
        err = platform_blk_write(h->dev, h->lba, h->d, h->n);
    } else {
        if (!hw_write_allowed("hexedit")) {
            snprintf(h->msg, sizeof(h->msg), "Memory writes are disabled (Secure Boot)");
            return false;
        }
        err = platform_mem_write(h->base, h->d, h->n);
    }
    if (err) {
        snprintf(h->msg, sizeof(h->msg), "Save failed: %s", pal_strerror(err));
        return false;
    }
    h->modified = false;
    snprintf(h->msg, sizeof(h->msg), "Saved");
    return true;
}

static int hexval(uint32_t c)
{
    if (c >= '0' && c <= '9')
        return (int)(c - '0');
    c |= 32;
    return c >= 'a' && c <= 'f' ? (int)(c - 'a' + 10) : -1;
}

static int cmd_hexedit(int argc, char **argv)
{
    if (!pal_con_interactive())
        return cmd_err("hexedit", "needs an interactive console");
    Hex *h = xcalloc(1, sizeof(Hex));
    int rc = RC_OK;
    uint64_t a, b;
    char *end;
    if (argc == 5 && !strcasecmp(argv[1], "-d")) {
        a = strtoull(argv[3], &end, 16);
        b = *end ? 0 : strtoull(argv[4], &end, 16);
        uint32_t bs = 0;
        if (*end || !b || b > 0x100)
            rc = cmd_err("hexedit", "-d DEVICE LBA COUNT (hexadecimal, COUNT up to 100)");
        else if (platform_blk_read(argv[2], a, b, &h->d, &h->n, &bs))
            rc = cmd_err("hexedit", "cannot read %s", argv[2]);
        h->kind = HX_DISK;
        snprintf(h->dev, sizeof(h->dev), "%s", argv[2]);
        h->lba = a;
        h->base = a * bs;
        h->name = xasprintf("%s LBA %llX", argv[2], (unsigned long long)a);
    } else if (argc == 4 && !strcasecmp(argv[1], "-m")) {
        a = strtoull(argv[2], &end, 16);
        b = *end ? 0 : strtoull(argv[3], &end, 16);
        if (*end || !b || b > 0x100000)
            rc = cmd_err("hexedit", "-m ADDRESS SIZE (hexadecimal, SIZE up to 100000)");
        else if (platform_mem_read(a, (size_t)b, &h->d))
            rc = cmd_err("hexedit", "memory access is not available");
        h->n = (size_t)b;
        h->kind = HX_MEM;
        h->base = a;
        h->name = xasprintf("%llX", (unsigned long long)a);
    } else if (argc == 2 || (argc == 3 && !strcasecmp(argv[1], "-f"))) {
        h->kind = HX_FILE;
        h->name = path_resolve(argv[argc - 1]);
        char *data = NULL;
        int e = h->name ? file_read_all(h->name, &data, &h->n) : PAL_ENOENT;
        if (e == PAL_ENOENT && h->name) {
            h->d = xmalloc(1);
            h->n = 0;
            snprintf(h->msg, sizeof(h->msg), "New file");
        } else if (e) {
            rc = cmd_perr("hexedit", argv[argc - 1], e);
        } else {
            h->d = (uint8_t *)data;
        }
    } else {
        rc = cmd_usage("hexedit");
    }
    if (rc) {
        free(h->d);
        free(h->name);
        free(h);
        return rc;
    }
    h->cap = h->n;
    scr_begin(&h->s);
    for (;;) {
        if (h->n && h->cur >= h->n)
            h->cur = h->n - 1;
        hx_draw(h);
        PalKey k;
        if (!con_get_key(&k, -1))
            continue;
        h->msg[0] = 0;
        int act = action_of(&k);
        int rows = h->s.rows - 3;
        if (act == A_EXIT || (k.scan == KEY_ESC && !act)) {
            if (!h->modified)
                break;
            int ans = scr_ask(&h->s, "  Data modified. Save it? (y = yes, n = no, ESC = cancel)");
            if (ans == 'n' || (ans == 'y' && hx_save(h)))
                break;
            continue;
        }
        if (act == A_SAVE) {
            hx_save(h);
            continue;
        }
        if (act == A_GOTO) {
            char *s = scr_prompt(&h->s, "  Go to offset (hex): ", NULL);
            uint64_t o;
            if (s && *s) {
                o = strtoull(s, &end, 16);
                if (!*end && o >= h->base && o - h->base < h->n)
                    h->cur = (size_t)(o - h->base), h->nibble = 0;
            }
            free(s);
            continue;
        }
        switch (k.scan) {
        case KEY_UP: if (h->cur >= 16) h->cur -= 16; h->nibble = 0; continue;
        case KEY_DOWN: if (h->cur + 16 < h->n) h->cur += 16; h->nibble = 0; continue;
        case KEY_LEFT:
            if (!h->ascii && h->nibble) h->nibble = 0;
            else if (h->cur) h->cur--, h->nibble = h->ascii ? 0 : 1;
            continue;
        case KEY_RIGHT:
            if (!h->ascii && !h->nibble) h->nibble = 1;
            else if (h->cur + 1 < h->n) h->cur++, h->nibble = 0;
            continue;
        case KEY_HOME: h->cur -= h->cur % 16; h->nibble = 0; continue;
        case KEY_END: h->cur = MIN(h->n ? h->n - 1 : 0, h->cur - h->cur % 16 + 15); continue;
        case KEY_PGUP: h->cur = h->cur >= (size_t)rows * 16 ? h->cur - (size_t)rows * 16 : h->cur % 16; continue;
        case KEY_PGDN: if (h->cur + (size_t)rows * 16 < h->n) h->cur += (size_t)rows * 16; continue;
        case KEY_INSERT:
        case KEY_DELETE:
            if (h->kind != HX_FILE) {
                snprintf(h->msg, sizeof(h->msg), "The size of disk blocks and memory cannot change");
                continue;
            }
            if (k.scan == KEY_INSERT) {
                h->d = xrealloc(h->d, h->n + 1);
                memmove(h->d + h->cur + 1, h->d + h->cur, h->n - h->cur);
                h->d[h->cur] = 0;
                h->n++;
            } else if (h->n) {
                memmove(h->d + h->cur, h->d + h->cur + 1, h->n - h->cur - 1);
                h->n--;
            }
            h->modified = true;
            continue;
        default:
            break;
        }
        if (k.ch == '\t') {
            h->ascii = !h->ascii;
            h->nibble = 0;
            continue;
        }
        if (!h->n && k.ch >= 0x20) { /* empty file: typing appends */
            h->d = xrealloc(h->d, 1);
            h->d[0] = 0;
            h->n = 1;
        }
        if (h->ascii && k.ch >= 0x20 && k.ch < 0x7f) {
            h->d[h->cur] = (uint8_t)k.ch;
            h->modified = true;
            if (h->cur + 1 < h->n)
                h->cur++;
            else if (h->kind == HX_FILE) { /* typing at the end extends a file */
                h->d = xrealloc(h->d, h->n + 1);
                h->d[h->n++] = 0;
                h->cur++;
            }
        } else if (!h->ascii && hexval(k.ch) >= 0) {
            int v = hexval(k.ch);
            uint8_t *p = &h->d[h->cur];
            *p = h->nibble ? (uint8_t)((*p & 0xF0) | v) : (uint8_t)((*p & 0x0F) | v << 4);
            h->modified = true;
            if (!h->nibble)
                h->nibble = 1;
            else if (h->cur + 1 < h->n)
                h->cur++, h->nibble = 0;
        }
    }
    scr_end(&h->s);
    free(h->d);
    free(h->name);
    free(h);
    return RC_OK;
}

static const Cmd edit_cmds[] = {
    { "edit", cmd_edit, "edit FILE", "Full-screen text editor (UTF-8 or UCS-2 files; Ctrl-E for help)",
      "  edit FILE           open FILE; a new FILE is created when you save\n"
      "Keys (Ctrl-E shows them inside the editor):\n"
      "  F1  Ctrl-G  go to line          F6  Ctrl-K  cut the current line\n"
      "  F2  Ctrl-S  save                F7  Ctrl-U  paste the cut lines\n"
      "  F3  Ctrl-Q  exit (also Esc)     F8  Ctrl-O  open another file\n"
      "  F4  Ctrl-F  find                F9  Ctrl-T  switch UTF-8 / UCS-2\n"
      "  F5  Ctrl-R  find and replace    F10 Ctrl-N  find next\n"
      "Find is case-sensitive and wraps around; replace asks y/n/a(ll) per match.\n"
      "Cut lines in a row form one block; paste inserts the last block cut.\n"
      "The file keeps its encoding (UTF-8 with or without BOM, or UCS-2) and line\n"
      "ends (LF or CRLF). Tab characters are kept and shown up to the next multiple\n"
      "of 4 columns; the Tab key inserts 4 spaces. Needs an interactive console.\n" },
    { "hexedit", cmd_hexedit, "hexedit [-f] FILE | hexedit -d DEVICE LBA COUNT | hexedit -m ADDRESS SIZE",
      "Full-screen hex editor for files, disk blocks or memory",
      "  -d DEVICE LBA COUNT   disk blocks (DEVICE: blkN, fsN: or a handle;\n"
      "                        numbers in hex)\n"
      "  -m ADDRESS SIZE       memory (writing is disabled while Secure Boot\n"
      "                        is active)\n"
      "  Keys: arrows, PgUp/PgDn, Tab switches hex/ASCII, F1 go to, F2 save, F3 exit.\n" },
};

void cmds_edit_init(void)
{
    shell_register(edit_cmds, ARRAY_SIZE(edit_cmds));
}
