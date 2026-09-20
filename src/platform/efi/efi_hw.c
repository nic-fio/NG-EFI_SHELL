/* Memory and hardware: dmem, mm, pci, smbiosview, acpiview, mode, sermode,
 * loadpcirom, gop, cpuid. Writes are refused while Secure Boot is active. */
#include "efi_cmds.h"

/* ---- Small helpers ---- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static bool hexarg(const char *s, uint64_t *v)
{
    char *end;
    if (!s || !*s)
        return false;
    *v = strtoull(s, &end, 16);
    return !*end;
}

static void hexdump_at(const uint8_t *d, size_t n, uint64_t addr)
{
    for (size_t off = 0; off < n && !con_break(); off += 16) {
        out_printf("  %016llX: ", (unsigned long long)(addr + off));
        for (size_t k = 0; k < 16; k++) {
            if (off + k < n)
                out_printf("%02X%c", d[off + k], k == 7 ? '-' : ' ');
            else
                out_puts("   ");
        }
        out_puts(" *");
        for (size_t k = 0; k < 16 && off + k < n; k++)
            out_printf("%c", d[off + k] >= 0x20 && d[off + k] < 0x7f ? d[off + k] : '.');
        out_puts("*\n");
    }
}

static void *find_config_table(const EFI_GUID *g)
{
    for (UINTN k = 0; k < gST->NumberOfTableEntries; k++)
        if (!memcmp(&gST->ConfigurationTable[k].VendorGuid, g, sizeof(EFI_GUID)))
            return gST->ConfigurationTable[k].VendorTable;
    return NULL;
}

/* ---- PCI root bridge access ---- */

typedef EFI_STATUS(EFIAPI *RB_ACCESS)(void *This, UINTN Width, UINT64 Address, UINTN Count, void *Buffer);

typedef struct {
    EFI_HANDLE ParentHandle;
    void *PollMem, *PollIo;
    RB_ACCESS MemRead, MemWrite, IoRead, IoWrite, PciRead, PciWrite;
    void *CopyMem, *Map, *Unmap, *AllocateBuffer, *FreeBuffer, *Flush, *GetAttributes, *SetAttributes;
    EFI_STATUS(EFIAPI *Configuration)(void *This, void **Resources);
    UINT32 SegmentNumber;
} ROOT_BRIDGE;

static EFI_GUID rootbridge_guid = { 0x2F707EBB, 0x4A1A, 0x11D4, { 0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

static ROOT_BRIDGE **bridges(int *count)
{
    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    *count = 0;
    if (gBS->LocateHandleBuffer(ByProtocol, &rootbridge_guid, NULL, &n, &hs) != EFI_SUCCESS)
        return NULL;
    ROOT_BRIDGE **r = xcalloc(n, sizeof(ROOT_BRIDGE *));
    for (UINTN i = 0; i < n; i++)
        if (gBS->HandleProtocol(hs[i], &rootbridge_guid, (void **)&r[*count]) == EFI_SUCCESS)
            (*count)++;
    gBS->FreePool(hs);
    return r;
}

static ROOT_BRIDGE *bridge_for(UINT32 seg, UINT32 bus)
{
    int n;
    ROOT_BRIDGE **b = bridges(&n);
    ROOT_BRIDGE *found = NULL;
    for (int i = 0; i < n && !found; i++) {
        if (b[i]->SegmentNumber != seg)
            continue;
        /* bus range from the ACPI resource descriptors */
        uint8_t *res = NULL;
        bool in_range = true;
        if (b[i]->Configuration && b[i]->Configuration(b[i], (void **)&res) == EFI_SUCCESS && res) {
            for (uint8_t *p = res; p[0] == 0x8A;) {
                uint16_t len = rd16(p + 1);
                if (p[3] == 2) /* bus number resource */
                    in_range = bus >= rd64(p + 14) && bus <= rd64(p + 22);
                p += 3 + len;
            }
        }
        if (in_range)
            found = b[i];
    }
    free(b);
    return found;
}

#define PCI_ADDR(bus, dev, fn, reg) (((UINT64)(bus) << 24) | ((UINT64)(dev) << 16) | ((UINT64)(fn) << 8) | (reg))
/* extended registers (>= 0x100) go in bits 32..63 of the root bridge address */
#define PCIE_ADDR(bus, dev, fn, reg) (((UINT64)(reg) << 32) | PCI_ADDR(bus, dev, fn, 0))

static bool pci_read(UINT32 seg, UINT32 bus, UINT32 dev, UINT32 fn, UINT32 reg, int width, void *buf)
{
    ROOT_BRIDGE *rb = bridge_for(seg, bus);
    if (!rb)
        return false;
    UINT64 a = reg < 0x100 ? PCI_ADDR(bus, dev, fn, reg) : PCIE_ADDR(bus, dev, fn, reg);
    return rb->PciRead(rb, (UINTN)(width == 1 ? 0 : width == 2 ? 1 : width == 4 ? 2 : 3), a, 1, buf) == EFI_SUCCESS;
}

static bool pci_write(UINT32 seg, UINT32 bus, UINT32 dev, UINT32 fn, UINT32 reg, int width, void *buf)
{
    ROOT_BRIDGE *rb = bridge_for(seg, bus);
    if (!rb)
        return false;
    UINT64 a = reg < 0x100 ? PCI_ADDR(bus, dev, fn, reg) : PCIE_ADDR(bus, dev, fn, reg);
    return rb->PciWrite(rb, (UINTN)(width == 1 ? 0 : width == 2 ? 1 : width == 4 ? 2 : 3), a, 1, buf) == EFI_SUCCESS;
}

/* ---- dmem ---- */

static int cmd_dmem(int argc, char **argv)
{
    uint64_t addr = 0, size = 0x200;
    bool have_addr = false, mmio = false;
    int nops = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "-mmio"))
            mmio = true;
        else if (!strcasecmp(argv[i], "-b"))
            ;
        else if (nops == 0 && hexarg(argv[i], &addr))
            have_addr = true, nops++;
        else if (nops == 1 && hexarg(argv[i], &size))
            nops++;
        else
            return cmd_usage("dmem");
    }
    if (!have_addr) {
        /* like the UEFI Shell: show the system table */
        addr = (uint64_t)(uintptr_t)gST;
        size = gST->Hdr.HeaderSize;
        out_printf("EFI system table at 0x%llx\n", (unsigned long long)addr);
    }
    if (!size || size > 0x100000)
        return cmd_err("dmem", "size must be between 1 and 0x100000");
    uint8_t *buf = xmalloc((size_t)size);
    if (mmio) {
        ROOT_BRIDGE *rb = bridge_for(0, 0);
        if (!rb || rb->MemRead(rb, 0, addr, (UINTN)size, buf) != EFI_SUCCESS) {
            free(buf);
            return cmd_err("dmem", "MMIO read failed");
        }
    } else {
        memcpy(buf, (const void *)(uintptr_t)addr, (size_t)size);
    }
    out_printf("Memory address %016llX %llX bytes\n", (unsigned long long)addr, (unsigned long long)size);
    hexdump_at(buf, (size_t)size, addr);
    if (!have_addr) {
        out_printf("  FirmwareVendor %p, ConIn %p, ConOut %p, RuntimeServices %p, BootServices %p\n",
                   gST->FirmwareVendor, (void *)gST->ConIn, (void *)gST->ConOut, (void *)gST->RuntimeServices,
                   (void *)gST->BootServices);
        char gs[37];
        for (UINTN k = 0; k < gST->NumberOfTableEntries; k++)
            out_printf("  %-36s %p\n", guid_str(&gST->ConfigurationTable[k].VendorGuid, gs),
                       gST->ConfigurationTable[k].VendorTable);
    }
    free(buf);
    return RC_OK;
}

/* ---- mm ---- */

enum { MM_MEM, MM_MMIO, MM_IO, MM_PCI, MM_PCIE };

static inline uint64_t io_in(uint16_t port, int w)
{
    uint32_t v = 0;
    if (w == 1) {
        uint8_t b;
        __asm__ volatile("inb %1, %0" : "=a"(b) : "Nd"(port));
        v = b;
    } else if (w == 2) {
        uint16_t x;
        __asm__ volatile("inw %1, %0" : "=a"(x) : "Nd"(port));
        v = x;
    } else {
        __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    }
    return v;
}

