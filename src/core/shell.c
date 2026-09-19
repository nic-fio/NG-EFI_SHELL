/* Shell core: command registry, option parsing, canonical paths and wildcards,
 * file helpers, command-line splitting and execution, the prompt (REPL) and
 * nesh_main, the portable entry point. */
#include "shell.h"
#include "../basic/basic.h"

int platform_run_image(const char *path, int argc, char **argv);

/* ---- Command registry ---- */

static const Cmd **cmds;
static int ncmds;
static int last_status;

static int cmd_sort(const void *a, const void *b)
{
    return strcmp((*(const Cmd **)a)->name, (*(const Cmd **)b)->name);
}

void shell_register(const Cmd *table, int n)
{
    cmds = xrealloc(cmds, sizeof(Cmd *) * (ncmds + n));
    for (int i = 0; i < n; i++)
        cmds[ncmds++] = &table[i];
    qsort(cmds, ncmds, sizeof(Cmd *), cmd_sort);
}

const Cmd *shell_find_cmd(const char *name)
{
    for (int i = 0; i < ncmds; i++)
        if (!strcasecmp(cmds[i]->name, name))
            return cmds[i];
    return NULL;
}

int shell_cmd_count(void) { return ncmds; }
const Cmd *shell_cmd_at(int i) { return cmds[i]; }
int shell_last_status(void) { return last_status; }

/* ---- Messages and options ---- */

int cmd_err(const char *cmd, const char *fmt, ...)
{
    Sbuf b;
    sb_init(&b);
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    err_printf("%s: %s\n", cmd, b.s ? b.s : "");
    sb_free(&b);
    return RC_FAIL;
}

int cmd_perr(const char *cmd, const char *path, int palerr)
{
    return cmd_err(cmd, "%s: %s", path, pal_strerror(palerr));
}

int cmd_usage(const char *cmd)
{
    const Cmd *c = shell_find_cmd(cmd);
    err_printf("usage: %s\n", c && c->usage ? c->usage : cmd);
    return RC_USAGE;
}

int getopts(int argc, char **argv, const char *opts, bool *flags)
{
    size_t nopts = strlen(opts);
    for (size_t i = 0; i < nopts; i++)
        flags[i] = false;
    /* options may appear anywhere: operands are moved after them */
    char **operands = xmalloc(sizeof(char *) * (argc + 1));
    int nop = 0, i = 1;
    bool ok = true;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        if (a[0] == '-' && a[1] && !isdigit((uint8_t)a[1])) {
            for (const char *c = a + 1; *c; c++) {
                const char *pos = strchr(opts, *c);
                if (!pos) {
                    const Cmd *cmd = shell_find_cmd(argv[0]);
                    err_printf("%s: unknown option -%c\n", argv[0], *c);
                    if (cmd && cmd->usage)
                        err_printf("usage: %s\n", cmd->usage);
                    ok = false;
                    break;
                }
                flags[pos - opts] = true;
            }
            if (!ok)
                break;
        } else {
            operands[nop++] = argv[i];
        }
    }
    for (; ok && i < argc; i++)
        operands[nop++] = argv[i];
    if (!ok) {
        free(operands);
        return -1;
    }
    int first = argc - nop;
    for (int k = 0; k < nop; k++)
        argv[first + k] = operands[k];
    free(operands);
    return first;
}

/* ---- Paths ---- */

static char *cwd;

const char *shell_cwd(void)
{
    return cwd ? cwd : "";
}

/* Extra names for volumes ("usb:" -> fs1), set by map or by EFI applications. */
typedef struct {
    char *name;
    char *volume;
} MapAlias;
static MapAlias *map_aliases;
static int nmap_aliases;

static int find_volume(const char *name, size_t len)
{
    for (int i = 0; i < pal_volume_count(); i++) {
        const PalVolume *v = pal_volume(i);
        if (strlen(v->name) == len && !strncasecmp(v->name, name, len))
            return i;
    }
    for (int k = 0; k < nmap_aliases; k++) {
        if (strlen(map_aliases[k].name) == len && !strncasecmp(map_aliases[k].name, name, len)) {
            for (int i = 0; i < pal_volume_count(); i++)
                if (!strcasecmp(pal_volume(i)->name, map_aliases[k].volume))
                    return i;
        }
    }
    return -1;
}

