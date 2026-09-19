/* IPv6 network commands on top of the firmware IPv6 stack: ifconfig6, ping6.
 * tftp, http and ping accept IPv6 addresses too (see efi_net.c). The firmware
 * must provide the IPv6 drivers (IP6, UDP6, DHCP6, MTFTP6); on machines where
 * they are not loaded they can be loaded from files with "load". */
#include "efi_net.h"

static EFI_GUID ip6cfg_guid = { 0x937fe521, 0x95ae, 0x4d1a, { 0x89, 0x29, 0x48, 0xbc, 0xd9, 0x0a, 0xd3, 0x1a } };
static EFI_GUID ip6sb_guid = { 0xec835dd3, 0xfe0f, 0x617b, { 0xa6, 0x21, 0xb3, 0x50, 0xc3, 0xe1, 0x33, 0x88 } };
static EFI_GUID ip6_guid = { 0x2c8759d5, 0x5c2d, 0x66ef, { 0x92, 0x5f, 0xb6, 0x6c, 0x10, 0x19, 0x57, 0xe2 } };

/* ---- addresses ---- */

bool ip6_zero(const IPV6 *ip)
{
    for (int i = 0; i < 16; i++)
        if (ip->a[i])
            return false;
    return true;
}

static bool ip6_link_local(const IPV6 *ip)
{
    return ip->a[0] == 0xfe && (ip->a[1] & 0xc0) == 0x80;
}

bool ip6_parse(const char *s, IPV6 *ip)
{
    size_t len = strlen(s);
    if (len && s[0] == '[' && s[len - 1] == ']') {
        s++;
        len -= 2;
    }
    uint16_t head[8], tail[8];
    int nh = 0, nt = 0;
    bool gap = false;
    const char *p = s, *end = s + len;
    if (p + 1 < end && p[0] == ':' && p[1] == ':') {
        gap = true;
        p += 2;
    }
    while (p < end) {
        /* an IPv4 address in the last 32 bits: ::ffff:10.0.2.15 */
        const char *q = p;
        while (q < end && *q != ':' && *q != '.')
            q++;
        if (q < end && *q == '.') {
            int v[4], n = 0;
            for (const char *r = p; n < 4; n++) {
                char *e;
                long x = strtol(r, &e, 10);
                if (e == r || x < 0 || x > 255 || e > end)
                    return false;
                v[n] = (int)x;
                r = e;
                if (n < 3 && (r >= end || *r++ != '.'))
                    return false;
                if (n == 3 && r != end)
                    return false;
            }
            uint16_t *dst = gap ? tail : head;
            int *cnt = gap ? &nt : &nh;
            if (*cnt > 6)
                return false;
            dst[(*cnt)++] = (uint16_t)(v[0] << 8 | v[1]);
            dst[(*cnt)++] = (uint16_t)(v[2] << 8 | v[3]);
            p = end;
            break;
        }
        int digits = 0;
        uint32_t g = 0;
        while (p < end && isxdigit((uint8_t)*p) && digits < 5) {
            g = g * 16 + (uint32_t)(isdigit((uint8_t)*p) ? *p - '0' : (tolower((uint8_t)*p) - 'a' + 10));
            p++;
            digits++;
        }
        if (!digits || digits > 4)
            return false;
        if (gap) {
            if (nt >= 8)
                return false;
            tail[nt++] = (uint16_t)g;
        } else {
            if (nh >= 8)
                return false;
            head[nh++] = (uint16_t)g;
        }
        if (p == end)
            break;
        if (*p != ':')
            return false;
        p++;
        if (p < end && *p == ':') {
            if (gap)
                return false; /* only one "::" */
            gap = true;
            p++;
        } else if (p == end) {
            return false; /* trailing single ':' */
        }
    }
    if (gap ? nh + nt > 7 : nh != 8)
        return false;
    memset(ip, 0, sizeof(*ip));
    for (int i = 0; i < nh; i++)
        ip->a[2 * i] = (UINT8)(head[i] >> 8), ip->a[2 * i + 1] = (UINT8)head[i];
    for (int i = 0; i < nt; i++) {
        int k = 8 - nt + i;
        ip->a[2 * k] = (UINT8)(tail[i] >> 8), ip->a[2 * k + 1] = (UINT8)tail[i];
    }
    return true;
}

