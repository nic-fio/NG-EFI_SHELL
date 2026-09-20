/* Shell and session commands: help, ver, cls, exit, history, echo, pause, sleep, which. */
#include "../core/shell.h"
#include "../basic/interp_int.h"

static const char help_basic[] =
    "NESH BASIC - quick reference\n"
    "\n"
    "Values    integers (64 bit): 42  -7  &HFF  0xFF  &B1010  &O17\n"
    "          strings: \"text\" (write \"\" for a quote). String variables end with $.\n"
    "Variables x = 5   name$ = \"abc\"   LET is optional. Names are case-insensitive.\n"
    "Arrays    DIM a(10)  (elements 0..10)   REDIM a(20) keeps the values.\n"
    "Operators + - * / \\ MOD ^   = <> < > <= >=   AND OR XOR NOT SHL SHR (bitwise)\n"
    "          + also joins strings. True is -1, false is 0.\n"
    "Output    PRINT a; b$, c    (';' joins, ',' tabulates, trailing ';' = no newline)\n"
    "Input     INPUT \"Name\"; n$     PAUSE [\"msg\"]    k$ = KEY$(ms)\n"
    "Screen    CLS   COLOR fg[, bg]   LOCATE row, col\n"
    "\n"
    "IF cond THEN ... [ELSEIF cond THEN ...] [ELSE ...] END IF\n"
    "IF cond THEN stmt [ELSE stmt]           (single line, ':' separates statements)\n"
    "FOR i = 1 TO 10 [STEP 2] ... NEXT [i]\n"
    "WHILE cond ... WEND\n"
    "DO [WHILE|UNTIL cond] ... LOOP [WHILE|UNTIL cond]\n"
    "SELECT CASE x ... CASE 1, 2 ... CASE 3 TO 9 ... CASE IS > 10 ... CASE ELSE ... END SELECT\n"
    "EXIT FOR|WHILE|DO   CONTINUE FOR|WHILE|DO   GOTO label   (label: at line start)\n"
    "SUB name(a, b$) ... END SUB            call:  name 1, \"x\"   or  CALL name(1, \"x\")\n"
    "FUNCTION f$(x) ... RETURN value ... END FUNCTION\n"
    "LOCAL x, y$   inside SUB/FUNCTION; other variables are global.\n"
    "END [code]    ends the script.   ' or REM starts a comment.   \" _\" continues a line.\n"
    "\n"
    "Shell commands\n"
    "RUN \"cp a.txt b.txt\"          command line (quotes group words, > file redirects)\n"
    "RUN \"cp\", src$, dst$          separate arguments (safe with spaces)\n"
    "RUN \"ls -l\" TO \"list.txt\"     output to a file (APPEND \"file\" to append)\n"
    "out$ = RUN$(\"ls\")             captures the output\n"
    "ERR                           exit code of the last command (0 = success)\n"
    "RUN$(\"map -data\")             many commands print key=value records with -data:\n"
    "                              read them with RECORDS and FIELD$ (see 'help functions')\n"
    "Scripts (" SCRIPT_EXT ") and .efi files run like commands: ARGC and ARG$(n) give the arguments.\n"
    "At the prompt, lines that are not BASIC statements are run as commands.\n"
    "\n"
    "See 'help functions' for the list of built-in functions.\n";

