/* File and volume commands. */
#include "../core/shell.h"

/* Resolves operands, expanding wildcards. Returns a NULL-terminated list of
 * canonical paths, or NULL after printing an error. With !must_exist, missing
 * paths are kept and patterns without matches are skipped (rm -f). */
static char **expand(const char *cmd, int argc, char **argv, int first, int *count, bool must_exist)
{
    char **out = NULL;
    int n = 0;
    for (int i = first; i < argc; i++) {
        char *p = path_resolve(argv[i]);
        if (!p) {
            cmd_err(cmd, "%s: invalid path or volume", argv[i]);
            goto fail;
        }
        if (path_has_wildcards(p)) {
            int m;
            char **list = path_glob(p, &m);
            free(p);
            if (!m) {
                if (!must_exist)
                    continue;
                cmd_err(cmd, "%s: no match", argv[i]);
                goto fail;
            }
            out = xrealloc(out, sizeof(char *) * (n + m + 1));
            for (int k = 0; k < m; k++)
                out[n++] = list[k];
            free(list);
        } else {
            PalStat st;
            if (must_exist && pal_stat(p, &st) != PAL_OK) {
                cmd_err(cmd, "%s: not found", argv[i]);
                free(p);
                goto fail;
            }
            out = xrealloc(out, sizeof(char *) * (n + 2));
            out[n++] = p;
        }
    }
    if (!out)
        out = xmalloc(sizeof(char *));
    out[n] = NULL;
    *count = n;
    return out;
fail:
    for (int k = 0; k < n; k++)
        free(out[k]);
    free(out);
    return NULL;
}

static int str_cmp(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void free_list(char **l)
{
    argv_free(l);
}

static void human_size(uint64_t v, char *buf, size_t n)
{
    const char *u = "BKMGTP";
    int i = 0;
    uint64_t whole = v, frac = 0;
    while (whole >= 1024 && i < 5) {
        frac = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        i++;
    }
    if (i == 0)
        snprintf(buf, n, "%lluB", (unsigned long long)whole);
    else
        snprintf(buf, n, "%llu.%llu%c", (unsigned long long)whole, (unsigned long long)frac, u[i]);
}

/* ---- map ---- */

/* Block devices ("blkN:"), provided by the platform (none on the host). */
void platform_map_blocks(bool verbose);
bool platform_map_target(const char *target, char *volname, size_t n); /* handle or blkN -> fsN */

static void refresh_volumes(void)
{
    char *old = xstrdup(shell_cwd());
    pal_volumes_refresh();
    if (shell_chdir(old) != PAL_OK) {
        int bv = pal_boot_volume();
        if (bv < 0 && pal_volume_count())
            bv = 0;
        if (bv >= 0) {
            char *root = xasprintf("%s:\\", pal_volume(bv)->name);
            shell_chdir(root);
            free(root);
        }
    }
    free(old);
}

static int cmd_map(int argc, char **argv)
{
    bool refresh = false, verbose = false, del = false;
    const char *type = NULL;
    char *ops[2];
    int nops = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcasecmp(a, "-r") || !strcasecmp(a, "-u"))
            refresh = true;
        else if (!strcasecmp(a, "-v") || !strcasecmp(a, "-f") || !strcasecmp(a, "-c"))
            verbose = true;
        else if (!strcasecmp(a, "-d"))
            del = true;
        else if (!strcasecmp(a, "-b") || !strcasecmp(a, "-sfo"))
            ; /* accepted */
        else if (!strcasecmp(a, "-t") && i + 1 < argc)
            type = argv[++i];
        else if (a[0] == '-' || nops == 2)
            return cmd_usage("map");
        else
            ops[nops++] = argv[i];
    }
    if (del) {
        if (nops != 1)
            return cmd_usage("map");
        char *n = xstrdup(ops[0]);
        size_t l = strlen(n);
        if (l && n[l - 1] == ':')
            n[l - 1] = 0;
        bool is_volume = false;
        for (int k = 0; k < pal_volume_count(); k++)
            is_volume |= !strcasecmp(pal_volume(k)->name, n);
        if (is_volume) {
            free(n);
            return cmd_err("map", "%s is a volume, not an extra name", ops[0]);
        }
        map_alias_set(n, NULL);
        free(n);
        return RC_OK;
    }
    if (nops == 2) {
        /* map NAME TARGET: TARGET is fsN:, blkN: or a handle number */
        char vol[32] = "";
        char *t = path_resolve(ops[1]);
        if (t && strchr(t, ':')) {
            snprintf(vol, sizeof(vol), "%.*s", (int)(strchr(t, ':') - t), t);
        } else if (!platform_map_target(ops[1], vol, sizeof(vol))) {
            free(t);
            return cmd_err("map", "%s: not a volume, block device or handle with a file system", ops[1]);
        }
        free(t);
        char *n = xstrdup(ops[0]);
        size_t l = strlen(n);
        if (l && n[l - 1] == ':')
            n[l - 1] = 0;
        bool ok = map_alias_set(n, vol);
        free(n);
        if (!ok)
            return cmd_err("map", "%s is already the name of a volume", ops[0]);
        return RC_OK;
    }
    if (refresh)
        refresh_volumes();
    bool want_fs = !type || !strcasecmp(type, "fs") || !strcasecmp(type, "hd") || !strcasecmp(type, "fp");
    bool want_blk = !type || !strcasecmp(type, "blk");
    if (want_fs && out_data_mode()) {
        for (int k = 0; k < pal_volume_count(); k++) {
            const PalVolume *v = pal_volume(k);
            if (nops == 1 && strncasecmp(ops[0], v->name, strlen(v->name)))
                continue;
            char *aliases = map_aliases_of(v->name);
            data_record();
            data_field("kind", "volume");
            data_field("volume", "%s", v->name);
            data_field("label", "%s", v->label);
            data_field("size", "%llu", (unsigned long long)v->size);
            data_field("free", "%llu", (unsigned long long)v->free);
            data_field("readonly", "%s", v->readonly ? "yes" : "no");
            data_field("removable", "%s", v->removable ? "yes" : "no");
            data_field("boot", "%s", k == pal_boot_volume() ? "yes" : "no");
            data_field("aliases", "%s", aliases);
            data_field("devpath", "%s", v->devpath);
            free(aliases);
        }
    } else if (want_fs) {
        if (!pal_volume_count()) {
            out_puts("No file system volumes found.\n");
        } else {
            out_printf("%-6s %-16s %9s %9s  %s\n", "Volume", "Label", "Size", "Free", "Flags");
            for (int k = 0; k < pal_volume_count(); k++) {
                const PalVolume *v = pal_volume(k);
                if (nops == 1 && strncasecmp(ops[0], v->name, strlen(v->name)))
                    continue;
                char size[16], fr[16];
                human_size(v->size, size, sizeof(size));
                human_size(v->free, fr, sizeof(fr));
                char *aliases = map_aliases_of(v->name);
                out_printf("%-6s %-16s %9s %9s  %s%s%s%s%s\n", v->name, v->label[0] ? v->label : "-",
                           v->size ? size : "-", v->size ? fr : "-", v->readonly ? "ro " : "",
                           v->removable ? "removable " : "", k == pal_boot_volume() ? "boot " : "",
                           *aliases ? "also " : "", aliases);
                free(aliases);
                if (verbose && v->devpath[0])
                    out_printf("       %s\n", v->devpath);
            }
        }
    }
    if (want_blk && nops == 0)
        platform_map_blocks(verbose);
    return RC_OK;
}

