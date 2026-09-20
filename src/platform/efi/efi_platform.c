/* EFI platform services: running images, Secure Boot state, system commands. */
#include "efi_cmds.h"

static EFI_GUID gShellParamsGuid = EFI_SHELL_PARAMETERS_PROTOCOL_GUID;

/* ---- Secure Boot ---- */

bool secure_boot_active(void)
{
    static int cached = -1;
    if (cached < 0) {
        UINT8 v = 0;
        UINTN sz = 1;
        cached = gRT->GetVariable((CHAR16 *)u"SecureBoot", &gEfiGlobalVariableGuid, NULL, &sz, &v) == EFI_SUCCESS && v == 1;
    }
    return cached == 1;
}

bool hw_write_allowed(const char *cmd)
{
    if (!secure_boot_active())
        return true;
    err_printf("%s: writing to hardware is disabled while Secure Boot is active\n", cmd);
    return false;
}

/* LoadImage refuses an image that Secure Boot does not allow with
 * EFI_SECURITY_VIOLATION (image loaded but untrusted) or, as OVMF does,
 * EFI_ACCESS_DENIED (not loaded at all). */
bool efi_blocked_by_secure_boot(EFI_STATUS st)
{
    return st == EFI_SECURITY_VIOLATION || (st == EFI_ACCESS_DENIED && secure_boot_active());
}

/* ---- Console proxy: captures the output of started images ---- */

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *real_conout;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL proxy;

