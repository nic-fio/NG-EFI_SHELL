/* Core built-in functions of NESH BASIC (portable). */
#include "interp_int.h"
#include "../core/shell.h"

#define FN(name) static bool name(Interp *in, Node *call, Value *a, int n, Value *out)
#define UNUSED (void)in, (void)call, (void)a, (void)n

static Value mkstr(const char *s, size_t len)
{
    return v_str(str_new(s, len));
}

static size_t cp_len(const Str *s)
{
    return utf8_len(s->s, s->len);
}

static size_t cp_off(const Str *s, int64_t cps)
{
    if (cps <= 0)
        return 0;
    return utf8_offset(s->s, s->len, (size_t)cps);
}

/* ---- Strings ---- */

FN(f_len)
{
    UNUSED;
    *out = v_int((int64_t)cp_len(a[0].s));
    return true;
}

FN(f_left)
{
    UNUSED;
    size_t o = cp_off(a[0].s, a[1].i);
    *out = mkstr(a[0].s->s, o);
    return true;
}

FN(f_right)
{
    UNUSED;
    int64_t total = (int64_t)cp_len(a[0].s);
    int64_t k = a[1].i < 0 ? 0 : a[1].i > total ? total : a[1].i;
    size_t o = cp_off(a[0].s, total - k);
    *out = mkstr(a[0].s->s + o, a[0].s->len - o);
    return true;
}

FN(f_mid)
{
    UNUSED;
    if (a[1].i < 1)
        return rt_err(in, call, "MID$: start position must be 1 or more");
    size_t s = cp_off(a[0].s, a[1].i - 1);
    size_t e = a[0].s->len;
    if (n > 2) {
        if (a[2].i < 0)
            return rt_err(in, call, "MID$: negative length");
        e = s + utf8_offset(a[0].s->s + s, a[0].s->len - s, (size_t)a[2].i);
    }
    *out = mkstr(a[0].s->s + s, e - s);
    return true;
}

FN(f_instr)
{
    UNUSED;
    int64_t start = 1;
    const Str *h, *nd;
    if (n == 3) {
        if (a[0].t != V_INT || a[1].t != V_STR || a[2].t != V_STR)
            return rt_err(in, call, "INSTR([start,] text$, find$)");
        start = a[0].i;
        h = a[1].s;
        nd = a[2].s;
    } else {
        if (a[0].t != V_STR || a[1].t != V_STR)
            return rt_err(in, call, "INSTR([start,] text$, find$)");
        h = a[0].s;
        nd = a[1].s;
    }
    if (start < 1)
        start = 1;
    size_t o = cp_off(h, start - 1);
    *out = v_int(0);
    if (o > h->len)
        return true;
    for (size_t i = o; i + nd->len <= h->len; i++) {
        if (!memcmp(h->s + i, nd->s, nd->len)) {
            *out = v_int((int64_t)utf8_len(h->s, i) + 1);
            break;
        }
    }
    return true;
}

static bool change_case(Value *a, Value *out, bool up)
{
    Str *r = str_new(a[0].s->s, a[0].s->len);
    for (size_t i = 0; i < r->len; i++)
        r->s[i] = (char)(up ? toupper((uint8_t)r->s[i]) : tolower((uint8_t)r->s[i]));
    *out = v_str(r);
    return true;
}

FN(f_ucase) { UNUSED; return change_case(a, out, true); }
FN(f_lcase) { UNUSED; return change_case(a, out, false); }

static bool do_trim(Value *a, Value *out, bool left, bool right)
{
    const char *s = a[0].s->s;
    size_t b = 0, e = a[0].s->len;
    while (left && b < e && isspace((uint8_t)s[b]))
        b++;
    while (right && e > b && isspace((uint8_t)s[e - 1]))
        e--;
    *out = mkstr(s + b, e - b);
    return true;
}

FN(f_trim) { UNUSED; return do_trim(a, out, true, true); }
FN(f_ltrim) { UNUSED; return do_trim(a, out, true, false); }
FN(f_rtrim) { UNUSED; return do_trim(a, out, false, true); }

FN(f_replace)
{
    UNUSED;
    const Str *s = a[0].s, *f = a[1].s, *r = a[2].s;
    if (!f->len) {
        *out = v_copy(&a[0]);
        return true;
    }
    Sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < s->len;) {
        if (i + f->len <= s->len && !memcmp(s->s + i, f->s, f->len)) {
            sb_add(&b, r->s, r->len);
            i += f->len;
        } else {
            sb_putc(&b, s->s[i++]);
        }
    }
    *out = v_str(str_take_sb(&b));
    return true;
}

