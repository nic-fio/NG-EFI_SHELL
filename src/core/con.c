/* Console services: stack of output sinks (console, capture, file), -data
 * record helpers, colors, keyboard with type-ahead buffer and Ctrl-C detection. */
#include "con.h"

typedef struct {
    int kind; /* 0 console, 1 capture, 2 file */
    Sbuf *buf;
    PalFile *file;
    int err;
} Sink;

#define MAX_SINKS 16
static Sink sinks[MAX_SINKS];
static int nsinks;

static Sink *top(void)
{
    return nsinks ? &sinks[nsinks - 1] : NULL;
}

/* ---- Paging ----
 * Long output would scroll away on a console that cannot be scrolled back,
 * so the shell stops at every screenful and waits for a key. Paging is only
 * used for output that goes to the console (see out_paging). */

static void set_break(void); /* stop the running command, as Ctrl-C does */

static bool paging;      /* paging wanted for the current command */
static bool page_quit;   /* the user pressed q: drop the rest of the output */
static int page_left;    /* lines before the next pause */
static int page_col;     /* current column, to count wrapped lines */
static int page_rows, page_cols;

void out_paging(bool on)
{
    paging = on;
    page_quit = false;
    page_col = 0;
    pal_con_size(&page_cols, &page_rows);
    if (page_cols < 8)
        page_cols = 80;
    if (page_rows < 4)
        page_rows = 25;
    page_left = page_rows - 1;
}

bool out_paging_quit(void)
{
    return page_quit;
}

/* Waits after a full screen. Returns false if the user wants to stop. */
static bool page_pause(void)
{
    static const char *msg = "-- More -- (Enter: one line, Space: one page, q: stop) ";
    pal_con_write(msg, strlen(msg));
    pal_con_raw(true);
    PalKey k;
    con_get_key(&k, -1);
    pal_con_raw(false);
    /* erase the message */
    pal_con_write("\r", 1);
    for (size_t i = 0; i < strlen(msg); i++)
        pal_con_write(" ", 1);
    pal_con_write("\r", 1);
    if (k.ch == 'q' || k.ch == 'Q' || k.ch == 27 || k.ch == 3 || k.scan == KEY_ESC) {
        page_quit = true;
        set_break(); /* also stop the command: no point in producing more */
        return false;
    }
    page_left = (k.ch == '\r' || k.ch == '\n') ? 1 : page_rows - 1;
    return true;
}

/* Writes to the console, stopping at every screenful. */
static void write_paged(const char *s, size_t n)
{
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        bool eol = s[i] == '\n';
        if (!eol) {
            if (((uint8_t)s[i] & 0xC0) != 0x80)
                page_col++;
            if (page_col < page_cols)
                continue;
            eol = true; /* the line wraps */
        }
        pal_con_write(s + start, i + 1 - start);
        start = i + 1;
        page_col = 0;
        if (--page_left > 0)
            continue;
        if (!page_pause())
            return; /* q: the rest of this write is dropped */
    }
    if (start < n)
        pal_con_write(s + start, n - start);
}

void out_write(const char *s, size_t n)
{
    Sink *t = top();
    if (!t) {
        if (page_quit)
            return;
        if (paging && pal_con_interactive())
            write_paged(s, n);
        else
            pal_con_write(s, n);
    } else if (t->kind == 1) {
        sb_add(t->buf, s, n);
    } else if (t->kind == 2 && !t->err) {
        t->err = pal_write(t->file, s, n);
    }
}

void out_puts(const char *s)
{
    out_write(s, strlen(s));
}

void out_printf(const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < (int)sizeof(tmp)) {
        out_write(tmp, n);
        return;
    }
    Sbuf b;
    sb_init(&b);
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    out_write(b.s, b.len);
    sb_free(&b);
}

bool out_is_console(void)
{
    return !top();
}

void out_color(int fg, int bg)
{
    if (out_is_console())
        pal_con_set_color(fg, bg);
}

void out_reset_color(void)
{
    if (out_is_console())
        pal_con_reset_color();
}

void err_printf(const char *fmt, ...)
{
    Sbuf b;
    sb_init(&b);
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    int fg, bg;
    pal_con_get_color(&fg, &bg);
    pal_con_set_color(C_LIGHTRED, bg);
    pal_con_write(b.s, b.len);
    pal_con_set_color(fg, bg);
    sb_free(&b);
}

static bool data_mode, data_first;