/* ---- cd / pwd ---- */

static int cmd_cd(int argc, char **argv)
{
    if (argc == 1) {
        out_printf("%s\n", shell_cwd());
        return RC_OK;
    }
    if (argc > 2)
        return cmd_usage("cd");
    int e = shell_chdir(argv[1]);
    if (e)
        return cmd_perr("cd", argv[1], e);
    return RC_OK;
}

static int cmd_pwd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    out_printf("%s\n", shell_cwd());
    return RC_OK;
}

/* ---- ls / dir ---- */

typedef struct {
    char *name;
    PalStat st;
} Entry;

static int entry_cmp(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->st.is_dir != y->st.is_dir)
        return x->st.is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void print_entry_name(const Entry *e)
{
    size_t l = strlen(e->name);
    bool exec = !e->st.is_dir && l > 4 && (!strcasecmp(e->name + l - 4, ".efi") || !strcasecmp(e->name + l - 4, SCRIPT_EXT));
    int fg, bg;
    pal_con_get_color(&fg, &bg);
    if (e->st.is_dir)
        out_color(C_LIGHTBLUE, bg);
    else if (exec)
        out_color(C_LIGHTGREEN, bg);
    out_puts(e->name);
    if (e->st.is_dir || exec)
        out_color(fg, bg);
    if (e->st.is_dir)
        out_puts("\\");
}

static void data_file(const char *path, const PalStat *st)
{
    const PalTime *t = &st->mtime;
    data_record();
    data_field("name", "%s", path_basename(path));
    data_field("path", "%s", path);
    data_field("type", "%s", st->is_dir ? "dir" : "file");
    data_field("size", "%llu", (unsigned long long)st->size);
    data_field("modified", "%04d-%02d-%02d %02d:%02d:%02d", t->year, t->month, t->day, t->hour, t->min, t->sec);
    data_field("readonly", "%s", st->attr & PAL_ATTR_READONLY ? "yes" : "no");
    data_field("hidden", "%s", st->attr & PAL_ATTR_HIDDEN ? "yes" : "no");
    data_field("system", "%s", st->attr & PAL_ATTR_SYSTEM ? "yes" : "no");
    data_field("archive", "%s", st->attr & PAL_ATTR_ARCHIVE ? "yes" : "no");
}

static void print_long(const Entry *e)
{
    char attr[6] = "-----";
    if (e->st.is_dir) attr[0] = 'd';
    if (e->st.attr & PAL_ATTR_READONLY) attr[1] = 'r';
    if (e->st.attr & PAL_ATTR_HIDDEN) attr[2] = 'h';
    if (e->st.attr & PAL_ATTR_SYSTEM) attr[3] = 's';
    if (e->st.attr & PAL_ATTR_ARCHIVE) attr[4] = 'a';
    const PalTime *t = &e->st.mtime;
    out_printf("%04d-%02d-%02d %02d:%02d  %s  ", t->year, t->month, t->day, t->hour, t->min, attr);
    if (e->st.is_dir)
        out_printf("%12s  ", "<DIR>");
    else
        out_printf("%12llu  ", (unsigned long long)e->st.size);
    print_entry_name(e);
    out_puts("\n");
}

static void print_columns(Entry *ents, int n)
{
    int cols, rows;
    pal_con_size(&cols, &rows);
    if (!out_is_console()) {
        for (int i = 0; i < n; i++) {
            out_puts(ents[i].name);
            out_puts(ents[i].st.is_dir ? "\\\n" : "\n");
        }
        return;
    }
    int w = 1;
    for (int i = 0; i < n; i++)
        w = MAX(w, (int)utf8_len(ents[i].name, strlen(ents[i].name)) + (ents[i].st.is_dir ? 1 : 0) + 2);
    int per = MAX(1, (cols - 1) / w);
    int nrows = (n + per - 1) / per;
    for (int r = 0; r < nrows; r++) {
        for (int c = 0; c < per; c++) {
            int i = c * nrows + r;
            if (i >= n)
                continue;
            print_entry_name(&ents[i]);
            int used = (int)utf8_len(ents[i].name, strlen(ents[i].name)) + (ents[i].st.is_dir ? 1 : 0);
            if (c + 1 < per && (c + 1) * nrows + r < n)
                for (int k = used; k < w; k++)
                    out_puts(" ");
        }
        out_puts("\n");
    }
}

typedef struct {
    bool lng;       /* -l */
    bool all;       /* -a: hidden and system files too */
    bool recursive; /* -r */
    uint64_t need;  /* -aXYZ: attributes the entries must have */
    bool need_dir;
} LsOpts;

static bool entry_wanted(const LsOpts *o, const PalStat *st)
{
    if (o->need || o->need_dir)
        return (st->attr & o->need) == o->need && (!o->need_dir || st->is_dir);
    return o->all || !(st->attr & (PAL_ATTR_HIDDEN | PAL_ATTR_SYSTEM));
}

