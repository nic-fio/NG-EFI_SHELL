/* Platform layer for UEFI. */
#include "efi_glue.h"

EFI_HANDLE gImage;
EFI_SYSTEM_TABLE *gST;
EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;
EFI_LOADED_IMAGE_PROTOCOL *gLoadedImage;

EFI_GUID gEfiLoadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
EFI_GUID gEfiSimpleFileSystemGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiFileInfoGuid = EFI_FILE_INFO_GUID;
EFI_GUID gEfiFileSystemInfoGuid = EFI_FILE_SYSTEM_INFO_GUID;
EFI_GUID gEfiDevicePathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
EFI_GUID gEfiDevicePathToTextGuid = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
EFI_GUID gEfiGlobalVariableGuid = EFI_GLOBAL_VARIABLE_GUID;
static EFI_GUID gEfiTextInputExGuid = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
static EFI_GUID gEfiShellParametersGuid = EFI_SHELL_PARAMETERS_PROTOCOL_GUID;
static EFI_GUID gEfiBlockIoGuid = EFI_BLOCK_IO_PROTOCOL_GUID;

int pal_argc;
char **pal_argv;

/* ---- Errors ---- */

int efi_to_pal(EFI_STATUS st)
{
    switch (st) {
    case EFI_SUCCESS: return PAL_OK;
    case EFI_NOT_FOUND: return PAL_ENOENT;
    case EFI_ACCESS_DENIED: return PAL_EACCES;
    case EFI_WRITE_PROTECTED: return PAL_EROFS;
    case EFI_VOLUME_FULL: return PAL_ENOSPC;
    case EFI_OUT_OF_RESOURCES: return PAL_ENOMEM;
    case EFI_INVALID_PARAMETER: return PAL_EINVAL;
    case EFI_UNSUPPORTED: return PAL_ENOTSUP;
    case EFI_NO_MEDIA: return PAL_ENOMEDIA;
    case EFI_SECURITY_VIOLATION: return PAL_ESECURITY;
    case EFI_ABORTED: return PAL_EABORT;
    default: return PAL_EIO;
    }
}

const char *efi_strerror(EFI_STATUS st)
{
    static const char *names[] = {
        "success", "load error", "invalid parameter", "unsupported", "bad buffer size",
        "buffer too small", "not ready", "device error", "write protected", "out of resources",
        "volume corrupted", "volume full", "no media", "media changed", "not found",
        "access denied", "no response", "no mapping", "timeout", "not started",
        "already started", "aborted", "ICMP error", "TFTP error", "protocol error",
        "incompatible version", "security violation", "CRC error", "end of media", "?", "?",
        "end of file", "invalid language", "compromised data",
    };
    UINTN code = st & ~EFI_ERROR_BIT;
    if (code < ARRAY_SIZE(names))
        return names[code];
    return "unknown error";
}

/* ---- Memory ---- */

void *pal_alloc(size_t n)
{
    void *p;
    if (gBS->AllocatePool(EfiLoaderData, n, &p) != EFI_SUCCESS)
        return NULL;
    return p;
}

void pal_free(void *p)
{
    gBS->FreePool(p);
}

void rt_fatal(const char *msg)
{
    pal_con_write("\nnesh: fatal: ", 14);
    pal_con_write(msg, strlen(msg));
    pal_con_write("\n", 1);
    pal_exit(1);
}

/* ---- Console ---- */

static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *con_in_ex;
static UINTN initial_attr;

void pal_con_write(const char *s, size_t n)
{
    CHAR16 buf[256];
    size_t k = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += utf8_decode(s + i, n - i, &cp);
        if (cp == '\n')
            buf[k++] = '\r';
        buf[k++] = cp > 0xFFFF ? 0xFFFD : (CHAR16)cp;
        if (k >= ARRAY_SIZE(buf) - 3) {
            buf[k] = 0;
            gST->ConOut->OutputString(gST->ConOut, buf);
            k = 0;
        }
    }
    if (k) {
        buf[k] = 0;
        gST->ConOut->OutputString(gST->ConOut, buf);
    }
}

