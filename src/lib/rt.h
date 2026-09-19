/* NESH runtime: standard C subset + shared helpers.
 * The EFI build uses the freestanding implementation in libc.c/fmt.c;
 * the host build (NESH_HOST) uses the system C library. */
#ifndef NESH_RT_H
#define NESH_RT_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

#ifdef NESH_HOST
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#else
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strcpy(char *d, const char *s);
char *strcat(char *d, const char *s);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
size_t strnlen(const char *s, size_t n);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
unsigned long strtoul(const char *s, char **end, int base);
long strtol(const char *s, char **end, int base);

void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *realloc(void *p, size_t n);
void free(void *p);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isalpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static inline int isalnum(int c) { return isdigit(c) || isalpha(c); }
static inline int isxdigit(int c) { return isdigit(c) || ((c | 32) >= 'a' && (c | 32) <= 'f'); }
static inline int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int isprint(int c) { return c >= 0x20 && c < 0x7f; }
static inline int toupper(int c) { return islower(c) ? c - 32 : c; }
static inline int tolower(int c) { return isupper(c) ? c + 32 : c; }
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Allocation helpers: abort the shell on out-of-memory. */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Growable string buffer (always NUL terminated). */
typedef struct {
    char *s;
    size_t len;
    size_t cap;
} Sbuf;

void sb_init(Sbuf *b);
void sb_free(Sbuf *b);
void sb_reserve(Sbuf *b, size_t extra);
void sb_putc(Sbuf *b, char c);
void sb_add(Sbuf *b, const char *s, size_t n);
void sb_adds(Sbuf *b, const char *s);
void sb_printf(Sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_vprintf(Sbuf *b, const char *fmt, va_list ap);
void sb_clear(Sbuf *b);
char *sb_steal(Sbuf *b); /* returns the string, buffer becomes empty */

/* UTF-8 helpers. */
int utf8_decode(const char *s, size_t len, uint32_t *cp); /* bytes consumed (>=1) */
int utf8_encode(uint32_t cp, char out[4]);                /* bytes written */
size_t utf8_len(const char *s, size_t n);                 /* code points */
size_t utf8_offset(const char *s, size_t n, size_t cps);  /* byte offset of code point index */
void sb_put_cp(Sbuf *b, uint32_t cp);
/* UCS-2 conversion; returned buffers are malloc'd. */
uint16_t *utf8_to_ucs2(const char *s, size_t *out_units);
char *ucs2_to_utf8(const uint16_t *s, size_t units); /* units == (size_t)-1: NUL terminated */
size_t ucs2_len(const uint16_t *s);

/* Misc. */
bool str_iequal(const char *a, const char *b);
bool glob_match(const char *pat, const char *s, bool icase);
bool parse_int(const char *s, int64_t *out); /* decimal, 0x, &H, 0b, &B, &O */

/* Fatal error from the runtime: implemented by the platform layer. */
void rt_fatal(const char *msg) __attribute__((noreturn));

#endif
