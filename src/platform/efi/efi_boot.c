/* Boot manager: Boot#### / BootOrder / BootNext / Timeout.
 *
 * Every change is first collected in a plan of variable writes; the plan is
 * shown (-n: only shown), confirmed (-y: no question), preceded by an
 * automatic backup of all boot variables (-B: no backup) and then applied. */
#include "efi_cmds.h"
#include "../../basic/interp_int.h"

#define LOAD_OPTION_ACTIVE 0x00000001
#define LOAD_OPTION_FORCE_RECONNECT 0x00000002
#define LOAD_OPTION_HIDDEN 0x00000008
#define BOOT_ATTR (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

void var_backup_line(Sbuf *b, const char *name, const EFI_GUID *g, UINT32 attr, const uint8_t *d, size_t n);
int var_restore_text(const char *text, bool verbose);

/* ---- Load options ---- */

typedef struct {
    UINT32 attr;
    char *desc;
    const EFI_DEVICE_PATH_PROTOCOL *dp;
    size_t dplen;
    const uint8_t *opt;
    size_t optlen;
    uint8_t *raw; /* owns the data */
    size_t rawlen;
} LoadOpt;

static void lo_free(LoadOpt *o)
{
    free(o->desc);
    free(o->raw);
    memset(o, 0, sizeof(*o));
}

static bool devpath_valid(const uint8_t *p, size_t len)
{
    size_t off = 0;
    while (off + 4 <= len) {
        const EFI_DEVICE_PATH_PROTOCOL *n = (const void *)(p + off);
        size_t l = n->Length[0] | (n->Length[1] << 8);
        if (l < 4 || off + l > len)
            return false;
        off += l;
        if (n->Type == END_DEVICE_PATH_TYPE && n->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE)
            return off == len;
    }
    return false;
}

/* Takes ownership of data on success. */
static bool lo_parse(uint8_t *d, size_t n, LoadOpt *o)
{
    memset(o, 0, sizeof(*o));
    if (n < 6 + 2)
        return false;
    UINT32 attr = (UINT32)(d[0] | d[1] << 8 | d[2] << 16 | (UINT32)d[3] << 24);
    size_t fplen = (size_t)(d[4] | d[5] << 8);
    size_t p = 6;
    while (p + 1 < n && (d[p] | d[p + 1]))
        p += 2;
    if (p + 1 >= n)
        return false;
    size_t desc_units = (p - 6) / 2;
    p += 2; /* NUL */
    if (p + fplen > n || !devpath_valid(d + p, fplen))
        return false;
    uint16_t *u = xmalloc((desc_units + 1) * 2);
    memcpy(u, d + 6, desc_units * 2);
    u[desc_units] = 0;
    o->desc = ucs2_to_utf8(u, (size_t)-1);
    free(u);
    o->attr = attr;
    o->dp = (const void *)(d + p);
    o->dplen = fplen;
    o->opt = d + p + fplen;
    o->optlen = n - p - fplen;
    o->raw = d;
    o->rawlen = n;
    return true;
}

static uint8_t *lo_build(UINT32 attr, const char *desc, const void *dp, size_t dplen, const void *opt, size_t optlen,
                         size_t *out)
{
    size_t units;
    uint16_t *wd = utf8_to_ucs2(desc, &units);
    size_t n = 6 + (units + 1) * 2 + dplen + optlen;
    uint8_t *d = xmalloc(n);
    d[0] = (uint8_t)attr, d[1] = (uint8_t)(attr >> 8), d[2] = (uint8_t)(attr >> 16), d[3] = (uint8_t)(attr >> 24);
    d[4] = (uint8_t)dplen, d[5] = (uint8_t)(dplen >> 8);
    memcpy(d + 6, wd, (units + 1) * 2);
    memcpy(d + 6 + (units + 1) * 2, dp, dplen);
    if (optlen)
        memcpy(d + 6 + (units + 1) * 2 + dplen, opt, optlen);
    free(wd);
    *out = n;
    return d;
}

static char *opt_text(const LoadOpt *o)
{
    /* optional data shown as text when it is a UCS-2 string (kernel/loader arguments) */
    if (!o->optlen)
        return xstrdup("");
    if (o->optlen % 2 == 0) {
        size_t units = o->optlen / 2;
        bool ok = true;
        for (size_t i = 0; i < units && ok; i++) {
            uint16_t c = (uint16_t)(o->opt[2 * i] | o->opt[2 * i + 1] << 8);
            if (c == 0 && i == units - 1)
                break;
            /* arguments are plain text: anything else is binary data (e.g. a GUID) */
            if (c < 0x20 || (c > 0x7E && c < 0xA0) || c > 0x24F)
                ok = false;
        }
        if (ok) {
            uint16_t *u = xmalloc(o->optlen + 2);
            memcpy(u, o->opt, o->optlen);
            u[units] = 0;
            char *r = ucs2_to_utf8(u, (size_t)-1);
            free(u);
            return r;
        }
    }
    return xasprintf("<%zu bytes of binary data>", o->optlen);
}

static char *boot_name(const char *kind, unsigned id)
{
    return xasprintf("%s%04X", kind, id);
}

static bool read_opt(const char *kind, unsigned id, LoadOpt *o)
{
    char *name = boot_name(kind, id);
    size_t n;
    uint8_t *d = efi_var_read(name, &gEfiGlobalVariableGuid, &n, NULL, NULL);
    free(name);
    if (!d)
        return false;
    if (!lo_parse(d, n, o)) {
        free(d);
        memset(o, 0, sizeof(*o));
        o->desc = xstrdup("<invalid load option>");
        return true;
    }
    return true;
}

static uint16_t *read_u16_list(const char *name, size_t *count)
{
    size_t n;
    uint8_t *d = efi_var_read(name, &gEfiGlobalVariableGuid, &n, NULL, NULL);
    *count = d ? n / 2 : 0;
    return (uint16_t *)d;
}

static bool read_u16(const char *name, uint16_t *v)
{
    size_t n;
    uint8_t *d = efi_var_read(name, &gEfiGlobalVariableGuid, &n, NULL, NULL);
    bool ok = d && n >= 2;
    if (ok)
        *v = (uint16_t)(d[0] | d[1] << 8);
    free(d);
    return ok;
}

typedef struct {
    const char *kind;
    unsigned *ids;
    int n;
} IdCollect;

static bool hex4(const char *s)
{
    for (int i = 0; i < 4; i++)
        if (!isxdigit((uint8_t)s[i]) || islower((uint8_t)s[i]))
            return false;
    return s[4] == 0;
}

static unsigned *existing_ids(const char *kind, int *count)
{
    /* enumerate "Boot####" (global GUID) */
    UINTN cap = 512;
    CHAR16 *name = xcalloc(1, cap);
    EFI_GUID g;
    memset(&g, 0, sizeof(g));
    unsigned *ids = NULL;
    int n = 0;
    size_t kl = strlen(kind);
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
        if (memcmp(&g, &gEfiGlobalVariableGuid, sizeof(g)))
            continue;
        char *s = ucs2_to_utf8(name, (size_t)-1);
        if (strlen(s) == kl + 4 && !strncmp(s, kind, kl) && hex4(s + kl)) {
            ids = xrealloc(ids, sizeof(unsigned) * (n + 1));
            ids[n++] = (unsigned)strtoul(s + kl, NULL, 16);
        }
        free(s);
    }
    free(name);
    *count = n;
    return ids;
}