/* volume NULL deletes the alias. Names are given without the ':'. */
bool map_alias_set(const char *name, const char *volume)
{
    for (int k = 0; k < nmap_aliases; k++) {
        if (!strcasecmp(map_aliases[k].name, name)) {
            free(map_aliases[k].name);
            free(map_aliases[k].volume);
            map_aliases[k] = map_aliases[--nmap_aliases];
            break;
        }
    }
    if (!volume)
        return true;
    if (find_volume(name, strlen(name)) >= 0)
        return false; /* would hide a real volume */
    map_aliases = xrealloc(map_aliases, sizeof(MapAlias) * (nmap_aliases + 1));
    map_aliases[nmap_aliases].name = xstrdup(name);
    map_aliases[nmap_aliases].volume = xstrdup(volume);
    nmap_aliases++;
    return true;
}

/* Aliases of a volume, "name1:;name2:" (malloc'd, may be empty). */
char *map_aliases_of(const char *volume)
{
    Sbuf b;
    sb_init(&b);
    for (int k = 0; k < nmap_aliases; k++) {
        if (!strcasecmp(map_aliases[k].volume, volume)) {
            if (b.len)
                sb_putc(&b, ';');
            sb_printf(&b, "%s:", map_aliases[k].name);
        }
    }
    return sb_steal(&b);
}

char *path_resolve(const char *p)
{
    int vol = -1;
    const char *rest = p;
    Sbuf raw;
    sb_init(&raw);
    const char *colon = strchr(p, ':');
    const char *sep = p + strcspn(p, "\\/");
    if (colon && colon < sep) {
        vol = find_volume(p, (size_t)(colon - p));
        if (vol < 0)
            return NULL;
        rest = colon + 1;
        sb_adds(&raw, "\\");
        sb_adds(&raw, rest);
    } else {
        const char *c = cwd ? strchr(cwd, ':') : NULL;
        if (!c)
            return NULL;
        vol = find_volume(cwd, (size_t)(c - cwd));
        if (vol < 0)
            return NULL;
        if (*p == '\\' || *p == '/') {
            sb_adds(&raw, p);
        } else {
            sb_adds(&raw, c + 1);
            sb_adds(&raw, "\\");
            sb_adds(&raw, p);
        }
    }
    /* normalize components */
    char **comp = NULL;
    int nc = 0;
    char *s = raw.s ? raw.s : (char *)"";
    for (char *q = s; *q;) {
        while (*q == '\\' || *q == '/')
            q++;
        if (!*q)
            break;
        char *e = q;
        while (*e && *e != '\\' && *e != '/')
            e++;
        size_t l = (size_t)(e - q);
        if (l == 1 && q[0] == '.') {
            /* skip */
        } else if (l == 2 && q[0] == '.' && q[1] == '.') {
            if (nc)
                free(comp[--nc]);
        } else {
            comp = xrealloc(comp, sizeof(char *) * (nc + 1));
            comp[nc++] = xstrndup(q, l);
        }
        q = e;
    }
    Sbuf out;
    sb_init(&out);
    sb_adds(&out, pal_volume(vol)->name);
    sb_putc(&out, ':');
    if (!nc)
        sb_putc(&out, '\\');
    for (int i = 0; i < nc; i++) {
        sb_putc(&out, '\\');
        sb_adds(&out, comp[i]);
        free(comp[i]);
    }
    free(comp);
    sb_free(&raw);
    return sb_steal(&out);
}

int shell_chdir(const char *p)
{
    char *np = path_resolve(p);
    if (!np)
        return PAL_ENOENT;
    PalStat st;
    int e = pal_stat(np, &st);
    if (e) {
        free(np);
        return e;
    }
    if (!st.is_dir) {
        free(np);
        return PAL_ENOTDIR;
    }
    free(cwd);
    cwd = np;
    return PAL_OK;
}

const char *path_basename(const char *p)
{
    const char *b = strrchr(p, '\\');
    const char *c = strrchr(p, ':');
    if (c && (!b || c > b))
        b = c;
    return b ? b + 1 : p;
}