FN(f_string)
{
    UNUSED;
    if (a[0].i < 0 || a[0].i > 1000000)
        return rt_err(in, call, "STRING$: invalid count");
    char unit[4];
    size_t ul;
    if (a[1].t == V_STR) {
        if (!a[1].s->len)
            return rt_err(in, call, "STRING$: empty character");
        uint32_t cp;
        ul = (size_t)utf8_decode(a[1].s->s, a[1].s->len, &cp);
        memcpy(unit, a[1].s->s, ul);
    } else {
        ul = (size_t)utf8_encode((uint32_t)a[1].i, unit);
    }
    Sbuf b;
    sb_init(&b);
    for (int64_t i = 0; i < a[0].i; i++)
        sb_add(&b, unit, ul);
    *out = v_str(str_take_sb(&b));
    return true;
}

FN(f_space)
{
    UNUSED;
    if (a[0].i < 0 || a[0].i > 1000000)
        return rt_err(in, call, "SPACE$: invalid count");
    Str *r = str_new(NULL, (size_t)a[0].i);
    memset(r->s, ' ', r->len);
    *out = v_str(r);
    return true;
}

static bool pad(Value *a, int n, Value *out, bool left)
{
    int64_t w = a[1].i;
    const char *fill = " ";
    size_t fl = 1;
    if (n > 2 && a[2].s->len) {
        uint32_t cp;
        fl = (size_t)utf8_decode(a[2].s->s, a[2].s->len, &cp);
        fill = a[2].s->s;
    }
    int64_t len = (int64_t)cp_len(a[0].s);
    Sbuf b;
    sb_init(&b);
    if (!left)
        sb_add(&b, a[0].s->s, a[0].s->len);
    for (int64_t i = len; i < w; i++)
        sb_add(&b, fill, fl);
    if (left)
        sb_add(&b, a[0].s->s, a[0].s->len);
    *out = v_str(str_take_sb(&b));
    return true;
}

FN(f_lpad) { UNUSED; return pad(a, n, out, true); }
FN(f_rpad) { UNUSED; return pad(a, n, out, false); }

FN(f_chr)
{
    UNUSED;
    if (a[0].i < 0 || a[0].i > 0x10FFFF)
        return rt_err(in, call, "CHR$: invalid character code");
    char buf[4];
    *out = mkstr(buf, (size_t)utf8_encode((uint32_t)a[0].i, buf));
    return true;
}

FN(f_asc)
{
    UNUSED;
    if (!a[0].s->len)
        return rt_err(in, call, "ASC: empty string");
    uint32_t cp;
    utf8_decode(a[0].s->s, a[0].s->len, &cp);
    *out = v_int(cp);
    return true;
}

FN(f_val)
{
    UNUSED;
    /* Lenient: leading spaces, optional sign, prefixes; stops at the first invalid char. */
    const char *s = a[0].s->s;
    while (isspace((uint8_t)*s))
        s++;
    char buf[80];
    size_t k = 0;
    while (s[k] && k < sizeof(buf) - 1 && (isalnum((uint8_t)s[k]) || s[k] == '-' || s[k] == '+' || s[k] == '&' || s[k] == '_'))
        k++;
    memcpy(buf, s, k);
    buf[k] = 0;
    int64_t v = 0;
    while (k > 0 && !parse_int(buf, &v))
        buf[--k] = 0;
    *out = v_int(k ? v : 0);
    return true;
}

FN(f_str)
{
    UNUSED;
    char buf[32];
    int l = snprintf(buf, sizeof(buf), "%lld", (long long)a[0].i);
    *out = mkstr(buf, (size_t)l);
    return true;
}

static bool radix(Interp *in, Node *call, Value *a, int n, Value *out, int bits, const char *digits)
{
    uint64_t v = (uint64_t)a[0].i;
    char buf[80];
    int k = 0;
    do {
        buf[k++] = digits[v & ((1u << bits) - 1)];
        v >>= bits;
    } while (v);
    int64_t width = n > 1 ? a[1].i : 0;
    if (width < 0 || width > 64)
        return rt_err(in, call, "invalid number of digits");
    while (k < width)
        buf[k++] = '0';
    char r[80];
    for (int i = 0; i < k; i++)
        r[i] = buf[k - 1 - i];
    *out = mkstr(r, (size_t)k);
    return true;
}

