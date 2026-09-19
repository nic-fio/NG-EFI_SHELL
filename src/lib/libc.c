/* Freestanding C library subset for the EFI build. */
#include "rt.h"
#include "../pal/pal.h"

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    while (n >= 8) {
        uint64_t v;
        __builtin_memcpy(&v, sp, 8);
        __builtin_memcpy(dp, &v, 8);
        dp += 8, sp += 8, n -= 8;
    }
    while (n--)
        *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (dp == sp || n == 0)
        return d;
    if (dp < sp || dp >= sp + n)
        return memcpy(d, s, n);
    while (n--)
        dp[n] = sp[n];
    return d;
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *dp = d;
    while (n--)
        *dp++ = (uint8_t)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = s;
    for (; n; n--, p++)
        if (*p == (uint8_t)c)
            return (void *)p;
    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return p - s;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (uint8_t)*a - (uint8_t)*b;
        if (!*a)
            break;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower((uint8_t)*a) == tolower((uint8_t)*b))
        a++, b++;
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = tolower((uint8_t)*a), y = tolower((uint8_t)*b);
        if (x != y)
            return x - y;
        if (!x)
            break;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c)
            r = s;
        if (!*s)
            return (char *)r;
    }
}

char *strstr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (!nl)
        return (char *)h;
    for (; *h; h++)
        if (*h == *n && !strncmp(h, n, nl))
            return (char *)h;
    return NULL;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

char *strcat(char *d, const char *s)
{
    strcpy(d + strlen(d), s);
    return d;
}

size_t strnlen(const char *s, size_t n)
{
    size_t l = 0;
    while (l < n && s[l])
        l++;
    return l;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    s += strcspn(s, accept);
    return *s ? (char *)s : NULL;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long)strtoull(s, end, base);
}

char *strdup(const char *s)
{
    return strndup(s, strlen(s));
}

char *strndup(const char *s, size_t n)
{
    size_t l = 0;
    while (l < n && s[l])
        l++;
    char *r = malloc(l + 1);
    if (r) {
        memcpy(r, s, l);
        r[l] = 0;
    }
    return r;
}

/* ---- Heap: pool allocations with a size header (needed by realloc). ---- */

typedef struct {
    size_t size;
    size_t pad; /* keeps the payload 16-byte aligned */
} HeapHdr;

void *malloc(size_t n)
{
    HeapHdr *h = pal_alloc(sizeof(HeapHdr) + (n ? n : 1));
    if (!h)
        return NULL;
    h->size = n;
    return h + 1;
}

void *calloc(size_t n, size_t m)
{
    size_t t = n * m;
    if (m && t / m != n)
        return NULL;
    void *p = malloc(t);
    if (p)
        memset(p, 0, t);
    return p;
}

void free(void *p)
{
    if (p)
        pal_free((HeapHdr *)p - 1);
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    HeapHdr *h = (HeapHdr *)p - 1;
    if (n <= h->size && n >= h->size / 2) {
        h->size = n;
        return p;
    }
    void *q = malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, MIN(n, h->size));
    free(p);
    return q;
}

/* ---- Numbers ---- */

unsigned long long strtoull(const char *s, char **end, int base)
{
    unsigned long long v = 0;
    while (isspace((uint8_t)*s))
        s++;
    if (*s == '+')
        s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 32) == 'x' && isxdigit((uint8_t)s[2])) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = 10;
    }
    for (;; s++) {
        int c = (uint8_t)*s, d;
        if (isdigit(c))
            d = c - '0';
        else if (isalpha(c))
            d = (c | 32) - 'a' + 10;
        else
            break;
        if (d >= base)
            break;
        v = v * base + d;
    }
    if (end)
        *end = (char *)s;
    return v;
}

long long strtoll(const char *s, char **end, int base)
{
    while (isspace((uint8_t)*s))
        s++;
    if (*s == '-')
        return -(long long)strtoull(s + 1, end, base);
    return (long long)strtoull(s, end, base);
}

/* Shell sort: small, non-recursive, good enough for directory listings. */
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    uint8_t *a = base;
    uint8_t tmp[256];
    uint8_t *t = sz <= sizeof(tmp) ? tmp : malloc(sz);
    if (!t)
        return;
    for (size_t gap = n / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < n; i++) {
            memcpy(t, a + i * sz, sz);
            size_t j = i;
            while (j >= gap && cmp(a + (j - gap) * sz, t) > 0) {
                memcpy(a + j * sz, a + (j - gap) * sz, sz);
                j -= gap;
            }
            memcpy(a + j * sz, t, sz);
        }
    }
    if (t != tmp)
        free(t);
}

long strtol(const char *s, char **end, int base)
{
    return (long)strtoll(s, end, base);
}