static bool read_key_now(PalKey *k)
{
    if (con_in_ex) {
        EFI_KEY_DATA kd;
        if (con_in_ex->ReadKeyStrokeEx(con_in_ex, &kd) != EFI_SUCCESS)
            return false;
        k->ch = kd.Key.UnicodeChar;
        k->scan = kd.Key.ScanCode;
        k->mods = 0;
        UINT32 sh = kd.KeyState.KeyShiftState;
        if (sh & EFI_SHIFT_STATE_VALID) {
            if (sh & (EFI_LEFT_CONTROL_PRESSED | EFI_RIGHT_CONTROL_PRESSED))
                k->mods |= MOD_CTRL;
            if (sh & (EFI_LEFT_ALT_PRESSED | EFI_RIGHT_ALT_PRESSED))
                k->mods |= MOD_ALT;
        }
        /* Some keyboards report Ctrl+letter as the plain letter plus the
         * shift state: normalize to the ASCII control code. */
        if ((k->mods & MOD_CTRL) && k->ch && k->ch < 128 && isalpha((int)k->ch))
            k->ch = (k->ch | 32) - 'a' + 1;
        if (!k->ch && !k->scan)
            return false; /* partial key (e.g. only a modifier toggle) */
        return true;
    }
    EFI_INPUT_KEY key;
    if (gST->ConIn->ReadKeyStroke(gST->ConIn, &key) != EFI_SUCCESS)
        return false;
    k->ch = key.UnicodeChar;
    k->scan = key.ScanCode;
    k->mods = 0;
    return true;
}

bool pal_con_read_key(PalKey *k, int timeout_ms)
{
    EFI_EVENT key_event = con_in_ex ? con_in_ex->WaitForKeyEx : gST->ConIn->WaitForKey;
    EFI_EVENT timer = NULL;
    if (timeout_ms >= 0) {
        if (gBS->CreateEvent(EVT_TIMER, 0, NULL, NULL, &timer) != EFI_SUCCESS)
            timer = NULL;
        else
            gBS->SetTimer(timer, TimerRelative, (UINT64)timeout_ms * 10000);
    }
    bool got = false;
    for (;;) {
        if (read_key_now(k)) {
            got = true;
            break;
        }
        if (timeout_ms == 0)
            break;
        EFI_EVENT evs[2] = { key_event, timer };
        UINTN idx;
        if (gBS->WaitForEvent(timer ? 2 : 1, evs, &idx) != EFI_SUCCESS) {
            gBS->Stall(1000);
            continue;
        }
        if (idx == 1) {
            got = read_key_now(k);
            break;
        }
    }
    if (timer)
        gBS->CloseEvent(timer);
    return got;
}

void pal_con_clear(void)
{
    gST->ConOut->ClearScreen(gST->ConOut);
}

void pal_con_set_color(int fg, int bg)
{
    gST->ConOut->SetAttribute(gST->ConOut, (UINTN)((fg & 15) | ((bg & 7) << 4)));
}

void pal_con_get_color(int *fg, int *bg)
{
    INT32 a = gST->ConOut->Mode->Attribute;
    *fg = a & 15;
    *bg = (a >> 4) & 7;
}

void pal_con_reset_color(void)
{
    gST->ConOut->SetAttribute(gST->ConOut, initial_attr);
}

void pal_con_size(int *cols, int *rows)
{
    UINTN c = 80, r = 25;
    if (gST->ConOut->QueryMode(gST->ConOut, gST->ConOut->Mode->Mode, &c, &r) != EFI_SUCCESS)
        c = 80, r = 25;
    *cols = (int)c;
    *rows = (int)r;
}

void pal_con_set_cursor(int col, int row)
{
    gST->ConOut->SetCursorPosition(gST->ConOut, (UINTN)col, (UINTN)row);
}

void pal_con_get_cursor(int *col, int *row)
{
    *col = gST->ConOut->Mode->CursorColumn;
    *row = gST->ConOut->Mode->CursorRow;
}

void pal_con_show_cursor(bool on)
{
    gST->ConOut->EnableCursor(gST->ConOut, on);
}

bool pal_con_interactive(void)
{
    return true;
}

void pal_con_raw(bool on)
{
    (void)on;
}

bool pal_con_ansi(void)
{
    return false;
}

/* ---- Time ---- */

static uint64_t tsc_per_ms;
static uint64_t tsc_start;

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void calibrate_tsc(void)
{
    uint64_t a = rdtsc();
    gBS->Stall(10000);
    uint64_t b = rdtsc();
    tsc_per_ms = (b - a) / 10;
    if (!tsc_per_ms)
        tsc_per_ms = 1;
    tsc_start = b;
}

uint64_t pal_ticks_ms(void)
{
    return (rdtsc() - tsc_start) / tsc_per_ms;
}

void pal_sleep_us(uint64_t us)
{
    gBS->Stall((UINTN)us);
}

void pal_sleep_ms(uint32_t ms)
{
    gBS->Stall((UINTN)ms * 1000);
}

bool pal_get_time(PalTime *t)
{
    EFI_TIME et;
    if (gRT->GetTime(&et, NULL) != EFI_SUCCESS)
        return false;
    t->year = et.Year;
    t->month = et.Month;
    t->day = et.Day;
    t->hour = et.Hour;
    t->min = et.Minute;
    t->sec = et.Second;
    return true;
}