static EFI_STATUS EFIAPI proxy_output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *s)
{
    (void)This;
    Sbuf b;
    sb_init(&b);
    for (; *s; s++)
        if (*s != '\r')
            sb_put_cp(&b, *s);
    if (b.len)
        out_write(b.s, b.len);
    sb_free(&b);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI proxy_test(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *s)
{
    (void)This;
    return real_conout->TestString(real_conout, s);
}

static EFI_STATUS EFIAPI proxy_reset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN e)
{
    (void)This;
    (void)e;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI proxy_query(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN m, UINTN *c, UINTN *r)
{
    (void)This;
    return real_conout->QueryMode(real_conout, m, c, r);
}

static EFI_STATUS EFIAPI proxy_setmode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN m)
{
    (void)This;
    (void)m;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI proxy_attr(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN a)
{
    (void)This;
    (void)a;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI proxy_clear(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This)
{
    (void)This;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI proxy_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN c, UINTN r)
{
    (void)This;
    (void)c;
    (void)r;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI proxy_enable(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN v)
{
    (void)This;
    (void)v;
    return EFI_SUCCESS;
}

static void st_update_crc(void)
{
    gST->Hdr.CRC32 = 0;
    UINT32 crc = 0;
    gBS->CalculateCrc32(gST, gST->Hdr.HeaderSize, &crc);
    gST->Hdr.CRC32 = crc;
}

static int capture_depth;

static void capture_begin(void)
{
    if (capture_depth++)
        return; /* an application started by another one: the proxy is already in place */
    real_conout = gST->ConOut;
    proxy.Reset = proxy_reset;
    proxy.OutputString = proxy_output;
    proxy.TestString = proxy_test;
    proxy.QueryMode = proxy_query;
    proxy.SetMode = proxy_setmode;
    proxy.SetAttribute = proxy_attr;
    proxy.ClearScreen = proxy_clear;
    proxy.SetCursorPosition = proxy_cursor;
    proxy.EnableCursor = proxy_enable;
    proxy.Mode = real_conout->Mode;
    gST->ConOut = &proxy;
    st_update_crc();
}

static void capture_end(void)
{
    if (--capture_depth)
        return;
    gST->ConOut = real_conout;
    st_update_crc();
}

/* ---- Running images ---- */

static char *join_cmdline(int argc, char **argv)
{
    Sbuf b;
    sb_init(&b);
    for (int i = 0; i < argc; i++) {
        if (i)
            sb_putc(&b, ' ');
        bool q = !*argv[i] || strpbrk(argv[i], " \t\"");
        if (q)
            sb_putc(&b, '"');
        for (const char *p = argv[i]; *p; p++) {
            if (*p == '"')
                sb_putc(&b, '"');
            sb_putc(&b, *p);
        }
        if (q)
            sb_putc(&b, '"');
    }
    return sb_steal(&b);
}

int efi_start_image(const char *path, int argc, char **argv, bool driver_ok)
{
    char *data;
    size_t size;
    int e = file_read_all(path, &data, &size);
    if (e)
        return cmd_perr(argv[0], path, e);
    EFI_DEVICE_PATH_PROTOCOL *dp = efi_file_devpath(path);
    EFI_HANDLE h = NULL;
    EFI_STATUS st = gBS->LoadImage(FALSE, gImage, dp, data, size, &h);
    free(data);
    free(dp);
    if (EFI_ERROR(st)) {
        if (efi_blocked_by_secure_boot(st)) {
            if (h)
                gBS->UnloadImage(h);
            return cmd_err(path_basename(path), "not allowed by Secure Boot (the image is not signed by a trusted key)");
        }
        return cmd_err(path_basename(path), "cannot load the image: %s", efi_strerror(st));
    }
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    gBS->HandleProtocol(h, &gEfiLoadedImageGuid, (void **)&li);

    /* command line in LoadOptions, arguments in ShellParameters */
    char *line = join_cmdline(argc, argv);
    size_t units;
    uint16_t *wline = utf8_to_ucs2(line, &units);
    free(line);
    if (li) {
        li->LoadOptions = wline;
        li->LoadOptionsSize = (UINT32)((units + 1) * 2);
    }
    EFI_SHELL_PARAMETERS_PROTOCOL sp;
    sp.Argc = (UINTN)argc;
    sp.Argv = xcalloc((size_t)argc + 1, sizeof(CHAR16 *));
    for (int i = 0; i < argc; i++)
        sp.Argv[i] = utf8_to_ucs2(argv[i], NULL);
    sp.StdIn = efi_console_file(0);
    sp.StdOut = efi_console_file(1);
    sp.StdErr = efi_console_file(2);
    bool installed = gBS->InstallProtocolInterface(&h, &gShellParamsGuid, EFI_NATIVE_INTERFACE, &sp) == EFI_SUCCESS;

    bool capture = !out_is_console();
    if (capture)
        capture_begin();
    efi_break_clear();
    UINTN exit_size = 0;
    CHAR16 *exit_data = NULL;
    st = gBS->StartImage(h, &exit_size, &exit_data);
    if (capture)
        capture_end();
    pal_con_reset_color();

    if (installed)
        gBS->UninstallProtocolInterface(h, &gShellParamsGuid, &sp);
    for (int i = 0; i < argc; i++)
        free(sp.Argv[i]);
    free(sp.Argv);
    if (exit_data) {
        char *msg = ucs2_to_utf8(exit_data, exit_size / 2);
        if (*msg)
            err_printf("%s: %s\n", path_basename(path), msg);
        free(msg);
        gBS->FreePool(exit_data);
    }
    /* Applications are unloaded by the firmware when they exit; drivers stay resident
     * and keep using LoadOptions, so the buffer is leaked on purpose for them. */
    if (!driver_ok || !li || li->ImageCodeType == EfiLoaderCode)
        free(wline);
    if (st == EFI_SUCCESS)
        return RC_OK;
    err_printf("%s: exited with status: %s\n", path_basename(path), efi_strerror(st));
    int code = (int)(st & 0xFF);
    return code ? code : RC_FAIL;
}

int platform_run_image(const char *path, int argc, char **argv)
{
    return efi_start_image(path, argc, argv, false);
}

/* ---- ver ---- */

void platform_print_version(void)
{
    char *vendor = ucs2_to_utf8(gST->FirmwareVendor, (size_t)-1);
    UINT32 major = gST->Hdr.Revision >> 16, minor = gST->Hdr.Revision & 0xFFFF;
    if (out_data_mode()) {
        data_field("firmware", "%s", vendor);
        data_field("firmware_revision", "0x%08x", gST->FirmwareRevision);
    } else {
        out_printf("Firmware: %s (revision 0x%08x)\n", vendor, gST->FirmwareRevision);
    }
    if (minor % 10)
        info_line(0, 9, "UEFI", "%u.%u.%u", major, minor / 10, minor % 10);
    else
        info_line(0, 9, "UEFI", "%u.%u", major, minor / 10);
    info_line(0, 9, "Secure Boot", "%s", secure_boot_active() ? (out_data_mode() ? "yes" : "active")
                                                             : (out_data_mode() ? "no" : "inactive"));
    free(vendor);
}

/* ---- reset ---- */

static int cmd_reset(int argc, char **argv)
{
    /* UEFI Shell syntax: -w|-s|-c [STRING], -fwui; also "setup" and -f */
    EFI_RESET_TYPE type = EfiResetCold;
    bool setup = false;
    const char *reason = NULL;
    int modes = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcasecmp(a, "-w") || !strcasecmp(a, "-s") || !strcasecmp(a, "-c")) {
            char c = (char)tolower((uint8_t)a[1]);
            type = c == 'w' ? EfiResetWarm : c == 's' ? EfiResetShutdown : EfiResetCold;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                reason = argv[++i];
        } else if (!strcasecmp(a, "setup") || !strcasecmp(a, "-setup") || !strcasecmp(a, "-f") ||
                   !strcasecmp(a, "-fwui")) {
            setup = true;
        } else {
            return cmd_usage("reset");
        }
        modes++;
    }
    if (modes > 1)
        return cmd_usage("reset");
    if (setup) {
        UINT64 supported = 0, ind = 0;
        UINTN sz = sizeof(supported);
        if (gRT->GetVariable((CHAR16 *)u"OsIndicationsSupported", &gEfiGlobalVariableGuid, NULL, &sz, &supported) != EFI_SUCCESS ||
            !(supported & EFI_OS_INDICATIONS_BOOT_TO_FW_UI))
            return cmd_err("reset", "this firmware does not support booting into its setup");
        sz = sizeof(ind);
        UINT32 attr = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
        if (gRT->GetVariable((CHAR16 *)u"OsIndications", &gEfiGlobalVariableGuid, NULL, &sz, &ind) != EFI_SUCCESS)
            ind = 0;
        ind |= EFI_OS_INDICATIONS_BOOT_TO_FW_UI;
        EFI_STATUS st = gRT->SetVariable((CHAR16 *)u"OsIndications", &gEfiGlobalVariableGuid, attr, sizeof(ind), &ind);
        if (EFI_ERROR(st))
            return cmd_err("reset", "cannot set OsIndications: %s", efi_strerror(st));
        pal_reset(PAL_RESET_COLD);
    }
    if (reason) {
        /* ResetData: the string, NUL included */
        size_t units;
        uint16_t *w = utf8_to_ucs2(reason, &units);
        gRT->ResetSystem(type, EFI_SUCCESS, (units + 1) * sizeof(uint16_t), w);
        free(w);
    }
    pal_reset(type == EfiResetShutdown ? PAL_RESET_SHUTDOWN : type == EfiResetWarm ? PAL_RESET_WARM : PAL_RESET_COLD);
    return cmd_err("reset", "the firmware did not reset the system");
}

/* ---- memmap ---- */

static const char *mem_type_name(UINT32 t)
{
    static const char *n[] = {
        "Reserved", "LoaderCode", "LoaderData", "BootServicesCode", "BootServicesData",
        "RuntimeCode", "RuntimeData", "Conventional", "Unusable", "ACPIReclaim",
        "ACPINVS", "MMIO", "MMIOPortSpace", "PalCode", "Persistent", "Unaccepted",
    };
    return t < ARRAY_SIZE(n) ? n[t] : "Other";
}

EFI_MEMORY_DESCRIPTOR *efi_memory_map(UINTN *count, UINTN *desc_size)
{
    UINTN size = 0, key, dsz;
    UINT32 ver;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    EFI_STATUS st = gBS->GetMemoryMap(&size, NULL, &key, &dsz, &ver);
    for (int tries = 0; st == EFI_BUFFER_TOO_SMALL && tries < 8; tries++) {
        size += 8 * dsz;
        free(map);
        map = xmalloc(size);
        st = gBS->GetMemoryMap(&size, map, &key, &dsz, &ver);
    }
    if (EFI_ERROR(st)) {
        free(map);
        return NULL;
    }
    *count = size / dsz;
    *desc_size = dsz;
    return map;
}

static int cmd_memmap(int argc, char **argv)
{
    bool f[2]; /* s b(page break: accepted) */
    int i = getopts(argc, argv, "sb", f);
    if (i < 0 || i != argc)
        return i < 0 ? RC_USAGE : cmd_usage("memmap");
    UINTN n, dsz;
    EFI_MEMORY_DESCRIPTOR *map = efi_memory_map(&n, &dsz);
    if (!map)
        return cmd_err("memmap", "cannot read the memory map");
    UINT64 pages[EfiMaxMemoryType + 1] = { 0 };
    if (!f[0])
        out_printf("%-18s %-16s %-16s %10s  %s\n", "Type", "Start", "End", "Pages", "Attributes");
    for (UINTN k = 0; k < n; k++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + k * dsz);
        UINT32 t = d->Type < EfiMaxMemoryType ? d->Type : EfiMaxMemoryType;
        pages[t] += d->NumberOfPages;
        if (!f[0] && out_data_mode()) {
            data_record();
            data_field("type", "%s", mem_type_name(d->Type));
            data_field("start", "0x%llx", (unsigned long long)d->PhysicalStart);
            data_field("end", "0x%llx", (unsigned long long)(d->PhysicalStart + d->NumberOfPages * 4096 - 1));
            data_field("pages", "%llu", (unsigned long long)d->NumberOfPages);
            data_field("attributes", "0x%llx", (unsigned long long)d->Attribute);
        } else if (!f[0])
            out_printf("%-18s %016llx %016llx %10llu  %016llx\n", mem_type_name(d->Type),
                       (unsigned long long)d->PhysicalStart,
                       (unsigned long long)(d->PhysicalStart + d->NumberOfPages * 4096 - 1),
                       (unsigned long long)d->NumberOfPages, (unsigned long long)d->Attribute);
    }
    if (out_data_mode()) {
        if (f[0])
            for (int t = 0; t <= EfiMaxMemoryType; t++) {
                if (!pages[t])
                    continue;
                data_record();
                data_field("type", "%s", mem_type_name((UINT32)t));
                data_field("pages", "%llu", (unsigned long long)pages[t]);
                data_field("bytes", "%llu", (unsigned long long)(pages[t] * 4096));
            }
        free(map);
        return RC_OK;
    }
    out_printf("%s%-18s %12s %10s\n", f[0] ? "" : "\n", "Type", "Pages", "MiB");
    UINT64 total = 0, usable = 0;
    for (int t = 0; t <= EfiMaxMemoryType; t++) {
        if (!pages[t])
            continue;
        out_printf("%-18s %12llu %10llu\n", mem_type_name((UINT32)t), (unsigned long long)pages[t],
                   (unsigned long long)(pages[t] * 4096 / (1024 * 1024)));
        if (t != EfiMemoryMappedIO && t != EfiMemoryMappedIOPortSpace && t != EfiReservedMemoryType)
            total += pages[t];
        if (t == EfiConventionalMemory || t == EfiBootServicesCode || t == EfiBootServicesData ||
            t == EfiLoaderCode || t == EfiLoaderData)
            usable += pages[t];
    }
    out_printf("Total memory: %llu MiB, available to the OS: %llu MiB\n",
               (unsigned long long)(total * 4096 / (1024 * 1024)), (unsigned long long)(usable * 4096 / (1024 * 1024)));
    free(map);
    return RC_OK;
}