static bool parse_id(const char *s, unsigned *id)
{
    if (!strncasecmp(s, "boot", 4) || !strncasecmp(s, "driver", 6))
        s += s[0] == 'b' || s[0] == 'B' ? 4 : 6;
    size_t l = strlen(s);
    if (!l || l > 4)
        return false;
    for (size_t i = 0; i < l; i++)
        if (!isxdigit((uint8_t)s[i]))
            return false;
    *id = (unsigned)strtoul(s, NULL, 16);
    return true;
}

/* ---- Plans ---- */

typedef struct {
    char *name;
    uint8_t *data; /* NULL: delete */
    size_t size;
    char *what;    /* human description */
} Write;

typedef struct {
    Write *w;
    int n;
    bool dry, yes, nobackup;
    bool quiet; /* bcfg: no listing, backup only if possible */
} Plan;

static void plan_add(Plan *p, const char *name, uint8_t *data, size_t size, char *what)
{
    p->w = xrealloc(p->w, sizeof(Write) * (p->n + 1));
    p->w[p->n].name = xstrdup(name);
    p->w[p->n].data = data;
    p->w[p->n].size = size;
    p->w[p->n].what = what;
    p->n++;
}

static void plan_free(Plan *p)
{
    for (int i = 0; i < p->n; i++) {
        free(p->w[i].name);
        free(p->w[i].data);
        free(p->w[i].what);
    }
    free(p->w);
    p->w = NULL;
    p->n = 0;
}

static uint8_t *u16_bytes(const unsigned *v, int n, size_t *size)
{
    uint8_t *d = xmalloc((size_t)(n ? n : 1) * 2);
    for (int i = 0; i < n; i++) {
        d[2 * i] = (uint8_t)v[i];
        d[2 * i + 1] = (uint8_t)(v[i] >> 8);
    }
    *size = (size_t)n * 2;
    return d;
}

static char *order_text(const unsigned *v, int n)
{
    Sbuf b;
    sb_init(&b);
    for (int i = 0; i < n; i++)
        sb_printf(&b, "%s%04X", i ? "," : "", v[i]);
    if (!n)
        sb_adds(&b, "(empty)");
    return sb_steal(&b);
}

static void plan_order(Plan *p, const char *var, const unsigned *v, int n)
{
    size_t size;
    uint8_t *d = u16_bytes(v, n, &size);
    char *t = order_text(v, n);
    plan_add(p, var, d, size, xasprintf("%s = %s", var, t));
    free(t);
}

/* Saves all boot-related variables to a timestamped file on the boot volume. */
static char *backup_boot_vars(void)
{
    Sbuf b;
    sb_init(&b);
    sb_adds(&b, "# NESH boot variables backup: GUID ATTRIBUTES NAME DATA\n");
    static const char *kinds[] = { "Boot", "Driver", "SysPrep" };
    for (size_t k = 0; k < ARRAY_SIZE(kinds); k++) {
        int n;
        unsigned *ids = existing_ids(kinds[k], &n);
        for (int i = 0; i < n; i++) {
            char *name = boot_name(kinds[k], ids[i]);
            size_t sz;
            UINT32 attr;
            uint8_t *d = efi_var_read(name, &gEfiGlobalVariableGuid, &sz, &attr, NULL);
            if (d)
                var_backup_line(&b, name, &gEfiGlobalVariableGuid, attr, d, sz);
            free(d);
            free(name);
        }
        free(ids);
    }
    static const char *singles[] = { "BootOrder", "DriverOrder", "SysPrepOrder", "BootNext", "Timeout" };
    for (size_t k = 0; k < ARRAY_SIZE(singles); k++) {
        size_t sz;
        UINT32 attr;
        uint8_t *d = efi_var_read(singles[k], &gEfiGlobalVariableGuid, &sz, &attr, NULL);
        if (d)
            var_backup_line(&b, singles[k], &gEfiGlobalVariableGuid, attr, d, sz);
        free(d);
    }
    int bv = pal_boot_volume();
    if (bv < 0) {
        sb_free(&b);
        return NULL;
    }
    PalTime t = { 0 };
    pal_get_time(&t);
    char *dir1 = xasprintf("%s:\\nesh", pal_volume(bv)->name);
    char *dir2 = xasprintf("%s\\backup", dir1);
    pal_mkdir(dir1);
    pal_mkdir(dir2);
    char *path = xasprintf("%s\\boot-%04d%02d%02d-%02d%02d%02d.txt", dir2, t.year, t.month, t.day, t.hour, t.min, t.sec);
    int e = file_write_all(path, b.s, b.len, false);
    sb_free(&b);
    free(dir1);
    free(dir2);
    if (e) {
        free(path);
        return NULL;
    }
    return path;
}

static bool confirm(const char *question)
{
    char *prompt = xasprintf("%s [y/N] ", question);
    char *ans = lineedit_read(prompt, false);
    free(prompt);
    bool yes = ans && (!strcasecmp(ans, "y") || !strcasecmp(ans, "yes") || !strcasecmp(ans, "s") || !strcasecmp(ans, "si"));
    free(ans);
    return yes;
}

static int plan_apply(Plan *p)
{
    if (!p->n) {
        out_puts("Nothing to change.\n");
        return RC_OK;
    }
    if (!p->quiet || p->dry) {
        out_puts(p->dry ? "Planned changes (dry run, nothing is written):\n" : "Changes:\n");
        for (int i = 0; i < p->n; i++)
            out_printf("  %s\n", p->w[i].what);
    }
    if (p->dry)
        return RC_OK;
    if (!p->yes && !confirm("Apply these changes?")) {
        out_puts("Cancelled.\n");
        return RC_FAIL;
    }
    if (!p->nobackup) {
        char *bk = backup_boot_vars();
        if (!bk && p->quiet)
            ; /* bcfg keeps the UEFI Shell behaviour: no backup possible, go on */
        else if (!bk)
            return cmd_err("bootmgr", "cannot save the backup of the boot variables on the boot volume "
                                      "(use -B to continue without backup)");
        else if (!p->quiet)
            out_printf("Backup saved to %s\n", bk);
        free(bk);
    }
    int rc = RC_OK;
    for (int i = 0; i < p->n; i++) {
        EFI_STATUS st = efi_var_write(p->w[i].name, &gEfiGlobalVariableGuid, p->w[i].data ? BOOT_ATTR : 0,
                                      p->w[i].data, p->w[i].data ? p->w[i].size : 0);
        if (EFI_ERROR(st) && !(st == EFI_NOT_FOUND && !p->w[i].data)) {
            rc = cmd_err("bootmgr", "writing %s failed: %s", p->w[i].name, efi_strerror(st));
            break;
        }
    }
    if (!rc && !p->quiet)
        out_puts("Done.\n");
    return rc;
}

/* ---- Helpers for the order list ---- */

static unsigned *get_order(const char *var, int *n)
{
    size_t cnt;
    uint16_t *o = read_u16_list(var, &cnt);
    unsigned *r = xmalloc(sizeof(unsigned) * (cnt + 1));
    for (size_t i = 0; i < cnt; i++)
        r[i] = o[i];
    free(o);
    *n = (int)cnt;
    return r;
}

