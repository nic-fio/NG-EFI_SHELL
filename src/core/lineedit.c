/* Line editor: cursor movement, history (persistent), Ctrl-R search, Tab completion. */
#include "shell.h"

#define MAX_HISTORY 500

static char **hist;
static int nhist;
static char *hist_file;
static bool hist_loaded;

/* ---- History ---- */

static void hist_push(const char *line)
{
    if (nhist == MAX_HISTORY) {
        free(hist[0]);
        memmove(hist, hist + 1, sizeof(char *) * (MAX_HISTORY - 1));
        nhist--;
    }
    hist = xrealloc(hist, sizeof(char *) * (nhist + 1));
    hist[nhist++] = xstrdup(line);
}

static void hist_load(void)
{
    if (hist_loaded)
        return;
    hist_loaded = true;
#ifndef NESH_HOST
    int bv = pal_boot_volume();
    if (bv < 0)
        return;
    hist_file = xasprintf("%s:\\nesh_history.txt", pal_volume(bv)->name);
#else
    const char *env = getenv("NESH_HISTORY");
    if (!env)
        return;
    hist_file = path_resolve(env);
    if (!hist_file)
        return;
#endif
    char *text;
    size_t len;
    if (file_read_text(hist_file, &text, &len) != PAL_OK)
        return;
    for (char *p = text; *p;) {
        char *e = strchr(p, '\n');
        if (e)
            *e = 0;
        if (*p)
            hist_push(p);
        if (!e)
            break;
        p = e + 1;
    }
    free(text);
}

void history_add(const char *line)
{
    hist_load();
    if (!*line || (nhist && !strcmp(hist[nhist - 1], line)))
        return;
    hist_push(line);
    if (hist_file) {
        char *l = xasprintf("%s\n", line);
        if (file_write_all(hist_file, l, strlen(l), true) != PAL_OK) {
            free(hist_file); /* read-only media: stop trying */
            hist_file = NULL;
        }
        free(l);
    }
}

int history_count(void) { hist_load(); return nhist; }
const char *history_at(int i) { return i >= 0 && i < nhist ? hist[i] : NULL; }

void history_clear(void)
{
    for (int i = 0; i < nhist; i++)
        free(hist[i]);
    free(hist);
    hist = NULL;
    nhist = 0;
    if (hist_file)
        file_write_all(hist_file, "", 0, false);
}

/* ---- Editing buffer (code points) ---- */

typedef struct {
    uint32_t *c;
    int len, cap, pos;
    const char *prompt;
    int prompt_w;
    int start_col, start_row; /* EFI: screen position of the prompt */
    int shown;                /* code points currently drawn after the prompt */
    int cols, rows;
} Ed;

static void ed_insert(Ed *e, uint32_t cp)
{
    if (e->len + 1 >= e->cap) {
        e->cap = e->cap ? e->cap * 2 : 64;
        e->c = xrealloc(e->c, sizeof(uint32_t) * e->cap);
    }
    memmove(e->c + e->pos + 1, e->c + e->pos, sizeof(uint32_t) * (e->len - e->pos));
    e->c[e->pos++] = cp;
    e->len++;
}

static void ed_delete(Ed *e, int from, int to)
{
    if (from < 0)
        from = 0;
    if (to > e->len)
        to = e->len;
    if (from >= to)
        return;
    memmove(e->c + from, e->c + to, sizeof(uint32_t) * (e->len - to));
    e->len -= to - from;
    if (e->pos > to)
        e->pos -= to - from;
    else if (e->pos > from)
        e->pos = from;
}

static void ed_set(Ed *e, const char *s)
{
    e->len = e->pos = 0;
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += utf8_decode(s + i, n - i, &cp);
        ed_insert(e, cp);
    }
}

static char *ed_text(Ed *e, int from, int to)
{
    Sbuf b;
    sb_init(&b);
    for (int i = from; i < to; i++)
        sb_put_cp(&b, e->c[i]);
    return sb_steal(&b);
}

