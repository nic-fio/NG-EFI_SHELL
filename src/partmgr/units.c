/* partmgr: sizes as the user writes and reads them. */
#include "units.h"

const char *pm_parse_size(const char *s, uint32_t bsize, uint64_t *bytes, bool *rest)
{
    *bytes = 0;
    *rest = false;
    while (isspace((uint8_t)*s))
        s++;
    if (!strcasecmp(s, "rest") || !strcasecmp(s, "all") || !strcasecmp(s, "max")) {
        *rest = true;
        return NULL;
    }
    if (!isdigit((uint8_t)*s))
        return "write a size such as 512M, 20G or rest";
    /* whole part and up to six decimals, kept as millionths */
    uint64_t whole = 0, frac = 0;
    int fd = 0;
    for (; isdigit((uint8_t)*s); s++) {
        if (whole > (UINT64_MAX - 9) / 10)
            return "the number is too large";
        whole = whole * 10 + (uint64_t)(*s - '0');
    }
    if (*s == '.' || *s == ',') {
        for (s++; isdigit((uint8_t)*s); s++)
            if (fd < 6)
                frac = frac * 10 + (uint64_t)(*s - '0'), fd++;
    }
    while (fd < 6)
        frac *= 10, fd++;
    while (*s == ' ')
        s++;
    uint64_t mul;
    char u = (char)toupper((uint8_t)*s);
    const char *after = *s ? s + 1 : s;
    switch (u) {
    case 0:
        mul = 1024 * 1024;
        break;
    case 'B':
        mul = 1;
        break;
    case 'K':
        mul = 1024;
        break;
    case 'M':
        mul = 1024 * 1024;
        break;
    case 'G':
        mul = 1024ull * 1024 * 1024;
        break;
    case 'T':
        mul = 1024ull * 1024 * 1024 * 1024;
        break;
    case 'S':
        if (!bsize)
            return "sizes in blocks are not possible here";
        mul = bsize;
        break;
    default:
        return "unknown unit: use K, M, G or T";
    }
    /* KB, KiB, MB, MiB... are the same binary units; B and S stand alone */
    if (u != 'B' && u != 'S' && u) {
        if (!strcasecmp(after, "IB") || !strcasecmp(after, "B"))
            after += strlen(after);
    }
    while (*after == ' ')
        after++;
    if (*after)
        return "unknown unit: use K, M, G or T";
    if (whole > UINT64_MAX / mul)
        return "the size is too large";
    uint64_t v = whole * mul, f = frac * mul / 1000000; /* frac < 10^6, mul <= 2^40: no overflow */
    if (v > UINT64_MAX - f)
        return "the size is too large";
    *bytes = v + f;
    return NULL;
}

void pm_fmt_size(char *out, size_t n, uint64_t bytes)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    int u = 0;
    uint64_t whole = bytes, tenth = 0;
    while (whole >= 1024 && u < 5) {
        tenth = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        u++;
    }
    if (u == 0)
        snprintf(out, n, "%llu B", (unsigned long long)bytes);
    else
        snprintf(out, n, "%llu.%llu %s", (unsigned long long)whole, (unsigned long long)tenth, units[u]);
}

void pm_fmt_exact(char *out, size_t n, uint64_t bytes)
{
    const uint64_t mib = 1024 * 1024, gib = 1024 * mib;
    if (bytes && bytes % gib == 0)
        snprintf(out, n, "%llu GiB", (unsigned long long)(bytes / gib));
    else if (bytes % mib == 0)
        snprintf(out, n, "%llu MiB", (unsigned long long)(bytes / mib));
    else
        pm_fmt_size(out, n, bytes);
}