char *path_dirname(const char *canon)
{
    const char *b = strrchr(canon, '\\');
    if (!b)
        return xstrdup(canon);
    const char *c = strchr(canon, ':');
    if (c && b == c + 1)
        return xstrndup(canon, (size_t)(b - canon) + 1); /* root */
    return xstrndup(canon, (size_t)(b - canon));
}

char *path_join(const char *dir, const char *name)
{
    size_t l = strlen(dir);
    if (l && dir[l - 1] == '\\')
        return xasprintf("%s%s", dir, name);
    return xasprintf("%s\\%s", dir, name);
}

bool path_has_wildcards(const char *p)
{
    return strchr(p, '*') || strchr(p, '?');
}

static int str_cmp(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

char **path_glob(const char *pattern, int *count)
{
    *count = 0;
    char *dir = path_dirname(pattern);
    const char *pat = path_basename(pattern);
    char **list = NULL;
    int n = 0;
    PalDir *d;
    if (pal_opendir(dir, &d) == PAL_OK) {
        PalStat st;
        while (pal_readdir(d, &st) == 1) {
            if (glob_match(pat, st.name, true) && (pat[0] == '.' || st.name[0] != '.' || !strcmp(pat, "*"))) {
                list = xrealloc(list, sizeof(char *) * (n + 1));
                list[n++] = path_join(dir, st.name);
            }
            free(st.name);
        }
        pal_closedir(d);
    }
    free(dir);
    if (n)
        qsort(list, n, sizeof(char *), str_cmp);
    *count = n;
    return list;
}

/* ---- Files ---- */

int file_read_all(const char *path, char **data, size_t *len)
{
    PalFile *f;
    int e = pal_open(path, PAL_O_READ, &f);
    if (e)
        return e;
    Sbuf b;
    sb_init(&b);
    char buf[4096];
    for (;;) {
        size_t got;
        e = pal_read(f, buf, sizeof(buf), &got);
        if (e == PAL_EISDIR || (e == PAL_OK && got == 0 && b.len == 0)) {
            PalStat st;
            if (pal_stat(path, &st) == PAL_OK && st.is_dir)
                e = PAL_EISDIR;
        }
        if (e || !got)
            break;
        sb_add(&b, buf, got);
    }
    pal_close(f);
    if (e) {
        sb_free(&b);
        return e;
    }
    *len = b.len;
    *data = sb_steal(&b);
    return PAL_OK;
}

int file_write_all(const char *path, const char *data, size_t len, bool append)
{
    PalFile *f;
    int e = pal_open(path, PAL_O_WRITE | PAL_O_CREATE | (append ? PAL_O_APPEND : PAL_O_TRUNC), &f);
    if (e)
        return e;
    e = pal_write(f, data, len);
    int e2 = pal_close(f);
    return e ? e : e2;
}

int file_read_text(const char *path, char **text, size_t *len)
{
    char *data;
    size_t n;
    int e = file_read_all(path, &data, &n);
    if (e)
        return e;
    if (n >= 2 && (uint8_t)data[0] == 0xFF && (uint8_t)data[1] == 0xFE) {
        size_t units = (n - 2) / 2;
        uint16_t *u = xmalloc((units + 1) * 2);
        memcpy(u, data + 2, units * 2);
        u[units] = 0;
        free(data);
        Sbuf b;
        sb_init(&b);
        for (size_t i = 0; i < units; i++)
            if (u[i] != '\r')
                sb_put_cp(&b, u[i]);
        free(u);
        *len = b.len;
        *text = sb_steal(&b);
        return PAL_OK;
    }
    size_t off = (n >= 3 && !memcmp(data, "\xEF\xBB\xBF", 3)) ? 3 : 0;
    /* drop carriage returns */
    size_t k = 0;
    for (size_t i = off; i < n; i++)
        if (data[i] != '\r')
            data[k++] = data[i];
    data[k] = 0;
    *len = k;
    *text = data;
    return PAL_OK;
}

/* ---- Command line splitting ---- */

void argv_free(char **argv)
{
    if (!argv)
        return;
    for (char **a = argv; *a; a++)
        free(*a);
    free(argv);
}

/* Splits words; double quotes group (a doubled "" inside quotes is a quote).
 * If redir is not NULL, unquoted "> file" / ">> file" are extracted. */
static char **split_ex(const char *line, int *argc, const char **err, char **redir, bool *append, bool keep_quotes)
{
    int n = 0, cap = 8;
    char **v = xmalloc(sizeof(char *) * cap);
    const char *p = line;
    *err = NULL;
    if (redir)
        *redir = NULL;
    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (redir && *p == '>') {
            bool app = p[1] == '>';
            p += app ? 2 : 1;
            while (*p == ' ' || *p == '\t')
                p++;
            Sbuf f;
            sb_init(&f);
            bool q = false;
            for (; *p && (q || (*p != ' ' && *p != '\t')); p++) {
                if (*p == '"')
                    q = !q;
                else
                    sb_putc(&f, *p);
            }
            if (!f.len) {
                sb_free(&f);
                *err = "missing file name after >";
                goto fail;
            }
            free(*redir);
            *redir = sb_steal(&f);
            *append = app;
            continue;
        }
        Sbuf w;
        sb_init(&w);
        bool q = false, any = false;
        for (; *p && (q || (*p != ' ' && *p != '\t' && !(redir && *p == '>'))); p++) {
            if (*p == '"') {
                if (keep_quotes) {
                    sb_putc(&w, '"');
                    q = !q;
                } else if (q && p[1] == '"') {
                    sb_putc(&w, '"');
                    p++;
                } else {
                    q = !q;
                }
                any = true;
            } else {
                sb_putc(&w, *p);
                any = true;
            }
        }
        if (q) {
            sb_free(&w);
            *err = "unterminated quote";
            goto fail;
        }
        if (n + 2 > cap)
            v = xrealloc(v, sizeof(char *) * (cap *= 2));
        v[n++] = any ? sb_steal(&w) : xstrdup("");
    }
    v[n] = NULL;
    *argc = n;
    return v;
fail:
    v[n] = NULL;
    argv_free(v);
    if (redir) {
        free(*redir);
        *redir = NULL;
    }
    return NULL;
}