static int order_index(const unsigned *v, int n, unsigned id)
{
    for (int i = 0; i < n; i++)
        if (v[i] == id)
            return i;
    return -1;
}

/* ---- Commands ---- */

typedef struct {
    bool n, y, B, v, t, f, d;
    const char *desc, *args, *file;
    char *pos[64];
    int npos;
} BArgs;

static bool bargs(int argc, char **argv, BArgs *a)
{
    memset(a, 0, sizeof(*a));
    for (int i = 2; i < argc; i++) {
        const char *s = argv[i];
        if (!strcmp(s, "--")) {
            for (i++; i < argc && a->npos < 64; i++)
                a->pos[a->npos++] = argv[i];
            break;
        }
        if ((!strcmp(s, "-D") || !strcmp(s, "-a") || !strcmp(s, "-F")) && i + 1 < argc) {
            if (s[1] == 'D')
                a->desc = argv[++i];
            else if (s[1] == 'a')
                a->args = argv[++i];
            else
                a->file = argv[++i];
            continue;
        }
        if (s[0] == '-' && s[1] && !s[2] && strchr("nyBvtfd", s[1])) {
            switch (s[1]) {
            case 'n': a->n = true; break;
            case 'y': a->y = true; break;
            case 'B': a->B = true; break;
            case 'v': a->v = true; break;
            case 't': a->t = true; break;
            case 'f': a->f = true; break;
            case 'd': a->d = true; break;
            }
            continue;
        }
        if (a->npos < 64)
            a->pos[a->npos++] = argv[i];
    }
    return true;
}

static void data_entry(const char *kind, unsigned id, const LoadOpt *o, int pos, bool current, bool next)
{
    data_record();
    data_field("kind", "entry");
    data_field("id", "%04X", id);
    data_field("variable", "%s%04X", kind, id);
    if (pos >= 0)
        data_field("order", "%d", pos + 1);
    else
        data_field("order", "%s", "");
    data_field("active", "%s", o->attr & LOAD_OPTION_ACTIVE ? "yes" : "no");
    data_field("hidden", "%s", o->attr & LOAD_OPTION_HIDDEN ? "yes" : "no");
    data_field("current", "%s", current ? "yes" : "no");
    data_field("next", "%s", next ? "yes" : "no");
    data_field("description", "%s", o->desc ? o->desc : "");
    char *t = o->dp ? efi_devpath_text(o->dp) : xstrdup("");
    data_field("devpath", "%s", t);
    free(t);
    char *args = o->raw ? opt_text(o) : xstrdup("");
    data_field("args", "%s", args);
    free(args);
}

static void print_entry(unsigned id, const LoadOpt *o, int pos, bool current, bool next, bool verbose)
{
    int fg, bg;
    pal_con_get_color(&fg, &bg);
    char ord[8] = "  -";
    if (pos >= 0)
        snprintf(ord, sizeof(ord), "%3d", pos + 1);
    bool active = o->attr & LOAD_OPTION_ACTIVE;
    out_printf("%s  %04X  %c%c%c  ", ord, id, active ? 'A' : '-', o->attr & LOAD_OPTION_HIDDEN ? 'H' : ' ',
               current ? '*' : next ? 'N' : ' ');
    if (!active)
        out_color(C_DARKGRAY, bg);
    else if (current)
        out_color(C_WHITE, bg);
    out_printf("%s\n", o->desc ? o->desc : "");
    out_color(fg, bg);
    if (o->dp) {
        char *t = efi_devpath_text(o->dp);
        out_color(C_DARKGRAY, bg);
        out_printf("                 %s\n", t);
        out_color(fg, bg);
        free(t);
    }
    if (o->optlen) {
        char *t = opt_text(o);
        if (verbose || *t != '<')
            out_printf("                 args: %s\n", t);
        free(t);
    }
}

static int bm_list(BArgs *a, const char *kind)
{
    char *order_var = xasprintf("%sOrder", kind);
    int norder;
    unsigned *order = get_order(order_var, &norder);
    int nids;
    unsigned *ids = existing_ids(kind, &nids);
    uint16_t cur = 0xFFFF, next = 0xFFFF, timeout = 0;
    bool is_boot = !strcmp(kind, "Boot");
    bool has_cur = is_boot && read_u16("BootCurrent", &cur);
    bool has_next = is_boot && read_u16("BootNext", &next);
    bool has_to = is_boot && read_u16("Timeout", &timeout);
    if (out_data_mode()) {
        if (is_boot) {
            data_record();
            data_field("kind", "settings");
            char v[3][8] = { "", "", "" };
            if (has_cur)
                snprintf(v[0], sizeof(v[0]), "%04X", cur);
            if (has_next)
                snprintf(v[1], sizeof(v[1]), "%04X", next);
            if (has_to)
                snprintf(v[2], sizeof(v[2]), "%u", timeout);
            data_field("bootcurrent", "%s", v[0]);
            data_field("bootnext", "%s", v[1]);
            data_field("timeout", "%s", v[2]);
        }
        for (int pass = 0; pass < 2; pass++) {
            const unsigned *list = pass ? ids : order;
            int n = pass ? nids : norder;
            for (int i = 0; i < n; i++) {
                if (pass && order_index(order, norder, ids[i]) >= 0)
                    continue;
                LoadOpt o;
                if (read_opt(kind, list[i], &o)) {
                    data_entry(kind, list[i], &o, pass ? -1 : i, has_cur && cur == list[i], has_next && next == list[i]);
                    lo_free(&o);
                }
            }
        }
        free(order);
        free(ids);
        free(order_var);
        return RC_OK;
    }
    if (is_boot) {
        out_printf("BootCurrent: %s", has_cur ? "" : "-");
        if (has_cur)
            out_printf("%04X", cur);
        out_printf("   BootNext: %s", has_next ? "" : "-");
        if (has_next)
            out_printf("%04X", next);
        out_printf("   Timeout: ");
        if (has_to)
            out_printf("%u s\n", timeout);
        else
            out_puts("-\n");
    }
    out_puts("Ord  Id    Flg  Description\n");
    for (int i = 0; i < norder; i++) {
        LoadOpt o;
        if (!read_opt(kind, order[i], &o)) {
            out_printf("%3d  %04X  !    <missing: listed in %s but not defined>\n", i + 1, order[i], order_var);
            continue;
        }
        print_entry(order[i], &o, i, has_cur && cur == order[i], has_next && next == order[i], a->v);
        lo_free(&o);
    }
    for (int i = 0; i < nids; i++) {
        if (order_index(order, norder, ids[i]) >= 0)
            continue;
        LoadOpt o;
        if (read_opt(kind, ids[i], &o)) {
            print_entry(ids[i], &o, -1, has_cur && cur == ids[i], has_next && next == ids[i], a->v);
            lo_free(&o);
        }
    }
    if (!nids)
        out_printf("(no %s entries)\n", kind);
    out_puts("Flags: A active, H hidden, * current boot, N next boot. '-' in Ord: not in the boot order.\n");
    free(order);
    free(ids);
    free(order_var);
    return RC_OK;
}

static char *join_args(char **v, int n)
{
    Sbuf b;
    sb_init(&b);
    for (int i = 0; i < n; i++) {
        if (i)
            sb_putc(&b, ' ');
        sb_adds(&b, v[i]);
    }
    return sb_steal(&b);
}