static int list_dir(const char *dir, const char *pattern, const LsOpts *o, bool header)
{
    PalDir *d;
    int e = pal_opendir(dir, &d);
    if (e)
        return cmd_perr("ls", dir, e);
    Entry *ents = NULL;
    int n = 0;
    char **subdirs = NULL;
    int nsub = 0;
    PalStat st;
    while ((e = pal_readdir(d, &st)) == 1) {
        if (o->recursive && st.is_dir) {
            subdirs = xrealloc(subdirs, sizeof(char *) * (nsub + 1));
            subdirs[nsub++] = path_join(dir, st.name);
        }
        if ((pattern && !glob_match(pattern, st.name, true)) || !entry_wanted(o, &st)) {
            free(st.name);
            continue;
        }
        ents = xrealloc(ents, sizeof(Entry) * (n + 1));
        ents[n].name = st.name;
        ents[n].st = st;
        n++;
        if (con_break())
            break;
    }
    pal_closedir(d);
    if (n)
        qsort(ents, n, sizeof(Entry), entry_cmp);
    if ((header || o->recursive) && !out_data_mode())
        out_printf("%s:\n", dir);
    uint64_t total = 0;
    int files = 0, dirs = 0;
    if (out_data_mode()) {
        for (int i = 0; i < n; i++) {
            char *p = path_join(dir, ents[i].name);
            data_file(p, &ents[i].st);
            free(p);
        }
    } else if (o->lng) {
        for (int i = 0; i < n && !con_break(); i++) {
            print_long(&ents[i]);
            if (ents[i].st.is_dir)
                dirs++;
            else
                files++, total += ents[i].st.size;
        }
        out_printf("%d file%s, %d dir%s, %llu bytes\n", files, files == 1 ? "" : "s", dirs, dirs == 1 ? "" : "s",
                   (unsigned long long)total);
    } else {
        print_columns(ents, n);
    }
    for (int i = 0; i < n; i++)
        free(ents[i].name);
    free(ents);
    int rc = e < 0 ? cmd_perr("ls", dir, e) : RC_OK;
    if (o->recursive && nsub)
        qsort(subdirs, nsub, sizeof(char *), str_cmp);
    for (int i = 0; i < nsub; i++) {
        if (!con_break()) {
            if (!out_data_mode())
                out_puts("\n");
            rc |= list_dir(subdirs[i], pattern, o, true);
        }
        free(subdirs[i]);
    }
    free(subdirs);
    return rc;
}

static int do_ls(int argc, char **argv, bool force_long)
{
    LsOpts o = { 0 };
    o.lng = force_long;
    char **ops = xmalloc(sizeof(char *) * (argc + 1));
    int nops = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) {
            ops[nops++] = argv[i];
        } else if (!strcmp(a, "-l")) {
            o.lng = true;
        } else if (!strcmp(a, "-r")) {
            o.recursive = true;
        } else if (!strcmp(a, "-b")) {
            /* page break: accepted */
        } else if (a[1] == 'a' || a[1] == 'A') {
            if (!a[2])
                o.all = true;
            for (const char *c = a + 2; *c; c++) {
                switch (tolower((uint8_t)*c)) {
                case 'a': o.need |= PAL_ATTR_ARCHIVE; break;
                case 's': o.need |= PAL_ATTR_SYSTEM; break;
                case 'h': o.need |= PAL_ATTR_HIDDEN; break;
                case 'r': o.need |= PAL_ATTR_READONLY; break;
                case 'd': o.need_dir = true; break;
                default:
                    free(ops);
                    return cmd_err(argv[0], "unknown attribute '%c' in %s (a, s, h, r, d)", *c, a);
                }
            }
        } else {
            free(ops);
            err_printf("%s: unknown option %s\n", argv[0], a);
            return cmd_usage(argv[0]);
        }
    }
    int rc = RC_OK;
    if (!nops) {
        rc = list_dir(shell_cwd(), NULL, &o, false);
        free(ops);
        return rc;
    }
    bool header = nops > 1;
    for (int i = 0; i < nops; i++) {
        char *p = path_resolve(ops[i]);
        if (!p) {
            rc = cmd_err("ls", "%s: invalid path or volume", ops[i]);
            continue;
        }
        if (path_has_wildcards(p)) {
            char *dir = path_dirname(p);
            LsOpts oo = o;
            if (!o.need && !o.need_dir)
                oo.all = true;
            rc |= list_dir(dir, path_basename(p), &oo, header);
            free(dir);
        } else {
            PalStat st;
            int e = pal_stat(p, &st);
            if (e) {
                rc = cmd_perr("ls", ops[i], e);
            } else if (st.is_dir) {
                rc |= list_dir(p, NULL, &o, header);
            } else if (out_data_mode()) {
                data_file(p, &st);
            } else {
                Entry en = { (char *)path_basename(p), st };
                if (o.lng)
                    print_long(&en);
                else {
                    print_entry_name(&en);
                    out_puts("\n");
                }
            }
        }
        free(p);
    }
    free(ops);
    return rc;
}

static int cmd_ls(int argc, char **argv) { return do_ls(argc, argv, false); }
static int cmd_dir(int argc, char **argv) { return do_ls(argc, argv, true); }

/* ---- cat ---- */

/* more: cat with paging, even in a script or after "set pager off". */
static int cmd_more(int argc, char **argv);

static int cmd_cat(int argc, char **argv)
{
    /* -a / -u (UEFI Shell: force ASCII / UCS-2): the encoding is detected anyway */
    int first = 1;
    while (first < argc && (!strcasecmp(argv[first], "-a") || !strcasecmp(argv[first], "-u")))
        first++;
    if (first >= argc)
        return cmd_usage(argv[0]);
    int n;
    char **list = expand(argv[0], argc, argv, first, &n, true);
    if (!list)
        return RC_FAIL;
    int rc = RC_OK;
    for (int i = 0; i < n && !con_break(); i++) {
        char *text;
        size_t len;
        int e = file_read_text(list[i], &text, &len);
        if (e) {
            rc = cmd_perr(argv[0], list[i], e);
            continue;
        }
        out_write(text, len);
        if (len && text[len - 1] != '\n' && out_is_console())
            out_puts("\n");
        free(text);
    }
    free_list(list);
    return rc;
}

/* ---- cp / mv / rm ---- */

static int copy_file(const char *cmd, const char *src, const char *dst)
{
    if (!strcasecmp(src, dst))
        return cmd_err(cmd, "%s: source and destination are the same file", src);
    PalFile *in, *out;
    int e = pal_open(src, PAL_O_READ, &in);
    if (e)
        return cmd_perr(cmd, src, e);
    e = pal_open(dst, PAL_O_WRITE | PAL_O_CREATE | PAL_O_TRUNC, &out);
    if (e) {
        pal_close(in);
        return cmd_perr(cmd, dst, e);
    }
    size_t bufsz = 256 * 1024;
    char *buf = xmalloc(bufsz);
    for (;;) {
        size_t got;
        e = pal_read(in, buf, bufsz, &got);
        if (e || !got)
            break;
        e = pal_write(out, buf, got);
        if (e)
            break;
        if (con_break()) {
            e = PAL_EABORT;
            break;
        }
    }
    free(buf);
    pal_close(in);
    int e2 = pal_close(out);
    if (!e)
        e = e2;
    if (e) {
        pal_remove(dst);
        return cmd_perr(cmd, dst, e);
    }
    return RC_OK;
}