static inline void io_out(uint16_t port, int w, uint64_t v)
{
    if (w == 1)
        __asm__ volatile("outb %0, %1" : : "a"((uint8_t)v), "Nd"(port));
    else if (w == 2)
        __asm__ volatile("outw %0, %1" : : "a"((uint16_t)v), "Nd"(port));
    else
        __asm__ volatile("outl %0, %1" : : "a"((uint32_t)v), "Nd"(port));
}

/* PCI addresses as in the UEFI Shell:
 *   -PCI  0x000000ssbbddffrr (segment, bus, device, function, register)
 *   -PCIE 0x0000ssbbddfffrrr (register up to 0xFFF) */
static void pci_split(int kind, uint64_t a, UINT32 *seg, UINT32 *bus, UINT32 *dev, UINT32 *fn, UINT32 *reg)
{
    if (kind == MM_PCI) {
        *reg = a & 0xFF, *fn = (a >> 8) & 0xFF, *dev = (a >> 16) & 0xFF, *bus = (a >> 24) & 0xFF, *seg = (a >> 32) & 0xFFFF;
    } else {
        *reg = a & 0xFFF, *fn = (a >> 12) & 0xFF, *dev = (a >> 20) & 0xFF, *bus = (a >> 28) & 0xFF, *seg = (a >> 36) & 0xFFFF;
    }
}

static bool mm_read(int kind, uint64_t a, int w, uint64_t *v)
{
    *v = 0;
    switch (kind) {
    case MM_MEM:
    case MM_MMIO:
        if (w == 1) *v = *(volatile uint8_t *)(uintptr_t)a;
        else if (w == 2) *v = *(volatile uint16_t *)(uintptr_t)a;
        else if (w == 4) *v = *(volatile uint32_t *)(uintptr_t)a;
        else *v = *(volatile uint64_t *)(uintptr_t)a;
        return true;
    case MM_IO:
        if (w == 8 || a > 0xFFFF)
            return false;
        *v = io_in((uint16_t)a, w);
        return true;
    default: {
        UINT32 s, b, d, f, r;
        pci_split(kind, a, &s, &b, &d, &f, &r);
        return pci_read(s, b, d, f, r, w, v);
    }
    }
}

static bool mm_write(int kind, uint64_t a, int w, uint64_t v)
{
    switch (kind) {
    case MM_MEM:
    case MM_MMIO:
        if (w == 1) *(volatile uint8_t *)(uintptr_t)a = (uint8_t)v;
        else if (w == 2) *(volatile uint16_t *)(uintptr_t)a = (uint16_t)v;
        else if (w == 4) *(volatile uint32_t *)(uintptr_t)a = (uint32_t)v;
        else *(volatile uint64_t *)(uintptr_t)a = v;
        return true;
    case MM_IO:
        if (w == 8 || a > 0xFFFF)
            return false;
        io_out((uint16_t)a, w, v);
        return true;
    default: {
        UINT32 s, b, d, f, r;
        pci_split(kind, a, &s, &b, &d, &f, &r);
        return pci_write(s, b, d, f, r, w, &v);
    }
    }
}

static int cmd_mm(int argc, char **argv)
{
    static const char *kinds[] = { "MEM", "MMIO", "IO", "PCI", "PCIE" };
    int kind = MM_MEM, w = 1;
    bool noninteractive = false, have_val = false;
    uint64_t addr = 0, val = 0;
    int nops = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int k = -1;
        for (int j = 0; j < 5; j++)
            if (a[0] == '-' && !strcasecmp(a + 1, kinds[j]))
                k = j;
        if (k >= 0) {
            kind = k;
        } else if (!strcasecmp(a, "-w") && i + 1 < argc) {
            w = (int)strtol(argv[++i], NULL, 10);
            if (w != 1 && w != 2 && w != 4 && w != 8)
                return cmd_err("mm", "width must be 1, 2, 4 or 8");
        } else if (!strcasecmp(a, "-n")) {
            noninteractive = true;
        } else if (nops == 0 && hexarg(a, &addr)) {
            nops++;
        } else if (nops == 1 && hexarg(a, &val)) {
            nops++;
            have_val = true;
        } else {
            return cmd_usage("mm");
        }
    }
    if (!nops)
        return cmd_usage("mm");
    if (addr % (uint64_t)w && kind != MM_IO && kind != MM_PCI && kind != MM_PCIE)
        return cmd_err("mm", "the address must be aligned to the width");
    if (have_val) {
        if (!hw_write_allowed("mm"))
            return RC_FAIL;
        if (!mm_write(kind, addr, w, val))
            return cmd_err("mm", "write failed");
        return RC_OK;
    }
    /* display; interactive: type a new value, Enter for the next, q to quit */
    for (int count = 0;; count++) {
        uint64_t v;
        if (!mm_read(kind, addr, w, &v))
            return cmd_err("mm", "read failed at %llx", (unsigned long long)addr);
        char *line = xasprintf("%-4s 0x%016llX : 0x%0*llX", kinds[kind], (unsigned long long)addr, w * 2,
                               (unsigned long long)v);
        if (noninteractive || !pal_con_interactive()) {
            out_printf("%s\n", line);
            free(line);
            return RC_OK;
        }
        char *prompt = xasprintf("%s > ", line);
        free(line);
        char *in = lineedit_read(prompt, false);
        free(prompt);
        if (!in || !strcasecmp(in, "q") || !strcasecmp(in, "quit")) {
            free(in);
            return RC_OK;
        }
        if (*in) {
            uint64_t nv;
            if (!hexarg(in, &nv)) {
                err_printf("mm: enter a hexadecimal value, Enter for the next address, q to quit\n");
                free(in);
                count--;
                continue;
            }
            if (!hw_write_allowed("mm")) {
                free(in);
                return RC_FAIL;
            }
            mm_write(kind, addr, w, nv);
        }
        free(in);
        addr += kind == MM_PCI || kind == MM_PCIE ? (uint64_t)w : (uint64_t)w;
    }
}

/* ---- pci ---- */

static const char *class_name(uint8_t base, uint8_t sub)
{
    static const char *bases[] = {
        "Unclassified", "Mass Storage Controller", "Network Controller", "Display Controller",
        "Multimedia Device", "Memory Controller", "Bridge Device", "Communication Controller",
        "System Peripheral", "Input Device", "Docking Station", "Processor", "Serial Bus Controller",
        "Wireless Controller", "Intelligent I/O", "Satellite Communication", "Encryption Controller",
        "Signal Processing", "Processing Accelerator", "Non-Essential Instrumentation",
    };
    static char buf[96];
    const char *b = base < ARRAY_SIZE(bases) ? bases[base] : base == 0xFF ? "Unassigned" : "Unknown";
    const char *s = NULL;
    switch (base << 8 | sub) {
    case 0x0100: s = "SCSI"; break;
    case 0x0101: s = "IDE controller"; break;
    case 0x0105: s = "ATA controller"; break;
    case 0x0106: s = "SATA controller"; break;
    case 0x0107: s = "SAS controller"; break;
    case 0x0108: s = "NVM Express"; break;
    case 0x0200: s = "Ethernet"; break;
    case 0x0280: s = "Other network"; break;
    case 0x0300: s = "VGA controller"; break;
    case 0x0302: s = "3D controller"; break;
    case 0x0403: s = "HD Audio"; break;
    case 0x0600: s = "Host/PCI bridge"; break;
    case 0x0601: s = "PCI/ISA bridge"; break;
    case 0x0604: s = "PCI/PCI bridge"; break;
    case 0x0680: s = "Other bridge type"; break;
    case 0x0700: s = "Serial controller"; break;
    case 0x0c03: s = "USB controller"; break;
    case 0x0c05: s = "SMBus"; break;
    case 0x0d11: s = "Bluetooth"; break;
    case 0x0d80: s = "Other wireless"; break;
    }
    if (s)
        snprintf(buf, sizeof(buf), "%s - %s", b, s);
    else
        snprintf(buf, sizeof(buf), "%s - subclass %02X", b, sub);
    return buf;
}

