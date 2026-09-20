/* Block devices and disk-level commands: blkN: names, dblk, timezone, getmtc. */
#include "efi_cmds.h"

static EFI_GUID blockio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

/* ---- blkN: block devices, ordered by device path like the volumes ---- */

typedef struct {
    EFI_HANDLE h;
    char *dp;
} Blk;

static Blk *blks;
static int nblks;

static int blk_cmp(const void *a, const void *b)
{
    return strcmp(((const Blk *)a)->dp, ((const Blk *)b)->dp);
}

static void blk_refresh(void)
{
    for (int i = 0; i < nblks; i++)
        free(blks[i].dp);
    free(blks);
    blks = NULL;
    nblks = 0;
    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    if (gBS->LocateHandleBuffer(ByProtocol, &blockio_guid, NULL, &n, &hs) != EFI_SUCCESS)
        return;
    blks = xcalloc(n ? n : 1, sizeof(Blk));
    for (UINTN i = 0; i < n; i++) {
        EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
        gBS->HandleProtocol(hs[i], &gEfiDevicePathGuid, (void **)&dp);
        blks[nblks].h = hs[i];
        blks[nblks].dp = efi_devpath_text(dp);
        nblks++;
    }
    gBS->FreePool(hs);
    qsort(blks, nblks, sizeof(Blk), blk_cmp);
}

static int volume_of(EFI_HANDLE h)
{
    for (int i = 0; i < pal_volume_count(); i++)
        if (efi_volume_handle(i) == h)
            return i;
    return -1;
}

void platform_map_blocks(bool verbose)
{
    blk_refresh();
    if (!nblks)
        return;
    bool data = out_data_mode();
    if (!data)
        out_printf("\n%-6s %-10s %9s  %s\n", "Device", "Type", "Size", verbose ? "Device path" : "Volume");
    for (int i = 0; i < nblks; i++) {
        EFI_BLOCK_IO_PROTOCOL *b;
        if (gBS->HandleProtocol(blks[i].h, &blockio_guid, (void **)&b) != EFI_SUCCESS)
            continue;
        EFI_BLOCK_IO_MEDIA *m = b->Media;
        char size[16] = "-";
        if (m->MediaPresent) {
            uint64_t mb = (m->LastBlock + 1) * m->BlockSize / (1024 * 1024);
            snprintf(size, sizeof(size), mb >= 10240 ? "%lluG" : "%lluM",
                     (unsigned long long)(mb >= 10240 ? mb / 1024 : mb));
        }
        const char *type = m->LogicalPartition ? "partition" : m->RemovableMedia ? "removable" : "disk";
        int vi = volume_of(blks[i].h);
        char blk[16];
        snprintf(blk, sizeof(blk), "blk%d", i);
        if (data) {
            data_record();
            data_field("kind", "block");
            data_field("device", "%s", blk);
            data_field("type", "%s", type);
            data_field("media", "%s", m->MediaPresent ? "yes" : "no");
            data_field("size", "%llu", m->MediaPresent ? (unsigned long long)((m->LastBlock + 1) * m->BlockSize) : 0ULL);
            data_field("blocksize", "%u", m->BlockSize);
            data_field("readonly", "%s", m->ReadOnly ? "yes" : "no");
            data_field("volume", "%s", vi >= 0 ? pal_volume(vi)->name : "");
            data_field("devpath", "%s", blks[i].dp);
        } else if (verbose)
            out_printf("%-6s %-10s %9s  %s%s%s\n", blk, type, size, blks[i].dp, vi >= 0 ? "  = " : "",
                       vi >= 0 ? pal_volume(vi)->name : "");
        else
            out_printf("%-6s %-10s %9s  %s%s\n", blk, type, size, vi >= 0 ? pal_volume(vi)->name : "",
                       m->MediaPresent ? "" : "(no media)");
    }
}

