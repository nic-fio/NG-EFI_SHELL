/* Drivers and devices: drivers, devices, devtree, dh, openinfo, connect,
 * disconnect, reconnect, load, unload, drvdiag, drvcfg.
 *
 * Handles are shown as small hexadecimal numbers that stay the same for the
 * whole session (new handles get new numbers). Relations between drivers and
 * controllers come from the open-protocol information of the handle database:
 *   BY_DRIVER             agent = driver managing the controller
 *   BY_CHILD_CONTROLLER   agent = bus driver, ControllerHandle = child */
#include "efi_cmds.h"
#include "../../../include/efi_shell.h"

static EFI_GUID binding_guid = EFI_DRIVER_BINDING_PROTOCOL_GUID;
static EFI_GUID cn2_guid = EFI_COMPONENT_NAME2_PROTOCOL_GUID;
static EFI_GUID cn_guid = EFI_COMPONENT_NAME_PROTOCOL_GUID;
static EFI_GUID diag2_guid = EFI_DRIVER_DIAGNOSTICS2_PROTOCOL_GUID;
static EFI_GUID cfg2_guid = EFI_DRIVER_CONFIGURATION2_PROTOCOL_GUID;
static EFI_GUID hiicfg_guid = EFI_HII_CONFIG_ACCESS_PROTOCOL_GUID;
static EFI_GUID blockio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID pciio_guid = { 0x4CF5B200, 0x68B8, 0x4CA5, { 0x9E, 0xEC, 0xB2, 0x3E, 0x3F, 0x50, 0x02, 0x9A } };

/* ---- Stable handle numbers ---- */

static EFI_HANDLE *hdb;
static int nhdb;

static void hdb_refresh(void)
{
    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    if (gBS->LocateHandleBuffer(AllHandles, NULL, NULL, &n, &hs) != EFI_SUCCESS)
        return;
    for (UINTN i = 0; i < n; i++) {
        bool known = false;
        for (int k = 0; k < nhdb && !known; k++)
            known = hdb[k] == hs[i];
        if (!known) {
            hdb = xrealloc(hdb, sizeof(EFI_HANDLE) * (nhdb + 1));
            hdb[nhdb++] = hs[i];
        }
    }
    gBS->FreePool(hs);
}

int efi_handle_index(EFI_HANDLE h)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < nhdb; k++)
            if (hdb[k] == h)
                return k + 1;
        hdb_refresh();
    }
    return 0;
}

static bool handle_alive(EFI_HANDLE h)
{
    EFI_GUID **p = NULL;
    UINTN n = 0;
    if (gBS->ProtocolsPerHandle(h, &p, &n) != EFI_SUCCESS)
        return false;
    gBS->FreePool(p);
    return true;
}

bool efi_parse_handle(const char *s, EFI_HANDLE *h)
{
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    if (!*s || *end || !v)
        return false;
    hdb_refresh();
    if ((int)v > nhdb || !handle_alive(hdb[v - 1]))
        return false;
    *h = hdb[v - 1];
    return true;
}

/* ---- Snapshot of the open-protocol information ---- */

typedef struct {
    EFI_HANDLE h;
    EFI_GUID proto;
    EFI_HANDLE agent, ctrl;
    UINT32 attr, count;
} Rec;

typedef struct {
    EFI_HANDLE *all;
    int nall;
    Rec *r;
    int nr;
} Snap;

static void snap_build(Snap *s)
{
    memset(s, 0, sizeof(*s));
    hdb_refresh();
    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    if (gBS->LocateHandleBuffer(AllHandles, NULL, NULL, &n, &hs) != EFI_SUCCESS)
        return;
    s->all = xmalloc(sizeof(EFI_HANDLE) * (n ? n : 1));
    memcpy(s->all, hs, sizeof(EFI_HANDLE) * n);
    s->nall = (int)n;
    gBS->FreePool(hs);
    for (int i = 0; i < s->nall; i++) {
        EFI_GUID **protos = NULL;
        UINTN np = 0;
        if (gBS->ProtocolsPerHandle(s->all[i], &protos, &np) != EFI_SUCCESS)
            continue;
        for (UINTN p = 0; p < np; p++) {
            EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e = NULL;
            UINTN ne = 0;
            if (gBS->OpenProtocolInformation(s->all[i], protos[p], &e, &ne) != EFI_SUCCESS)
                continue;
            for (UINTN k = 0; k < ne; k++) {
                s->r = xrealloc(s->r, sizeof(Rec) * (s->nr + 1));
                Rec *r = &s->r[s->nr++];
                r->h = s->all[i];
                r->proto = *protos[p];
                r->agent = e[k].AgentHandle;
                r->ctrl = e[k].ControllerHandle;
                r->attr = e[k].Attributes;
                r->count = e[k].OpenCount;
            }
            gBS->FreePool(e);
        }
        gBS->FreePool(protos);
    }
}

static void snap_free(Snap *s)
{
    free(s->all);
    free(s->r);
    memset(s, 0, sizeof(*s));
}

typedef struct {
    EFI_HANDLE *v;
    int n;
} HList;

static void hl_add(HList *l, EFI_HANDLE h)
{
    if (!h)
        return;
    for (int i = 0; i < l->n; i++)
        if (l->v[i] == h)
            return;
    l->v = xrealloc(l->v, sizeof(EFI_HANDLE) * (l->n + 1));
    l->v[l->n++] = h;
}

static void hl_free(HList *l)
{
    free(l->v);
    l->v = NULL;
    l->n = 0;
}

static bool hl_has(const HList *l, EFI_HANDLE h)
{
    for (int i = 0; i < l->n; i++)
        if (l->v[i] == h)
            return true;
    return false;
}

/* Drivers managing a controller. */
static void drivers_of(const Snap *s, EFI_HANDLE ctrl, HList *out)
{
    for (int i = 0; i < s->nr; i++)
        if (s->r[i].h == ctrl && (s->r[i].attr & EFI_OPEN_PROTOCOL_BY_DRIVER))
            hl_add(out, s->r[i].agent);
}

/* Child controllers created on a controller (by any driver, or by drv if not NULL). */
static void children_of(const Snap *s, EFI_HANDLE ctrl, EFI_HANDLE drv, HList *out)
{
    for (int i = 0; i < s->nr; i++)
        if (s->r[i].h == ctrl && (s->r[i].attr & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) && s->r[i].ctrl != ctrl &&
            (!drv || s->r[i].agent == drv))
            hl_add(out, s->r[i].ctrl);
}

static void parents_of(const Snap *s, EFI_HANDLE h, HList *out)
{
    for (int i = 0; i < s->nr; i++)
        if ((s->r[i].attr & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) && s->r[i].ctrl == h && s->r[i].h != h)
            hl_add(out, s->r[i].h);
}

/* Controllers managed by a driver, and children it created. */
static void managed_by(const Snap *s, EFI_HANDLE drv, HList *ctrls, HList *children)
{
    for (int i = 0; i < s->nr; i++) {
        if (s->r[i].agent != drv)
            continue;
        if ((s->r[i].attr & EFI_OPEN_PROTOCOL_BY_DRIVER) && ctrls)
            hl_add(ctrls, s->r[i].h);
        if ((s->r[i].attr & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) && children && s->r[i].ctrl != s->r[i].h)
            hl_add(children, s->r[i].ctrl);
    }
}

static bool has_proto(EFI_HANDLE h, EFI_GUID *g)
{
    void *p;
    return gBS->HandleProtocol(h, g, &p) == EFI_SUCCESS;
}

/* ---- Names ---- */

static const char *lang_opt; /* -l LANG */

