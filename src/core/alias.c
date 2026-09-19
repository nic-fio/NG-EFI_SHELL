/* Command aliases: short names for commands, e.g. "alias ll ls -l".
 * Permanent aliases are stored by the platform layer (UEFI variables with
 * the same GUID as the UEFI Shell), temporary ones only live in memory. */
#include "shell.h"

typedef struct {
    char *name;
    char *value;
    bool nv;
} Alias;

static Alias *aliases;
static int naliases;

void platform_alias_load(void (*cb)(const char *name, const char *value));
bool platform_alias_store(const char *name, const char *value);

static Alias *find(const char *name)
{
    for (int i = 0; i < naliases; i++)
        if (!strcasecmp(aliases[i].name, name))
            return &aliases[i];
    return NULL;
}

static void put(const char *name, const char *value, bool nv)
{
    Alias *a = find(name);
    if (!a) {
        aliases = xrealloc(aliases, sizeof(Alias) * (naliases + 1));
        a = &aliases[naliases++];
        a->name = xstrdup(name);
        a->value = NULL;
    }
    free(a->value);
    a->value = xstrdup(value);
    a->nv = nv;
}

static void load_cb(const char *name, const char *value)
{
    if (!shell_find_cmd(name))
        put(name, value, true);
}

void alias_init(void)
{
    /* names used by other shells for commands NESH calls differently */
    static const char *defaults[][2] = {
        { "copy", "cp" }, { "ren", "mv" }, { "move", "mv" }, { "rd", "rmdir" }, { "cls", NULL },
    };
    for (size_t i = 0; i < ARRAY_SIZE(defaults); i++)
        if (defaults[i][1] && !shell_find_cmd(defaults[i][0]))
            put(defaults[i][0], defaults[i][1], false);
    platform_alias_load(load_cb);
}

const char *alias_get(const char *name, bool *nv)
{
    Alias *a = find(name);
    if (a && nv)
        *nv = a->nv;
    return a ? a->value : NULL;
}

/* value NULL deletes. Returns an error message or NULL. */
const char *alias_set(const char *name, const char *value, bool nv, bool replace)
{
    if (!*name || strpbrk(name, " \t\\/:\""))
        return "invalid alias name";
    Alias *a = find(name);
    if (!value) {
        if (!a)
            return "no such alias";
        if (a->nv)
            platform_alias_store(a->name, NULL);
        free(a->name);
        free(a->value);
        *a = aliases[--naliases];
        return NULL;
    }
    if (shell_find_cmd(name))
        return "a built-in command has this name";
    if (a && !replace)
        return "the alias already exists";
    if (a && a->nv && !nv)
        platform_alias_store(a->name, NULL);
    if (nv && !platform_alias_store(name, value))
        return "cannot store the alias in the firmware (NVRAM)";
    put(name, value, nv);
    return NULL;
}

static int name_cmp(const void *a, const void *b)
{
    return strcasecmp(((const Alias *)a)->name, ((const Alias *)b)->name);
}

char **alias_names(int *count)
{
    qsort(aliases, naliases, sizeof(Alias), name_cmp);
    char **v = xmalloc(sizeof(char *) * (naliases + 1));
    for (int i = 0; i < naliases; i++)
        v[i] = xstrdup(aliases[i].name);
    v[naliases] = NULL;
    *count = naliases;
    return v;
}

static int cmd_alias(int argc, char **argv)
{
    bool del = false, vol = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] && !argv[i][2]; i++) {
        if (argv[i][1] == 'd')
            del = true;
        else if (argv[i][1] == 'v')
            vol = true;
        else
            return cmd_usage("alias");
    }
    if (i == argc) {
        if (del)
            return cmd_usage("alias");
        int n;
        char **names = alias_names(&n);
        for (int k = 0; k < n; k++) {
            bool nv;
            const char *v = alias_get(names[k], &nv);
            if (out_data_mode()) {
                data_record();
                data_field("name", "%s", names[k]);
                data_field("value", "%s", v);
                data_field("kind", "%s", nv ? "permanent" : "temporary");
            } else {
                out_printf("%s %-12s = %s\n", nv ? " " : "V", names[k], v);
            }
        }
        argv_free(names);
        return RC_OK;
    }
    const char *name = argv[i++];
    if (del) {
        const char *e = i == argc ? alias_set(name, NULL, false, true) : "usage";
        return e ? cmd_err("alias", "%s: %s", name, e) : RC_OK;
    }
    if (i == argc) {
        const char *v = alias_get(name, NULL);
        if (!v)
            return cmd_err("alias", "%s: no such alias", name);
        out_printf("%s\n", v);
        return RC_OK;
    }
    Sbuf b;
    sb_init(&b);
    for (; i < argc; i++) {
        if (b.len)
            sb_putc(&b, ' ');
        bool q = strpbrk(argv[i], " \t") != NULL;
        if (q)
            sb_putc(&b, '"');
        sb_adds(&b, argv[i]);
        if (q)
            sb_putc(&b, '"');
    }
    const char *e = alias_set(name, b.s, !vol, true);
    sb_free(&b);
    return e ? cmd_err("alias", "%s: %s", name, e) : RC_OK;
}

static const Cmd alias_cmds[] = {
    { "alias", cmd_alias, "alias [-v] [NAME [COMMAND...]] | alias -d NAME", "Show or define command aliases",
      "  alias               list the aliases (V: temporary)\n"
      "  alias ll ls -l      define ll, kept in the firmware across reboots\n"
      "  alias -v ll ls -l   temporary alias\n"
      "  alias -d ll         delete an alias\n"
      "Extra arguments are appended: 'll fs0:' runs 'ls -l fs0:'.\n", CMD_DATA },
};

void alias_cmds_init(void)
{
    shell_register(alias_cmds, ARRAY_SIZE(alias_cmds));
}