/* blkN, blkN:, fsN:, or a handle number -> handle with BlockIo */
static bool blk_lookup(const char *s, EFI_HANDLE *out)
{
    if (!strncasecmp(s, "blk", 3) && isdigit((uint8_t)s[3])) {
        blk_refresh();
        char *end;
        long n = strtol(s + 3, &end, 10);
        if ((*end && strcmp(end, ":")) || n < 0 || n >= nblks)
            return false;
        *out = blks[n].h;
        return true;
    }
    char *p = strchr(s, ':') ? path_resolve(s) : NULL;
    if (p) {
        for (int i = 0; i < pal_volume_count(); i++) {
            size_t l = strlen(pal_volume(i)->name);
            if (!strncasecmp(p, pal_volume(i)->name, l) && p[l] == ':') {
                *out = efi_volume_handle(i);
                free(p);
                return true;
            }
        }
        free(p);
        return false;
    }
    return efi_parse_handle(s, out);
}

bool platform_map_target(const char *target, char *volname, size_t n)
{
    EFI_HANDLE h;
    if (!blk_lookup(target, &h) && !efi_parse_handle(target, &h))
        return false;
    void *fs;
    if (gBS->HandleProtocol(h, &sfs_guid, &fs) != EFI_SUCCESS)
        return false;
    int vi = volume_of(h);
    if (vi < 0) {
        pal_volumes_refresh();
        vi = volume_of(h);
    }
    if (vi < 0)
        return false;
    snprintf(volname, n, "%s", pal_volume(vi)->name);
    return true;
}

/* ---- dblk ---- */

static void dump_bytes(const uint8_t *d, size_t n, uint64_t base)
{
    for (size_t off = 0; off < n && !con_break(); off += 16) {
        out_printf("  %08llx: ", (unsigned long long)(base + off));
        for (size_t k = 0; k < 16; k++) {
            out_printf("%02X%c", d[off + k], k == 7 ? '-' : ' ');
        }
        out_puts(" *");
        for (size_t k = 0; k < 16; k++)
            out_printf("%c", d[off + k] >= 0x20 && d[off + k] < 0x7f ? d[off + k] : '.');
        out_puts("*\n");
    }
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static void decode_block(const uint8_t *d, uint32_t bs, uint64_t lba)
{
    if (bs < 512)
        return;
    if (lba == 1 && !memcmp(d, "EFI PART", 8)) {
        out_printf("  GPT header: revision %08X, disk LBAs %llu-%llu, %u partition entries of %u bytes at LBA %llu\n",
                   le32(d + 8), (unsigned long long)le64(d + 40), (unsigned long long)le64(d + 48), le32(d + 80),
                   le32(d + 84), (unsigned long long)le64(d + 72));
        return;
    }
    if (d[510] != 0x55 || d[511] != 0xAA)
        return;
    if ((d[0] == 0xEB || d[0] == 0xE9) && (!memcmp(d + 54, "FAT", 3) || !memcmp(d + 82, "FAT", 3))) {
        unsigned bps = d[11] | d[12] << 8;
        bool f32 = !memcmp(d + 82, "FAT", 3);
        out_printf("  FAT boot sector: OEM \"%.8s\", %u bytes/sector, %u sectors/cluster, %s, label \"%.11s\"\n",
                   d + 3, bps, d[13], f32 ? "FAT32" : "FAT12/16", d + (f32 ? 71 : 43));
        return;
    }
    if (lba == 0) {
        out_puts("  MBR partition table:\n");
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = d + 446 + 16 * i;
            if (!e[4])
                continue;
            out_printf("    %d: type %02X%s start LBA %u, %u sectors%s\n", i + 1, e[4],
                       e[4] == 0xEE ? " (GPT protective)" : e[4] == 0xEF ? " (EFI system)" : "", le32(e + 8),
                       le32(e + 12), e[0] == 0x80 ? ", active" : "");
        }
    }
}

