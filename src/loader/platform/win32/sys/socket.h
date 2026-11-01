#ifndef LL_WIN32_SYS_SOCKET_H
#define LL_WIN32_SYS_SOCKET_H

// <sys/socket.h> for MinGW: Winsock2 under the names the arcade network code
// uses.  A socket is an int here as it is on Linux (a Win32 SOCKET fits), so
// the game's structs and calls keep their shape.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <stddef.h>
#include <stdint.h>

typedef int socklen_t;
typedef unsigned short sa_family_t;
typedef unsigned int in_addr_t;

// The names Winsock spells differently.
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#endif

#define MSG_NOSIGNAL 0
#define MSG_DONTWAIT 0
#define MSG_WAITALL 0
#define MSG_PEEK MSG_PEEK
#define MSG_OOB MSG_OOB

#define SOL_IP IPPROTO_IP
#define SOL_TCP IPPROTO_TCP

// Af_unix sockets have no Winsock counterpart; the code that opens one is
// Linux's (the game's IPC), and fails here.
#ifndef AF_UNIX
#define AF_UNIX 1
#define PF_UNIX AF_UNIX
#endif
#ifndef AF_INET
#define AF_INET 2
#define PF_INET AF_INET
#endif
#ifndef AF_INET6
#define AF_INET6 23
#define PF_INET6 AF_INET6
#endif
#ifndef AF_PACKET
#define AF_PACKET 17
#define PF_PACKET AF_PACKET
#endif

#ifndef PF_LOCAL
#define PF_LOCAL AF_UNIX
#endif

// Linux's struct sockaddr / sockaddr_storage, which Winsock also has, are
// the same layout; the POSIX aliases the code reads are added here.
typedef struct sockaddr sockaddr;
typedef struct sockaddr_storage sockaddr_storage;

// The scatter/gather calls (sendmsg, recvmsg) have no Winsock equivalent;
// the arcade IPC that uses them is Linux's, and fails here.
struct iovec
{
    void *iov_base;
    size_t iov_len;
};

struct msghdr
{
    void *msg_name;
    socklen_t msg_namelen;
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

struct cmsghdr
{
    size_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
};

#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_DATA(cmsg) ((unsigned char *)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_FIRSTHDR(mhdr) ((mhdr)->msg_controllen >= sizeof(struct cmsghdr) ? (struct cmsghdr *)(mhdr)->msg_control : (struct cmsghdr *)NULL)

#ifdef __cplusplus
extern "C"
{
#endif

    // socket(), bind(), connect() and the rest are Winsock's own: its
    // sockaddr taking an int length is Linux's socklen_t here. socketpair()
    // and the message calls are the ones that have no Winsock call behind
    // them.
    int socketpair(int domain, int type, int protocol, int sv[2]);
    ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);
    ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags);

#ifdef __cplusplus
}
#endif

#endif // LL_WIN32_SYS_SOCKET_H