static const char help_functions[] =
    "Built-in functions\n"
    "\n"
    "Strings   LEN(s$) LEFT$(s$,n) RIGHT$(s$,n) MID$(s$,start[,n]) INSTR([start,]s$,find$)\n"
    "          UCASE$ LCASE$ TRIM$ LTRIM$ RTRIM$ REPLACE$(s$,find$,new$)\n"
    "          STRING$(n,c$) SPACE$(n) LPAD$(s$,w[,c$]) RPAD$(s$,w[,c$])\n"
    "          SPLIT(s$,sep$,arr$) -> count   JOIN$(arr$,sep$)   UBOUND(arr)\n"
    "Convert   VAL(s$) STR$(n) HEX$(n[,digits]) BIN$(n[,digits]) OCT$(n) CHR$(code) ASC(s$)\n"
    "Math      ABS SGN MIN(a,b,...) MAX(a,b,...) RND[(n)]\n"
    "Files     FILEEXISTS(p$) DIREXISTS(p$) FILESIZE(p$) READFILE$(p$)\n"
    "          WRITEFILE p$, s$   APPENDFILE p$, s$   DIR$(pattern$) then DIR$ for the next\n"
    "          CWD$\n"
    "Program   ARGC ARG$(n) ERR RUN$(cmd...) TICKS (ms) DATE$ TIME$ KEY$[(ms)]\n"
    "          MENU(title$, \"a|b|c\" [, timeout_s [, default]]) -> choice, 0 = cancelled\n"
    "          PLATFORM$ VERSION$\n"
    "Data      RECORDS(text$, arr$) splits \"-data\" output into records, FIELD$(record$, key$)\n"
    "          e.g. n = RECORDS(RUN$(\"map -data\"), v$()): PRINT FIELD$(v$(0), \"volume\")\n"
    "Environment ENV$(name$)   SETENV name$, value$ [, permanent]   DELENV name$\n"
    "          (shell variables shared with EFI applications, see 'help set')\n"
    "Firmware  VAREXISTS(name$[,guid$]) VAR$(name$[,guid$]) (text)  VARHEX$(...) (hex bytes)\n"
    "          VARNUM(name$[,guid$]) (little-endian number)   DELVAR name$[, guid$]\n"
    "          SETVAR name$, hex$ [, guid$ [, attr]]   SETVARSTR name$, text$ [, guid$ [, attr]]\n"
    "          FWVENDOR$ SECUREBOOT   (guid$: \"global\" by default, see 'help var')\n"
    "          BOOTCOUNT BOOTID(i) BOOTDESC$(id) BOOTPATH$(id) BOOTARGS$(id) BOOTCURRENT\n"
    "          BOOTENABLED(id)\n"
    "\n"
    "File functions set ERR (0 = success) instead of stopping the script.\n";

static void print_wrapped_table(void)
{
    int cols, rows;
    pal_con_size(&cols, &rows);
    int w = 0;
    for (int i = 0; i < shell_cmd_count(); i++)
        w = MAX(w, (int)strlen(shell_cmd_at(i)->name));
    for (int i = 0; i < shell_cmd_count(); i++) {
        const Cmd *c = shell_cmd_at(i);
        out_printf("  %-*s  %s\n", w, c->name, c->summary ? c->summary : "");
    }
}

/* Prints the lines of a help text that mention "word" as a whole word.
 * The help texts write statements and functions in capitals, so the match is
 * case-sensitive: it does not catch the same word used in a sentence.
 * Returns how many lines were printed. */
static int print_lines_about(const char *text, const char *word)
{
    size_t wl = strlen(word);
    int printed = 0;
    for (const char *line = text; *line;) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        for (size_t i = 0; i + wl <= len; i++) {
            char before = i ? line[i - 1] : ' ', after = line[i + wl];
            if (strncmp(line + i, word, wl) || isalnum((uint8_t)before) || before == '_' || before == '$')
                continue;
            if (isalnum((uint8_t)after) || after == '_' || (after == '$' && word[wl - 1] != '$'))
                continue;
            out_printf("  %.*s\n", (int)len, line);
            printed++;
            break;
        }
        line = nl ? nl + 1 : line + len;
    }
    return printed;
}

/* "help PRINT", "help LEN": statements and built-in functions of NESH BASIC. */
static int help_basic_word(const char *name)
{
    bool func = basic_find_func(name) != NULL;
    bool keyword = basic_is_keyword(name);
    if (!func && !keyword) {
        /* LEN is registered as "len", RIGHT$ as "right$": try both spellings */
        char *with = xasprintf("%s$", name);
        func = basic_find_func(with) != NULL;
        free(with);
        if (!func)
            return RC_NOTFOUND;
    }
    char *upper = xstrdup(name);
    for (char *q = upper; *q; q++)
        *q = (char)toupper((uint8_t)*q);
    out_printf("%s is a NESH BASIC %s.\n\n", upper, func && !keyword ? "built-in function" : "statement");
    int n = print_lines_about(help_basic, upper);
    n += print_lines_about(help_functions, upper);
    if (!n)
        out_printf("  See 'help basic' and 'help functions'.\n");
    else
        out_puts("\nMore: 'help basic' (language), 'help functions' (functions).\n");
    free(upper);
    return RC_OK;
}

static int cmd_help(int argc, char **argv)
{
    if (argc < 2) {
        out_puts("Commands (help NAME for details, 'help basic' for the scripting language):\n\n");
        print_wrapped_table();
        out_puts("\nScripts (" SCRIPT_EXT ") and EFI applications (.efi) are run by typing their name.\n");
        return RC_OK;
    }
    if (!strcasecmp(argv[1], "basic") || !strcasecmp(argv[1], "language")) {
        out_puts(help_basic);
        return RC_OK;
    }
    if (!strcasecmp(argv[1], "functions") || !strcasecmp(argv[1], "func")) {
        out_puts(help_functions);
        return RC_OK;
    }
    const Cmd *c = shell_find_cmd(argv[1]);
    if (!c) {
        if (help_basic_word(argv[1]) == RC_OK)
            return RC_OK;
        return cmd_err("help", "no help for '%s' (try 'help', 'help basic' or 'help functions')", argv[1]);
    }
    out_printf("usage: %s\n\n%s\n", c->usage ? c->usage : c->name, c->summary ? c->summary : "");
    if (c->help)
        out_printf("\n%s", c->help);
    return RC_OK;
}