/* RFC 5952 text form: lowercase, the longest run of zero groups becomes "::" */
char *ip6_str(const IPV6 *ip)
{
    uint16_t g[8];
    for (int i = 0; i < 8; i++)
        g[i] = (uint16_t)(ip->a[2 * i] << 8 | ip->a[2 * i + 1]);
    int best = -1, bestlen = 1;
    for (int i = 0; i < 8;) {
        if (g[i]) {
            i++;
            continue;
        }
        int k = i;
        while (k < 8 && !g[k])
            k++;
        if (k - i > bestlen)
            best = i, bestlen = k - i;
        i = k;
    }
    Sbuf b;
    sb_init(&b);
    for (int i = 0; i < 8; i++) {
        if (i == best) {
            sb_adds(&b, "::");
            i += bestlen - 1;
            continue;
        }
        if (i && i != best + bestlen)
            sb_putc(&b, ':');
        sb_printf(&b, "%x", g[i]);
    }
    return sb_steal(&b);
}

/* ---- configuration (EFI_IP6_CONFIG_PROTOCOL) ---- */

enum { C6_INTERFACE_INFO = 0, C6_ALT_ID = 1, C6_POLICY = 2, C6_DAD = 3, C6_MANUAL_ADDRESS = 4, C6_GATEWAY = 5,
       C6_DNS = 6 };
enum { P6_MANUAL = 0, P6_AUTOMATIC = 1 };

typedef struct {
    EFI_STATUS(EFIAPI *SetData)(void *This, UINT32 Type, UINTN Size, void *Data);
    EFI_STATUS(EFIAPI *GetData)(void *This, UINT32 Type, UINTN *Size, void *Data);
    void *RegisterDataNotify, *UnregisterDataNotify;
} IP6_CONFIG;

typedef struct {
    IPV6 Address;
    UINT8 PrefixLength;
} ADDR_INFO6;

typedef struct {
    IPV6 Gateway, Destination;
    UINT8 PrefixLength;
} ROUTE6;

typedef struct {
    CHAR16 Name[32];
    UINT8 IfType;
    UINT32 HwAddressSize;
    UINT8 HwAddress[32];
    UINT32 AddressInfoCount;
    ADDR_INFO6 *AddressInfo;
    UINT32 RouteCount;
    ROUTE6 *RouteTable;
} IF_INFO6;

typedef struct {
    IPV6 Address;
    BOOLEAN IsAnycast;
    UINT8 PrefixLength;
} MANUAL6;

typedef struct {
    EFI_HANDLE h;
    IP6_CONFIG *cfg;
    char name[32];
} NetIf6;

static IF_INFO6 *if6_info(NetIf6 *nif)
{
    UINTN size = 0;
    if (nif->cfg->GetData(nif->cfg, C6_INTERFACE_INFO, &size, NULL) != EFI_BUFFER_TOO_SMALL || !size)
        return NULL;
    IF_INFO6 *info = xcalloc(1, size);
    if (nif->cfg->GetData(nif->cfg, C6_INTERFACE_INFO, &size, info) != EFI_SUCCESS) {
        free(info);
        return NULL;
    }
    return info;
}

static NetIf6 *interfaces6(int *count)
{
    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    *count = 0;
    if (gBS->LocateHandleBuffer(ByProtocol, &ip6cfg_guid, NULL, &n, &hs) != EFI_SUCCESS)
        return NULL;
    NetIf6 *r = xcalloc(n, sizeof(NetIf6));
    for (UINTN i = 0; i < n; i++) {
        NetIf6 *x = &r[*count];
        x->h = hs[i];
        if (gBS->HandleProtocol(hs[i], &ip6cfg_guid, (void **)&x->cfg) != EFI_SUCCESS)
            continue;
        IF_INFO6 *info = if6_info(x);
        char *nm = info ? ucs2_to_utf8(info->Name, (size_t)-1) : xasprintf("eth%d", *count);
        snprintf(x->name, sizeof(x->name), "%s", nm);
        free(nm);
        free(info);
        (*count)++;
    }
    gBS->FreePool(hs);
    return r;
}

static const char no_stack6_msg[] =
    "no IPv6 network interface: the firmware IPv6 stack is not loaded.\n"
    "  Enable IPv6 network boot in the firmware setup, or load the drivers from files:\n"
    "  load MnpDxe.efi Ip6Dxe.efi Udp6Dxe.efi Dhcp6Dxe.efi Mtftp6Dxe.efi TcpDxe.efi\n"
    "       (they need a random number generator)";