static bool is_inside(const char *parent, const char *child)
{
    size_t l = strlen(parent);
    return !strncasecmp(parent, child, l) && (child[l] == '\\' || child[l] == 0 || parent[l - 1] == '\\');
}

static int copy_tree(const char *cmd, const char *src, const char *dst, bool recursive)
{
    PalStat st;
    int e = pal_stat(src, &st);
    if (e)
        return cmd_perr(cmd, src, e);
    if (!st.is_dir)
        return copy_file(cmd, src, dst);
    if (!recursive)
        return cmd_err(cmd, "%s is a directory (use -r)", src);
    if (is_inside(src, dst))
        return cmd_err(cmd, "cannot copy %s into itself", src);
    e = pal_mkdir(dst);
    if (e && e != PAL_EEXIST)
        return cmd_perr(cmd, dst, e);
    PalDir *d;
    e = pal_opendir(src, &d);
    if (e)
        return cmd_perr(cmd, src, e);
    int rc = RC_OK;
    PalStat es;
    while ((e = pal_readdir(d, &es)) == 1) {
        char *s = path_join(src, es.name), *t = path_join(dst, es.name);
        rc |= copy_tree(cmd, s, t, true);
        free(s);
        free(t);
        free(es.name);
        if (con_break()) {
            rc |= RC_FAIL; /* incomplete copy */
            break;
        }
    }
    pal_closedir(d);
    if (e < 0)
        rc |= cmd_perr(cmd, src, e);
    return rc;
}

static int remove_tree(const char *cmd, const char *path, bool recursive, bool quiet_missing)
{
    PalStat st;
    int e = pal_stat(path, &st);
    if (e)
        return quiet_missing && e == PAL_ENOENT ? RC_OK : cmd_perr(cmd, path, e);
    if (st.is_dir) {
        if (!recursive)
            return cmd_err(cmd, "%s is a directory (use -r)", path);
        PalDir *d;
        e = pal_opendir(path, &d);
        if (e)
            return cmd_perr(cmd, path, e);
        char **names = NULL;
        int n = 0;
        PalStat es;
        while (pal_readdir(d, &es) == 1) {
            names = xrealloc(names, sizeof(char *) * (n + 1));
            names[n++] = es.name;
        }
        pal_closedir(d);
        int rc = RC_OK;
        for (int i = 0; i < n; i++) {
            char *c = path_join(path, names[i]);
            rc |= remove_tree(cmd, c, true, false);
            free(c);
            free(names[i]);
        }
        free(names);
        if (rc)
            return rc;
    }
    const char *c = strchr(path, ':');
    if (c && c[1] == '\\' && !c[2])
        return cmd_err(cmd, "cannot remove the root directory");
    e = pal_remove(path);
    return e ? cmd_perr(cmd, path, e) : RC_OK;
}

/* Destination for SRC given DST: DST\basename(SRC) if DST is a directory. */
static char *target_for(const char *src, const char *dst, bool dst_is_dir)
{
    return dst_is_dir ? path_join(dst, path_basename(src)) : xstrdup(dst);
}

static int cmd_cp(int argc, char **argv)
{
    bool f[2]; /* r q (q: quiet, accepted: cp never asks) */
    int i = getopts(argc, argv, "rq", f);
    if (i < 0)
        return RC_USAGE;
    if (argc - i < 2)
        return cmd_usage("cp");
    char *dst = path_resolve(argv[argc - 1]);
    if (!dst)
        return cmd_err("cp", "%s: invalid path or volume", argv[argc - 1]);
    int n;
    char **srcs = expand("cp", argc - 1, argv, i, &n, true);
    if (!srcs) {
        free(dst);
        return RC_FAIL;
    }
    PalStat st;
    bool dst_dir = pal_stat(dst, &st) == PAL_OK && st.is_dir;
    int rc = RC_OK;
    if (n > 1 && !dst_dir) {
        rc = cmd_err("cp", "%s: not a directory", dst);
    } else {
        for (int k = 0; k < n && !con_break(); k++) {
            char *t = target_for(srcs[k], dst, dst_dir);
            rc |= copy_tree("cp", srcs[k], t, f[0]);
            free(t);
        }
    }
    free_list(srcs);
    free(dst);
    return rc;
}

static int cmd_mv(int argc, char **argv)
{
    if (argc < 3)
        return cmd_usage("mv");
    char *dst = path_resolve(argv[argc - 1]);
    if (!dst)
        return cmd_err("mv", "%s: invalid path or volume", argv[argc - 1]);
    int n;
    char **srcs = expand("mv", argc - 1, argv, 1, &n, true);
    if (!srcs) {
        free(dst);
        return RC_FAIL;
    }
    PalStat st;
    bool dst_dir = pal_stat(dst, &st) == PAL_OK && st.is_dir;
    int rc = RC_OK;
    if (n > 1 && !dst_dir) {
        rc = cmd_err("mv", "%s: not a directory", dst);
    } else {
        for (int k = 0; k < n; k++) {
            char *t = target_for(srcs[k], dst, dst_dir);
            if (is_inside(srcs[k], t) && strcasecmp(srcs[k], t)) {
                rc |= cmd_err("mv", "cannot move %s into itself", srcs[k]);
                free(t);
                continue;
            }
            PalStat ts;
            char *old = NULL; /* existing target file, set aside until the move succeeded */
            if (pal_stat(t, &ts) == PAL_OK && strcasecmp(srcs[k], t)) {
                if (ts.is_dir) {
                    rc |= cmd_err("mv", "%s already exists", t);
                    free(t);
                    continue;
                }
                for (int j = 0; !old && j < 1000; j++) {
                    old = xasprintf("%s.mv%d", t, j);
                    if (pal_stat(old, &ts) == PAL_OK) {
                        free(old);
                        old = NULL;
                    }
                }
                int e = old ? pal_rename(t, old) : PAL_EEXIST;
                if (e) {
                    rc |= cmd_perr("mv", t, e);
                    free(old);
                    free(t);
                    continue;
                }
            }
            int r = RC_OK;
            int e = pal_rename(srcs[k], t);
            if (e == PAL_ENOTSUP) {
                /* different volume: copy, and delete the source only after a complete copy */
                r = copy_tree("mv", srcs[k], t, true);
                if (r)
                    remove_tree("mv", t, true, true); /* drop the partial copy */
                else
                    r = remove_tree("mv", srcs[k], true, false);
            } else if (e) {
                r = cmd_perr("mv", srcs[k], e);
            }
            if (old) {
                /* on success delete the old target, otherwise put it back if t is free */
                PalStat ns;
                if (!r) {
                    e = pal_remove(old);
                } else if (pal_stat(t, &ns) != PAL_OK) {
                    e = pal_rename(old, t);
                } else {
                    cmd_err("mv", "the old %s is kept as %s", t, old);
                    e = PAL_OK;
                }
                if (e)
                    r |= cmd_perr("mv", old, e);
                free(old);
            }
            rc |= r;
            free(t);
        }
    }
    free_list(srcs);
    free(dst);
    return rc;
}