/* ---- sysinfo ---- */

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static int cmd_sysinfo(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (out_data_mode())
        data_record();
    platform_print_version();
    uint32_t r[4];
    char vendor[13];
    cpuid(0, 0, r);
    memcpy(vendor, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    vendor[12] = 0;
    char brand[49] = "";
    cpuid(0x80000000, 0, r);
    if (r[0] >= 0x80000004) {
        for (uint32_t l = 0; l < 3; l++) {
            cpuid(0x80000002 + l, 0, r);
            memcpy(brand + l * 16, r, 16);
        }
        brand[48] = 0;
    }
    char *b = brand;
    while (*b == ' ')
        b++;
    if (out_data_mode()) {
        data_field("cpu", "%s", *b ? b : vendor);
        data_field("cpu_vendor", "%s", vendor);
    } else {
        out_printf("CPU:      %s%s%s\n", *b ? b : vendor, *b ? " / " : "", *b ? vendor : "");
    }
    UINTN n, dsz;
    EFI_MEMORY_DESCRIPTOR *map = efi_memory_map(&n, &dsz);
    if (map) {
        UINT64 total = 0;
        for (UINTN k = 0; k < n; k++) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + k * dsz);
            if (d->Type != EfiMemoryMappedIO && d->Type != EfiMemoryMappedIOPortSpace && d->Type != EfiReservedMemoryType)
                total += d->NumberOfPages;
        }
        if (out_data_mode())
            data_field("memory", "%llu", (unsigned long long)(total * 4096));
        else
            out_printf("Memory:   %llu MiB\n", (unsigned long long)(total * 4096 / (1024 * 1024)));
        free(map);
    }
    int cols, rows;
    pal_con_size(&cols, &rows);
    if (out_data_mode()) {
        data_field("console_columns", "%d", cols);
        data_field("console_rows", "%d", rows);
    } else {
        out_printf("Console:  %d x %d\n", cols, rows);
    }
    efi_print_gop_summary();
    EFI_GUID acpi2 = EFI_ACPI_20_TABLE_GUID, acpi1 = EFI_ACPI_10_TABLE_GUID;
    EFI_GUID smb = EFI_SMBIOS_TABLE_GUID, smb3 = EFI_SMBIOS3_TABLE_GUID;
    bool has_acpi = false, has_smbios = false;
    for (UINTN k = 0; k < gST->NumberOfTableEntries; k++) {
        EFI_GUID *g = &gST->ConfigurationTable[k].VendorGuid;
        if (!memcmp(g, &acpi2, sizeof(EFI_GUID)) || !memcmp(g, &acpi1, sizeof(EFI_GUID)))
            has_acpi = true;
        if (!memcmp(g, &smb, sizeof(EFI_GUID)) || !memcmp(g, &smb3, sizeof(EFI_GUID)))
            has_smbios = true;
    }
    int bv = pal_boot_volume();
    if (out_data_mode()) {
        data_field("config_tables", "%llu", (unsigned long long)gST->NumberOfTableEntries);
        data_field("acpi", "%s", has_acpi ? "yes" : "no");
        data_field("smbios", "%s", has_smbios ? "yes" : "no");
        data_field("boot_volume", "%s", bv >= 0 ? pal_volume(bv)->name : "");
        data_field("boot_devpath", "%s", bv >= 0 ? pal_volume(bv)->devpath : "");
        return RC_OK;
    }
    out_printf("Tables:   %llu configuration tables%s%s\n", (unsigned long long)gST->NumberOfTableEntries,
               has_acpi ? ", ACPI" : "", has_smbios ? ", SMBIOS" : "");
    if (bv >= 0)
        out_printf("Started from: %s: %s\n", pal_volume(bv)->name, pal_volume(bv)->devpath);
    return RC_OK;
}