static char *pick_lang(const char *supported, bool iso639_2)
{
    if (iso639_2)
        return xstrdup(lang_opt && strlen(lang_opt) == 3 ? lang_opt : "eng");
    const char *want[] = { lang_opt, "en", "en-US" };
    for (size_t w = 0; w < ARRAY_SIZE(want); w++) {
        if (!want[w] || !supported)
            continue;
        size_t l = strlen(want[w]);
        for (const char *p = supported; *p;) {
            const char *e = strchr(p, ';');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (n == l && !strncasecmp(p, want[w], l))
                return xstrdup(want[w]);
            if (!e)
                break;
            p = e + 1;
        }
    }
    const char *e = supported ? strchr(supported, ';') : NULL;
    return supported ? xstrndup(supported, e ? (size_t)(e - supported) : strlen(supported)) : xstrdup("en");
}

static char *driver_name(EFI_HANDLE drv)
{
    EFI_COMPONENT_NAME2_PROTOCOL *cn = NULL;
    bool v2 = gBS->HandleProtocol(drv, &cn2_guid, (void **)&cn) == EFI_SUCCESS;
    if (!v2 && gBS->HandleProtocol(drv, &cn_guid, (void **)&cn) != EFI_SUCCESS)
        return NULL;
    char *l = pick_lang(cn->SupportedLanguages, !v2);
    CHAR16 *name = NULL;
    EFI_STATUS st = cn->GetDriverName(cn, l, &name);
    free(l);
    return EFI_ERROR(st) || !name ? NULL : ucs2_to_utf8(name, (size_t)-1);
}

/* Module name from the CodeView debug entry of a loaded PE image, if present. */
static char *pe_debug_name(const EFI_LOADED_IMAGE_PROTOCOL *li)
{
    const uint8_t *b = li->ImageBase;
    if (!b || li->ImageSize < 0x200 || b[0] != 'M' || b[1] != 'Z')
        return NULL;
    uint32_t pe = *(const uint32_t *)(b + 0x3c);
    if (pe + 0x108 > li->ImageSize || memcmp(b + pe, "PE\0\0", 4))
        return NULL;
    uint16_t magic = *(const uint16_t *)(b + pe + 24);
    uint32_t ddoff = pe + 24 + (magic == 0x20b ? 112 : 96);
    uint32_t nrva = *(const uint32_t *)(b + pe + 24 + (magic == 0x20b ? 108 : 92));
    if (nrva <= 6)
        return NULL;
    uint32_t drva = *(const uint32_t *)(b + ddoff + 6 * 8), dsize = *(const uint32_t *)(b + ddoff + 6 * 8 + 4);
    if (!drva || drva + dsize > li->ImageSize)
        return NULL;
    for (uint32_t off = 0; off + 28 <= dsize; off += 28) {
        const uint8_t *d = b + drva + off;
        uint32_t type = *(const uint32_t *)(d + 12), rva = *(const uint32_t *)(d + 20);
        uint32_t size = *(const uint32_t *)(d + 16);
        if (type != 2 || !rva || rva + size > li->ImageSize || size < 8)
            continue;
        const char *cv = (const char *)(b + rva);
        size_t skip = !memcmp(cv, "RSDS", 4) ? 24 : !memcmp(cv, "NB10", 4) ? 16 : !memcmp(cv, "MTOC", 4) ? 20 : 0;
        if (!skip || skip >= size)
            continue;
        const char *path = cv + skip;
        size_t pl = strnlen(path, size - skip);
        const char *base = path;
        for (size_t i = 0; i < pl; i++)
            if (path[i] == '/' || path[i] == '\\')
                base = path + i + 1;
        const char *dot = memchr(base, '.', (size_t)(path + pl - base));
        return xstrndup(base, dot ? (size_t)(dot - base) : (size_t)(path + pl - base));
    }
    return NULL;
}

/* Module name stored in the firmware volume (user interface section of the file). */
typedef struct {
    void *GetVolumeAttributes;
    void *SetVolumeAttributes;
    void *ReadFile;
    EFI_STATUS(EFIAPI *ReadSection)(void *This, const EFI_GUID *NameGuid, UINT8 SectionType, UINTN SectionInstance,
                                    void **Buffer, UINTN *BufferSize, UINT32 *AuthenticationStatus);
} FV2_PROTOCOL_HDR;

static char *fv_ui_name(const EFI_LOADED_IMAGE_PROTOCOL *li)
{
    static EFI_GUID fv2_guid = { 0x220E73B6, 0x6BDB, 0x4413, { 0x84, 0x05, 0xB9, 0x74, 0xB1, 0x08, 0x61, 0x9A } };
    const EFI_DEVICE_PATH_PROTOCOL *n = li->FilePath;
    if (!n || !li->DeviceHandle)
        return NULL;
    /* the last node before the end must be FvFile(GUID) */
    const EFI_DEVICE_PATH_PROTOCOL *file = NULL;
    for (int guard = 0; guard < 16 && n->Type != END_DEVICE_PATH_TYPE; guard++) {
        size_t l = n->Length[0] | (n->Length[1] << 8);
        if (l < 4)
            return NULL;
        if (n->Type == MEDIA_DEVICE_PATH && n->SubType == MEDIA_PIWG_FW_FILE_DP && l >= 20)
            file = n;
        n = (const void *)((const uint8_t *)n + l);
    }
    FV2_PROTOCOL_HDR *fv = NULL;
    if (!file || gBS->HandleProtocol(li->DeviceHandle, &fv2_guid, (void **)&fv) != EFI_SUCCESS)
        return NULL;
    void *buf = NULL;
    UINTN size = 0;
    UINT32 auth = 0;
    if (fv->ReadSection(fv, (const EFI_GUID *)((const uint8_t *)file + 4), 0x15 /* user interface */, 0, &buf, &size,
                        &auth) != EFI_SUCCESS || !buf)
        return NULL;
    char *r = ucs2_to_utf8(buf, size / 2);
    gBS->FreePool(buf);
    return r;
}

static char *image_name(EFI_HANDLE image)
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    if (!image || gBS->HandleProtocol(image, &gEfiLoadedImageGuid, (void **)&li) != EFI_SUCCESS || !li)
        return xstrdup("");
    char *n = pe_debug_name(li);
    if (!n)
        n = fv_ui_name(li);
    if (n)
        return n;
    char *t = efi_devpath_text(li->FilePath);
    return t;
}

static char *device_name(EFI_HANDLE h)
{
    char *n = efi_device_name(h, true, true);
    return n ? n : xstrdup("<unknown>");
}

static void print_trunc(const char *s, int width)
{
    int n = (int)utf8_len(s, strlen(s));
    if (n <= width) {
        out_printf("%s%*s", s, width - n, "");
        return;
    }
    size_t off = utf8_offset(s, strlen(s), (size_t)(width - 1));
    out_write(s, off);
    out_puts("~");
}

/* Common options: -l LANG, -b (page break, accepted), -c, -r, -d, -v... */
typedef struct {
    bool flag[128];
    const char *lang;
    const char *proto;
    char *pos[8];
    int npos;
} Opts;

static bool parse_opts(const char *cmd, int argc, char **argv, const char *flags, Opts *o)
{
    memset(o, 0, sizeof(*o));
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcasecmp(a, "-l") && i + 1 < argc) {
            o->lang = argv[++i];
        } else if (!strcasecmp(a, "-p") && i + 1 < argc && strchr(flags, 'p')) {
            o->proto = argv[++i];
        } else if (!strcasecmp(a, "-verbose") && strchr(flags, 'v')) {
            o->flag['v'] = true;
        } else if (!strcasecmp(a, "-nc") && strchr(flags, 'N')) {
            o->flag['N'] = true;
        } else if (a[0] == '-' && a[1] && !a[2] && (strchr(flags, tolower((uint8_t)a[1])) || a[1] == 'b')) {
            o->flag[tolower((uint8_t)a[1])] = true; /* -b (page break) is accepted for compatibility */
        } else if (a[0] == '-' && a[1]) {
            err_printf("%s: unknown option %s\n", cmd, a);
            return false;
        } else if (o->npos < 8) {
            o->pos[o->npos++] = argv[i];
        }
    }
    lang_opt = o->lang;
    return true;
}