static int cmd_rm(int argc, char **argv)
{
    bool f[3]; /* r f q */
    int i = getopts(argc, argv, "rfq", f);
    if (i < 0)
        return RC_USAGE;
    if (i >= argc)
        return cmd_usage(argv[0]);
    int rc = RC_OK;
    for (; i < argc; i++) {
        int n;
        char *one[] = { argv[0], argv[i], NULL };
        char **list = expand(argv[0], 2, one, 1, &n, !f[1]);
        if (!list) {
            rc = RC_FAIL;
            continue;
        }
        for (int k = 0; k < n; k++)
            rc |= remove_tree(argv[0], list[k], f[0] || f[2], f[1]); /* -q: UEFI Shell style */
        free_list(list);
    }
    return rc;
}

static int cmd_mkdir(int argc, char **argv)
{
    bool f[1]; /* p */
    int i = getopts(argc, argv, "p", f);
    if (i < 0)
        return RC_USAGE;
    if (i >= argc)
        return cmd_usage(argv[0]);
    int rc = RC_OK;
    for (; i < argc; i++) {
        char *p = path_resolve(argv[i]);
        if (!p) {
            rc = cmd_err(argv[0], "%s: invalid path or volume", argv[i]);
            continue;
        }
        if (f[0]) {
            /* create each missing component */
            char *c = strchr(p, '\\');
            while (c) {
                char *next = strchr(c + 1, '\\');
                if (next)
                    *next = 0;
                if (c[1]) {
                    int e = pal_mkdir(p);
                    PalStat st;
                    if (e != PAL_OK && !(pal_stat(p, &st) == PAL_OK && st.is_dir)) {
                        rc = cmd_perr(argv[0], p, e == PAL_EEXIST ? PAL_ENOTDIR : e);
                        if (next)
                            *next = '\\';
                        break;
                    }
                }
                if (next)
                    *next = '\\';
                c = next;
            }
        } else {
            int e = pal_mkdir(p);
            if (e)
                rc = cmd_perr(argv[0], argv[i], e);
        }
        free(p);
    }
    return rc;
}

static int cmd_rmdir(int argc, char **argv)
{
    if (argc < 2)
        return cmd_usage("rmdir");
    int rc = RC_OK;
    for (int i = 1; i < argc; i++) {
        char *p = path_resolve(argv[i]);
        PalStat st;
        int e = p ? pal_stat(p, &st) : PAL_ENOENT;
        if (!e && !st.is_dir)
            e = PAL_ENOTDIR;
        if (!e)
            e = pal_remove(p);
        if (e)
            rc = cmd_perr("rmdir", argv[i], e);
        free(p);
    }
    return rc;
}

static int touch_one(const char *path, const PalTime *now, bool recursive)
{
    PalStat st;
    int rc = RC_OK;
    if (pal_stat(path, &st) != PAL_OK) {
        PalFile *f;
        int e = pal_open(path, PAL_O_WRITE | PAL_O_CREATE, &f);
        if (e)
            return cmd_perr("touch", path, e);
        pal_close(f);
        return RC_OK;
    }
    int e = pal_set_mtime(path, now);
    if (e)
        rc = cmd_perr("touch", path, e);
    if (recursive && st.is_dir) {
        PalDir *d;
        if (pal_opendir(path, &d) == PAL_OK) {
            PalStat es;
            while (pal_readdir(d, &es) == 1) {
                char *c = path_join(path, es.name);
                rc |= touch_one(c, now, true);
                free(c);
                free(es.name);
            }
            pal_closedir(d);
        }
    }
    return rc;
}

static int cmd_touch(int argc, char **argv)
{
    bool f[1]; /* r */
    int i = getopts(argc, argv, "r", f);
    if (i < 0)
        return RC_USAGE;
    if (i >= argc)
        return cmd_usage("touch");
    PalTime now;
    pal_get_time(&now);
    int rc = RC_OK;
    for (; i < argc; i++) {
        char *p = path_resolve(argv[i]);
        if (!p) {
            rc = cmd_err("touch", "%s: invalid path", argv[i]);
            continue;
        }
        if (path_has_wildcards(p)) {
            int n;
            char **l = path_glob(p, &n);
            for (int k = 0; k < n; k++) {
                rc |= touch_one(l[k], &now, f[0]);
                free(l[k]);
            }
            free(l);
        } else {
            rc |= touch_one(p, &now, f[0]);
        }
        free(p);
    }
    return rc;
}

/* ---- vol ---- */

static int cmd_vol(int argc, char **argv)
{
    const char *label = NULL, *vname = NULL;
    bool clear = false;
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "-n") && i + 1 < argc)
            label = argv[++i];
        else if (!strcasecmp(argv[i], "-d"))
            clear = true;
        else if (argv[i][0] != '-' && !vname)
            vname = argv[i];
        else
            return cmd_usage("vol");
    }
    char *root = NULL;
    if (vname && !strpbrk(vname, ":\\/")) {
        /* "vol fs1": a volume or map name without the colon */
        char *withc = xasprintf("%s:", vname);
        root = path_resolve(withc);
        free(withc);
    }
    if (!root)
        root = path_resolve(vname ? vname : "\\");
    int vi = -1;
    for (int k = 0; root && k < pal_volume_count(); k++) {
        size_t l = strlen(pal_volume(k)->name);
        if (!strncasecmp(root, pal_volume(k)->name, l) && root[l] == ':')
            vi = k;
    }
    free(root);
    if (vi < 0)
        return cmd_err("vol", "%s: no such volume", vname ? vname : "(current)");
    if (label || clear) {
        if (label && (strlen(label) > 11 || strpbrk(label, "%^*+=[]|:;\"<>?/.")))
            return cmd_err("vol", "invalid label (at most 11 characters, no punctuation)");
        int e = pal_volume_set_label(vi, clear ? "" : label);
        if (e)
            return cmd_err("vol", "cannot change the label: %s", pal_strerror(e));
    }
    pal_volumes_refresh();
    const PalVolume *v = pal_volume(vi);
    if (out_data_mode()) {
        data_record();
        data_field("volume", "%s", v->name);
        data_field("label", "%s", v->label);
        data_field("size", "%llu", (unsigned long long)v->size);
        data_field("free", "%llu", (unsigned long long)v->free);
        data_field("readonly", "%s", v->readonly ? "yes" : "no");
        return RC_OK;
    }
    out_printf("Volume %s: %s (%s)\n", v->name, v->label[0] ? v->label : "<no label>", v->readonly ? "ro" : "rw");
    out_printf("  %12llu bytes total disk space\n", (unsigned long long)v->size);
    out_printf("  %12llu bytes available on disk\n", (unsigned long long)v->free);
    return RC_OK;
}