int pal_set_time(const PalTime *t)
{
    EFI_TIME et;
    EFI_STATUS st = gRT->GetTime(&et, NULL);
    if (EFI_ERROR(st))
        return efi_to_pal(st);
    et.Year = (UINT16)t->year;
    et.Month = (UINT8)t->month;
    et.Day = (UINT8)t->day;
    et.Hour = (UINT8)t->hour;
    et.Minute = (UINT8)t->min;
    et.Second = (UINT8)t->sec;
    et.Nanosecond = 0;
    return efi_to_pal(gRT->SetTime(&et));
}

/* ---- Volumes ---- */

typedef struct {
    PalVolume v;
    EFI_HANDLE handle;
} EfiVolume;

static EfiVolume *volumes;
static int nvolumes;
static int boot_volume = -1;

size_t efi_devpath_size(const EFI_DEVICE_PATH_PROTOCOL *dp)
{
    const uint8_t *p = (const uint8_t *)dp;
    for (;;) {
        const EFI_DEVICE_PATH_PROTOCOL *n = (const void *)p;
        size_t len = n->Length[0] | (n->Length[1] << 8);
        if (len < 4)
            return 0;
        p += len;
        if (n->Type == END_DEVICE_PATH_TYPE && n->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE)
            break;
    }
    return p - (const uint8_t *)dp;
}

char *efi_devpath_text(const EFI_DEVICE_PATH_PROTOCOL *dp)
{
    EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *tt;
    if (!dp || gBS->LocateProtocol(&gEfiDevicePathToTextGuid, NULL, (void **)&tt) != EFI_SUCCESS)
        return xstrdup("");
    CHAR16 *t = tt->ConvertDevicePathToText(dp, FALSE, TRUE);
    if (!t)
        return xstrdup("");
    char *r = ucs2_to_utf8(t, (size_t)-1);
    gBS->FreePool(t);
    return r;
}

static int vol_cmp(const void *a, const void *b)
{
    const EfiVolume *x = a, *y = b;
    return strcmp(x->v.devpath, y->v.devpath);
}

static void fill_volume_info(EfiVolume *ev)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root;
    ev->v.label = xstrdup("");
    if (gBS->HandleProtocol(ev->handle, &gEfiSimpleFileSystemGuid, (void **)&fs) != EFI_SUCCESS)
        return;
    if (fs->OpenVolume(fs, &root) != EFI_SUCCESS)
        return;
    UINTN size = 512;
    EFI_FILE_SYSTEM_INFO *info = xmalloc(size);
    EFI_STATUS st = root->GetInfo(root, &gEfiFileSystemInfoGuid, &size, info);
    if (st == EFI_BUFFER_TOO_SMALL) {
        info = xrealloc(info, size);
        st = root->GetInfo(root, &gEfiFileSystemInfoGuid, &size, info);
    }
    if (st == EFI_SUCCESS) {
        free(ev->v.label);
        ev->v.label = ucs2_to_utf8(info->VolumeLabel, (size_t)-1);
        ev->v.size = info->VolumeSize;
        ev->v.free = info->FreeSpace;
        ev->v.readonly = info->ReadOnly;
    }
    free(info);
    root->Close(root);
    EFI_BLOCK_IO_PROTOCOL *bio;
    if (gBS->HandleProtocol(ev->handle, &gEfiBlockIoGuid, (void **)&bio) == EFI_SUCCESS)
        ev->v.removable = bio->Media->RemovableMedia;
}

void pal_volumes_refresh(void)
{
    for (int i = 0; i < nvolumes; i++) {
        free(volumes[i].v.label);
        free(volumes[i].v.devpath);
    }
    free(volumes);
    volumes = NULL;
    nvolumes = 0;
    boot_volume = -1;

    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    if (gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemGuid, NULL, &n, &hs) != EFI_SUCCESS)
        return;
    volumes = xcalloc(n, sizeof(EfiVolume));
    for (UINTN i = 0; i < n; i++) {
        EfiVolume *ev = &volumes[nvolumes++];
        ev->handle = hs[i];
        EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
        gBS->HandleProtocol(hs[i], &gEfiDevicePathGuid, (void **)&dp);
        ev->v.devpath = efi_devpath_text(dp);
    }
    gBS->FreePool(hs);
    qsort(volumes, nvolumes, sizeof(EfiVolume), vol_cmp);
    for (int i = 0; i < nvolumes; i++) {
        snprintf(volumes[i].v.name, sizeof(volumes[i].v.name), "fs%d", i);
        fill_volume_info(&volumes[i]);
        if (gLoadedImage && volumes[i].handle == gLoadedImage->DeviceHandle)
            boot_volume = i;
    }
}

int pal_volume_count(void)
{
    return nvolumes;
}

const PalVolume *pal_volume(int idx)
{
    return idx >= 0 && idx < nvolumes ? &volumes[idx].v : NULL;
}

