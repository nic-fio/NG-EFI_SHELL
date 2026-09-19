/* UEFI variables: the "var" command and the BASIC functions VAR$, VAREXISTS... */
#include "efi_cmds.h"
#include "../../basic/interp_int.h"

static const struct {
    const char *name;
    EFI_GUID guid;
} known_guids[] = {
    { "global", EFI_GLOBAL_VARIABLE_GUID },
    { "security", EFI_IMAGE_SECURITY_DATABASE_GUID },
    { "shim", { 0x605DAB50, 0xE046, 0x4300, { 0xAB, 0xB6, 0x3D, 0xD8, 0x10, 0xDD, 0x8B, 0x23 } } },
    { "shell", { 0x158DEF5A, 0xF656, 0x419C, { 0xB0, 0x27, 0x7A, 0x31, 0x92, 0xC0, 0x79, 0xD2 } } },
    { "systemd", { 0x4A67B082, 0x0A4C, 0x41CF, { 0xB6, 0xC7, 0x44, 0x0B, 0x29, 0xBB, 0x8C, 0x4F } } },
};

void guid_format(const EFI_GUID *g, char out[37])
{
    snprintf(out, 37, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g->Data1, g->Data2, g->Data3,
             g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6],
             g->Data4[7]);
}

const char *guid_name(const EFI_GUID *g)
{
    for (size_t i = 0; i < ARRAY_SIZE(known_guids); i++)
        if (!memcmp(g, &known_guids[i].guid, sizeof(EFI_GUID)))
            return known_guids[i].name;
    return NULL;
}

static int hexval(int c)
{
    return isdigit(c) ? c - '0' : isxdigit(c) ? (c | 32) - 'a' + 10 : -1;
}

bool guid_parse(const char *s, EFI_GUID *g)
{
    for (size_t i = 0; i < ARRAY_SIZE(known_guids); i++) {
        if (!strcasecmp(s, known_guids[i].name)) {
            *g = known_guids[i].guid;
            return true;
        }
    }
    /* xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    if (strlen(s) != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
        return false;
    uint8_t b[16];
    int k = 0;
    for (int i = 0; i < 36; i++) {
        if (s[i] == '-')
            continue;
        int h = hexval((uint8_t)s[i]), l = hexval((uint8_t)s[i + 1]);
        if (h < 0 || l < 0)
            return false;
        b[k++] = (uint8_t)(h << 4 | l);
        i++;
    }
    g->Data1 = (UINT32)b[0] << 24 | (UINT32)b[1] << 16 | (UINT32)b[2] << 8 | b[3];
    g->Data2 = (UINT16)(b[4] << 8 | b[5]);
    g->Data3 = (UINT16)(b[6] << 8 | b[7]);
    memcpy(g->Data4, b + 8, 8);
    return true;
}

void *efi_var_read(const char *name, const EFI_GUID *g, size_t *size, UINT32 *attr, EFI_STATUS *st)
{
    uint16_t *wn = utf8_to_ucs2(name, NULL);
    UINTN sz = 0;
    UINT32 a = 0;
    EFI_STATUS s = gRT->GetVariable(wn, (EFI_GUID *)g, &a, &sz, NULL);
    void *data = NULL;
    if (s == EFI_BUFFER_TOO_SMALL) {
        data = xmalloc(sz + 2);
        s = gRT->GetVariable(wn, (EFI_GUID *)g, &a, &sz, data);
        if (EFI_ERROR(s)) {
            free(data);
            data = NULL;
        } else {
            ((uint8_t *)data)[sz] = 0; /* convenience terminators */
            ((uint8_t *)data)[sz + 1] = 0;
        }
    } else if (s == EFI_SUCCESS) {
        data = xmalloc(2); /* zero-length variable */
        memset(data, 0, 2);
    }
    free(wn);
    if (st)
        *st = s;
    if (data) {
        *size = sz;
        if (attr)
            *attr = a;
    }
    return data;
}

EFI_STATUS efi_var_write(const char *name, const EFI_GUID *g, UINT32 attr, const void *data, size_t size)
{
    uint16_t *wn = utf8_to_ucs2(name, NULL);
    EFI_STATUS s = gRT->SetVariable(wn, (EFI_GUID *)g, attr, size, (void *)data);
    free(wn);
    return s;
}

static void attr_string(UINT32 a, char out[8])
{
    snprintf(out, 8, "%s%s%s%s", a & EFI_VARIABLE_NON_VOLATILE ? "NV" : "  ",
             a & EFI_VARIABLE_BOOTSERVICE_ACCESS ? "BS" : "  ", a & EFI_VARIABLE_RUNTIME_ACCESS ? "RT" : "  ",
             a & (EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS | EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS) ? "A" : "");
}

static bool parse_attr(const char *s, UINT32 *out)
{
    int64_t v;
    if (parse_int(s, &v)) {
        *out = (UINT32)v;
        return true;
    }
    UINT32 a = 0;
    char *copy = xstrdup(s);
    bool ok = true;
    for (char *t = copy; t && *t;) {
        char *c = strchr(t, ',');
        if (c)
            *c = 0;
        if (!strcasecmp(t, "nv"))
            a |= EFI_VARIABLE_NON_VOLATILE;
        else if (!strcasecmp(t, "bs"))
            a |= EFI_VARIABLE_BOOTSERVICE_ACCESS;
        else if (!strcasecmp(t, "rt"))
            a |= EFI_VARIABLE_RUNTIME_ACCESS;
        else
            ok = false;
        t = c ? c + 1 : NULL;
    }
    free(copy);
    *out = a;
    return ok && a;
}

/* ---- Value decoding ---- */