static void write_cps(const uint32_t *c, int n)
{
    Sbuf b;
    sb_init(&b);
    for (int i = 0; i < n; i++)
        sb_put_cp(&b, c[i]);
    if (b.len)
        pal_con_write(b.s, b.len);
    sb_free(&b);
}

static void ed_begin(Ed *e)
{
    pal_con_size(&e->cols, &e->rows);
    if (e->cols < 10)
        e->cols = 80;
    pal_con_write(e->prompt, strlen(e->prompt));
    e->prompt_w = (int)utf8_len(e->prompt, strlen(e->prompt));
    e->shown = 0;
    if (!pal_con_ansi()) {
        int col, row;
        pal_con_get_cursor(&col, &row);
        e->start_row = row;
        e->start_col = col - e->prompt_w;
        while (e->start_col < 0) { /* prompt wrapped */
            e->start_col += e->cols;
            e->start_row--;
        }
    }
}

static void efi_goto(Ed *e, int offset)
{
    int abs = e->start_col + e->prompt_w + offset;
    pal_con_set_cursor(abs % e->cols, e->start_row + abs / e->cols);
}

static void ed_redraw(Ed *e)
{
    if (pal_con_ansi()) {
        Sbuf b;
        sb_init(&b);
        sb_adds(&b, "\r");
        sb_adds(&b, e->prompt);
        for (int i = 0; i < e->len; i++)
            sb_put_cp(&b, e->c[i]);
        sb_adds(&b, "\x1b[K");
        if (e->len > e->pos)
            sb_printf(&b, "\x1b[%dD", e->len - e->pos);
        pal_con_write(b.s, b.len);
        sb_free(&b);
        return;
    }
    pal_con_show_cursor(false);
    efi_goto(e, 0);
    write_cps(e->c, e->len);
    int extra = e->shown - e->len;
    for (int i = 0; i < extra; i++)
        pal_con_write(" ", 1);
    /* the screen may have scrolled: recompute the start row from the cursor */
    int col, row;
    pal_con_get_cursor(&col, &row);
    int end_off = e->start_col + e->prompt_w + e->len + (extra > 0 ? extra : 0);
    int expect_row = e->start_row + end_off / e->cols;
    if (end_off % e->cols == 0 && end_off > 0 && col != 0)
        expect_row--; /* cursor stays at the last column without wrapping */
    if (row < expect_row)
        e->start_row -= expect_row - row;
    e->shown = e->len;
    efi_goto(e, e->pos);
    pal_con_show_cursor(true);
}

static void ed_finish(Ed *e)
{
    if (!pal_con_ansi())
        efi_goto(e, e->len);
    pal_con_write("\n", 1);
}

/* ---- Completion ---- */

typedef struct {
    char **v;
    int n;
} Cands;

static void cand_add(Cands *c, const char *s)
{
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->v[i], s))
            return;
    c->v = xrealloc(c->v, sizeof(char *) * (c->n + 1));
    c->v[c->n++] = xstrdup(s);
}

static void cands_free(Cands *c)
{
    for (int i = 0; i < c->n; i++)
        free(c->v[i]);
    free(c->v);
}