static bool get_handle_arg(const char *cmd, const char *s, EFI_HANDLE *h)
{
    if (efi_parse_handle(s, h))
        return true;
    err_printf("%s: %s is not a valid handle number (see dh)\n", cmd, s);
    return false;
}

/* ---- drivers ---- */

static int cmd_drivers(int argc, char **argv)
{
    Opts o;
    if (!parse_opts("drivers", argc, argv, "", &o) || o.npos)
        return o.npos ? cmd_usage("drivers") : RC_USAGE;
    Snap s;
    snap_build(&s);
    UINTN n = 0;
    EFI_HANDLE *drv = NULL;
    gBS->LocateHandleBuffer(ByProtocol, &binding_guid, NULL, &n, &drv);
    bool data = out_data_mode();
    if (!data) {
        out_puts("            T   D\n            Y C I\n            P F A\n");
        out_puts("DRV VERSION E G G #D #C DRIVER NAME                         IMAGE NAME\n");
        out_puts("=== ======= = = = == == =================================== ===================\n");
    }
    for (UINTN i = 0; i < n && !con_break(); i++) {
        EFI_DRIVER_BINDING_PROTOCOL *db = NULL;
        gBS->HandleProtocol(drv[i], &binding_guid, (void **)&db);
        HList ctrls = { 0 }, kids = { 0 };
        managed_by(&s, drv[i], &ctrls, &kids);
        bool cfg = has_proto(drv[i], &cfg2_guid);
        bool diag = has_proto(drv[i], &diag2_guid);
        char *name = driver_name(drv[i]);
        char *img = image_name(db ? db->ImageHandle : NULL);
        if (data) {
            data_record();
            data_field("handle", "%X", efi_handle_index(drv[i]));
            data_field("version", "%X", db ? db->Version : 0);
            data_field("type", "%s", kids.n ? "bus" : "device");
            data_field("config", "%s", cfg ? "yes" : "no");
            data_field("diag", "%s", diag ? "yes" : "no");
            data_field("controllers", "%d", ctrls.n);
            data_field("children", "%d", kids.n);
            data_field("name", "%s", name ? name : "");
            data_field("image", "%s", img);
            goto next;
        }
        out_printf("%3X %07X %c %c %c ", efi_handle_index(drv[i]), db ? db->Version : 0, kids.n ? 'B' : 'D',
                   cfg ? 'X' : '-', diag ? 'X' : '-');
        if (ctrls.n)
            out_printf("%2d ", ctrls.n);
        else
            out_puts(" - ");
        if (kids.n)
            out_printf("%2d ", kids.n);
        else
            out_puts(" - ");
        print_trunc(name ? name : "<unknown>", 35);
        out_printf(" %s\n", img);
    next:
        free(name);
        free(img);
        hl_free(&ctrls);
        hl_free(&kids);
    }
    if (drv)
        gBS->FreePool(drv);
    snap_free(&s);
    if (!data)
        out_puts("TYPE: B bus driver, D device driver. CFG/DIAG: configuration/diagnostics support.\n");
    return RC_OK;
}

/* ---- devices ---- */

static bool is_controller(const Snap *s, EFI_HANDLE h)
{
    /* a device has a device path and is not an image or a driver */
    if (!has_proto(h, &gEfiDevicePathGuid) || has_proto(h, &gEfiLoadedImageGuid) || has_proto(h, &binding_guid))
        return false;
    (void)s;
    return true;
}

static int cmd_devices(int argc, char **argv)
{
    Opts o;
    if (!parse_opts("devices", argc, argv, "", &o) || o.npos)
        return o.npos ? cmd_usage("devices") : RC_USAGE;
    Snap s;
    snap_build(&s);
    bool data = out_data_mode();
    if (!data) {
        out_puts("     T   D\n     Y C I\n     P F A\n");
        out_puts("CTRL E G G #P #D #C Device Name\n");
        out_puts("==== = = = == == == =============================================================\n");
    }
    for (int i = 0; i < s.nall && !con_break(); i++) {
        EFI_HANDLE h = s.all[i];
        if (!is_controller(&s, h))
            continue;
        HList par = { 0 }, drv = { 0 }, kids = { 0 };
        parents_of(&s, h, &par);
        drivers_of(&s, h, &drv);
        children_of(&s, h, NULL, &kids);
        bool cfg = false, diag = false;
        for (int k = 0; k < drv.n; k++) {
            cfg |= has_proto(drv.v[k], &cfg2_guid);
            diag |= has_proto(drv.v[k], &diag2_guid);
        }
        char *name = device_name(h);
        if (data) {
            data_record();
            data_field("handle", "%X", efi_handle_index(h));
            data_field("type", "%s", !par.n ? "root" : kids.n ? "bus" : "device");
            data_field("config", "%s", cfg ? "yes" : "no");
            data_field("diag", "%s", diag ? "yes" : "no");
            data_field("parents", "%d", par.n);
            data_field("drivers", "%d", drv.n);
            data_field("children", "%d", kids.n);
            data_field("name", "%s", name);
        } else {
            out_printf("%4X %c %c %c ", efi_handle_index(h), !par.n ? 'R' : kids.n ? 'B' : 'D', cfg ? 'X' : '-',
                       diag ? 'X' : '-');
            for (int k = 0; k < 3; k++) {
                int v = k == 0 ? par.n : k == 1 ? drv.n : kids.n;
                if (v)
                    out_printf("%2d ", v);
                else
                    out_puts(" - ");
            }
            out_printf("%s\n", name);
        }
        free(name);
        hl_free(&par);
        hl_free(&drv);
        hl_free(&kids);
    }
    snap_free(&s);
    if (!data)
        out_puts("TYPE: R root controller, B bus controller, D device. #P parents, #D drivers, #C children.\n");
    return RC_OK;
}

/* ---- devtree ---- */

static void tree(const Snap *s, EFI_HANDLE h, int depth, bool show_dp, HList *seen)
{
    if (hl_has(seen, h) || depth > 32)
        return;
    hl_add(seen, h);
    char *name = show_dp ? NULL : efi_device_name(h, true, false);
    if (!name) {
        EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
        gBS->HandleProtocol(h, &gEfiDevicePathGuid, (void **)&dp);
        if (dp) {
            name = efi_devpath_text(dp);
        } else {
            /* virtual device without a path (e.g. console splitter): show its protocols */
            EFI_GUID **protos = NULL;
            UINTN np = 0;
            Sbuf b;
            sb_init(&b);
            sb_adds(&b, "<");
            char gs[37];
            if (gBS->ProtocolsPerHandle(h, &protos, &np) == EFI_SUCCESS) {
                for (UINTN i = 0; i < np && i < 4; i++)
                    sb_printf(&b, "%s%s", i ? " " : "", guid_str(protos[i], gs));
                gBS->FreePool(protos);
            }
            sb_adds(&b, ">");
            name = sb_steal(&b);
        }
    }
    out_printf("%*sCtrl[%X] %s\n", depth * 2, "", efi_handle_index(h), name);
    free(name);
    HList kids = { 0 };
    children_of(s, h, NULL, &kids);
    for (int i = 0; i < kids.n && !con_break(); i++)
        tree(s, kids.v[i], depth + 1, show_dp, seen);
    hl_free(&kids);
}