static bool looks_ucs2(const uint8_t *d, size_t n)
{
    if (n < 2 || n % 2)
        return false;
    size_t units = n / 2;
    for (size_t i = 0; i < units; i++) {
        uint16_t c = (uint16_t)(d[2 * i] | d[2 * i + 1] << 8);
        if (c == 0)
            return i == units - 1 && i > 0;
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
            return false;
        if (c >= 0xD800 && c <= 0xDFFF)
            return false;
    }
    return true;
}

static bool looks_ascii(const uint8_t *d, size_t n)
{
    if (!n)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (d[i] == 0)
            return i == n - 1 && i > 0;
        if (d[i] < 0x20 || d[i] >= 0x7f)
            return false;
    }
    return n > 2;
}

static char *decode_string(const uint8_t *d, size_t n)
{
    if (looks_ucs2(d, n)) {
        uint16_t *u = xmalloc(n + 2);
        memcpy(u, d, n);
        u[n / 2] = 0;
        char *r = ucs2_to_utf8(u, (size_t)-1);
        free(u);
        return r;
    }
    return xstrndup((const char *)d, strnlen((const char *)d, n));
}

static uint64_t le_value(const uint8_t *d, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n && i < 8; i++)
        v |= (uint64_t)d[i] << (8 * i);
    return v;
}

static void hexdump_mem(const uint8_t *d, size_t n)
{
    for (size_t off = 0; off < n; off += 16) {
        out_printf("  %04zx  ", off);
        for (size_t k = 0; k < 16; k++) {
            if (off + k < n)
                out_printf("%02x ", d[off + k]);
            else
                out_puts("   ");
        }
        out_puts(" |");
        for (size_t k = 0; k < 16 && off + k < n; k++)
            out_printf("%c", d[off + k] >= 0x20 && d[off + k] < 0x7f ? d[off + k] : '.');
        out_puts("|\n");
    }
}

static char *to_hex(const uint8_t *d, size_t n)
{
    char *r = xmalloc(n * 2 + 1);
    for (size_t i = 0; i < n; i++)
        snprintf(r + 2 * i, 3, "%02x", d[i]);
    r[n * 2] = 0;
    return r;
}

static uint8_t *from_hex(const char *s, size_t *n)
{
    size_t l = strlen(s);
    uint8_t *r = xmalloc(l / 2 + 1);
    size_t k = 0;
    for (size_t i = 0; i < l;) {
        if (s[i] == ' ' || s[i] == ':' || s[i] == '-') {
            i++;
            continue;
        }
        int h = hexval((uint8_t)s[i]), lo = i + 1 < l ? hexval((uint8_t)s[i + 1]) : -1;
        if (h < 0 || lo < 0) {
            free(r);
            return NULL;
        }
        r[k++] = (uint8_t)(h << 4 | lo);
        i += 2;
    }
    *n = k;
    return r;
}

/* ---- Enumeration ---- */

typedef bool (*VarCb)(const char *name, const EFI_GUID *g, void *ctx);

static void var_foreach(VarCb cb, void *ctx)
{
    UINTN cap = 512;
    CHAR16 *name = xcalloc(1, cap);
    EFI_GUID g;
    memset(&g, 0, sizeof(g));
    for (;;) {
        UINTN sz = cap;
        EFI_STATUS st = gRT->GetNextVariableName(&sz, name, &g);
        if (st == EFI_BUFFER_TOO_SMALL) {
            name = xrealloc(name, sz);
            cap = sz;
            continue;
        }
        if (EFI_ERROR(st))
            break;
        char *n = ucs2_to_utf8(name, (size_t)-1);
        bool go = cb(n, &g, ctx);
        free(n);
        if (!go)
            break;
    }
    free(name);
}

typedef struct {
    bool filter_guid;
    EFI_GUID guid;
    const char *pattern;
    int count;
    Sbuf *save;
} ListCtx;

/* Backup file format: one variable per line, "GUID ATTR NAME HEXDATA". */
void var_backup_line(Sbuf *b, const char *name, const EFI_GUID *g, UINT32 attr, const uint8_t *d, size_t n)
{
    char gs[37];
    guid_format(g, gs);
    char *hex = to_hex(d, n);
    sb_printf(b, "%s %08x %s %s\n", gs, attr, name, hex);
    free(hex);
}

static void attr_plus(UINT32 a, char *out, size_t n);

static bool list_cb(const char *name, const EFI_GUID *g, void *ctx)
{
    ListCtx *c = ctx;
    if (c->filter_guid && memcmp(g, &c->guid, sizeof(EFI_GUID)))
        return true;
    if (c->pattern && !glob_match(c->pattern, name, true))
        return true;
    size_t size = 0;
    UINT32 attr = 0;
    uint8_t *d = efi_var_read(name, g, &size, &attr, NULL);
    if (c->save) {
        if (d)
            var_backup_line(c->save, name, g, attr, d, size);
    } else if (out_data_mode()) {
        char gs[37], as[32];
        guid_format(g, gs);
        const char *gn = guid_name(g);
        attr_plus(d ? attr : 0, as, sizeof(as));
        data_record();
        data_field("name", "%s", name);
        data_field("guid", "%s", gs);
        data_field("guid_name", "%s", gn ? gn : "");
        data_field("attributes", "%s", as);
        data_field("size", "%zu", d ? size : (size_t)0);
    } else {
        char gs[37], as[8];
        guid_format(g, gs);
        const char *gn = guid_name(g);
        attr_string(d ? attr : 0, as);
        out_printf("%-36s %-7s %6zu  %s\n", gn ? gn : gs, as, d ? size : (size_t)0, name);
    }
    free(d);
    c->count++;
    return !con_break();
}

/* ---- var command ---- */

typedef struct {
    const char *guid, *attr, *type;
    bool x, s, n, v, y;
    char *pos[8];
    int npos;
} VarArgs;

