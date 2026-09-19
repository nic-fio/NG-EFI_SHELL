/* Shell core: command registry, execution, paths, file helpers. */
#ifndef NESH_SHELL_H
#define NESH_SHELL_H

#include "con.h"

#define NESH_NAME "NESH"
#define NESH_VERSION "0.1.0"
#define SCRIPT_EXT ".nsb"

/* Exit codes */
#define RC_OK 0
#define RC_FAIL 1
#define RC_USAGE 2
#define RC_NOTFOUND 127
#define RC_BREAK 130

typedef int (*CmdFn)(int argc, char **argv);

typedef struct {
    const char *name;
    CmdFn fn;
    const char *usage;   /* "cp [-r] SRC... DST" */
    const char *summary; /* one line */
    const char *help;    /* detailed help, may be NULL */
    int flags;           /* CMD_* */
} Cmd;

#define CMD_KEEP_QUOTES 1 /* arguments keep their double quotes (setvar "text" vs L"text") */
#define CMD_DATA 2        /* supports "-data" (key=value records, see out_data_mode) */

void shell_register(const Cmd *cmds, int n);
const Cmd *shell_find_cmd(const char *name);
int shell_cmd_count(void);
const Cmd *shell_cmd_at(int i);

/* Execution. Returns the exit code (also stored as the last status). */
int shell_exec_argv(int argc, char **argv);
int shell_exec_line(const char *line); /* splits words, handles "> file" and ">> file" */
char **shell_split(const char *line, int *argc, const char **err);
void argv_free(char **argv);
int shell_last_status(void);
/* Looks for NAME, NAME.nsb, NAME.efi like the command lookup does (the "path"
 * variable unless NAME contains a path). Returns a canonical path or NULL. */
char *shell_find_executable(const char *name);

/* Paths: canonical form is "fsN:\dir\file". */
char *path_resolve(const char *p); /* NULL if the volume does not exist */
const char *shell_cwd(void);
int shell_chdir(const char *p);
const char *path_basename(const char *p);
char *path_dirname(const char *canon);
char *path_join(const char *dir, const char *name);
bool path_has_wildcards(const char *p);
/* Expands a pattern like "fs0:\efi\*.efi" into canonical paths (sorted). */
char **path_glob(const char *pattern, int *count);

/* Files (canonical paths). */
int file_read_all(const char *path, char **data, size_t *len);
int file_write_all(const char *path, const char *data, size_t len, bool append);
/* Text file -> UTF-8 (handles UTF-8 BOM and UCS-2 LE with BOM). */
int file_read_text(const char *path, char **text, size_t *len);

/* Command helpers: print "cmd: message" and return RC_FAIL. */
int cmd_err(const char *cmd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int cmd_perr(const char *cmd, const char *path, int palerr);
int cmd_usage(const char *cmd);

/* Simple option parser: flags given as single letters in "opts".
 * Returns the index of the first operand, or -1 after printing an error. */
int getopts(int argc, char **argv, const char *opts, bool *flags);

/* Line editor (interactive prompt, INPUT). Returns NULL on Ctrl-C / EOF. */
char *lineedit_read(const char *prompt, bool use_history);
void history_add(const char *line);
int history_count(void);
const char *history_at(int i);
void history_clear(void);

/* Secure Boot state: low-level hardware writes are refused when active. */
bool secure_boot_active(void);
bool hw_write_allowed(const char *cmd);

/* Interactive menu: returns the chosen item (1-based), 0 if cancelled. */
int ui_menu(const char *title, char **items, int n, int timeout_s, int def);

/* Environment variables (separate from BASIC variables). */
void env_init(void);
char *env_get(const char *name);                                  /* malloc'd, NULL if missing */
const char *env_set(const char *name, const char *value, bool nv); /* value NULL deletes; returns error or NULL */
bool env_exists(const char *name);
bool env_is_readonly(const char *name);
bool env_is_nv(const char *name);
char **env_names(int *count);
void env_cmds_init(void);
void env_overlay_push(const char *name, const char *value); /* temporary, never stored */
void env_overlay_pop(int count);

/* Extra volume names (map aliases). */
bool map_alias_set(const char *name, const char *volume);
char *map_aliases_of(const char *volume);

/* Aliases. */
void alias_init(void);
const char *alias_get(const char *name, bool *nv);
const char *alias_set(const char *name, const char *value, bool nv, bool replace); /* value NULL deletes */
char **alias_names(int *count);
void alias_cmds_init(void);

/* Scripts currently running (for the shell protocol's BatchIsActive). */
int shell_script_depth(void);
void shell_script_enter(void);
void shell_script_leave(void);

/* exit: stop every script and leave the shell with shell_exit_code.
 * exit /b: end only the current script (the interpreter clears the flag). */
extern bool shell_exit_requested;
extern bool shell_script_exit_requested;
extern int shell_exit_code;

/* Registration of command modules. */
void cmds_core_init(void);
void cmds_fs_init(void);
void cmds_text_init(void);
void cmds_sys_init(void);
void cmds_util_init(void);
void cmds_edit_init(void);
void platform_cmds_init(void); /* EFI-only commands (stubs on the host) */

#endif
