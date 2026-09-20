/* Shell environment variables (separate from BASIC variables).
 *
 * Names are case-insensitive. Variables are either volatile (lost when the
 * shell exits) or non-volatile: the platform layer stores those as UEFI
 * variables with the EDK2 shell GUID, so they are shared with the UEFI Shell.
 * A few variables are computed by the shell and read-only. */
#include "shell.h"

typedef struct {
    char *name;
    char *value;
    bool nv;
} EnvVar;

static EnvVar *vars;
static int nvars;

/* Platform storage for non-volatile variables (efi: UEFI variables, host: none). */
void platform_env_load(void (*cb)(const char *name, const char *value));
bool platform_env_store(const char *name, const char *value); /* value NULL: delete */
const char *platform_uefi_version(void);

static const char *readonly_vars[] = { "cwd", "lasterror", "uefishellsupport", "uefishellversion", "uefiversion",
                                       "neshversion" };

bool env_is_readonly(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(readonly_vars); i++)
        if (!strcasecmp(name, readonly_vars[i]))
            return true;
    return false;
}

static EnvVar *find(const char *name)
{
    for (int i = 0; i < nvars; i++)
        if (!strcasecmp(vars[i].name, name))
            return &vars[i];
    return NULL;
}

static void put(const char *name, const char *value, bool nv)
{
    EnvVar *v = find(name);
    if (!v) {
        vars = xrealloc(vars, sizeof(EnvVar) * (nvars + 1));
        v = &vars[nvars++];
        v->name = xstrdup(name);
        v->value = NULL;
    }
    free(v->value);
    v->value = xstrdup(value);
    v->nv = nv;
}

static void load_cb(const char *name, const char *value)
{
    if (!env_is_readonly(name))
        put(name, value, true);
}

void env_init(void)
{
    platform_env_load(load_cb);
    if (!find("path")) {
        /* same default as the UEFI Shell, based on the volume NESH started from */
        int bv = pal_boot_volume();
        const char *v = bv >= 0 ? pal_volume(bv)->name : "fs0";
        char *p = xasprintf(".;%s:\\efi\\tools;%s:\\efi\\boot;%s:\\", v, v, v);
        put("path", p, false);
        free(p);
    }
    if (!find("profiles"))
        put("profiles", ";NESH;", false);
}

/* Overlay: temporary values that hide the real ones for the duration of a
 * command (EFI_SHELL_PROTOCOL.Execute with an environment). Nothing is
 * written to the firmware. value NULL hides the variable. */
typedef struct {
    char *name;
    char *value;
} Overlay;
static Overlay *overlay;
static int noverlay;

void env_overlay_push(const char *name, const char *value)
{
    overlay = xrealloc(overlay, sizeof(Overlay) * (noverlay + 1));
    overlay[noverlay].name = xstrdup(name);
    overlay[noverlay].value = value ? xstrdup(value) : NULL;
    noverlay++;
}

void env_overlay_pop(int count)
{
    while (count-- > 0 && noverlay > 0) {
        noverlay--;
        free(overlay[noverlay].name);
        free(overlay[noverlay].value);
    }
}

/* Returns a malloc'd value, or NULL if the variable does not exist. */
char *env_get(const char *name)
{
    for (int i = noverlay - 1; i >= 0; i--)
        if (!strcasecmp(overlay[i].name, name))
            return overlay[i].value ? xstrdup(overlay[i].value) : NULL;
    if (!strcasecmp(name, "cwd"))
        return xstrdup(shell_cwd());
    if (!strcasecmp(name, "lasterror"))
        return xasprintf("0x%x", shell_last_status());
    if (!strcasecmp(name, "uefishellsupport"))
        return xstrdup("3"); /* full UEFI Shell level */
    if (!strcasecmp(name, "uefishellversion"))
        return xstrdup("2.2");
    if (!strcasecmp(name, "uefiversion"))
        return xstrdup(platform_uefi_version());
    if (!strcasecmp(name, "neshversion"))
        return xstrdup(NESH_VERSION);
    EnvVar *v = find(name);
    return v ? xstrdup(v->value) : NULL;
}

bool env_exists(const char *name)
{
    return env_is_readonly(name) || find(name);
}

bool env_is_nv(const char *name)
{
    EnvVar *v = find(name);
    return v && v->nv;
}

/* Sets (value != NULL) or deletes (value == NULL) a variable.
 * Returns 0, or an error message. */