static bool var_args(int argc, char **argv, int first, VarArgs *va)
{
    memset(va, 0, sizeof(*va));
    for (int i = first; i < argc; i++) {
        const char *a = argv[i];
        if ((!strcmp(a, "-g") || !strcmp(a, "-a") || !strcmp(a, "-t")) && i + 1 < argc) {
            if (a[1] == 'g')
                va->guid = argv[++i];
            else if (a[1] == 'a')
                va->attr = argv[++i];
            else
                va->type = argv[++i];
        } else if (a[0] == '-' && a[1] && !a[2] && strchr("xsnvy", a[1])) {
            switch (a[1]) {
            case 'x': va->x = true; break;
            case 's': va->s = true; break;
            case 'n': va->n = true; break;
            case 'v': va->v = true; break;
            case 'y': va->y = true; break;
            }
        } else if (a[0] == '-' && a[1] && !isdigit((uint8_t)a[1])) {
            err_printf("var: unknown option %s\n", a);
            return false;
        } else if (va->npos < 8) {
            va->pos[va->npos++] = argv[i];
        }
    }
    return true;
}

static bool get_guid(const VarArgs *va, EFI_GUID *g)
{
    if (!va->guid) {
        *g = gEfiGlobalVariableGuid;
        return true;
    }
    if (!guid_parse(va->guid, g)) {
        err_printf("var: invalid GUID %s (use xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx or a name like global)\n", va->guid);
        return false;
    }
    return true;
}

static int var_get(VarArgs *va)
{
    if (va->npos != 1)
        return cmd_usage("var");
    EFI_GUID g;
    if (!get_guid(va, &g))
        return RC_USAGE;
    size_t n;
    UINT32 attr;
    EFI_STATUS st;
    uint8_t *d = efi_var_read(va->pos[0], &g, &n, &attr, &st);
    if (!d)
        return cmd_err("var", "%s: %s", va->pos[0], efi_strerror(st));
    char as[8];
    attr_string(attr, as);
    if (va->v)
        out_printf("%s  attributes %s (0x%x), %zu bytes\n", va->pos[0], as, attr, n);
    bool is_order = !strcasecmp(va->pos[0], "BootOrder") || !strcasecmp(va->pos[0], "DriverOrder");
    if (va->x || (!va->s && !va->n && !is_order && !looks_ucs2(d, n) && !looks_ascii(d, n) && n != 1 && n != 2 &&
                  n != 4 && n != 8)) {
        hexdump_mem(d, n);
    } else if (va->s || (!va->n && !is_order && (looks_ucs2(d, n) || looks_ascii(d, n)))) {
        char *s = decode_string(d, n);
        out_printf("%s\n", s);
        free(s);
    } else if (is_order && n % 2 == 0) {
        for (size_t i = 0; i < n; i += 2)
            out_printf("%s%04X", i ? "," : "", (unsigned)(d[i] | d[i + 1] << 8));
        out_puts("\n");
    } else {
        uint64_t v = le_value(d, n);
        out_printf("%llu (0x%llx)\n", (unsigned long long)v, (unsigned long long)v);
    }
    free(d);
    return RC_OK;
}

static int var_set(VarArgs *va)
{
    if (va->npos != 2)
        return cmd_usage("var");
    EFI_GUID g;
    if (!get_guid(va, &g))
        return RC_USAGE;
    const char *name = va->pos[0], *value = va->pos[1];
    const char *type = va->type ? va->type : "str";
    uint8_t *data = NULL;
    size_t n = 0;
    int64_t num = 0;
    if (!strcasecmp(type, "str")) {
        uint16_t *u = utf8_to_ucs2(value, &n);
        n = (n + 1) * 2;
        data = (uint8_t *)u;
    } else if (!strcasecmp(type, "ascii")) {
        n = strlen(value) + 1;
        data = (uint8_t *)xstrdup(value);
    } else if (!strcasecmp(type, "u8") || !strcasecmp(type, "u16") || !strcasecmp(type, "u32") ||
               !strcasecmp(type, "u64")) {
        if (!parse_int(value, &num))
            return cmd_err("var", "%s is not a number", value);
        n = type[1] == '8' ? 1 : type[1] == '1' ? 2 : type[1] == '3' ? 4 : 8;
        data = xmalloc(8);
        for (size_t i = 0; i < 8; i++)
            data[i] = (uint8_t)((uint64_t)num >> (8 * i));
    } else if (!strcasecmp(type, "hex")) {
        data = from_hex(value, &n);
        if (!data)
            return cmd_err("var", "invalid hex data");
    } else if (!strcasecmp(type, "file")) {
        char *p = path_resolve(value);
        char *fd;
        int e = p ? file_read_all(p, &fd, &n) : PAL_ENOENT;
        free(p);
        if (e)
            return cmd_perr("var", value, e);
        data = (uint8_t *)fd;
    } else {
        return cmd_err("var", "unknown type %s (str, ascii, u8, u16, u32, u64, hex, file)", type);
    }
    UINT32 attr;
    if (va->attr) {
        if (!parse_attr(va->attr, &attr)) {
            free(data);
            return cmd_err("var", "invalid attributes %s (e.g. nv,bs,rt or 0x7)", va->attr);
        }
    } else {
        size_t on;
        uint8_t *old = efi_var_read(name, &g, &on, &attr, NULL);
        if (old)
            free(old); /* keep the attributes of an existing variable */
        else
            attr = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
    }
    EFI_STATUS st = efi_var_write(name, &g, attr, data, n);
    free(data);
    if (EFI_ERROR(st))
        return cmd_err("var", "cannot write %s: %s", name, efi_strerror(st));
    return RC_OK;
}

static int var_del(VarArgs *va)
{
    if (va->npos != 1)
        return cmd_usage("var");
    EFI_GUID g;
    if (!get_guid(va, &g))
        return RC_USAGE;
    EFI_STATUS st = efi_var_write(va->pos[0], &g, 0, NULL, 0);
    if (EFI_ERROR(st))
        return cmd_err("var", "cannot delete %s: %s", va->pos[0], efi_strerror(st));
    return RC_OK;
}