static int bm_add(BArgs *a, const char *kind)
{
    if (a->npos < 2)
        return cmd_err("bootmgr", "usage: bootmgr add FILE \"DESCRIPTION\" [ARGS...] [-t] [-n] [-y]");
    char *path = path_resolve(a->pos[0]);
    PalStat st;
    if (!path || pal_stat(path, &st) != PAL_OK || st.is_dir) {
        free(path);
        return cmd_err("bootmgr", "%s: file not found", a->pos[0]);
    }
    const char *bad = file_check_efi_app(path);
    if (bad && !a->f) {
        free(path);
        return cmd_err("bootmgr", "%s: %s (use -f to add it anyway)", a->pos[0], bad);
    }
    EFI_DEVICE_PATH_PROTOCOL *dp = efi_file_devpath(path);
    if (!dp) {
        free(path);
        return cmd_err("bootmgr", "cannot build the device path of %s", a->pos[0]);
    }
    size_t dplen = efi_devpath_size(dp);
    int nids;
    unsigned *ids = existing_ids(kind, &nids);
    unsigned id = 0;
    while (id <= 0xFFFF && (order_index(ids, nids, id) >= 0))
        id++;
    free(ids);
    if (id > 0xFFFF) {
        free(dp);
        free(path);
        return cmd_err("bootmgr", "no free entry number");
    }
    char *args = a->npos > 2 ? join_args(a->pos + 2, a->npos - 2) : NULL;
    uint16_t *wargs = NULL;
    size_t argbytes = 0;
    if (args) {
        size_t units;
        wargs = utf8_to_ucs2(args, &units);
        argbytes = (units + 1) * 2;
    }
    size_t size;
    uint8_t *data = lo_build(LOAD_OPTION_ACTIVE, a->pos[1], dp, dplen, wargs, argbytes, &size);
    char *name = boot_name(kind, id);
    char *dptext = efi_devpath_text(dp);
    Plan p = { .dry = a->n, .yes = a->y, .nobackup = a->B };
    plan_add(&p, name, data, size, xasprintf("create %s \"%s\" -> %s%s%s", name, a->pos[1], dptext,
                                             args ? " args: " : "", args ? args : ""));
    char *order_var = xasprintf("%sOrder", kind);
    int norder;
    unsigned *order = get_order(order_var, &norder);
    order = xrealloc(order, sizeof(unsigned) * (norder + 1));
    if (a->t) {
        memmove(order + 1, order, sizeof(unsigned) * norder);
        order[0] = id;
    } else {
        order[norder] = id;
    }
    plan_order(&p, order_var, order, norder + 1);
    int rc = plan_apply(&p);
    plan_free(&p);
    free(order);
    free(order_var);
    free(name);
    free(dptext);
    free(args);
    free(wargs);
    free(dp);
    free(path);
    return rc;
}

static bool need_id(BArgs *a, unsigned *id, const char *kind, LoadOpt *o)
{
    if (a->npos != 1 || !parse_id(a->pos[0], id)) {
        err_printf("bootmgr: an entry number is required (e.g. 0003)\n");
        return false;
    }
    if (o && !read_opt(kind, *id, o)) {
        err_printf("bootmgr: %s%04X does not exist\n", kind, *id);
        return false;
    }
    return true;
}

static int bm_del(BArgs *a, const char *kind)
{
    unsigned id;
    LoadOpt o;
    if (!need_id(a, &id, kind, &o))
        return RC_FAIL;
    char *name = boot_name(kind, id);
    Plan p = { .dry = a->n, .yes = a->y, .nobackup = a->B };
    plan_add(&p, name, NULL, 0, xasprintf("delete %s \"%s\"", name, o.desc ? o.desc : ""));
    char *order_var = xasprintf("%sOrder", kind);
    int norder;
    unsigned *order = get_order(order_var, &norder);
    int idx = order_index(order, norder, id);
    if (idx >= 0) {
        memmove(order + idx, order + idx + 1, sizeof(unsigned) * (norder - idx - 1));
        plan_order(&p, order_var, order, norder - 1);
    }
    uint16_t next;
    if (!strcmp(kind, "Boot") && read_u16("BootNext", &next) && next == id)
        plan_add(&p, "BootNext", NULL, 0, xstrdup("delete BootNext"));
    int rc = plan_apply(&p);
    plan_free(&p);
    free(order);
    free(order_var);
    free(name);
    lo_free(&o);
    return rc;
}

static int bm_rewrite(BArgs *a, const char *kind, const char *sub)
{
    unsigned id;
    LoadOpt o;
    if (!need_id(a, &id, kind, &o))
        return RC_FAIL;
    if (!o.raw) {
        lo_free(&o);
        return cmd_err("bootmgr", "the entry is not a valid load option and cannot be modified");
    }
    UINT32 attr = o.attr;
    const char *desc = o.desc;
    const void *dp = o.dp;
    size_t dplen = o.dplen;
    const void *opt = o.opt;
    size_t optlen = o.optlen;
    uint16_t *wargs = NULL;
    EFI_DEVICE_PATH_PROTOCOL *newdp = NULL;
    Sbuf what;
    sb_init(&what);
    char *name = boot_name(kind, id);
    int rc = RC_OK;
    if (!strcmp(sub, "enable") || !strcmp(sub, "disable")) {
        bool en = sub[0] == 'e';
        attr = en ? attr | LOAD_OPTION_ACTIVE : attr & ~(UINT32)LOAD_OPTION_ACTIVE;
        sb_printf(&what, "%s %s \"%s\"", en ? "enable" : "disable", name, desc);
    } else { /* edit */
        if (!a->desc && !a->args && !a->file) {
            rc = cmd_err("bootmgr", "edit: nothing to change (use -D DESC, -a ARGS, -F FILE)");
            goto out;
        }
        sb_printf(&what, "modify %s:", name);
        if (a->desc) {
            desc = a->desc;
            sb_printf(&what, " description \"%s\"", desc);
        }
        if (a->args) {
            if (*a->args) {
                size_t units;
                wargs = utf8_to_ucs2(a->args, &units);
                opt = wargs;
                optlen = (units + 1) * 2;
            } else {
                opt = NULL;
                optlen = 0;
            }
            sb_printf(&what, " args \"%s\"", a->args);
        }
        if (a->file) {
            char *path = path_resolve(a->file);
            PalStat st;
            if (!path || pal_stat(path, &st) != PAL_OK || st.is_dir) {
                free(path);
                rc = cmd_err("bootmgr", "%s: file not found", a->file);
                goto out;
            }
            const char *bad = file_check_efi_app(path);
            if (bad && !a->f) {
                free(path);
                rc = cmd_err("bootmgr", "%s: %s (use -f to use it anyway)", a->file, bad);
                goto out;
            }
            newdp = efi_file_devpath(path);
            free(path);
            if (!newdp) {
                rc = cmd_err("bootmgr", "cannot build the device path of %s", a->file);
                goto out;
            }
            dp = newdp;
            dplen = efi_devpath_size(newdp);
            char *t = efi_devpath_text(newdp);
            sb_printf(&what, " file %s", t);
            free(t);
        }
    }
    {
        size_t size;
        uint8_t *data = lo_build(attr, desc, dp, dplen, opt, optlen, &size);
        Plan p = { .dry = a->n, .yes = a->y, .nobackup = a->B };
        plan_add(&p, name, data, size, sb_steal(&what));
        rc = plan_apply(&p);
        plan_free(&p);
    }
out:
    sb_free(&what);
    free(name);
    free(wargs);
    free(newdp);
    lo_free(&o);
    return rc;
}