char **shell_split(const char *line, int *argc, const char **err)
{
    return split_ex(line, argc, err, NULL, NULL, false);
}

/* ---- Execution ---- */

static bool has_ext(const char *p, const char *ext)
{
    size_t l = strlen(p), e = strlen(ext);
    return l > e && !strcasecmp(p + l - e, ext);
}

/* Looks for NAME, NAME.nsb, NAME.efi: as given if it contains a path,
 * otherwise in the directories of the "path" environment variable. */
char *shell_find_executable(const char *name)
{
    static const char *exts[] = { "", SCRIPT_EXT, ".efi" };
    bool is_path = strpbrk(name, "\\/:") != NULL;
    bool has_known_ext = has_ext(name, SCRIPT_EXT) || has_ext(name, ".efi");
    char **dirs = NULL;
    int nd = 0;
    if (is_path) {
        dirs = xmalloc(sizeof(char *));
        dirs[nd++] = NULL;
    } else {
        char *path = env_get("path");
        for (char *t = path ? path : (char *)"."; t && *t;) {
            char *e = strchr(t, ';');
            size_t l = e ? (size_t)(e - t) : strlen(t);
            if (l) {
                char *d = xstrndup(t, l);
                dirs = xrealloc(dirs, sizeof(char *) * (nd + 1));
                dirs[nd++] = !strcmp(d, ".") || !strcmp(d, ".\\") ? (free(d), xstrdup(shell_cwd())) : d;
            }
            t = e ? e + 1 : NULL;
        }
        free(path);
    }
    char *found = NULL;
    for (int d = 0; d < nd && !found; d++) {
        for (size_t e = 0; e < ARRAY_SIZE(exts) && !found; e++) {
            /* only files with a known extension are executable */
            if (!*exts[e] ? !has_known_ext : has_known_ext)
                continue;
            char *cand = xasprintf("%s%s", name, exts[e]);
            char *full = dirs[d] ? path_join(dirs[d], cand) : xstrdup(cand);
            free(cand);
            char *canon = path_resolve(full);
            free(full);
            PalStat st;
            if (canon && pal_stat(canon, &st) == PAL_OK && !st.is_dir)
                found = canon;
            else
                free(canon);
        }
    }
    for (int d = 0; d < nd; d++)
        free(dirs[d]);
    free(dirs);
    return found;
}