/* Restores variables from a backup file. Returns the number of failures. */
int var_restore_text(const char *text, bool verbose)
{
    int fails = 0;
    char *copy = xstrdup(text);
    for (char *line = copy; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *gs = line, *as = NULL, *name = NULL, *hex = NULL;
        as = strchr(gs, ' ');
        if (as) {
            *as++ = 0;
            name = strchr(as, ' ');
        }
        if (name) {
            *name++ = 0;
            hex = strchr(name, ' ');
        }
        if (hex)
            *hex++ = 0;
        EFI_GUID g;
        size_t n = 0;
        uint8_t *d = hex ? from_hex(hex, &n) : NULL;
        char *end = NULL;
        unsigned long long attr = as ? strtoull(as, &end, 16) : 0;
        if (*gs && *gs != '#') {
            if (!d || !guid_parse(gs, &g) || !end || *end || !*name) {
                err_printf("var: malformed line: %s\n", gs);
                fails++;
            } else {
                EFI_STATUS st = efi_var_write(name, &g, (UINT32)attr, d, n);
                if (EFI_ERROR(st)) {
                    err_printf("var: cannot write %s: %s\n", name, efi_strerror(st));
                    fails++;
                } else if (verbose) {
                    out_printf("restored %s\n", name);
                }
            }
        }
        free(d);
        line = nl ? nl + 1 : NULL;
    }
    free(copy);
    return fails;
}

static int cmd_var(int argc, char **argv)
{
    if (argc < 2)
        return cmd_usage("var");
    VarArgs va;
    if (!var_args(argc, argv, 2, &va))
        return RC_USAGE;
    const char *sub = argv[1];
    if (!strcasecmp(sub, "list") || !strcasecmp(sub, "ls")) {
        ListCtx c = { 0 };
        if (va.guid) {
            if (!get_guid(&va, &c.guid))
                return RC_USAGE;
            c.filter_guid = true;
        }
        c.pattern = va.npos ? va.pos[0] : NULL;
        var_foreach(list_cb, &c);
        if (!c.count && c.pattern)
            return cmd_err("var", "no variable matches %s", c.pattern);
        return RC_OK;
    }
    if (!strcasecmp(sub, "get"))
        return var_get(&va);
    if (!strcasecmp(sub, "set"))
        return var_set(&va);
    if (!strcasecmp(sub, "del") || !strcasecmp(sub, "rm"))
        return var_del(&va);
    if (!strcasecmp(sub, "save")) {
        if (va.npos < 1 || va.npos > 2)
            return cmd_usage("var");
        char *p = path_resolve(va.pos[0]);
        if (!p)
            return cmd_err("var", "%s: invalid path", va.pos[0]);
        Sbuf b;
        sb_init(&b);
        sb_adds(&b, "# NESH variable backup: GUID ATTRIBUTES NAME DATA\n");
        ListCtx c = { 0 };
        c.save = &b;
        c.pattern = va.npos > 1 ? va.pos[1] : NULL;
        if (va.guid) {
            if (!get_guid(&va, &c.guid)) {
                free(p);
                sb_free(&b);
                return RC_USAGE;
            }
            c.filter_guid = true;
        }
        var_foreach(list_cb, &c);
        int e = file_write_all(p, b.s, b.len, false);
        sb_free(&b);
        if (e) {
            int rc = cmd_perr("var", p, e);
            free(p);
            return rc;
        }
        out_printf("%d variable%s saved to %s\n", c.count, c.count == 1 ? "" : "s", p);
        free(p);
        return RC_OK;
    }
    if (!strcasecmp(sub, "load")) {
        if (va.npos != 1)
            return cmd_usage("var");
        char *p = path_resolve(va.pos[0]);
        char *text;
        size_t len;
        int e = p ? file_read_text(p, &text, &len) : PAL_ENOENT;
        free(p);
        if (e)
            return cmd_perr("var", va.pos[0], e);
        int fails = var_restore_text(text, true);
        free(text);
        return fails ? RC_FAIL : RC_OK;
    }
    return cmd_usage("var");
}

/* ---- dmpstore and setvar (UEFI Shell syntax) ---- */

static void attr_plus(UINT32 a, char *out, size_t n)
{
    snprintf(out, n, "%s%s%s%s%s%s", a & EFI_VARIABLE_NON_VOLATILE ? "NV+" : "",
             a & EFI_VARIABLE_BOOTSERVICE_ACCESS ? "BS+" : "", a & EFI_VARIABLE_RUNTIME_ACCESS ? "RT+" : "",
             a & EFI_VARIABLE_HARDWARE_ERROR_RECORD ? "HR+" : "",
             a & EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS ? "AW+" : "",
             a & EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS ? "AT+" : "");
    size_t l = strlen(out);
    if (l)
        out[l - 1] = 0;
}

static void dump_variable(const char *name, const EFI_GUID *g, UINT32 attr, const uint8_t *d, size_t n)
{
    char gs[37], as[32];
    guid_format(g, gs);
    for (char *c = gs; *c; c++)
        *c = (char)toupper((uint8_t)*c);
    attr_plus(attr, as, sizeof(as));
    out_printf("Variable %s '%s:%s' DataSize = 0x%zX\n", as, gs, name, n);
    hexdump_mem(d, n);
}

typedef struct {
    const char *pattern;
    bool all, has_guid;
    EFI_GUID guid;
    int mode; /* 0 dump, 1 collect for delete, 2 save */
    Sbuf *out;
    char **names;
    EFI_GUID *guids;
    int count;
} DsCtx;