static void pci_list(UINT32 seg_filter, bool any_seg)
{
    bool data = out_data_mode();
    if (!data)
        out_puts("   Seg  Bus  Dev  Func\n   ---  ---  ---  ----\n");
    int n;
    ROOT_BRIDGE **rbs = bridges(&n);
    for (int i = 0; i < n && !con_break(); i++) {
        ROOT_BRIDGE *rb = rbs[i];
        if (!any_seg && rb->SegmentNumber != seg_filter)
            continue;
        UINT32 bmin = 0, bmax = 255;
        uint8_t *res = NULL;
        if (rb->Configuration && rb->Configuration(rb, (void **)&res) == EFI_SUCCESS && res)
            for (uint8_t *p = res; p[0] == 0x8A; p += 3 + rd16(p + 1))
                if (p[3] == 2)
                    bmin = (UINT32)rd64(p + 14), bmax = (UINT32)rd64(p + 22);
        for (UINT32 bus = bmin; bus <= bmax && bus < 256; bus++) {
            for (UINT32 dev = 0; dev < 32; dev++) {
                for (UINT32 fn = 0; fn < 8; fn++) {
                    UINT32 id = 0xFFFFFFFF;
                    rb->PciRead(rb, 2, PCI_ADDR(bus, dev, fn, 0), 1, &id);
                    if ((id & 0xFFFF) == 0xFFFF) {
                        if (fn == 0)
                            break;
                        continue;
                    }
                    UINT32 cls = 0;
                    UINT8 hdr = 0;
                    rb->PciRead(rb, 2, PCI_ADDR(bus, dev, fn, 8), 1, &cls);
                    rb->PciRead(rb, 0, PCI_ADDR(bus, dev, fn, 0x0E), 1, &hdr);
                    if (data) {
                        data_record();
                        data_field("segment", "%02X", rb->SegmentNumber);
                        data_field("bus", "%02X", bus);
                        data_field("device", "%02X", dev);
                        data_field("function", "%02X", fn);
                        data_field("vendor", "%04X", id & 0xFFFF);
                        data_field("deviceid", "%04X", id >> 16);
                        data_field("class", "%06X", cls >> 8);
                        data_field("classname", "%s", class_name((uint8_t)(cls >> 24), (uint8_t)(cls >> 16)));
                    } else {
                    out_printf("    %02X   %02X   %02X    %02X ==> %s\n", rb->SegmentNumber, bus, dev, fn,
                               class_name((uint8_t)(cls >> 24), (uint8_t)(cls >> 16)));
                    out_printf("             Vendor %04X Device %04X Prog Interface %X\n", id & 0xFFFF, id >> 16,
                               (cls >> 8) & 0xFF);
                    }
                    if (fn == 0 && !(hdr & 0x80))
                        break; /* single-function device */
                }
            }
        }
    }
    free(rbs);
}

static void pci_interpret(const uint8_t *c)
{
    uint16_t vid = rd16(c), did = rd16(c + 2), cmd = rd16(c + 4), sts = rd16(c + 6);
    out_printf("  Vendor ID %04X  Device ID %04X  Revision %02X\n", vid, did, c[8]);
    out_printf("  Class %02X%02X%02X: %s\n", c[0xB], c[0xA], c[9], class_name(c[0xB], c[0xA]));
    out_printf("  Command %04X (I/O %s, memory %s, bus master %s)  Status %04X%s\n", cmd, cmd & 1 ? "on" : "off",
               cmd & 2 ? "on" : "off", cmd & 4 ? "on" : "off", sts, sts & 0x10 ? " (capabilities)" : "");
    uint8_t ht = c[0xE] & 0x7F;
    out_printf("  Header type %02X (%s%s)\n", c[0xE], ht == 0 ? "device" : ht == 1 ? "PCI-PCI bridge" : "CardBus bridge",
               c[0xE] & 0x80 ? ", multi-function" : "");
    int nbars = ht == 0 ? 6 : ht == 1 ? 2 : 0;
    for (int i = 0; i < nbars; i++) {
        uint32_t bar = rd32(c + 0x10 + 4 * i);
        if (!bar)
            continue;
        if (bar & 1) {
            out_printf("  BAR%d  I/O  0x%X\n", i, bar & ~3u);
        } else {
            bool is64 = ((bar >> 1) & 3) == 2;
            uint64_t base = bar & ~0xFull;
            if (is64 && i + 1 < nbars)
                base |= (uint64_t)rd32(c + 0x10 + 4 * (i + 1)) << 32;
            out_printf("  BAR%d  MEM%s 0x%llX%s\n", i, is64 ? "64" : "32", (unsigned long long)base,
                       bar & 8 ? " prefetchable" : "");
            if (is64)
                i++;
        }
    }
    if (ht == 1)
        out_printf("  Buses: primary %02X secondary %02X subordinate %02X\n", c[0x18], c[0x19], c[0x1A]);
    else if (ht == 0)
        out_printf("  Subsystem %04X:%04X\n", rd16(c + 0x2C), rd16(c + 0x2E));
    out_printf("  Interrupt line %02X pin %02X\n", c[0x3C], c[0x3D]);
    if (sts & 0x10) {
        static const struct { uint8_t id; const char *name; } caps[] = {
            { 1, "Power Management" }, { 5, "MSI" }, { 9, "Vendor Specific" }, { 0x10, "PCI Express" },
            { 0x11, "MSI-X" }, { 0x12, "SATA" }, { 0x13, "Advanced Features" },
        };
        out_puts("  Capabilities:");
        uint8_t p = c[0x34] & 0xFC;
        for (int guard = 0; p && guard < 48; guard++) {
            const char *nm = NULL;
            for (size_t k = 0; k < ARRAY_SIZE(caps); k++)
                if (caps[k].id == c[p])
                    nm = caps[k].name;
            out_printf(" [%02X] %s", p, nm ? nm : "?");
            p = c[p + 1] & 0xFC;
        }
        out_puts("\n");
    }
}

/* -data for one device: the decoded header as a single record */
static void pci_data(const uint8_t *c, UINT32 seg, UINT32 bus, UINT32 dev, UINT32 fn)
{
    uint8_t ht = c[0xE] & 0x7F;
    data_record();
    data_field("segment", "%02X", seg);
    data_field("bus", "%02X", bus);
    data_field("device", "%02X", dev);
    data_field("function", "%02X", fn);
    data_field("vendor", "%04X", rd16(c));
    data_field("deviceid", "%04X", rd16(c + 2));
    data_field("revision", "%02X", c[8]);
    data_field("class", "%02X%02X%02X", c[0xB], c[0xA], c[9]);
    data_field("classname", "%s", class_name(c[0xB], c[0xA]));
    data_field("command", "%04X", rd16(c + 4));
    data_field("status", "%04X", rd16(c + 6));
    data_field("headertype", "%02X", c[0xE]);
    int nbars = ht == 0 ? 6 : ht == 1 ? 2 : 0;
    for (int i = 0; i < nbars; i++) {
        uint32_t bar = rd32(c + 0x10 + 4 * i);
        char key[8];
        snprintf(key, sizeof(key), "bar%d", i);
        if (!bar)
            continue;
        if (bar & 1) {
            data_field(key, "io 0x%X", bar & ~3u);
        } else {
            bool is64 = ((bar >> 1) & 3) == 2;
            uint64_t base = bar & ~0xFull;
            if (is64 && i + 1 < nbars)
                base |= (uint64_t)rd32(c + 0x10 + 4 * (i + 1)) << 32;
            data_field(key, "mem%s 0x%llX%s", is64 ? "64" : "32", (unsigned long long)base, bar & 8 ? " prefetchable" : "");
            if (is64)
                i++;
        }
    }
    if (ht == 1) {
        data_field("primarybus", "%02X", c[0x18]);
        data_field("secondarybus", "%02X", c[0x19]);
        data_field("subordinatebus", "%02X", c[0x1A]);
    } else if (ht == 0) {
        data_field("subsystem", "%04X:%04X", rd16(c + 0x2C), rd16(c + 0x2E));
    }
    data_field("irqline", "%02X", c[0x3C]);
    data_field("irqpin", "%02X", c[0x3D]);
}