/* ---- GOP (only what sysinfo needs) ---- */

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelFormat;
    UINT32 PixelMask[4];
    UINT32 PixelsPerScanLine;
} GOP_MODE_INFO;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    GOP_MODE_INFO *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} GOP_MODE;

typedef struct {
    void *QueryMode;
    void *SetMode;
    void *Blt;
    GOP_MODE *Mode;
} GOP;

void efi_print_gop_summary(void)
{
    EFI_GUID guid = { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };
    GOP *gop;
    if (gBS->LocateProtocol(&guid, NULL, (void **)&gop) != EFI_SUCCESS || !gop->Mode || !gop->Mode->Info)
        return;
    if (out_data_mode()) {
        data_field("graphics_width", "%u", gop->Mode->Info->HorizontalResolution);
        data_field("graphics_height", "%u", gop->Mode->Info->VerticalResolution);
        data_field("framebuffer", "0x%llx", (unsigned long long)gop->Mode->FrameBufferBase);
        return;
    }
    out_printf("Graphics: %u x %u (mode %u of %u), frame buffer at 0x%llx\n", gop->Mode->Info->HorizontalResolution,
               gop->Mode->Info->VerticalResolution, gop->Mode->Mode, gop->Mode->MaxMode,
               (unsigned long long)gop->Mode->FrameBufferBase);
}

