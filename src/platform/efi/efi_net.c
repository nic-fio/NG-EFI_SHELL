/* Network commands on top of the firmware IPv4 stack: ifconfig, ping, tftp, http.
 * ping, tftp and http switch to the IPv6 stack (efi_net6.c) for IPv6 addresses.
 * The firmware must provide the network drivers (MNP, IP4, UDP4, DHCP4, MTFTP4,
 * TCP4, HTTP); on machines where they are not loaded they can be loaded from
 * files with "load" (see "help ifconfig"). */
#include "efi_net.h"

typedef struct {
    UINT8 a[4];
} IPV4;

static EFI_GUID ip4cfg2_guid = { 0x5b446ed1, 0xe30b, 0x4faa, { 0x87, 0x1a, 0x36, 0x54, 0xec, 0xa3, 0x60, 0x80 } };
static EFI_GUID ip4sb_guid = { 0xc51711e7, 0xb4bf, 0x404a, { 0xbf, 0xb8, 0x0a, 0x04, 0x8e, 0xf1, 0xff, 0xe4 } };
static EFI_GUID ip4_guid = { 0x41d94cd2, 0x35b6, 0x455a, { 0x82, 0x58, 0xd4, 0xe5, 0x13, 0x34, 0xaa, 0xdd } };
static EFI_GUID mtftp4sb_guid = { 0x2fe800be, 0x8f01, 0x4aa6, { 0x94, 0x6b, 0xd7, 0x13, 0x88, 0xe1, 0x83, 0x3f } };
static EFI_GUID mtftp4_guid = { 0x78247c57, 0x63db, 0x4708, { 0x99, 0xc2, 0xa8, 0xb4, 0xa9, 0xa6, 0x1f, 0x6b } };
static EFI_GUID mtftp6sb_guid = { 0xd9760ff3, 0x3cca, 0x4267, { 0x80, 0xf9, 0x75, 0x27, 0xfa, 0xfa, 0x42, 0x23 } };
static EFI_GUID mtftp6_guid = { 0xbf0a78ba, 0xec29, 0x49cf, { 0xa1, 0xc9, 0x7a, 0xe5, 0x4e, 0xab, 0x6a, 0x51 } };
static EFI_GUID httpsb_guid = { 0xbdc8e6af, 0xd9bc, 0x4379, { 0xa7, 0x2a, 0xe0, 0xc4, 0xe7, 0x5d, 0xae, 0x1c } };
static EFI_GUID http_guid = { 0x7a59b29b, 0x910b, 0x4171, { 0x82, 0x42, 0xa8, 0x5a, 0x0d, 0xf2, 0x5b, 0x5b } };
static EFI_GUID snp_guid = { 0xA19832B9, 0xAC25, 0x11D3, { 0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

/* ---- IP4 configuration (EFI_IP4_CONFIG2_PROTOCOL) ---- */

/* EFI_IP4_CONFIG2_DATA_TYPE (the IPv6 list is different) */
enum { CFG_INTERFACE_INFO = 0, CFG_POLICY = 1, CFG_MANUAL_ADDRESS = 2, CFG_GATEWAY = 3, CFG_DNS = 4 };
enum { POLICY_STATIC = 0, POLICY_DHCP = 1 };

typedef struct {
    EFI_STATUS(EFIAPI *SetData)(void *This, UINT32 Type, UINTN Size, void *Data);
    EFI_STATUS(EFIAPI *GetData)(void *This, UINT32 Type, UINTN *Size, void *Data);
    void *RegisterDataNotify, *UnregisterDataNotify;
} IP4_CONFIG2;

typedef struct {
    IPV4 Subnet, Mask, Gateway;
} ROUTE;

typedef struct {
    CHAR16 Name[32];
    UINT8 IfType;
    UINT32 HwAddressSize;
    UINT8 HwAddress[32];
    IPV4 StationAddress;
    IPV4 SubnetMask;
    UINT32 RouteTableSize;
    ROUTE *RouteTable;
} IF_INFO;

typedef struct {
    EFI_HANDLE h;
    IP4_CONFIG2 *cfg;
    char name[32];
} NetIf;

static bool ip_parse(const char *s, IPV4 *ip)
{
    int v[4];
    int n = 0;
    const char *p = s;
    for (; n < 4; n++) {
        char *end;
        long x = strtol(p, &end, 10);
        if (end == p || x < 0 || x > 255)
            return false;
        v[n] = (int)x;
        p = end;
        if (n < 3) {
            if (*p != '.')
                return false;
            p++;
        }
    }
    if (*p)
        return false;
    for (int i = 0; i < 4; i++)
        ip->a[i] = (UINT8)v[i];
    return true;
}

static char *ip_str(const IPV4 *ip)
{
    return xasprintf("%u.%u.%u.%u", ip->a[0], ip->a[1], ip->a[2], ip->a[3]);
}

static bool ip_zero(const IPV4 *ip)
{
    return !(ip->a[0] | ip->a[1] | ip->a[2] | ip->a[3]);
}

static IF_INFO *if_info(NetIf *nif)
{
    UINTN size = 0;
    EFI_STATUS st = nif->cfg->GetData(nif->cfg, CFG_INTERFACE_INFO, &size, NULL);
    if (st != EFI_BUFFER_TOO_SMALL || !size)
        return NULL;
    IF_INFO *info = xcalloc(1, size);
    if (nif->cfg->GetData(nif->cfg, CFG_INTERFACE_INFO, &size, info) != EFI_SUCCESS) {
        free(info);
        return NULL;
    }
    return info;
}

static NetIf *interfaces(int *count)
{
    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    *count = 0;
    if (gBS->LocateHandleBuffer(ByProtocol, &ip4cfg2_guid, NULL, &n, &hs) != EFI_SUCCESS)
        return NULL;
    NetIf *r = xcalloc(n, sizeof(NetIf));
    for (UINTN i = 0; i < n; i++) {
        NetIf *x = &r[*count];
        x->h = hs[i];
        if (gBS->HandleProtocol(hs[i], &ip4cfg2_guid, (void **)&x->cfg) != EFI_SUCCESS)
            continue;
        IF_INFO *info = if_info(x);
        char *nm = info ? ucs2_to_utf8(info->Name, (size_t)-1) : xasprintf("eth%d", *count);
        snprintf(x->name, sizeof(x->name), "%s", nm);
        free(nm);
        free(info);
        (*count)++;
    }
    gBS->FreePool(hs);
    return r;
}

static const char no_stack_msg[] =
    "no network interface: the firmware network stack is not loaded.\n"
    "  Enable network boot in the firmware setup, or load the drivers from files:\n"
    "  load MnpDxe.efi ArpDxe.efi Ip4Dxe.efi Udp4Dxe.efi Dhcp4Dxe.efi Mtftp4Dxe.efi TcpDxe.efi\n"
    "       DnsDxe.efi HttpUtilitiesDxe.efi HttpDxe.efi   (they need a random number generator)";

/* Interface by name (NULL: the first one). */
static bool find_if(const char *cmd, const char *name, NetIf *out)
{
    int n;
    NetIf *all = interfaces(&n);
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
            err_printf("%s: %s\n", cmd, no_stack_msg);
        else
            err_printf("%s: no interface named %s (see ifconfig -l)\n", cmd, name);
    }
    return ok;
}

/* Waits (processing timers) until the interface has an address. */
static bool wait_address(NetIf *nif, int timeout_ms)
{
    uint64_t end = pal_ticks_ms() + (uint64_t)timeout_ms;
    for (;;) {
        IF_INFO *info = if_info(nif);
        bool ok = info && !ip_zero(&info->StationAddress);
        free(info);
        if (ok)
            return true;
        if (pal_ticks_ms() >= end || con_break())
            return false;
        gBS->Stall(50000);
    }
}

static void ip_list(NetIf *nif, UINT32 type, Sbuf *b)
{
    IPV4 v[8];
    UINTN sz = sizeof(v);
    if (nif->cfg->GetData(nif->cfg, type, &sz, v) != EFI_SUCCESS)
        return;
    for (UINTN i = 0; i < sz / sizeof(IPV4); i++) {
        char *t = ip_str(&v[i]);
        sb_printf(b, "%s%s", b->len ? " " : "", t);
        free(t);
    }
}

/* Gateways: the configured ones, or (with DHCP) the default route. */
static void gateways(NetIf *nif, const IF_INFO *info, Sbuf *b)
{
    ip_list(nif, CFG_GATEWAY, b);
    for (UINT32 i = 0; !b->len && i < info->RouteTableSize; i++) {
        /* with DHCP the gateway is only in the route table, as the default route */
        if (ip_zero(&info->RouteTable[i].Subnet) && ip_zero(&info->RouteTable[i].Mask) &&
            !ip_zero(&info->RouteTable[i].Gateway)) {
            char *g = ip_str(&info->RouteTable[i].Gateway);
            sb_adds(b, g);
            free(g);
        }
    }
}

/* -data: one record per interface */
static void if_data(NetIf *nif)
{
    IF_INFO *info = if_info(nif);
    if (!info)
        return;
    UINTN sz = sizeof(UINT32);
    UINT32 policy = 0;
    nif->cfg->GetData(nif->cfg, CFG_POLICY, &sz, &policy);
    data_record();
    data_field("name", "%s", nif->name);
    data_field("media", "%s", net_media_state(nif->h));
    data_field("policy", "%s", policy == POLICY_DHCP ? "dhcp" : "static");
    Sbuf b;
    sb_init(&b);
    for (UINT32 i = 0; i < info->HwAddressSize && i < 32; i++)
        sb_printf(&b, "%s%02X", i ? "-" : "", info->HwAddress[i]);
    data_field("mac", "%s", b.s ? b.s : "");
    char *ip = ip_str(&info->StationAddress), *mask = ip_str(&info->SubnetMask);
    data_field("ip", "%s", ip);
    data_field("mask", "%s", mask);
    free(ip);
    free(mask);
    b.len = 0;
    gateways(nif, info, &b);
    data_field("gateway", "%s", b.len ? b.s : "");
    b.len = 0;
    ip_list(nif, CFG_DNS, &b);
    data_field("dns", "%s", b.len ? b.s : "");
    sb_free(&b);
    free(info);
}

const char *net_media_state(EFI_HANDLE nic)
{
    struct {
        UINT32 Revision;
        void *f[12];
        struct { UINT32 State; UINT32 HwAddressSize, MediaHeaderSize, MaxPacketSize, NvRamSize, NvRamAccessSize,
                 ReceiveFilterMask, ReceiveFilterSetting, MaxMCastFilterCount, MCastFilterCount;
                 UINT8 MCastFilter[16][32]; UINT8 CurrentAddress[32], BroadcastAddress[32], PermanentAddress[32];
                 UINT8 IfType; BOOLEAN MacAddressChangeable, MultipleTxSupported, MediaPresentSupported, MediaPresent; } *Mode;
    } *snp = NULL;
    if (gBS->HandleProtocol(nic, &snp_guid, (void **)&snp) != EFI_SUCCESS || !snp || !snp->Mode)
        return "unknown";
    return !snp->Mode->MediaPresentSupported ? "unknown" : snp->Mode->MediaPresent ? "present" : "disconnected";
}

static void if_show(NetIf *nif)
{
    IF_INFO *info = if_info(nif);
    if (!info)
        return;
    UINTN sz = sizeof(UINT32);
    UINT32 policy = 0;
    nif->cfg->GetData(nif->cfg, CFG_POLICY, &sz, &policy);
    out_printf("name         : %s\n", nif->name);
    out_printf("media state  : %s\n", net_media_state(nif->h));
    out_printf("policy       : %s\n", policy == POLICY_DHCP ? "dhcp" : "static");
    out_puts("mac addr     : ");
    for (UINT32 i = 0; i < info->HwAddressSize && i < 32; i++)
        out_printf("%s%02X", i ? "-" : "", info->HwAddress[i]);
    char *ip = ip_str(&info->StationAddress), *mask = ip_str(&info->SubnetMask);
    out_printf("\nipv4 address : %s\nsubnet mask  : %s\n", ip, mask);
    free(ip);
    free(mask);
    Sbuf b;
    sb_init(&b);
    gateways(nif, info, &b);
    if (b.len)
        out_printf("gateway      : %s\n", b.s);
    b.len = 0;
    ip_list(nif, CFG_DNS, &b);
    if (b.len)
        out_printf("dns server   : %s\n", b.s);
    sb_free(&b);
    if (info->RouteTableSize) {
        out_puts("route table  :\n");
        for (UINT32 i = 0; i < info->RouteTableSize; i++) {
            char *a = ip_str(&info->RouteTable[i].Subnet), *b = ip_str(&info->RouteTable[i].Mask),
                 *c = ip_str(&info->RouteTable[i].Gateway);
            out_printf("  subnet %s netmask %s gateway %s\n", a, b, c);
            free(a);
            free(b);
            free(c);
        }
    }
    out_puts("\n");
    free(info);
}

static int cmd_ifconfig(int argc, char **argv)
{
    if (argc == 1 || !strcasecmp(argv[1], "-l")) {
        int n;
        NetIf *all = interfaces(&n);
        if (!n) {
            free(all);
            return cmd_err("ifconfig", "%s", no_stack_msg);
        }
        for (int i = 0; i < n; i++)
            if (argc < 3 || !strcasecmp(all[i].name, argv[2])) {
                if (out_data_mode())
                    if_data(&all[i]);
                else
                    if_show(&all[i]);
            }
        free(all);
        return RC_OK;
    }
    if (!strcasecmp(argv[1], "-r")) {
        int n;
        NetIf *all = interfaces(&n);
        int rc = n ? RC_OK : cmd_err("ifconfig", "%s", no_stack_msg);
        for (int i = 0; i < n; i++) {
            if (argc > 2 && strcasecmp(all[i].name, argv[2]))
                continue;
            /* back to DHCP: switching the policy clears the static settings */
            UINT32 p = POLICY_STATIC;
            all[i].cfg->SetData(all[i].cfg, CFG_POLICY, sizeof(p), &p);
            p = POLICY_DHCP;
            EFI_STATUS st = all[i].cfg->SetData(all[i].cfg, CFG_POLICY, sizeof(p), &p);
            if (EFI_ERROR(st))
                rc = cmd_err("ifconfig", "%s: %s", all[i].name, efi_strerror(st));
        }
        free(all);
        return rc;
    }
    if (strcasecmp(argv[1], "-s") || argc < 4)
        return cmd_usage("ifconfig");
    NetIf nif;
    if (!find_if("ifconfig", argv[2], &nif))
        return RC_FAIL;
    const char *mode = argv[3];
    EFI_STATUS st;
    if (!strcasecmp(mode, "dhcp")) {
        UINT32 p = POLICY_DHCP;
        st = nif.cfg->SetData(nif.cfg, CFG_POLICY, sizeof(p), &p);
        if (EFI_ERROR(st))
            return cmd_err("ifconfig", "%s", efi_strerror(st));
        out_printf("%s: waiting for DHCP...", nif.name);
        bool ok = wait_address(&nif, 15000);
        out_puts(ok ? " done\n" : " no answer yet (the address may arrive later)\n");
        if (ok)
            if_show(&nif);
        return ok ? RC_OK : RC_FAIL;
    }
    if (!strcasecmp(mode, "static")) {
        IPV4 m[2], gw;
        if (argc != 7 || !ip_parse(argv[4], &m[0]) || !ip_parse(argv[5], &m[1]) || !ip_parse(argv[6], &gw))
            return cmd_err("ifconfig", "usage: ifconfig -s NAME static IP MASK GATEWAY");
        UINT32 p = POLICY_STATIC;
        st = nif.cfg->SetData(nif.cfg, CFG_POLICY, sizeof(p), &p);
        if (!EFI_ERROR(st))
            st = nif.cfg->SetData(nif.cfg, CFG_MANUAL_ADDRESS, sizeof(m), m);
        if (st == EFI_NOT_READY) /* address being checked for duplicates: done asynchronously */
            st = EFI_SUCCESS;
        if (!EFI_ERROR(st) && !ip_zero(&gw))
            st = nif.cfg->SetData(nif.cfg, CFG_GATEWAY, sizeof(gw), &gw);
        return EFI_ERROR(st) ? cmd_err("ifconfig", "%s", efi_strerror(st)) : RC_OK;
    }
    if (!strcasecmp(mode, "dns")) {
        int n = argc - 4;
        if (n < 1 || n > 8)
            return cmd_err("ifconfig", "usage: ifconfig -s NAME dns IP [IP...]");
        IPV4 d[8];
        for (int i = 0; i < n; i++)
            if (!ip_parse(argv[4 + i], &d[i]))
                return cmd_err("ifconfig", "%s is not an IPv4 address", argv[4 + i]);
        st = nif.cfg->SetData(nif.cfg, CFG_DNS, sizeof(IPV4) * (UINTN)n, d);
        return EFI_ERROR(st) ? cmd_err("ifconfig", "%s", efi_strerror(st)) : RC_OK;
    }
    return cmd_usage("ifconfig");
}

/* ---- ping (ICMP echo over EFI_IP4_PROTOCOL) ---- */

typedef struct {
    UINT8 DefaultProtocol;
    BOOLEAN AcceptAnyProtocol, AcceptIcmpErrors, AcceptBroadcast, AcceptPromiscuous, UseDefaultAddress;
    IPV4 StationAddress, SubnetMask;
    UINT8 TypeOfService, TimeToLive;
    BOOLEAN DoNotFragment, RawData;
    UINT32 ReceiveTimeout, TransmitTimeout;
} IP4_CONFIG_DATA;

typedef struct {
    IPV4 DestinationAddress;
    void *OverrideData;
    UINT32 OptionsLength;
    void *OptionsBuffer;
    UINT32 TotalDataLength;
    UINT32 FragmentCount;
    FRAGMENT FragmentTable[1];
} IP4_TX;

typedef struct {
    EFI_TIME TimeStamp;
    EFI_EVENT RecycleSignal;
    UINT32 HeaderLength;
    UINT8 *Header;
    UINT32 OptionsLength;
    void *Options;
    UINT32 DataLength;
    UINT32 FragmentCount;
    FRAGMENT FragmentTable[1];
} IP4_RX;

typedef struct {
    EFI_EVENT Event;
    EFI_STATUS Status;
    union {
        IP4_RX *RxData;
        IP4_TX *TxData;
    } Packet;
} IP4_TOKEN;

typedef struct {
    void *GetModeData;
    EFI_STATUS(EFIAPI *Configure)(void *This, IP4_CONFIG_DATA *Config);
    void *Groups, *Routes;
    EFI_STATUS(EFIAPI *Transmit)(void *This, IP4_TOKEN *Token);
    EFI_STATUS(EFIAPI *Receive)(void *This, IP4_TOKEN *Token);
    EFI_STATUS(EFIAPI *Cancel)(void *This, IP4_TOKEN *Token);
    EFI_STATUS(EFIAPI *Poll)(void *This);
} IP4;

static uint16_t csum(const uint8_t *d, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2)
        s += (uint32_t)(d[i] << 8 | d[i + 1]);
    if (n & 1)
        s += (uint32_t)d[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static int cmd_ping(int argc, char **argv)
{
    int64_t count = 10, size = 16;
    const char *target = NULL, *ifname = NULL, *source = NULL;
    bool ip6 = false;
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "-n") && i + 1 < argc) {
            if (!parse_int(argv[++i], &count) || count < 1 || count > 10000)
                return cmd_usage("ping");
        } else if (!strcasecmp(argv[i], "-l") && i + 1 < argc) {
            if (!parse_int(argv[++i], &size) || size < 0 || size > 1400)
                return cmd_usage("ping");
        } else if (!strcasecmp(argv[i], "-i") && i + 1 < argc) {
            ifname = argv[++i];
        } else if (!strcasecmp(argv[i], "-s") && i + 1 < argc) {
            source = argv[++i]; /* IPv4: the interface address is used */
        } else if (!strcasecmp(argv[i], "-_ip6")) {
            ip6 = true; /* UEFI Shell option: the address says it already */
        } else if (argv[i][0] != '-' && !target) {
            target = argv[i];
        } else {
            return cmd_usage("ping");
        }
    }
    if (target && (ip6 || strchr(target, ':')))
        return ping6_run("ping", target, count, size, ifname, source);
    IPV4 dst;
    if (!target || !ip_parse(target, &dst))
        return target ? cmd_err("ping", "%s is not an IP address", target) : cmd_usage("ping");
    NetIf nif;
    if (!find_if("ping", ifname, &nif))
        return RC_FAIL;
    if (!wait_address(&nif, 5000))
        return cmd_err("ping", "%s has no IPv4 address (ifconfig -s %s dhcp)", nif.name, nif.name);
    SERVICE_BINDING *sb;
    if (gBS->HandleProtocol(nif.h, &ip4sb_guid, (void **)&sb) != EFI_SUCCESS)
        return cmd_err("ping", "no IP4 service on %s", nif.name);
    EFI_HANDLE child = NULL;
    IP4 *ip4;
    EFI_STATUS st = sb->CreateChild(sb, &child);
    if (EFI_ERROR(st) || gBS->HandleProtocol(child, &ip4_guid, (void **)&ip4) != EFI_SUCCESS)
        return cmd_err("ping", "cannot create an IP4 instance: %s", efi_strerror(st));
    IP4_CONFIG_DATA cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.DefaultProtocol = 1; /* ICMP */
    cfg.AcceptIcmpErrors = TRUE;
    cfg.UseDefaultAddress = TRUE;
    cfg.TimeToLive = 64;
    st = ip4->Configure(ip4, &cfg);
    int rc = RC_OK;
    if (EFI_ERROR(st)) {
        rc = cmd_err("ping", "cannot configure IP4: %s", efi_strerror(st));
        goto out;
    }
    out_printf("Ping %s %lld data bytes.\n", target, (long long)size);
    int sent = 0, received = 0;
    uint64_t rtt_min = UINT64_MAX, rtt_max = 0, rtt_sum = 0;
    uint16_t ident = (uint16_t)(pal_ticks_ms() & 0xFFFF);
    IP4_TOKEN rx;
    memset(&rx, 0, sizeof(rx));
    gBS->CreateEvent(0, 0, NULL, NULL, &rx.Event);
    ip4->Receive(ip4, &rx);
    for (int64_t seq = 1; seq <= count && !con_break(); seq++) {
        size_t plen = 8 + (size_t)size;
        uint8_t *pkt = xcalloc(1, plen);
        pkt[0] = 8; /* echo request */
        pkt[4] = (uint8_t)(ident >> 8), pkt[5] = (uint8_t)ident;
        pkt[6] = (uint8_t)(seq >> 8), pkt[7] = (uint8_t)seq;
        for (size_t k = 8; k < plen; k++)
            pkt[k] = (uint8_t)k;
        uint16_t c = csum(pkt, plen);
        pkt[2] = (uint8_t)(c >> 8), pkt[3] = (uint8_t)c;
        IP4_TX tx;
        memset(&tx, 0, sizeof(tx));
        tx.DestinationAddress = dst;
        tx.TotalDataLength = (UINT32)plen;
        tx.FragmentCount = 1;
        tx.FragmentTable[0].FragmentLength = (UINT32)plen;
        tx.FragmentTable[0].FragmentBuffer = pkt;
        IP4_TOKEN txt;
        memset(&txt, 0, sizeof(txt));
        gBS->CreateEvent(0, 0, NULL, NULL, &txt.Event);
        txt.Packet.TxData = &tx;
        uint64_t t0 = pal_ticks_ms();
        st = ip4->Transmit(ip4, &txt);
        if (!EFI_ERROR(st)) {
            sent++;
            while (gBS->CheckEvent(txt.Event) != EFI_SUCCESS && pal_ticks_ms() - t0 < 1000)
                ip4->Poll(ip4);
        } else {
            err_printf("ping: send failed: %s\n", efi_strerror(st));
        }
        bool got = false;
        while (!EFI_ERROR(st) && !got && pal_ticks_ms() - t0 < 1000 && !con_break()) {
            ip4->Poll(ip4);
            if (gBS->CheckEvent(rx.Event) != EFI_SUCCESS)
                continue;
            IP4_RX *r = rx.Packet.RxData;
            if (rx.Status == EFI_SUCCESS && r && r->FragmentCount && r->DataLength >= 8) {
                const uint8_t *p = r->FragmentTable[0].FragmentBuffer;
                uint16_t rid = (uint16_t)(p[4] << 8 | p[5]), rseq = (uint16_t)(p[6] << 8 | p[7]);
                if (p[0] == 0 && rid == ident && rseq == (uint16_t)seq) {
                    uint64_t rtt = pal_ticks_ms() - t0;
                    uint8_t ttl = r->Header && r->HeaderLength >= 9 ? r->Header[8] : 0;
                    out_printf("%u bytes from %s : icmp_seq=%lld ttl=%u time=%llums\n", r->DataLength - 8, target,
                               (long long)seq, ttl, (unsigned long long)rtt);
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
            ip4->Receive(ip4, &rx);
        }
        if (!got && !EFI_ERROR(st))
            out_printf("Echo request sequence %lld timeout.\n", (long long)seq);
        gBS->CloseEvent(txt.Event);
        free(pkt);
        /* one echo per second */
        while (pal_ticks_ms() - t0 < 1000 && seq < count && !con_break())
            ip4->Poll(ip4);
    }
    ip4->Cancel(ip4, NULL);
    gBS->CloseEvent(rx.Event);
    out_printf("\n%d packets transmitted, %d received, %d%% packet loss\n", sent, received,
               sent ? (sent - received) * 100 / sent : 0);
    if (received)
        out_printf("Rtt(round trip time) min=%llums max=%llums avg=%llums\n", (unsigned long long)rtt_min,
                   (unsigned long long)rtt_max, (unsigned long long)(rtt_sum / (uint64_t)received));
    rc = received ? RC_OK : RC_FAIL;
out:
    ip4->Configure(ip4, NULL);
    sb->DestroyChild(sb, child);
    return rc;
}

/* ---- tftp (EFI_MTFTP4_PROTOCOL / EFI_MTFTP6_PROTOCOL: same functions and token) ---- */

typedef struct {
    BOOLEAN UseDefaultSetting;
    IPV4 StationIp, SubnetMask;
    UINT16 LocalPort;
    IPV4 GatewayIp, ServerIp;
    UINT16 InitialServerPort, TryCount, TimeoutValue;
} MTFTP4_CONFIG;

typedef struct {
    IPV6 StationIp;
    UINT16 LocalPort;
    IPV6 ServerIp;
    UINT16 InitialServerPort, TryCount, TimeoutValue;
} MTFTP6_CONFIG;

typedef struct {
    UINT8 *OptionStr;
    UINT8 *ValueStr;
} MTFTP4_OPTION;

typedef struct {
    EFI_STATUS Status;
    EFI_EVENT Event;
    void *OverrideData;
    UINT8 *Filename;
    UINT8 *ModeStr;
    UINT32 OptionCount;
    MTFTP4_OPTION *OptionList;
    UINT64 BufferSize;
    void *Buffer;
    void *Context;
    void *CheckPacket, *TimeoutCallback, *PacketNeeded;
} MTFTP4_TOKEN;

typedef struct {
    void *GetModeData;
    EFI_STATUS(EFIAPI *Configure)(void *This, void *Config); /* MTFTP4_CONFIG or MTFTP6_CONFIG */
    void *GetInfo, *ParseOptions;
    EFI_STATUS(EFIAPI *ReadFile)(void *This, MTFTP4_TOKEN *Token);
    void *WriteFile, *ReadDirectory, *Poll;
} MTFTP4;

static int cmd_tftp(int argc, char **argv)
{
    const char *ifname = NULL;
    int64_t lport = 0, rport = 69, tries = 6, timeout = 4, blksize = 0;
    char *ops[3];
    int nops = 0;
    for (int i = 1; i < argc; i++) {
        int64_t *dst = !strcasecmp(argv[i], "-l") ? &lport : !strcasecmp(argv[i], "-r") ? &rport
                     : !strcasecmp(argv[i], "-c") ? &tries : !strcasecmp(argv[i], "-t") ? &timeout
                     : !strcasecmp(argv[i], "-s") ? &blksize : NULL;
        if (dst && i + 1 < argc) {
            if (!parse_int(argv[++i], dst) || *dst < 0 || *dst > 65535)
                return cmd_usage("tftp");
        } else if ((!strcasecmp(argv[i], "-i") || !strcasecmp(argv[i], "-w")) && i + 1 < argc) {
            if (argv[i][1] == 'i')
                ifname = argv[i + 1];
            i++;
        } else if (argv[i][0] != '-' && nops < 3) {
            ops[nops++] = argv[i];
        } else {
            return cmd_usage("tftp");
        }
    }
    if (nops < 2)
        return cmd_usage("tftp");
    IPV4 server;
    IPV6 server6;
    bool v6 = false;
    if (!ip_parse(ops[0], &server)) {
        if (!ip6_parse(ops[0], &server6))
            return cmd_err("tftp", "%s is not an IP address", ops[0]);
        v6 = true;
    }
    const char *remote = ops[1];
    const char *slash = strrchr(remote, '/');
    char *local = path_resolve(nops > 2 ? ops[2] : slash ? slash + 1 : remote);
    if (!local)
        return cmd_err("tftp", "invalid local file");
    EFI_HANDLE nic;
    MTFTP4_CONFIG cfg;
    MTFTP6_CONFIG cfg6;
    memset(&cfg, 0, sizeof(cfg));
    memset(&cfg6, 0, sizeof(cfg6));
    if (v6) {
        if (!net6_prepare("tftp", ifname, &server6, &nic, &cfg6.StationIp)) {
            free(local);
            return RC_FAIL;
        }
        cfg6.LocalPort = (UINT16)lport;
        cfg6.ServerIp = server6;
        cfg6.InitialServerPort = (UINT16)rport;
        cfg6.TryCount = (UINT16)tries;
        cfg6.TimeoutValue = (UINT16)timeout;
    } else {
        NetIf nif;
        if (!find_if("tftp", ifname, &nif)) {
            free(local);
            return RC_FAIL;
        }
        if (!wait_address(&nif, 5000)) {
            free(local);
            return cmd_err("tftp", "%s has no IPv4 address (ifconfig -s %s dhcp)", nif.name, nif.name);
        }
        nic = nif.h;
        cfg.UseDefaultSetting = TRUE;
        cfg.LocalPort = (UINT16)lport;
        cfg.ServerIp = server;
        cfg.InitialServerPort = (UINT16)rport;
        cfg.TryCount = (UINT16)tries;
        cfg.TimeoutValue = (UINT16)timeout;
    }
    SERVICE_BINDING *sb;
    EFI_HANDLE child = NULL;
    MTFTP4 *m;
    if (gBS->HandleProtocol(nic, v6 ? &mtftp6sb_guid : &mtftp4sb_guid, (void **)&sb) != EFI_SUCCESS ||
        sb->CreateChild(sb, &child) != EFI_SUCCESS ||
        gBS->HandleProtocol(child, v6 ? &mtftp6_guid : &mtftp4_guid, (void **)&m) != EFI_SUCCESS) {
        free(local);
        return cmd_err("tftp", "no TFTP%s service on this interface (Mtftp%dDxe not loaded?)", v6 ? "v6" : "", v6 ? 6 : 4);
    }
    int rc = RC_OK;
    EFI_STATUS st = m->Configure(m, v6 ? (void *)&cfg6 : (void *)&cfg);
    if (EFI_ERROR(st)) {
        rc = cmd_err("tftp", "cannot configure TFTP: %s", efi_strerror(st));
        goto out;
    }
    char bs[16];
    snprintf(bs, sizeof(bs), "%lld", (long long)blksize);
    MTFTP4_OPTION opt = { (UINT8 *)"blksize", (UINT8 *)bs };
    UINT64 size = 1024 * 1024;
    for (int attempt = 0; attempt < 2; attempt++) {
        MTFTP4_TOKEN t;
        memset(&t, 0, sizeof(t));
        t.Filename = (UINT8 *)remote;
        t.ModeStr = (UINT8 *)"octet";
        if (blksize) {
            t.OptionCount = 1;
            t.OptionList = &opt;
        }
        t.BufferSize = size;
        t.Buffer = xmalloc((size_t)size);
        out_printf("Downloading %s from %s ...\n", remote, ops[0]);
        st = m->ReadFile(m, &t); /* no event: the call returns when the transfer is over */
        if (st == EFI_BUFFER_TOO_SMALL && attempt == 0 && t.BufferSize > size) {
            size = t.BufferSize; /* the whole file size is known now: download again */
            free(t.Buffer);
            continue;
        }
        if (EFI_ERROR(st)) {
            rc = cmd_err("tftp", "download failed: %s", efi_strerror(st));
        } else {
            int e = file_write_all(local, t.Buffer, (size_t)t.BufferSize, false);
            if (e)
                rc = cmd_perr("tftp", local, e);
            else
                out_printf("%llu bytes saved to %s\n", (unsigned long long)t.BufferSize, local);
        }
        free(t.Buffer);
        break;
    }
out:
    m->Configure(m, NULL);
    sb->DestroyChild(sb, child);
    free(local);
    return rc;
}

/* ---- http (EFI_HTTP_PROTOCOL) ---- */

typedef struct {
    BOOLEAN UseDefaultAddress;
    IPV4 LocalAddress, LocalSubnet;
    UINT16 LocalPort;
} HTTP4_ACCESS;

typedef struct {
    IPV6 LocalAddress;
    UINT16 LocalPort;
} HTTP6_ACCESS;

typedef struct {
    UINT32 HttpVersion; /* 1: HTTP/1.1 */
    UINT32 TimeOutMillisec;
    BOOLEAN LocalAddressIsIPv6;
    void *AccessPoint;
} HTTP_CONFIG;

typedef struct {
    CHAR8 *FieldName;
    CHAR8 *FieldValue;
} HTTP_HEADER;

typedef struct {
    UINT32 Method; /* 0: GET */
    CHAR16 *Url;
} HTTP_REQUEST;

typedef struct {
    UINT32 StatusCode;
} HTTP_RESPONSE;

typedef struct {
    void *Data; /* HTTP_REQUEST* or HTTP_RESPONSE* */
    UINTN HeaderCount;
    HTTP_HEADER *Headers;
    UINTN BodyLength;
    void *Body;
} HTTP_MESSAGE;

typedef struct {
    EFI_EVENT Event;
    EFI_STATUS Status;
    HTTP_MESSAGE *Message;
} HTTP_TOKEN;

typedef struct {
    void *GetModeData;
    EFI_STATUS(EFIAPI *Configure)(void *This, HTTP_CONFIG *Config);
    EFI_STATUS(EFIAPI *Request)(void *This, HTTP_TOKEN *Token);
    EFI_STATUS(EFIAPI *Cancel)(void *This, HTTP_TOKEN *Token);
    EFI_STATUS(EFIAPI *Response)(void *This, HTTP_TOKEN *Token);
    EFI_STATUS(EFIAPI *Poll)(void *This);
} HTTP;

static int http_code(UINT32 s)
{
    static const int codes[] = { 0, 100, 101, 200, 201, 202, 203, 204, 205, 206, 300, 301, 302, 303, 304, 305, 307,
                                 400, 401, 402, 403, 404, 405, 406, 407, 408, 409, 410, 411, 412, 413, 414, 415, 416,
                                 417, 500, 501, 502, 503, 504, 505, 308, 429 };
    return s < ARRAY_SIZE(codes) ? codes[s] : 0;
}

static bool http_wait(HTTP *h, HTTP_TOKEN *t, uint64_t timeout_ms)
{
    uint64_t end = pal_ticks_ms() + timeout_ms;
    while (gBS->CheckEvent(t->Event) != EFI_SUCCESS) {
        if (pal_ticks_ms() > end || con_break()) {
            h->Cancel(h, t);
            return false;
        }
        h->Poll(h);
    }
    return true;
}

static int cmd_http(int argc, char **argv)
{
    const char *ifname = NULL;
    int64_t timeout = 5000, lport = 0, bufsize = 32768;
    char *ops[2];
    int nops = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "-i") && i + 1 < argc) {
            ifname = argv[++i];
        } else if ((!strcasecmp(argv[i], "-t") || !strcasecmp(argv[i], "-l") || !strcasecmp(argv[i], "-s")) && i + 1 < argc) {
            int64_t *d = argv[i][1] == 't' ? &timeout : argv[i][1] == 'l' ? &lport : &bufsize;
            if (!parse_int(argv[++i], d) || *d < 0)
                return cmd_usage("http");
        } else if (!strcasecmp(argv[i], "-m")) {
            /* downloading into memory only: accepted, the file is still written */
        } else if (argv[i][0] != '-' && nops < 2) {
            ops[nops++] = argv[i];
        } else {
            return cmd_usage("http");
        }
    }
    if (!nops)
        return cmd_usage("http");
    const char *url = ops[0];
    if (strncasecmp(url, "http://", 7))
        return cmd_err("http", "only http:// addresses are supported (https needs the firmware TLS driver)");
    const char *hoststart = url + 7;
    size_t hostlen = strcspn(hoststart, "/");
    char *host = xstrndup(hoststart, hostlen);
    const char *slash = strrchr(url + 7, '/');
    const char *fname = slash && slash[1] ? slash + 1 : "index.html";
    char *local = path_resolve(nops > 1 ? ops[1] : fname);
    if (!local) {
        free(host);
        return cmd_err("http", "invalid local file");
    }
    SERVICE_BINDING *sb = NULL;
    EFI_HANDLE child = NULL, nic = NULL;
    HTTP *h = NULL;
    int rc = RC_OK;
    HTTP4_ACCESS ap;
    HTTP6_ACCESS ap6;
    memset(&ap, 0, sizeof(ap));
    memset(&ap6, 0, sizeof(ap6));
    bool v6 = host[0] == '[';
    if (v6) {
        /* http://[2001:db8::1]:8080/file */
        IPV6 dst;
        char *close = strchr(host, ']');
        char *a = close ? xstrndup(host + 1, (size_t)(close - host - 1)) : NULL;
        bool ok = a && ip6_parse(a, &dst);
        free(a);
        if (!ok) {
            rc = cmd_err("http", "%s is not a valid IPv6 address", host);
            goto done;
        }
        if (!net6_prepare("http", ifname, &dst, &nic, &ap6.LocalAddress)) {
            rc = RC_FAIL;
            goto done;
        }
        ap6.LocalPort = (UINT16)lport;
    } else {
        NetIf nif;
        if (!find_if("http", ifname, &nif)) {
            rc = RC_FAIL;
            goto done;
        }
        if (!wait_address(&nif, 5000)) {
            rc = cmd_err("http", "%s has no IPv4 address (ifconfig -s %s dhcp)", nif.name, nif.name);
            goto done;
        }
        nic = nif.h;
        ap.UseDefaultAddress = TRUE;
        ap.LocalPort = (UINT16)lport;
    }
    if (gBS->HandleProtocol(nic, &httpsb_guid, (void **)&sb) != EFI_SUCCESS || sb->CreateChild(sb, &child) != EFI_SUCCESS ||
        gBS->HandleProtocol(child, &http_guid, (void **)&h) != EFI_SUCCESS) {
        rc = cmd_err("http", "no HTTP service on this interface (HttpDxe not loaded?)");
        sb = NULL;
        goto done;
    }
    HTTP_CONFIG cfg = { 1, (UINT32)timeout, v6, v6 ? (void *)&ap6 : (void *)&ap };
    EFI_STATUS st = h->Configure(h, &cfg);
    if (EFI_ERROR(st)) {
        rc = cmd_err("http", "cannot configure HTTP: %s", efi_strerror(st));
        goto done;
    }
    /* request */
    uint16_t *wurl = utf8_to_ucs2(url, NULL);
    HTTP_REQUEST req = { 0, wurl };
    HTTP_HEADER hdrs[3] = { { "Host", host }, { "Accept", "*/*" }, { "User-Agent", "NESH/" NESH_VERSION } };
    HTTP_MESSAGE msg = { &req, 3, hdrs, 0, NULL };
    HTTP_TOKEN tok = { NULL, EFI_SUCCESS, &msg };
    gBS->CreateEvent(0, 0, NULL, NULL, &tok.Event);
    out_printf("Downloading %s\n", url);
    st = h->Request(h, &tok);
    if (EFI_ERROR(st) || !http_wait(h, &tok, (uint64_t)timeout) || EFI_ERROR(tok.Status)) {
        rc = cmd_err("http", "request failed: %s", efi_strerror(EFI_ERROR(st) ? st : EFI_ERROR(tok.Status) ? tok.Status : EFI_TIMEOUT));
        goto req_done;
    }
    /* response: status, headers and the first part of the body */
    HTTP_RESPONSE resp = { 0 };
    uint8_t *buf = xmalloc((size_t)bufsize);
    HTTP_MESSAGE rmsg = { &resp, 0, NULL, (UINTN)bufsize, buf };
    tok.Message = &rmsg;
    tok.Status = EFI_SUCCESS;
    st = h->Response(h, &tok);
    if (EFI_ERROR(st) || !http_wait(h, &tok, (uint64_t)timeout) || EFI_ERROR(tok.Status)) {
        rc = cmd_err("http", "no response: %s", efi_strerror(EFI_ERROR(st) ? st : EFI_ERROR(tok.Status) ? tok.Status : EFI_TIMEOUT));
        free(buf);
        goto req_done;
    }
    int code = http_code(resp.StatusCode);
    uint64_t length = UINT64_MAX;
    for (UINTN i = 0; i < rmsg.HeaderCount; i++)
        if (rmsg.Headers[i].FieldName && !strcasecmp(rmsg.Headers[i].FieldName, "Content-Length"))
            length = strtoull(rmsg.Headers[i].FieldValue, NULL, 10);
    if (rmsg.Headers)
        gBS->FreePool(rmsg.Headers);
    if (code != 200) {
        rc = cmd_err("http", "server answered %d", code);
        free(buf);
        goto req_done;
    }
    Sbuf body;
    sb_init(&body);
    sb_add(&body, (const char *)buf, rmsg.BodyLength);
    /* the rest of the body */
    while (body.len < length && !con_break()) {
        HTTP_MESSAGE more = { NULL, 0, NULL, (UINTN)bufsize, buf };
        tok.Message = &more;
        tok.Status = EFI_SUCCESS;
        st = h->Response(h, &tok);
        if (EFI_ERROR(st) || !http_wait(h, &tok, (uint64_t)timeout) || EFI_ERROR(tok.Status) || !more.BodyLength)
            break; /* end of data (connection closed) or error */
        sb_add(&body, (const char *)buf, more.BodyLength);
        if (length != UINT64_MAX)
            out_printf("\r  %zu / %llu bytes", body.len, (unsigned long long)length);
    }
    free(buf);
    if (length != UINT64_MAX && body.len < length) {
        rc = cmd_err("http", "\ndownload incomplete (%zu of %llu bytes)", body.len, (unsigned long long)length);
    } else {
        int e = file_write_all(local, body.s ? body.s : "", body.len, false);
        if (e)
            rc = cmd_perr("http", local, e);
        else
            out_printf("\n%zu bytes saved to %s\n", body.len, local);
    }
    sb_free(&body);