static int cmd_pci(int argc, char **argv)
{
    uint64_t v[3];
    int nv = 0;
    uint64_t seg = 0;
    bool interp = false, ext = false, have_seg = false;
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "-s") && i + 1 < argc) {
            if (!hexarg(argv[++i], &seg))
                return cmd_usage("pci");
            have_seg = true;
        } else if (!strcasecmp(argv[i], "-i")) {
            interp = true;
        } else if (!strcasecmp(argv[i], "-ec")) {
            ext = true;
        } else if (!strcasecmp(argv[i], "-b")) {
        } else if (nv < 3 && hexarg(argv[i], &v[nv])) {
            nv++;
        } else {
            return cmd_usage("pci");
        }
    }
    if (nv == 0) {
        pci_list((UINT32)seg, !have_seg);
        return RC_OK;
    }
    if (nv == 1)
        return cmd_usage("pci");
    UINT32 bus = (UINT32)v[0], dev = (UINT32)v[1], fn = nv > 2 ? (UINT32)v[2] : 0;
    size_t size = ext ? 4096 : 256;
    uint8_t *c = xcalloc(1, size);
    for (size_t off = 0; off < size; off += 4) {
        if (!pci_read((UINT32)seg, bus, dev, fn, (UINT32)off, 4, c + off)) {
            if (off < 256) {
                free(c);
                return cmd_err("pci", "cannot read the configuration space of %02X:%02X.%X", bus, dev, fn);
            }
            size = off; /* no extended configuration space */
            break;
        }
    }
    if (rd16(c) == 0xFFFF) {
        free(c);
        return cmd_err("pci", "no device at segment %llX bus %02X device %02X function %X", (unsigned long long)seg,
                       bus, dev, fn);
    }
    if (out_data_mode()) {
        pci_data(c, (UINT32)seg, bus, dev, fn);
        free(c);
        return RC_OK;
    }
    out_printf("PCI Segment %02llX Bus %02X Device %02X Func %02X\n", (unsigned long long)seg, bus, dev, fn);
    hexdump_at(c, size, 0);
    if (interp)
        pci_interpret(c);
    free(c);
    return RC_OK;
}

/* ---- smbiosview ---- */

static const char *smbios_string(const uint8_t *s, uint8_t idx)
{
    if (!idx)
        return "";
    const char *p = (const char *)s + s[1];
    for (uint8_t i = 1; i < idx && *p; i++)
        p += strlen(p) + 1;
    return *p ? p : "<bad string>";
}

static const uint8_t *smbios_next(const uint8_t *s)
{
    const uint8_t *p = s + s[1];
    while (p[0] || p[1])
        p++;
    return p + 2;
}

static const char *smbios_type_name(uint8_t t)
{
    static const char *names[] = {
        "BIOS Information", "System Information", "Baseboard Information", "System Enclosure",
        "Processor Information", "Memory Controller", "Memory Module", "Cache Information",
        "Port Connector", "System Slots", "On Board Devices", "OEM Strings", "System Configuration Options",
        "BIOS Language", "Group Associations", "System Event Log", "Physical Memory Array", "Memory Device",
        "32-bit Memory Error", "Memory Array Mapped Address", "Memory Device Mapped Address",
        "Built-in Pointing Device", "Portable Battery", "System Reset", "Hardware Security",
        "System Power Controls", "Voltage Probe", "Cooling Device", "Temperature Probe",
        "Electrical Current Probe", "Out-of-Band Remote Access", "BIS Entry Point", "System Boot Information",
        "64-bit Memory Error", "Management Device", "Management Device Component",
        "Management Device Threshold", "Memory Channel", "IPMI Device", "Power Supply", "Additional Information",
        "Onboard Devices Extended", "Management Controller Host Interface", "TPM Device", "Processor Additional",
    };
    if (t < ARRAY_SIZE(names))
        return names[t];
    return t == 127 ? "End of Table" : t >= 128 ? "OEM specific" : "Unknown";
}

#define SI(label, ...) info_line(4, 0, label, __VA_ARGS__)