static int bm_order(BArgs *a, const char *kind, bool top)
{
    char *order_var = xasprintf("%sOrder", kind);
    int norder;
    unsigned *order = get_order(order_var, &norder);
    unsigned *nw = NULL;
    int nn = 0;
    int rc = RC_OK;
    if (top) {
        unsigned id;
        LoadOpt o;
        if (!need_id(a, &id, kind, &o)) {
            rc = RC_FAIL;
            goto out;
        }
        lo_free(&o);
        nw = xmalloc(sizeof(unsigned) * (norder + 1));
        nw[nn++] = id;
        for (int i = 0; i < norder; i++)
            if (order[i] != id)
                nw[nn++] = order[i];
    } else {
        if (!a->npos) {
            char *t = order_text(order, norder);
            out_printf("%s: %s\n", order_var, t);
            free(t);
            goto out;
        }
        /* accepts "0001,0003 0002" */
        char *all = join_args(a->pos, a->npos);
        for (char *t = all; *t;) {
            while (*t == ',' || *t == ' ')
                t++;
            if (!*t)
                break;
            char *e = t;
            while (*e && *e != ',' && *e != ' ')
                e++;
            char save = *e;
            *e = 0;
            unsigned id;
            LoadOpt o;
            if (!parse_id(t, &id)) {
                rc = cmd_err("bootmgr", "invalid entry number %s", t);
            } else if (!read_opt(kind, id, &o)) {
                rc = cmd_err("bootmgr", "%s%04X does not exist", kind, id);
            } else {
                lo_free(&o);
                if (order_index(nw, nn, id) >= 0)
                    rc = cmd_err("bootmgr", "%04X is listed twice", id);
                nw = xrealloc(nw, sizeof(unsigned) * (nn + 1));
                nw[nn++] = id;
            }
            *e = save;
            t = e;
        }
        free(all);
        if (rc)
            goto out;
        for (int i = 0; i < norder; i++)
            if (order_index(nw, nn, order[i]) < 0)
                out_printf("note: %04X is removed from %s (the entry itself is kept)\n", order[i], order_var);
    }
    {
        Plan p = { .dry = a->n, .yes = a->y, .nobackup = a->B };
        plan_order(&p, order_var, nw, nn);
        rc = plan_apply(&p);
        plan_free(&p);
    }
out:
    free(nw);
    free(order);
    free(order_var);
    return rc;
}

static int bm_next(BArgs *a)
{
    Plan p = { .dry = a->n, .yes = true, .nobackup = a->B };
    if (a->d) {
        plan_add(&p, "BootNext", NULL, 0, xstrdup("delete BootNext"));
    } else if (!a->npos) {
        uint16_t v;
        if (read_u16("BootNext", &v))
            out_printf("BootNext: %04X\n", v);
        else
            out_puts("BootNext is not set\n");
        return RC_OK;
    } else {
        unsigned id;
        LoadOpt o;
        if (!need_id(a, &id, "Boot", &o))
            return RC_FAIL;
        size_t size;
        uint8_t *d = u16_bytes(&id, 1, &size);
        plan_add(&p, "BootNext", d, size, xasprintf("BootNext = %04X \"%s\" (used once at the next boot)", id, o.desc));
        lo_free(&o);
    }
    int rc = plan_apply(&p); /* BootNext only affects one boot: no confirmation */
    plan_free(&p);
    return rc;
}

static int bm_timeout(BArgs *a)
{
    if (!a->npos) {
        uint16_t v;
        if (read_u16("Timeout", &v))
            out_printf("Timeout: %u s\n", v);
        else
            out_puts("Timeout is not set\n");
        return RC_OK;
    }
    int64_t s;
    if (!parse_int(a->pos[0], &s) || s < 0 || s > 0xFFFF)
        return cmd_err("bootmgr", "timeout: seconds between 0 and 65535");
    unsigned v = (unsigned)s;
    size_t size;
    uint8_t *d = u16_bytes(&v, 1, &size);
    Plan p = { .dry = a->n, .yes = a->y, .nobackup = a->B };
    plan_add(&p, "Timeout", d, size, xasprintf("Timeout = %u s", v));
    int rc = plan_apply(&p);
    plan_free(&p);
    return rc;
}

static int bm_backup(BArgs *a)
{
    if (a->npos != 1)
        return cmd_err("bootmgr", "usage: bootmgr backup FILE");
    char *p = path_resolve(a->pos[0]);
    if (!p)
        return cmd_err("bootmgr", "%s: invalid path", a->pos[0]);
    char *tmp = backup_boot_vars(); /* also keeps a copy in \nesh\backup */
    if (!tmp) {
        free(p);
        return cmd_err("bootmgr", "cannot write the backup");
    }
    char *text;
    size_t len;
    int e = file_read_all(tmp, &text, &len);
    if (!e)
        e = file_write_all(p, text, len, false);
    if (!e) {
        free(text);
        out_printf("Boot variables saved to %s\n", p);
    }
    free(tmp);
    free(p);
    return e ? cmd_perr("bootmgr", a->pos[0], e) : RC_OK;
}

static int bm_restore(BArgs *a)
{
    if (a->npos != 1)
        return cmd_err("bootmgr", "usage: bootmgr restore FILE");
    char *p = path_resolve(a->pos[0]);
    char *text;
    size_t len;
    int e = p ? file_read_text(p, &text, &len) : PAL_ENOENT;
    free(p);
    if (e)
        return cmd_perr("bootmgr", a->pos[0], e);
    /* entries that exist now but not in the backup are deleted */
    Plan del = { .dry = a->n, .yes = a->y, .nobackup = a->B };
    static const char *kinds[] = { "Boot", "Driver", "SysPrep" };
    for (size_t k = 0; k < ARRAY_SIZE(kinds); k++) {
        int n;
        unsigned *ids = existing_ids(kinds[k], &n);
        for (int i = 0; i < n; i++) {
            char *name = boot_name(kinds[k], ids[i]);
            char *needle = xasprintf(" %s ", name);
            if (!strstr(text, needle))
                plan_add(&del, name, NULL, 0, xasprintf("delete %s (not in the backup)", name));
            free(needle);
            free(name);
        }
        free(ids);
    }
    int lines = 0;
    for (char *c = text; *c; c++)
        if (*c == '\n' && c[1] && c[1] != '#')
            lines++;
    out_printf("The backup contains %d variables.\n", lines);
    for (int i = 0; i < del.n; i++)
        out_printf("  %s\n", del.w[i].what);
    if (a->n) {
        out_puts("Dry run: nothing is written.\n");
        plan_free(&del);
        free(text);
        return RC_OK;
    }
    if (!a->y && !confirm("Restore the boot variables from this backup?")) {
        out_puts("Cancelled.\n");
        plan_free(&del);
        free(text);
        return RC_FAIL;
    }
    if (!a->B) {
        char *bk = backup_boot_vars();
        if (!bk) {
            plan_free(&del);
            free(text);
            return cmd_err("bootmgr", "cannot save the current boot variables (use -B to skip)");
        }
        out_printf("Current state saved to %s\n", bk);
        free(bk);
    }
    for (int i = 0; i < del.n; i++)
        efi_var_write(del.w[i].name, &gEfiGlobalVariableGuid, 0, NULL, 0);
    plan_free(&del);
    int fails = var_restore_text(text, false);
    free(text);
    if (fails)
        return cmd_err("bootmgr", "%d variables could not be restored", fails);
    out_puts("Done.\n");
    return RC_OK;
}