static int cmd_devtree(int argc, char **argv)
{
    Opts o;
    if (!parse_opts("devtree", argc, argv, "d", &o) || o.npos > 1)
        return o.npos > 1 ? cmd_usage("devtree") : RC_USAGE;
    Snap s;
    snap_build(&s);
    HList seen = { 0 };
    if (o.npos) {
        EFI_HANDLE h;
        if (!get_handle_arg("devtree", o.pos[0], &h)) {
            snap_free(&s);
            return RC_FAIL;
        }
        tree(&s, h, 0, o.flag['d'], &seen);
    } else {
        for (int i = 0; i < s.nall && !con_break(); i++) {
            EFI_HANDLE h = s.all[i];
            if (!is_controller(&s, h))
                continue;
            HList par = { 0 };
            parents_of(&s, h, &par);
            if (!par.n)
                tree(&s, h, 0, o.flag['d'], &seen);
            hl_free(&par);
        }
    }
    hl_free(&seen);
    snap_free(&s);
    return RC_OK;
}

/* ---- dh ---- */

static void pciio_location(void *p, UINTN loc[4])
{
    typedef EFI_STATUS(EFIAPI * GetLoc)(void *, UINTN *, UINTN *, UINTN *, UINTN *);
    GetLoc f = (GetLoc)((void **)p)[14]; /* EFI_PCI_IO_PROTOCOL.GetLocation */
    loc[0] = loc[1] = loc[2] = loc[3] = 0;
    f(p, &loc[0], &loc[1], &loc[2], &loc[3]);
}

static void dh_detail(EFI_HANDLE h, const EFI_GUID *g)
{
    void *p = NULL;
    if (gBS->HandleProtocol(h, (EFI_GUID *)g, &p) != EFI_SUCCESS || !p)
        return;
    if (!memcmp(g, &gEfiDevicePathGuid, sizeof(EFI_GUID))) {
        char *t = efi_devpath_text(p);
        out_printf("      %s\n", t);
        free(t);
    } else if (!memcmp(g, &gEfiLoadedImageGuid, sizeof(EFI_GUID))) {
        EFI_LOADED_IMAGE_PROTOCOL *li = p;
        char *t = efi_devpath_text(li->FilePath);
        char *n = pe_debug_name(li);
        out_printf("      Name: %s  File: %s\n", n ? n : "-", t);
        out_printf("      Base: 0x%llx  Size: 0x%llx  Code: %s  Parent: %X  Device: %X\n",
                   (unsigned long long)(uintptr_t)li->ImageBase, (unsigned long long)li->ImageSize,
                   li->ImageCodeType == EfiLoaderCode ? "application"
                   : li->ImageCodeType == EfiRuntimeServicesCode ? "runtime driver" : "boot driver",
                   efi_handle_index(li->ParentHandle), li->DeviceHandle ? efi_handle_index(li->DeviceHandle) : 0);
        free(t);
        free(n);
    } else if (!memcmp(g, &binding_guid, sizeof(EFI_GUID))) {
        EFI_DRIVER_BINDING_PROTOCOL *db = p;
        char *n = driver_name(h);
        out_printf("      Version: 0x%X  Image: %X  Name: %s\n", db->Version, efi_handle_index(db->ImageHandle),
                   n ? n : "-");
        free(n);
    } else if (!memcmp(g, &blockio_guid, sizeof(EFI_GUID))) {
        EFI_BLOCK_IO_PROTOCOL *b = p;
        out_printf("      %s%s%s, block size %u, %llu blocks (%llu MiB)\n", b->Media->RemovableMedia ? "removable " : "",
                   b->Media->LogicalPartition ? "partition" : "disk", b->Media->ReadOnly ? ", read-only" : "",
                   b->Media->BlockSize, (unsigned long long)(b->Media->LastBlock + 1),
                   (unsigned long long)((b->Media->LastBlock + 1) * b->Media->BlockSize / (1024 * 1024)));
    } else if (!memcmp(g, &sfs_guid, sizeof(EFI_GUID))) {
        for (int i = 0; i < pal_volume_count(); i++)
            if (efi_volume_handle(i) == h)
                out_printf("      Volume %s: \"%s\"\n", pal_volume(i)->name, pal_volume(i)->label);
    } else if (!memcmp(g, &pciio_guid, sizeof(EFI_GUID))) {
        UINTN loc[4];
        pciio_location(p, loc);
        out_printf("      Segment %llx Bus %02llx Device %02llx Function %llx\n", (unsigned long long)loc[0],
                   (unsigned long long)loc[1], (unsigned long long)loc[2], (unsigned long long)loc[3]);
    }
}

static void dh_one(EFI_HANDLE h, bool detail)
{
    EFI_GUID **protos = NULL;
    UINTN np = 0;
    if (gBS->ProtocolsPerHandle(h, &protos, &np) != EFI_SUCCESS)
        return;
    char gs[37];
    if (out_data_mode()) {
        data_record();
        data_field("handle", "%X", efi_handle_index(h));
        char *name = efi_device_name(h, true, false);
        data_field("name", "%s", name ? name : "");
        free(name);
        Sbuf b;
        sb_init(&b);
        for (UINTN i = 0; i < np; i++)
            sb_printf(&b, "%s%s", i ? " " : "", guid_str(protos[i], gs));
        data_field("protocols", "%s", b.s ? b.s : "");
        sb_free(&b);
        EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
        if (detail && gBS->HandleProtocol(h, &gEfiDevicePathGuid, (void **)&dp) == EFI_SUCCESS && dp) {
            char *t = efi_devpath_text(dp);
            data_field("devicepath", "%s", t);
            free(t);
        }
    } else if (!detail) {
        out_printf("%3X:", efi_handle_index(h));
        for (UINTN i = 0; i < np; i++)
            out_printf(" %s", guid_str(protos[i], gs));
        out_puts("\n");
    } else {
        char *name = efi_device_name(h, true, false);
        out_printf("Handle %X (%p)%s%s\n", efi_handle_index(h), h, name ? "  " : "", name ? name : "");
        free(name);
        for (UINTN i = 0; i < np; i++) {
            out_printf("   %s\n", guid_str(protos[i], gs));
            dh_detail(h, protos[i]);
        }
    }
    gBS->FreePool(protos);
}

static int cmd_dh(int argc, char **argv)
{
    Opts o;
    if (!parse_opts("dh", argc, argv, "dvp", &o) || o.npos > 1)
        return o.npos > 1 ? cmd_usage("dh") : RC_USAGE;
    bool detail = o.flag['d'] || o.flag['v'];
    if (o.npos) {
        EFI_HANDLE h;
        if (!get_handle_arg("dh", o.pos[0], &h))
            return RC_FAIL;
        dh_one(h, true);
        return RC_OK;
    }
    EFI_GUID filter;
    if (o.proto && !guid_db_find(o.proto, &filter) && !guid_parse(o.proto, &filter))
        return cmd_err("dh", "unknown protocol %s (use a name like BlockIo or a GUID)", o.proto);
    hdb_refresh();
    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    EFI_STATUS st = o.proto ? gBS->LocateHandleBuffer(ByProtocol, &filter, NULL, &n, &hs)
                            : gBS->LocateHandleBuffer(AllHandles, NULL, NULL, &n, &hs);
    if (EFI_ERROR(st)) {
        if (o.proto)
            out_printf("No handle has the %s protocol.\n", o.proto);
        return o.proto ? RC_OK : cmd_err("dh", "cannot read the handle database");
    }
    /* in handle-number order */
    int *idx = xmalloc(sizeof(int) * n);
    for (UINTN i = 0; i < n; i++)
        idx[i] = efi_handle_index(hs[i]);
    for (UINTN i = 0; i < n && !con_break(); i++) {
        UINTN best = i;
        for (UINTN k = i + 1; k < n; k++)
            if (idx[k] < idx[best])
                best = k;
        int ti = idx[i];
        idx[i] = idx[best];
        idx[best] = ti;
        EFI_HANDLE th = hs[i];
        hs[i] = hs[best];
        hs[best] = th;
        dh_one(hs[i], detail);
    }
    free(idx);
    gBS->FreePool(hs);
    return RC_OK;
}