static int cand_cmp(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

/* Completes file names: "dir\pre" -> entries of dir starting with pre.
 * Candidates are full replacement words; directories end with '\'. */
static void complete_files(Cands *c, const char *word, bool only_exec)
{
    const char *sep = NULL;
    for (const char *p = word; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            sep = p;
    char *dirpart = sep ? xstrndup(word, (size_t)(sep - word + 1)) : xstrdup("");
    const char *pre = sep ? sep + 1 : word;
    char *dir = path_resolve(*dirpart ? dirpart : ".");
    if (!dir) {
        free(dirpart);
        return;
    }
    PalDir *d;
    if (pal_opendir(dir, &d) == PAL_OK) {
        PalStat st;
        while (pal_readdir(d, &st) == 1) {
            if (!strncasecmp(st.name, pre, strlen(pre))) {
                size_t nl = strlen(st.name);
                bool exec = nl > 4 && (!strcasecmp(st.name + nl - 4, ".efi") || !strcasecmp(st.name + nl - 4, SCRIPT_EXT));
                if (!only_exec || st.is_dir || exec) {
                    char *w = xasprintf("%s%s%s", dirpart, st.name, st.is_dir ? "\\" : "");
                    cand_add(c, w);
                    free(w);
                }
            }
            free(st.name);
        }
        pal_closedir(d);
    }
    free(dir);
    free(dirpart);
}

static void complete(Ed *e)
{
    /* word under the cursor */
    int start = e->pos;
    bool quoted = false;
    for (int i = 0; i < e->pos; i++)
        if (e->c[i] == '"')
            quoted = !quoted;
    while (start > 0 && (quoted ? e->c[start - 1] != '"' : e->c[start - 1] != ' '))
        start--;
    bool first = true;
    for (int i = 0; i < start; i++)
        if (e->c[i] != ' ' && e->c[i] != '"')
            first = false;
    char *word = ed_text(e, start, e->pos);

    Cands c = { 0 };
    if (first && !strpbrk(word, "\\/:")) {
        for (int i = 0; i < shell_cmd_count(); i++)
            if (!strncasecmp(shell_cmd_at(i)->name, word, strlen(word)))
                cand_add(&c, shell_cmd_at(i)->name);
    }
    if (!strpbrk(word, "\\/:")) {
        for (int i = 0; i < pal_volume_count(); i++) {
            char *v = xasprintf("%s:", pal_volume(i)->name);
            if (!strncasecmp(v, word, strlen(word)) && *word)
                cand_add(&c, v);
            free(v);
        }
    }
    complete_files(&c, word, first);

    if (c.n == 1) {
        const char *w = c.v[0];
        ed_delete(e, start, e->pos);
        e->pos = start;
        size_t n = strlen(w);
        for (size_t i = 0; i < n;) {
            uint32_t cp;
            i += utf8_decode(w + i, n - i, &cp);
            ed_insert(e, cp);
        }
        char last = n ? w[n - 1] : 0;
        if (last != '\\' && last != ':')
            ed_insert(e, ' ');
    } else if (c.n > 1) {
        /* longest common prefix (case-insensitive) */
        size_t lcp = strlen(c.v[0]);
        for (int i = 1; i < c.n; i++) {
            size_t k = 0;
            while (k < lcp && c.v[i][k] && tolower((uint8_t)c.v[i][k]) == tolower((uint8_t)c.v[0][k]))
                k++;
            lcp = k;
        }
        if (lcp > strlen(word)) {
            char *pre = xstrndup(c.v[0], lcp);
            ed_delete(e, start, e->pos);
            e->pos = start;
            size_t n = strlen(pre);
            for (size_t i = 0; i < n;) {
                uint32_t cp;
                i += utf8_decode(pre + i, n - i, &cp);
                ed_insert(e, cp);
            }
            free(pre);
        } else {
            qsort(c.v, c.n, sizeof(char *), cand_cmp);
            int width = 0;
            for (int i = 0; i < c.n; i++)
                width = MAX(width, (int)strlen(path_basename(c.v[i])) + 2);
            int per_line = MAX(1, (e->cols - 1) / width);
            if (!pal_con_ansi())
                efi_goto(e, e->len);
            pal_con_write("\n", 1);
            for (int i = 0; i < c.n; i++) {
                const char *name = c.v[i];
                const char *b = path_basename(name);
                if (!*b)
                    b = name;
                char *cell = xasprintf("%-*s", width, b);
                pal_con_write(cell, strlen(cell));
                free(cell);
                if ((i + 1) % per_line == 0 || i == c.n - 1)
                    pal_con_write("\n", 1);
            }
            ed_begin(e);
        }
    }
    cands_free(&c);
    free(word);
    ed_redraw(e);
}

/* ---- Reverse search (Ctrl-R) ---- */

static void ed_change_prompt(Ed *e, const char *np)
{
    if (!pal_con_ansi()) {
        int old_total = e->prompt_w + e->shown;
        efi_goto(e, -e->prompt_w);
        e->prompt = np;
        e->prompt_w = (int)utf8_len(np, strlen(np));
        pal_con_write(np, strlen(np));
        e->shown = MAX(old_total - e->prompt_w, 0);
    } else {
        e->prompt = np;
    }
    ed_redraw(e);
}

static void reverse_search(Ed *e)
{
    Sbuf q;
    sb_init(&q);
    int idx = nhist;
    const char *orig_prompt = e->prompt;
    char *prompt = NULL;
    for (;;) {
        int found = -1;
        for (int i = idx - 1; i >= 0 && q.len; i--) {
            if (strstr(hist[i], q.s)) {
                found = i;
                break;
            }
        }
        ed_set(e, found >= 0 ? hist[found] : "");
        char *np = xasprintf("(search '%s'): ", q.s ? q.s : "");
        ed_change_prompt(e, np);
        free(prompt);
        prompt = np;
        PalKey k;
        if (!con_get_key(&k, -1))
            continue;
        if (k.ch == 18) { /* Ctrl-R again: older match */
            if (found >= 0)
                idx = found;
            continue;
        }
        if (k.ch == 8 || k.ch == 127) {
            if (q.len) {
                q.len--;
                while (q.len && ((uint8_t)q.s[q.len] & 0xC0) == 0x80)
                    q.len--;
                q.s[q.len] = 0;
            }
            idx = nhist;
            continue;
        }
        if (k.ch >= 0x20 && !k.scan) {
            sb_put_cp(&q, k.ch);
            idx = nhist;
            continue;
        }
        if (k.scan == KEY_ESC || k.ch == 27 || k.ch == 3 || k.ch == 7)
            ed_set(e, "");
        break; /* any other key accepts the found line */
    }
    ed_change_prompt(e, orig_prompt);
    free(prompt);
    sb_free(&q);
}

/* ---- Main entry ---- */

static char *read_plain_line(void)
{
    /* non-interactive input (host with redirected stdin) */
    Sbuf b;
    sb_init(&b);
    for (;;) {
        PalKey k;
        if (!pal_con_read_key(&k, -1))
            continue;
        if (k.ch == 4) { /* EOF */
            if (!b.len) {
                sb_free(&b);
                return NULL;
            }
            break;
        }
        if (k.ch == '\n' || k.ch == '\r')
            break;
        if (k.ch)
            sb_put_cp(&b, k.ch);
    }
    return sb_steal(&b);
}

static bool is_word_char(uint32_t c)
{
    return c != ' ' && c != '\\' && c != '/' && c != '"';
}

char *lineedit_read(const char *prompt, bool use_history)
{
    if (!pal_con_interactive()) {
        char *l = read_plain_line();
        if (l && use_history)
            history_add(l);
        return l;
    }
    if (use_history)
        hist_load();
    Ed e;
    memset(&e, 0, sizeof(e));
    e.prompt = prompt;
    pal_con_raw(true);
    ed_begin(&e);
    int hpos = use_history ? nhist : 0;
    char *saved = NULL; /* line being edited before browsing history */
    char *result = NULL;
    for (;;) {
        PalKey k;
        if (!con_get_key(&k, -1))
            continue;
        uint32_t ch = k.ch;
        if (k.mods & MOD_ALT) {
            if (ch == 'b' || ch == 'B')
                k.scan = KEY_LEFT, k.mods = MOD_CTRL, ch = 0;
            else if (ch == 'f' || ch == 'F')
                k.scan = KEY_RIGHT, k.mods = MOD_CTRL, ch = 0;
            else
                continue;
        }
        if (k.scan) {
            switch (k.scan) {
            case KEY_LEFT:
                if (k.mods & MOD_CTRL) {
                    while (e.pos > 0 && !is_word_char(e.c[e.pos - 1]))
                        e.pos--;
                    while (e.pos > 0 && is_word_char(e.c[e.pos - 1]))
                        e.pos--;
                } else if (e.pos > 0) {
                    e.pos--;
                }
                break;
            case KEY_RIGHT:
                if (k.mods & MOD_CTRL) {
                    while (e.pos < e.len && !is_word_char(e.c[e.pos]))
                        e.pos++;
                    while (e.pos < e.len && is_word_char(e.c[e.pos]))
                        e.pos++;
                } else if (e.pos < e.len) {
                    e.pos++;
                }
                break;
            case KEY_HOME: e.pos = 0; break;
            case KEY_END: e.pos = e.len; break;
            case KEY_DELETE: ed_delete(&e, e.pos, e.pos + 1); break;
            case KEY_UP:
            case KEY_DOWN:
                if (!use_history)
                    break;
                if (hpos == nhist) {
                    free(saved);
                    saved = ed_text(&e, 0, e.len);
                }
                if (k.scan == KEY_UP && hpos > 0)
                    hpos--;
                else if (k.scan == KEY_DOWN && hpos < nhist)
                    hpos++;
                ed_set(&e, hpos < nhist ? hist[hpos] : (saved ? saved : ""));
                break;
            case KEY_ESC:
                ed_set(&e, "");
                break;
            default:
                break;
            }
            ed_redraw(&e);
            continue;
        }
        switch (ch) {
        case '\r':
        case '\n':
            result = ed_text(&e, 0, e.len);
            ed_finish(&e);
            goto done;
        case 3: /* Ctrl-C */
            if (!pal_con_ansi())
                efi_goto(&e, e.len);
            pal_con_write("^C\n", 3);
            goto done;
        case 4: /* Ctrl-D */
            if (!e.len && pal_con_ansi()) {
                ed_finish(&e);
                goto done;
            }
            ed_delete(&e, e.pos, e.pos + 1);
            break;
        case 1: e.pos = 0; break;      /* Ctrl-A */
        case 5: e.pos = e.len; break;  /* Ctrl-E */
        case 2: if (e.pos) e.pos--; break;          /* Ctrl-B */
        case 6: if (e.pos < e.len) e.pos++; break;  /* Ctrl-F */
        case 8:
        case 127:
            ed_delete(&e, e.pos - 1, e.pos);
            break;
        case 11: ed_delete(&e, e.pos, e.len); break; /* Ctrl-K */
        case 21: ed_delete(&e, 0, e.pos); break;     /* Ctrl-U */
        case 23: {                                   /* Ctrl-W */
            int p = e.pos;
            while (p > 0 && e.c[p - 1] == ' ')
                p--;
            while (p > 0 && e.c[p - 1] != ' ')
                p--;
            ed_delete(&e, p, e.pos);
            break;
        }
        case 12: /* Ctrl-L */
            pal_con_clear();
            ed_begin(&e);
            break;
        case 9:
            complete(&e);
            continue;
        case 18: /* Ctrl-R */
            if (use_history)
                reverse_search(&e);
            continue;
        case 27:
            ed_set(&e, "");
            break;
        default:
            if (ch >= 0x20) {
                ed_insert(&e, ch);
                if (e.pos == e.len && e.shown == e.len - 1) {
                    /* typing at the end of the line: just echo the character */
                    char buf[4];
                    pal_con_write(buf, (size_t)utf8_encode(ch, buf));
                    e.shown = e.len;
                    if (!pal_con_ansi() && (e.start_col + e.prompt_w + e.len) % e.cols == 0)
                        ed_redraw(&e); /* let the redraw handle wrapping/scrolling */
                    continue;
                }
            }
            break;
        }
        ed_redraw(&e);
    }
done:
    pal_con_raw(false);
    free(saved);
    free(e.c);
    if (result && use_history) {
        const char *t = result;
        while (*t == ' ')
            t++;
        if (*t)
            history_add(result);
    }
    return result;
}