static bool ds_match(DsCtx *c, const char *name, const EFI_GUID *g)
{
    if (!c->all) {
        const EFI_GUID *want = c->has_guid ? &c->guid : &gEfiGlobalVariableGuid;
        if (memcmp(g, want, sizeof(EFI_GUID)))
            return false;
    }
    return !c->pattern || glob_match(c->pattern, name, true);
}

/* dmpstore file record: NameSize, DataSize, Name (UCS-2 with NUL), GUID, Attributes, Data, CRC32 */
static void ds_record(Sbuf *b, const char *name, const EFI_GUID *g, UINT32 attr, const uint8_t *d, size_t n)
{
    size_t units;
    uint16_t *wn = utf8_to_ucs2(name, &units);
    UINT32 ns = (UINT32)((units + 1) * 2), ds = (UINT32)n;
    size_t start = b->len;
    sb_add(b, (const char *)&ns, 4);
    sb_add(b, (const char *)&ds, 4);
    sb_add(b, (const char *)wn, ns);
    sb_add(b, (const char *)g, sizeof(EFI_GUID));
    sb_add(b, (const char *)&attr, 4);
    sb_add(b, (const char *)d, n);
    UINT32 crc = 0;
    gBS->CalculateCrc32(b->s + start, b->len - start, &crc);
    sb_add(b, (const char *)&crc, 4);
    free(wn);
}

static bool ds_cb(const char *name, const EFI_GUID *g, void *ctx)
{
    DsCtx *c = ctx;
    if (!ds_match(c, name, g))
        return true;
    if (c->mode == 1) {
        c->names = xrealloc(c->names, sizeof(char *) * (c->count + 1));
        c->guids = xrealloc(c->guids, sizeof(EFI_GUID) * (c->count + 1));
        c->names[c->count] = xstrdup(name);
        c->guids[c->count] = *g;
        c->count++;
        return true;
    }
    size_t n;
    UINT32 attr;
    uint8_t *d = efi_var_read(name, g, &n, &attr, NULL);
    if (d) {
        if (c->mode == 0)
            dump_variable(name, g, attr, d, n);
        else
            ds_record(c->out, name, g, attr, d, n);
        c->count++;
    }
    free(d);
    return !con_break();
}

static int cmd_dmpstore(int argc, char **argv)
{
    DsCtx c = { 0 };
    bool del = false;
    const char *save = NULL, *load = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcasecmp(a, "-all"))
            c.all = true;
        else if (!strcasecmp(a, "-d"))
            del = true;
        else if (!strcasecmp(a, "-b"))
            ; /* page break: accepted */
        else if (!strcasecmp(a, "-guid") && i + 1 < argc) {
            if (!guid_parse(argv[++i], &c.guid))
                return cmd_err("dmpstore", "invalid GUID %s", argv[i]);
            c.has_guid = true;
        } else if (!strcasecmp(a, "-s") && i + 1 < argc)
            save = argv[++i];
        else if (!strcasecmp(a, "-l") && i + 1 < argc)
            load = argv[++i];
        else if (a[0] == '-')
            return cmd_usage("dmpstore");
        else if (!c.pattern)
            c.pattern = a;
        else
            return cmd_usage("dmpstore");
    }
    if (del + (save != NULL) + (load != NULL) > 1)
        return cmd_err("dmpstore", "-d, -s and -l cannot be combined");

    if (load) {
        char *p = path_resolve(load);
        char *data;
        size_t len;
        int e = p ? file_read_all(p, &data, &len) : PAL_ENOENT;
        free(p);
        if (e)
            return cmd_perr("dmpstore", load, e);
        int loaded = 0, fails = 0; /* like the UEFI Shell: global GUID only unless -guid or -all */
        size_t off = 0;
        while (off + 8 <= len) {
            UINT32 ns, ds;
            memcpy(&ns, data + off, 4);
            memcpy(&ds, data + off + 4, 4);
            size_t rec = 8 + (size_t)ns + sizeof(EFI_GUID) + 4 + ds + 4;
            if (ns < 2 || ns % 2 || off + rec > len) {
                fails++;
                err_printf("dmpstore: %s: truncated or damaged file\n", load);
                break;
            }
            UINT32 crc = 0, stored;
            gBS->CalculateCrc32(data + off, rec - 4, &crc);
            memcpy(&stored, data + off + rec - 4, 4);
            uint16_t *wn = xmalloc(ns + 2);
            memcpy(wn, data + off + 8, ns);
            wn[ns / 2] = 0;
            char *name = ucs2_to_utf8(wn, (size_t)-1);
            free(wn);
            EFI_GUID g;
            UINT32 attr;
            memcpy(&g, data + off + 8 + ns, sizeof(g));
            memcpy(&attr, data + off + 8 + ns + sizeof(g), 4);
            if (crc != stored) {
                err_printf("dmpstore: %s: CRC error, not loaded\n", name);
                fails++;
            } else if (ds_match(&c, name, &g)) {
                EFI_STATUS st = efi_var_write(name, &g, attr, data + off + 8 + ns + sizeof(g) + 4, ds);
                char gs[37];
                guid_format(&g, gs);
                out_printf("Load variable %s:%s - %s\n", gs, name, EFI_ERROR(st) ? efi_strerror(st) : "Success");
                if (EFI_ERROR(st))
                    fails++;
                else
                    loaded++;
            }
            free(name);
            off += rec;
        }
        free(data);
        if (!loaded && !fails)
            out_puts("No variable in the file matches (without -guid only the global GUID is loaded; use -all).\n");
        return fails ? RC_FAIL : RC_OK;
    }

    if (del) {
        if (!c.pattern && !c.all && !c.has_guid)
            return cmd_err("dmpstore", "-d needs a variable name, a GUID or -all");
        c.mode = 1;
        var_foreach(ds_cb, &c);
        if (!c.count) {
            out_puts("No matching variable.\n");
            return RC_OK;
        }
        if (!c.pattern || path_has_wildcards(c.pattern)) {
            char *q = xasprintf("Delete %d variables? [y/N] ", c.count);
            char *ans = lineedit_read(q, false);
            bool yes = ans && (!strcasecmp(ans, "y") || !strcasecmp(ans, "yes"));
            free(ans);
            free(q);
            if (!yes) {
                for (int i = 0; i < c.count; i++)
                    free(c.names[i]);
                free(c.names);
                free(c.guids);
                out_puts("Cancelled.\n");
                return RC_FAIL;
            }
        }
        int fails = 0;
        for (int i = 0; i < c.count; i++) {
            EFI_STATUS st = efi_var_write(c.names[i], &c.guids[i], 0, NULL, 0);
            char gs[37];
            guid_format(&c.guids[i], gs);
            out_printf("Delete variable %s:%s - %s\n", gs, c.names[i], EFI_ERROR(st) ? efi_strerror(st) : "Success");
            fails += EFI_ERROR(st) != 0;
            free(c.names[i]);
        }
        free(c.names);
        free(c.guids);
        return fails ? RC_FAIL : RC_OK;
    }

    if (save) {
        char *p = path_resolve(save);
        if (!p)
            return cmd_err("dmpstore", "%s: invalid path", save);
        Sbuf b;
        sb_init(&b);
        c.mode = 2;
        c.out = &b;
        var_foreach(ds_cb, &c);
        int e = file_write_all(p, b.s ? b.s : "", b.len, false);
        sb_free(&b);
        if (e) {
            int rc = cmd_perr("dmpstore", p, e);
            free(p);
            return rc;
        }
        out_printf("%d variables saved to %s\n", c.count, p);
        free(p);
        return RC_OK;
    }

    var_foreach(ds_cb, &c);
    if (!c.count && c.pattern)
        return cmd_err("dmpstore", "variable %s not found", c.pattern);
    return RC_OK;
}