int pal_boot_volume(void)
{
    return boot_volume;
}

EFI_HANDLE efi_volume_handle(int idx)
{
    return idx >= 0 && idx < nvolumes ? volumes[idx].handle : NULL;
}

/* Split "fsN:\a\b" into the volume index and the path inside the volume. */
static int split_path(const char *path, const char **rest)
{
    const char *c = strchr(path, ':');
    if (!c)
        return -1;
    for (int i = 0; i < nvolumes; i++) {
        size_t l = strlen(volumes[i].v.name);
        if ((size_t)(c - path) == l && !strncasecmp(path, volumes[i].v.name, l)) {
            *rest = c + 1;
            return i;
        }
    }
    return -1;
}

EFI_DEVICE_PATH_PROTOCOL *efi_file_devpath(const char *path)
{
    const char *rest;
    int vi = split_path(path, &rest);
    if (vi < 0)
        return NULL;
    EFI_DEVICE_PATH_PROTOCOL *vdp = NULL;
    if (gBS->HandleProtocol(volumes[vi].handle, &gEfiDevicePathGuid, (void **)&vdp) != EFI_SUCCESS)
        return NULL;
    size_t vsz = efi_devpath_size(vdp) - 4; /* without the end node */
    size_t units;
    uint16_t *name = utf8_to_ucs2(*rest ? rest : "\\", &units);
    size_t fsz = 4 + (units + 1) * 2;
    uint8_t *r = xmalloc(vsz + fsz + 4);
    memcpy(r, vdp, vsz);
    FILEPATH_DEVICE_PATH *f = (FILEPATH_DEVICE_PATH *)(r + vsz);
    f->Header.Type = MEDIA_DEVICE_PATH;
    f->Header.SubType = MEDIA_FILEPATH_DP;
    f->Header.Length[0] = (UINT8)fsz;
    f->Header.Length[1] = (UINT8)(fsz >> 8);
    memcpy(f->PathName, name, (units + 1) * 2);
    free(name);
    EFI_DEVICE_PATH_PROTOCOL *end = (void *)(r + vsz + fsz);
    end->Type = END_DEVICE_PATH_TYPE;
    end->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    end->Length[0] = 4;
    end->Length[1] = 0;
    return (EFI_DEVICE_PATH_PROTOCOL *)r;
}

/* ---- Files ---- */

struct PalFile {
    EFI_FILE_PROTOCOL *h;
};

struct PalDir {
    EFI_FILE_PROTOCOL *h;
    EFI_FILE_INFO *buf;
    UINTN bufsize;
};

static EFI_STATUS open_root(int vi, EFI_FILE_PROTOCOL **root)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS st = gBS->HandleProtocol(volumes[vi].handle, &gEfiSimpleFileSystemGuid, (void **)&fs);
    if (EFI_ERROR(st))
        return st;
    return fs->OpenVolume(fs, root);
}

EFI_STATUS efi_open_path(const char *path, UINT64 mode, UINT64 attr, EFI_FILE_PROTOCOL **out)
{
    const char *rest;
    int vi = split_path(path, &rest);
    if (vi < 0)
        return EFI_NOT_FOUND;
    EFI_FILE_PROTOCOL *root;
    EFI_STATUS st = open_root(vi, &root);
    if (EFI_ERROR(st))
        return st;
    while (*rest == '\\')
        rest++;
    if (!*rest) {
        *out = root;
        return EFI_SUCCESS;
    }
    uint16_t *name = utf8_to_ucs2(rest, NULL);
    st = root->Open(root, out, name, mode, attr);
    free(name);
    root->Close(root);
    return st;
}

static EFI_FILE_INFO *get_info(EFI_FILE_PROTOCOL *h)
{
    UINTN size = sizeof(EFI_FILE_INFO) + 256;
    EFI_FILE_INFO *info = xmalloc(size);
    EFI_STATUS st = h->GetInfo(h, &gEfiFileInfoGuid, &size, info);
    if (st == EFI_BUFFER_TOO_SMALL) {
        info = xrealloc(info, size);
        st = h->GetInfo(h, &gEfiFileInfoGuid, &size, info);
    }
    if (EFI_ERROR(st)) {
        free(info);
        return NULL;
    }
    return info;
}

static void fill_stat(PalStat *st, EFI_FILE_INFO *info, bool with_name)
{
    st->name = with_name ? ucs2_to_utf8(info->FileName, (size_t)-1) : NULL;
    st->size = info->FileSize;
    st->attr = info->Attribute;
    st->is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
    st->mtime.year = info->ModificationTime.Year;
    st->mtime.month = info->ModificationTime.Month;
    st->mtime.day = info->ModificationTime.Day;
    st->mtime.hour = info->ModificationTime.Hour;
    st->mtime.min = info->ModificationTime.Minute;
    st->mtime.sec = info->ModificationTime.Second;
}

