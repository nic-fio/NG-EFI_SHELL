/* Shared helpers: allocation wrappers, string buffer, UTF-8, globbing. */
#include "rt.h"

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        rt_fatal("out of memory");
    return p;
}

void *xcalloc(size_t n, size_t m)
{
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p)
        rt_fatal("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        rt_fatal("out of memory");
    return p;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *r = xmalloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

char *xasprintf(const char *fmt, ...)
{
    Sbuf b;
    sb_init(&b);
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    return sb_steal(&b);
}

/* ---- Sbuf ---- */

void sb_init(Sbuf *b)
{
    b->s = NULL;
    b->len = b->cap = 0;
}

void sb_free(Sbuf *b)
{
    free(b->s);
    sb_init(b);
}

void sb_reserve(Sbuf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap * 2 : 32;
    while (cap < b->len + extra + 1)
        cap *= 2;
    b->s = xrealloc(b->s, cap);
    b->cap = cap;
    b->s[b->len] = 0; /* the buffer is a valid string even if nothing is added */
}

void sb_putc(Sbuf *b, char c)
{
    sb_reserve(b, 1);
    b->s[b->len++] = c;
    b->s[b->len] = 0;
}

void sb_add(Sbuf *b, const char *s, size_t n)
{
    sb_reserve(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void sb_adds(Sbuf *b, const char *s)
{
    sb_add(b, s, strlen(s));
}

void sb_vprintf(Sbuf *b, const char *fmt, va_list ap)
{
    if (!fmt)
        return;
    va_list ap2;
    va_copy(ap2, ap);
    char tmp[256];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (n < (int)sizeof(tmp)) {
        sb_add(b, tmp, n);
    } else {
        sb_reserve(b, n);
        vsnprintf(b->s + b->len, n + 1, fmt, ap2);
        b->len += n;
    }
    va_end(ap2);
}

void sb_printf(Sbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(b, fmt, ap);
    va_end(ap);
}

void sb_clear(Sbuf *b)
{
    b->len = 0;
    if (b->s)
        b->s[0] = 0;
}

char *sb_steal(Sbuf *b)
{
    char *s = b->s ? b->s : xstrdup("");
    sb_init(b);
    return s;
}

/* ---- UTF-8 ---- */

int utf8_decode(const char *s, size_t len, uint32_t *cp)
{
    const uint8_t *u = (const uint8_t *)s;
    if (!len) {
        *cp = 0;
        return 0;
    }
    uint8_t c = u[0];
    int n;
    uint32_t v;
    if (c < 0x80) {
        *cp = c;
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        n = 2, v = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3, v = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4, v = c & 0x07;
    } else {
        *cp = 0xFFFD;
        return 1;
    }
    if ((size_t)n > len) {
        *cp = 0xFFFD;
        return 1;
    }
    for (int i = 1; i < n; i++) {
        if ((u[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        v = (v << 6) | (u[i] & 0x3F);
    }
    *cp = v;
    return n;
}

int utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t utf8_len(const char *s, size_t n)
{
    size_t count = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += utf8_decode(s + i, n - i, &cp);
        count++;
    }
    return count;
}

size_t utf8_offset(const char *s, size_t n, size_t cps)
{
    size_t i = 0;
    while (i < n && cps--) {
        uint32_t cp;
        i += utf8_decode(s + i, n - i, &cp);
    }
    return i;
}

void sb_put_cp(Sbuf *b, uint32_t cp)
{
    char tmp[4];
    sb_add(b, tmp, utf8_encode(cp, tmp));
}

uint16_t *utf8_to_ucs2(const char *s, size_t *out_units)
{
    size_t n = strlen(s);
    uint16_t *r = xmalloc((n + 1) * sizeof(uint16_t));
    size_t k = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += utf8_decode(s + i, n - i, &cp);
        r[k++] = cp > 0xFFFF ? 0xFFFD : (uint16_t)cp;
    }
    r[k] = 0;
    if (out_units)
        *out_units = k;
    return r;
}

size_t ucs2_len(const uint16_t *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

char *ucs2_to_utf8(const uint16_t *s, size_t units)
{
    if (units == (size_t)-1)
        units = ucs2_len(s);
    Sbuf b;
    sb_init(&b);
    sb_reserve(&b, units);
    for (size_t i = 0; i < units && s[i]; i++)
        sb_put_cp(&b, s[i]);
    return sb_steal(&b);
}

/* ---- Misc ---- */

bool str_iequal(const char *a, const char *b)
{
    return strcasecmp(a, b) == 0;
}

bool glob_match(const char *pat, const char *s, bool icase)
{
    const char *star = NULL, *ss = NULL;
    while (*s) {
        char p = *pat, c = *s;
        if (icase) {
            p = (char)tolower((uint8_t)p);
            c = (char)tolower((uint8_t)c);
        }
        if (*pat == '*') {
            star = ++pat;
            ss = s;
        } else if (*pat == '?' || (*pat && p == c)) {
            pat++, s++;
        } else if (star) {
            pat = star;
            s = ++ss;
        } else {
            return false;
        }
    }
    while (*pat == '*')
        pat++;
    return !*pat;
}

bool parse_int(const char *s, int64_t *out)
{
    bool neg = false;
    while (isspace((uint8_t)*s))
        s++;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    int base = 10;
    if (s[0] == '0' && (s[1] | 32) == 'x')
        base = 16, s += 2;
    else if (s[0] == '0' && (s[1] | 32) == 'b')
        base = 2, s += 2;
    else if (s[0] == '&' && (s[1] | 32) == 'h')
        base = 16, s += 2;
    else if (s[0] == '&' && (s[1] | 32) == 'b')
        base = 2, s += 2;
    else if (s[0] == '&' && (s[1] | 32) == 'o')
        base = 8, s += 2;
    if (!*s)
        return false;
    uint64_t v = 0;
    for (; *s; s++) {
        int c = (uint8_t)*s, d;
        if (c == '_')
            continue;
        if (isdigit(c))
            d = c - '0';
        else if (isalpha(c))
            d = (c | 32) - 'a' + 10;
        else
            break;
        if (d >= base)
            return false;
        v = v * base + d;
    }
    while (isspace((uint8_t)*s))
        s++;
    if (*s)
        return false;
    *out = neg ? -(int64_t)v : (int64_t)v;
    return true;
}