/* ---- attrib ---- */

static void attr_str(const PalStat *st, char out[8])
{
    snprintf(out, 8, "%c%c%c%c%c", st->is_dir ? 'D' : ' ', st->attr & PAL_ATTR_ARCHIVE ? 'A' : ' ',
             st->attr & PAL_ATTR_SYSTEM ? 'S' : ' ', st->attr & PAL_ATTR_HIDDEN ? 'H' : ' ',
             st->attr & PAL_ATTR_READONLY ? 'R' : ' ');
}

static int cmd_attrib(int argc, char **argv)
{
    uint64_t set = 0, clr = 0;
    char **files = xmalloc(sizeof(char *) * (argc + 1));
    int nf = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((a[0] == '+' || a[0] == '-') && a[1] && !a[2] && strchr("asrhASRH", a[1])) {
            uint64_t bit = tolower((uint8_t)a[1]) == 'a' ? PAL_ATTR_ARCHIVE : tolower((uint8_t)a[1]) == 's' ? PAL_ATTR_SYSTEM
                         : tolower((uint8_t)a[1]) == 'h' ? PAL_ATTR_HIDDEN : PAL_ATTR_READONLY;
            if (a[0] == '+')
                set |= bit;
            else
                clr |= bit;
        } else if (a[0] == '-' && a[1]) {
            free(files);
            return cmd_usage("attrib");
        } else {
            files[nf++] = argv[i];
        }
    }
    char *star[] = { "*" };
    char **targets = nf ? files : star;
    int nt = nf ? nf : 1;
    int rc = RC_OK;
    for (int i = 0; i < nt; i++) {
        char *p = path_resolve(targets[i]);
        if (!p) {
            rc = cmd_err("attrib", "%s: invalid path", targets[i]);
            continue;
        }
        int n = 0;
        char **list;
        PalStat st;
        if (path_has_wildcards(p)) {
            list = path_glob(p, &n);
        } else if (!nf || (pal_stat(p, &st) == PAL_OK && st.is_dir && !set && !clr)) {
            /* a directory without changes: show its contents */
            char *pat = path_join(p, "*");
            list = path_glob(pat, &n);
            free(pat);
        } else {
            list = xmalloc(sizeof(char *));
            list[n++] = xstrdup(p);
        }
        free(p);
        for (int k = 0; k < n; k++) {
            if (pal_stat(list[k], &st) != PAL_OK) {
                rc = cmd_err("attrib", "%s: not found", list[k]);
            } else {
                if (set || clr) {
                    uint64_t na = (st.attr & ~clr) | set;
                    int e = pal_set_attr(list[k], na & ~(uint64_t)PAL_ATTR_DIR);
                    if (e)
                        rc = cmd_perr("attrib", list[k], e);
                    else
                        st.attr = na;
                }
                char as[8];
                attr_str(&st, as);
                out_printf("%s  %s\n", as, list[k]);
            }
            free(list[k]);
        }
        free(list);
    }
    free(files);
    return rc;
}

/* ---- comp / cmp ---- */

static int compare_files(const char *cmd, const char *a, const char *b, int64_t max_diffs, int64_t show)
{
    char *pa = path_resolve(a), *pb = path_resolve(b);
    char *da = NULL, *db = NULL;
    size_t la = 0, lb = 0;
    int ea = pa ? file_read_all(pa, &da, &la) : PAL_ENOENT;
    int eb = pb ? file_read_all(pb, &db, &lb) : PAL_ENOENT;
    int rc = RC_OK;
    if (ea || eb) {
        rc = cmd_perr(cmd, ea ? a : b, ea ? ea : eb);
        goto out;
    }
    out_printf("Compare %s to %s\n", pa, pb);
    int64_t diffs = 0;
    size_t n = MIN(la, lb);
    for (size_t i = 0; i < n && diffs < max_diffs;) {
        if (da[i] == db[i]) {
            i++;
            continue;
        }
        size_t start = i;
        while (i < n && da[i] != db[i])
            i++;
        diffs++;
        out_printf("Difference #%lld at offset 0x%zx (%zu bytes)\n", (long long)diffs, start, i - start);
        for (int f = 0; f < 2; f++) {
            const char *d = f ? db : da;
            out_printf("  %s:", f ? "file2" : "file1");
            for (size_t k = start; k < n && k < start + (size_t)show; k++)
                out_printf(" %02x", (uint8_t)d[k]);
            out_puts("  *");
            for (size_t k = start; k < n && k < start + (size_t)show; k++)
                out_printf("%c", (uint8_t)d[k] >= 0x20 && (uint8_t)d[k] < 0x7f ? d[k] : '.');
            out_puts("*\n");
        }
    }
    if (la != lb && diffs < max_diffs) {
        diffs++;
        out_printf("Difference #%lld: sizes differ (%zu and %zu bytes)\n", (long long)diffs, la, lb);
    }
    if (diffs) {
        out_puts("[difference(s) encountered]\n");
        rc = RC_FAIL;
    } else {
        out_puts("[no differences encountered]\n");
    }
out:
    free(da);
    free(db);
    free(pa);
    free(pb);
    return rc;
}

static int cmd_comp(int argc, char **argv)
{
    int64_t max_diffs = 10, show = 16;
    char *ops[2];
    int nops = 0;
    for (int i = 1; i < argc; i++) {
        if ((!strcasecmp(argv[i], "-n") || !strcasecmp(argv[i], "-s")) && i + 1 < argc) {
            int64_t v;
            if (!strcasecmp(argv[i + 1], "all") && argv[i][1] == 'n')
                v = INT64_MAX;
            else if (!parse_int(argv[i + 1], &v) || v < 1)
                return cmd_usage(argv[0]);
            if (tolower((uint8_t)argv[i][1]) == 'n')
                max_diffs = v;
            else
                show = v;
            i++;
        } else if (argv[i][0] == '-' || nops == 2) {
            return cmd_usage(argv[0]);
        } else {
            ops[nops++] = argv[i];
        }
    }
    if (nops != 2)
        return cmd_usage(argv[0]);
    if (!strcasecmp(argv[0], "cmp"))
        max_diffs = 1;
    return compare_files(argv[0], ops[0], ops[1], max_diffs, show);
}