FN(f_hex) { return radix(in, call, a, n, out, 4, "0123456789ABCDEF"); }
FN(f_bin) { return radix(in, call, a, n, out, 1, "01"); }
FN(f_oct) { return radix(in, call, a, n, out, 3, "01234567"); }

FN(f_split)
{
    UNUSED;
    Var *arr = interp_array_arg(in, call->args[2], true);
    if (!arr)
        return false;
    if (!name_is_str(arr->name))
        return rt_err(in, call, "SPLIT needs a string array (name ending with $)");
    const Str *s = a[0].s, *sep = a[1].s;
    int64_t count = 0;
    Value *parts = NULL;
    size_t start = 0;
    if (s->len) {
        for (size_t i = 0;; ) {
            bool at_end = i >= s->len;
            bool is_sep = !at_end && (sep->len ? (i + sep->len <= s->len && !memcmp(s->s + i, sep->s, sep->len))
                                               : isspace((uint8_t)s->s[i]));
            if (at_end || is_sep) {
                /* with an empty separator, runs of whitespace count as one */
                if (sep->len || i > start) {
                    parts = xrealloc(parts, sizeof(Value) * (size_t)(count + 1));
                    parts[count++] = mkstr(s->s + start, i - start);
                }
                if (at_end)
                    break;
                i += sep->len ? sep->len : 1;
                start = i;
            } else {
                i++;
            }
        }
    }
    var_array_resize(arr, 0, false);
    var_array_resize(arr, count, false);
    for (int64_t i = 0; i < count; i++) {
        v_free(&arr->arr[i]);
        arr->arr[i] = parts[i];
    }
    free(parts);
    *out = v_int(count);
    return true;
}

FN(f_join)
{
    UNUSED;
    Var *arr = interp_array_arg(in, call->args[0], false);
    if (!arr)
        return false;
    Sbuf b;
    sb_init(&b);
    for (int64_t i = 0; i < arr->count; i++) {
        if (i)
            sb_add(&b, a[1].s->s, a[1].s->len);
        char *s = v_to_cstr(&arr->arr[i]);
        sb_adds(&b, s);
        free(s);
    }
    *out = v_str(str_take_sb(&b));
    return true;
}

FN(f_ubound)
{
    UNUSED;
    Var *arr = interp_array_arg(in, call->args[0], false);
    if (!arr)
        return false;
    *out = v_int(arr->count - 1);
    return true;
}

/* ---- Math ---- */

FN(f_abs) { UNUSED; *out = v_int(a[0].i < 0 ? (int64_t)(0 - (uint64_t)a[0].i) : a[0].i); return true; }
FN(f_sgn) { UNUSED; *out = v_int(a[0].i > 0 ? 1 : a[0].i < 0 ? -1 : 0); return true; }

FN(f_min)
{
    UNUSED;
    int64_t m = a[0].i;
    for (int i = 1; i < n; i++)
        if (a[i].i < m)
            m = a[i].i;
    *out = v_int(m);
    return true;
}

FN(f_max)
{
    UNUSED;
    int64_t m = a[0].i;
    for (int i = 1; i < n; i++)
        if (a[i].i > m)
            m = a[i].i;
    *out = v_int(m);
    return true;
}

FN(f_rnd)
{
    UNUSED;
    uint64_t x = in->rnd; /* xorshift64 */
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    in->rnd = x;
    if (n) {
        if (a[0].i <= 0)
            return rt_err(in, call, "RND: limit must be positive");
        *out = v_int((int64_t)(x % (uint64_t)a[0].i));
    } else {
        *out = v_int((int64_t)(x >> 33));
    }
    return true;
}

/* ---- Program and system ---- */

FN(f_err) { UNUSED; *out = v_int(in->err); return true; }
FN(f_argc) { UNUSED; *out = v_int(in->argc > 0 ? in->argc - 1 : 0); return true; }

FN(f_arg)
{
    UNUSED;
    if (a[0].i < 0 || a[0].i >= in->argc)
        *out = v_cstr("");
    else
        *out = v_cstr(in->argv[a[0].i]);
    return true;
}

FN(f_ticks) { UNUSED; *out = v_int((int64_t)pal_ticks_ms()); return true; }

FN(f_date)
{
    UNUSED;
    PalTime t;
    char buf[16] = "";
    if (pal_get_time(&t))
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.year, t.month, t.day);
    *out = v_cstr(buf);
    return true;
}

