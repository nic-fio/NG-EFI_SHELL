/* Test application for NESH's EFI_SHELL_PROTOCOL implementation.
 * Started by the QEMU tests as: shelltest.efi alpha "beta gamma"
 * Prints one "PASS name" / "FAIL name" line per check and "SHELLTEST DONE n" at the end.
 * Needs a writable volume fs1:. */
#include "../../include/efi_shell.h"

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_SHELL_PROTOCOL *Sh;
static int fails;

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

static void print(const CHAR16 *s)
{
    ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)s);
}

static int slen(const CHAR16 *s)
{
    int n = 0;
    while (s && s[n])
        n++;
    return n;
}

static int scmp(const CHAR16 *a, const CHAR16 *b)
{
    if (!a || !b)
        return a != b;
    while (*a && *a == *b)
        a++, b++;
    return *a - *b;
}

static int icmp(const CHAR16 *a, const CHAR16 *b)
{
    if (!a || !b)
        return a != b;
    for (;; a++, b++) {
        CHAR16 x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y || !x)
            return x - y;
    }
}

static int contains(const CHAR16 *h, const CHAR16 *n)
{
    int ln = slen(n);
    for (; h && *h; h++) {
        int k = 0;
        while (k < ln && h[k] == n[k])
            k++;
        if (k == ln)
            return 1;
    }
    return 0;
}

static void check(const CHAR16 *name, int ok)
{
    print(ok ? u"PASS " : u"FAIL ");
    print(name);
    print(u"\r\n");
    if (!ok)
        fails++;
}