static const Cmd efi_sys_cmds[] = {
    { "reset", cmd_reset, "reset [-w [STRING] | -s [STRING] | -c [STRING] | -fwui | setup]",
      "Restart or shut down the machine, or reboot into the firmware setup",
      "  (none), -c         cold reset\n"
      "  -w                 warm reset\n"
      "  -s                 shut down (power off)\n"
      "  setup, -fwui, -f   restart into the firmware setup screen (sets the\n"
      "                     OsIndications variable; the firmware must support it)\n"
      "  STRING             reset reason passed to the firmware as ResetData\n"
      "Only one mode may be given. If the firmware does not reset, reset fails\n"
      "with an error.\n"
      "Example: reset -s \"maintenance\"\n" },
    { "memmap", cmd_memmap, "memmap [-s] [-b]", "Show the UEFI memory map (-s summary only)",
      "  (none)  every memory descriptor, then totals per type\n"
      "  -s      totals per type only\n"
      "  -b      accepted for UEFI Shell compatibility and ignored\n"
      "Start and End are physical addresses; a page is 4 KiB. Total memory\n"
      "excludes reserved and MMIO ranges; \"available to the OS\" counts\n"
      "conventional, loader and boot services memory.\n"
      "With -data: type, start, end, pages, attributes (one record per\n"
      "descriptor); with -s: type, pages, bytes (one record per type).\n",
      CMD_DATA },
    { "sysinfo", cmd_sysinfo, "sysinfo", "Firmware, CPU, memory and display information",
      "Shows firmware vendor and revision, UEFI version, Secure Boot state, CPU,\n"
      "memory size, console size, graphics mode, configuration tables and the\n"
      "volume NESH was started from. Arguments are ignored.\n"
      "With -data: one record with firmware, firmware_revision, uefi,\n"
      "secure_boot, cpu, cpu_vendor, memory, console_columns, console_rows,\n"
      "graphics_width, graphics_height, framebuffer, config_tables, acpi,\n"
      "smbios, boot_volume, boot_devpath.\n",
      CMD_DATA },
};

void platform_cmds_init(void)
{
    shell_register(efi_sys_cmds, ARRAY_SIZE(efi_sys_cmds));
    efi_shell_protocol_install();
    efi_exit_hook = efi_shell_protocol_uninstall;
    efi_var_init();
    efi_boot_init();
    efi_drivers_init();
    efi_disk_init();
    efi_hw_init();
    efi_net_init();
}