static bool find_if6(const char *cmd, const char *name, NetIf6 *out)
{
    int n;
    NetIf6 *all = interfaces6(&n);
    bool ok = false;
    for (int i = 0; i < n && !ok; i++) {
        if (!name || !strcasecmp(all[i].name, name)) {
            *out = all[i];
            ok = true;
        }
    }
    free(all);
    if (!ok) {
        if (!n)
            err_printf("%s: %s\n", cmd, no_stack6_msg);
        else
            err_printf("%s: no interface named %s (see ifconfig6 -l)\n", cmd, name);
    }
    return ok;
}

/* An address of the interface that can reach dst (NULL: any address). */
static bool pick_source(NetIf6 *nif, const IPV6 *dst, IPV6 *src)
{
    IF_INFO6 *info = if6_info(nif);
    bool ok = false;
    bool want_ll = dst && ip6_link_local(dst);
    for (UINT32 i = 0; info && i < info->AddressInfoCount && !ok; i++) {
        const IPV6 *a = &info->AddressInfo[i].Address;
        if (!dst || ip6_link_local(a) == want_ll) {
            *src = *a;
            ok = true;
        }
    }
    free(info);
    return ok;
}

bool net6_prepare(const char *cmd, const char *ifname, const IPV6 *dst, EFI_HANDLE *nic, IPV6 *src)
{
    NetIf6 nif;
    if (!find_if6(cmd, ifname, &nif))
        return false;
    /* addresses come from duplicate detection and router advertisements: wait a bit */
    uint64_t end = pal_ticks_ms() + 10000;
    while (!pick_source(&nif, dst, src)) {
        if (pal_ticks_ms() >= end || con_break()) {
            char *d = ip6_str(dst);
            err_printf("%s: %s has no IPv6 address that can reach %s (ifconfig6 -s %s auto)\n", cmd, nif.name, d,
                       nif.name);
            free(d);
            return false;
        }
        gBS->Stall(50000);
    }
    *nic = nif.h;
    return true;
}

static void addr_list6(NetIf6 *nif, UINT32 type, Sbuf *b)
{
    IPV6 v[8];
    UINTN sz = sizeof(v);
    if (nif->cfg->GetData(nif->cfg, type, &sz, v) != EFI_SUCCESS)
        return;
    for (UINTN i = 0; i < sz / sizeof(IPV6); i++) {
        char *t = ip6_str(&v[i]);
        sb_printf(b, "%s%s", b->len ? " " : "", t);
        free(t);
    }
}

/* Gateways: the configured ones, or the default routes learnt from the routers. */
static void gateways6(NetIf6 *nif, const IF_INFO6 *info, Sbuf *b)
{
    addr_list6(nif, C6_GATEWAY, b);
    for (UINT32 i = 0; !b->len && i < info->RouteCount; i++) {
        const ROUTE6 *r = &info->RouteTable[i];
        if (!r->PrefixLength && ip6_zero(&r->Destination) && !ip6_zero(&r->Gateway)) {
            char *g = ip6_str(&r->Gateway);
            sb_adds(b, g);
            free(g);
        }
    }
}

static UINT32 policy6(NetIf6 *nif)
{
    UINTN sz = sizeof(UINT32);
    UINT32 p = P6_AUTOMATIC;
    nif->cfg->GetData(nif->cfg, C6_POLICY, &sz, &p);
    return p;
}

static UINT32 dad6(NetIf6 *nif)
{
    UINTN sz = sizeof(UINT32);
    UINT32 d = 0;
    nif->cfg->GetData(nif->cfg, C6_DAD, &sz, &d);
    return d;
}

static void mac_str(const IF_INFO6 *info, Sbuf *b)
{
    for (UINT32 i = 0; i < info->HwAddressSize && i < 32; i++)
        sb_printf(b, "%s%02X", i ? "-" : "", info->HwAddress[i]);
}