/* ---- openinfo ---- */

static int cmd_openinfo(int argc, char **argv)
{
    Opts o;
    if (!parse_opts("openinfo", argc, argv, "", &o) || o.npos != 1)
        return o.npos != 1 ? cmd_usage("openinfo") : RC_USAGE;
    EFI_HANDLE h;
    if (!get_handle_arg("openinfo", o.pos[0], &h))
        return RC_FAIL;
    EFI_GUID **protos = NULL;
    UINTN np = 0;
    if (gBS->ProtocolsPerHandle(h, &protos, &np) != EFI_SUCCESS)
        return cmd_err("openinfo", "cannot read the handle");
    out_printf("Handle %X (%p)\n", efi_handle_index(h), h);
    char gs[37];
    for (UINTN i = 0; i < np; i++) {
        out_printf("   %s\n", guid_str(protos[i], gs));
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e = NULL;
        UINTN ne = 0;
        if (gBS->OpenProtocolInformation(h, protos[i], &e, &ne) != EFI_SUCCESS)
            continue;
        for (UINTN k = 0; k < ne; k++) {
            UINT32 a = e[k].Attributes;
            const char *how = a & EFI_OPEN_PROTOCOL_EXCLUSIVE ? (a & EFI_OPEN_PROTOCOL_BY_DRIVER ? "Driver+Exclusive" : "Exclusive")
                            : a & EFI_OPEN_PROTOCOL_BY_DRIVER ? "Driver"
                            : a & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER ? "Child"
                            : a & EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL ? "HandProt"
                            : a & EFI_OPEN_PROTOCOL_GET_PROTOCOL ? "GetProt"
                            : a & EFI_OPEN_PROTOCOL_TEST_PROTOCOL ? "TestProt" : "?";
            char *img = NULL;
            EFI_DRIVER_BINDING_PROTOCOL *db = NULL;
            if (e[k].AgentHandle && gBS->HandleProtocol(e[k].AgentHandle, &binding_guid, (void **)&db) == EFI_SUCCESS)
                img = image_name(db->ImageHandle);
            else
                img = image_name(e[k].AgentHandle);
            out_printf("      Drv[%X] Ctrl[%X] Cnt(%u) %-16s %s\n", e[k].AgentHandle ? efi_handle_index(e[k].AgentHandle) : 0,
                       e[k].ControllerHandle ? efi_handle_index(e[k].ControllerHandle) : 0, e[k].OpenCount, how, img);
            free(img);
        }
        gBS->FreePool(e);
    }
    gBS->FreePool(protos);
    return RC_OK;
}

/* ---- connect / disconnect / reconnect ---- */

static void connect_all(bool recursive)
{
    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    if (gBS->LocateHandleBuffer(AllHandles, NULL, NULL, &n, &hs) != EFI_SUCCESS)
        return;
    for (UINTN i = 0; i < n; i++)
        gBS->ConnectController(hs[i], NULL, NULL, recursive);
    gBS->FreePool(hs);
}

/* Connects the drivers along a device path (like the firmware does for consoles). */
static EFI_STATUS connect_devpath(EFI_DEVICE_PATH_PROTOCOL *dp)
{
    EFI_HANDLE prev = NULL;
    for (int guard = 0; guard < 32; guard++) {
        EFI_DEVICE_PATH_PROTOCOL *rem = dp;
        EFI_HANDLE h;
        EFI_STATUS st = gBS->LocateDevicePath(&gEfiDevicePathGuid, &rem, &h);
        if (EFI_ERROR(st))
            return st;
        if (rem->Type == END_DEVICE_PATH_TYPE) {
            gBS->ConnectController(h, NULL, NULL, TRUE);
            return EFI_SUCCESS;
        }
        if (h == prev)
            return EFI_NOT_FOUND; /* no driver produced the next node */
        prev = h;
        gBS->ConnectController(h, NULL, rem, FALSE);
    }
    return EFI_NOT_FOUND;
}

static void connect_consoles(void)
{
    static const char *vars[] = { "ConIn", "ConOut", "ErrOut" };
    for (size_t v = 0; v < ARRAY_SIZE(vars); v++) {
        size_t n;
        uint8_t *d = efi_var_read(vars[v], &gEfiGlobalVariableGuid, &n, NULL, NULL);
        if (!d)
            continue;
        /* multi-instance device path: instances end with END_INSTANCE nodes */
        size_t start = 0, off = 0;
        while (off + 4 <= n) {
            EFI_DEVICE_PATH_PROTOCOL *node = (void *)(d + off);
            size_t l = node->Length[0] | (node->Length[1] << 8);
            if (l < 4 || off + l > n)
                break;
            if (node->Type == END_DEVICE_PATH_TYPE) {
                node->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE; /* terminate this instance */
                connect_devpath((EFI_DEVICE_PATH_PROTOCOL *)(d + start));
                start = off + l;
            }
            off += l;
        }
        free(d);
    }
}

static int report(const char *what, EFI_HANDLE a, EFI_HANDLE b, EFI_STATUS st)
{
    if (b)
        out_printf("%s - Handle [%X] Driver [%X] Result %s.\n", what, efi_handle_index(a), efi_handle_index(b),
                   EFI_ERROR(st) ? efi_strerror(st) : "Success");
    else
        out_printf("%s - Handle [%X] Result %s.\n", what, efi_handle_index(a), EFI_ERROR(st) ? efi_strerror(st) : "Success");
    return EFI_ERROR(st) ? RC_FAIL : RC_OK;
}

static int cmd_connect(int argc, char **argv)
{
    Opts o;
    if (!parse_opts("connect", argc, argv, "rc", &o) || o.npos > 2)
        return o.npos > 2 ? cmd_usage("connect") : RC_USAGE;
    if (o.flag['c']) {
        connect_consoles();
        out_puts("Console devices connected.\n");
        if (!o.npos)
            return RC_OK;
    }
    if (!o.npos) {
        connect_all(true);
        out_puts("All controllers connected.\n");
        return RC_OK;
    }
    EFI_HANDLE ctrl, drv = NULL;
    if (o.npos == 2) {
        if (!get_handle_arg("connect", o.pos[0], &drv) || !get_handle_arg("connect", o.pos[1], &ctrl))
            return RC_FAIL;
        if (!has_proto(drv, &binding_guid))
            return cmd_err("connect", "%s is not a driver", o.pos[0]);
    } else if (!get_handle_arg("connect", o.pos[0], &ctrl)) {
        return RC_FAIL;
    }
    EFI_HANDLE list[2] = { drv, NULL };
    EFI_STATUS st = gBS->ConnectController(ctrl, drv ? list : NULL, NULL, o.flag['r']);
    return report("Connect", ctrl, drv, st);
}