static int cmd_dblk(int argc, char **argv)
{
    char *ops[3];
    int nops = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "-b"))
            continue; /* page break: accepted */
        if (argv[i][0] == '-' || nops == 3)
            return cmd_usage("dblk");
        ops[nops++] = argv[i];
    }
    if (!nops)
        return cmd_usage("dblk");
    EFI_HANDLE h;
    EFI_BLOCK_IO_PROTOCOL *b;
    if (!blk_lookup(ops[0], &h) || gBS->HandleProtocol(h, &blockio_guid, (void **)&b) != EFI_SUCCESS)
        return cmd_err("dblk", "%s: not a block device (see map)", ops[0]);
    uint64_t lba = 0, count = 1;
    char *end;
    if (nops > 1) {
        lba = strtoull(ops[1], &end, 16);
        if (*end)
            return cmd_err("dblk", "LBA must be hexadecimal");
    }
    if (nops > 2) {
        count = strtoull(ops[2], &end, 16);
        if (*end || !count || count > 0x10)
            return cmd_err("dblk", "block count must be 1 to 10 (hexadecimal)");
    }
    EFI_BLOCK_IO_MEDIA *m = b->Media;
    if (!m->MediaPresent)
        return cmd_err("dblk", "no media in %s", ops[0]);
    if (lba > m->LastBlock)
        return cmd_err("dblk", "LBA beyond the end of the device (last LBA %llx)", (unsigned long long)m->LastBlock);
    if (lba + count - 1 > m->LastBlock)
        count = m->LastBlock - lba + 1;
    size_t size = (size_t)count * m->BlockSize;
    UINTN pages = (size + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS buf;
    if (gBS->AllocatePages(AllocateAnyPages, EfiBootServicesData, pages, &buf) != EFI_SUCCESS)
        return cmd_err("dblk", "out of memory");
    EFI_STATUS st = b->ReadBlocks(b, m->MediaId, lba, size, (void *)(uintptr_t)buf);
    int rc = RC_OK;
    if (EFI_ERROR(st)) {
        rc = cmd_err("dblk", "read error: %s", efi_strerror(st));
    } else {
        for (uint64_t i = 0; i < count && !con_break(); i++) {
            const uint8_t *d = (const uint8_t *)(uintptr_t)buf + i * m->BlockSize;
            out_printf("LBA %016llX  Size %08X bytes  BlkIo %p\n", (unsigned long long)(lba + i), m->BlockSize, (void *)b);
            dump_bytes(d, m->BlockSize, 0);
            decode_block(d, m->BlockSize, lba + i);
        }
    }
    gBS->FreePages(buf, pages);
    return rc;
}

/* ---- block access for hexedit -d ---- */

typedef EFI_STATUS(EFIAPI *WRITE_BLOCKS)(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, UINT64 Lba, UINTN Size, void *Buf);
typedef EFI_STATUS(EFIAPI *FLUSH_BLOCKS)(EFI_BLOCK_IO_PROTOCOL *This);

static EFI_BLOCK_IO_PROTOCOL *blk_proto(const char *dev)
{
    EFI_HANDLE h;
    EFI_BLOCK_IO_PROTOCOL *b;
    if (!blk_lookup(dev, &h) || gBS->HandleProtocol(h, &blockio_guid, (void **)&b) != EFI_SUCCESS ||
        !b->Media->MediaPresent)
        return NULL;
    return b;
}