static void if6_data(NetIf6 *nif)
{
    IF_INFO6 *info = if6_info(nif);
    if (!info)
        return;
    data_record();
    data_field("name", "%s", nif->name);
    data_field("media", "%s", net_media_state(nif->h));
    data_field("policy", "%s", policy6(nif) == P6_AUTOMATIC ? "automatic" : "manual");
    Sbuf b;
    sb_init(&b);
    mac_str(info, &b);
    data_field("mac", "%s", b.s ? b.s : "");
    b.len = 0;
    for (UINT32 i = 0; i < info->AddressInfoCount; i++) {
        char *a = ip6_str(&info->AddressInfo[i].Address);
        sb_printf(&b, "%s%s/%u", b.len ? " " : "", a, info->AddressInfo[i].PrefixLength);
        free(a);
    }
    data_field("ip", "%s", b.len ? b.s : "");
    b.len = 0;
    gateways6(nif, info, &b);
    data_field("gateway", "%s", b.len ? b.s : "");
    b.len = 0;
    addr_list6(nif, C6_DNS, &b);
    data_field("dns", "%s", b.len ? b.s : "");
    data_field("dad", "%u", dad6(nif));
    sb_free(&b);
    free(info);
}

static void if6_show(NetIf6 *nif)
{
    IF_INFO6 *info = if6_info(nif);
    if (!info)
        return;
    out_printf("name         : %s\n", nif->name);
    out_printf("media state  : %s\n", net_media_state(nif->h));
    out_printf("policy       : %s\n", policy6(nif) == P6_AUTOMATIC ? "automatic" : "manual");
    out_printf("dad transmits: %u\n", dad6(nif));
    Sbuf b;
    sb_init(&b);
    mac_str(info, &b);
    out_printf("mac addr     : %s\n", b.s ? b.s : "");
    if (!info->AddressInfoCount)
        out_puts("ipv6 address : none\n");
    for (UINT32 i = 0; i < info->AddressInfoCount; i++) {
        char *a = ip6_str(&info->AddressInfo[i].Address);
        out_printf("%s%s/%u%s\n", i ? "               " : "ipv6 address : ", a, info->AddressInfo[i].PrefixLength,
                   ip6_link_local(&info->AddressInfo[i].Address) ? "  (link-local)" : "");
        free(a);
    }
    b.len = 0;
    gateways6(nif, info, &b);
    if (b.len)
        out_printf("gateway      : %s\n", b.s);
    b.len = 0;
    addr_list6(nif, C6_DNS, &b);
    if (b.len)
        out_printf("dns server   : %s\n", b.s);
    sb_free(&b);
    if (info->RouteCount) {
        out_puts("route table  :\n");
        for (UINT32 i = 0; i < info->RouteCount; i++) {
            char *d = ip6_str(&info->RouteTable[i].Destination), *g = ip6_str(&info->RouteTable[i].Gateway);
            out_printf("  %s/%u gateway %s\n", d, info->RouteTable[i].PrefixLength, g);
            free(d);
            free(g);
        }
    }
    out_puts("\n");
    free(info);
}

/* "ADDR" or "ADDR/LEN" (LEN 64 by default) */
static bool parse_host(const char *s, MANUAL6 *m)
{
    char *t = xstrdup(s);
    char *slash = strchr(t, '/');
    int64_t len = 64;
    bool ok = true;
    if (slash) {
        *slash = 0;
        ok = parse_int(slash + 1, &len) && len >= 1 && len <= 128;
    }
    memset(m, 0, sizeof(*m));
    ok = ok && ip6_parse(t, &m->Address);
    m->PrefixLength = (UINT8)len;
    free(t);
    return ok;
}

static EFI_STATUS set_policy6(NetIf6 *nif, UINT32 p)
{
    return nif->cfg->SetData(nif->cfg, C6_POLICY, sizeof(p), &p);
}

