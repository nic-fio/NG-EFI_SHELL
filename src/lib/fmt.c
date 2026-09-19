/* vsnprintf for the EFI build: %d %i %u %x %X %o %c %s %p %%,
 * flags '-' '0' '+' ' ', width/precision (also '*'), length hh h l ll z j t. */
#include "rt.h"

typedef struct {
    char *buf;
    size_t size;
    size_t pos;
} Out;

static void put(Out *o, char c)
{
    if (o->pos + 1 < o->size)
        o->buf[o->pos] = c;
    o->pos++;
}

static void pad(Out *o, char c, int n)
{
    while (n-- > 0)
        put(o, c);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    Out o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;
        bool left = false, zero = false, plus = false, space = false;
        for (;; fmt++) {
            if (*fmt == '-')
                left = true;
            else if (*fmt == '0')
                zero = true;
            else if (*fmt == '+')
                plus = true;
            else if (*fmt == ' ')
                space = true;
            else if (*fmt == '#')
                ;
            else
                break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0)
                left = true, width = -width;
            fmt++;
        } else {
            while (isdigit((uint8_t)*fmt))
                width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (isdigit((uint8_t)*fmt))
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }
        int lng = 0; /* 0 int, 1 long, 2 long long, -1 short, -2 char */
        for (;; fmt++) {
            if (*fmt == 'l')
                lng++;
            else if (*fmt == 'h')
                lng--;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't')
                lng = 2;
            else
                break;
        }
        char c = *fmt;
        if (!c)
            break;
        if (c == '%') {
            put(&o, '%');
            continue;
        }
        if (c == 'c') {
            char ch = (char)va_arg(ap, int);
            if (!left)
                pad(&o, ' ', width - 1);
            put(&o, ch);
            if (left)
                pad(&o, ' ', width - 1);
            continue;
        }
        if (c == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int n = 0;
            while (s[n] && (prec < 0 || n < prec))
                n++;
            if (!left)
                pad(&o, ' ', width - n);
            for (int i = 0; i < n; i++)
                put(&o, s[i]);
            if (left)
                pad(&o, ' ', width - n);
            continue;
        }
        unsigned long long v;
        bool neg = false;
        int base = 10;
        bool upper = false;
        if (c == 'd' || c == 'i') {
            long long sv;
            if (lng >= 2)
                sv = va_arg(ap, long long);
            else if (lng == 1)
                sv = va_arg(ap, long);
            else
                sv = va_arg(ap, int);
            if (lng == -1)
                sv = (short)sv;
            else if (lng <= -2)
                sv = (signed char)sv;
            neg = sv < 0;
            v = neg ? 0ULL - (unsigned long long)sv : (unsigned long long)sv;
        } else if (c == 'u' || c == 'x' || c == 'X' || c == 'o' || c == 'p') {
            if (c == 'p') {
                v = (uintptr_t)va_arg(ap, void *);
                lng = 2;
            } else if (lng >= 2)
                v = va_arg(ap, unsigned long long);
            else if (lng == 1)
                v = va_arg(ap, unsigned long);
            else
                v = va_arg(ap, unsigned int);
            if (lng == -1)
                v = (unsigned short)v;
            else if (lng <= -2)
                v = (unsigned char)v;
            base = c == 'o' ? 8 : c == 'u' ? 10 : 16;
            upper = c == 'X';
        } else {
            put(&o, '%');
            put(&o, c);
            continue;
        }
        char tmp[24];
        int n = 0;
        do {
            int d = (int)(v % base);
            tmp[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
            v /= base;
        } while (v);
        if (prec == 0 && n == 1 && tmp[0] == '0')
            n = 0;
        if (c == 'p') {
            tmp[n++] = 'x';
            tmp[n++] = '0';
        }
        char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
        int digits = prec > n ? prec : n;
        int total = digits + (sign ? 1 : 0);
        if (prec >= 0)
            zero = false;
        if (!left && !zero)
            pad(&o, ' ', width - total);
        if (sign)
            put(&o, sign);
        if (!left && zero)
            pad(&o, '0', width - total);
        pad(&o, '0', digits - n);
        while (n)
            put(&o, tmp[--n]);
        if (left)
            pad(&o, ' ', width - total);
    }
    if (size)
        buf[o.pos < size ? o.pos : size - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