int pal_open(const char *path, int flags, PalFile **f)
{
    UINT64 mode = EFI_FILE_MODE_READ;
    if (flags & (PAL_O_WRITE | PAL_O_APPEND))
        mode |= EFI_FILE_MODE_WRITE;
    if (flags & PAL_O_CREATE)
        mode |= EFI_FILE_MODE_CREATE;
    EFI_FILE_PROTOCOL *h;
    EFI_STATUS st = efi_open_path(path, mode, 0, &h);
    if (EFI_ERROR(st))
        return efi_to_pal(st);
    EFI_FILE_INFO *info = get_info(h);
    if (info && (info->Attribute & EFI_FILE_DIRECTORY) && (flags & (PAL_O_WRITE | PAL_O_APPEND))) {
        free(info);
        h->Close(h);
        return PAL_EISDIR;
    }
    if (info && (flags & PAL_O_TRUNC) && info->FileSize) {
        info->FileSize = 0;
        st = h->SetInfo(h, &gEfiFileInfoGuid, info->Size, info);
        if (EFI_ERROR(st)) {
            free(info);
            h->Close(h);
            return efi_to_pal(st);
        }
    }
    if (flags & PAL_O_APPEND)
        h->SetPosition(h, 0xFFFFFFFFFFFFFFFFULL);
    free(info);
    *f = xmalloc(sizeof(PalFile));
    (*f)->h = h;
    return PAL_OK;
}

int pal_read(PalFile *f, void *buf, size_t n, size_t *got)
{
    UINTN sz = n;
    EFI_STATUS st = f->h->Read(f->h, &sz, buf);
    *got = EFI_ERROR(st) ? 0 : sz;
    return efi_to_pal(st);
}

int pal_write(PalFile *f, const void *buf, size_t n)
{
    while (n) {
        UINTN sz = n;
        EFI_STATUS st = f->h->Write(f->h, &sz, (void *)buf);
        if (EFI_ERROR(st))
            return efi_to_pal(st);
        if (!sz)
            return PAL_EIO;
        buf = (const uint8_t *)buf + sz;
        n -= sz;
    }
    return PAL_OK;
}

int pal_seek(PalFile *f, uint64_t pos)
{
    return efi_to_pal(f->h->SetPosition(f->h, pos));
}

int pal_close(PalFile *f)
{
    EFI_STATUS st = f->h->Close(f->h);
    free(f);
    return efi_to_pal(st);
}

int pal_stat(const char *path, PalStat *st)
{
    EFI_FILE_PROTOCOL *h;
    EFI_STATUS s = efi_open_path(path, EFI_FILE_MODE_READ, 0, &h);
    if (EFI_ERROR(s))
        return efi_to_pal(s);
    EFI_FILE_INFO *info = get_info(h);
    h->Close(h);
    if (!info)
        return PAL_EIO;
    fill_stat(st, info, false);
    free(info);
    return PAL_OK;
}

int pal_opendir(const char *path, PalDir **d)
{
    EFI_FILE_PROTOCOL *h;
    EFI_STATUS s = efi_open_path(path, EFI_FILE_MODE_READ, 0, &h);
    if (EFI_ERROR(s))
        return efi_to_pal(s);
    EFI_FILE_INFO *info = get_info(h);
    bool isdir = info && (info->Attribute & EFI_FILE_DIRECTORY);
    free(info);
    if (!isdir) {
        h->Close(h);
        return PAL_ENOTDIR;
    }
    *d = xmalloc(sizeof(PalDir));
    (*d)->h = h;
    (*d)->bufsize = sizeof(EFI_FILE_INFO) + 512;
    (*d)->buf = xmalloc((*d)->bufsize);
    return PAL_OK;
}

int pal_readdir(PalDir *d, PalStat *st)
{
    for (;;) {
        UINTN sz = d->bufsize;
        EFI_STATUS s = d->h->Read(d->h, &sz, d->buf);
        if (s == EFI_BUFFER_TOO_SMALL) {
            d->bufsize = sz;
            d->buf = xrealloc(d->buf, sz);
            continue;
        }
        if (EFI_ERROR(s))
            return efi_to_pal(s);
        if (sz == 0)
            return 0;
        CHAR16 *n = d->buf->FileName;
        if ((n[0] == '.' && n[1] == 0) || (n[0] == '.' && n[1] == '.' && n[2] == 0))
            continue;
        fill_stat(st, d->buf, true);
        return 1;
    }
}

void pal_closedir(PalDir *d)
{
    d->h->Close(d->h);
    free(d->buf);
    free(d);
}