static void smbios_decode(const uint8_t *s)
{
    uint8_t len = s[1];
#define STR(off) (len > (off) ? smbios_string(s, s[off]) : "")
    switch (s[0]) {
    case 0:
        SI("Vendor", "%s", STR(4));
        SI("Version", "%s", STR(5));
        SI("Release date", "%s", STR(8));
        if (len > 9)
            SI("ROM size", "%u KiB", (s[9] + 1) * 64);
        if (len > 0x15 && s[0x14] != 0xFF)
            SI("BIOS release", "%u.%u", s[0x14], s[0x15]);
        break;
    case 1:
        SI("Manufacturer", "%s", STR(4));
        SI("Product", "%s", STR(5));
        SI("Version", "%s", STR(6));
        SI("Serial", "%s", STR(7));
        if (len >= 0x19) {
            const uint8_t *u = s + 8;
            SI("UUID", "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[3], u[2], u[1], u[0],
               u[5], u[4], u[7], u[6], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
        }
        if (len > 0x1A) {
            SI("SKU", "%s", STR(0x19));
            SI("Family", "%s", STR(0x1A));
        }
        break;
    case 2:
    case 3:
        SI("Manufacturer", "%s", STR(4));
        if (s[0] == 2)
            SI("Product", "%s", STR(5));
        SI("Version", "%s", STR(6));
        SI("Serial", "%s", STR(7));
        SI("Asset tag", "%s", STR(8));
        break;
    case 4:
        SI("Socket", "%s", STR(4));
        SI("Manufacturer", "%s", STR(7));
        SI("Version", "%s", STR(0x10));
        if (len > 0x17) {
            SI("Current speed", "%u MHz", rd16(s + 0x16));
            SI("Max speed", "%u MHz", rd16(s + 0x14));
            SI("External clock", "%u MHz", rd16(s + 0x12));
        }
        if (len > 0x25) {
            SI("Cores", "%u", s[0x23]);
            SI("Enabled cores", "%u", s[0x24]);
            SI("Threads", "%u", s[0x25]);
        }
        break;
    case 16:
        if (len > 0x0E) {
            SI("Maximum capacity", "%u MiB", rd32(s + 7) / 1024);
            SI("Devices", "%u", rd16(s + 0x0D));
        }
        break;
    case 17: {
        uint16_t sz = len > 0x0D ? rd16(s + 0x0C) : 0;
        uint32_t mb = sz == 0x7FFF && len > 0x1F ? rd32(s + 0x1C) : (sz & 0x8000 ? (sz & 0x7FFF) / 1024 : sz);
        SI("Locator", "%s", STR(0x10));
        SI("Bank", "%s", STR(0x11));
        SI("Size", "%u MiB%s", mb, sz ? "" : " (empty)");
        if (len > 0x16)
            SI("Speed", "%u MT/s", rd16(s + 0x15));
        if (len > 0x1A) {
            SI("Manufacturer", "%s", STR(0x17));
            SI("Serial", "%s", STR(0x18));
            SI("Part number", "%s", STR(0x1A));
        }
        break;
    }
    case 19:
        if (len > 0x0B) {
            SI("Start", "0x%llX", (unsigned long long)rd32(s + 4) * 1024);
            SI("End", "0x%llX", (unsigned long long)rd32(s + 8) * 1024 + 1023);
        }
        break;
    default:
        break;
    }
#undef STR
}
#undef SI

static int cmd_smbiosview(int argc, char **argv)
{
    int64_t want_type = -1, want_handle = -1;
    bool stats = false;
    for (int i = 1; i < argc; i++) {
        uint64_t v;
        if (!strcasecmp(argv[i], "-t") && i + 1 < argc && parse_int(argv[i + 1], &want_type)) {
            i++; /* decimal type, as in the UEFI Shell */
        } else if (!strcasecmp(argv[i], "-h") && i + 1 < argc && hexarg(argv[i + 1], &v)) {
            want_handle = (int64_t)v;
            i++;
        } else if (!strcasecmp(argv[i], "-s")) {
            stats = true;
        } else if (!strcasecmp(argv[i], "-a") || !strcasecmp(argv[i], "-b")) {
        } else {
            return cmd_usage("smbiosview");
        }
    }
    EFI_GUID g3 = EFI_SMBIOS3_TABLE_GUID, g2 = EFI_SMBIOS_TABLE_GUID;
    const uint8_t *ep3 = find_config_table(&g3), *ep2 = find_config_table(&g2);
    const uint8_t *table;
    uint64_t tlen;
    bool data = out_data_mode();
    if (ep3 && !memcmp(ep3, "_SM3_", 5)) {
        if (!data)
            out_printf("SMBIOS %u.%u (64-bit entry point at %p)\n", ep3[7], ep3[8], (const void *)ep3);
        table = (const uint8_t *)(uintptr_t)rd64(ep3 + 0x10);
        tlen = rd32(ep3 + 0x0C);
    } else if (ep2 && !memcmp(ep2, "_SM_", 4)) {
        if (!data)
            out_printf("SMBIOS %u.%u (32-bit entry point at %p), %u structures\n", ep2[6], ep2[7], (const void *)ep2,
                   rd16(ep2 + 0x1C));
        table = (const uint8_t *)(uintptr_t)rd32(ep2 + 0x18);
        tlen = rd16(ep2 + 0x16);
    } else {
        return cmd_err("smbiosview", "no SMBIOS table found");
    }
    int counts[256] = { 0 };
    for (const uint8_t *s = table; s + 4 <= table + tlen && !con_break(); s = smbios_next(s)) {
        if (s[1] < 4)
            break;
        counts[s[0]]++;
        bool show = (want_type < 0 || s[0] == want_type) && (want_handle < 0 || rd16(s + 2) == want_handle);
        if (show && !stats && data) {
            data_record();
            data_field("type", "%u", s[0]);
            data_field("typename", "%s", smbios_type_name(s[0]));
            data_field("handle", "%04X", rd16(s + 2));
            data_field("length", "%u", s[1]);
            smbios_decode(s);
        } else if (show && !stats) {
            out_printf("Type %u (%s)  Handle 0x%04X  Length %u\n", s[0], smbios_type_name(s[0]), rd16(s + 2), s[1]);
            smbios_decode(s);
        }
        if (s[0] == 127)
            break;
    }
    if (stats) {
        if (!data)
            out_puts("Type  Count  Name\n");
        for (int t = 0; t < 256; t++) {
            if (counts[t] && data) {
                data_record();
                data_field("type", "%d", t);
                data_field("count", "%d", counts[t]);
                data_field("typename", "%s", smbios_type_name((uint8_t)t));
            } else if (counts[t])
                out_printf("%4d  %5d  %s\n", t, counts[t], smbios_type_name((uint8_t)t));
        }
    }
    return RC_OK;
}

/* ---- acpiview ---- */

typedef struct {
    const uint8_t *p;
    char sig[5];
} AcpiTable;

static AcpiTable *acpi_tables(int *count, const uint8_t **rsdp_out)
{
    EFI_GUID g2 = EFI_ACPI_20_TABLE_GUID, g1 = EFI_ACPI_10_TABLE_GUID;
    const uint8_t *rsdp = find_config_table(&g2);
    if (!rsdp)
        rsdp = find_config_table(&g1);
    *count = 0;
    *rsdp_out = rsdp;
    if (!rsdp || memcmp(rsdp, "RSD PTR ", 8))
        return NULL;
    AcpiTable *t = NULL;
    int n = 0;
    const uint8_t *sdt;
    int esize;
    if (rsdp[15] >= 2 && rd64(rsdp + 24)) {
        sdt = (const uint8_t *)(uintptr_t)rd64(rsdp + 24);
        esize = 8;
    } else {
        sdt = (const uint8_t *)(uintptr_t)rd32(rsdp + 16);
        esize = 4;
    }
    uint32_t len = rd32(sdt + 4);
    t = xrealloc(t, sizeof(AcpiTable) * (n + 1));
    t[n].p = sdt;
    memcpy(t[n].sig, sdt, 4);
    t[n++].sig[4] = 0;
    for (uint32_t off = 36; off + esize <= len; off += esize) {
        const uint8_t *tab = (const uint8_t *)(uintptr_t)(esize == 8 ? rd64(sdt + off) : rd32(sdt + off));
        if (!tab)
            continue;
        t = xrealloc(t, sizeof(AcpiTable) * (n + 3));
        t[n].p = tab;
        memcpy(t[n].sig, tab, 4);
        t[n++].sig[4] = 0;
        if (!memcmp(tab, "FACP", 4)) {
            uint32_t flen = rd32(tab + 4);
            uint64_t dsdt = flen >= 148 && rd64(tab + 140) ? rd64(tab + 140) : rd32(tab + 40);
            uint64_t facs = flen >= 140 && rd64(tab + 132) ? rd64(tab + 132) : rd32(tab + 36);
            if (dsdt) {
                t[n].p = (const uint8_t *)(uintptr_t)dsdt;
                memcpy(t[n].sig, t[n].p, 4);
                t[n++].sig[4] = 0;
            }
            if (facs) {
                t[n].p = (const uint8_t *)(uintptr_t)facs;
                memcpy(t[n].sig, t[n].p, 4);
                t[n++].sig[4] = 0;
            }
        }
    }
    *count = n;
    return t;
}

static uint32_t acpi_len(const uint8_t *t)
{
    return rd32(t + 4);
}

static bool acpi_checksum_ok(const uint8_t *t)
{
    if (!memcmp(t, "FACS", 4))
        return true; /* FACS has no checksum */
    uint8_t sum = 0;
    for (uint32_t i = 0; i < acpi_len(t); i++)
        sum += t[i];
    return sum == 0;
}

static void acpi_decode(const uint8_t *t)
{
    uint32_t len = acpi_len(t);
    if (!memcmp(t, "APIC", 4)) {
        out_printf("  Local APIC address 0x%08X, flags %X\n", rd32(t + 36), rd32(t + 40));
        for (uint32_t off = 44; off + 2 <= len && t[off + 1];) {
            const uint8_t *e = t + off;
            switch (e[0]) {
            case 0: out_printf("  Processor local APIC: ACPI id %u, APIC id %u, %s\n", e[2], e[3], e[4] & 1 ? "enabled" : "disabled"); break;
            case 1: out_printf("  I/O APIC: id %u, address 0x%08X, GSI base %u\n", e[2], rd32(e + 4), rd32(e + 8)); break;
            case 2: out_printf("  Interrupt override: bus %u, source %u -> GSI %u, flags %X\n", e[2], e[3], rd32(e + 4), rd16(e + 8)); break;
            case 4: out_printf("  Local APIC NMI: processor %u, LINT%u\n", e[2], e[5]); break;
            case 9: out_printf("  Processor x2APIC: id %u, %s\n", rd32(e + 4), e[8] & 1 ? "enabled" : "disabled"); break;
            default: out_printf("  Entry type %u, length %u\n", e[0], e[1]); break;
            }
            off += e[1];
        }
    } else if (!memcmp(t, "MCFG", 4)) {
        for (uint32_t off = 44; off + 16 <= len; off += 16)
            out_printf("  PCI Express ECAM: base 0x%llX, segment %u, buses %02X-%02X\n",
                       (unsigned long long)rd64(t + off), rd16(t + off + 8), t[off + 10], t[off + 11]);
    } else if (!memcmp(t, "HPET", 4) && len >= 56) {
        out_printf("  HPET base address 0x%llX, number %u\n", (unsigned long long)rd64(t + 44), t[52]);
    } else if (!memcmp(t, "FACP", 4)) {
        out_printf("  FACS 0x%X, DSDT 0x%X, SCI interrupt %u, PM1a event block 0x%X, profile %u\n", rd32(t + 36),
                   rd32(t + 40), rd16(t + 46), rd32(t + 56), t[45]);
    }
}

static void acpi_header(const AcpiTable *a)
{
    const uint8_t *t = a->p;
    if (out_data_mode()) {
        bool facs = !memcmp(t, "FACS", 4);
        data_record();
        data_field("signature", "%s", a->sig);
        data_field("address", "0x%llX", (unsigned long long)(uintptr_t)t);
        data_field("length", "%u", acpi_len(t));
        if (facs) {
            data_field("revision", "%s", "");
            data_field("oemid", "%s", "");
            data_field("oemtable", "%s", "");
        } else {
            data_field("revision", "%u", t[8]);
            data_field("oemid", "%.6s", t + 10);
            data_field("oemtable", "%.8s", t + 16);
        }
        data_field("checksum", "%s", facs ? "none" : acpi_checksum_ok(t) ? "ok" : "bad");
        return;
    }
    if (!memcmp(t, "FACS", 4)) {
        out_printf("%-4s  %p  %6u  FACS\n", a->sig, (const void *)t, acpi_len(t));
        return;
    }
    out_printf("%-4s  %p  %6u  rev %-2u  %.6s  %.8s  %s\n", a->sig, (const void *)t, acpi_len(t), t[8], t + 10, t + 16,
               acpi_checksum_ok(t) ? "checksum ok" : "BAD CHECKSUM");
}

static int cmd_acpiview(int argc, char **argv)
{
    const char *sig = NULL;
    bool dump = false, list = false;
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "-s") && i + 1 < argc)
            sig = argv[++i];
        else if (!strcasecmp(argv[i], "-d"))
            dump = true;
        else if (!strcasecmp(argv[i], "-l"))
            list = true;
        else if (!strcasecmp(argv[i], "-r") && i + 1 < argc)
            i++; /* spec version: accepted */
        else if (!strcasecmp(argv[i], "-q") || !strcasecmp(argv[i], "-h"))
            ;
        else
            return cmd_usage("acpiview");
    }
    (void)list;
    if (dump && out_data_mode())
        return cmd_err("acpiview", "-d cannot be used with -data");
    int n;
    const uint8_t *rsdp;
    AcpiTable *t = acpi_tables(&n, &rsdp);
    if (!t)
        return cmd_err("acpiview", "no ACPI tables found");
    bool data = out_data_mode();
    if (!sig) {
        if (!data) {
            out_printf("RSDP at %p, revision %u, OEM %.6s\n", (const void *)rsdp, rsdp[15], rsdp + 9);
            out_puts("Sig   Address           Length  Rev     OEM     Table\n");
        }
        for (int i = 0; i < n; i++)
            acpi_header(&t[i]);
        free(t);
        return RC_OK;
    }
    int rc = RC_FAIL, found = 0;
    for (int i = 0; i < n; i++) {
        if (strncasecmp(t[i].sig, sig, 4))
            continue;
        found++;
        acpi_header(&t[i]);
        if (dump) {
            char *name = found > 1 ? xasprintf("%s%d.bin", t[i].sig, found) : xasprintf("%s.bin", t[i].sig);
            char *path = path_resolve(name);
            int e = path ? file_write_all(path, (const char *)t[i].p, acpi_len(t[i].p), false) : PAL_EINVAL;
            if (e)
                cmd_perr("acpiview", name, e);
            else
                out_printf("  saved to %s\n", path);
            free(path);
            free(name);
        } else if (!data) {
            acpi_decode(t[i].p);
            hexdump_at(t[i].p, acpi_len(t[i].p), (uint64_t)(uintptr_t)t[i].p);
        }
        rc = RC_OK;
    }
    free(t);
    if (!found)
        return cmd_err("acpiview", "table %s not found (acpiview lists them)", sig);
    return rc;
}

