#ifndef LL_WIN32_NETINET_IN_H
#define LL_WIN32_NETINET_IN_H

// <netinet/in.h> for MinGW: Winsock's in_addr, sockaddr_in and the IP_
// constants, under the names the arcade network code uses.

#include <sys/socket.h>

#ifndef IPPROTO_IP
#define IPPROTO_IP 0
#endif

// Linux spells the option levels IPPROTO_*; Winsock has the same values.
#ifndef IP_TOS
#define IP_TOS 1
#define IP_TTL 2
#define IP_HDRINCL 3
#define IP_OPTIONS 4
#define IP_MULTICAST_IF 9
#define IP_MULTICAST_TTL 10
#define IP_MULTICAST_LOOP 11
#define IP_ADD_MEMBERSHIP 12
#define IP_DROP_MEMBERSHIP 13
#endif
#ifndef IP_MULTICAST_ALL
#define IP_MULTICAST_ALL 49
#endif
#ifndef IP_PKTINFO
#define IP_PKTINFO 8
#endif

#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif
#ifndef TCP_MAXSEG
#define TCP_MAXSEG 2
#endif
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE 4
#endif
#ifndef TCP_KEEPINTVL
#define TCP_KEEPINTVL 5
#endif
#ifndef TCP_KEEPCNT
#define TCP_KEEPCNT 6
#endif

#ifndef INADDR_ANY
#define INADDR_ANY ((unsigned long)0x00000000)
#endif
#ifndef INADDR_BROADCAST
#define INADDR_BROADCAST ((unsigned long)0xffffffff)
#endif
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK ((unsigned long)0x7f000001)
#endif
#ifndef INADDR_NONE
#define INADDR_NONE ((unsigned long)0xffffffff)
#endif
#ifndef IN_CLASSA
#define IN_CLASSA(a) ((((long int)(a)) & 0x80000000) == 0)
#endif

#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46

#endif // LL_WIN32_NETINET_IN_H
