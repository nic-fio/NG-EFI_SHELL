/* Text and data commands, date/time. */
#include "../core/shell.h"

static const char *find_icase(const char *h, size_t hl, const char *n, size_t nl, bool icase)
{
    if (!nl)
        return h;
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t k = 0;
        while (k < nl && (icase ? tolower((uint8_t)h[i + k]) == tolower((uint8_t)n[k]) : h[i + k] == n[k]))
            k++;
        if (k == nl)
            return h + i;
    }
    return NULL;
}

static int cmd_grep(int argc, char **argv)
{
    bool f[4]; /* i v n c */
    int i = getopts(argc, argv, "ivnc", f);
    if (i < 0)
        return RC_USAGE;
    if (argc - i < 2)
        return cmd_usage("grep");
    const char *pat = argv[i++];
    size_t pl = strlen(pat);
    bool multi = argc - i > 1;
    int matches = 0;
    int rc = RC_OK;
    for (; i < argc && !con_break(); i++) {
        char *p = path_resolve(argv[i]);
        if (!p) {
            rc = cmd_err("grep", "%s: invalid path", argv[i]);
            continue;
        }
        int nfiles = 1;
        char **files = NULL;
        if (path_has_wildcards(p)) {
            files = path_glob(p, &nfiles);
        } else {
            files = xmalloc(sizeof(char *) * 2);
            files[0] = xstrdup(p);
            files[1] = NULL;
        }
        if (nfiles > 1)
            multi = true;
        for (int k = 0; k < nfiles; k++) {
            char *text;
            size_t len;
            int e = file_read_text(files[k], &text, &len);
            if (e) {
                if (e != PAL_EISDIR)
                    rc = cmd_perr("grep", files[k], e);
                free(files[k]);
                continue;
            }
            int count = 0, lineno = 0;
            for (char *s = text; *s || s < text + len;) {
                char *eol = memchr(s, '\n', (size_t)(text + len - s));
                size_t ll = eol ? (size_t)(eol - s) : (size_t)(text + len - s);
                lineno++;
                bool hit = find_icase(s, ll, pat, pl, f[0]) != NULL;
                if (hit != f[1]) {
                    count++;
                    if (!f[3]) {
                        if (multi)
                            out_printf("%s:", path_basename(files[k]));
                        if (f[2])
                            out_printf("%d:", lineno);
                        out_write(s, ll);
                        out_puts("\n");
                    }
                }
                if (!eol)
                    break;
                s = eol + 1;
                if (s >= text + len)
                    break;
            }
            if (f[3]) {
                if (multi)
                    out_printf("%s:", path_basename(files[k]));
                out_printf("%d\n", count);
            }
            matches += count;
            free(text);
            free(files[k]);
        }
        free(files);
        free(p);
    }
    if (rc)
        return rc;
    return matches ? RC_OK : RC_FAIL; /* like grep: 1 when nothing matched */
}

static int cmd_hexdump(int argc, char **argv)
{
    int64_t skip = 0, count = -1;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if ((!strcmp(argv[i], "-s") || !strcmp(argv[i], "-n")) && i + 1 < argc) {
            int64_t v;
            if (!parse_int(argv[i + 1], &v) || v < 0)
                return cmd_usage("hexdump");
            if (argv[i][1] == 's')
                skip = v;
            else
                count = v;
            i++;
        } else {
            return cmd_usage("hexdump");
        }
    }
    if (i != argc - 1)
        return cmd_usage("hexdump");
    char *p = path_resolve(argv[i]);
    if (!p)
        return cmd_err("hexdump", "%s: invalid path", argv[i]);
    PalFile *f;
    int e = pal_open(p, PAL_O_READ, &f);
    if (e) {
        free(p);
        return cmd_perr("hexdump", argv[i], e);
    }
    if (skip && (e = pal_seek(f, (uint64_t)skip))) {
        pal_close(f);
        free(p);
        return cmd_perr("hexdump", argv[i], e);
    }
    uint8_t buf[16];
    uint64_t off = (uint64_t)skip;
    while (count != 0 && !con_break()) {
        size_t want = count < 0 || count > 16 ? 16 : (size_t)count;
        size_t got = 0;
        /* fill a whole line even if the file system returns short reads */
        while (got < want) {
            size_t g;
            e = pal_read(f, buf + got, want - got, &g);
            if (e || !g)
                break;
            got += g;
        }
        if (!got)
            break;
        out_printf("%08llx  ", (unsigned long long)off);
        for (size_t k = 0; k < 16; k++) {
            if (k < got)
                out_printf("%02x ", buf[k]);
            else
                out_puts("   ");
            if (k == 7)
                out_puts(" ");
        }
        out_puts(" |");
        for (size_t k = 0; k < got; k++)
            out_printf("%c", buf[k] >= 0x20 && buf[k] < 0x7f ? buf[k] : '.');
        out_puts("|\n");
        off += got;
        if (count > 0)
            count -= (int64_t)got;
        if (got < want)
            break;
    }
    pal_close(f);
    free(p);
    return e ? cmd_perr("hexdump", argv[i], e) : RC_OK;
}