/* Checks that the files referenced by the entries exist. */
static int bm_check(BArgs *a, const char *kind)
{
    (void)a;
    EFI_GUID sfs = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    int nids;
    unsigned *ids = existing_ids(kind, &nids);
    int problems = 0;
    char *order_var = xasprintf("%sOrder", kind);
    int norder;
    unsigned *order = get_order(order_var, &norder);
    for (int i = 0; i < norder; i++) {
        if (order_index(ids, nids, order[i]) < 0) {
            out_printf("%04X  listed in %s but the entry does not exist\n", order[i], order_var);
            problems++;
        }
    }
    for (int i = 0; i < nids; i++) {
        LoadOpt o;
        if (!read_opt(kind, ids[i], &o))
            continue;
        const char *status = "ok";
        if (!o.raw) {
            status = "INVALID: malformed load option";
            problems++;
        } else {
            /* find the file path node, if any */
            const uint8_t *p = (const uint8_t *)o.dp;
            size_t off = 0;
            const FILEPATH_DEVICE_PATH *fp = NULL;
            while (off + 4 <= o.dplen) {
                const EFI_DEVICE_PATH_PROTOCOL *n = (const void *)(p + off);
                if (n->Type == MEDIA_DEVICE_PATH && n->SubType == MEDIA_FILEPATH_DP) {
                    fp = (const void *)n;
                    break;
                }
                if (n->Type == END_DEVICE_PATH_TYPE)
                    break;
                off += n->Length[0] | (n->Length[1] << 8);
            }
            if (!fp) {
                status = "n/a (firmware application or device)";
            } else {
                EFI_DEVICE_PATH_PROTOCOL *rem = (EFI_DEVICE_PATH_PROTOCOL *)o.dp;
                EFI_HANDLE h;
                EFI_STATUS st = gBS->LocateDevicePath(&sfs, &rem, &h);
                if (EFI_ERROR(st) || (const void *)rem != (const void *)fp) {
                    /* short-form paths (starting with HD()) are expanded by the firmware at boot */
                    const EFI_DEVICE_PATH_PROTOCOL *first = o.dp;
                    status = first->Type == MEDIA_DEVICE_PATH ? "unverified (short-form path)" : "MISSING: device not found";
                    if (first->Type != MEDIA_DEVICE_PATH)
                        problems++;
                } else {
                    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
                    EFI_FILE_PROTOCOL *root, *f;
                    size_t units = ((fp->Header.Length[0] | (fp->Header.Length[1] << 8)) - 4) / 2;
                    CHAR16 *name = xcalloc(units + 1, 2);
                    memcpy(name, fp->PathName, units * 2);
                    if (gBS->HandleProtocol(h, &sfs, (void **)&fs) == EFI_SUCCESS && fs->OpenVolume(fs, &root) == EFI_SUCCESS) {
                        if (root->Open(root, &f, name, EFI_FILE_MODE_READ, 0) == EFI_SUCCESS) {
                            f->Close(f);
                        } else {
                            status = "MISSING: file not found";
                            problems++;
                        }
                        root->Close(root);
                    }
                    free(name);
                }
            }
        }
        out_printf("%04X  %-40s %s\n", ids[i], o.desc ? o.desc : "", status);
        lo_free(&o);
    }
    free(ids);
    free(order);
    free(order_var);
    if (problems)
        out_printf("%d problem%s found.\n", problems, problems == 1 ? "" : "s");
    else
        out_puts("No problems found.\n");
    return problems ? RC_FAIL : RC_OK;
}

static int cmd_bootmgr(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "list";
    BArgs a;
    bargs(argc, argv, &a);
    const char *kind = "Boot";
    /* -driver: operate on Driver#### entries */
    for (int i = 0; i < a.npos; i++) {
        if (!strcasecmp(a.pos[i], "-driver")) {
            kind = "Driver";
            memmove(a.pos + i, a.pos + i + 1, sizeof(char *) * (a.npos - i - 1));
            a.npos--;
            break;
        }
    }
    if (!strcasecmp(sub, "list") || !strcasecmp(sub, "ls"))
        return bm_list(&a, kind);
    if (!strcasecmp(sub, "add"))
        return bm_add(&a, kind);
    if (!strcasecmp(sub, "del") || !strcasecmp(sub, "rm"))
        return bm_del(&a, kind);
    if (!strcasecmp(sub, "enable") || !strcasecmp(sub, "disable") || !strcasecmp(sub, "edit"))
        return bm_rewrite(&a, kind, sub[0] == 'e' && sub[1] == 'n' ? "enable" : sub[0] == 'd' ? "disable" : "edit");
    if (!strcasecmp(sub, "order"))
        return bm_order(&a, kind, false);
    if (!strcasecmp(sub, "top"))
        return bm_order(&a, kind, true);
    if (!strcasecmp(sub, "next"))
        return bm_next(&a);
    if (!strcasecmp(sub, "timeout"))
        return bm_timeout(&a);
    if (!strcasecmp(sub, "backup"))
        return bm_backup(&a);
    if (!strcasecmp(sub, "restore"))
        return bm_restore(&a);
    if (!strcasecmp(sub, "check"))
        return bm_check(&a, kind);
    return cmd_usage("bootmgr");
}

/* ---- bcfg (UEFI Shell syntax) ----
 * Numbers are positions in BootOrder/DriverOrder (hexadecimal, as shown by
 * "bcfg boot dump"). No confirmation is asked, as in the UEFI Shell; the
 * automatic backup is still saved when the boot volume is writable. */

static bool bcfg_num(const char *s, unsigned *v)
{
    char *end;
    if (!s || !*s)
        return false;
    *v = (unsigned)strtoul(s, &end, 16);
    return !*end;
}

/* Device path for a file: full, or short form starting at the partition (HD) node. */
static EFI_DEVICE_PATH_PROTOCOL *bcfg_file_dp(const char *file, bool short_form, size_t *len)
{
    char *p = path_resolve(file);
    PalStat st;
    if (!p || pal_stat(p, &st) != PAL_OK || st.is_dir) {
        free(p);
        err_printf("bcfg: %s: file not found\n", file);
        return NULL;
    }
    EFI_DEVICE_PATH_PROTOCOL *dp = efi_file_devpath(p);
    free(p);
    if (!dp)
        return NULL;
    *len = efi_devpath_size(dp);
    if (!short_form)
        return dp;
    const uint8_t *n = (const uint8_t *)dp;
    size_t off = 0;
    while (off + 4 <= *len) {
        const EFI_DEVICE_PATH_PROTOCOL *node = (const void *)(n + off);
        size_t l = node->Length[0] | (node->Length[1] << 8);
        if (node->Type == END_DEVICE_PATH_TYPE || l < 4)
            break;
        if (node->Type == MEDIA_DEVICE_PATH && node->SubType == MEDIA_HARDDRIVE_DP) {
            size_t sl = *len - off;
            EFI_DEVICE_PATH_PROTOCOL *s = xmalloc(sl);
            memcpy(s, n + off, sl);
            free(dp);
            *len = sl;
            return s;
        }
        off += l;
    }
    out_puts("bcfg: the volume has no partition table: using the full device path\n");
    return dp;
}