int platform_blk_read(const char *dev, uint64_t lba, uint64_t count, uint8_t **buf, size_t *len, uint32_t *bsize)
{
    EFI_BLOCK_IO_PROTOCOL *b = blk_proto(dev);
    if (!b)
        return PAL_ENOENT;
    if (lba + count - 1 > b->Media->LastBlock)
        return PAL_EINVAL;
    size_t size = (size_t)count * b->Media->BlockSize;
    UINTN pages = (size + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS p;
    if (gBS->AllocatePages(AllocateAnyPages, EfiBootServicesData, pages, &p) != EFI_SUCCESS)
        return PAL_ENOMEM;
    EFI_STATUS st = b->ReadBlocks(b, b->Media->MediaId, lba, size, (void *)(uintptr_t)p);
    if (!EFI_ERROR(st)) {
        *buf = xmalloc(size);
        memcpy(*buf, (void *)(uintptr_t)p, size);
        *len = size;
        *bsize = b->Media->BlockSize;
    }
    gBS->FreePages(p, pages);
    return efi_to_pal(st);
}

int platform_blk_write(const char *dev, uint64_t lba, const uint8_t *buf, size_t len)
{
    EFI_BLOCK_IO_PROTOCOL *b = blk_proto(dev);
    if (!b)
        return PAL_ENOENT;
    if (b->Media->ReadOnly)
        return PAL_EROFS;
    UINTN pages = (len + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS p;
    if (gBS->AllocatePages(AllocateAnyPages, EfiBootServicesData, pages, &p) != EFI_SUCCESS)
        return PAL_ENOMEM;
    memcpy((void *)(uintptr_t)p, buf, len);
    EFI_STATUS st = ((WRITE_BLOCKS)b->WriteBlocks)(b, b->Media->MediaId, lba, len, (void *)(uintptr_t)p);
    if (!EFI_ERROR(st))
        ((FLUSH_BLOCKS)b->FlushBlocks)(b);
    gBS->FreePages(p, pages);
    return efi_to_pal(st);
}

int platform_mem_read(uint64_t addr, size_t len, uint8_t **buf)
{
    *buf = xmalloc(len);
    memcpy(*buf, (const void *)(uintptr_t)addr, len);
    return PAL_OK;
}

int platform_mem_write(uint64_t addr, const uint8_t *buf, size_t len)
{
    memcpy((void *)(uintptr_t)addr, buf, len);
    return PAL_OK;
}

/* ---- timezone ---- */

#define TZ_UNSPECIFIED 2047

static const struct {
    int minutes;
    const char *where;
} zones[] = {
    { -720, "Baker Island" }, { -660, "Samoa" }, { -600, "Hawaii" }, { -540, "Alaska" },
    { -480, "Los Angeles, Vancouver" }, { -420, "Denver, Phoenix" }, { -360, "Chicago, Mexico City" },
    { -300, "New York, Toronto" }, { -240, "Santiago, Halifax" }, { -210, "Newfoundland" },
    { -180, "Buenos Aires, Sao Paulo" }, { -120, "South Georgia" }, { -60, "Azores" }, { 0, "London, Lisbon, UTC" },
    { 60, "Rome, Paris, Berlin" }, { 120, "Athens, Cairo, Helsinki" }, { 180, "Moscow, Istanbul" },
    { 210, "Tehran" }, { 240, "Dubai" }, { 270, "Kabul" }, { 300, "Karachi" }, { 330, "India" },
    { 345, "Nepal" }, { 360, "Dhaka" }, { 420, "Bangkok, Jakarta" }, { 480, "Beijing, Singapore" },
    { 540, "Tokyo, Seoul" }, { 570, "Adelaide" }, { 600, "Sydney" }, { 660, "Solomon Islands" },
    { 720, "Auckland" }, { 780, "Tonga" }, { 840, "Kiribati" },
};

static void print_offset(int minutes)
{
    int a = minutes < 0 ? -minutes : minutes;
    out_printf("UTC%c%02d:%02d", minutes < 0 ? '-' : '+', a / 60, a % 60);
}

static int cmd_timezone(int argc, char **argv)
{
    const char *set = NULL;
    bool list = false, full = false;
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "-s") && i + 1 < argc)
            set = argv[++i];
        else if (!strcasecmp(argv[i], "-l"))
            list = true;
        else if (!strcasecmp(argv[i], "-f"))
            full = true;
        else if (!strcasecmp(argv[i], "-b"))
            ;
        else
            return cmd_usage("timezone");
    }
    if (list) {
        for (size_t i = 0; i < ARRAY_SIZE(zones); i++) {
            print_offset(zones[i].minutes);
            out_printf("  %s\n", zones[i].where);
        }
        return RC_OK;
    }
    EFI_TIME t;
    EFI_STATUS st = gRT->GetTime(&t, NULL);
    if (EFI_ERROR(st))
        return cmd_err("timezone", "cannot read the clock: %s", efi_strerror(st));
    if (set) {
        int sign = 1, h = 0, m = 0;
        const char *p = set;
        if (*p == '+' || *p == '-')
            sign = *p++ == '-' ? -1 : 1;
        char *end;
        h = (int)strtol(p, &end, 10);
        if (*end == ':')
            m = (int)strtol(end + 1, &end, 10);
        if (*end || h > 14 || m > 59 || (h == 14 && m))
            return cmd_err("timezone", "use -s [+|-]hh:mm, for example -s +01:00");
        /* UEFI: TimeZone is the offset of the local time from UTC, in minutes */
        t.TimeZone = (INT16)(sign * (h * 60 + m));
        st = gRT->SetTime(&t);
        if (EFI_ERROR(st))
            return cmd_err("timezone", "cannot set the time zone: %s", efi_strerror(st));
    }
    if (t.TimeZone == TZ_UNSPECIFIED) {
        out_puts("Time zone not specified: the clock keeps local time\n");
    } else {
        print_offset(t.TimeZone);
        for (size_t i = 0; i < ARRAY_SIZE(zones); i++)
            if (zones[i].minutes == t.TimeZone)
                out_printf("  (%s)", zones[i].where);
        out_puts("\n");
    }
    if (full)
        out_printf("TimeZone field: %d, daylight: adjust %s, in daylight %s\n", t.TimeZone,
                   t.Daylight & 1 ? "yes" : "no", t.Daylight & 2 ? "yes" : "no");
    return RC_OK;
}

