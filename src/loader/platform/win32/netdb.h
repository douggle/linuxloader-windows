#ifndef LL_WIN32_NETDB_H
#define LL_WIN32_NETDB_H

// <netdb.h> for MinGW: Winsock's getaddrinfo(), gethostbyname() and
// getservbyname(), and the error codes the arcade network code checks.

#include <sys/socket.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Winsock's WSAHOST_NOT_FOUND and friends are EAI_*'s values on Linux;
    // the loader names the ones the code compares against.
#ifndef EAI_AGAIN
#define EAI_AGAIN WSATRY_AGAIN
#define EAI_BADFLAGS WSAEINVAL
#define EAI_FAIL WSANO_RECOVERY
#define EAI_FAMILY WSAEAFNOSUPPORT
#define EAI_MEMORY WSA_NOT_ENOUGH_MEMORY
#define EAI_NODATA WSANO_DATA
#define EAI_NONAME WSAHOST_NOT_FOUND
#define EAI_SERVICE WSATYPE_NOT_FOUND
#define EAI_SOCKTYPE WSAESOCKTNOSUPPORT
#endif

#define NI_MAXHOST 1025
#define NI_MAXSERV 32
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_NOFQDN 4
#define NI_NAMEREQD 8
#define NI_DGRAM 16

#ifndef HOST_NOT_FOUND
#define HOST_NOT_FOUND 1
#define TRY_AGAIN 2
#define NO_RECOVERY 3
#define NO_DATA 4
#define NO_ADDRESS NO_DATA
#endif

#ifdef __cplusplus
}
#endif

#endif // LL_WIN32_NETDB_H
