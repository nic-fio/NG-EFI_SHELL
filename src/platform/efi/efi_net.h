/* Shared by the IPv4 (efi_net.c) and IPv6 (efi_net6.c) network commands. */
#ifndef EFI_NET_H
#define EFI_NET_H

#include "efi_cmds.h"

typedef struct {
    UINT8 a[16];
} IPV6;

typedef struct {
    EFI_STATUS(EFIAPI *CreateChild)(void *This, EFI_HANDLE *Child);
    EFI_STATUS(EFIAPI *DestroyChild)(void *This, EFI_HANDLE Child);
} SERVICE_BINDING;

typedef struct {
    UINT32 FragmentLength;
    void *FragmentBuffer;
} FRAGMENT;

/* "present", "disconnected" or "unknown" (from the Simple Network Protocol of the NIC) */
const char *net_media_state(EFI_HANDLE nic);

/* IPv6 text <-> binary. ip6_parse accepts "fec0::2" and "[fec0::2]". */
bool ip6_parse(const char *s, IPV6 *ip);
char *ip6_str(const IPV6 *ip);
bool ip6_zero(const IPV6 *ip);

/* Finds the IPv6 interface (name NULL: the first one) and waits until it has an
 * address that can reach dst (link-local for fe80::, any other one otherwise).
 * Returns the NIC handle and that source address; prints the error on failure. */
bool net6_prepare(const char *cmd, const char *ifname, const IPV6 *dst, EFI_HANDLE *nic, IPV6 *src);

/* ping over IPv6 (used by ping6 and by ping with an IPv6 address) */
int ping6_run(const char *cmd, const char *target, int64_t count, int64_t size, const char *ifname, const char *source);

void efi_net6_init(void);

#endif