static int disconnect_common(const char *cmd, int argc, char **argv, bool reconnect)
{
    Opts o;
    if (!parse_opts(cmd, argc, argv, "r", &o))
        return RC_USAGE;
    if (o.npos > 3 || (!o.npos && !o.flag['r']))
        return cmd_usage(cmd);
    if (o.flag['r'] && !o.npos) {
        UINTN n = 0;
        EFI_HANDLE *hs = NULL;
        if (gBS->LocateHandleBuffer(AllHandles, NULL, NULL, &n, &hs) == EFI_SUCCESS) {
            for (UINTN i = 0; i < n; i++)
                gBS->DisconnectController(hs[i], NULL, NULL);
            gBS->FreePool(hs);
        }
        if (reconnect)
            connect_all(true);
        else
            connect_consoles(); /* keep the screen and the keyboard working */
        out_printf("All controllers %s.\n", reconnect ? "reconnected" : "disconnected (consoles reconnected)");
        return RC_OK;
    }
    EFI_HANDLE h[3] = { NULL, NULL, NULL };
    for (int i = 0; i < o.npos; i++)
        if (!get_handle_arg(cmd, o.pos[i], &h[i]))
            return RC_FAIL;
    if (h[1] && !has_proto(h[1], &binding_guid))
        return cmd_err(cmd, "%s is not a driver", o.pos[1]);
    EFI_STATUS st = gBS->DisconnectController(h[0], h[1], h[2]);
    int rc = report("Disconnect", h[0], h[1], st);
    if (reconnect && !EFI_ERROR(st)) {
        EFI_HANDLE list[2] = { h[1], NULL };
        st = gBS->ConnectController(h[0], h[1] ? list : NULL, NULL, TRUE);
        rc = report("Connect", h[0], h[1], st);
    }
    return rc;
}

static int cmd_disconnect(int argc, char **argv) { return disconnect_common("disconnect", argc, argv, false); }
static int cmd_reconnect(int argc, char **argv) { return disconnect_common("reconnect", argc, argv, true); }

/* ---- load / unload ---- */

static int cmd_load(int argc, char **argv)
{
    Opts o;
    if (!parse_opts("load", argc, argv, "N", &o) || !o.npos)
        return !o.npos ? cmd_usage("load") : RC_USAGE;
    int rc = RC_OK, started = 0;
    for (int i = 0; i < o.npos; i++) {
        int n;
        char **files;
        char *p = path_resolve(o.pos[i]);
        if (!p) {
            rc = cmd_err("load", "%s: invalid path", o.pos[i]);
            continue;
        }
        if (path_has_wildcards(p)) {
            files = path_glob(p, &n);
        } else {
            files = xmalloc(sizeof(char *));
            files[0] = xstrdup(p);
            n = 1;
        }
        free(p);
        for (int k = 0; k < n; k++) {
            char *data;
            size_t size;
            int e = file_read_all(files[k], &data, &size);
            if (e) {
                rc = cmd_perr("load", files[k], e);
                free(files[k]);
                continue;
            }
            EFI_DEVICE_PATH_PROTOCOL *dp = efi_file_devpath(files[k]);
            EFI_HANDLE img = NULL;
            EFI_STATUS st = gBS->LoadImage(FALSE, gImage, dp, data, size, &img);
            free(data);
            free(dp);
            if (EFI_ERROR(st)) {
                if (img)
                    gBS->UnloadImage(img);
                rc = cmd_err("load", "%s: %s", files[k],
                             efi_blocked_by_secure_boot(st)
                                 ? "not allowed by Secure Boot (the driver is not signed by a trusted key)"
                                 : efi_strerror(st));
                free(files[k]);
                continue;
            }
            EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
            gBS->HandleProtocol(img, &gEfiLoadedImageGuid, (void **)&li);
            if (li && li->ImageCodeType == EfiLoaderCode) {
                gBS->UnloadImage(img);
                rc = cmd_err("load", "%s is an application, not a driver: run it by typing its name", files[k]);
                free(files[k]);
                continue;
            }
            uintptr_t base = (uintptr_t)(li ? li->ImageBase : 0); /* li is gone if the driver fails */
            st = gBS->StartImage(img, NULL, NULL);
            out_printf("Image '%s' loaded at 0x%llx - %s\n", files[k], (unsigned long long)base,
                       EFI_ERROR(st) ? efi_strerror(st) : "Success");
            if (EFI_ERROR(st))
                rc = RC_FAIL;
            else
                started++;
            free(files[k]);
        }
        free(files);
    }
    if (started && !o.flag['N'])
        connect_all(true); /* let the new drivers bind to their devices */
    return rc;
}

static int cmd_unload(int argc, char **argv)
{
    Opts o;
    if (!parse_opts("unload", argc, argv, "nv", &o) || o.npos != 1)
        return o.npos != 1 ? cmd_usage("unload") : RC_USAGE;
    EFI_HANDLE h;
    if (!get_handle_arg("unload", o.pos[0], &h))
        return RC_FAIL;
    if (!has_proto(h, &gEfiLoadedImageGuid))
        return cmd_err("unload", "handle %s is not an image", o.pos[0]);
    if (h == gImage)
        return cmd_err("unload", "NESH cannot unload itself (use exit)");
    if (o.flag['v'])
        dh_one(h, true);
    if (!o.flag['n']) {
        char *img = image_name(h);
        char *q = xasprintf("Unload handle %X (%s)? [y/N] ", efi_handle_index(h), img);
        char *ans = lineedit_read(q, false);
        bool yes = ans && (!strcasecmp(ans, "y") || !strcasecmp(ans, "yes"));
        free(ans);
        free(q);
        free(img);
        if (!yes) {
            out_puts("Cancelled.\n");
            return RC_FAIL;
        }
    }
    EFI_STATUS st = gBS->UnloadImage(h);
    out_printf("Unload - Handle [%X] Result %s.\n", efi_handle_index(h),
               EFI_ERROR(st) ? (st == EFI_UNSUPPORTED ? "not supported: the driver cannot be unloaded" : efi_strerror(st))
                             : "Success");
    return EFI_ERROR(st) ? RC_FAIL : RC_OK;
}

/* ---- drvdiag / drvcfg ---- */

typedef struct {
    EFI_HANDLE drv, ctrl, child;
} Target;

/* Driver/controller(/child) combinations for the given (optional) handles. */
static Target *targets(const Snap *s, EFI_GUID *proto, EFI_HANDLE want_drv, EFI_HANDLE want_ctrl, EFI_HANDLE want_child,
                       bool with_children, int *count)
{
    Target *t = NULL;
    int n = 0;
    UINTN nd = 0;
    EFI_HANDLE *drivers = NULL;
    gBS->LocateHandleBuffer(ByProtocol, proto, NULL, &nd, &drivers);
    for (UINTN i = 0; i < nd; i++) {
        if (want_drv && drivers[i] != want_drv)
            continue;
        HList ctrls = { 0 };
        managed_by(s, drivers[i], &ctrls, NULL);
        for (int c = 0; c < ctrls.n; c++) {
            if (want_ctrl && ctrls.v[c] != want_ctrl)
                continue;
            if (!want_child) {
                t = xrealloc(t, sizeof(Target) * (n + 1));
                t[n++] = (Target){ drivers[i], ctrls.v[c], NULL };
            }
            if (with_children || want_child) {
                HList kids = { 0 };
                children_of(s, ctrls.v[c], drivers[i], &kids);
                for (int k = 0; k < kids.n; k++) {
                    if (want_child && kids.v[k] != want_child)
                        continue;
                    t = xrealloc(t, sizeof(Target) * (n + 1));
                    t[n++] = (Target){ drivers[i], ctrls.v[c], kids.v[k] };
                }
                hl_free(&kids);
            }
        }
        hl_free(&ctrls);
    }
    if (drivers)
        gBS->FreePool(drivers);
    *count = n;
    return t;
}

static bool target_args(const char *cmd, Opts *o, EFI_HANDLE h[3])
{
    h[0] = h[1] = h[2] = NULL;
    for (int i = 0; i < o->npos && i < 3; i++)
        if (!get_handle_arg(cmd, o->pos[i], &h[i]))
            return false;
    return o->npos <= 3 || (cmd_usage(cmd), false);
}