FN(f_time)
{
    UNUSED;
    PalTime t;
    char buf[16] = "";
    if (pal_get_time(&t))
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.hour, t.min, t.sec);
    *out = v_cstr(buf);
    return true;
}

static const char *key_name(const PalKey *k, char *buf)
{
    static const char *names[] = { "", "UP", "DOWN", "RIGHT", "LEFT", "HOME", "END", "INSERT", "DELETE",
                                   "PGUP", "PGDN" };
    if (k->scan) {
        if (k->scan < ARRAY_SIZE(names))
            return names[k->scan];
        if (k->scan >= KEY_F1 && k->scan < KEY_F1 + 10) {
            snprintf(buf, 8, "F%d", k->scan - KEY_F1 + 1);
            return buf;
        }
        if (k->scan == KEY_ESC)
            return "ESC";
        return "";
    }
    switch (k->ch) {
    case '\r': case '\n': return "ENTER";
    case 8: return "BACKSPACE";
    case 9: return "TAB";
    case 27: return "ESC";
    }
    buf[utf8_encode(k->ch, buf)] = 0;
    return buf;
}

/* KEY$ : non-blocking; KEY$(ms): wait up to ms (-1 = forever).
 * Special keys return names: "UP", "ENTER", "ESC", "F1"... */
FN(f_key)
{
    UNUSED;
    int timeout = n ? (int)a[0].i : 0;
    PalKey k;
    pal_con_raw(true);
    bool got = con_get_key(&k, timeout < 0 ? -1 : timeout);
    pal_con_raw(false);
    char buf[8];
    if (got && k.ch == 3)
        return rt_err(in, call, "interrupted (Ctrl-C)");
    *out = v_cstr(got ? key_name(&k, buf) : "");
    return true;
}

FN(f_cwd) { UNUSED; *out = v_cstr(shell_cwd()); return true; }

static char *resolve_arg(Interp *in, Node *call, const Value *v)
{
    char *p = path_resolve(v->s->s);
    if (!p)
        rt_err(in, call, "invalid path: %s", v->s->s);
    return p;
}

FN(f_fileexists)
{
    UNUSED;
    char *p = path_resolve(a[0].s->s);
    PalStat st;
    *out = v_int(p && pal_stat(p, &st) == PAL_OK && !st.is_dir ? -1 : 0);
    free(p);
    return true;
}

FN(f_direxists)
{
    UNUSED;
    char *p = path_resolve(a[0].s->s);
    PalStat st;
    *out = v_int(p && pal_stat(p, &st) == PAL_OK && st.is_dir ? -1 : 0);
    free(p);
    return true;
}

FN(f_filesize)
{
    UNUSED;
    char *p = path_resolve(a[0].s->s);
    PalStat st;
    int e = p ? pal_stat(p, &st) : PAL_ENOENT;
    free(p);
    in->err = e ? RC_FAIL : RC_OK;
    *out = v_int(e ? -1 : (int64_t)st.size);
    return true;
}

FN(f_readfile)
{
    UNUSED;
    char *p = resolve_arg(in, call, &a[0]);
    if (!p)
        return false;
    char *data;
    size_t len;
    int e = file_read_text(p, &data, &len);
    free(p);
    in->err = e ? RC_FAIL : RC_OK;
    if (e) {
        *out = v_cstr("");
        return true;
    }
    *out = mkstr(data, len);
    free(data);
    return true;
}

static bool write_common(Interp *in, Node *call, Value *a, Value *out, bool append)
{
    char *p = resolve_arg(in, call, &a[0]);
    if (!p)
        return false;
    char *s = v_to_cstr(&a[1]);
    int e = file_write_all(p, s, strlen(s), append);
    free(s);
    free(p);
    in->err = e ? RC_FAIL : RC_OK;
    *out = v_int(e ? RC_FAIL : RC_OK);
    return true;
}

FN(f_writefile) { UNUSED; return write_common(in, call, a, out, false); }
FN(f_appendfile) { UNUSED; return write_common(in, call, a, out, true); }

static void dir_reset(Interp *in)
{
    for (int i = 0; i < in->dir_n; i++)
        free(in->dir_list[i]);
    free(in->dir_list);
    in->dir_list = NULL;
    in->dir_n = in->dir_i = 0;
}