/* ifconfig6 -s NAME man [host ADDR[/LEN]...] [gw ADDR...] [dns ADDR...] */
static int set_manual6(NetIf6 *nif, int argc, char **argv)
{
    MANUAL6 hosts[8];
    IPV6 gws[8], dnss[8];
    int nh = 0, ng = 0, nd = 0;
    int what = 0; /* 1 host, 2 gw, 3 dns */
    for (int i = 0; i < argc; i++) {
        if (!strcasecmp(argv[i], "host"))
            what = 1;
        else if (!strcasecmp(argv[i], "gw"))
            what = 2;
        else if (!strcasecmp(argv[i], "dns"))
            what = 3;
        else if (what == 1 && nh < 8 && parse_host(argv[i], &hosts[nh]))
            nh++;
        else if (what == 2 && ng < 8 && ip6_parse(argv[i], &gws[ng]))
            ng++;
        else if (what == 3 && nd < 8 && ip6_parse(argv[i], &dnss[nd]))
            nd++;
        else
            return cmd_err("ifconfig6", "%s: not a valid IPv6 %s", argv[i],
                           what == 1 ? "address (ADDR or ADDR/LEN)" : what ? "address" : "setting (host, gw or dns)");
    }
    /* switching to manual clears the automatic settings; the policy must be manual
       for the other settings to be accepted */
    EFI_STATUS st = policy6(nif) == P6_MANUAL ? EFI_SUCCESS : set_policy6(nif, P6_MANUAL);
    if (!EFI_ERROR(st) && nh) {
        st = nif->cfg->SetData(nif->cfg, C6_MANUAL_ADDRESS, sizeof(MANUAL6) * (UINTN)nh, hosts);
        if (st == EFI_NOT_READY) { /* duplicate address detection running: wait for it */
            uint64_t end = pal_ticks_ms() + 5000;
            IPV6 any;
            while (!pick_source(nif, &hosts[0].Address, &any) && pal_ticks_ms() < end && !con_break())
                gBS->Stall(50000);
            st = EFI_SUCCESS;
        }
    }
    if (!EFI_ERROR(st) && ng)
        st = nif->cfg->SetData(nif->cfg, C6_GATEWAY, sizeof(IPV6) * (UINTN)ng, gws);
    if (!EFI_ERROR(st) && nd)
        st = nif->cfg->SetData(nif->cfg, C6_DNS, sizeof(IPV6) * (UINTN)nd, dnss);
    return EFI_ERROR(st) ? cmd_err("ifconfig6", "%s: %s", nif->name, efi_strerror(st)) : RC_OK;
}

static int cmd_ifconfig6(int argc, char **argv)
{
    if (argc == 1 || !strcasecmp(argv[1], "-l")) {
        int n;
        NetIf6 *all = interfaces6(&n);
        if (!n) {
            free(all);
            return cmd_err("ifconfig6", "%s", no_stack6_msg);
        }
        int shown = 0;
        for (int i = 0; i < n; i++) {
            if (argc > 2 && strcasecmp(all[i].name, argv[2]))
                continue;
            shown++;
            if (out_data_mode())
                if6_data(&all[i]);
            else
                if6_show(&all[i]);
        }
        free(all);
        return shown ? RC_OK : cmd_err("ifconfig6", "no interface named %s (see ifconfig6 -l)", argv[2]);
    }
    if (!strcasecmp(argv[1], "-r")) {
        int n;
        NetIf6 *all = interfaces6(&n);
        int rc = n ? RC_OK : cmd_err("ifconfig6", "%s", no_stack6_msg);
        for (int i = 0; i < n; i++) {
            if (argc > 2 && strcasecmp(all[i].name, argv[2]))
                continue;
            /* back to automatic: switching the policy clears the manual settings */
            set_policy6(&all[i], P6_MANUAL);
            EFI_STATUS st = set_policy6(&all[i], P6_AUTOMATIC);
            if (EFI_ERROR(st))
                rc = cmd_err("ifconfig6", "%s: %s", all[i].name, efi_strerror(st));
        }
        free(all);
        return rc;
    }
    if (strcasecmp(argv[1], "-s") || argc < 4)
        return cmd_usage("ifconfig6");
    NetIf6 nif;
    if (!find_if6("ifconfig6", argv[2], &nif))
        return RC_FAIL;
    const char *mode = argv[3];
    if (!strcasecmp(mode, "auto")) {
        if (argc != 4)
            return cmd_usage("ifconfig6");
        EFI_STATUS st = set_policy6(&nif, P6_AUTOMATIC);
        return EFI_ERROR(st) ? cmd_err("ifconfig6", "%s", efi_strerror(st)) : RC_OK;
    }
    if (!strcasecmp(mode, "man"))
        return set_manual6(&nif, argc - 4, argv + 4);
    if (!strcasecmp(mode, "dad")) {
        int64_t v;
        if (argc != 5 || !parse_int(argv[4], &v) || v < 0 || v > 10)
            return cmd_err("ifconfig6", "usage: ifconfig6 -s NAME dad COUNT (0-10)");
        UINT32 d = (UINT32)v;
        EFI_STATUS st = nif.cfg->SetData(nif.cfg, C6_DAD, sizeof(d), &d);
        return EFI_ERROR(st) ? cmd_err("ifconfig6", "%s", efi_strerror(st)) : RC_OK;
    }
    return cmd_usage("ifconfig6");
}