static int head_tail(int argc, char **argv, bool tail)
{
    int64_t lines = 10;
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) {
        if (!parse_int(argv[i + 1], &lines) || lines < 0)
            return cmd_usage(argv[0]);
        i += 2;
    }
    if (i != argc - 1)
        return cmd_usage(argv[0]);
    char *p = path_resolve(argv[i]);
    char *text;
    size_t len;
    int e = p ? file_read_text(p, &text, &len) : PAL_ENOENT;
    free(p);
    if (e)
        return cmd_perr(argv[0], argv[i], e);
    size_t start = 0, end = len;
    if (tail) {
        int64_t n = 0;
        size_t k = len;
        if (k && text[k - 1] == '\n')
            k--;
        while (k > 0) {
            if (text[k - 1] == '\n' && ++n == lines)
                break;
            k--;
        }
        start = lines ? k : len;
    } else {
        int64_t n = 0;
        size_t k = 0;
        while (k < len && n < lines) {
            if (text[k] == '\n')
                n++;
            k++;
        }
        end = k;
    }
    out_write(text + start, end - start);
    if (end > start && text[end - 1] != '\n')
        out_puts("\n");
    free(text);
    return RC_OK;
}

static int cmd_head(int argc, char **argv) { return head_tail(argc, argv, false); }
static int cmd_tail(int argc, char **argv) { return head_tail(argc, argv, true); }

static int cmd_wc(int argc, char **argv)
{
    if (argc < 2)
        return cmd_usage("wc");
    int rc = RC_OK;
    for (int i = 1; i < argc; i++) {
        char *p = path_resolve(argv[i]);
        char *data;
        size_t len;
        int e = p ? file_read_all(p, &data, &len) : PAL_ENOENT;
        free(p);
        if (e) {
            rc = cmd_perr("wc", argv[i], e);
            continue;
        }
        uint64_t lines = 0, words = 0;
        bool inword = false;
        for (size_t k = 0; k < len; k++) {
            if (data[k] == '\n')
                lines++;
            if (isspace((uint8_t)data[k]))
                inword = false;
            else if (!inword)
                inword = true, words++;
        }
        out_printf("%8llu %8llu %8llu %s\n", (unsigned long long)lines, (unsigned long long)words,
                   (unsigned long long)len, argv[i]);
        free(data);
    }
    return rc;
}

/* ---- date / time ---- */

static int cmd_date(int argc, char **argv)
{
    PalTime t;
    if (!pal_get_time(&t))
        return cmd_err(argv[0], "cannot read the real time clock");
    if (argc == 1) {
        if (out_data_mode()) {
            data_record();
            data_field("date", "%04d-%02d-%02d", t.year, t.month, t.day);
            data_field("year", "%d", t.year);
            data_field("month", "%d", t.month);
            data_field("day", "%d", t.day);
        } else {
            out_printf("%04d-%02d-%02d\n", t.year, t.month, t.day);
        }
        return RC_OK;
    }
    int y, m, d;
    int64_t v[3];
    /* YYYY-MM-DD, or MM/DD/YYYY and MM/DD/YY as in the UEFI Shell */
    char *s = xstrdup(argv[1]);
    char sep = strchr(s, '/') ? '/' : '-';
    char *p1 = strchr(s, sep), *p2 = p1 ? strchr(p1 + 1, sep) : NULL;
    bool ok = p1 && p2;
    if (ok) {
        *p1 = *p2 = 0;
        ok = parse_int(s, &v[0]) && parse_int(p1 + 1, &v[1]) && parse_int(p2 + 1, &v[2]);
    }
    free(s);
    if (!ok || argc != 2)
        return cmd_usage("date");
    if (sep == '/') {
        y = (int)(v[2] < 100 ? v[2] + 2000 : v[2]), m = (int)v[0], d = (int)v[1];
    } else {
        y = (int)v[0], m = (int)v[1], d = (int)v[2];
    }
    if (y < 1900 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31)
        return cmd_err("date", "invalid date");
    t.year = y, t.month = m, t.day = d;
    int e = pal_set_time(&t);
    return e ? cmd_err("date", "cannot set the date: %s", pal_strerror(e)) : RC_OK;
}