static void print_target(const Target *t)
{
    out_printf("Drv[%X] Ctrl[%X]", efi_handle_index(t->drv), efi_handle_index(t->ctrl));
    if (t->child)
        out_printf(" Child[%X]", efi_handle_index(t->child));
}

static int cmd_drvdiag(int argc, char **argv)
{
    Opts o;
    if (!parse_opts("drvdiag", argc, argv, "csem", &o))
        return RC_USAGE;
    EFI_HANDLE h[3];
    if (!target_args("drvdiag", &o, h))
        return RC_FAIL;
    int modes = o.flag['s'] + o.flag['e'] + o.flag['m'];
    if (modes > 1)
        return cmd_err("drvdiag", "choose only one of -s, -e, -m");
    Snap s;
    snap_build(&s);
    int n;
    Target *t = targets(&s, &diag2_guid, h[0], h[1], h[2], o.flag['c'], &n);
    int rc = RC_OK;
    if (!n)
        out_puts("No driver with diagnostics manages the selected devices.\n");
    for (int i = 0; i < n && !con_break(); i++) {
        char *dn = driver_name(t[i].drv);
        print_target(&t[i]);
        if (!modes) {
            out_printf("  %s\n", dn ? dn : "");
            free(dn);
            continue;
        }
        free(dn);
        EFI_DRIVER_DIAGNOSTICS2_PROTOCOL *d = NULL;
        gBS->HandleProtocol(t[i].drv, &diag2_guid, (void **)&d);
        char *lang = pick_lang(d->SupportedLanguages, false);
        EFI_GUID *err = NULL;
        UINTN bsize = 0;
        CHAR16 *buf = NULL;
        EFI_DRIVER_DIAGNOSTIC_TYPE type = o.flag['e'] ? EfiDriverDiagnosticTypeExtended
                                        : o.flag['m'] ? EfiDriverDiagnosticTypeManufacturing
                                                      : EfiDriverDiagnosticTypeStandard;
        EFI_STATUS st = d->RunDiagnostics(d, t[i].ctrl, t[i].child, type, lang, &err, &bsize, &buf);
        free(lang);
        out_printf("  %s\n", EFI_ERROR(st) ? efi_strerror(st) : "passed");
        if (buf) {
            char *msg = ucs2_to_utf8(buf, bsize / 2);
            if (*msg)
                out_printf("    %s\n", msg);
            free(msg);
            gBS->FreePool(buf);
        }
        if (EFI_ERROR(st))
            rc = RC_FAIL;
    }
    free(t);
    snap_free(&s);
    return rc;
}

static int cmd_drvcfg(int argc, char **argv)
{
    Opts o;
    const char *force = NULL;
    /* -f TYPE takes a value: pull it out before the common parser */
    char **av = xmalloc(sizeof(char *) * (argc + 1));
    int ac = 0;
    for (int i = 0; i < argc; i++) {
        if (i > 0 && !strcasecmp(argv[i], "-f") && i + 1 < argc)
            force = argv[++i];
        else
            av[ac++] = argv[i];
    }
    av[ac] = NULL;
    bool ok = parse_opts("drvcfg", ac, av, "cvs", &o);
    free(av);
    if (!ok)
        return RC_USAGE;
    EFI_HANDLE h[3];
    if (!target_args("drvcfg", &o, h))
        return RC_FAIL;
    int64_t ftype = 0;
    if (force && (!parse_int(force, &ftype) || ftype < 0))
        return cmd_err("drvcfg", "-f needs a default type: 0 safe defaults, 1 manufacturing defaults");
    if ((force != NULL) + o.flag['v'] + o.flag['s'] > 1)
        return cmd_err("drvcfg", "choose only one of -f, -v, -s");
    Snap s;
    snap_build(&s);
    int n;
    Target *t = targets(&s, &cfg2_guid, h[0], h[1], h[2], o.flag['c'], &n);
    int rc = RC_OK;
    if (!n) {
        out_puts("No driver with a configuration protocol manages the selected devices.\n");
        /* modern drivers use HII forms instead: tell the user where they are */
        UINTN nh = 0;
        EFI_HANDLE *hh = NULL;
        if (gBS->LocateHandleBuffer(ByProtocol, &hiicfg_guid, NULL, &nh, &hh) == EFI_SUCCESS) {
            out_printf("%llu handles offer HII configuration forms (set up from the firmware setup menu):",
                       (unsigned long long)nh);
            for (UINTN i = 0; i < nh; i++)
                out_printf(" %X", efi_handle_index(hh[i]));
            out_puts("\n");
            gBS->FreePool(hh);
        }
    }
    static const char *actions[] = { "none", "stop the controller", "restart the controller", "restart the platform" };
    for (int i = 0; i < n && !con_break(); i++) {
        EFI_DRIVER_CONFIGURATION2_PROTOCOL *c = NULL;
        gBS->HandleProtocol(t[i].drv, &cfg2_guid, (void **)&c);
        print_target(&t[i]);
        EFI_STATUS st = EFI_SUCCESS;
        EFI_DRIVER_CONFIGURATION_ACTION_REQUIRED act = EfiDriverConfigurationActionNone;
        if (force) {
            st = c->ForceDefaults(c, t[i].ctrl, t[i].child, (UINT32)ftype, &act);
        } else if (o.flag['v']) {
            st = c->OptionsValid(c, t[i].ctrl, t[i].child);
        } else if (o.flag['s']) {
            char *lang = pick_lang(c->SupportedLanguages, false);
            st = c->SetOptions(c, t[i].ctrl, t[i].child, lang, &act);
            free(lang);
        } else {
            char *dn = driver_name(t[i].drv);
            out_printf("  %s\n", dn ? dn : "");
            free(dn);
            continue;
        }
        out_printf("  %s", EFI_ERROR(st) ? efi_strerror(st) : o.flag['v'] ? "options valid" : "done");
        if (!EFI_ERROR(st) && (unsigned)act < ARRAY_SIZE(actions) && act)
            out_printf(", action required: %s", actions[act]);
        out_puts("\n");
        if (EFI_ERROR(st))
            rc = RC_FAIL;
    }
    free(t);
    snap_free(&s);
    return rc;
}

