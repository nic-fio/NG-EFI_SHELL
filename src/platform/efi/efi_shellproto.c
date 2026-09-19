/* EFI_SHELL_PROTOCOL 2.2, implemented by NESH so that applications written
 * for the UEFI Shell (ShellLib, ShellCEntryLib...) run under NESH.
 *
 * Memory handed to the caller (help text, device paths, file info, names)
 * comes straight from AllocatePool so that the caller can FreePool it.
 * Strings returned as CONST stay owned by the shell (kept in caches). */
#include "efi_cmds.h"
#include "../../../include/efi_shell.h"

static EFI_GUID shell_guid = EFI_SHELL_PROTOCOL_GUID;
static EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID cn2_guid = EFI_COMPONENT_NAME2_PROTOCOL_GUID;
static EFI_GUID cn_guid = EFI_COMPONENT_NAME_PROTOCOL_GUID;
static EFI_GUID textinex_guid = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;

static EFI_SHELL_PROTOCOL shell;
static EFI_HANDLE installed_on;
static EFI_SHELL_PROTOCOL *previous; /* protocol of a parent shell we replaced */
static bool page_break;

/* ---- Memory helpers ---- */

static void *pool(size_t n)
{
    void *p = NULL;
    if (gBS->AllocatePool(EfiBootServicesData, n ? n : 1, &p) != EFI_SUCCESS)
        return NULL;
    memset(p, 0, n ? n : 1);
    return p;
}

static CHAR16 *pool_ucs2(const char *s)
{
    size_t units;
    uint16_t *u = utf8_to_ucs2(s, &units);
    CHAR16 *r = pool((units + 1) * 2);
    if (r)
        memcpy(r, u, (units + 1) * 2);
    free(u);
    return r;
}

static char *u8(const CHAR16 *s)
{
    return s ? ucs2_to_utf8(s, (size_t)-1) : NULL;
}

/* Cache of CONST strings returned to applications: an entry is replaced
 * only when the same key is asked again with a different value. */
typedef struct {
    char *key;
    CHAR16 *value;
} Cached;
static Cached *cache;
static int ncache;

static const CHAR16 *cached(const char *key, const char *value)
{
    for (int i = 0; i < ncache; i++) {
        if (!strcmp(cache[i].key, key)) {
            char *old = u8(cache[i].value);
            bool same = !strcmp(old, value);
            free(old);
            if (same)
                return cache[i].value;
            free(cache[i].value); /* the application saw the previous value: acceptable per spec */
            cache[i].value = utf8_to_ucs2(value, NULL);
            return cache[i].value;
        }
    }
    cache = xrealloc(cache, sizeof(Cached) * (ncache + 1));
    cache[ncache].key = xstrdup(key);
    cache[ncache].value = utf8_to_ucs2(value, NULL);
    return cache[ncache++].value;
}

/* A list "a\0b\0\0" as a cached UCS-2 buffer. */
static const CHAR16 *cached_list(const char *key, char **names, int n)
{
    size_t total = 1;
    for (int i = 0; i < n; i++)
        total += strlen(names[i]) + 1;
    CHAR16 *buf = xcalloc(total, 2);
    size_t k = 0;
    for (int i = 0; i < n; i++) {
        uint16_t *u = utf8_to_ucs2(names[i], NULL);
        for (size_t j = 0; u[j]; j++)
            buf[k++] = u[j];
        buf[k++] = 0;
        free(u);
    }
    buf[k] = 0;
    for (int i = 0; i < ncache; i++) {
        if (!strcmp(cache[i].key, key)) {
            free(cache[i].value);
            cache[i].value = buf;
            return buf;
        }
    }
    cache = xrealloc(cache, sizeof(Cached) * (ncache + 1));
    cache[ncache].key = xstrdup(key);
    cache[ncache].value = buf;
    return cache[ncache++].value;
}

/* ---- Shell file handles ----
 * A SHELL_FILE_HANDLE is a NeshFile whose first member is an
 * EFI_FILE_PROTOCOL, so applications may also call its methods directly. */

#define NESH_FILE_MAGIC 0x4E455346 /* "NESF" */

enum { F_REAL, F_STDIN, F_STDOUT, F_STDERR, F_NUL };

typedef struct {
    EFI_FILE_PROTOCOL proto;
    UINT32 magic;
    int kind;
    EFI_FILE_PROTOCOL *real;
    char *path; /* canonical path, NULL if unknown */
    CHAR16 *in;  /* stdin: pending characters of the last line */
    size_t inlen, inpos;
} NeshFile;

static NeshFile *nf(EFI_FILE_PROTOCOL *p)
{
    NeshFile *f = (NeshFile *)p;
    return f && f->magic == NESH_FILE_MAGIC ? f : NULL;
}

static EFI_FILE_PROTOCOL *fp(SHELL_FILE_HANDLE h)
{
    return (EFI_FILE_PROTOCOL *)h;
}

static NeshFile *wrap(EFI_FILE_PROTOCOL *real, int kind, const char *path);