int pal_mkdir(const char *path)
{
    EFI_FILE_PROTOCOL *h;
    if (!EFI_ERROR(efi_open_path(path, EFI_FILE_MODE_READ, 0, &h))) {
        h->Close(h);
        return PAL_EEXIST;
    }
    EFI_STATUS st = efi_open_path(path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                              EFI_FILE_DIRECTORY, &h);
    if (EFI_ERROR(st))
        return efi_to_pal(st);
    h->Close(h);
    return PAL_OK;
}

int pal_remove(const char *path)
{
    EFI_FILE_PROTOCOL *h;
    EFI_STATUS st = efi_open_path(path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &h);
    if (EFI_ERROR(st))
        return efi_to_pal(st);
    EFI_FILE_INFO *info = get_info(h);
    if (info && (info->Attribute & EFI_FILE_DIRECTORY)) {
        /* Refuse non-empty directories explicitly: not all drivers check. */
        UINTN bsz = sizeof(EFI_FILE_INFO) + 512;
        EFI_FILE_INFO *e = xmalloc(bsz);
        for (;;) {
            UINTN sz = bsz;
            EFI_STATUS s = h->Read(h, &sz, e);
            if (s == EFI_BUFFER_TOO_SMALL) {
                bsz = sz;
                e = xrealloc(e, bsz);
                continue;
            }
            if (EFI_ERROR(s) || sz == 0)
                break;
            CHAR16 *n = e->FileName;
            if ((n[0] == '.' && n[1] == 0) || (n[0] == '.' && n[1] == '.' && n[2] == 0))
                continue;
            free(e);
            free(info);
            h->Close(h);
            return PAL_ENOTEMPTY;
        }
        free(e);
    }
    free(info);
    st = h->Delete(h); /* closes the handle */
    return st == EFI_WARN_DELETE_FAILURE ? PAL_EACCES : efi_to_pal(st);
}

int pal_rename(const char *from, const char *to)
{
    const char *rf, *rt;
    int vf = split_path(from, &rf), vt = split_path(to, &rt);
    if (vf < 0 || vt < 0)
        return PAL_ENOENT;
    if (vf != vt)
        return PAL_ENOTSUP;
    EFI_FILE_PROTOCOL *h;
    EFI_STATUS st = efi_open_path(from, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &h);
    if (EFI_ERROR(st))
        return efi_to_pal(st);
    EFI_FILE_INFO *info = get_info(h);
    if (!info) {
        h->Close(h);
        return PAL_EIO;
    }
    size_t units;
    uint16_t *name = utf8_to_ucs2(rt, &units); /* full path from the root, starts with '\' */
    size_t nsz = sizeof(EFI_FILE_INFO) + (units + 1) * 2;
    EFI_FILE_INFO *ni = xmalloc(nsz);
    memcpy(ni, info, sizeof(EFI_FILE_INFO));
    ni->Size = nsz;
    memcpy(ni->FileName, name, (units + 1) * 2);
    st = h->SetInfo(h, &gEfiFileInfoGuid, nsz, ni);
    free(name);
    free(ni);
    free(info);
    h->Close(h);
    return efi_to_pal(st);
}

/* Opens a file for SetInfo and returns its current info. */
static EFI_FILE_INFO *open_for_info(const char *path, EFI_FILE_PROTOCOL **h, EFI_STATUS *st)
{
    *st = efi_open_path(path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, h);
    if (EFI_ERROR(*st)) {
        /* directories and read-only files can often only be opened for reading */
        *st = efi_open_path(path, EFI_FILE_MODE_READ, 0, h);
        if (EFI_ERROR(*st))
            return NULL;
    }
    EFI_FILE_INFO *info = get_info(*h);
    if (!info) {
        (*h)->Close(*h);
        *st = EFI_DEVICE_ERROR;
    }
    return info;
}

int pal_set_attr(const char *path, uint64_t attr)
{
    EFI_FILE_PROTOCOL *h;
    EFI_STATUS st;
    EFI_FILE_INFO *info = open_for_info(path, &h, &st); /* read-only files: opened for reading */
    if (!info)
        return efi_to_pal(st);
    info->Attribute = (info->Attribute & EFI_FILE_DIRECTORY) | (attr & EFI_FILE_VALID_ATTR & ~EFI_FILE_DIRECTORY);
    st = h->SetInfo(h, &gEfiFileInfoGuid, info->Size, info);
    free(info);
    h->Close(h);
    return efi_to_pal(st);
}

int pal_set_size(const char *path, uint64_t size)
{
    EFI_FILE_PROTOCOL *h;
    EFI_STATUS st;
    EFI_FILE_INFO *info = open_for_info(path, &h, &st);
    if (!info)
        return efi_to_pal(st);
    if (info->Attribute & EFI_FILE_DIRECTORY) {
        free(info);
        h->Close(h);
        return PAL_EISDIR;
    }
    info->FileSize = size;
    st = h->SetInfo(h, &gEfiFileInfoGuid, info->Size, info);
    free(info);
    h->Close(h);
    return efi_to_pal(st);
}