static int script_depth;
int shell_script_depth(void) { return script_depth; }
void shell_script_enter(void) { script_depth++; }
void shell_script_leave(void) { script_depth--; }

/* Replaces an alias in argv[0] with its words; returns a new argv or NULL. */
static char **expand_alias(int argc, char **argv, int *nargc)
{
    const char *val = alias_get(argv[0], NULL);
    if (!val)
        return NULL;
    int an;
    const char *err;
    char **aw = shell_split(val, &an, &err);
    if (!aw || !an) {
        argv_free(aw);
        return NULL;
    }
    char **nv = xmalloc(sizeof(char *) * (an + argc));
    for (int i = 0; i < an; i++)
        nv[i] = aw[i];
    free(aw);
    for (int i = 1; i < argc; i++)
        nv[an + i - 1] = xstrdup(argv[i]);
    nv[an + argc - 1] = NULL;
    *nargc = an + argc - 1;
    return nv;
}

int shell_exec_argv(int argc, char **argv)
{
    if (argc < 1)
        return last_status = RC_OK;
    static int alias_depth;
    if (alias_depth < 8) {
        int nargc;
        char **nv = expand_alias(argc, argv, &nargc);
        if (nv) {
            /* "alias ls ls -l": the expanded ls is the command, not the alias again */
            int saved = alias_depth;
            alias_depth = strcasecmp(nv[0], argv[0]) ? alias_depth + 1 : 8;
            int r = shell_exec_argv(nargc, nv);
            alias_depth = saved;
            argv_free(nv);
            return r;
        }
    }
    const char *name = argv[0];
    int rc;
    size_t nl = strlen(name);
    /* "fs1:" alone changes the current volume */
    if (argc == 1 && nl > 1 && name[nl - 1] == ':' && !strpbrk(name, "\\/")) {
        int e = shell_chdir(name);
        if (e)
            rc = cmd_err(name, "%s", e == PAL_ENOENT ? "no such volume" : pal_strerror(e));
        else
            rc = RC_OK;
        return last_status = rc;
    }
    const Cmd *c = shell_find_cmd(name);
    if (c) {
        /* "-data": removed from the arguments, the command sees out_data_mode() */
        bool want_data = false;
        char **cargv = xmalloc(sizeof(char *) * (size_t)(argc + 1));
        int cargc = 0;
        for (int i = 0; i < argc; i++) {
            if (i && !strcasecmp(argv[i], "-sfo")) {
                free(cargv);
                err_printf("%s: -sfo (UEFI Shell format) is not supported: use -data\n", name);
                return last_status = RC_USAGE;
            }
            if (i && !strcasecmp(argv[i], "-data"))
                want_data = true;
            else
                cargv[cargc++] = argv[i];
        }
        cargv[cargc] = NULL;
        if (want_data && !(c->flags & CMD_DATA)) {
            free(cargv);
            err_printf("%s: -data is not supported by this command\n", name);
            return last_status = RC_USAGE;
        }
        bool saved_data = out_data_mode();
        out_set_data_mode(want_data);
        con_clear_break();
        rc = c->fn(cargc, cargv);
        out_set_data_mode(saved_data);
        free(cargv); /* the strings belong to the caller's argv */
        if (con_break()) {
            con_clear_break();
            err_printf("^C\n");
            if (!rc)
                rc = RC_BREAK;
        }
        return last_status = rc;
    }
    char *path = shell_find_executable(name);
    if (!path) {
        err_printf("%s: command not found\n", name);
        return last_status = RC_NOTFOUND;
    }
    char *saved = argv[0];
    argv[0] = path;
    if (has_ext(path, SCRIPT_EXT))
        rc = basic_run_file(path, argc, argv);
    else
        rc = platform_run_image(path, argc, argv);
    argv[0] = saved;
    free(path);
    return last_status = rc;
}