/* ---- setsize ---- */

static int cmd_setsize(int argc, char **argv)
{
    int64_t size;
    if (argc < 3 || !parse_int(argv[1], &size) || size < 0)
        return cmd_usage("setsize");
    int rc = RC_OK;
    for (int i = 2; i < argc; i++) {
        char *p = path_resolve(argv[i]);
        PalStat st;
        if (p && pal_stat(p, &st) != PAL_OK) { /* the UEFI Shell creates missing files */
            PalFile *f;
            if (pal_open(p, PAL_O_WRITE | PAL_O_CREATE, &f) == PAL_OK)
                pal_close(f);
        }
        int e = p ? pal_set_size(p, (uint64_t)size) : PAL_ENOENT;
        if (e)
            rc = cmd_perr("setsize", argv[i], e);
        free(p);
    }
    return rc;
}

static int cmd_stat(int argc, char **argv)
{
    if (argc < 2)
        return cmd_usage("stat");
    int rc = RC_OK;
    for (int i = 1; i < argc; i++) {
        char *p = path_resolve(argv[i]);
        PalStat st;
        int e = p ? pal_stat(p, &st) : PAL_ENOENT;
        if (e) {
            rc = cmd_perr("stat", argv[i], e);
        } else if (out_data_mode()) {
            data_file(p, &st);
        } else {
            const PalTime *t = &st.mtime;
            out_printf("path:     %s\n", p);
            out_printf("type:     %s\n", st.is_dir ? "directory" : "file");
            if (!st.is_dir)
                out_printf("size:     %llu\n", (unsigned long long)st.size);
            out_printf("modified: %04d-%02d-%02d %02d:%02d:%02d\n", t->year, t->month, t->day, t->hour, t->min, t->sec);
            out_printf("attrs:   %s%s%s%s\n", st.attr & PAL_ATTR_READONLY ? " readonly" : "",
                       st.attr & PAL_ATTR_HIDDEN ? " hidden" : "", st.attr & PAL_ATTR_SYSTEM ? " system" : "",
                       st.attr & PAL_ATTR_ARCHIVE ? " archive" : "");
        }
        free(p);
    }
    return rc;
}

static int cmd_more(int argc, char **argv)
{
    out_paging(true);
    return cmd_cat(argc, argv);
}