int pal_set_mtime(const char *path, const PalTime *t)
{
    EFI_FILE_PROTOCOL *h;
    EFI_STATUS st;
    EFI_FILE_INFO *info = open_for_info(path, &h, &st);
    if (!info)
        return efi_to_pal(st);
    EFI_TIME *m = &info->ModificationTime;
    m->Year = (UINT16)t->year;
    m->Month = (UINT8)t->month;
    m->Day = (UINT8)t->day;
    m->Hour = (UINT8)t->hour;
    m->Minute = (UINT8)t->min;
    m->Second = (UINT8)t->sec;
    m->Nanosecond = 0;
    info->LastAccessTime = *m;
    st = h->SetInfo(h, &gEfiFileInfoGuid, info->Size, info);
    free(info);
    h->Close(h);
    return efi_to_pal(st);
}

int pal_volume_set_label(int idx, const char *label)
{
    if (idx < 0 || idx >= nvolumes)
        return PAL_ENOENT;
    EFI_FILE_PROTOCOL *root;
    EFI_STATUS st = open_root(idx, &root);
    if (EFI_ERROR(st))
        return efi_to_pal(st);
    UINTN size = 512;
    EFI_FILE_SYSTEM_INFO *info = xmalloc(size);
    st = root->GetInfo(root, &gEfiFileSystemInfoGuid, &size, info);
    if (!EFI_ERROR(st)) {
        size_t units;
        uint16_t *w = utf8_to_ucs2(label, &units);
        size_t nsz = sizeof(EFI_FILE_SYSTEM_INFO) + (units + 1) * 2;
        EFI_FILE_SYSTEM_INFO *ni = xmalloc(nsz);
        memcpy(ni, info, sizeof(EFI_FILE_SYSTEM_INFO));
        ni->Size = nsz;
        memcpy(ni->VolumeLabel, w, (units + 1) * 2);
        st = root->SetInfo(root, &gEfiFileSystemInfoGuid, nsz, ni);
        free(ni);
        free(w);
    }
    free(info);
    root->Close(root);
    if (!EFI_ERROR(st)) {
        free(volumes[idx].v.label);
        volumes[idx].v.label = xstrdup(label);
    }
    return efi_to_pal(st);
}

/* ---- System ---- */

void pal_reset(int type)
{
    EFI_RESET_TYPE t = type == PAL_RESET_WARM ? EfiResetWarm
                     : type == PAL_RESET_SHUTDOWN ? EfiResetShutdown : EfiResetCold;
    gRT->ResetSystem(t, EFI_SUCCESS, 0, NULL);
}

void (*efi_exit_hook)(void);

/* The text mode found at start, put back on leaving if NESH changed it. */
static INT32 start_mode = -1;

static void restore_text_mode(void)
{
    if (start_mode >= 0)
        gST->ConOut->SetMode(gST->ConOut, (UINTN)start_mode);
}

void pal_exit(int code)
{
    if (efi_exit_hook)
        efi_exit_hook();
    pal_con_reset_color();
    restore_text_mode();
    gBS->Exit(gImage, code ? EFIERR(code) : EFI_SUCCESS, 0, NULL);
    for (;;)
        __asm__ volatile("hlt");
}

const char *pal_platform_name(void)
{
    return "UEFI x86_64";
}

/* ---- Entry point ---- */

/* Split a UCS-2 command line into UTF-8 arguments (double quotes group). */
static void split_cmdline(const CHAR16 *s, size_t units)
{
    char *line = ucs2_to_utf8(s, units);
    int cap = 8;
    pal_argv = xmalloc(sizeof(char *) * cap);
    pal_argc = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        Sbuf a;
        sb_init(&a);
        bool q = false;
        for (; *p && (q || (*p != ' ' && *p != '\t')); p++) {
            if (*p == '"')
                q = !q;
            else
                sb_putc(&a, *p);
        }
        if (pal_argc + 1 >= cap)
            pal_argv = xrealloc(pal_argv, sizeof(char *) * (cap *= 2));
        pal_argv[pal_argc++] = sb_steal(&a);
    }
    pal_argv[pal_argc] = NULL;
    free(line);
}