/* setvar data items: "ascii" (no terminator), L"unicode" (with terminator),
 * --DevicePathText, or hexadecimal bytes. */
static bool setvar_item(const char *s, Sbuf *out)
{
    size_t l = strlen(s);
    if (l >= 2 && s[0] == '"' && s[l - 1] == '"') {
        sb_add(out, s + 1, l - 2);
        return true;
    }
    if (l >= 3 && (s[0] == 'L' || s[0] == 'l') && s[1] == '"' && s[l - 1] == '"') {
        char *inner = xstrndup(s + 2, l - 3);
        size_t units;
        uint16_t *u = utf8_to_ucs2(inner, &units);
        sb_add(out, (const char *)u, (units + 1) * 2);
        free(u);
        free(inner);
        return true;
    }
    if (l > 2 && s[0] == '-' && s[1] == '-') {
        EFI_GUID ft = EFI_DEVICE_PATH_FROM_TEXT_PROTOCOL_GUID;
        EFI_DEVICE_PATH_FROM_TEXT_PROTOCOL *f;
        if (gBS->LocateProtocol(&ft, NULL, (void **)&f) != EFI_SUCCESS)
            return false;
        uint16_t *u = utf8_to_ucs2(s + 2, NULL);
        EFI_DEVICE_PATH_PROTOCOL *dp = f->ConvertTextToDevicePath(u);
        free(u);
        if (!dp)
            return false;
        sb_add(out, (const char *)dp, efi_devpath_size(dp));
        gBS->FreePool(dp);
        return true;
    }
    size_t n;
    const char *h = (l > 2 && s[0] == '0' && (s[1] | 32) == 'x') ? s + 2 : s;
    if (strlen(h) % 2)
        return false;
    uint8_t *b = from_hex(h, &n);
    if (!b)
        return false;
    sb_add(out, (const char *)b, n);
    free(b);
    return true;
}

static int cmd_setvar(int argc, char **argv)
{
    const char *name = NULL;
    EFI_GUID g = gEfiGlobalVariableGuid;
    UINT32 attr = 0;
    bool has_data = false;
    Sbuf data;
    sb_init(&data);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (has_data) {
            if (!setvar_item(a, &data))
                goto bad_data;
            continue;
        }
        if (!strcasecmp(a, "-guid") && i + 1 < argc) {
            if (!guid_parse(argv[++i], &g)) {
                sb_free(&data);
                return cmd_err("setvar", "invalid GUID %s", argv[i]);
            }
        } else if (!strcasecmp(a, "-bs")) {
            attr |= EFI_VARIABLE_BOOTSERVICE_ACCESS;
        } else if (!strcasecmp(a, "-rt")) {
            attr |= EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_BOOTSERVICE_ACCESS;
        } else if (!strcasecmp(a, "-nv")) {
            attr |= EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS;
        } else if (a[0] == '=') {
            has_data = true;
            if (a[1] && !setvar_item(a + 1, &data))
                goto bad_data;
        } else if (!name) {
            name = a;
        } else {
            sb_free(&data);
            return cmd_usage("setvar");
        }
    }
    if (!name) {
        sb_free(&data);
        return cmd_usage("setvar");
    }
    if (!has_data) {
        size_t n;
        UINT32 a;
        EFI_STATUS st;
        uint8_t *d = efi_var_read(name, &g, &n, &a, &st);
        if (!d)
            return cmd_err("setvar", "%s: %s", name, efi_strerror(st));
        dump_variable(name, &g, a, d, n);
        free(d);
        return RC_OK;
    }
    if (!attr) {
        size_t on;
        uint8_t *old = efi_var_read(name, &g, &on, &attr, NULL);
        free(old);
        if (!old)
            attr = EFI_VARIABLE_BOOTSERVICE_ACCESS; /* new variable, no attribute given: temporary */
    }
    EFI_STATUS st = efi_var_write(name, &g, data.len ? attr : 0, data.s, data.len);
    sb_free(&data);
    if (EFI_ERROR(st))
        return cmd_err("setvar", "%s: %s", name, efi_strerror(st));
    return RC_OK;
