#ifndef LL_WIN32_NET_IF_ARP_H
#define LL_WIN32_NET_IF_ARP_H

// <net/if_arp.h> for MinGW: the hardware types getifaddrs() reports for an
// AF_PACKET entry.

#include <net/if.h>

#define ARPHRD_NETROM 0
#define ARPHRD_ETHER 1
#define ARPHRD_IEEE802 6
#define ARPHRD_LOOPBACK 772
#define ARPHRD_VOID 0xFFFF
#define ARPHRD_NONE 0xFFFE

struct arpreq
{
    struct sockaddr arp_pa;
    struct sockaddr arp_ha;
    int arp_flags;
    struct sockaddr arp_netmask;
    char arp_dev[IFNAMSIZ];
};

#endif // LL_WIN32_NET_IF_ARP_H