/* ---- mode (text console) ---- */

static int cmd_mode(int argc, char **argv)
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *o = gST->ConOut;
    if (argc == 1) {
        out_puts("Available text modes:\n");
        for (INT32 m = 0; m < o->Mode->MaxMode; m++) {
            UINTN c, r;
            if (o->QueryMode(o, (UINTN)m, &c, &r) == EFI_SUCCESS)
                out_printf("  %2d: %3llu columns x %3llu rows%s\n", m, (unsigned long long)c, (unsigned long long)r,
                           m == o->Mode->Mode ? "  (current)" : "");
        }
        return RC_OK;
    }
    int64_t c, r;
    if (argc != 3 || !parse_int(argv[1], &c) || !parse_int(argv[2], &r))
        return cmd_usage("mode");
    for (INT32 m = 0; m < o->Mode->MaxMode; m++) {
        UINTN mc, mr;
        if (o->QueryMode(o, (UINTN)m, &mc, &mr) == EFI_SUCCESS && (int64_t)mc == c && (int64_t)mr == r) {
            EFI_STATUS st = o->SetMode(o, (UINTN)m);
            return EFI_ERROR(st) ? cmd_err("mode", "%s", efi_strerror(st)) : RC_OK;
        }
    }
    return cmd_err("mode", "no text mode with %lld columns and %lld rows (mode lists them)", (long long)c, (long long)r);
}

/* ---- sermode ---- */

typedef struct {
    UINT32 ControlMask, Timeout;
    UINT64 BaudRate;
    UINT32 ReceiveFifoDepth, DataBits, Parity, StopBits;
} SERIAL_MODE;

typedef struct {
    UINT32 Revision;
    void *Reset;
    EFI_STATUS(EFIAPI *SetAttributes)(void *This, UINT64 Baud, UINT32 Fifo, UINT32 Timeout, UINT32 Parity,
                                      UINT8 DataBits, UINT32 StopBits);
    void *SetControl, *GetControl, *Write, *Read;
    SERIAL_MODE *Mode;
} SERIAL_IO;

static int cmd_sermode(int argc, char **argv)
{
    static EFI_GUID serial_guid = { 0xBB25CF6F, 0xF1D4, 0x11D2, { 0x9A, 0x0C, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0xFD } };
    static const char *parity[] = { "default", "none", "even", "odd", "mark", "space" };
    static const char *stops[] = { "default", "1", "1.5", "2" };
    if (argc == 1 || argc == 2) {
        UINTN n = 0;
        EFI_HANDLE *hs = NULL;
        EFI_HANDLE only = NULL;
        if (argc == 2 && !efi_parse_handle(argv[1], &only))
            return cmd_err("sermode", "%s is not a handle", argv[1]);
        if (gBS->LocateHandleBuffer(ByProtocol, &serial_guid, NULL, &n, &hs) != EFI_SUCCESS) {
            out_puts("No serial ports.\n");
            return RC_OK;
        }
        for (UINTN i = 0; i < n; i++) {
            SERIAL_IO *s;
            if ((only && hs[i] != only) || gBS->HandleProtocol(hs[i], &serial_guid, (void **)&s) != EFI_SUCCESS)
                continue;
            SERIAL_MODE *m = s->Mode;
            out_printf("Handle %X: %llu baud, parity %s, %u data bits, stop bits %s\n", efi_handle_index(hs[i]),
                       (unsigned long long)m->BaudRate, m->Parity < 6 ? parity[m->Parity] : "?", m->DataBits,
                       m->StopBits < 4 ? stops[m->StopBits] : "?");
        }
        gBS->FreePool(hs);
        return RC_OK;
    }
    if (argc != 6)
        return cmd_usage("sermode");
    EFI_HANDLE h;
    SERIAL_IO *s;
    if (!efi_parse_handle(argv[1], &h) || gBS->HandleProtocol(h, &serial_guid, (void **)&s) != EFI_SUCCESS)
        return cmd_err("sermode", "%s is not a serial port handle", argv[1]);
    int64_t baud, bits;
    const char *p = argv[3];
    int par = tolower((uint8_t)p[0]) == 'n' ? 1 : tolower((uint8_t)p[0]) == 'e' ? 2 : tolower((uint8_t)p[0]) == 'o' ? 3
            : tolower((uint8_t)p[0]) == 'm' ? 4 : tolower((uint8_t)p[0]) == 's' ? 5 : tolower((uint8_t)p[0]) == 'd' ? 0 : -1;
    int stop = !strcmp(argv[5], "0") ? 0 : !strcmp(argv[5], "1") ? 1 : !strcmp(argv[5], "1.5") || !strcmp(argv[5], "15") ? 2 : !strcmp(argv[5], "2") ? 3 : -1;
    if (!parse_int(argv[2], &baud) || !parse_int(argv[4], &bits) || par < 0 || stop < 0 || bits < 5 || bits > 8)
        return cmd_err("sermode", "usage: sermode HANDLE BAUD d|n|e|o|m|s DATABITS(5-8) 0|1|1.5|2");
    EFI_STATUS st = s->SetAttributes(s, (UINT64)baud, s->Mode->ReceiveFifoDepth, s->Mode->Timeout, (UINT32)par,
                                     (UINT8)bits, (UINT32)stop);
    return EFI_ERROR(st) ? cmd_err("sermode", "%s", efi_strerror(st)) : RC_OK;
}

/* ---- loadpcirom ---- */

typedef struct {
    EFI_STATUS(EFIAPI *GetInfo)(void *This, void *Source, UINT32 SourceSize, UINT32 *DestinationSize,
                                UINT32 *ScratchSize);
    EFI_STATUS(EFIAPI *Decompress)(void *This, void *Source, UINT32 SourceSize, void *Destination,
                                   UINT32 DestinationSize, void *Scratch, UINT32 ScratchSize);
} DECOMPRESS;

