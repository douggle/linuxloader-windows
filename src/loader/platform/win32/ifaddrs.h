#ifndef LL_WIN32_IFADDRS_H
#define LL_WIN32_IFADDRS_H

// <ifaddrs.h> for MinGW: the machine's interfaces, over GetAdaptersAddresses.
// Every adapter's IPv4 and IPv6 addresses are an entry each, and so is its
// MAC (an AF_PACKET entry), as Linux reports them.

#include <sys/socket.h>

#ifdef __cplusplus
extern "C"
{
#endif

    struct ifaddrs
    {
        struct ifaddrs *ifa_next;
        char *ifa_name;
        unsigned int ifa_flags;
        struct sockaddr *ifa_addr;
        struct sockaddr *ifa_netmask;
        union
        {
            struct sockaddr *ifu_broadaddr;
            struct sockaddr *ifu_dstaddr;
        } ifa_ifu;
        void *ifa_data;
    };

#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr ifa_ifu.ifu_dstaddr

    int getifaddrs(struct ifaddrs **ifap);
    void freeifaddrs(struct ifaddrs *ifa);

#ifdef __cplusplus
}
#endif

#endif // LL_WIN32_IFADDRS_H