static int list_count(EFI_SHELL_FILE_INFO *l)
{
    int n = 0;
    if (!l)
        return 0;
    for (LIST_ENTRY *e = l->Link.ForwardLink; e != &l->Link; e = e->ForwardLink)
        n++;
    return n;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    ST = st;
    BS = st->BootServices;
    EFI_GUID shell_guid = EFI_SHELL_PROTOCOL_GUID;
    EFI_GUID params_guid = EFI_SHELL_PARAMETERS_PROTOCOL_GUID;
    EFI_STATUS s;

    s = BS->LocateProtocol(&shell_guid, NULL, (void **)&Sh);
    check(u"locate shell protocol", s == EFI_SUCCESS && Sh);
    if (!Sh)
        return EFI_NOT_FOUND;
    check(u"version 2.2", Sh->MajorVersion == 2 && Sh->MinorVersion >= 2);

    /* parameters */
    EFI_SHELL_PARAMETERS_PROTOCOL *sp = NULL;
    s = BS->HandleProtocol(image, &params_guid, (void **)&sp);
    check(u"shell parameters", s == EFI_SUCCESS && sp && sp->Argc == 3 && !scmp(sp->Argv[1], u"alpha") &&
                                    !scmp(sp->Argv[2], u"beta gamma"));
    UINTN n = 14 * sizeof(CHAR16);
    s = Sh->WriteFile(sp->StdOut, &n, (void *)u"stdout works\r\n");
    check(u"write to StdOut", s == EFI_SUCCESS);

    /* environment */
    check(u"GetEnv path", slen(Sh->GetEnv(u"path")) > 0);
    check(u"SetEnv", Sh->SetEnv(u"apptest", u"v1", TRUE) == EFI_SUCCESS && !scmp(Sh->GetEnv(u"apptest"), u"v1"));
    UINT32 attr = 0xFFFF;
    check(u"GetEnvEx volatile", Sh->GetEnvEx(u"apptest", &attr) && !(attr & EFI_VARIABLE_NON_VOLATILE));
    const CHAR16 *list = Sh->GetEnv(NULL);
    int found = 0;
    for (const CHAR16 *p = list; p && *p; p += slen(p) + 1)
        if (!icmp(p, u"apptest"))
            found = 1;
    check(u"GetEnv list", found);
    s = Sh->SetEnv(u"apptest", u"", TRUE);
    const CHAR16 *after = Sh->GetEnv(u"apptest");
    if (s != EFI_SUCCESS)
        print(u"  SetEnv delete: error status\r\n");
    if (after) {
        print(u"  SetEnv delete: still set to [");
        print(after);
        print(u"]\r\n");
    }
    check(u"SetEnv delete", s == EFI_SUCCESS && !after);
    check(u"read-only cwd", Sh->SetEnv(u"cwd", u"x", TRUE) != EFI_SUCCESS);

    /* current directory */
    check(u"SetCurDir", Sh->SetCurDir(NULL, u"fs1:\\") == EFI_SUCCESS);
    check(u"GetCurDir", !icmp(Sh->GetCurDir(NULL), u"fs1:\\"));
    check(u"GetCurDir of fs0", !icmp(Sh->GetCurDir(u"fs0:"), u"fs0:\\"));

    /* files */
    SHELL_FILE_HANDLE h = NULL;
    s = Sh->CreateFile(u"app.txt", 0, &h);
    check(u"CreateFile (relative)", s == EFI_SUCCESS && h);
    n = 6;
    check(u"WriteFile", Sh->WriteFile(h, &n, (void *)"hello!") == EFI_SUCCESS && n == 6);
    UINT64 pos = 0;
    check(u"GetFilePosition", Sh->GetFilePosition(h, &pos) == EFI_SUCCESS && pos == 6);
    check(u"FlushFile", Sh->FlushFile(h) == EFI_SUCCESS);
    check(u"SetFilePosition", Sh->SetFilePosition(h, 0) == EFI_SUCCESS);
    char buf[16];
    n = sizeof(buf);
    check(u"ReadFile", Sh->ReadFile(h, &n, buf) == EFI_SUCCESS && n == 6 && buf[0] == 'h' && buf[5] == '!');
    UINT64 size = 0;
    check(u"GetFileSize", Sh->GetFileSize(h, &size) == EFI_SUCCESS && size == 6);
    EFI_FILE_INFO *info = Sh->GetFileInfo(h);
    check(u"GetFileInfo", info && info->FileSize == 6 && !icmp(info->FileName, u"app.txt"));
    if (info) {
        check(u"SetFileInfo", Sh->SetFileInfo(h, info) == EFI_SUCCESS);
        BS->FreePool(info);
    }
    /* a SHELL_FILE_HANDLE can also be used as an EFI_FILE_PROTOCOL */
    EFI_FILE_PROTOCOL *fp = (EFI_FILE_PROTOCOL *)h;
    check(u"handle as EFI_FILE_PROTOCOL", fp->SetPosition(fp, 1) == EFI_SUCCESS);
    check(u"CloseFile", Sh->CloseFile(h) == EFI_SUCCESS);
    s = Sh->OpenFileByName(u"fs1:\\app.txt", &h, EFI_FILE_MODE_READ);
    check(u"OpenFileByName", s == EFI_SUCCESS);
    if (s == EFI_SUCCESS)
        Sh->CloseFile(h);
    check(u"OpenFileByName missing", Sh->OpenFileByName(u"fs1:\\nope.txt", &h, EFI_FILE_MODE_READ) == EFI_NOT_FOUND);

    EFI_SHELL_FILE_INFO *fl = NULL;
    s = Sh->FindFiles(u"fs1:\\*.txt", &fl);
    check(u"FindFiles", s == EFI_SUCCESS && list_count(fl) >= 1);
    if (fl) {
        EFI_SHELL_FILE_INFO *first = (EFI_SHELL_FILE_INFO *)fl->Link.ForwardLink;
        check(u"file list names", contains(first->FullName, u"fs1:\\") && first->Info && first->Handle);
    }
    check(u"FreeFileList", Sh->FreeFileList(&fl) == EFI_SUCCESS && fl == NULL);
    s = Sh->OpenFileList(u"*.txt", EFI_FILE_MODE_READ, &fl);
    check(u"OpenFileList relative", s == EFI_SUCCESS && list_count(fl) >= 1);
    int before = list_count(fl);
    s = Sh->OpenFileList(u"app.txt", EFI_FILE_MODE_READ, &fl);
    check(u"OpenFileList append", s == EFI_SUCCESS && list_count(fl) == before + 1);
    check(u"RemoveDupInFileList", Sh->RemoveDupInFileList(&fl) == EFI_SUCCESS && list_count(fl) == before);
    Sh->FreeFileList(&fl);

    const EFI_DEVICE_PATH_PROTOCOL *vdp = Sh->GetDevicePathFromMap(u"fs1:");
    check(u"GetDevicePathFromMap", vdp != NULL);
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_HANDLE vol = NULL;
    EFI_DEVICE_PATH_PROTOCOL *rem = (EFI_DEVICE_PATH_PROTOCOL *)vdp;
    if (vdp)
        BS->LocateDevicePath(&sfs_guid, &rem, &vol);
    SHELL_FILE_HANDLE root = NULL;
    s = vol ? Sh->OpenRootByHandle(vol, &root) : EFI_NOT_FOUND;
    check(u"OpenRootByHandle", s == EFI_SUCCESS);
    if (root) {
        s = Sh->FindFilesInDir(root, &fl);
        int ok = 0;
        if (s == EFI_SUCCESS && fl)
            for (LIST_ENTRY *e = fl->Link.ForwardLink; e != &fl->Link; e = e->ForwardLink)
                if (!icmp(((EFI_SHELL_FILE_INFO *)e)->FileName, u"app.txt"))
                    ok = 1;
        check(u"FindFilesInDir", ok);
        Sh->FreeFileList(&fl);
        Sh->CloseFile(root);
    }
    s = Sh->OpenRoot((EFI_DEVICE_PATH_PROTOCOL *)vdp, &root);
    check(u"OpenRoot", s == EFI_SUCCESS);
    if (s == EFI_SUCCESS)
        Sh->CloseFile(root);

    /* mappings */
    EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)vdp;
    const CHAR16 *map = Sh->GetMapFromDevicePath(&dp);
    check(u"GetMapFromDevicePath", map && contains(map, u"fs1:"));
    EFI_DEVICE_PATH_PROTOCOL *fdp = Sh->GetDevicePathFromFilePath(u"fs1:\\app.txt");
    check(u"GetDevicePathFromFilePath", fdp != NULL);
    CHAR16 *back = fdp ? Sh->GetFilePathFromDevicePath(fdp) : NULL;
    check(u"GetFilePathFromDevicePath", back && !icmp(back, u"fs1:\\app.txt"));
    if (back)
        BS->FreePool(back);
    if (fdp)
        BS->FreePool(fdp);
    check(u"SetMap", Sh->SetMap(vdp, u"data:") == EFI_SUCCESS);
    s = Sh->OpenFileByName(u"data:\\app.txt", &h, EFI_FILE_MODE_READ);
    check(u"open through new mapping", s == EFI_SUCCESS);
    if (s == EFI_SUCCESS)
        Sh->CloseFile(h);
    check(u"SetMap delete", Sh->SetMap(NULL, u"data:") == EFI_SUCCESS &&
                                 Sh->OpenFileByName(u"data:\\app.txt", &h, EFI_FILE_MODE_READ) != EFI_SUCCESS);

    check(u"DeleteFileByName", Sh->DeleteFileByName(u"fs1:\\app.txt") == EFI_SUCCESS &&
                                   Sh->OpenFileByName(u"fs1:\\app.txt", &h, EFI_FILE_MODE_READ) == EFI_NOT_FOUND);

    /* execution */
    EFI_STATUS cs = 1;
    s = Sh->Execute(&image, u"echo from Execute", NULL, &cs);
    check(u"Execute", s == EFI_SUCCESS && cs == EFI_SUCCESS);
    s = Sh->Execute(&image, u"nosuchcommand", NULL, &cs);
    check(u"Execute error status", s == EFI_SUCCESS && EFI_ERROR(cs));
    CHAR16 *env[] = { u"tmpfoo=bar", NULL };
    s = Sh->Execute(&image, u"set tmpfoo", env, &cs);
    check(u"Execute with environment", s == EFI_SUCCESS && cs == EFI_SUCCESS && !Sh->GetEnv(u"tmpfoo"));

    /* aliases and help */
    check(u"SetAlias", Sh->SetAlias(u"ls -l", u"lltest", TRUE, TRUE) == EFI_SUCCESS);
    BOOLEAN vol_alias = FALSE;
    check(u"GetAlias", !scmp(Sh->GetAlias(u"lltest", &vol_alias), u"ls -l") && vol_alias);
    check(u"SetAlias delete", Sh->SetAlias(u"lltest", NULL, TRUE, TRUE) == EFI_SUCCESS && !Sh->GetAlias(u"lltest", NULL));
    CHAR16 *help = NULL;
    check(u"GetHelpText", Sh->GetHelpText(u"ls", NULL, &help) == EFI_SUCCESS && contains(help, u"ls"));
    if (help)
        BS->FreePool(help);

    /* GUID names */
    const CHAR16 *gname = NULL;
    check(u"GetGuidName", Sh->GetGuidName(&shell_guid, &gname) == EFI_SUCCESS && !scmp(gname, u"Shell"));
    EFI_GUID g;
    check(u"GetGuidFromName", Sh->GetGuidFromName(u"ShellParameters", &g) == EFI_SUCCESS && g.Data1 == 0x752F3136);
    EFI_GUID mine = { 0x12345678, 0x1234, 0x5678, { 1, 2, 3, 4, 5, 6, 7, 8 } };
    check(u"RegisterGuidName", Sh->RegisterGuidName(&mine, u"NeshTestGuid") == EFI_SUCCESS &&
                                   Sh->GetGuidFromName(u"NeshTestGuid", &g) == EFI_SUCCESS && g.Data1 == 0x12345678);

    /* devices and state */
    CHAR16 *dname = NULL;
    s = vol ? Sh->GetDeviceName(vol, EFI_DEVICE_NAME_USE_COMPONENT_NAME | EFI_DEVICE_NAME_USE_DEVICE_PATH, (CHAR8 *)"en",
                                &dname)
            : EFI_NOT_FOUND;
    check(u"GetDeviceName", s == EFI_SUCCESS && slen(dname) > 0);
    if (dname) {
        print(u"  device name: ");
        print(dname);
        print(u"\r\n");
        BS->FreePool(dname);
    }
    check(u"IsRootShell", Sh->IsRootShell());
    Sh->EnablePageBreak();
    int pb = Sh->GetPageBreak();
    Sh->DisablePageBreak();
    check(u"page break flag", pb && !Sh->GetPageBreak());
    check(u"BatchIsActive", Sh->BatchIsActive()); /* started from a script */
    check(u"ExecutionBreak not signalled", BS->CheckEvent(Sh->ExecutionBreak) == EFI_NOT_READY);

    Sh->SetCurDir(NULL, u"fs0:\\");
    print(fails ? u"SHELLTEST DONE with failures\r\n" : u"SHELLTEST DONE ok\r\n");
    return fails ? EFI_ABORTED : EFI_SUCCESS;
}