static EFI_DEVICE_PATH_PROTOCOL *bcfg_handle_dp(const char *hs, size_t *len)
{
    EFI_HANDLE h;
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
    if (!efi_parse_handle(hs, &h) || gBS->HandleProtocol(h, &gEfiDevicePathGuid, (void **)&dp) != EFI_SUCCESS || !dp) {
        err_printf("bcfg: %s is not a handle with a device path\n", hs);
        return NULL;
    }
    *len = efi_devpath_size(dp);
    EFI_DEVICE_PATH_PROTOCOL *c = xmalloc(*len);
    memcpy(c, dp, *len);
    return c;
}

static void bcfg_dump(const char *kind, bool verbose)
{
    char *order_var = xasprintf("%sOrder", kind);
    int n;
    unsigned *order = get_order(order_var, &n);
    for (int i = 0; i < n; i++) {
        LoadOpt o;
        out_printf("Option: %02X. Variable: %s%04X\n", i, kind, order[i]);
        if (!read_opt(kind, order[i], &o)) {
            out_puts("  <missing variable>\n");
            continue;
        }
        char *t = o.dp ? efi_devpath_text(o.dp) : xstrdup("");
        out_printf("  Desc    - %s\n  DevPath - %s\n  Optional- %c\n", o.desc ? o.desc : "", t, o.optlen ? 'Y' : 'N');
        if (verbose && o.optlen) {
            char *a = opt_text(&o);
            out_printf("  Data    - %s\n", a);
            free(a);
        }
        free(t);
        lo_free(&o);
    }
    if (!n)
        out_printf("No %s options.\n", kind);
    free(order);
    free(order_var);
}

static int cmd_bcfg(int argc, char **argv)
{
    if (argc < 2 || (strcasecmp(argv[1], "boot") && strcasecmp(argv[1], "driver")))
        return cmd_usage("bcfg");
    const char *kind = !strcasecmp(argv[1], "boot") ? "Boot" : "Driver";
    const char *sub = argc > 2 ? argv[2] : "dump";
    char **a = argv + 3;
    int na = argc - 3;
    char *order_var = xasprintf("%sOrder", kind);
    int norder;
    unsigned *order = get_order(order_var, &norder);
    Plan p = { .yes = true, .quiet = true };
    int rc = RC_OK;
    unsigned pos, pos2;

    if (!strcasecmp(sub, "dump")) {
        bcfg_dump(kind, na > 0 && !strcasecmp(a[0], "-v"));
        goto out;
    }
    if (!strcasecmp(sub, "add") || !strcasecmp(sub, "addp") || !strcasecmp(sub, "addh")) {
        if (na != 3 || !bcfg_num(a[0], &pos)) {
            rc = cmd_usage("bcfg");
            goto out;
        }
        size_t dplen;
        EFI_DEVICE_PATH_PROTOCOL *dp = !strcasecmp(sub, "addh") ? bcfg_handle_dp(a[1], &dplen)
                                                                : bcfg_file_dp(a[1], !strcasecmp(sub, "addp"), &dplen);
        if (!dp) {
            rc = RC_FAIL;
            goto out;
        }
        int nids;
        unsigned *ids = existing_ids(kind, &nids);
        unsigned id = 0;
        while (id <= 0xFFFF && order_index(ids, nids, id) >= 0)
            id++;
        free(ids);
        size_t size;
        uint8_t *data = lo_build(LOAD_OPTION_ACTIVE, a[2], dp, dplen, NULL, 0, &size);
        free(dp);
        char *name = boot_name(kind, id);
        plan_add(&p, name, data, size, xasprintf("create %s", name));
        if (pos > (unsigned)norder)
            pos = (unsigned)norder;
        order = xrealloc(order, sizeof(unsigned) * (norder + 1));
        memmove(order + pos + 1, order + pos, sizeof(unsigned) * (norder - pos));
        order[pos] = id;
        plan_order(&p, order_var, order, norder + 1);
        rc = plan_apply(&p);
        if (!rc)
            out_printf("Target = %04X.\n", id);
        free(name);
        goto out;
    }
    if (!strcasecmp(sub, "rm")) {
        if (na != 1 || !bcfg_num(a[0], &pos) || pos >= (unsigned)norder) {
            rc = cmd_err("bcfg", "invalid option number (see bcfg %s dump)", argv[1]);
            goto out;
        }
        char *name = boot_name(kind, order[pos]);
        plan_add(&p, name, NULL, 0, xasprintf("delete %s", name));
        memmove(order + pos, order + pos + 1, sizeof(unsigned) * (norder - pos - 1));
        plan_order(&p, order_var, order, norder - 1);
        free(name);
        rc = plan_apply(&p);
        goto out;
    }
    if (!strcasecmp(sub, "mv")) {
        if (na != 2 || !bcfg_num(a[0], &pos) || !bcfg_num(a[1], &pos2) || pos >= (unsigned)norder ||
            pos2 >= (unsigned)norder) {
            rc = cmd_err("bcfg", "invalid option number (see bcfg %s dump)", argv[1]);
            goto out;
        }
        unsigned id = order[pos];
        memmove(order + pos, order + pos + 1, sizeof(unsigned) * (norder - pos - 1));
        memmove(order + pos2 + 1, order + pos2, sizeof(unsigned) * (norder - 1 - pos2));
        order[pos2] = id;
        plan_order(&p, order_var, order, norder);
        rc = plan_apply(&p);
        goto out;
    }
    if (!strcasecmp(sub, "mod") || !strcasecmp(sub, "modf") || !strcasecmp(sub, "modp") ||
        !strcasecmp(sub, "modh") || !strcasecmp(sub, "-opt")) {
        bool opt = !strcasecmp(sub, "-opt");
        if ((opt ? na < 1 || na > 2 : na != 2) || !bcfg_num(a[0], &pos) || pos >= (unsigned)norder) {
            rc = cmd_err("bcfg", "invalid option number or arguments (see help bcfg)");
            goto out;
        }
        LoadOpt o;
        if (!read_opt(kind, order[pos], &o) || !o.raw) {
            rc = cmd_err("bcfg", "%s%04X is missing or invalid", kind, order[pos]);
            goto out;
        }
        const char *desc = o.desc;
        const void *dp = o.dp, *optd = o.opt;
        size_t dplen = o.dplen, optlen = o.optlen;
        EFI_DEVICE_PATH_PROTOCOL *newdp = NULL;
        uint8_t *newopt = NULL;
        if (!strcasecmp(sub, "mod")) {
            desc = a[1];
        } else if (opt) {
            if (na == 1) {
                optd = NULL;
                optlen = 0;
            } else {
                /* a file: its content; otherwise the text as UCS-2 (loader/kernel arguments) */
                char *fp = path_resolve(a[1]);
                PalStat st;
                char *fd;
                size_t fl;
                if (fp && pal_stat(fp, &st) == PAL_OK && !st.is_dir && file_read_all(fp, &fd, &fl) == PAL_OK) {
                    newopt = (uint8_t *)fd;
                    optlen = fl;
                } else {
                    size_t units;
                    newopt = (uint8_t *)utf8_to_ucs2(a[1], &units);
                    optlen = (units + 1) * 2;
                }
                free(fp);
                optd = newopt;
            }
        } else {
            newdp = !strcasecmp(sub, "modh") ? bcfg_handle_dp(a[1], &dplen)
                                             : bcfg_file_dp(a[1], !strcasecmp(sub, "modp"), &dplen);
            if (!newdp) {
                lo_free(&o);
                rc = RC_FAIL;
                goto out;
            }
            dp = newdp;
        }
        size_t size;
        uint8_t *data = lo_build(o.attr, desc, dp, dplen, optd, optlen, &size);
        char *name = boot_name(kind, order[pos]);
        plan_add(&p, name, data, size, xasprintf("modify %s", name));
        rc = plan_apply(&p);
        free(name);
        free(newdp);
        free(newopt);
        lo_free(&o);
        goto out;
    }
    rc = cmd_usage("bcfg");
out:
    plan_free(&p);
    free(order);
    free(order_var);
    return rc;
}