static const Cmd drv_cmds[] = {
    { "drivers", cmd_drivers, "drivers [-l LANG]", "List the UEFI drivers",
      "  drivers           one line per driver\n"
      "  drivers -l fr     driver names in another language, if the driver has it\n"
      "Columns: DRV driver handle, VERSION driver version (hex), TYPE, CFG and DIAG\n"
      "(X: supports drvcfg / drvdiag), #D devices it manages, #C child devices it\n"
      "created, driver name (default language en) and the file it was loaded from.\n"
      "TYPE B is a bus driver: it created child devices (e.g. one per USB device);\n"
      "D is a device driver (a bus driver with no children yet also shows D).\n"
      "Handle numbers are hexadecimal; -b pages the output.\n"
      "With -data: handle, version, type, config, diag, controllers, children,\n"
      "name, image.\n", CMD_DATA },
    { "devices", cmd_devices, "devices [-l LANG]", "List the devices (controllers) and their drivers",
      "  devices           one line per device (controller)\n"
      "Columns: CTRL device handle, TYPE (R root: no parent, B bus: has children,\n"
      "D device), CFG and DIAG (X: one of its drivers supports drvcfg / drvdiag),\n"
      "#P parents, #D drivers managing it, #C children, device name.\n"
      "A device is a handle with a device path that is not an image or a driver.\n"
      "Names come from the drivers (in English) or else from the device path;\n"
      "-l is accepted and ignored; -b pages the output.\n"
      "With -data: handle, type, config, diag, parents, drivers, children, name.\n", CMD_DATA },
    { "devtree", cmd_devtree, "devtree [-d] [-l LANG] [HANDLE]", "Show the device tree (-d: device paths)",
      "  devtree           tree of all devices, from the root devices down\n"
      "  devtree HANDLE    only the device HANDLE and what is below it\n"
      "  devtree -d        show device paths instead of names\n"
      "Each line is \"Ctrl[HANDLE] name\"; children are indented under their parent.\n"
      "A device without a device path shows up to 4 of its protocols in <...>.\n"
      "-l is accepted and ignored; -b pages the output.\n"
      "Example:  devtree -d 3F    device paths of device 3F and its children\n" },
    { "dh", cmd_dh, "dh [-d|-v] [-p PROTOCOL] [HANDLE]", "Show handles and their protocols",
      "  dh              one line per handle: number and protocol names\n"
      "  dh HANDLE       details of one handle (device path, image, driver, disk...)\n"
      "  dh -p BlockIo   only handles with a protocol (name or GUID)\n"
      "  dh -d           details of all handles\n"
      "Handle numbers are hexadecimal and stay the same during the session.\n"
      "With -data: handle, name, protocols (and devicepath with -d or HANDLE).\n", CMD_DATA },
    { "openinfo", cmd_openinfo, "openinfo HANDLE", "Show who opened the protocols of a handle",
      "  openinfo HANDLE    each protocol of HANDLE and who has it open\n"
      "For each protocol, one line per user: Drv[agent handle] Ctrl[controller\n"
      "handle] Cnt(open count), how it is open, and the agent's image name.\n"
      "How: Driver (a driver manages the device), Exclusive, Driver+Exclusive,\n"
      "Child (used by a child device), HandProt / GetProt / TestProt (simple use).\n"
      "Handles are hexadecimal (see dh); -b pages the output.\n" },
    { "connect", cmd_connect, "connect [-r] [-c] [[DRIVER] CONTROLLER]",
      "Connect drivers to devices (no handle: all; -r recursive; -c consoles)",
      "  connect                    connect all drivers to all devices (recursive)\n"
      "  connect CONTROLLER         connect the best drivers to one device\n"
      "  connect DRIVER CONTROLLER  connect only DRIVER to that device\n"
      "  -r                         also connect the new child devices, recursively\n"
      "  -c                         connect the consoles (ConIn, ConOut, ErrOut)\n"
      "Connecting asks drivers to start managing a device. Bus drivers then create\n"
      "child devices (e.g. the partitions of a disk, which then get a file system).\n"
      "With -c and handles, the consoles are connected first, then the device.\n"
      "Exit code is nonzero if the firmware reports an error for the device.\n"
      "Example:  connect -r 3F    connect device 3F and everything below it\n" },
    { "disconnect", cmd_disconnect, "disconnect CONTROLLER [DRIVER [CHILD]] | disconnect -r",
      "Disconnect drivers from a device (-r: all devices, consoles reconnected)",
      "  disconnect CONTROLLER               stop all drivers managing a device\n"
      "  disconnect CONTROLLER DRIVER        stop only DRIVER on that device\n"
      "  disconnect CONTROLLER DRIVER CHILD  make DRIVER release one child device\n"
      "  disconnect -r                       disconnect all devices, then connect\n"
      "                                      the consoles again (screen, keyboard)\n"
      "When a bus driver stops, the child devices it created disappear too.\n"
      "DRIVER is a driver handle (see drivers); -r is ignored when handles are given.\n"
      "Exit code is nonzero if the firmware reports an error.\n" },
    { "reconnect", cmd_reconnect, "reconnect CONTROLLER [DRIVER [CHILD]] | reconnect -r",
      "Disconnect and connect again (-r: all devices)",
      "  reconnect CONTROLLER               disconnect all drivers, connect again\n"
      "  reconnect CONTROLLER DRIVER        the same, only for DRIVER\n"
      "  reconnect CONTROLLER DRIVER CHILD  release one child, then connect DRIVER\n"
      "  reconnect -r                       disconnect and connect all devices\n"
      "The connect step is recursive (child devices are connected too) and runs\n"
      "only if the disconnect succeeded; -r is ignored when handles are given.\n"
      "Exit code is nonzero if a step fails.\n" },
    { "load", cmd_load, "load [-nc] FILE...", "Load UEFI drivers (-nc: do not connect them to devices)",
      "  load FILE...      load and start drivers, then connect all devices\n"
      "  load -nc FILE...  load and start drivers, but do not connect them\n"
      "FILE may contain wildcards. Applications are refused (run them by typing\n"
      "their name), and so are drivers not allowed by Secure Boot.\n"
      "The connect step runs if at least one driver was loaded and started.\n"
      "Exit code is nonzero if any file fails.\n"
      "Example:  load fs0:\\drivers\\MyDxe.efi   load a driver and connect it\n" },
    { "unload", cmd_unload, "unload [-n] [-v] HANDLE", "Unload an image (-n: no question, -v: show details)",
      "  unload HANDLE     unload an image, after asking for confirmation\n"
      "  unload -n HANDLE  do not ask\n"
      "  unload -v HANDLE  show the handle details (like dh HANDLE) first\n"
      "HANDLE must be a loaded image (see dh -p LoadedImage); for most drivers it\n"
      "is the DRV number shown by drivers. -verbose is the same as -v.\n"
      "Many drivers cannot be unloaded: the result is then \"not supported\".\n"
      "Exit code is nonzero on error or if you do not answer y.\n" },
    { "drvdiag", cmd_drvdiag, "drvdiag [-c] [-l LANG] [-s|-e|-m] [DRIVER [CONTROLLER [CHILD]]]",
      "Driver diagnostics (-s standard, -e extended, -m manufacturing; none: list)",
      "  drvdiag           list driver/device pairs that offer diagnostics\n"
      "  -s                run the standard diagnostics\n"
      "  -e                run the extended diagnostics\n"
      "  -m                run the manufacturing diagnostics\n"
      "  -c                also include the child devices of each device\n"
      "  -l LANG           language of the messages (default en)\n"
      "Handles narrow the selection: DRIVER, then CONTROLLER, then CHILD.\n"
      "Each line shows Drv[..] Ctrl[..] (Child[..]) and the driver name, or the\n"
      "result (\"passed\" or the error) and the driver's message.\n"
      "Only drivers with the Driver Diagnostics 2 protocol are used.\n"
      "Exit code is nonzero if a test fails.\n"
      "Example:  drvdiag -s 7A    standard tests of driver 7A on all its devices\n" },
    { "drvcfg", cmd_drvcfg, "drvcfg [-c] [-l LANG] [-f TYPE | -v | -s] [DRIVER [CONTROLLER [CHILD]]]",
      "Driver configuration (-f defaults, -v validate, -s set options; none: list)",
      "  drvcfg            list driver/device pairs with a configuration protocol\n"
      "  -s                set options: the driver asks for its settings\n"
      "  -v                check that the current settings are valid\n"
      "  -f TYPE           restore defaults: 0 safe, 1 manufacturing (other\n"
      "                    numbers are passed to the driver)\n"
      "  -c                also include the child devices of each device\n"
      "  -l LANG           language used by -s (default en)\n"
      "Handles narrow the selection: DRIVER, then CONTROLLER, then CHILD.\n"
      "After -s or -f the driver may ask to stop or restart the controller or the\n"
      "platform: the line says so (\"action required\").\n"
      "Only the Driver Configuration 2 protocol is used. If none is found, the\n"
      "handles with HII setup forms are listed (use the firmware setup menu).\n"
      "Exit code is nonzero if an operation fails.\n" },
};

void efi_drivers_init(void)
{
    shell_register(drv_cmds, ARRAY_SIZE(drv_cmds));
}