static int cmd_time(int argc, char **argv)
{
    PalTime t;
    if (!pal_get_time(&t))
        return cmd_err(argv[0], "cannot read the real time clock");
    if (argc == 1) {
        if (out_data_mode()) {
            data_record();
            data_field("time", "%02d:%02d:%02d", t.hour, t.min, t.sec);
            data_field("hour", "%d", t.hour);
            data_field("minute", "%d", t.min);
            data_field("second", "%d", t.sec);
        } else {
            out_printf("%02d:%02d:%02d\n", t.hour, t.min, t.sec);
        }
        return RC_OK;
    }
    int64_t v[3] = { 0, 0, 0 };
    char *s = xstrdup(argv[1]);
    char *parts[3] = { s, NULL, NULL };
    int np = 1;
    for (char *c = s; *c && np < 3; c++)
        if (*c == ':') {
            *c = 0;
            parts[np++] = c + 1;
        }
    bool ok = argc == 2 && np >= 2;
    for (int k = 0; ok && k < np; k++)
        ok = parse_int(parts[k], &v[k]);
    free(s);
    if (!ok || v[0] > 23 || v[1] > 59 || v[2] > 59 || v[0] < 0 || v[1] < 0 || v[2] < 0)
        return cmd_usage("time");
    t.hour = (int)v[0], t.min = (int)v[1], t.sec = (int)v[2];
    int e = pal_set_time(&t);
    return e ? cmd_err("time", "cannot set the time: %s", pal_strerror(e)) : RC_OK;
}

static const Cmd text_cmds[] = {
    { "grep", cmd_grep, "grep [-i] [-v] [-n] [-c] TEXT FILE...",
      "Print the lines of files that contain a text",
      "  -i                  ignore upper/lower case\n"
      "  -v                  print the lines that do NOT contain TEXT\n"
      "  -n                  put the line number before each line\n"
      "  -c                  print only the number of matching lines\n"
      "  grep -i error log.txt      lines containing \"error\", any case\n"
      "  grep -c TODO *" SCRIPT_EXT "         matching lines in each script\n"
      "TEXT is plain text, not a pattern. FILE may contain wildcards; with several\n"
      "files each line starts with the file name. UTF-8 and UCS-2 files are read;\n"
      "directories are skipped. Use -- before a TEXT that starts with '-'.\n"
      "Exit code: 0 if a line matched, 1 if none, other values on errors.\n" },
    { "hexdump", cmd_hexdump, "hexdump [-s OFFSET] [-n LENGTH] FILE", "Hexadecimal dump of a file",
      "  -s OFFSET           start at byte OFFSET (default 0)\n"
      "  -n LENGTH           dump at most LENGTH bytes (default: to the end)\n"
      "  hexdump -n 0x200 disk.img      the first 512 bytes\n"
      "Each line shows the offset, 16 bytes in hex and the printable characters.\n"
      "Numbers may be decimal or hex (0x). Options go before FILE. Ctrl-C stops.\n" },
    { "head", cmd_head, "head [-n N] FILE", "First N lines of a file (default 10)",
      "  -n N                print the first N lines (default 10)\n"
      "  head -n 20 log.txt  the first 20 lines\n"
      "The file is read as text (UTF-8 or UCS-2; carriage returns are removed).\n"
      "One FILE only; -n goes before it.\n" },
    { "tail", cmd_tail, "tail [-n N] FILE", "Last N lines of a file (default 10)",
      "  -n N                print the last N lines (default 10)\n"
      "  tail -n 5 log.txt   the last 5 lines\n"
      "The file is read as text (UTF-8 or UCS-2; carriage returns are removed).\n"
      "One FILE only; -n goes before it.\n" },
    { "wc", cmd_wc, "wc FILE...", "Count lines, words and bytes",
      "  wc a.txt b.txt      one line per file: lines, words, bytes, name\n"
      "Lines are counted as newline characters; words are separated by spaces,\n"
      "tabs or newlines; bytes is the file size. Wildcards are not expanded.\n" },
    { "date", cmd_date, "date [YYYY-MM-DD | MM/DD/YYYY | MM/DD/YY]", "Show or set the date",
      "  date                show the date as YYYY-MM-DD\n"
      "  date 2026-09-19     set the date\n"
      "  date 09/19/2026     set the date, UEFI Shell order (MM/DD/YYYY or MM/DD/YY)\n"
      "A two-digit year means 20YY. The time of day is not changed.\n"
      "With -data: date, year, month, day.\n", CMD_DATA },
    { "time", cmd_time, "time [HH:MM[:SS]]", "Show or set the time",
      "  time                show the time as HH:MM:SS (24 hours)\n"
      "  time 14:30          set the time (seconds become 0)\n"
      "  time 14:30:15       set the time with seconds\n"
      "The date is not changed.\n"
      "With -data: time, hour, minute, second.\n", CMD_DATA },
};

void cmds_text_init(void)
{
    shell_register(text_cmds, ARRAY_SIZE(text_cmds));
}

void cmds_sys_init(void)
{
    /* portable system commands live in cmds_text_init; platform ones in platform_cmds_init */
}