static int cmd_ver(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    extern void platform_print_version(void);
    if (out_data_mode()) {
        data_record();
        data_field("shell", "%s", NESH_NAME);
        data_field("version", "%s", NESH_VERSION);
        data_field("platform", "%s", pal_platform_name());
        platform_print_version(); /* adds its fields to the same record */
        return RC_OK;
    }
    out_printf("%s %s - New EFI Shell\n", NESH_NAME, NESH_VERSION);
    out_printf("Platform: %s\n", pal_platform_name());
    platform_print_version();
    return RC_OK;
}

static int cmd_cls(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        int64_t bg;
        if (!parse_int(argv[1], &bg) || bg < 0 || bg > 7)
            return cmd_usage("cls");
        int fg, obg;
        pal_con_get_color(&fg, &obg);
        pal_con_set_color(fg, (int)bg);
    }
    if (out_is_console())
        pal_con_clear();
    return RC_OK;
}

static int cmd_exit(int argc, char **argv)
{
    /* exit /b (UEFI Shell): end only the current script */
    bool script = argc > 1 && (!strcasecmp(argv[1], "/b") || !strcasecmp(argv[1], "-b"));
    int i = script ? 2 : 1;
    int64_t code = 0;
    if (argc - i > 1 || (i < argc && !parse_int(argv[i], &code)))
        return cmd_usage("exit");
    if (!script) {
        shell_exit_requested = true;
        shell_exit_code = (int)code;
    } else if (shell_script_depth() > 0) {
        shell_script_exit_requested = true;
        shell_exit_code = (int)code;
    }
    return (int)code;
}

static int cmd_history(int argc, char **argv)
{
    bool f[1];
    int i = getopts(argc, argv, "c", f);
    if (i < 0)
        return RC_USAGE;
    if (f[0]) {
        history_clear();
        return RC_OK;
    }
    int n = history_count();
    int64_t last = n;
    if (i < argc && (!parse_int(argv[i], &last) || last < 0))
        return cmd_usage("history");
    for (int k = (int)MAX(0, n - last); k < n; k++)
        out_printf("%5d  %s\n", k + 1, history_at(k));
    return RC_OK;
}

static int cmd_echo(int argc, char **argv)
{
    bool nonl = false;
    int i = 1;
    /* echo -on / -off: UEFI Shell script echo switches, nothing to do here */
    if (argc == 2 && (!strcasecmp(argv[1], "-on") || !strcasecmp(argv[1], "-off")))
        return RC_OK;
    if (i < argc && !strcmp(argv[i], "-n")) {
        nonl = true;
        i++;
    }
    for (; i < argc; i++) {
        out_puts(argv[i]);
        if (i + 1 < argc)
            out_puts(" ");
    }
    if (!nonl)
        out_puts("\n");
    return RC_OK;
}

static int cmd_pause(int argc, char **argv)
{
    Sbuf b;
    sb_init(&b);
    /* -q (UEFI Shell): no message */
    bool quiet = argc > 1 && !strcasecmp(argv[1], "-q");
    for (int i = quiet ? 2 : 1; i < argc; i++) {
        if (b.len)
            sb_putc(&b, ' ');
        sb_adds(&b, argv[i]);
    }
    bool ok = con_pause(b.len ? b.s : quiet ? "" : NULL);
    sb_free(&b);
    return ok ? RC_OK : RC_BREAK;
}

static int cmd_sleep(int argc, char **argv)
{
    int64_t ms;
    if (argc != 2 || !parse_int(argv[1], &ms) || ms < 0)
        return cmd_usage("sleep");
    uint64_t end = pal_ticks_ms() + (uint64_t)ms;
    while (pal_ticks_ms() < end) {
        uint64_t left = end - pal_ticks_ms();
        pal_sleep_ms(left > 20 ? 20 : (uint32_t)left);
        if (con_break())
            return RC_BREAK;
    }
    return RC_OK;
}