int shell_exec_line(const char *line)
{
    int argc;
    const char *err;
    char *redir = NULL;
    bool append = false;
    char **argv = split_ex(line, &argc, &err, &redir, &append, false);
    if (!argv) {
        err_printf("syntax error: %s\n", err);
        return last_status = RC_USAGE;
    }
    const Cmd *c = argc ? shell_find_cmd(argv[0]) : NULL;
    if (c && (c->flags & CMD_KEEP_QUOTES)) {
        argv_free(argv);
        free(redir);
        argv = split_ex(line, &argc, &err, &redir, &append, true);
        if (!argv) {
            err_printf("syntax error: %s\n", err);
            return last_status = RC_USAGE;
        }
    }
    if (!argc) {
        argv_free(argv);
        free(redir);
        return last_status;
    }
    char *target = NULL;
    if (redir) {
        target = path_resolve(redir);
        if (!target) {
            err_printf("%s: invalid path\n", redir);
            free(redir);
            argv_free(argv);
            return last_status = RC_FAIL;
        }
        int e = out_push_file(target, append);
        if (e) {
            err_printf("%s: %s\n", target, pal_strerror(e));
            free(target);
            free(redir);
            argv_free(argv);
            return last_status = RC_FAIL;
        }
    }
    int rc = shell_exec_argv(argc, argv);
    if (target) {
        int e = out_pop();
        if (e) {
            err_printf("%s: %s\n", target, pal_strerror(e));
            rc = RC_FAIL;
        }
    }
    free(target);
    free(redir);
    argv_free(argv);
    return last_status = rc;
}

/* ---- Interactive shell ---- */

bool shell_exit_requested;
bool shell_script_exit_requested;
int shell_exit_code;

static Interp *repl_interp;

static void banner(void)
{
    int fg, bg;
    pal_con_get_color(&fg, &bg);
    pal_con_set_color(C_WHITE, bg);
    out_printf("%s %s", NESH_NAME, NESH_VERSION);
    pal_con_set_color(fg, bg);
    out_printf(" - New EFI Shell (%s)\n", pal_platform_name());
    out_puts("Type 'help' for the commands, 'help basic' for the scripting language.\n");
    if (secure_boot_active())
        out_puts("Secure Boot is active: low-level hardware writes are disabled.\n");
    out_puts("\n");
}

static void print_prompt_path(Sbuf *p)
{
    sb_printf(p, "%s> ", shell_cwd()[0] ? shell_cwd() : "nesh");
}

static void run_startup(void)
{
    int bv = pal_boot_volume();
    if (bv < 0)
        return;
    char *path = xasprintf("%s:\\startup%s", pal_volume(bv)->name, SCRIPT_EXT);
    PalStat st;
    if (pal_stat(path, &st) != PAL_OK) {
        free(path);
        return;
    }
    bool skip = false;
    if (pal_con_interactive()) {
        pal_con_raw(true);
        for (int s = 3; s > 0 && !skip; s--) {
            out_printf("\rRunning %s in %d s (ESC to skip, any other key to start now) ", path, s);
            PalKey k;
            if (pal_con_read_key(&k, 1000)) {
                skip = k.scan == KEY_ESC || k.ch == 27 || k.ch == 3;
                break;
            }
        }
        pal_con_raw(false);
        out_puts("\n");
    }
    if (!skip) {
        char *argv[] = { path, NULL };
        basic_run_file(path, 1, argv);
    }
    free(path);
}