bad_data:
    sb_free(&data);
    return cmd_err("setvar", "invalid data: use hex bytes (0102), \"ascii\", L\"unicode\" or --DevicePath");
}

static const Cmd edk_var_cmds[] = {
    { "dmpstore", cmd_dmpstore, "dmpstore [-b] [-d] [-all | [NAME] [-guid GUID]] [-s FILE | -l FILE]",
      "Dump, delete, save or load UEFI variables (UEFI Shell syntax)",
      "  dmpstore                  all variables with the global GUID\n"
      "  dmpstore -all             variables of every GUID\n"
      "  dmpstore Boot*            variables matching a pattern\n"
      "  dmpstore NAME -d          delete (asks when a pattern matches several)\n"
      "  dmpstore -all -s FILE     save to a file (binary format of the UEFI Shell)\n"
      "  dmpstore -l FILE          load variables from such a file\n"
      "See also: var (NESH syntax).\n" },
    { "setvar", cmd_setvar, "setvar NAME [-guid GUID] [-bs] [-rt] [-nv] [=DATA...]",
      "Show, set or delete a UEFI variable (UEFI Shell syntax)",
      "  setvar NAME                 show the value\n"
      "  setvar NAME -nv -rt =0100   set hexadecimal bytes\n"
      "  setvar NAME =\"text\"         ASCII text (no terminator)\n"
      "  setvar NAME =L\"text\"        UCS-2 text (with terminator)\n"
      "  setvar NAME =--PciRoot(0)   binary device path from its text form\n"
      "  setvar NAME =               delete the variable\n"
      "Without -bs/-rt/-nv an existing variable keeps its attributes; a new one\n"
      "is temporary (boot services only).\n",
      CMD_KEEP_QUOTES },
};

/* ---- Environment variables of the shell (shared with the EDK2 UEFI Shell) ---- */

static EFI_GUID shell_var_guid = { 0x158DEF5A, 0xF656, 0x419C, { 0xB0, 0x27, 0x7A, 0x31, 0x92, 0xC0, 0x79, 0xD2 } };

typedef struct {
    void (*cb)(const char *name, const char *value);
} EnvLoad;

static bool env_load_cb(const char *name, const EFI_GUID *g, void *ctx)
{
    if (memcmp(g, &shell_var_guid, sizeof(EFI_GUID)))
        return true;
    size_t n;
    UINT32 attr;
    uint8_t *d = efi_var_read(name, g, &n, &attr, NULL);
    if (d && (attr & EFI_VARIABLE_NON_VOLATILE) && looks_ucs2(d, n)) {
        char *v = decode_string(d, n);
        ((EnvLoad *)ctx)->cb(name, v);
        free(v);
    }
    free(d);
    return true;
}

void platform_env_load(void (*cb)(const char *name, const char *value))
{
    EnvLoad ctx = { cb };
    var_foreach(env_load_cb, &ctx);
}

bool platform_env_store(const char *name, const char *value)
{
    if (!value)
        return !EFI_ERROR(efi_var_write(name, &shell_var_guid, 0, NULL, 0));
    size_t units;
    uint16_t *u = utf8_to_ucs2(value, &units);
    EFI_STATUS st = efi_var_write(name, &shell_var_guid, EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
                                  u, (units + 1) * 2);
    free(u);
    return !EFI_ERROR(st);
}

/* Aliases: same storage as the UEFI Shell (name = alias, data = command). */
static EFI_GUID shell_alias_guid = { 0x0053D9D6, 0x2659, 0x4599, { 0xA2, 0x6B, 0xEF, 0x45, 0x36, 0xE6, 0x31, 0xA9 } };

static bool alias_load_cb(const char *name, const EFI_GUID *g, void *ctx)
{
    if (memcmp(g, &shell_alias_guid, sizeof(EFI_GUID)))
        return true;
    size_t n;
    uint8_t *d = efi_var_read(name, g, &n, NULL, NULL);
    if (d && looks_ucs2(d, n)) {
        char *v = decode_string(d, n);
        ((EnvLoad *)ctx)->cb(name, v);
        free(v);
    }
    free(d);
    return true;
}

void platform_alias_load(void (*cb)(const char *name, const char *value))
{
    EnvLoad ctx = { cb };
    var_foreach(alias_load_cb, &ctx);
}

bool platform_alias_store(const char *name, const char *value)
{
    if (!value)
        return !EFI_ERROR(efi_var_write(name, &shell_alias_guid, 0, NULL, 0));
    size_t units;
    uint16_t *u = utf8_to_ucs2(value, &units);
    EFI_STATUS st = efi_var_write(name, &shell_alias_guid, EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
                                  u, (units + 1) * 2);
    free(u);
    return !EFI_ERROR(st);
}

bool platform_break_pending(void)
{
    return efi_break_pending();
}

const char *platform_uefi_version(void)
{
    static char buf[16];
    UINT32 major = gST->Hdr.Revision >> 16, minor = gST->Hdr.Revision & 0xFFFF;
    if (minor % 10)
        snprintf(buf, sizeof(buf), "%u.%u.%u", major, minor / 10, minor % 10);
    else
        snprintf(buf, sizeof(buf), "%u.%u", major, minor / 10);
    return buf;
}

/* ---- BASIC functions ---- */

#define FN(name) static bool name(Interp *in, Node *call, Value *a, int n, Value *out)

static bool arg_guid(Interp *in, Node *call, Value *a, int n, int idx, EFI_GUID *g)
{
    if (n <= idx) {
        *g = gEfiGlobalVariableGuid;
        return true;
    }
    if (!guid_parse(a[idx].s->s, g))
        return rt_err(in, call, "invalid GUID: %s", a[idx].s->s);
    return true;
}