static int cmd_type(int argc, char **argv)
{
    if (argc < 2)
        return cmd_usage("which");
    int rc = RC_OK;
    for (int i = 1; i < argc; i++) {
        const char *alias = alias_get(argv[i], NULL);
        char *p = NULL;
        if (alias) {
            out_printf("%s: alias for %s\n", argv[i], alias);
        } else if (shell_find_cmd(argv[i])) {
            out_printf("%s: built-in command\n", argv[i]);
        } else if ((p = shell_find_executable(argv[i]))) {
            out_printf("%s: %s\n", argv[i], p);
            free(p);
        } else if (basic_find_func(argv[i])) {
            out_printf("%s: BASIC function\n", argv[i]);
        } else {
            err_printf("%s: not found\n", argv[i]);
            rc = RC_NOTFOUND;
        }
    }
    return rc;
}

static const Cmd core_cmds[] = {
    { "help", cmd_help, "help [COMMAND | STATEMENT | FUNCTION | basic | functions]", "Show help",
      "  help                list all the commands with a one-line summary\n"
      "  help NAME           usage and details of one command\n"
      "  help PRINT          a statement or function of the language: the lines\n"
      "                      about it from the references below\n"
      "  help basic          quick reference of the BASIC scripting language\n"
      "  help functions      list of the built-in BASIC functions\n"
      "Names are not case-sensitive. 'help language' and 'help func' also work.\n"
      "An unknown NAME is an error.\n" },
    { "ver", cmd_ver, "ver", "Show shell, firmware and UEFI versions",
      "  ver                 shell version and platform; on UEFI also the firmware\n"
      "                      vendor and revision, UEFI version and Secure Boot state\n"
      "With -data: shell, version, platform, firmware, firmware_revision, uefi,\n"
      "secure_boot (yes/no).\n", CMD_DATA },
    { "cls", cmd_cls, "cls [BACKGROUND]", "Clear the screen",
      "  cls                 clear the screen\n"
      "  cls 1               set a blue background, then clear\n"
      "BACKGROUND: 0 black, 1 blue, 2 green, 3 cyan, 4 red, 5 magenta, 6 brown,\n"
      "7 light gray. The screen is not cleared when the output is redirected.\n" },
    { "exit", cmd_exit, "exit [/b] [CODE]", "Leave the shell and return to the firmware",
      "  exit                leave the shell with exit code 0\n"
      "  exit 3              leave the shell with exit code 3\n"
      "  exit /b 2           end only the current script, with exit code 2\n"
      "In a script (RUN \"exit\"), exit stops all running scripts at once, then\n"
      "leaves the shell. exit /b is like END CODE; at the prompt it only sets the\n"
      "exit code. CODE may be decimal or hex (0x10). -b is the same as /b.\n", CMD_ARG_B },
    { "history", cmd_history, "history [-c] [N]", "Show the last N commands (-c clears the history)",
      "Keys at the prompt: Up/Down browse the history, Ctrl-R searches it,\n"
      "Tab completes commands and paths, Ctrl-A/E start/end of line,\n"
      "Ctrl-K/U delete to end/start, Ctrl-W delete word, Ctrl-L clear screen.\n" },
    { "echo", cmd_echo, "echo [-n] [TEXT...]", "Print text (-n: no newline)",
      "  echo TEXT...        print the words separated by one space, then a newline\n"
      "  echo -n TEXT...     the same without the final newline\n"
      "  echo \"a   b\"        quotes keep the spaces\n"
      "Only a first -n is an option; any other word is printed as it is,\n"
      "including -b (echo never pages).\n"
      "echo -on and echo -off (UEFI Shell script switches) do nothing.\n", CMD_ARG_B },
    { "pause", cmd_pause, "pause [-q] [MESSAGE]", "Wait for a key",
      "  pause               print \"Press any key to continue...\" and wait\n"
      "  pause Insert disk   print your own message and wait for a key\n"
      "  pause -q            wait without a message\n"
      "Exit code 0 for most keys, 130 for Esc or Ctrl-C. In a script, Esc can be\n"
      "tested in ERR; Ctrl-C also stops the script.\n" },
    { "sleep", cmd_sleep, "sleep MILLISECONDS", "Wait for the given time",
      "  sleep 500           wait half a second\n"
      "Ctrl-C stops the wait (exit code 130). For microseconds see stall.\n" },
    { "which", cmd_type, "which NAME...", "Show what a command name refers to",
      "  which NAME...       say if NAME is an alias, a built-in command, a file\n"
      "                      or a BASIC function\n"
      "  which ls            ls: built-in command\n"
      "  which ll            ll: alias for ls -l\n"
      "Files are searched like commands: NAME" SCRIPT_EXT " and NAME.efi in the directories\n"
      "of the path variable, or at the path given. Exit code 127 if a NAME is not\n"
      "found.\n" },
};

void cmds_core_init(void)
{
    shell_register(core_cmds, ARRAY_SIZE(core_cmds));
}