const char *env_set(const char *name, const char *value, bool nv)
{
    if (!*name || strpbrk(name, " \t="))
        return "invalid variable name";
    if (env_is_readonly(name))
        return "read-only variable";
    EnvVar *v = find(name);
    if (!value) {
        if (!v)
            return "no such variable";
        if (v->nv)
            platform_env_store(v->name, NULL);
        free(v->name);
        free(v->value);
        *v = vars[--nvars];
        return NULL;
    }
    if (v && v->nv && !nv)
        platform_env_store(v->name, NULL); /* becomes volatile: remove the stored copy */
    if (nv && !platform_env_store(name, value))
        return "cannot store the variable in the firmware (NVRAM)";
    put(name, value, nv);
    return NULL;
}

static int env_cmp(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

/* All variable names (sorted, including the read-only ones); free with argv_free. */
char **env_names(int *count)
{
    int n = nvars + (int)ARRAY_SIZE(readonly_vars);
    char **names = xmalloc(sizeof(char *) * (n + 1));
    int k = 0;
    for (size_t i = 0; i < ARRAY_SIZE(readonly_vars); i++)
        names[k++] = xstrdup(readonly_vars[i]);
    for (int i = 0; i < nvars; i++)
        names[k++] = xstrdup(vars[i].name);
    names[k] = NULL;
    qsort(names, k, sizeof(char *), env_cmp);
    *count = k;
    return names;
}

/* ---- set command ---- */

static int cmd_set(int argc, char **argv)
{
    bool vol = false, del = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] && !argv[i][2]; i++) {
        if (argv[i][1] == 'v')
            vol = true;
        else if (argv[i][1] == 'd')
            del = true;
        else
            return cmd_usage("set");
    }
    if (i >= argc) {
        if (del)
            return cmd_usage("set");
        int n;
        char **names = env_names(&n);
        for (int k = 0; k < n; k++) {
            char *v = env_get(names[k]);
            const char *kind = env_is_readonly(names[k]) ? "readonly" : env_is_nv(names[k]) ? "permanent" : "temporary";
            if (out_data_mode()) {
                data_record();
                data_field("name", "%s", names[k]);
                data_field("value", "%s", v ? v : "");
                data_field("kind", "%s", kind);
            } else {
                out_printf("%c %-18s = %s\n", kind[0] == 'r' ? 'R' : kind[0] == 't' ? 'V' : ' ', names[k], v ? v : "");
            }
            free(v);
        }
        argv_free(names);
        if (!out_data_mode())
            out_puts("(V: temporary, R: read-only, others are kept in the firmware)\n");
        return RC_OK;
    }
    const char *name = argv[i++];
    if (del) {
        if (i != argc)
            return cmd_usage("set");
        const char *e = env_set(name, NULL, false);
        return e ? cmd_err("set", "%s: %s", name, e) : RC_OK;
    }
    if (i == argc) {
        char *v = env_get(name);
        if (!v)
            return cmd_err("set", "%s: no such variable", name);
        out_printf("%s\n", v);
        free(v);
        return RC_OK;
    }
    Sbuf b;
    sb_init(&b);
    for (; i < argc; i++) {
        if (b.len)
            sb_putc(&b, ' ');
        sb_adds(&b, argv[i]);
    }
    const char *e = env_set(name, b.s, !vol);
    sb_free(&b);
    return e ? cmd_err("set", "%s: %s", name, e) : RC_OK;
}

static const Cmd env_cmds[] = {
    { "set", cmd_set, "set [-v] [NAME [VALUE...]] | set -d NAME",
      "Show or change environment variables",
      "  set                 list all the variables\n"
      "  set NAME            show one variable\n"
      "  set NAME VALUE      set a variable, kept in the firmware across reboots\n"
      "  set -v NAME VALUE   set a temporary variable (lost when the shell exits)\n"
      "  set -d NAME         delete a variable\n"
      "Environment variables are shared with the EFI applications started by\n"
      "the shell and are separate from BASIC variables. In scripts: ENV$(name$),\n"
      "SETENV, DELENV. path lists the directories searched for commands (';'\n"
      "separated, '.' = current directory). pager=off stops the automatic\n"
      "paging of long output at the prompt. Read-only: cwd, lasterror,\n"
      "uefishellsupport, uefishellversion, uefiversion, neshversion.\n"
      "With -data (set): name, value, kind (permanent, temporary, readonly).\n", CMD_DATA },
};

void env_cmds_init(void)
{
    shell_register(env_cmds, ARRAY_SIZE(env_cmds));
}
