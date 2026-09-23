/* partmgr: the disks of the machine, found through the firmware's block
 * devices (EFI_BLOCK_IO_PROTOCOL).
 *
 * Block devices are numbered as NESH's map numbers them - every handle with
 * block I/O, sorted by device path - so blk2 in partmgr is blk2 in NESH.
 * Only whole disks with a medium are listed. The disk partmgr.efi was loaded
 * from is recognised by its device path, which the path of the loading
 * partition starts with, and is never given a write function. */
#include "disks.h"
#include "../pal/efi_glue.h"

static EFI_GUID blockio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID rng_guid = EFI_RNG_PROTOCOL_GUID;

typedef EFI_STATUS(EFIAPI *WRITE_BLOCKS)(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, UINT64 Lba, UINTN Size,
                                         void *Buf);
typedef EFI_STATUS(EFIAPI *FLUSH_BLOCKS)(EFI_BLOCK_IO_PROTOCOL *This);

typedef struct {
    EFI_HANDLE h;
    char *dp;
} Handle;

static int handle_cmp(const void *a, const void *b)
{
    return strcmp(((const Handle *)a)->dp, ((const Handle *)b)->dp);
}

/* ---- block I/O through a page-aligned bounce buffer (IoAlign) ---- */

static int bio(EFI_BLOCK_IO_PROTOCOL *b, uint64_t lba, uint32_t count, void *buf, bool write)
{
    size_t size = (size_t)count * b->Media->BlockSize;
    UINTN pages = (size + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS p;
    if (gBS->AllocatePages(AllocateAnyPages, EfiBootServicesData, pages, &p) != EFI_SUCCESS)
        return PAL_ENOMEM;
    EFI_STATUS st;
    if (write) {
        memcpy((void *)(uintptr_t)p, buf, size);
        st = ((WRITE_BLOCKS)b->WriteBlocks)(b, b->Media->MediaId, lba, size, (void *)(uintptr_t)p);
        if (!EFI_ERROR(st))
            st = ((FLUSH_BLOCKS)b->FlushBlocks)(b);
    } else {
        st = b->ReadBlocks(b, b->Media->MediaId, lba, size, (void *)(uintptr_t)p);
        if (!EFI_ERROR(st))
            memcpy(buf, (void *)(uintptr_t)p, size);
    }
    gBS->FreePages(p, pages);
    return efi_to_pal(st);
}

static int disk_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    return bio(ctx, lba, count, buf, false);
}

static int disk_write(void *ctx, uint64_t lba, uint32_t count, const void *buf)
{
    return bio(ctx, lba, count, (void *)buf, true);
}

/* ---- random numbers: the firmware's generator, or a mix of the clocks ---- */

static uint64_t mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static void disk_random(void *ctx, void *buf, size_t n)
{
    (void)ctx;
    EFI_RNG_PROTOCOL *rng;
    if (gBS->LocateProtocol(&rng_guid, NULL, (void **)&rng) == EFI_SUCCESS &&
        rng->GetRNG(rng, NULL, n, buf) == EFI_SUCCESS)
        return;
    static uint64_t state;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    PalTime t = { 0 };
    pal_get_time(&t);
    state ^= ((uint64_t)hi << 32 | lo) ^ (uint64_t)(t.sec + 60 * (t.min + 60 * (t.hour + 24 * t.day)));
    uint8_t *p = buf;
    for (size_t i = 0; i < n; i++) {
        if (i % 8 == 0)
            state = mix(state);
        p[i] = (uint8_t)(state >> (8 * (i % 8)));
    }
}

/* ---- the disks ---- */

static const char *kind_of(const char *dp)
{
    static const struct {
        const char *node, *kind;
    } kinds[] = {
        { "NVMe(", "NVMe" }, { "Sata(", "SATA" }, { "USB(", "USB" }, { "UsbClass(", "USB" },
        { "UsbWwid(", "USB" }, { "Scsi(", "SCSI" }, { "Ata(", "ATA" }, { "SD(", "SD" },
        { "eMMC(", "eMMC" }, { "UFS(", "UFS" }, { "Sas(", "SAS" }, { "iSCSI(", "iSCSI" },
    };
    for (size_t i = 0; i < ARRAY_SIZE(kinds); i++)
        if (strstr(dp, kinds[i].node))
            return kinds[i].kind;
    return "disk";
}

static PmDisk *disks;
static int ndisks;

int pm_disks(PmDisk **out)
{
    for (int i = 0; i < ndisks; i++)
        free(disks[i].devpath);
    free(disks);
    disks = NULL;
    ndisks = 0;
    *out = NULL;

    /* the partition partmgr was loaded from */
    char *boot_dp = NULL;
    if (gLoadedImage && gLoadedImage->DeviceHandle) {
        EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
        if (gBS->HandleProtocol(gLoadedImage->DeviceHandle, &gEfiDevicePathGuid, (void **)&dp) == EFI_SUCCESS)
            boot_dp = efi_devpath_text(dp);
    }

    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    if (gBS->LocateHandleBuffer(ByProtocol, &blockio_guid, NULL, &n, &hs) != EFI_SUCCESS) {
        free(boot_dp);
        return 0;
    }
    Handle *all = xcalloc(n ? n : 1, sizeof(Handle));
    for (UINTN i = 0; i < n; i++) {
        EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
        gBS->HandleProtocol(hs[i], &gEfiDevicePathGuid, (void **)&dp);
        all[i].h = hs[i];
        all[i].dp = efi_devpath_text(dp);
    }
    gBS->FreePool(hs);
    qsort(all, n, sizeof(Handle), handle_cmp);

    disks = xcalloc(n ? n : 1, sizeof(PmDisk));
    for (UINTN i = 0; i < n; i++) {
        EFI_BLOCK_IO_PROTOCOL *b;
        if (gBS->HandleProtocol(all[i].h, &blockio_guid, (void **)&b) != EFI_SUCCESS)
            continue;
        EFI_BLOCK_IO_MEDIA *m = b->Media;
        if (m->LogicalPartition || !m->MediaPresent || m->BlockSize < 512)
            continue;
        PmDisk *d = &disks[ndisks++];
        snprintf(d->name, sizeof(d->name), "blk%u", (unsigned)i);
        snprintf(d->kind, sizeof(d->kind), "%s", kind_of(all[i].dp));
        d->devpath = xstrdup(all[i].dp);
        d->size = (m->LastBlock + 1) * m->BlockSize;
        d->removable = m->RemovableMedia;
        d->readonly = m->ReadOnly;
        size_t l = strlen(all[i].dp);
        d->boot = boot_dp && !strncmp(boot_dp, all[i].dp, l) && (boot_dp[l] == '/' || !boot_dp[l]);
        d->dev = (PtDev){ disk_read, d->readonly || d->boot ? NULL : disk_write, disk_random, b, m->BlockSize,
                          m->LastBlock + 1 };
    }
    for (UINTN i = 0; i < n; i++)
        free(all[i].dp);
    free(all);
    free(boot_dp);
    *out = disks;
    return ndisks;
}