static uint8_t *basic_read(Interp *in, Node *call, Value *a, int n, size_t *size, bool *ok)
{
    EFI_GUID g;
    *ok = arg_guid(in, call, a, n, 1, &g);
    if (!*ok)
        return NULL;
    EFI_STATUS st;
    uint8_t *d = efi_var_read(a[0].s->s, &g, size, NULL, &st);
    in->err = d ? RC_OK : (int64_t)(st & 0xFF);
    return d;
}

FN(f_varexists)
{
    size_t sz;
    bool ok;
    uint8_t *d = basic_read(in, call, a, n, &sz, &ok);
    *out = v_int(d ? -1 : 0);
    free(d);
    return ok;
}

FN(f_var)
{
    size_t sz;
    bool ok;
    uint8_t *d = basic_read(in, call, a, n, &sz, &ok);
    if (!ok)
        return false;
    if (d) {
        char *s = decode_string(d, sz);
        *out = v_cstr(s);
        free(s);
    } else {
        *out = v_cstr("");
    }
    free(d);
    return true;
}

FN(f_varhex)
{
    size_t sz;
    bool ok;
    uint8_t *d = basic_read(in, call, a, n, &sz, &ok);
    if (!ok)
        return false;
    char *h = d ? to_hex(d, sz) : xstrdup("");
    *out = v_cstr(h);
    free(h);
    free(d);
    return true;
}

FN(f_varnum)
{
    size_t sz;
    bool ok;
    uint8_t *d = basic_read(in, call, a, n, &sz, &ok);
    if (!ok)
        return false;
    *out = v_int(d ? (int64_t)le_value(d, sz) : 0);
    free(d);
    return true;
}

static bool set_common(Interp *in, Node *call, Value *a, int n, Value *out, bool is_str)
{
    EFI_GUID g;
    if (!arg_guid(in, call, a, n, 2, &g))
        return false;
    uint8_t *data;
    size_t size;
    if (is_str) {
        size_t units;
        data = (uint8_t *)utf8_to_ucs2(a[1].s->s, &units);
        size = (units + 1) * 2;
    } else {
        data = from_hex(a[1].s->s, &size);
        if (!data)
            return rt_err(in, call, "SETVAR: data must be hexadecimal, e.g. \"0100\"");
    }
    UINT32 attr = n > 3 ? (UINT32)a[3].i
                        : EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
    if (n <= 3) {
        size_t osz;
        UINT32 oattr;
        uint8_t *old = efi_var_read(a[0].s->s, &g, &osz, &oattr, NULL);
        if (old) {
            attr = oattr;
            free(old);
        }
    }
    EFI_STATUS st = efi_var_write(a[0].s->s, &g, attr, data, size);
    free(data);
    in->err = EFI_ERROR(st) ? (int64_t)(st & 0xFF) : RC_OK;
    *out = v_int(in->err);
    return true;
}

FN(f_setvar) { return set_common(in, call, a, n, out, false); }
FN(f_setvarstr) { return set_common(in, call, a, n, out, true); }

FN(f_delvar)
{
    EFI_GUID g;
    if (!arg_guid(in, call, a, n, 1, &g))
        return false;
    EFI_STATUS st = efi_var_write(a[0].s->s, &g, 0, NULL, 0);
    in->err = EFI_ERROR(st) ? (int64_t)(st & 0xFF) : RC_OK;
    *out = v_int(in->err);
    return true;
}

FN(f_fwvendor)
{
    (void)in, (void)call, (void)a, (void)n;
    char *s = ucs2_to_utf8(gST->FirmwareVendor, (size_t)-1);
    *out = v_cstr(s);
    free(s);
    return true;
}

FN(f_secureboot)
{
    (void)in, (void)call, (void)a, (void)n;
    *out = v_int(secure_boot_active() ? -1 : 0);
    return true;
}

static const BFunc var_funcs[] = {
    { "varexists", 1, 2, "s", f_varexists },
    { "var$", 1, 2, "s", f_var },
    { "varhex$", 1, 2, "s", f_varhex },
    { "varnum", 1, 2, "s", f_varnum },
    { "setvar", 2, 4, "sssn", f_setvar },
    { "setvarstr", 2, 4, "sssn", f_setvarstr },
    { "delvar", 1, 2, "s", f_delvar },
    { "fwvendor$", 0, 0, "", f_fwvendor },
    { "secureboot", 0, 0, "", f_secureboot },
};

static const Cmd var_cmds[] = {
    { "var", cmd_var, "var list|get|set|del|save|load ...",
      "Read and write UEFI variables",
      "  var list [PATTERN] [-g GUID]     list variables (e.g. var list Boot*)\n"
      "  var get NAME [-g GUID] [-x|-s|-n] [-v]\n"
      "                                   show a value (-x hex dump, -s string,\n"
      "                                   -n number)\n"
      "  var set NAME VALUE [-g GUID] [-t TYPE] [-a ATTR]\n"
      "        TYPE: str (UCS-2, default), ascii, u8, u16, u32, u64, hex, file\n"
      "        ATTR: nv,bs,rt (default for new variables; existing ones keep\n"
      "        theirs)\n"
      "  var del NAME [-g GUID]           delete a variable\n"
      "  var save FILE [PATTERN] [-g GUID] save variables to a text file\n"
      "  var load FILE                    restore variables saved with var save\n"
      "GUID: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx or a name: global (default),\n"
      "security, shim, shell, systemd.\n"
      "With -data (var list): name, guid, guid_name, attributes, size.\n", CMD_DATA },
};

void efi_var_init(void)
{
    shell_register(var_cmds, ARRAY_SIZE(var_cmds));
    shell_register(edk_var_cmds, ARRAY_SIZE(edk_var_cmds));
    basic_register_funcs(var_funcs, ARRAY_SIZE(var_funcs));
}