static void setup_args(void)
{
    EFI_SHELL_PARAMETERS_PROTOCOL *sp;
    if (gBS->OpenProtocol(gImage, &gEfiShellParametersGuid, (void **)&sp, gImage, NULL,
                          EFI_OPEN_PROTOCOL_GET_PROTOCOL) == EFI_SUCCESS && sp->Argc > 0) {
        pal_argc = (int)sp->Argc;
        pal_argv = xmalloc(sizeof(char *) * (sp->Argc + 1));
        for (UINTN i = 0; i < sp->Argc; i++)
            pal_argv[i] = ucs2_to_utf8(sp->Argv[i], (size_t)-1);
        pal_argv[pal_argc] = NULL;
        return;
    }
    /* LoadOptions is a UCS-2 command line when started by a shell or a boot
     * entry with text arguments; binary data (e.g. from BDS) is ignored. */
    const CHAR16 *lo = gLoadedImage ? gLoadedImage->LoadOptions : NULL;
    size_t units = gLoadedImage ? gLoadedImage->LoadOptionsSize / 2 : 0;
    bool text = lo && units && !(gLoadedImage->LoadOptionsSize & 1);
    for (size_t i = 0; text && i < units && lo[i]; i++)
        if (lo[i] < 0x20 && lo[i] != '\t')
            text = false;
    if (text) {
        split_cmdline(lo, units);
        if (pal_argc > 0)
            return;
    }
    pal_argc = 1;
    pal_argv = xmalloc(sizeof(char *) * 2);
    pal_argv[0] = xstrdup("nesh");
    pal_argv[1] = NULL;
}

#define STACK_PAGES 256 /* 1 MiB */

static int main_rc;

static void main_on_stack(void)
{
    main_rc = nesh_main(pal_argc, pal_argv);
}

/* Calls fn with rsp = top (16-byte aligned), then restores the original stack. */
static void call_on_stack(void (*fn)(void), void *top)
{
    __asm__ volatile("mov %%rsp, %%rbx\n\t"
                     "mov %0, %%rsp\n\t"
                     "call *%1\n\t"
                     "mov %%rbx, %%rsp\n\t"
                     :
                     : "r"(top), "r"(fn)
                     : "rbx", "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
}

/* The narrowest text mode with at least MIN_COLS columns and MIN_ROWS rows
 * (the narrowest has the largest characters); -1 if the firmware has none.
 * The current mode counts too: if it is wide enough already, nothing changes. */
static INT32 wide_text_mode(UINTN min_cols, UINTN min_rows)
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *o = gST->ConOut;
    INT32 best = -1;
    UINTN best_cols = 0;
    for (INT32 m = 0; m < o->Mode->MaxMode; m++) {
        UINTN c, r;
        if (o->QueryMode(o, (UINTN)m, &c, &r) != EFI_SUCCESS || c < min_cols || r < min_rows)
            continue;
        if (best < 0 || c < best_cols)
            best = m, best_cols = c;
    }
    UINTN c, r;
    if (o->QueryMode(o, (UINTN)o->Mode->Mode, &c, &r) == EFI_SUCCESS && c >= min_cols && r >= min_rows)
        return -1;
    return best;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    gImage = image;
    gST = st;
    gBS = st->BootServices;
    gRT = st->RuntimeServices;
    gBS->SetWatchdogTimer(0, 0, 0, NULL);
    gBS->HandleProtocol(image, &gEfiLoadedImageGuid, (void **)&gLoadedImage);
    if (gBS->HandleProtocol(st->ConsoleInHandle, &gEfiTextInputExGuid, (void **)&con_in_ex) != EFI_SUCCESS)
        con_in_ex = NULL;
    initial_attr = (UINTN)st->ConOut->Mode->Attribute;
    if (!initial_attr)
        initial_attr = 0x07;
    /* 100 columns read better than 80: switch to such a mode when the
     * firmware has one, and put back the mode found when NESH ends. */
    INT32 wide = wide_text_mode(100, 25), found = st->ConOut->Mode->Mode;
    if (wide >= 0 && st->ConOut->SetMode(st->ConOut, (UINTN)wide) == EFI_SUCCESS)
        start_mode = found;
    st->ConOut->EnableCursor(st->ConOut, TRUE);
    calibrate_tsc();
    pal_volumes_refresh();
    setup_args();
    /* The interpreter is recursive: run on a private 1 MiB stack instead of
     * the firmware stack (UEFI only guarantees 128 KiB). */
    EFI_PHYSICAL_ADDRESS stack = 0;
    int rc;
    if (gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, STACK_PAGES, &stack) == EFI_SUCCESS) {
        call_on_stack(main_on_stack, (void *)(uintptr_t)(stack + STACK_PAGES * 4096));
        rc = main_rc;
        gBS->FreePages(stack, STACK_PAGES);
    } else {
        rc = nesh_main(pal_argc, pal_argv);
    }
    if (efi_exit_hook)
        efi_exit_hook(); /* nothing may point into this image after it exits */
    pal_con_reset_color();
    restore_text_mode();
    return rc ? EFIERR(rc & 0xFF) : EFI_SUCCESS;
}