int platform_fw_decompress(const uint8_t *in, size_t n, char **out, size_t *out_len)
{
    static EFI_GUID g = { 0xD8117CFE, 0x94A6, 0x11D4, { 0x9A, 0x3A, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };
    DECOMPRESS *d;
    if (gBS->LocateProtocol(&g, NULL, (void **)&d) != EFI_SUCCESS)
        return -2;
    UINT32 dsize, ssize;
    if (d->GetInfo(d, (void *)in, (UINT32)n, &dsize, &ssize) != EFI_SUCCESS)
        return -1;
    char *dst = xmalloc(dsize ? dsize : 1);
    void *scratch = xmalloc(ssize ? ssize : 1);
    EFI_STATUS st = d->Decompress(d, (void *)in, (UINT32)n, dst, dsize, scratch, ssize);
    free(scratch);
    if (EFI_ERROR(st)) {
        free(dst);
        return -1;
    }
    *out = dst;
    *out_len = dsize;
    return 0;
}

static int cmd_loadpcirom(int argc, char **argv)
{
    static EFI_GUID decompress_guid = { 0xD8117CFE, 0x94A6, 0x11D4, { 0x9A, 0x3A, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };
    bool noconnect = false;
    int rc = RC_OK, loaded = 0, files = 0;
    for (int i = 1; i < argc; i++)
        files += strcasecmp(argv[i], "-nc") != 0;
    if (!files)
        return cmd_usage("loadpcirom");
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "-nc")) {
            noconnect = true;
            continue;
        }
        char *path = path_resolve(argv[i]);
        char *rom;
        size_t len;
        int e = path ? file_read_all(path, &rom, &len) : PAL_ENOENT;
        if (e) {
            rc = cmd_perr("loadpcirom", argv[i], e);
            free(path);
            continue;
        }
        const uint8_t *r = (const uint8_t *)rom;
        int images = 0;
        for (size_t off = 0; off + 0x1A <= len;) {
            const uint8_t *h = r + off;
            if (rd16(h) != 0xAA55)
                break;
            uint16_t pcir = rd16(h + 0x18);
            if (off + pcir + 0x18 > len || memcmp(h + pcir, "PCIR", 4))
                break;
            const uint8_t *pc = h + pcir;
            size_t img_len = (size_t)rd16(pc + 0x10) * 512;
            bool last = pc[0x15] & 0x80;
            if (pc[0x14] == 3 && rd32(h + 4) == 0x0EF1) { /* EFI image */
                uint16_t subsys = rd16(h + 8), machine = rd16(h + 10), comp = rd16(h + 12), hoff = rd16(h + 0x16);
                size_t init = (size_t)rd16(h + 2) * 512;
                if (machine != 0x8664) {
                    out_printf("Image %d: machine %04X, not for x86_64: skipped\n", images, machine);
                } else if (hoff >= init || off + init > len) {
                    rc = cmd_err("loadpcirom", "image %d: damaged header", images);
                } else {
                    void *img = (void *)(h + hoff);
                    UINT32 size = (UINT32)(init - hoff);
                    void *dec = NULL;
                    if (comp == 1) { /* EFI compression */
                        DECOMPRESS *d;
                        UINT32 dsize, ssize;
                        if (gBS->LocateProtocol(&decompress_guid, NULL, (void **)&d) != EFI_SUCCESS ||
                            d->GetInfo(d, img, size, &dsize, &ssize) != EFI_SUCCESS) {
                            rc = cmd_err("loadpcirom", "image %d: cannot decompress", images);
                            goto next;
                        }
                        dec = xmalloc(dsize);
                        void *scratch = xmalloc(ssize ? ssize : 1);
                        EFI_STATUS st = d->Decompress(d, img, size, dec, dsize, scratch, ssize);
                        free(scratch);
                        if (EFI_ERROR(st)) {
                            free(dec);
                            rc = cmd_err("loadpcirom", "image %d: decompression failed", images);
                            goto next;
                        }
                        img = dec;
                        size = dsize;
                    }
                    /* device path: the ROM file + RelativeOffsetRange(start, end) of this image */
                    EFI_DEVICE_PATH_PROTOCOL *fdp = efi_file_devpath(path);
                    size_t fl = fdp ? efi_devpath_size(fdp) - 4 : 0;
                    uint8_t *dp = xcalloc(1, fl + 24 + 4);
                    if (fdp)
                        memcpy(dp, fdp, fl);
                    free(fdp);
                    uint8_t *node = dp + fl;
                    node[0] = MEDIA_DEVICE_PATH;
                    node[1] = 8; /* relative offset range */
                    node[2] = 24;
                    uint64_t start = off, endo = off + img_len - 1;
                    memcpy(node + 8, &start, 8);
                    memcpy(node + 16, &endo, 8);
                    node[24] = END_DEVICE_PATH_TYPE;
                    node[25] = END_ENTIRE_DEVICE_PATH_SUBTYPE;
                    node[26] = 4;
                    EFI_HANDLE ih = NULL;
                    EFI_STATUS st = gBS->LoadImage(FALSE, gImage, (EFI_DEVICE_PATH_PROTOCOL *)dp, img, size, &ih);
                    free(dp);
                    if (!EFI_ERROR(st))
                        st = gBS->StartImage(ih, NULL, NULL);
                    out_printf("Image %d (subsystem %u%s) from %s - %s\n", images, subsys, comp ? ", compressed" : "",
                               path_basename(path),
                               efi_blocked_by_secure_boot(st) ? "not allowed by Secure Boot (not signed)"
                               : EFI_ERROR(st) ? efi_strerror(st) : "loaded");
                    if (EFI_ERROR(st))
                        rc = RC_FAIL;
                    else
                        loaded++;
                    free(dec);
                }
            }
        next:
            images++;
            if (last || !img_len)
                break;
            off += img_len;
        }
        if (!images)
            rc = cmd_err("loadpcirom", "%s is not a PCI option ROM image", argv[i]);
        free(rom);
        free(path);
    }
    if (loaded && !noconnect) {
        UINTN n = 0;
        EFI_HANDLE *hs = NULL;
        if (gBS->LocateHandleBuffer(AllHandles, NULL, NULL, &n, &hs) == EFI_SUCCESS) {
            for (UINTN k = 0; k < n; k++)
                gBS->ConnectController(hs[k], NULL, NULL, TRUE);
            gBS->FreePool(hs);
        }
    }
    return rc;
}

/* ---- gop ---- */

typedef struct {
    UINT32 Version, HorizontalResolution, VerticalResolution, PixelFormat, PixelMask[4], PixelsPerScanLine;
} GOP_INFO;

typedef struct {
    UINT32 MaxMode, Mode;
    GOP_INFO *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} GOP_MODE_T;

typedef struct {
    EFI_STATUS(EFIAPI *QueryMode)(void *This, UINT32 Mode, UINTN *Size, GOP_INFO **Info);
    EFI_STATUS(EFIAPI *SetMode)(void *This, UINT32 Mode);
    void *Blt;
    GOP_MODE_T *Mode;
} GOP_T;

static int cmd_gop(int argc, char **argv)
{
    EFI_GUID guid = { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };
    GOP_T *g;
    if (gBS->LocateProtocol(&guid, NULL, (void **)&g) != EFI_SUCCESS)
        return cmd_err("gop", "no graphics output");
    if (argc == 1) {
        static const char *fmts[] = { "RGB", "BGR", "bitmask", "blt only" };
        for (UINT32 m = 0; m < g->Mode->MaxMode; m++) {
            GOP_INFO *info;
            UINTN size;
            if (g->QueryMode(g, m, &size, &info) != EFI_SUCCESS)
                continue;
            out_printf("  %2u: %5u x %-5u %s%s\n", m, info->HorizontalResolution, info->VerticalResolution,
                       info->PixelFormat < 4 ? fmts[info->PixelFormat] : "?", m == g->Mode->Mode ? "  (current)" : "");
            gBS->FreePool(info);
        }
        out_printf("Frame buffer at 0x%llx, %llu bytes\n", (unsigned long long)g->Mode->FrameBufferBase,
                   (unsigned long long)g->Mode->FrameBufferSize);
        return RC_OK;
    }
    int64_t m;
    if (argc != 2 || !parse_int(argv[1], &m) || m < 0 || m >= g->Mode->MaxMode)
        return cmd_usage("gop");
    EFI_STATUS st = g->SetMode(g, (UINT32)m);
    if (EFI_ERROR(st))
        return cmd_err("gop", "%s", efi_strerror(st));
    gST->ConOut->Reset(gST->ConOut, FALSE); /* let the text console adapt */
    return RC_OK;
}

/* ---- cpuid ---- */