static int repl(void)
{
    repl_interp = interp_new(0, NULL, true);
    Sbuf pending;
    sb_init(&pending);
    while (!shell_exit_requested) {
        int col, row;
        pal_con_get_cursor(&col, &row);
        if (col > 0 && !pal_con_ansi())
            pal_con_write("\n", 1); /* the last output did not end with a newline */
        Sbuf prompt;
        sb_init(&prompt);
        if (pending.len)
            sb_adds(&prompt, "... ");
        else
            print_prompt_path(&prompt);
        char *line = lineedit_read(prompt.s, true);
        sb_free(&prompt);
        con_clear_break();
        if (!line) {
            if (!pal_con_interactive()) /* end of input on the host */
                break;
            if (pending.len) {
                sb_clear(&pending);
                out_puts("(block cancelled)\n");
            }
            continue;
        }
        if (pending.len) {
            sb_adds(&pending, "\n");
            sb_adds(&pending, line);
            free(line);
            bool incomplete;
            int rc = interp_exec_interactive(repl_interp, pending.s, &incomplete);
            if (!incomplete) {
                sb_clear(&pending);
                last_status = rc;
            }
            continue;
        }
        char *t = line;
        while (*t == ' ' || *t == '\t')
            t++;
        if (!*t) {
            free(line);
            continue;
        }
        int rc;
        if (interp_is_basic_line(repl_interp, t)) {
            bool incomplete;
            rc = interp_exec_interactive(repl_interp, t, &incomplete);
            if (incomplete) {
                sb_adds(&pending, t);
                free(line);
                continue;
            }
        } else {
            rc = shell_exec_line(t);
            interp_set_err(repl_interp, rc);
        }
        last_status = rc;
        free(line);
    }
    sb_free(&pending);
    interp_free(repl_interp);
    repl_interp = NULL;
    return shell_exit_code;
}

static void usage(void)
{
    out_puts("usage: nesh [-n] [-c CODE] [-k SCRIPT...] [SCRIPT [ARGS...]]\n"
             "  -c CODE   run BASIC code and exit\n"
             "  -k        check the syntax of scripts without running them\n"
             "  -n        do not run startup" SCRIPT_EXT "\n");
}

int nesh_main(int argc, char **argv)
{
    basic_core_funcs_init();
    cmds_core_init();
    cmds_fs_init();
    cmds_text_init();
    cmds_sys_init();
    cmds_util_init();
    cmds_edit_init();
    env_cmds_init();
    alias_cmds_init();
    platform_cmds_init();

    int bv = pal_boot_volume();
    if (bv < 0 && pal_volume_count() > 0)
        bv = 0;
    if (bv >= 0) {
        char *root = xasprintf("%s:\\", pal_volume(bv)->name);
        shell_chdir(root);
        free(root);
    }
    env_init();
    alias_init();

    bool no_startup = false;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) {
            no_startup = true;
        } else if (!strcmp(argv[i], "-c")) {
            if (i + 1 >= argc) {
                usage();
                return RC_USAGE;
            }
            Program prog;
            const char *code = argv[i + 1];
            if (!parse_program(code, strlen(code), &prog)) {
                err_printf("syntax error: %s\n", prog.err);
                return RC_FAIL;
            }
            Interp *in = interp_new(argc - (i + 1), argv + i + 1, false);
            int rc = interp_run(in, &prog, "-c");
            interp_free(in);
            program_free(&prog);
            return rc;
        } else if (!strcmp(argv[i], "-k")) {
            /* syntax check only: parse every script, run nothing */
            int rc = i + 1 < argc ? RC_OK : RC_USAGE;
            for (i++; i < argc; i++) {
                char *path = path_resolve(argv[i]), *src = NULL;
                size_t len;
                int e = path ? file_read_text(path, &src, &len) : PAL_ENOENT;
                Program prog;
                if (e) {
                    err_printf("%s: %s\n", argv[i], pal_strerror(e));
                    rc = RC_FAIL;
                } else if (!parse_program(src, len, &prog)) {
                    err_printf("%s:%d: syntax error: %s\n", path, prog.err_line, prog.err);
                    rc = RC_FAIL;
                } else {
                    program_free(&prog);
                }
                free(src);
                free(path);
            }
            if (rc == RC_USAGE)
                usage();
            return rc;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return RC_OK;
        } else {
            break;
        }
    }
    if (i < argc) {
        char *path = path_resolve(argv[i]);
        if (!path) {
            err_printf("%s: invalid path\n", argv[i]);
            return RC_FAIL;
        }
        char *saved = argv[i];
        argv[i] = path;
        int rc = basic_run_file(path, argc - i, argv + i);
        argv[i] = saved;
        free(path);
        return rc;
    }
    if (pal_con_interactive())
        banner();
    if (!no_startup)
        run_startup();
    return repl();
}