/* ---- ping6 (ICMPv6 echo over EFI_IP6_PROTOCOL) ---- */

typedef struct {
    UINT8 DefaultProtocol;
    BOOLEAN AcceptAnyProtocol, AcceptIcmpErrors, AcceptPromiscuous;
    IPV6 DestinationAddress, StationAddress;
    UINT8 TrafficClass, HopLimit;
    UINT32 FlowLabel, ReceiveTimeout, TransmitTimeout;
} IP6_CONFIG_DATA;

typedef struct {
    IPV6 DestinationAddress;
    void *OverrideData;
    UINT32 ExtHdrsLength;
    void *ExtHdrs;
    UINT8 NextHeader;
    UINT32 DataLength;
    UINT32 FragmentCount;
    FRAGMENT FragmentTable[1];
} IP6_TX;

typedef struct {
    EFI_TIME TimeStamp;
    EFI_EVENT RecycleSignal;
    UINT32 HeaderLength;
    UINT8 *Header;
    UINT32 DataLength;
    UINT32 FragmentCount;
    FRAGMENT FragmentTable[1];
} IP6_RX;

typedef struct {
    EFI_EVENT Event;
    EFI_STATUS Status;
    union {
        IP6_RX *RxData;
        IP6_TX *TxData;
    } Packet;
} IP6_TOKEN;

typedef struct {
    void *GetModeData;
    EFI_STATUS(EFIAPI *Configure)(void *This, IP6_CONFIG_DATA *Config);
    void *Groups, *Routes, *Neighbors;
    EFI_STATUS(EFIAPI *Transmit)(void *This, IP6_TOKEN *Token);
    EFI_STATUS(EFIAPI *Receive)(void *This, IP6_TOKEN *Token);
    EFI_STATUS(EFIAPI *Cancel)(void *This, IP6_TOKEN *Token);
    EFI_STATUS(EFIAPI *Poll)(void *This);
} IP6;

#define ICMP6 58