/* DIR$(pattern$) returns the first match (file name only), DIR$ the next ones, "" at the end. */
FN(f_dir)
{
    UNUSED;
    if (n) {
        dir_reset(in);
        const char *pat = a[0].s->s;
        char *full;
        if (!*pat)
            full = path_join(shell_cwd(), "*");
        else {
            full = path_resolve(pat);
            if (!full) {
                *out = v_cstr("");
                return true;
            }
            PalStat st;
            if (!path_has_wildcards(full) && pal_stat(full, &st) == PAL_OK && st.is_dir) {
                char *j = path_join(full, "*");
                free(full);
                full = j;
            }
        }
        int cnt = 0;
        char **list = path_glob(full, &cnt);
        free(full);
        in->dir_list = list;
        in->dir_n = cnt;
    }
    if (in->dir_i < in->dir_n)
        *out = v_cstr(path_basename(in->dir_list[in->dir_i++]));
    else
        *out = v_cstr("");
    return true;
}

FN(f_run)
{
    UNUSED;
    Sbuf cap;
    sb_init(&cap);
    int rc = basic_run_command(in, call, &cap);
    if (rc < 0) {
        sb_free(&cap);
        return false;
    }
    while (cap.len && (cap.s[cap.len - 1] == '\n' || cap.s[cap.len - 1] == '\r'))
        cap.s[--cap.len] = 0;
    *out = v_str(str_take_sb(&cap));
    return true;
}

/* MENU(title$, items$ [, timeout_s [, default]]) : items separated by '|'. */
FN(f_menu)
{
    UNUSED;
    int count = 0;
    char **items = NULL;
    const char *s = a[1].s->s;
    for (;;) {
        const char *bar = strchr(s, '|');
        size_t l = bar ? (size_t)(bar - s) : strlen(s);
        items = xrealloc(items, sizeof(char *) * (count + 1));
        items[count++] = xstrndup(s, l);
        if (!bar)
            break;
        s = bar + 1;
    }
    int timeout = n > 2 ? (int)a[2].i : 0;
    int def = n > 3 ? (int)a[3].i : 1;
    if (def < 1 || def > count)
        def = 1;
    int r = ui_menu(a[0].s->s, items, count, timeout, def);
    for (int i = 0; i < count; i++)
        free(items[i]);
    free(items);
    if (r < 0)
        return rt_err(in, call, "interrupted (Ctrl-C)");
    *out = v_int(r);
    return true;
}

/* ENV$(name$): environment variable, "" if it does not exist (ERR = 1). */
FN(f_env)
{
    UNUSED;
    char *v = env_get(a[0].s->s);
    in->err = v ? RC_OK : RC_FAIL;
    *out = v_cstr(v ? v : "");
    free(v);
    return true;
}

/* SETENV name$, value$ [, permanent]: temporary unless permanent is true. */
FN(f_setenv)
{
    UNUSED;
    char *val = v_to_cstr(&a[1]);
    const char *e = env_set(a[0].s->s, val, n > 2 && a[2].i != 0);
    free(val);
    if (e)
        return rt_err(in, call, "SETENV %s: %s", a[0].s->s, e);
    in->err = RC_OK;
    *out = v_int(0);
    return true;
}

FN(f_delenv)
{
    UNUSED;
    const char *e = env_set(a[0].s->s, NULL, false);
    in->err = e ? RC_FAIL : RC_OK;
    *out = v_int(in->err);
    return true;
}

/* RECORDS(text$, arr$): splits "-data" output into records (separated by empty lines). */
FN(f_records)
{
    UNUSED;
    Var *arr = interp_array_arg(in, call->args[1], true);
    if (!arr)
        return false;
    if (!name_is_str(arr->name))
        return rt_err(in, call, "RECORDS needs a string array (name ending with $)");
    const char *s = a[0].s->s;
    size_t len = a[0].s->len;
    Value *recs = NULL;
    int64_t count = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && (s[i] == '\n' || s[i] == '\r')) /* skip empty lines */
            i++;
        if (i >= len)
            break;
        size_t start = i;
        while (i < len && !(s[i] == '\n' && (i + 1 >= len || s[i + 1] == '\n' || (s[i + 1] == '\r' && i + 2 < len && s[i + 2] == '\n'))))
            i++;
        size_t end = i < len ? i + 1 : len; /* keep the last newline of the record */
        recs = xrealloc(recs, sizeof(Value) * (size_t)(count + 1));
        recs[count++] = mkstr(s + start, end - start);
        i = end;
    }
    var_array_resize(arr, 0, false);
    var_array_resize(arr, count, false);
    for (int64_t k = 0; k < count; k++) {
        v_free(&arr->arr[k]);
        arr->arr[k] = recs[k];
    }
    free(recs);
    *out = v_int(count);
    return true;
}

