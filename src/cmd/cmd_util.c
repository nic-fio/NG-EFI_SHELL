/* Utilities: stall, parse, eficompress, efidecompress. */
#include "../core/shell.h"
#include "../lib/eficomp.h"

static int cmd_stall(int argc, char **argv)
{
    int64_t us;
    if (argc != 2 || !parse_int(argv[1], &us) || us < 0)
        return cmd_usage("stall");
    pal_sleep_us((uint64_t)us);
    return RC_OK;
}

/* parse FILE TABLE COLUMN [-i INSTANCE] [-s INSTANCE]
 * Reads "standard format output" (lines: TableName,"value","value",...) and
 * prints one column (1 = first value after the table name) of the matching
 * lines. -i: only the Nth line of that table; -s: only inside the Nth
 * ShellCommand section. Works with any comma separated file. */
static char **csv_split(const char *line, int *n)
{
    char **v = NULL;
    *n = 0;
    const char *p = line;
    for (;;) {
        Sbuf f;
        sb_init(&f);
        while (*p == ' ')
            p++;
        if (*p == '"') {
            for (p++; *p; p++) {
                if (*p == '"') {
                    if (p[1] == '"') {
                        sb_putc(&f, '"');
                        p++;
                        continue;
                    }
                    p++;
                    break;
                }
                sb_putc(&f, *p);
            }
            while (*p && *p != ',')
                p++;
        } else {
            while (*p && *p != ',')
                sb_putc(&f, *p++);
            while (f.len && f.s[f.len - 1] == ' ')
                f.s[--f.len] = 0;
        }
        v = xrealloc(v, sizeof(char *) * (*n + 2));
        v[(*n)++] = sb_steal(&f);
        if (*p != ',')
            break;
        p++;
    }
    v[*n] = NULL;
    return v;
}

static int cmd_parse(int argc, char **argv)
{
    int64_t instance = 0, section = 0, column;
    char *ops[3];
    int nops = 0;
    for (int i = 1; i < argc; i++) {
        if ((!strcasecmp(argv[i], "-i") || !strcasecmp(argv[i], "-s")) && i + 1 < argc) {
            int64_t v;
            if (!parse_int(argv[i + 1], &v) || v < 1)
                return cmd_usage("parse");
            if (tolower((uint8_t)argv[i][1]) == 'i')
                instance = v;
            else
                section = v;
            i++;
        } else if (nops < 3) {
            ops[nops++] = argv[i];
        } else {
            return cmd_usage("parse");
        }
    }
    if (nops != 3 || !parse_int(ops[2], &column) || column < 0)
        return cmd_usage("parse");
    char *path = path_resolve(ops[0]);
    char *text;
    size_t len;
    int e = path ? file_read_text(path, &text, &len) : PAL_ENOENT;
    free(path);
    if (e)
        return cmd_perr("parse", ops[0], e);
    int64_t seen = 0, sect = 0;
    bool found = false;
    for (char *line = text; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        int n;
        char **f = csv_split(line, &n);
        if (n && !strcasecmp(f[0], "ShellCommand"))
            sect++;
        if (n && !strcasecmp(f[0], ops[1]) && (!section || sect == section)) {
            seen++;
            if (!instance || seen == instance) {
                if (column < n)
                    out_printf("%s\n", f[column]);
                found = true;
            }
        }
        argv_free(f);
        line = nl ? nl + 1 : NULL;
    }
    free(text);
    return found ? RC_OK : RC_NOTFOUND;
}

/* Firmware decompressor (EFI_DECOMPRESS_PROTOCOL): -2 if not available. */
int platform_fw_decompress(const uint8_t *in, size_t n, char **out, size_t *out_len);

static int compress_common(int argc, char **argv, bool compress)
{
    bool own = false; /* -n: use the NESH decoder even if the firmware has one */
    int first = 1;
    if (argc == 4 && !compress && !strcasecmp(argv[1], "-n")) {
        own = true;
        first = 2;
    }
    if (argc - first != 2)
        return cmd_usage(argv[0]);
    const char *a1 = argv[first], *a2 = argv[first + 1];
    char *in = path_resolve(a1), *out = path_resolve(a2);
    char *data = NULL, *res = NULL;
    size_t len, rlen;
    int rc = RC_OK;
    int e = in ? file_read_all(in, &data, &len) : PAL_ENOENT;
    if (e) {
        rc = cmd_perr(argv[0], a1, e);
        goto out;
    }
    if (!out) {
        rc = cmd_err(argv[0], "%s: invalid path", a2);
        goto out;
    }
    if (compress) {
        res = efi_compress((const uint8_t *)data, len, &rlen);
    } else if ((own || (e = platform_fw_decompress((const uint8_t *)data, len, &res, &rlen)) == -2)
                   ? efi_decompress((const uint8_t *)data, len, &res, &rlen) != 0
                   : e != 0) {
        rc = cmd_err(argv[0], "%s is not in the UEFI compressed format or is damaged", a1);
        goto out;
    }
    e = file_write_all(out, res, rlen, false);
    if (e)
        rc = cmd_perr(argv[0], a2, e);
    else
        out_printf("%s: %zu -> %zu bytes\n", path_basename(out), len, rlen);
out:
    free(data);
    free(res);
    free(in);
    free(out);
    return rc;
}

static int cmd_eficompress(int argc, char **argv) { return compress_common(argc, argv, true); }
static int cmd_efidecompress(int argc, char **argv) { return compress_common(argc, argv, false); }

static const Cmd util_cmds[] = {
    { "stall", cmd_stall, "stall MICROSECONDS", "Wait for the given number of microseconds",
      "  stall 1000          wait 1000 microseconds (1 ms)\n"
      "Uses the firmware Stall service: the wait cannot be stopped with Ctrl-C.\n"
      "For longer waits use sleep (milliseconds, can be stopped).\n" },
    { "parse", cmd_parse, "parse FILE TABLE COLUMN [-i INSTANCE] [-s INSTANCE]",
      "Extract a column from standard-format (comma separated) output",
      "  Lines look like: TableName,\"value1\",\"value2\",...  COLUMN 1 is value1.\n"
      "  -i N: only the Nth line of TABLE.\n"
      "  -s N: only in the Nth ShellCommand section.\n" },
    { "eficompress", cmd_eficompress, "eficompress INFILE OUTFILE", "Compress a file in the UEFI compression format",
      "  eficompress big.bin big.cmp    compress big.bin into big.cmp\n"
      "Writes the UEFI (EFI 1.1) compressed format, which firmware and efidecompress\n"
      "can read. OUTFILE is overwritten. Prints the sizes before and after.\n" },
    { "efidecompress", cmd_efidecompress, "efidecompress [-n] INFILE OUTFILE",
      "Decompress a file in the UEFI compression format",
      "  -n                  use the NESH decoder even if the firmware has one\n"
      "  efidecompress big.cmp big.bin  decompress big.cmp into big.bin\n"
      "By default the firmware decompressor is used when available. OUTFILE is\n"
      "overwritten. Prints the sizes before and after.\n" },
};

void cmds_util_init(void)
{
    shell_register(util_cmds, ARRAY_SIZE(util_cmds));
}