bool out_data_mode(void)
{
    return data_mode;
}

void out_set_data_mode(bool on)
{
    data_mode = on;
    data_first = true;
}

void data_record(void)
{
    if (!data_first)
        out_write("\n", 1);
    data_first = false;
}

void data_field(const char *key, const char *fmt, ...)
{
    Sbuf b;
    sb_init(&b);
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    for (size_t i = 0; i < b.len; i++) /* one line per field */
        if (b.s[i] == '\n' || b.s[i] == '\r')
            b.s[i] = ' ';
    out_puts(key);
    out_write("=", 1);
    if (b.len)
        out_write(b.s, b.len);
    out_write("\n", 1);
    sb_free(&b);
}

void data_key(const char *label, char *out, size_t n)
{
    size_t k = 0;
    for (const char *p = label; *p && k + 1 < n; p++) {
        char c = (char)tolower((uint8_t)*p);
        if (isalnum((uint8_t)c))
            out[k++] = c;
        else if (k && out[k - 1] != '_')
            out[k++] = '_';
    }
    while (k && out[k - 1] == '_')
        k--;
    out[k] = 0;
}

void info_line(int indent, int width, const char *label, const char *fmt, ...)
{
    Sbuf b;
    sb_init(&b);
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    if (data_mode) {
        char key[64];
        data_key(label, key, sizeof(key));
        data_field(key, "%s", b.s ? b.s : "");
    } else {
        char *l = xasprintf("%s:", label);
        out_printf("%*s%-*s %s\n", indent, "", width, l, b.s ? b.s : "");
        free(l);
    }
    sb_free(&b);
}

void out_push_capture(Sbuf *b)
{
    if (nsinks >= MAX_SINKS)
        rt_fatal("output redirection nested too deeply");
    sinks[nsinks++] = (Sink){ 1, b, NULL, 0 };
}

int out_push_file(const char *path, bool append)
{
    if (nsinks >= MAX_SINKS)
        return PAL_ENOMEM;
    PalFile *f;
    int r = pal_open(path, PAL_O_WRITE | PAL_O_CREATE | (append ? PAL_O_APPEND : PAL_O_TRUNC), &f);
    if (r)
        return r;
    sinks[nsinks++] = (Sink){ 2, NULL, f, 0 };
    return PAL_OK;
}

int out_pop(void)
{
    if (!nsinks)
        return PAL_OK;
    Sink s = sinks[--nsinks];
    if (s.kind == 2) {
        int r = pal_close(s.file);
        return s.err ? s.err : r;
    }
    return PAL_OK;
}

/* ---- Keyboard ---- */

#define KEYBUF 32
static PalKey keybuf[KEYBUF];
static int kb_head, kb_count;
static bool break_flag;
static uint64_t last_poll;

static void kb_push(const PalKey *k)
{
    if (kb_count == KEYBUF)
        return;
    keybuf[(kb_head + kb_count++) % KEYBUF] = *k;
}

bool platform_break_pending(void);

void con_poll(void)
{
    if (platform_break_pending()) /* Ctrl-C seen by the firmware key notification */
        break_flag = true;
    uint64_t now = pal_ticks_ms();
    if (now - last_poll < 50 || !pal_con_interactive())
        return; /* with redirected input the keys are script lines, not keystrokes */
    last_poll = now;
    PalKey k;
    while (pal_con_read_key(&k, 0)) {
        if (k.ch == 3) { /* Ctrl-C */
            break_flag = true;
            kb_count = 0;
        } else {
            kb_push(&k);
        }
    }
}

static void set_break(void)
{
    break_flag = true;
}

bool con_break(void)
{
    con_poll();
    return break_flag;
}

void con_clear_break(void)
{
    break_flag = false;
}

bool con_get_key(PalKey *k, int timeout_ms)
{
    if (kb_count) {
        *k = keybuf[kb_head];
        kb_head = (kb_head + 1) % KEYBUF;
        kb_count--;
        return true;
    }
    return pal_con_read_key(k, timeout_ms);
}

bool con_pause(const char *msg)
{
    if (!msg)
        msg = "Press any key to continue...";
    pal_con_write(msg, strlen(msg));
    PalKey k;
    pal_con_raw(true);
    con_get_key(&k, -1);
    pal_con_raw(false);
    pal_con_write("\n", 1);
    if (k.ch == 3) {
        break_flag = true;
        return false;
    }
    return k.scan != KEY_ESC;
}