/* FIELD$(record$, key$): value of "key=value" in a record ("" and ERR=1 if missing). */
FN(f_field)
{
    UNUSED;
    const char *s = a[0].s->s, *key = a[1].s->s;
    size_t kl = a[1].s->len;
    in->err = RC_FAIL;
    *out = v_cstr("");
    for (const char *line = s; *line;) {
        const char *nl = strchr(line, '\n');
        size_t ll = nl ? (size_t)(nl - line) : strlen(line);
        if (ll > kl && line[kl] == '=' && !strncasecmp(line, key, kl)) {
            size_t vl = ll - kl - 1;
            if (vl && line[kl + 1 + vl - 1] == '\r')
                vl--;
            v_free(out);
            *out = mkstr(line + kl + 1, vl);
            in->err = RC_OK;
            break;
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    return true;
}

FN(f_platform) { UNUSED; *out = v_cstr(pal_platform_name()); return true; }
FN(f_version) { UNUSED; *out = v_cstr(NESH_VERSION); return true; }

static const BFunc core_funcs[] = {
    { "len", 1, 1, "s", f_len },
    { "left$", 2, 2, "sn", f_left },
    { "right$", 2, 2, "sn", f_right },
    { "mid$", 2, 3, "snn", f_mid },
    { "instr", 2, 3, "?", f_instr },
    { "ucase$", 1, 1, "s", f_ucase },
    { "lcase$", 1, 1, "s", f_lcase },
    { "trim$", 1, 1, "s", f_trim },
    { "ltrim$", 1, 1, "s", f_ltrim },
    { "rtrim$", 1, 1, "s", f_rtrim },
    { "replace$", 3, 3, "s", f_replace },
    { "string$", 2, 2, "n?", f_string },
    { "space$", 1, 1, "n", f_space },
    { "lpad$", 2, 3, "sns", f_lpad },
    { "rpad$", 2, 3, "sns", f_rpad },
    { "chr$", 1, 1, "n", f_chr },
    { "asc", 1, 1, "s", f_asc },
    { "val", 1, 1, "s", f_val },
    { "str$", 1, 1, "n", f_str },
    { "hex$", 1, 2, "n", f_hex },
    { "bin$", 1, 2, "n", f_bin },
    { "oct$", 1, 2, "n", f_oct },
    { "split", 3, 3, "ssa", f_split },
    { "join$", 2, 2, "as", f_join },
    { "ubound", 1, 1, "a", f_ubound },
    { "abs", 1, 1, "n", f_abs },
    { "sgn", 1, 1, "n", f_sgn },
    { "min", 1, 16, "n", f_min },
    { "max", 1, 16, "n", f_max },
    { "rnd", 0, 1, "n", f_rnd },
    { "err", 0, 0, "", f_err },
    { "argc", 0, 0, "", f_argc },
    { "arg$", 1, 1, "n", f_arg },
    { "ticks", 0, 0, "", f_ticks },
    { "date$", 0, 0, "", f_date },
    { "time$", 0, 0, "", f_time },
    { "key$", 0, 1, "n", f_key },
    { "cwd$", 0, 0, "", f_cwd },
    { "fileexists", 1, 1, "s", f_fileexists },
    { "direxists", 1, 1, "s", f_direxists },
    { "filesize", 1, 1, "s", f_filesize },
    { "readfile$", 1, 1, "s", f_readfile },
    { "writefile", 2, 2, "s?", f_writefile },
    { "appendfile", 2, 2, "s?", f_appendfile },
    { "dir$", 0, 1, "s", f_dir },
    { "run$", 1, 16, "a", f_run }, /* arguments evaluated by basic_run_command */
    { "menu", 2, 4, "ssnn", f_menu },
    { "env$", 1, 1, "s", f_env },
    { "setenv", 2, 3, "s?n", f_setenv },
    { "delenv", 1, 1, "s", f_delenv },
    { "records", 2, 2, "sa", f_records },
    { "field$", 2, 2, "s", f_field },
    { "platform$", 0, 0, "", f_platform },
    { "version$", 0, 0, "", f_version },
};

void basic_core_funcs_init(void)
{
    basic_register_funcs(core_funcs, ARRAY_SIZE(core_funcs));
}