/* ---- BASIC functions ---- */

#define FN(name) static bool name(Interp *in, Node *call, Value *a, int n, Value *out)

FN(f_bootcount)
{
    (void)in, (void)call, (void)a, (void)n;
    size_t cnt;
    free(read_u16_list("BootOrder", &cnt));
    *out = v_int((int64_t)cnt);
    return true;
}

FN(f_bootid)
{
    (void)n;
    size_t cnt;
    uint16_t *o = read_u16_list("BootOrder", &cnt);
    if (a[0].i < 0 || (size_t)a[0].i >= cnt) {
        free(o);
        return rt_err(in, call, "BOOTID: index %lld out of range (0 to %lld)", (long long)a[0].i, (long long)cnt - 1);
    }
    *out = v_int(o[a[0].i]);
    free(o);
    return true;
}

static bool opt_arg(Interp *in, Node *call, Value *a, LoadOpt *o)
{
    if (a[0].i < 0 || a[0].i > 0xFFFF)
        return rt_err(in, call, "invalid boot entry number");
    if (!read_opt("Boot", (unsigned)a[0].i, o)) {
        in->err = RC_NOTFOUND;
        memset(o, 0, sizeof(*o));
        return true;
    }
    in->err = RC_OK;
    return true;
}

FN(f_bootdesc)
{
    (void)n;
    LoadOpt o;
    if (!opt_arg(in, call, a, &o))
        return false;
    *out = v_cstr(o.desc ? o.desc : "");
    lo_free(&o);
    return true;
}

FN(f_bootpath)
{
    (void)n;
    LoadOpt o;
    if (!opt_arg(in, call, a, &o))
        return false;
    char *t = o.dp ? efi_devpath_text(o.dp) : xstrdup("");
    *out = v_cstr(t);
    free(t);
    lo_free(&o);
    return true;
}

FN(f_bootargs)
{
    (void)n;
    LoadOpt o;
    if (!opt_arg(in, call, a, &o))
        return false;
    char *t = o.raw ? opt_text(&o) : xstrdup("");
    *out = v_cstr(t);
    free(t);
    lo_free(&o);
    return true;
}

FN(f_bootenabled)
{
    (void)n;
    LoadOpt o;
    if (!opt_arg(in, call, a, &o))
        return false;
    *out = v_int(o.attr & LOAD_OPTION_ACTIVE ? -1 : 0);
    lo_free(&o);
    return true;
}

FN(f_bootcurrent)
{
    (void)in, (void)call, (void)a, (void)n;
    uint16_t v;
    *out = v_int(read_u16("BootCurrent", &v) ? v : -1);
    return true;
}

static const BFunc boot_funcs[] = {
    { "bootcount", 0, 0, "", f_bootcount },
    { "bootid", 1, 1, "n", f_bootid },
    { "bootdesc$", 1, 1, "n", f_bootdesc },
    { "bootpath$", 1, 1, "n", f_bootpath },
    { "bootargs$", 1, 1, "n", f_bootargs },
    { "bootenabled", 1, 1, "n", f_bootenabled },
    { "bootcurrent", 0, 0, "", f_bootcurrent },
};

static const Cmd boot_cmds[] = {
    { "bootmgr", cmd_bootmgr, "bootmgr [SUBCOMMAND] [OPTIONS]",
      "Manage the UEFI boot entries",
      "  list [-v]                  entries in boot order (default)\n"
      "  add FILE \"DESC\" [ARGS] [-t] new entry for FILE (-t: first in the order)\n"
      "  del ID                     delete an entry\n"
      "  enable ID | disable ID     activate / deactivate an entry\n"
      "  edit ID [-D DESC] [-a ARGS] [-F FILE]   change an entry\n"
      "  order [ID,ID,...]          show or set the boot order\n"
      "  top ID                     move an entry to the top of the order\n"
      "  next ID | next -d          entry for the next boot only / clear it\n"
      "  timeout [SECONDS]          firmware boot menu timeout\n"
      "  backup FILE | restore FILE save / restore all the boot variables\n"
      "  check                      find entries pointing to missing files\n"
      "Common options: -n dry run (show, change nothing), -y do not ask,\n"
      "-B no automatic backup, -driver work on Driver#### entries.\n"
      "IDs are the hexadecimal numbers of Boot#### (e.g. 0003).\n"
      "Before every change a backup is saved in \\nesh\\backup on the boot volume.\n"
      "To start an EFI file now, type its name (or RUN it in a script).\n"
      "With -data (list): one record per entry, plus one with the boot settings.\n", CMD_DATA },
};

static const Cmd bcfg_cmds[] = {
    { "bcfg", cmd_bcfg, "bcfg boot|driver [dump [-v] | add|addp|addh # FILE|HANDLE \"DESC\" | rm # | mv # # | "
                        "mod # \"DESC\" | modf|modp|modh # FILE|HANDLE | -opt # [FILE|\"DATA\"]]",
      "Manage boot/driver options (UEFI Shell syntax)",
      "  # is the position in BootOrder/DriverOrder (hex, as shown by dump).\n"
      "  dump [-v]             list the options in order\n"
      "  add # FILE \"DESC\"     new option for FILE at position # (full device path)\n"
      "  addp # FILE \"DESC\"    same with a short-form path (partition + file)\n"
      "  addh # HANDLE \"DESC\"  new option for the device path of a handle\n"
      "  rm #                  delete the option at position #\n"
      "  mv # #                move an option to another position\n"
      "  mod # \"DESC\"          change the description\n"
      "  modf|modp|modh # ...  change the file (full / short path) or use a handle\n"
      "  -opt # [FILE|\"TEXT\"]  optional data: a file's content or a text (UCS-2);\n"
      "                        without it the optional data is cleared\n"
      "No confirmation is asked; a backup is saved in \\nesh\\backup when possible.\n"
      "See also: bootmgr (NESH syntax, with dry run and confirmation).\n" },
};

void efi_boot_init(void)
{
    shell_register(boot_cmds, ARRAY_SIZE(boot_cmds));
    shell_register(bcfg_cmds, ARRAY_SIZE(bcfg_cmds));
    basic_register_funcs(boot_funcs, ARRAY_SIZE(boot_funcs));
}