req_done:
    gBS->CloseEvent(tok.Event);
    free(wurl);
    h->Configure(h, NULL);
done:
    if (sb && child)
        sb->DestroyChild(sb, child);
    free(host);
    free(local);
    return rc;
}

static const Cmd net_cmds[] = {
    { "ifconfig", cmd_ifconfig, "ifconfig [-l [NAME]] | -r [NAME] | -s NAME dhcp | -s NAME static IP MASK GW | -s NAME dns IP...",
      "Show or set the IPv4 configuration of the network interfaces",
      "  ifconfig -l [NAME]          list the interfaces (eth0...) and their settings\n"
      "  ifconfig -s eth0 dhcp       get an address with DHCP (waits up to 15 s)\n"
      "  ifconfig -s eth0 static 192.168.1.10 255.255.255.0 192.168.1.1\n"
      "                              fixed address, mask and gateway\n"
      "  ifconfig -s eth0 dns 192.168.1.1   DNS servers\n"
      "  ifconfig -r [NAME]          reset to DHCP\n"
      "The firmware network drivers must be loaded (network boot enabled in the\n"
      "setup, or: load MnpDxe.efi ArpDxe.efi Ip4Dxe.efi Udp4Dxe.efi Dhcp4Dxe.efi\n"
      "Mtftp4Dxe.efi TcpDxe.efi ...). See also ifconfig6 for IPv6.\n"
      "With -data (ifconfig -l -data): name, media, policy, mac, ip, mask,\n"
      "gateway, dns.\n", CMD_DATA },
    { "ping", cmd_ping, "ping [-n COUNT] [-l SIZE] [-i IF] ADDRESS",
      "Send echo requests to an IPv4 or IPv6 address",
      "  ping 192.168.1.1           IPv4\n"
      "  ping -n 3 2001:db8::1      IPv6 (same as ping6)\n"
      "Ends with an error (ERR <> 0) when no answer arrives.\n" },
    { "tftp", cmd_tftp, "tftp [-i IF] [-l PORT] [-r PORT] [-c TRIES] [-t TIMEOUT] [-s BLKSIZE] SERVER REMOTE [LOCAL]",
      "Download a file with TFTP",
      "  tftp 192.168.1.5 boot/grubx64.efi            saved as grubx64.efi\n"
      "  tftp 2001:db8::5 boot/grubx64.efi fs1:\\g.efi  IPv6 server\n" },
    { "http", cmd_http, "http [-i IF] [-t TIMEOUT_MS] [-s BUFSIZE] URL [LOCAL]", "Download a file with HTTP",
      "  http http://192.168.1.5/images/tool.efi\n"
      "  http http://[2001:db8::5]:8080/tool.efi fs1:\\tool.efi\n"
      "                               IPv6 address: in brackets\n"
      "Only http:// (https needs the firmware TLS driver).\n" },
};

void efi_net_init(void)
{
    shell_register(net_cmds, ARRAY_SIZE(net_cmds));
    efi_net6_init();
}