/* ---- getmtc ---- */

static int cmd_getmtc(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
        return cmd_usage("getmtc");
    UINT64 c;
    EFI_STATUS st = gBS->GetNextMonotonicCount(&c);
    if (EFI_ERROR(st))
        return cmd_err("getmtc", "%s", efi_strerror(st));
    out_printf("%llx\n", (unsigned long long)c);
    return RC_OK;
}

static const Cmd disk_cmds[] = {
    { "dblk", cmd_dblk, "dblk DEVICE [LBA [COUNT]]",
      "Show raw blocks of a disk, decoding MBR, GPT and FAT boot sectors",
      "  DEVICE  blkN (see map), fsN: or a handle number (hex)\n"
      "  LBA     first block (hex, default 0)\n"
      "  COUNT   number of blocks (hex, 1 to 10, default 1)\n"
      "  -b      page the output (UEFI Shell option, see help more)\n"
      "Each block is shown in hex and text. An MBR (LBA 0), a GPT header (LBA 1)\n"
      "and FAT boot sectors are also decoded. COUNT stops at the end of the\n"
      "device. The disk is only read.\n"
      "Example: dblk blk0 1\n" },
    { "timezone", cmd_timezone, "timezone [-s [+|-]hh:mm] [-l] [-f]",
      "Show or set the time zone of the clock (-l list, -f details)",
      "  (none)          show the time zone of the real-time clock\n"
      "  -s [+|-]hh:mm   set the offset from UTC (up to 14:00; :mm optional)\n"
      "  -l              list common offsets with example places\n"
      "  -f              also show the raw TimeZone and Daylight fields\n"
      "  -b              page the output (UEFI Shell option, see help more)\n"
      "-s changes only the time zone, not the date and time. -l ignores the\n"
      "other options.\n"
      "Example: timezone -s +01:00\n" },
    { "getmtc", cmd_getmtc, "getmtc", "Show the next monotonic count of the firmware",
      "Prints the 64-bit monotonic counter of the firmware in hex. Each call\n"
      "increases it, so two runs never show the same value.\n" },
};

void efi_disk_init(void)
{
    shell_register(disk_cmds, ARRAY_SIZE(disk_cmds));
}