/* ICMPv6 checksum: covers a pseudo header with both addresses */
static uint16_t icmp6_csum(const IPV6 *src, const IPV6 *dst, const uint8_t *d, size_t n)
{
    uint32_t s = 0;
    for (int i = 0; i < 16; i += 2)
        s += (uint32_t)(src->a[i] << 8 | src->a[i + 1]) + (uint32_t)(dst->a[i] << 8 | dst->a[i + 1]);
    s += (uint32_t)(n >> 16) + (uint32_t)(n & 0xFFFF) + ICMP6;
    for (size_t i = 0; i + 1 < n; i += 2)
        s += (uint32_t)(d[i] << 8 | d[i + 1]);
    if (n & 1)
        s += (uint32_t)d[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

int ping6_run(const char *cmd, const char *target, int64_t count, int64_t size, const char *ifname, const char *source)
{
    IPV6 dst, src;
    if (!ip6_parse(target, &dst))
        return cmd_err(cmd, "%s is not an IPv6 address", target);
    EFI_HANDLE nic;
    if (!net6_prepare(cmd, ifname, &dst, &nic, &src))
        return RC_FAIL;
    if (source && !ip6_parse(source, &src))
        return cmd_err(cmd, "%s is not an IPv6 address", source);
    SERVICE_BINDING *sb;
    if (gBS->HandleProtocol(nic, &ip6sb_guid, (void **)&sb) != EFI_SUCCESS)
        return cmd_err(cmd, "no IP6 service on this interface");
    EFI_HANDLE child = NULL;
    IP6 *ip6;
    EFI_STATUS st = sb->CreateChild(sb, &child);
    if (EFI_ERROR(st) || gBS->HandleProtocol(child, &ip6_guid, (void **)&ip6) != EFI_SUCCESS)
        return cmd_err(cmd, "cannot create an IP6 instance: %s", efi_strerror(st));
    IP6_CONFIG_DATA cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.DefaultProtocol = ICMP6;
    cfg.StationAddress = src;
    cfg.HopLimit = 64;
    st = ip6->Configure(ip6, &cfg);
    int rc = RC_OK;
    if (EFI_ERROR(st)) {
        rc = cmd_err(cmd, "cannot configure IP6: %s", efi_strerror(st));
        goto out;
    }
    char *srcs = ip6_str(&src);
    out_printf("Ping %s %lld data bytes (from %s).\n", target, (long long)size, srcs);
    free(srcs);
    int sent = 0, received = 0;
    uint64_t rtt_min = UINT64_MAX, rtt_max = 0, rtt_sum = 0;
    uint16_t ident = (uint16_t)(pal_ticks_ms() & 0xFFFF);
    IP6_TOKEN rx;
    memset(&rx, 0, sizeof(rx));
    gBS->CreateEvent(0, 0, NULL, NULL, &rx.Event);
    ip6->Receive(ip6, &rx);
    for (int64_t seq = 1; seq <= count && !con_break(); seq++) {
        size_t plen = 8 + (size_t)size;
        uint8_t *pkt = xcalloc(1, plen);
        pkt[0] = 128; /* echo request */
        pkt[4] = (uint8_t)(ident >> 8), pkt[5] = (uint8_t)ident;
        pkt[6] = (uint8_t)(seq >> 8), pkt[7] = (uint8_t)seq;
        for (size_t k = 8; k < plen; k++)
            pkt[k] = (uint8_t)k;
        uint16_t c = icmp6_csum(&src, &dst, pkt, plen);
        pkt[2] = (uint8_t)(c >> 8), pkt[3] = (uint8_t)c;
        IP6_TX tx;
        memset(&tx, 0, sizeof(tx));
        tx.DestinationAddress = dst;
        tx.NextHeader = ICMP6;
        tx.DataLength = (UINT32)plen;
        tx.FragmentCount = 1;
        tx.FragmentTable[0].FragmentLength = (UINT32)plen;
        tx.FragmentTable[0].FragmentBuffer = pkt;
        IP6_TOKEN txt;
        memset(&txt, 0, sizeof(txt));
        gBS->CreateEvent(0, 0, NULL, NULL, &txt.Event);
        txt.Packet.TxData = &tx;
        uint64_t t0 = pal_ticks_ms();
        st = ip6->Transmit(ip6, &txt);
        if (!EFI_ERROR(st)) {
            sent++;
            /* the first packet may wait for neighbor discovery */
            while (gBS->CheckEvent(txt.Event) != EFI_SUCCESS && pal_ticks_ms() - t0 < 3000)
                ip6->Poll(ip6);
        } else {
            err_printf("%s: send failed: %s\n", cmd, efi_strerror(st));
        }
        bool got = false;
        while (!EFI_ERROR(st) && !got && pal_ticks_ms() - t0 < 3000 && !con_break()) {
            ip6->Poll(ip6);
            if (gBS->CheckEvent(rx.Event) != EFI_SUCCESS)
                continue;
            IP6_RX *r = rx.Packet.RxData;
            if (rx.Status == EFI_SUCCESS && r && r->FragmentCount && r->DataLength >= 8 &&
                r->FragmentTable[0].FragmentLength >= 8) {
                const uint8_t *p = r->FragmentTable[0].FragmentBuffer;
                uint16_t rid = (uint16_t)(p[4] << 8 | p[5]), rseq = (uint16_t)(p[6] << 8 | p[7]);
                if (p[0] == 129 && rid == ident && rseq == (uint16_t)seq) {
                    uint64_t rtt = pal_ticks_ms() - t0;
                    uint8_t hops = r->Header && r->HeaderLength >= 8 ? r->Header[7] : 0;
                    out_printf("%u bytes from %s : icmp_seq=%lld hop_limit=%u time=%llums\n", r->DataLength - 8,
                               target, (long long)seq, hops, (unsigned long long)rtt);
                    received++;
                    rtt_sum += rtt;
                    rtt_min = MIN(rtt_min, rtt);
                    rtt_max = MAX(rtt_max, rtt);
                    got = true;
                }
            }
            if (r && r->RecycleSignal)
                gBS->SignalEvent(r->RecycleSignal);
            rx.Packet.RxData = NULL;
            ip6->Receive(ip6, &rx);
        }
        if (!got && !EFI_ERROR(st))
            out_printf("Echo request sequence %lld timeout.\n", (long long)seq);
        gBS->CloseEvent(txt.Event);
        free(pkt);
        while (pal_ticks_ms() - t0 < 1000 && seq < count && !con_break())
            ip6->Poll(ip6);
    }
    ip6->Cancel(ip6, NULL);
    gBS->CloseEvent(rx.Event);
    out_printf("\n%d packets transmitted, %d received, %d%% packet loss\n", sent, received,
               sent ? (sent - received) * 100 / sent : 0);
    if (received)
        out_printf("Rtt(round trip time) min=%llums max=%llums avg=%llums\n", (unsigned long long)rtt_min,
                   (unsigned long long)rtt_max, (unsigned long long)(rtt_sum / (uint64_t)received));
    rc = received ? RC_OK : RC_FAIL;
out:
    ip6->Configure(ip6, NULL);
    sb->DestroyChild(sb, child);
    return rc;
}

static int cmd_ping6(int argc, char **argv)
{
    int64_t count = 10, size = 16;
    const char *target = NULL, *ifname = NULL, *source = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "-n") && i + 1 < argc) {
            if (!parse_int(argv[++i], &count) || count < 1 || count > 10000)
                return cmd_usage("ping6");
        } else if (!strcasecmp(argv[i], "-l") && i + 1 < argc) {
            if (!parse_int(argv[++i], &size) || size < 0 || size > 1400)
                return cmd_usage("ping6");
        } else if (!strcasecmp(argv[i], "-i") && i + 1 < argc) {
            ifname = argv[++i];
        } else if (!strcasecmp(argv[i], "-s") && i + 1 < argc) {
            source = argv[++i];
        } else if (argv[i][0] != '-' && !target) {
            target = argv[i];
        } else {
            return cmd_usage("ping6");
        }
    }
    if (!target)
        return cmd_usage("ping6");
    return ping6_run("ping6", target, count, size, ifname, source);
}