static void do_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static int cmd_cpuid(int argc, char **argv)
{
    uint32_t r[4];
    if (argc > 1) {
        uint64_t leaf, sub = 0;
        if (!hexarg(argv[1], &leaf) || (argc > 2 && !hexarg(argv[2], &sub)) || argc > 3)
            return cmd_usage("cpuid");
        do_cpuid((uint32_t)leaf, (uint32_t)sub, r);
        out_printf("EAX=%08X EBX=%08X ECX=%08X EDX=%08X\n", r[0], r[1], r[2], r[3]);
        return RC_OK;
    }
    char vendor[13];
    do_cpuid(0, 0, r);
    uint32_t maxleaf = r[0];
    memcpy(vendor, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    vendor[12] = 0;
    do_cpuid(1, 0, r);
    uint32_t fam = (r[0] >> 8) & 0xF, model = (r[0] >> 4) & 0xF, step = r[0] & 0xF;
    if (fam == 0xF)
        fam += (r[0] >> 20) & 0xFF;
    if (fam >= 6)
        model |= ((r[0] >> 16) & 0xF) << 4;
    out_printf("Vendor: %s, max leaf %X\n", vendor, maxleaf);
    out_printf("Family %X, model %X, stepping %X, logical processors per package %u\n", fam, model, step,
               (r[1] >> 16) & 0xFF);
    static const struct { int reg, bit; const char *name; } feats[] = {
        { 3, 25, "SSE" }, { 3, 26, "SSE2" }, { 2, 0, "SSE3" }, { 2, 19, "SSE4.1" }, { 2, 20, "SSE4.2" },
        { 2, 28, "AVX" }, { 2, 25, "AES" }, { 2, 30, "RDRAND" }, { 2, 5, "VMX" }, { 3, 28, "HTT" }, { 2, 31, "hypervisor" },
    };
    out_puts("Features:");
    for (size_t i = 0; i < ARRAY_SIZE(feats); i++)
        if (r[feats[i].reg] & (1u << feats[i].bit))
            out_printf(" %s", feats[i].name);
    out_puts("\n");
    return RC_OK;
}

static const Cmd hw_cmds[] = {
    { "dmem", cmd_dmem, "dmem [ADDRESS [SIZE]] [-MMIO]",
      "Show memory (hex; no address: the system table and configuration tables)",
      "  ADDRESS  start address (hex); without it, dmem dumps the EFI system\n"
      "           table and lists the configuration tables (GUID and address)\n"
      "  SIZE     bytes to show (hex, default 0x200, max 0x100000)\n"
      "  -MMIO    read through the PCI root bridge of segment 0\n"
      "  -b       accepted for UEFI Shell compatibility and ignored\n"
      "Read-only. Reading an unmapped address can hang the machine.\n"
      "Example: dmem 0xFED00000 40 -MMIO\n" },
    { "mm", cmd_mm, "mm ADDRESS [VALUE] [-w 1|2|4|8] [-MEM|-MMIO|-IO|-PCI|-PCIE] [-n]",
      "Read or write memory, I/O ports or PCI configuration space",
      "  mm ADDRESS            show the value and let you type new ones\n"
      "                        (Enter: next address, q: quit)\n"
      "  mm ADDRESS VALUE      write VALUE\n"
      "  -n                    only show the value\n"
      "  -w 1|2|4|8            access width in bytes (default 1)\n"
      "  -MEM                  memory (default)   -MMIO  memory-mapped I/O\n"
      "  -IO                   I/O port\n"
      "  -PCI                  address 0x000000ssbbddffrr (segment, bus, device,\n"
      "                        function, register)\n"
      "  -PCIE                 address 0x0000ssbbddfffrrr (register up to 0xFFF)\n"
      "Numbers are hexadecimal. Writes are refused while Secure Boot is active.\n" },
    { "pci", cmd_pci, "pci [BUS DEV [FUNC]] [-s SEG] [-i] [-ec]",
      "List PCI devices, or show the configuration space of one device",
      "  pci                 list every device: segment, bus, device, function,\n"
      "                      class, vendor and device ID\n"
      "  pci BUS DEV [FUNC]  hex dump of the configuration space (FUNC 0 by default)\n"
      "  -i                  also decode the header: IDs, class, command/status,\n"
      "                      BARs, bus numbers, interrupt, capabilities\n"
      "  -ec                 dump the 4 KiB PCI Express extended space\n"
      "  -s SEG              PCI segment (default: all segments when listing, 0 else)\n"
      "  -b                  accepted for UEFI Shell compatibility, ignored\n"
      "Numbers are hexadecimal:  pci 0 1f 3 -i   decodes device 00:1F.3.\n"
      "With -data: one record per function (segment, bus, device, function, vendor,\n"
      "deviceid, class, classname); for one device, the decoded header.\n", CMD_DATA },
    { "smbiosview", cmd_smbiosview, "smbiosview [-t TYPE] [-h HANDLE] [-s]",
      "Show the SMBIOS tables (BIOS, system, board, CPU, memory...)",
      "  smbiosview          every structure, with the fields NESH decodes\n"
      "  -t TYPE             only one type (decimal): 0 BIOS, 1 system, 2 board,\n"
      "                      3 enclosure, 4 processor, 16 memory array, 17 memory\n"
      "                      device, 19 mapped address\n"
      "  -h HANDLE           only the structure with this handle (hexadecimal)\n"
      "  -s                  statistics: how many structures of each type\n"
      "  -a, -b              accepted for UEFI Shell compatibility, ignored\n"
      "  smbiosview -t 17    memory modules: slot, size, speed, part number\n"
      "With -data: one record per structure (type, typename, handle, length and the\n"
      "decoded fields, e.g. vendor, version, serial, size).\n",
      CMD_DATA },
    { "acpiview", cmd_acpiview, "acpiview [-l] | acpiview -s SIG [-d]",
      "List the ACPI tables, or show or save one",
      "  acpiview            list the tables: signature, address, length, revision,\n"
      "                      OEM, checksum (-l does the same)\n"
      "  -s SIG              show one table: summary of APIC, MCFG, HPET and FACP,\n"
      "                      then a hex dump (SIG is case-insensitive, e.g. apic)\n"
      "  -s SIG -d           save the table to SIG.bin in the current directory\n"
      "                      (SIG2.bin... when there are several)\n"
      "  -r, -q, -h          accepted for UEFI Shell compatibility, ignored\n"
      "With -data: one record per table (signature, address, length, revision,\n"
      "oemid, oemtable, checksum); -d cannot be combined with -data.\n",
      CMD_DATA },
    { "mode", cmd_mode, "mode [COLUMNS ROWS]", "List the text modes or select one",
      "  (none)        list the text modes of the console (current one marked)\n"
      "  COLUMNS ROWS  switch to the mode with exactly this size\n"
      "Example: mode 100 31\n" },
    { "sermode", cmd_sermode, "sermode [HANDLE [BAUD PARITY DATABITS STOPBITS]]",
      "Show or set serial port settings (parity n|e|o|m|s, stop bits 0|1|1.5|2)",
      "  (none)                            list all serial ports and settings\n"
      "  HANDLE                            show one port (handle number, hex)\n"
      "  HANDLE BAUD PARITY DATABITS STOP  change the settings of a port\n"
      "  PARITY    d (default), n, e, o, m or s (only the first letter counts)\n"
      "  DATABITS  5 to 8; STOP 0 (default), 1, 1.5 (or 15) or 2\n"
      "The receive FIFO depth and timeout are kept. Handles are shown by dh.\n"
      "Example: sermode 5C 115200 n 8 1\n" },
    { "loadpcirom", cmd_loadpcirom, "loadpcirom [-nc] ROMFILE...",
      "Load the UEFI drivers contained in a PCI option ROM file",
      "  ROMFILE  PCI option ROM image file; several files may be given\n"
      "  -nc      load and start the drivers but do not connect them\n"
      "Every x64 UEFI image in the ROM is loaded and started (compressed images\n"
      "are expanded first); legacy and other-architecture images are skipped.\n"
      "Then all controllers are connected, unless -nc is given.\n"
      "With Secure Boot active, the firmware checks the driver signatures.\n"
      "Example: loadpcirom -nc nic.rom\n" },
    { "gop", cmd_gop, "gop [MODE]", "List the graphics modes or select one",
      "  (none)  list the graphics modes (resolution, pixel format), then the\n"
      "          frame buffer address and size\n"
      "  MODE    switch to this mode number, as listed\n"
      "After a mode change the text console is reset to fit the new screen.\n"
      "Example: gop 2\n" },
    { "cpuid", cmd_cpuid, "cpuid [LEAF [SUBLEAF]]", "Show processor information (or raw CPUID registers)",
      "  (none)          vendor, family, model, stepping and main features\n"
      "  LEAF [SUBLEAF]  raw EAX, EBX, ECX, EDX of one leaf (SUBLEAF default 0)\n"
      "LEAF and SUBLEAF are hexadecimal. Family, model and stepping print in hex.\n"
      "Example: cpuid 80000001\n" },
};

void efi_hw_init(void)
{
    shell_register(hw_cmds, ARRAY_SIZE(hw_cmds));
}
