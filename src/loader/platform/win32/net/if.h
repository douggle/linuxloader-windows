#ifndef LL_WIN32_NET_IF_H
#define LL_WIN32_NET_IF_H

// <net/if.h> for MinGW: the interface flags getifaddrs() reports, and the
// struct ifreq the SIOCGIFHWADDR lookup fills (platform/win32/win32Posix.c).

#include <sys/socket.h>
#include <sys/ioctl.h>

#define IF_NAMESIZE 16
#define IFNAMSIZ IF_NAMESIZE

// Interface flags (Linux's values; getifaddrs() maps the adapter status onto
// them).
#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_DEBUG 0x4
#define IFF_LOOPBACK 0x8
#define IFF_POINTOPOINT 0x10
#define IFF_NOTRAILERS 0x20
#define IFF_RUNNING 0x40
#define IFF_NOARP 0x80
#define IFF_PROMISC 0x100
#define IFF_ALLMULTI 0x200
#define IFF_MULTICAST 0x1000

struct ifmap
{
    unsigned long mem_start;
    unsigned long mem_end;
    unsigned short base_addr;
    unsigned char irq;
    unsigned char dma;
    unsigned char port;
};

struct ifreq
{
    union
    {
        char ifrn_name[IFNAMSIZ];
        char ifru_name[IFNAMSIZ];
    } ifr_ifrn;
    union
    {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_dstaddr;
        struct sockaddr ifru_broadaddr;
        struct sockaddr ifru_netmask;
        struct sockaddr ifru_hwaddr;
        short ifru_flags;
        int ifru_ivalue;
        int ifru_mtu;
        struct ifmap ifru_map;
        char ifru_slave[IFNAMSIZ];
        char ifru_newname[IFNAMSIZ];
        void *ifru_data;
    } ifr_ifru;
};

#define ifr_name ifr_ifrn.ifrn_name
#define ifr_hwaddr ifr_ifru.ifru_hwaddr
#define ifr_addr ifr_ifru.ifru_addr
#define ifr_netmask ifr_ifru.ifru_netmask
#define ifr_broadaddr ifr_ifru.ifru_broadaddr
#define ifr_flags ifr_ifru.ifru_flags
#define ifr_mtu ifr_ifru.ifru_mtu

// The interface requests the loader answers.
#define SIOCGIFNAME 0x8910
#define SIOCGIFCONF 0x8912
#define SIOCGIFFLAGS 0x8913
#define SIOCSIFFLAGS 0x8914
#define SIOCGIFADDR 0x8915
#define SIOCSIFADDR 0x8916
#define SIOCGIFNETMASK 0x891b
#define SIOCGIFBRDADDR 0x8919
#define SIOCGIFHWADDR 0x8927
#define SIOCGIFMTU 0x8921

// if_nametoindex(), if_indextoname() and the IF_NAMESIZE name come from
// ws2tcpip.h on Windows; the index list is the loader's own, over the same
// adapter enumeration (platform/win32/win32Posix.c).
struct if_nameindex
{
    unsigned int if_index;
    char *if_name;
};

#ifdef __cplusplus
extern "C"
{
#endif

    struct if_nameindex *if_nameindex(void);
    void if_freenameindex(struct if_nameindex *ptr);

#ifdef __cplusplus
}
#endif

#endif // LL_WIN32_NET_IF_H