static const Cmd fs_cmds[] = {
    { "map", cmd_map, "map [-r] [-v] [-t fs|blk] | map NAME TARGET | map -d NAME",
      "List volumes and block devices; give volumes extra names",
      "  map                 volumes (fsN:) and block devices (blkN:)\n"
      "  map -r              rescan the devices (also -u)\n"
      "  map usb fs1:        fs1: can also be called usb:\n"
      "                      (TARGET: fsN:, blkN: or a handle)\n"
      "  map -d usb          remove an extra name\n", CMD_DATA },
    { "cd", cmd_cd, "cd [DIR | fsN:]", "Change the current directory",
      "  cd                  print the current directory\n"
      "  cd DIR              go to DIR (relative, or absolute from the root)\n"
      "  cd fs1:             go to the root of volume fs1:\n"
      "  cd ..               go to the parent directory\n"
      "Both \\ and / work as separators. DIR must be an existing directory.\n" },
    { "pwd", cmd_pwd, "pwd", "Print the current directory",
      "  Prints the full path, e.g. fs0:\\efi\\boot (same as cd with no arguments).\n" },
    { "ls", cmd_ls, "ls [-l] [-r] [-a[ashrd]] [PATH | PATTERN...]",
      "List files and directories",
      "  ls                    names in the current directory\n"
      "  ls -l fs1:\\efi        details: date, time, attributes, size, name\n"
      "  ls -r                 also every subdirectory\n"
      "  ls *.efi              only matching names\n"
      "  -a                    also hidden and system files\n"
      "  -aXY                  only entries with all the attributes X, Y...:\n"
      "                        d directory, r read-only, h hidden, s system,\n"
      "                        a archive (e.g. ls -ad lists directories)\n"
      "Directory names end with \\ (and are blue; programs .efi/.nsb are green).\n"
      "Hidden and system files are left out unless -a is given or a pattern\n"
      "names them. -b pages the output (see help more). dir is the same as\n"
      "ls -l.\n"
      "With -data: name, path, type, size, modified, readonly, hidden, system,\n"
      "archive (one record per entry).\n", CMD_DATA },
    { "dir", cmd_dir, "dir [-r] [-a[ashrd]] [PATH | PATTERN...]", "List files with details (same as ls -l)",
      "  dir                   current directory\n"
      "  dir -r fs0:\\efi       also all subdirectories\n"
      "  dir *.efi             only matching names (hidden files included)\n"
      "Each line: date, time, attributes (d dir, r read-only, h hidden, s system,\n"
      "a archive), size and name; a total line ends each directory. Hidden and\n"
      "system files are shown only with -a. -l is accepted and has no effect;\n"
      "-b pages the output. Other options as for ls.\n"
      "With -data: name, path, type, size, modified, readonly, hidden, system,\n"
      "archive.\n", CMD_DATA },
    { "more", cmd_more, "more FILE...", "Print files one screen at a time",
      "  more log.txt        stop at every screenful: Enter one line,\n"
      "                      Space one page, q stops\n"
      "Same as cat, with paging always on (cat pages too when you type it at\n"
      "the prompt). Any command pages with -b; 'set pager off' turns the\n"
      "automatic paging of the prompt off.\n" },
    { "cat", cmd_cat, "cat [-a|-u] FILE...", "Print files (UTF-8 or UCS-2 text, detected automatically)",
      "  cat readme.txt            print a file\n"
      "  cat fs0:\\logs\\*.txt       print all matching files, one after another\n"
      "Files may be UTF-8 (with or without BOM) or UCS-2 little-endian with BOM;\n"
      "carriage returns are removed. -a and -u (UEFI Shell: force ASCII or\n"
      "UCS-2) are accepted before the files and ignored. Ctrl-C stops.\n" },
    { "type", cmd_cat, "type [-a|-u] FILE...", "Print files (same as cat)",
      "  Same as cat (UEFI Shell name).\n" },
    { "cp", cmd_cp, "cp [-r] [-q] SRC... DST", "Copy files (-r directories too)",
      "  -r        copy directories and all their contents\n"
      "  -q        accepted for UEFI Shell compatibility (cp never asks)\n"
      "  cp a.txt b.txt               copy a file\n"
      "  cp *.efi fs1:\\tools          copy several files into a directory\n"
      "  cp -r fs0:\\efi fs1:\\backup   copy a whole folder\n"
      "If DST is an existing directory, each SRC is copied into it; with several\n"
      "sources DST must be a directory. Existing files are overwritten without\n"
      "asking; with -r, existing directories are merged. A directory cannot be\n"
      "copied into itself. After an error or Ctrl-C the partial file is deleted.\n" },
    { "mv", cmd_mv, "mv SRC... DST", "Move or rename files and directories",
      "  mv old.txt new.txt           rename a file\n"
      "  mv *.log fs0:\\logs           move files into a directory\n"
      "  mv fs0:\\tools fs1:\\          move a directory to another volume\n"
      "If DST is an existing directory, each SRC is moved into it; with several\n"
      "sources DST must be a directory. An existing file at the target is\n"
      "replaced without asking, but only once the move has succeeded (if not,\n"
      "it is put back); an existing directory is an error. Between volumes, mv\n"
      "copies and deletes the source only after a complete copy (a partial copy\n"
      "is removed). There are no options.\n" },
    { "rm", cmd_rm, "rm [-r] [-f] [-q] PATH...", "Delete files (-r or -q: directories too, -f ignore missing)",
      "  -r, -q    also delete directories with all their contents\n"
      "  -f        no error for paths or patterns that match nothing\n"
      "  rm *.tmp                 delete matching files\n"
      "  rm -r fs1:\\old           delete a directory tree\n"
      "Nothing is asked before deleting. Read-only files cannot be deleted:\n"
      "clear the flag with attrib -r first. The root of a volume is never\n"
      "removed. -q is the UEFI Shell form (there rm deletes directories too).\n" },
    { "del", cmd_rm, "del [-r] [-f] [-q] PATH...", "Delete files (same as rm)",
      "  Same as rm (UEFI Shell name).\n" },
    { "mkdir", cmd_mkdir, "mkdir [-p] DIR...", "Create directories (-p also the parents)",
      "  -p        also create missing parents; no error if a directory exists\n"
      "  mkdir -p fs0:\\a\\b\\c       create a whole path in one step\n"
      "Without -p the parent must exist and DIR must not exist yet. A file with\n"
      "the same name as DIR or a parent is always an error.\n" },
    { "md", cmd_mkdir, "md [-p] DIR...", "Create directories (same as mkdir)",
      "  Same as mkdir (UEFI Shell name).\n" },
    { "rmdir", cmd_rmdir, "rmdir DIR...", "Remove empty directories",
      "  rmdir fs0:\\old           remove an empty directory\n"
      "A directory that is not empty is an error (use rm -r to delete a whole\n"
      "tree). Wildcards are not expanded.\n" },
    { "touch", cmd_touch, "touch [-r] FILE...", "Set the modification time to now (creates missing files; -r recursive)",
      "  -r        also every file and directory inside the given directories\n"
      "  touch log.txt            create an empty file, or update its time\n"
      "  touch -r fs0:\\data       update a whole tree\n"
      "Wildcards are allowed; a pattern that matches nothing is skipped silently.\n" },
    { "vol", cmd_vol, "vol [fsN:] [-n LABEL | -d]", "Show a volume, set (-n) or delete (-d) its label",
      "  vol                      current volume: label, total and free space\n"
      "  vol fs1:                 another volume (also fs1, a map name or a path)\n"
      "  vol fs1: -n BOOTDISK     set the label (at most 11 characters)\n"
      "  vol -d                   delete the label of the current volume\n"
      "A label cannot contain % ^ * + = [ ] | : ; \" < > ? / or a dot.\n"
      "With -data: volume, label, size, free, readonly.\n", CMD_DATA },
    { "attrib", cmd_attrib, "attrib [+a|-a] [+s|-s] [+h|-h] [+r|-r] [FILE | DIR | PATTERN...]",
      "Show or change file attributes (archive, system, hidden, read-only)",
      "  attrib                   attributes of everything in the current dir\n"
      "  attrib DIR               attributes of the entries in DIR\n"
      "  attrib +r boot.efi       make a file read-only\n"
      "  attrib -h -s fs0:\\*      clear hidden and system on all root entries\n"
      "Letters: a archive, s system, h hidden, r read-only (upper case works).\n"
      "Each line shows D (directory), A, S, H, R and the path. When changing, a\n"
      "directory itself is changed, not its contents. Wildcards are allowed.\n" },
    { "comp", cmd_comp, "comp [-n COUNT|all] [-s BYTES] FILE1 FILE2",
      "Compare two files and show where they differ",
      "  -n COUNT  stop after COUNT differences (default 10; all: no limit)\n"
      "  -s BYTES  bytes shown for each difference (default 16)\n"
      "  comp -n all old.rom new.rom   list every difference\n"
      "A run of consecutive different bytes is one difference: its hex offset is\n"
      "printed, then the bytes of both files in hex and as text. Different sizes\n"
      "count as one more difference. ERR is nonzero if the files differ. Both\n"
      "files are read into memory.\n" },
    { "cmp", cmd_comp, "cmp [-s BYTES] FILE1 FILE2", "Compare two files and stop at the first difference",
      "  Same as comp -n 1. -s BYTES sets the bytes shown (default 16).\n" },
    { "setsize", cmd_setsize, "setsize SIZE FILE...", "Set the size of files (truncate or extend with zeros)",
      "  setsize 1048576 disk.img    make a file of 1 MiB\n"
      "  setsize 0 log.txt           empty a file\n"
      "SIZE is in bytes (decimal, or hex with 0x). Missing files are created\n"
      "first, as in the UEFI Shell.\n" },
    { "stat", cmd_stat, "stat PATH...", "Show file details",
      "  stat boot.efi            path, type, size, modified time, attributes\n"
      "Size is not shown for directories. Wildcards are not expanded.\n"
      "With -data: name, path, type, size, modified, readonly, hidden, system,\n"
      "archive.\n", CMD_DATA },
};

void cmds_fs_init(void)
{
    shell_register(fs_cmds, ARRAY_SIZE(fs_cmds));
}