static EFI_STATUS EFIAPI f_open(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **New, CHAR16 *Name, UINT64 Mode, UINT64 Attr)
{
    NeshFile *f = nf(This);
    if (!f || f->kind != F_REAL)
        return EFI_UNSUPPORTED;
    EFI_FILE_PROTOCOL *r;
    EFI_STATUS st = f->real->Open(f->real, &r, Name, Mode, Attr);
    if (EFI_ERROR(st))
        return st;
    char *path = NULL;
    if (f->path) {
        char *n = u8(Name);
        char *joined = (n[0] == '\\') ? xasprintf("%.*s%s", (int)(strchr(f->path, ':') - f->path + 1), f->path, n)
                                      : path_join(f->path, n);
        path = path_resolve(joined);
        free(joined);
        free(n);
    }
    *New = &wrap(r, F_REAL, path)->proto;
    free(path);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI f_close(EFI_FILE_PROTOCOL *This)
{
    NeshFile *f = nf(This);
    if (!f)
        return EFI_INVALID_PARAMETER;
    if (f->kind != F_REAL)
        return EFI_SUCCESS; /* console handles are never closed */
    EFI_STATUS st = f->real->Close(f->real);
    f->magic = 0;
    free(f->path);
    free(f);
    return st;
}

static EFI_STATUS EFIAPI f_delete(EFI_FILE_PROTOCOL *This)
{
    NeshFile *f = nf(This);
    if (!f)
        return EFI_INVALID_PARAMETER;
    if (f->kind != F_REAL)
        return EFI_WARN_DELETE_FAILURE;
    EFI_STATUS st = f->real->Delete(f->real);
    f->magic = 0;
    free(f->path);
    free(f);
    return st;
}

static EFI_STATUS EFIAPI f_read(EFI_FILE_PROTOCOL *This, UINTN *Size, void *Buffer)
{
    NeshFile *f = nf(This);
    if (!f)
        return EFI_INVALID_PARAMETER;
    switch (f->kind) {
    case F_REAL:
        return f->real->Read(f->real, Size, Buffer);
    case F_STDIN: {
        if (f->inpos >= f->inlen) {
            free(f->in);
            f->in = NULL;
            f->inlen = f->inpos = 0;
            char *line = lineedit_read("", false);
            if (!line) {
                *Size = 0;
                return EFI_SUCCESS;
            }
            char *l2 = xasprintf("%s\n", line);
            free(line);
            f->in = utf8_to_ucs2(l2, &f->inlen);
            free(l2);
        }
        size_t units = MIN(*Size / 2, f->inlen - f->inpos);
        memcpy(Buffer, f->in + f->inpos, units * 2);
        f->inpos += units;
        *Size = units * 2;
        return EFI_SUCCESS;
    }
    default:
        *Size = 0;
        return f->kind == F_NUL ? EFI_SUCCESS : EFI_UNSUPPORTED;
    }
}

static EFI_STATUS EFIAPI f_write(EFI_FILE_PROTOCOL *This, UINTN *Size, void *Buffer)
{
    NeshFile *f = nf(This);
    if (!f)
        return EFI_INVALID_PARAMETER;
    if (f->kind == F_REAL)
        return f->real->Write(f->real, Size, Buffer);
    if (f->kind == F_NUL)
        return EFI_SUCCESS;
    if (f->kind == F_STDIN)
        return EFI_UNSUPPORTED;
    /* console: UCS-2 text */
    const CHAR16 *s = Buffer;
    size_t units = *Size / 2;
    Sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < units; i++)
        if (s[i] != '\r' && s[i] != 0xFEFF && s[i])
            sb_put_cp(&b, s[i]);
    if (b.len) {
        if (f->kind == F_STDERR)
            pal_con_write(b.s, b.len);
        else
            out_write(b.s, b.len);
    }
    sb_free(&b);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI f_getpos(EFI_FILE_PROTOCOL *This, UINT64 *Pos)
{
    NeshFile *f = nf(This);
    if (!f)
        return EFI_INVALID_PARAMETER;
    if (f->kind != F_REAL)
        return EFI_UNSUPPORTED;
    return f->real->GetPosition(f->real, Pos);
}

static EFI_STATUS EFIAPI f_setpos(EFI_FILE_PROTOCOL *This, UINT64 Pos)
{
    NeshFile *f = nf(This);
    if (!f)
        return EFI_INVALID_PARAMETER;
    if (f->kind != F_REAL)
        return EFI_UNSUPPORTED;
    return f->real->SetPosition(f->real, Pos);
}

static EFI_STATUS EFIAPI f_getinfo(EFI_FILE_PROTOCOL *This, EFI_GUID *Type, UINTN *Size, void *Buffer)
{
    NeshFile *f = nf(This);
    if (!f)
        return EFI_INVALID_PARAMETER;
    if (f->kind != F_REAL)
        return EFI_UNSUPPORTED;
    return f->real->GetInfo(f->real, Type, Size, Buffer);
}

static EFI_STATUS EFIAPI f_setinfo(EFI_FILE_PROTOCOL *This, EFI_GUID *Type, UINTN Size, void *Buffer)
{
    NeshFile *f = nf(This);
    if (!f)
        return EFI_INVALID_PARAMETER;
    if (f->kind != F_REAL)
        return EFI_UNSUPPORTED;
    return f->real->SetInfo(f->real, Type, Size, Buffer);
}

static EFI_STATUS EFIAPI f_flush(EFI_FILE_PROTOCOL *This)
{
    NeshFile *f = nf(This);
    if (!f)
        return EFI_INVALID_PARAMETER;
    return f->kind == F_REAL ? f->real->Flush(f->real) : EFI_SUCCESS;
}

static NeshFile *wrap(EFI_FILE_PROTOCOL *real, int kind, const char *path)
{
    NeshFile *f = xcalloc(1, sizeof(NeshFile));
    f->proto.Revision = 0x00010000;
    f->proto.Open = f_open;
    f->proto.Close = f_close;
    f->proto.Delete = f_delete;
    f->proto.Read = f_read;
    f->proto.Write = f_write;
    f->proto.GetPosition = f_getpos;
    f->proto.SetPosition = f_setpos;
    f->proto.GetInfo = f_getinfo;
    f->proto.SetInfo = f_setinfo;
    f->proto.Flush = f_flush;
    f->magic = NESH_FILE_MAGIC;
    f->kind = kind;
    f->real = real;
    f->path = path ? xstrdup(path) : NULL;
    return f;
}

static NeshFile *console_files[3];

SHELL_FILE_HANDLE efi_console_file(int which)
{
    if (which < 0 || which > 2)
        return NULL;
    if (!console_files[which])
        console_files[which] = wrap(NULL, which == 0 ? F_STDIN : which == 1 ? F_STDOUT : F_STDERR, NULL);
    return &console_files[which]->proto;
}

static EFI_FILE_INFO *file_info(EFI_FILE_PROTOCOL *f)
{
    EFI_GUID g = EFI_FILE_INFO_GUID;
    UINTN size = sizeof(EFI_FILE_INFO) + 512;
    EFI_FILE_INFO *info = pool(size);
    if (!info)
        return NULL;
    EFI_STATUS st = f->GetInfo(f, &g, &size, info);
    if (st == EFI_BUFFER_TOO_SMALL) {
        gBS->FreePool(info);
        info = pool(size);
        if (!info)
            return NULL;
        st = f->GetInfo(f, &g, &size, info);
    }
    if (EFI_ERROR(st)) {
        gBS->FreePool(info);
        return NULL;
    }
    return info;
}

static EFI_STATUS open_name(const char *name, UINT64 mode, UINT64 attr, NeshFile **out)
{
    if (!strcasecmp(name, "NUL")) {
        *out = wrap(NULL, F_NUL, NULL);
        return EFI_SUCCESS;
    }
    char *path = path_resolve(name);
    if (!path)
        return EFI_NOT_FOUND;
    EFI_FILE_PROTOCOL *r;
    EFI_STATUS st = efi_open_path(path, mode, attr, &r);
    if (!EFI_ERROR(st))
        *out = wrap(r, F_REAL, path);
    free(path);
    return st;
}

/* ---- File lists ---- */

static EFI_SHELL_FILE_INFO *list_head(void)
{
    EFI_SHELL_FILE_INFO *h = pool(sizeof(EFI_SHELL_FILE_INFO));
    if (h)
        h->Link.ForwardLink = h->Link.BackLink = &h->Link;
    return h;
}

static bool list_add(EFI_SHELL_FILE_INFO *head, const char *full, const char *name, NeshFile *h, EFI_FILE_INFO *info)
{
    EFI_SHELL_FILE_INFO *n = pool(sizeof(EFI_SHELL_FILE_INFO));
    if (!n)
        return false;
    n->FullName = pool_ucs2(full);
    n->FileName = pool_ucs2(name);
    n->Handle = h ? &h->proto : NULL;
    n->Info = info;
    n->Status = EFI_SUCCESS;
    n->Link.ForwardLink = &head->Link;
    n->Link.BackLink = head->Link.BackLink;
    head->Link.BackLink->ForwardLink = &n->Link;
    head->Link.BackLink = &n->Link;
    return true;
}

static void list_remove(EFI_SHELL_FILE_INFO *n, bool close)
{
    n->Link.BackLink->ForwardLink = n->Link.ForwardLink;
    n->Link.ForwardLink->BackLink = n->Link.BackLink;
    if (close && n->Handle)
        fp(n->Handle)->Close(fp(n->Handle));
    if (n->FullName)
        gBS->FreePool((void *)n->FullName);
    if (n->FileName)
        gBS->FreePool((void *)n->FileName);
    if (n->Info)
        gBS->FreePool(n->Info);
    gBS->FreePool(n);
}

static EFI_STATUS EFIAPI sh_free_file_list(EFI_SHELL_FILE_INFO **list)
{
    if (!list)
        return EFI_INVALID_PARAMETER;
    if (!*list)
        return EFI_SUCCESS;
    EFI_SHELL_FILE_INFO *head = *list;
    while (head->Link.ForwardLink != &head->Link)
        list_remove((EFI_SHELL_FILE_INFO *)head->Link.ForwardLink, true);
    gBS->FreePool(head);
    *list = NULL;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI sh_remove_dup(EFI_SHELL_FILE_INFO **list)
{
    if (!list || !*list)
        return EFI_INVALID_PARAMETER;
    LIST_ENTRY *head = &(*list)->Link;
    for (LIST_ENTRY *a = head->ForwardLink; a != head; a = a->ForwardLink) {
        EFI_SHELL_FILE_INFO *fa = (EFI_SHELL_FILE_INFO *)a;
        for (LIST_ENTRY *b = a->ForwardLink; b != head;) {
            EFI_SHELL_FILE_INFO *fb = (EFI_SHELL_FILE_INFO *)b;
            b = b->ForwardLink;
            char *x = u8(fa->FullName), *y = u8(fb->FullName);
            bool dup = x && y && !strcasecmp(x, y);
            free(x);
            free(y);
            if (dup)
                list_remove(fb, true);
        }
    }
    return EFI_SUCCESS;
}

/* Adds the files matching a pattern (wildcards in the last component). */
static EFI_STATUS add_matches(const char *pattern, UINT64 mode, EFI_SHELL_FILE_INFO **list)
{
    char *canon = path_resolve(pattern);
    if (!canon)
        return EFI_NOT_FOUND;
    int n = 0;
    char **paths;
    if (path_has_wildcards(canon)) {
        paths = path_glob(canon, &n);
    } else {
        paths = xmalloc(sizeof(char *) * 2);
        PalStat st;
        if (pal_stat(canon, &st) == PAL_OK || (mode & EFI_FILE_MODE_CREATE))
            paths[n++] = xstrdup(canon);
    }
    free(canon);
    if (!n) {
        free(paths);
        return EFI_NOT_FOUND;
    }
    bool created = false;
    if (!*list) {
        *list = list_head();
        created = true;
        if (!*list) {
            for (int i = 0; i < n; i++)
                free(paths[i]);
            free(paths);
            return EFI_OUT_OF_RESOURCES;
        }
    }
    int opened = 0;
    for (int i = 0; i < n; i++) {
        NeshFile *h = NULL;
        EFI_STATUS st = open_name(paths[i], mode, 0, &h);
        if (!EFI_ERROR(st)) {
            EFI_FILE_INFO *info = file_info(&h->proto);
            list_add(*list, paths[i], path_basename(paths[i]), h, info);
            opened++;
        }
        free(paths[i]);
    }
    free(paths);
    if (!opened && created) {
        gBS->FreePool(*list);
        *list = NULL;
        return EFI_NOT_FOUND;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI sh_open_file_list(CHAR16 *Path, UINT64 OpenMode, EFI_SHELL_FILE_INFO **FileList)
{
    if (!Path || !FileList)
        return EFI_INVALID_PARAMETER;
    char *p = u8(Path);
    EFI_STATUS st = add_matches(p, OpenMode, FileList);
    free(p);
    return st;
}

static EFI_STATUS EFIAPI sh_find_files(const CHAR16 *Pattern, EFI_SHELL_FILE_INFO **FileList)
{
    if (!Pattern || !FileList)
        return EFI_INVALID_PARAMETER;
    *FileList = NULL;
    char *p = u8(Pattern);
    EFI_STATUS st = add_matches(p, EFI_FILE_MODE_READ, FileList);
    free(p);
    return st;
}

static EFI_STATUS EFIAPI sh_find_files_in_dir(SHELL_FILE_HANDLE Dir, EFI_SHELL_FILE_INFO **FileList)
{
    if (!Dir || !FileList)
        return EFI_INVALID_PARAMETER;
    EFI_FILE_PROTOCOL *d = fp(Dir);
    NeshFile *f = nf(d);
    const char *base = f ? f->path : NULL;
    d->SetPosition(d, 0);
    *FileList = list_head();
    if (!*FileList)
        return EFI_OUT_OF_RESOURCES;
    UINTN cap = sizeof(EFI_FILE_INFO) + 512;
    EFI_FILE_INFO *buf = xmalloc(cap);
    for (;;) {
        UINTN sz = cap;
        EFI_STATUS st = d->Read(d, &sz, buf);
        if (st == EFI_BUFFER_TOO_SMALL) {
            cap = sz;
            buf = xrealloc(buf, cap);
            continue;
        }
        if (EFI_ERROR(st) || !sz)
            break;
        EFI_FILE_INFO *info = pool(sz);
        if (!info)
            break;
        memcpy(info, buf, sz);
        char *name = u8(buf->FileName);
        char *full = base ? path_join(base, name) : xstrdup(name);
        list_add(*FileList, full, name, NULL, info);
        free(name);
        free(full);
    }
    free(buf);
    return EFI_SUCCESS;
}

/* ---- File functions ---- */

static EFI_FILE_INFO *EFIAPI sh_get_file_info(SHELL_FILE_HANDLE h)
{
    return h ? file_info(fp(h)) : NULL;
}

static EFI_STATUS EFIAPI sh_set_file_info(SHELL_FILE_HANDLE h, const EFI_FILE_INFO *info)
{
    EFI_GUID g = EFI_FILE_INFO_GUID;
    if (!h || !info)
        return EFI_INVALID_PARAMETER;
    return fp(h)->SetInfo(fp(h), &g, info->Size, (void *)info);
}

static EFI_STATUS EFIAPI sh_open_file_by_name(const CHAR16 *Name, SHELL_FILE_HANDLE *Handle, UINT64 Mode)
{
    if (!Name || !Handle)
        return EFI_INVALID_PARAMETER;
    char *n = u8(Name);
    NeshFile *f = NULL;
    EFI_STATUS st = open_name(n, Mode, 0, &f);
    free(n);
    *Handle = EFI_ERROR(st) ? NULL : &f->proto;
    return st;
}

static EFI_STATUS EFIAPI sh_close_file(SHELL_FILE_HANDLE h)
{
    return h ? fp(h)->Close(fp(h)) : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI sh_create_file(const CHAR16 *Name, UINT64 Attr, SHELL_FILE_HANDLE *Handle)
{
    if (!Name || !Handle)
        return EFI_INVALID_PARAMETER;
    char *n = u8(Name);
    NeshFile *f = NULL;
    EFI_STATUS st = open_name(n, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, Attr, &f);
    free(n);
    *Handle = EFI_ERROR(st) ? NULL : &f->proto;
    return st;
}

static EFI_STATUS EFIAPI sh_read_file(SHELL_FILE_HANDLE h, UINTN *Size, void *Buf)
{
    return h ? fp(h)->Read(fp(h), Size, Buf) : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI sh_write_file(SHELL_FILE_HANDLE h, UINTN *Size, void *Buf)
{
    return h ? fp(h)->Write(fp(h), Size, Buf) : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI sh_delete_file(SHELL_FILE_HANDLE h)
{
    return h ? fp(h)->Delete(fp(h)) : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI sh_delete_file_by_name(const CHAR16 *Name)
{
    SHELL_FILE_HANDLE h;
    EFI_STATUS st = sh_open_file_by_name(Name, &h, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE);
    if (EFI_ERROR(st))
        return st;
    return sh_delete_file(h);
}

static EFI_STATUS EFIAPI sh_get_pos(SHELL_FILE_HANDLE h, UINT64 *Pos)
{
    return h && Pos ? fp(h)->GetPosition(fp(h), Pos) : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI sh_set_pos(SHELL_FILE_HANDLE h, UINT64 Pos)
{
    return h ? fp(h)->SetPosition(fp(h), Pos) : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI sh_flush(SHELL_FILE_HANDLE h)
{
    return h ? fp(h)->Flush(fp(h)) : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI sh_get_file_size(SHELL_FILE_HANDLE h, UINT64 *Size)
{
    if (!h || !Size)
        return EFI_INVALID_PARAMETER;
    EFI_FILE_INFO *info = file_info(fp(h));
    if (!info)
        return EFI_DEVICE_ERROR;
    *Size = info->FileSize;
    gBS->FreePool(info);
    return EFI_SUCCESS;
}

static int volume_of_handle(EFI_HANDLE h)
{
    for (int i = 0; i < pal_volume_count(); i++)
        if (efi_volume_handle(i) == h)
            return i;
    return -1;
}

static EFI_STATUS EFIAPI sh_open_root_by_handle(EFI_HANDLE Dev, SHELL_FILE_HANDLE *Handle)
{
    if (!Dev || !Handle)
        return EFI_INVALID_PARAMETER;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS st = gBS->HandleProtocol(Dev, &sfs_guid, (void **)&fs);
    if (EFI_ERROR(st))
        return EFI_UNSUPPORTED;
    EFI_FILE_PROTOCOL *root;
    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st))
        return st;
    int vi = volume_of_handle(Dev);
    char *path = vi >= 0 ? xasprintf("%s:\\", pal_volume(vi)->name) : NULL;
    *Handle = &wrap(root, F_REAL, path)->proto;
    free(path);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI sh_open_root(EFI_DEVICE_PATH_PROTOCOL *Dp, SHELL_FILE_HANDLE *Handle)
{
    if (!Dp || !Handle)
        return EFI_INVALID_PARAMETER;
    EFI_HANDLE h;
    EFI_DEVICE_PATH_PROTOCOL *rem = Dp;
    if (EFI_ERROR(gBS->LocateDevicePath(&sfs_guid, &rem, &h)))
        return EFI_NOT_FOUND;
    return sh_open_root_by_handle(h, Handle);
}

/* ---- Environment, aliases, help ---- */

static const CHAR16 *EFIAPI sh_get_env(const CHAR16 *Name)
{
    if (!Name) {
        int n;
        char **names = env_names(&n);
        const CHAR16 *r = cached_list("env-list", names, n);
        argv_free(names);
        return r;
    }
    char *n = u8(Name);
    char *v = env_get(n);
    char *key = xasprintf("env:%s", n);
    const CHAR16 *r = v ? cached(key, v) : NULL;
    free(key);
    free(v);
    free(n);
    return r;
}

static const CHAR16 *EFIAPI sh_get_env_ex(const CHAR16 *Name, UINT32 *Attributes)
{
    const CHAR16 *r = sh_get_env(Name);
    if (r && Name && Attributes) {
        char *n = u8(Name);
        *Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | (env_is_nv(n) ? EFI_VARIABLE_NON_VOLATILE : 0);
        free(n);
    }
    return r;
}

static EFI_STATUS EFIAPI sh_set_env(const CHAR16 *Name, const CHAR16 *Value, BOOLEAN Volatile)
{
    if (!Name || !Value)
        return EFI_INVALID_PARAMETER;
    char *n = u8(Name), *v = u8(Value);
    const char *e = env_set(n, *v ? v : NULL, !Volatile);
    bool missing_delete = e && !*v && !env_exists(n);
    bool ro = env_is_readonly(n);
    free(n);
    free(v);
    if (!e || missing_delete)
        return EFI_SUCCESS;
    return ro ? EFI_ACCESS_DENIED : EFI_OUT_OF_RESOURCES;
}

static const CHAR16 *EFIAPI sh_get_alias(const CHAR16 *Alias, BOOLEAN *Volatile)
{
    if (!Alias) {
        int n;
        char **names = alias_names(&n);
        const CHAR16 *r = cached_list("alias-list", names, n);
        argv_free(names);
        return r;
    }
    char *a = u8(Alias);
    bool nv = false;
    const char *v = alias_get(a, &nv);
    const CHAR16 *r = NULL;
    if (v) {
        char *key = xasprintf("alias:%s", a);
        r = cached(key, v);
        free(key);
        if (Volatile)
            *Volatile = !nv;
    }
    free(a);
    return r;
}

static EFI_STATUS EFIAPI sh_set_alias(const CHAR16 *Command, const CHAR16 *Alias, BOOLEAN Replace, BOOLEAN Volatile)
{
    if (!Command)
        return EFI_INVALID_PARAMETER;
    char *c = u8(Command);
    const char *e;
    if (!Alias) {
        e = alias_set(c, NULL, false, true); /* delete the alias named Command */
    } else {
        char *a = u8(Alias);
        if (!Replace && alias_get(a, NULL)) {
            free(a);
            free(c);
            return EFI_ACCESS_DENIED;
        }
        e = alias_set(a, c, !Volatile, true);
        free(a);
    }
    free(c);
    return e ? EFI_ACCESS_DENIED : EFI_SUCCESS;
}

static EFI_STATUS EFIAPI sh_get_help_text(const CHAR16 *Command, const CHAR16 *Sections, CHAR16 **HelpText)
{
    (void)Sections;
    if (!Command || !HelpText)
        return EFI_INVALID_PARAMETER;
    char *c = u8(Command);
    const Cmd *cmd = shell_find_cmd(c);
    free(c);
    if (!cmd)
        return EFI_NOT_FOUND;
    char *t = xasprintf("%s\n\n%s\n%s%s", cmd->usage ? cmd->usage : cmd->name, cmd->summary ? cmd->summary : "",
                        cmd->help ? "\n" : "", cmd->help ? cmd->help : "");
    *HelpText = pool_ucs2(t);
    free(t);
    return *HelpText ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

/* ---- Mappings and current directory ---- */

static int volume_from_mapping(const char *m)
{
    size_t l = strlen(m);
    if (l && m[l - 1] == ':')
        l--;
    char *probe = xasprintf("%.*s:", (int)l, m);
    char *p = path_resolve(probe);
    free(probe);
    int vi = -1;
    if (p) {
        for (int i = 0; i < pal_volume_count(); i++) {
            size_t vl = strlen(pal_volume(i)->name);
            if (!strncasecmp(p, pal_volume(i)->name, vl) && p[vl] == ':')
                vi = i;
        }
        free(p);
    }
    return vi;
}

static const EFI_DEVICE_PATH_PROTOCOL *EFIAPI sh_get_dp_from_map(const CHAR16 *Mapping)
{
    if (!Mapping)
        return NULL;
    char *m = u8(Mapping);
    int vi = volume_from_mapping(m);
    free(m);
    if (vi < 0)
        return NULL;
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
    gBS->HandleProtocol(efi_volume_handle(vi), &gEfiDevicePathGuid, (void **)&dp);
    return dp;
}

static bool dp_is_prefix(const EFI_DEVICE_PATH_PROTOCOL *prefix, const EFI_DEVICE_PATH_PROTOCOL *dp, size_t *len)
{
    size_t pl = efi_devpath_size(prefix);
    size_t dl = efi_devpath_size(dp);
    if (pl < 4 || dl < pl)
        return false;
    pl -= 4; /* without the end node */
    if (memcmp(prefix, dp, pl))
        return false;
    *len = pl;
    return true;
}

static int volume_of_devpath(const EFI_DEVICE_PATH_PROTOCOL *dp, size_t *matched)
{
    for (int i = 0; i < pal_volume_count(); i++) {
        EFI_DEVICE_PATH_PROTOCOL *vdp = NULL;
        if (gBS->HandleProtocol(efi_volume_handle(i), &gEfiDevicePathGuid, (void **)&vdp) != EFI_SUCCESS || !vdp)
            continue;
        if (dp_is_prefix(vdp, dp, matched))
            return i;
    }
    return -1;
}

static const CHAR16 *EFIAPI sh_get_map_from_dp(EFI_DEVICE_PATH_PROTOCOL **Dp)
{
    if (!Dp || !*Dp)
        return NULL;
    size_t matched;
    int vi = volume_of_devpath(*Dp, &matched);
    if (vi < 0)
        return NULL;
    *Dp = (EFI_DEVICE_PATH_PROTOCOL *)((uint8_t *)*Dp + matched);
    char *aliases = map_aliases_of(pal_volume(vi)->name);
    char *s = xasprintf("%s:%s%s", pal_volume(vi)->name, *aliases ? ";" : "", aliases);
    char *key = xasprintf("map:%s", pal_volume(vi)->name);
    const CHAR16 *r = cached(key, s);
    free(key);
    free(s);
    free(aliases);
    return r;
}

static EFI_DEVICE_PATH_PROTOCOL *EFIAPI sh_get_dp_from_file_path(const CHAR16 *Path)
{
    if (!Path)
        return NULL;
    char *p = u8(Path);
    char *canon = path_resolve(p);
    free(p);
    if (!canon)
        return NULL;
    EFI_DEVICE_PATH_PROTOCOL *dp = efi_file_devpath(canon);
    free(canon);
    if (!dp)
        return NULL;
    size_t sz = efi_devpath_size(dp);
    EFI_DEVICE_PATH_PROTOCOL *r = pool(sz);
    if (r)
        memcpy(r, dp, sz);
    free(dp);
    return r;
}

static CHAR16 *EFIAPI sh_get_file_path_from_dp(const EFI_DEVICE_PATH_PROTOCOL *Dp)
{
    if (!Dp)
        return NULL;
    size_t matched;
    int vi = volume_of_devpath(Dp, &matched);
    if (vi < 0)
        return NULL;
    Sbuf b;
    sb_init(&b);
    sb_printf(&b, "%s:", pal_volume(vi)->name);
    const uint8_t *p = (const uint8_t *)Dp + matched;
    for (;;) {
        const EFI_DEVICE_PATH_PROTOCOL *n = (const void *)p;
        size_t l = n->Length[0] | (n->Length[1] << 8);
        if (n->Type == END_DEVICE_PATH_TYPE || l < 4)
            break;
        if (n->Type == MEDIA_DEVICE_PATH && n->SubType == MEDIA_FILEPATH_DP) {
            size_t units = (l - 4) / 2;
            CHAR16 *tmp = xcalloc(units + 1, 2);
            memcpy(tmp, p + 4, units * 2);
            char *s = u8(tmp);
            free(tmp);
            if (b.len && b.s[b.len - 1] != '\\' && s[0] != '\\')
                sb_putc(&b, '\\');
            sb_adds(&b, s);
            free(s);
        }
        p += l;
    }
    if (b.s[b.len - 1] == ':')
        sb_putc(&b, '\\');
    CHAR16 *r = pool_ucs2(b.s);
    sb_free(&b);
    return r;
}

static EFI_STATUS EFIAPI sh_set_map(const EFI_DEVICE_PATH_PROTOCOL *Dp, const CHAR16 *Mapping)
{
    if (!Mapping)
        return EFI_INVALID_PARAMETER;
    char *m = u8(Mapping);
    size_t l = strlen(m);
    if (l && m[l - 1] == ':')
        m[--l] = 0;
    EFI_STATUS st = EFI_SUCCESS;
    if (!Dp) {
        map_alias_set(m, NULL);
    } else {
        EFI_HANDLE h;
        EFI_DEVICE_PATH_PROTOCOL *rem = (EFI_DEVICE_PATH_PROTOCOL *)Dp;
        int vi = -1;
        if (!EFI_ERROR(gBS->LocateDevicePath(&sfs_guid, &rem, &h)))
            vi = volume_of_handle(h);
        if (vi < 0)
            st = EFI_UNSUPPORTED; /* only file system volumes can be mapped */
        else if (!map_alias_set(m, pal_volume(vi)->name))
            st = EFI_ACCESS_DENIED;
    }
    free(m);
    return st;
}

static const CHAR16 *EFIAPI sh_get_cur_dir(const CHAR16 *Fs)
{
    const char *cwd = shell_cwd();
    if (!Fs)
        return *cwd ? cached("cwd", cwd) : NULL;
    char *m = u8(Fs);
    int vi = volume_from_mapping(m);
    free(m);
    if (vi < 0)
        return NULL;
    const char *vn = pal_volume(vi)->name;
    size_t vl = strlen(vn);
    char *d = (!strncasecmp(cwd, vn, vl) && cwd[vl] == ':') ? xstrdup(cwd) : xasprintf("%s:\\", vn);
    char *key = xasprintf("cwd:%s", vn);
    const CHAR16 *r = cached(key, d);
    free(key);
    free(d);
    return r;
}

static EFI_STATUS EFIAPI sh_set_cur_dir(const CHAR16 *Fs, const CHAR16 *Dir)
{
    if (!Fs && !Dir)
        return EFI_INVALID_PARAMETER;
    char *target;
    if (!Fs) {
        target = u8(Dir);
    } else {
        char *f = u8(Fs), *d = Dir ? u8(Dir) : xstrdup("\\");
        size_t l = strlen(f);
        target = xasprintf("%s%s%s", f, l && f[l - 1] == ':' ? "" : ":", d);
        free(f);
        free(d);
    }
    int e = shell_chdir(target);
    free(target);
    return e ? EFI_NOT_FOUND : EFI_SUCCESS;
}

/* ---- Device names ---- */

static bool lang_supported(const char *list, const char *lang)
{
    size_t l = strlen(lang);
    for (const char *p = list; p && *p;) {
        const char *e = strchr(p, ';');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == l && !strncasecmp(p, lang, l))
            return true;
        p = e ? e + 1 : NULL;
    }
    return false;
}

static char *pick_lang(const char *supported, const char *wanted)
{
    if (wanted && lang_supported(supported, wanted))
        return xstrdup(wanted);
    if (lang_supported(supported, "en"))
        return xstrdup("en");
    if (lang_supported(supported, "en-US"))
        return xstrdup("en-US");
    const char *e = supported ? strchr(supported, ';') : NULL;
    return supported ? xstrndup(supported, e ? (size_t)(e - supported) : strlen(supported)) : xstrdup("en");
}

static char *controller_name(EFI_HANDLE driver, EFI_HANDLE controller, EFI_HANDLE child, const char *lang)
{
    EFI_COMPONENT_NAME2_PROTOCOL *cn = NULL;
    bool v2 = gBS->HandleProtocol(driver, &cn2_guid, (void **)&cn) == EFI_SUCCESS;
    if (!v2 && gBS->HandleProtocol(driver, &cn_guid, (void **)&cn) != EFI_SUCCESS)
        return NULL;
    char *l = v2 ? pick_lang(cn->SupportedLanguages, lang) : xstrdup("eng"); /* ComponentName uses ISO 639-2 */
    CHAR16 *name = NULL;
    EFI_STATUS st = cn->GetControllerName(cn, controller, child, l, &name);
    free(l);
    return EFI_ERROR(st) || !name ? NULL : u8(name);
}

char *efi_device_name(EFI_HANDLE h, bool component_name, bool device_path)
{
    if (component_name) {
        EFI_GUID **protos = NULL;
        UINTN np = 0;
        if (gBS->ProtocolsPerHandle(h, &protos, &np) == EFI_SUCCESS) {
            for (UINTN i = 0; i < np; i++) {
                EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e = NULL;
                UINTN ne = 0;
                if (gBS->OpenProtocolInformation(h, protos[i], &e, &ne) != EFI_SUCCESS)
                    continue;
                for (UINTN k = 0; k < ne; k++) {
                    char *n = NULL;
                    if (e[k].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER)
                        n = controller_name(e[k].AgentHandle, h, NULL, "en");
                    else if (e[k].Attributes & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER)
                        n = controller_name(e[k].AgentHandle, e[k].ControllerHandle, h, "en");
                    if (n) {
                        gBS->FreePool(e);
                        gBS->FreePool(protos);
                        return n;
                    }
                }
                gBS->FreePool(e);
            }
            gBS->FreePool(protos);
        }
    }
    if (device_path) {
        EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
        if (gBS->HandleProtocol(h, &gEfiDevicePathGuid, (void **)&dp) == EFI_SUCCESS && dp)
            return efi_devpath_text(dp);
    }
    return NULL;
}

static EFI_STATUS EFIAPI sh_get_device_name(EFI_HANDLE Dev, UINT32 Flags, CHAR8 *Language, CHAR16 **Name)
{
    (void)Language;
    if (!Dev || !Name || !(Flags & (EFI_DEVICE_NAME_USE_COMPONENT_NAME | EFI_DEVICE_NAME_USE_DEVICE_PATH)))
        return EFI_INVALID_PARAMETER;
    char *n = efi_device_name(Dev, Flags & EFI_DEVICE_NAME_USE_COMPONENT_NAME, Flags & EFI_DEVICE_NAME_USE_DEVICE_PATH);
    if (!n)
        return EFI_NOT_FOUND;
    *Name = pool_ucs2(n);
    free(n);
    return *Name ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

/* ---- GUID names ---- */

static EFI_STATUS EFIAPI sh_register_guid_name(const EFI_GUID *Guid, const CHAR16 *Name)
{
    if (!Guid || !Name)
        return EFI_INVALID_PARAMETER;
    char *n = u8(Name);
    bool ok = guid_db_register(Guid, n);
    free(n);
    return ok ? EFI_SUCCESS : EFI_ACCESS_DENIED;
}

static EFI_STATUS EFIAPI sh_get_guid_name(const EFI_GUID *Guid, const CHAR16 **Name)
{
    if (!Guid || !Name)
        return EFI_INVALID_PARAMETER;
    const char *n = guid_db_name(Guid);
    if (!n)
        return EFI_NOT_FOUND;
    char gs[37];
    guid_format(Guid, gs);
    char *key = xasprintf("guid:%s", gs);
    *Name = cached(key, n);
    free(key);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI sh_get_guid_from_name(const CHAR16 *Name, EFI_GUID *Guid)
{
    if (!Name || !Guid)
        return EFI_INVALID_PARAMETER;
    char *n = u8(Name);
    bool ok = guid_db_find(n, Guid) || guid_parse(n, Guid);
    free(n);
    return ok ? EFI_SUCCESS : EFI_NOT_FOUND;
}

/* ---- Execution ---- */

static EFI_STATUS EFIAPI sh_execute(EFI_HANDLE *Parent, CHAR16 *CommandLine, CHAR16 **Environment, EFI_STATUS *Status)
{
    (void)Parent;
    if (!CommandLine)
        return EFI_UNSUPPORTED; /* a nested interactive shell is not provided */
    /* temporary environment for this command only: "name=value" strings */
    int pushed = 0;
    for (CHAR16 **e = Environment; e && *e; e++) {
        char *s = u8(*e);
        char *eq = strchr(s, '=');
        if (eq) {
            *eq = 0;
            env_overlay_push(s, eq + 1);
            pushed++;
        }
        free(s);
    }
    char *line = u8(CommandLine);
    int rc = shell_exec_line(line);
    free(line);
    env_overlay_pop(pushed);
    if (Status)
        *Status = rc ? EFIERR((UINTN)rc) : EFI_SUCCESS;
    return EFI_SUCCESS;
}

static BOOLEAN EFIAPI sh_batch_is_active(void)
{
    return shell_script_depth() > 0;
}

static BOOLEAN EFIAPI sh_is_root_shell(void)
{
    return previous == NULL;
}

static void EFIAPI sh_enable_page_break(void)
{
    page_break = true;
}

static void EFIAPI sh_disable_page_break(void)
{
    page_break = false;
}

static BOOLEAN EFIAPI sh_get_page_break(void)
{
    return page_break;
}

/* ---- Ctrl-C: signalled by a key notification, even while an application runs ---- */

static void *notify_handles[3];
static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *notify_in;

static EFI_STATUS EFIAPI on_ctrl_c(EFI_KEY_DATA *KeyData)
{
    (void)KeyData;
    gBS->SignalEvent(shell.ExecutionBreak);
    return EFI_SUCCESS;
}

bool efi_break_pending(void)
{
    return shell.ExecutionBreak && gBS->CheckEvent(shell.ExecutionBreak) == EFI_SUCCESS;
}

void efi_break_clear(void)
{
    if (shell.ExecutionBreak)
        gBS->CheckEvent(shell.ExecutionBreak); /* CheckEvent resets a signalled event */
}

static void register_ctrl_c(void)
{
    if (gBS->HandleProtocol(gST->ConsoleInHandle, &textinex_guid, (void **)&notify_in) != EFI_SUCCESS)
        return;
    EFI_REGISTER_KEYSTROKE_NOTIFY reg = (EFI_REGISTER_KEYSTROKE_NOTIFY)notify_in->RegisterKeyNotify;
    EFI_KEY_DATA k;
    memset(&k, 0, sizeof(k));
    k.Key.UnicodeChar = 3; /* serial terminals send ^C as a character */
    reg(notify_in, &k, on_ctrl_c, &notify_handles[0]);
    k.Key.UnicodeChar = 'c';
    k.KeyState.KeyShiftState = EFI_SHIFT_STATE_VALID | EFI_LEFT_CONTROL_PRESSED;
    reg(notify_in, &k, on_ctrl_c, &notify_handles[1]);
    k.KeyState.KeyShiftState = EFI_SHIFT_STATE_VALID | EFI_RIGHT_CONTROL_PRESSED;
    reg(notify_in, &k, on_ctrl_c, &notify_handles[2]);
}

static void unregister_ctrl_c(void)
{
    if (!notify_in)
        return;
    EFI_UNREGISTER_KEYSTROKE_NOTIFY unreg = (EFI_UNREGISTER_KEYSTROKE_NOTIFY)notify_in->UnregisterKeyNotify;
    for (int i = 0; i < 3; i++)
        if (notify_handles[i])
            unreg(notify_in, notify_handles[i]);
}

/* ---- Installation ---- */

void efi_shell_protocol_install(void)
{
    shell.Execute = sh_execute;
    shell.GetEnv = sh_get_env;
    shell.SetEnv = sh_set_env;
    shell.GetAlias = sh_get_alias;
    shell.SetAlias = sh_set_alias;
    shell.GetHelpText = sh_get_help_text;
    shell.GetDevicePathFromMap = sh_get_dp_from_map;
    shell.GetMapFromDevicePath = sh_get_map_from_dp;
    shell.GetDevicePathFromFilePath = sh_get_dp_from_file_path;
    shell.GetFilePathFromDevicePath = sh_get_file_path_from_dp;
    shell.SetMap = sh_set_map;
    shell.GetCurDir = sh_get_cur_dir;
    shell.SetCurDir = sh_set_cur_dir;
    shell.OpenFileList = sh_open_file_list;
    shell.FreeFileList = sh_free_file_list;
    shell.RemoveDupInFileList = sh_remove_dup;
    shell.BatchIsActive = sh_batch_is_active;
    shell.IsRootShell = sh_is_root_shell;
    shell.EnablePageBreak = sh_enable_page_break;
    shell.DisablePageBreak = sh_disable_page_break;
    shell.GetPageBreak = sh_get_page_break;
    shell.GetDeviceName = sh_get_device_name;
    shell.GetFileInfo = sh_get_file_info;
    shell.SetFileInfo = sh_set_file_info;
    shell.OpenFileByName = sh_open_file_by_name;
    shell.CloseFile = sh_close_file;
    shell.CreateFile = sh_create_file;
    shell.ReadFile = sh_read_file;
    shell.WriteFile = sh_write_file;
    shell.DeleteFile = sh_delete_file;
    shell.DeleteFileByName = sh_delete_file_by_name;
    shell.GetFilePosition = sh_get_pos;
    shell.SetFilePosition = sh_set_pos;
    shell.FlushFile = sh_flush;
    shell.FindFiles = sh_find_files;
    shell.FindFilesInDir = sh_find_files_in_dir;
    shell.GetFileSize = sh_get_file_size;
    shell.OpenRoot = sh_open_root;
    shell.OpenRootByHandle = sh_open_root_by_handle;
    shell.MajorVersion = 2;
    shell.MinorVersion = 2;
    shell.RegisterGuidName = sh_register_guid_name;
    shell.GetGuidName = sh_get_guid_name;
    shell.GetGuidFromName = sh_get_guid_from_name;
    shell.GetEnvEx = sh_get_env_ex;
    gBS->CreateEvent(0, 0, NULL, NULL, &shell.ExecutionBreak);
    register_ctrl_c();

    /* Started from another shell: take its place (restored when NESH exits). */
    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    if (gBS->LocateHandleBuffer(ByProtocol, &shell_guid, NULL, &n, &hs) == EFI_SUCCESS && n > 0) {
        EFI_SHELL_PROTOCOL *old = NULL;
        if (gBS->HandleProtocol(hs[0], &shell_guid, (void **)&old) == EFI_SUCCESS &&
            gBS->ReinstallProtocolInterface(hs[0], &shell_guid, old, &shell) == EFI_SUCCESS) {
            previous = old;
            installed_on = hs[0];
        }
        gBS->FreePool(hs);
        if (installed_on)
            return;
    }
    EFI_HANDLE h = gImage;
    if (gBS->InstallProtocolInterface(&h, &shell_guid, EFI_NATIVE_INTERFACE, &shell) == EFI_SUCCESS)
        installed_on = h;
}

void efi_shell_protocol_uninstall(void)
{
    unregister_ctrl_c();
    if (installed_on) {
        if (previous)
            gBS->ReinstallProtocolInterface(installed_on, &shell_guid, &shell, previous);
        else
            gBS->UninstallProtocolInterface(installed_on, &shell_guid, &shell);
        installed_on = NULL;
    }
    if (shell.ExecutionBreak) {
        gBS->CloseEvent(shell.ExecutionBreak);
        shell.ExecutionBreak = NULL;
    }
}