static const Cmd net6_cmds[] = {
    { "ifconfig6", cmd_ifconfig6, "ifconfig6 [-l [NAME]] | -r [NAME] | -s NAME auto | -s NAME man [host ADDR[/LEN]...] [gw ADDR...] [dns ADDR...] | -s NAME dad COUNT",
      "Show or set the IPv6 configuration of the network interfaces",
      "  ifconfig6 -l [NAME]         list the interfaces and their IPv6 settings\n"
      "  ifconfig6 -s eth0 auto      automatic configuration (router advertisements)\n"
      "  ifconfig6 -s eth0 man host 2001:db8::10/64 gw 2001:db8::1 dns 2001:db8::53\n"
      "                              manual configuration (prefix length 64 if\n"
      "                              omitted)\n"
      "  ifconfig6 -s eth0 dad 1     duplicate address detection messages per address\n"
      "  ifconfig6 -r [NAME]         reset to automatic configuration\n"
      "A link-local address (fe80::...) is always present. ping, tftp and http\n"
      "accept IPv6 addresses; in URLs they go in brackets: http://[2001:db8::1]/f.\n"
      "The firmware IPv6 drivers must be loaded (IPv6 network boot enabled in the\n"
      "setup, or: load MnpDxe.efi Ip6Dxe.efi Udp6Dxe.efi Dhcp6Dxe.efi\n"
      "Mtftp6Dxe.efi TcpDxe.efi).\n"
      "With -data (ifconfig6 -l -data): name, media, policy, mac, ip (ADDR/LEN\n"
      "list), gateway, dns, dad.\n", CMD_DATA },
    { "ping6", cmd_ping6, "ping6 [-n COUNT] [-l SIZE] [-s SOURCE] [-i IF] ADDRESS",
      "Send echo requests to an IPv6 address (ping accepts IPv6 too)",
      "  -n COUNT    number of requests (default 10)\n"
      "  -l SIZE     data bytes per request (0-1400, default 16)\n"
      "  -s SOURCE   source address (default: one that can reach ADDRESS)\n"
      "  -i IF       interface name (default: the first one)\n"
      "  ping6 -n 3 fe80::1       link-local address: the link-local source is used\n"
      "Ends with an error (ERR <> 0) when no answer arrives. Without an address\n"
      "other than link-local, run 'ifconfig6 -s eth0 auto' first.\n" },
};

void efi_net6_init(void)
{
    shell_register(net6_cmds, ARRAY_SIZE(net6_cmds));
}
