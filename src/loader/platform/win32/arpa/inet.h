#ifndef LL_WIN32_ARPA_INET_H
#define LL_WIN32_ARPA_INET_H

// <arpa/inet.h> for MinGW: inet_addr(), inet_ntoa(), inet_aton() and the
// byte-order helpers, which Winsock declares (inet_aton() as well, in
// ws2tcpip.h).  inet_pton() and inet_ntop() come from ws2tcpip.h too.

#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Winsock has InetPtonA() under the name inet_pton() only with
    // __STDC_WANT_LIB_EXT1__; the loader spells the call itself.
    int inet_aton(const char *cp, struct in_addr *inp);

#ifdef __cplusplus
}
#endif

#endif // LL_WIN32_ARPA_INET_H
