// The POSIX calls the arcade support makes that Windows has no equivalent for,
// or none under that name: the mapping the loader does by hand (mmap), the
// interface ioctls of the Namco network administration, the raw syscalls the
// loader reaches libc with (a thread id, the settings files), and the free
// space of a game's drive.  The device code itself is not here: the Windows
// build has no evdev, /dev/dsp or /dev/video (see win32Devices.c).

#define _CRT_SECURE_NO_WARNINGS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <io.h>
#include <fcntl.h>
#include <process.h>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "sys/mman.h"
#include "sys/ioctl.h"
#include "sys/statvfs.h"
#include "sys/syscall.h"
#include "sys/socket.h"
#include "arpa/inet.h"
#include "ifaddrs.h"
#include "net/if.h"
#include "net/if_arp.h"

// -----------------------------------------------------------------------------
// mmap: the loader's own mappings (trampolines, the game's images)
// -----------------------------------------------------------------------------

static DWORD pageProtect(int prot)
{
    int readable = prot & PROT_READ, writable = prot & PROT_WRITE, executable = prot & PROT_EXEC;

    if (writable)
        return executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    if (executable)
        return readable ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    return readable ? PAGE_READONLY : PAGE_NOACCESS;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    DWORD protect = pageProtect(prot);
    void *p;

    (void)fd;
    (void)offset;

    if (length == 0)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    // A file mapping would need the file read in first; every caller here maps
    // anonymous memory (the trampolines) or asks for an address of its own.
    if (!(flags & MAP_ANONYMOUS))
    {
        errno = ENODEV;
        return MAP_FAILED;
    }

    p = VirtualAlloc((flags & MAP_FIXED) ? addr : NULL, length, MEM_RESERVE | MEM_COMMIT, protect);
    if (!p)
    {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return p;
}

int munmap(void *addr, size_t length)
{
    (void)length;
    if (!addr || !VirtualFree(addr, 0, MEM_RELEASE))
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int mprotect(void *addr, size_t len, int prot)
{
    DWORD old;

    if (!VirtualProtect(addr, len, pageProtect(prot), &old))
    {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int msync(void *addr, size_t length, int flags)
{
    (void)addr;
    (void)length;
    (void)flags;
    return 0;
}

int madvise(void *addr, size_t length, int advice)
{
    (void)addr;
    (void)length;
    (void)advice;
    return 0;
}

int mlock(const void *addr, size_t len)
{
    return VirtualLock((void *)addr, len) ? 0 : -1;
}

int munlock(const void *addr, size_t len)
{
    return VirtualUnlock((void *)addr, len) ? 0 : -1;
}

// -----------------------------------------------------------------------------
// The interfaces, over GetAdaptersAddresses
// -----------------------------------------------------------------------------

// The adapter's name in the loader's terms: its friendly name ("Ethernet"),
// which is short enough for IFNAMSIZ and is what the game's reports are
// written with.
static void adapterName(const IP_ADAPTER_ADDRESSES *adapter, char *out, size_t size)
{
    if (adapter->FriendlyName && WideCharToMultiByte(CP_ACP, 0, adapter->FriendlyName, -1, out, (int)size, NULL, NULL) > 0)
        return;
    snprintf(out, size, "eth0");
}

// The first adapter with an IPv4 address, or NULL.
static IP_ADAPTER_ADDRESSES *firstAdapter(ULONG family)
{
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *list = NULL;

    for (int attempt = 0; attempt < 3; attempt++)
    {
        list = (IP_ADAPTER_ADDRESSES *)malloc(size);
        if (!list)
            return NULL;
        ULONG result = GetAdaptersAddresses(family, GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                            NULL, list, &size);
        if (result == NO_ERROR)
            return list;
        free(list);
        list = NULL;
        if (result != ERROR_BUFFER_OVERFLOW)
            return NULL;
    }
    return NULL;
}

static IP_ADAPTER_ADDRESSES *findAdapter(IP_ADAPTER_ADDRESSES *list, const char *name)
{
    IP_ADAPTER_ADDRESSES *fallback = NULL;

    for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next)
    {
        char friendly[IFNAMSIZ];

        adapterName(a, friendly, sizeof(friendly));
        if (name && name[0] && strcmp(name, friendly) == 0)
            return a;
        if (!fallback && a->IfType != IF_TYPE_SOFTWARE_LOOPBACK && a->FirstUnicastAddress)
            fallback = a;
    }
    // The game names eth0 whatever the machine calls its adapter.
    return fallback ? fallback : list;
}

static void freeAdapterList(IP_ADAPTER_ADDRESSES *list)
{
    if (list)
        free(list);
}

static unsigned int adapterFlags(const IP_ADAPTER_ADDRESSES *a)
{
    unsigned int flags = IFF_MULTICAST;

    if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
        return IFF_UP | IFF_RUNNING | IFF_LOOPBACK;
    switch (a->OperStatus)
    {
    case IfOperStatusUp:
        flags |= IFF_UP | IFF_RUNNING | IFF_BROADCAST;
        break;
    default:
        break;
    }
    if (a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211)
        flags |= IFF_BROADCAST;
    return flags;
}

// The address of the adapter's first unicast address of the family, and its
// netmask out of the prefix length.
static int adapterAddress(const IP_ADAPTER_ADDRESSES *a, int family, struct sockaddr_storage *addr, struct sockaddr_storage *mask,
                          socklen_t *addrLen, socklen_t *maskLen)
{
    for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
    {
        if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != family)
            continue;
        if (addr)
        {
            memcpy(addr, u->Address.lpSockaddr, u->Address.iSockaddrLength);
            *addrLen = (socklen_t)u->Address.iSockaddrLength;
        }
        if (mask && family == AF_INET)
        {
            struct sockaddr_in *m = (struct sockaddr_in *)mask;
            unsigned long bits = u->OnLinkPrefixLength > 32 ? 32 : u->OnLinkPrefixLength;

            memset(m, 0, sizeof(*m));
            m->sin_family = AF_INET;
            m->sin_addr.s_addr = htonl(bits ? (0xffffffffUL << (32 - bits)) : 0);
            *maskLen = sizeof(*m);
        }
        return 1;
    }
    return 0;
}

// One entry of the list getifaddrs() builds.
typedef struct
{
    struct ifaddrs entry;
    char name[IFNAMSIZ];
    struct sockaddr_storage addr;
    struct sockaddr_storage mask;
    struct ifaddrs *next;
} llIfAddr;

static void freeIfAddrs(struct ifaddrs *head)
{
    while (head)
    {
        llIfAddr *node = (llIfAddr *)head;
        struct ifaddrs *next = head->ifa_next;

        free(node);
        head = next;
    }
}

int getifaddrs(struct ifaddrs **ifap)
{
    IP_ADAPTER_ADDRESSES *list;
    struct ifaddrs *head = NULL, *tail = NULL;

    if (!ifap)
    {
        errno = EINVAL;
        return -1;
    }
    *ifap = NULL;

    list = firstAdapter(AF_UNSPEC);
    if (!list)
    {
        // No adapter at all still leaves the loopback, which the game's
        // network report needs.
        llIfAddr *node = (llIfAddr *)calloc(1, sizeof(*node));
        if (!node)
            return -1;
        snprintf(node->name, sizeof(node->name), "lo");
        node->entry.ifa_name = node->name;
        node->entry.ifa_flags = IFF_UP | IFF_RUNNING | IFF_LOOPBACK;
        ((struct sockaddr_in *)&node->addr)->sin_family = AF_INET;
        ((struct sockaddr_in *)&node->addr)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ((struct sockaddr_in *)&node->mask)->sin_family = AF_INET;
        ((struct sockaddr_in *)&node->mask)->sin_addr.s_addr = htonl(0xff000000UL);
        node->entry.ifa_addr = (struct sockaddr *)&node->addr;
        node->entry.ifa_netmask = (struct sockaddr *)&node->mask;
        *ifap = &node->entry;
        return 0;
    }

    int families[2] = {AF_INET, AF_INET6};
    for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next)
    {
        char name[IFNAMSIZ];

        adapterName(a, name, sizeof(name));
        for (int f = 0; f < 2; f++)
        {
            struct sockaddr_storage addr, mask;
            socklen_t addrLen = 0, maskLen = 0;

            if (!adapterAddress(a, families[f], &addr, &mask, &addrLen, &maskLen))
                continue;
            llIfAddr *node = (llIfAddr *)calloc(1, sizeof(*node));
            if (!node)
            {
                freeIfAddrs(head);
                freeAdapterList(list);
                return -1;
            }
            memcpy(node->name, name, sizeof(node->name));
            node->entry.ifa_name = node->name;
            node->entry.ifa_flags = adapterFlags(a);
            memcpy(&node->addr, &addr, sizeof(addr));
            memcpy(&node->mask, &mask, sizeof(mask));
            node->entry.ifa_addr = (struct sockaddr *)&node->addr;
            if (maskLen)
                node->entry.ifa_netmask = (struct sockaddr *)&node->mask;
            if (tail)
            {
                tail->ifa_next = &node->entry;
                tail = &node->entry;
            }
            else
            {
                head = tail = &node->entry;
            }
        }
    }
    freeAdapterList(list);

    if (!head)
    {
        errno = ENXIO;
        return -1;
    }
    *ifap = head;
    return 0;
}

void freeifaddrs(struct ifaddrs *ifa)
{
    freeIfAddrs(ifa);
}

// -----------------------------------------------------------------------------
// ioctl: the interface requests the Namco network administration makes
// -----------------------------------------------------------------------------

static int interfaceIoctl(int fd, unsigned long request, void *arg)
{
    struct ifreq *ifr = (struct ifreq *)arg;
    IP_ADAPTER_ADDRESSES *list, *a;
    struct sockaddr_storage addr, mask;
    socklen_t addrLen = 0, maskLen = 0;
    int result = -1;

    (void)fd;
    if (!ifr)
    {
        errno = EINVAL;
        return -1;
    }

    list = firstAdapter(AF_UNSPEC);
    if (!list)
    {
        errno = ENODEV;
        return -1;
    }
    a = findAdapter(list, ifr->ifr_name);
    if (!a)
    {
        freeAdapterList(list);
        errno = ENODEV;
        return -1;
    }

    switch (request)
    {
    case SIOCGIFHWADDR:
        if (a->PhysicalAddressLength >= 6)
        {
            memset(&ifr->ifr_hwaddr, 0, sizeof(ifr->ifr_hwaddr));
            ifr->ifr_hwaddr.sa_family = ARPHRD_ETHER;
            memcpy(ifr->ifr_hwaddr.sa_data, a->PhysicalAddress, 6);
            result = 0;
        }
        break;
    case SIOCGIFADDR:
    case SIOCGIFNETMASK:
        if (adapterAddress(a, AF_INET, &addr, &mask, &addrLen, &maskLen))
        {
            memcpy(request == SIOCGIFADDR ? &ifr->ifr_addr : &ifr->ifr_netmask, &addr, addrLen);
            result = 0;
        }
        break;
    case SIOCGIFFLAGS:
        ifr->ifr_flags = (short)adapterFlags(a);
        result = 0;
        break;
    case SIOCGIFMTU:
        ifr->ifr_mtu = (int)a->Mtu;
        result = 0;
        break;
    default:
        break;
    }
    freeAdapterList(list);
    if (result != 0)
        errno = a ? ENOTTY : ENODEV;
    return result;
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    void *arg;

    va_start(args, request);
    arg = va_arg(args, void *);
    va_end(args);

    switch (request)
    {
    case SIOCGIFHWADDR:
    case SIOCGIFADDR:
    case SIOCGIFNETMASK:
    case SIOCGIFFLAGS:
    case SIOCGIFMTU:
        return interfaceIoctl(fd, request, arg);
    default:
        // The evdev, V4L2 and OSS requests: there is no such device here.
        errno = ENOTTY;
        return -1;
    }
}

// -----------------------------------------------------------------------------
// syscall: the calls the loader makes past libc
// -----------------------------------------------------------------------------

long syscall(long number, ...)
{
    va_list args;
    long result = -1;

    va_start(args, number);
    switch (number)
    {
    case SYS_gettid:
        result = (long)GetCurrentThreadId();
        break;
    case SYS_getpid:
        result = (long)_getpid();
        break;
    case SYS_open:
    {
        const char *path = va_arg(args, const char *);
        int flags = va_arg(args, int);
        int mode = va_arg(args, int);
        int oflags = 0;

        if (flags & O_RDWR)
            oflags |= _O_RDWR;
        else if (flags & O_WRONLY)
            oflags |= _O_WRONLY;
        else
            oflags |= _O_RDONLY;
        if (flags & O_CREAT)
            oflags |= _O_CREAT;
        if (flags & O_TRUNC)
            oflags |= _O_TRUNC;
        if (flags & O_APPEND)
            oflags |= _O_APPEND;
        if (flags & O_BINARY)
            oflags |= _O_BINARY;
        else
            oflags |= _O_BINARY; // the settings files are read whole
        result = _open(path, oflags, mode);
        break;
    }
    case SYS_openat:
    {
        int dirfd = va_arg(args, int);
        const char *path = va_arg(args, const char *);
        int flags = va_arg(args, int);
        int mode = va_arg(args, int);
        int oflags = 0;

        (void)dirfd; // AT_FDCWD is the only one used, and is the CWD here
        if (flags & O_RDWR)
            oflags |= _O_RDWR;
        else if (flags & O_WRONLY)
            oflags |= _O_WRONLY;
        else
            oflags |= _O_RDONLY;
        if (flags & O_CREAT)
            oflags |= _O_CREAT;
        if (flags & O_TRUNC)
            oflags |= _O_TRUNC;
        if (flags & O_APPEND)
            oflags |= _O_APPEND;
        oflags |= _O_BINARY;
        result = _open(path, oflags, mode);
        break;
    }
    case SYS_read:
    {
        int fd = va_arg(args, int);
        void *buf = va_arg(args, void *);
        size_t count = va_arg(args, size_t);

        result = _read(fd, buf, (unsigned int)count);
        break;
    }
    case SYS_write:
    {
        int fd = va_arg(args, int);
        const void *buf = va_arg(args, const void *);
        size_t count = va_arg(args, size_t);

        result = _write(fd, buf, (unsigned int)count);
        break;
    }
    case SYS_close:
        result = _close(va_arg(args, int));
        break;
    case SYS_access:
    {
        const char *path = va_arg(args, const char *);
        int mode = va_arg(args, int);

        (void)mode;
        result = _access(path, 0);
        break;
    }
    default:
        errno = ENOSYS;
        result = -1;
        break;
    }
    va_end(args);
    return result;
}

// -----------------------------------------------------------------------------
// statvfs: the free space of a game's drive
// -----------------------------------------------------------------------------

int statvfs(const char *path, struct statvfs *buf)
{
    ULARGE_INTEGER freeBytes, totalBytes, totalFree;
    char root[MAX_PATH];

    if (!path || !buf)
    {
        errno = EINVAL;
        return -1;
    }
    snprintf(root, sizeof(root), "%s", path);
    // GetDiskFreeSpaceEx() wants the volume, not the directory.
    if (root[0] && root[1] == ':' && (root[2] == '\0' || root[2] == '/' || root[2] == '\\'))
        root[3] = '\0';
    if (!GetDiskFreeSpaceExA(root, &freeBytes, &totalBytes, &totalFree))
    {
        errno = ENOENT;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_blocks = (fsblkcnt_t)(totalBytes.QuadPart / 4096);
    buf->f_bfree = (fsblkcnt_t)(totalFree.QuadPart / 4096);
    buf->f_bavail = (fsblkcnt_t)(freeBytes.QuadPart / 4096);
    buf->f_files = 0;
    buf->f_ffree = 0;
    buf->f_favail = 0;
    buf->f_namemax = 255;
    return 0;
}

int fstatvfs(int fd, struct statvfs *buf)
{
    (void)fd;
    return statvfs(".", buf);
}

// -----------------------------------------------------------------------------
// The odds and ends the POSIX headers carry
// -----------------------------------------------------------------------------

int socketpair(int domain, int type, int protocol, int sv[2])
{
    (void)domain;
    (void)type;
    (void)protocol;
    (void)sv;
    errno = EAFNOSUPPORT;
    return -1;
}

int inet_aton(const char *cp, struct in_addr *inp)
{
    unsigned long addr = inet_addr(cp);

    if (addr == INADDR_NONE && strcmp(cp, "255.255.255.255") != 0)
        return 0;
    if (inp)
        inp->s_addr = addr;
    return 1;
}

// setenv() and unsetenv(), which the CRT lacks; getenv() and _putenv_s() are
// what Windows has.
int setenv(const char *name, const char *value, int overwrite)
{
    if (!name || !*name)
    {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && getenv(name))
        return 0;
    return _putenv_s(name, value ? value : "") == 0 ? 0 : -1;
}

int unsetenv(const char *name)
{
    if (!name || !*name)
    {
        errno = EINVAL;
        return -1;
    }
    return _putenv_s(name, "") == 0 ? 0 : -1;
}

// A pipe the code reads and writes as descriptors: the CRT's own.
int pipe(int fds[2])
{
    return _pipe(fds, 4096, _O_BINARY);
}

// strndup(): POSIX 2008, which the CRT of some of the loader's builds does
// not carry (namcoN2Graphics.c calls it). Its own name, reached through the
// posix.h macro, so a CRT that does have one does not clash with it.
char *ll_strndup(const char *s, size_t n)
{
    size_t len;
    char *copy;

    if (!s)
        return NULL;
    for (len = 0; len < n && s[len]; len++)
        ;
    copy = malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

// The reentrant time conversions glibc has: the CRT's are not reentrant, and
// the loader logs from the game's threads.
struct tm *localtime_r(const time_t *timep, struct tm *result)
{
    struct tm *tm = timep ? localtime(timep) : NULL;

    if (!tm)
        return NULL;
    *result = *tm;
    return result;
}

struct tm *gmtime_r(const time_t *timep, struct tm *result)
{
    struct tm *tm = timep ? gmtime(timep) : NULL;

    if (!tm)
        return NULL;
    *result = *tm;
    return result;
}

// lstat(): no symbolic links here, so the file itself.
int lstat(const char *path, struct stat *buf)
{
    return stat(path, buf);
}

int symlink(const char *target, const char *linkpath)
{
    (void)target;
    (void)linkpath;
    errno = EPERM;
    return -1;
}

unsigned int geteuid(void)
{
    return 0;
}

unsigned int getegid(void)
{
    return 0;
}

unsigned int getuid(void)
{
    return 0;
}

// The default thread attributes glibc lets a program set (the emulated
// pthreads take theirs from the caller).
int pthread_getattr_default_np(pthread_attr_t *attr)
{
    if (!attr)
        return EINVAL;
    return pthread_attr_init(attr);
}

int pthread_setattr_default_np(const pthread_attr_t *attr)
{
    (void)attr;
    return 0;
}

// The message calls of the game's IPC: a Unix socket pair, which Windows has
// no equivalent of.
ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
    (void)sockfd;
    (void)msg;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
    (void)sockfd;
    (void)msg;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

// if_nameindex(): the adapters, as Linux lists them.
struct if_nameindex *if_nameindex(void)
{
    IP_ADAPTER_ADDRESSES *list = firstAdapter(AF_UNSPEC);
    struct if_nameindex *entries;
    size_t count = 0, i = 0;

    if (!list)
    {
        errno = ENXIO;
        return NULL;
    }
    for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next)
        count++;
    entries = (struct if_nameindex *)calloc(count + 1, sizeof(*entries));
    if (!entries)
    {
        freeAdapterList(list);
        return NULL;
    }
    for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next)
    {
        char name[IFNAMSIZ];

        adapterName(a, name, sizeof(name));
        entries[i].if_index = a->IfIndex;
        entries[i].if_name = _strdup(name);
        i++;
    }
    freeAdapterList(list);
    entries[i].if_index = 0;
    entries[i].if_name = NULL;
    return entries;
}

void if_freenameindex(struct if_nameindex *ptr)
{
    if (!ptr)
        return;
    for (struct if_nameindex *entry = ptr; entry->if_name; entry++)
        free(entry->if_name);
    free(ptr);
}

// memfd_create(): no anonymous files with a /proc path on Windows; callers
// fall back to having no such file.
int memfd_create(const char *name, unsigned int flags)
{
    (void)name;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

// asprintf() (posix.h maps it here): not every MinGW CRT has it.
int ll_asprintf(char **strp, const char *fmt, ...)
{
    va_list ap, copy;
    int len;

    va_start(ap, fmt);
    va_copy(copy, ap);
    len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (len < 0 || !(*strp = malloc((size_t)len + 1)))
    {
        va_end(ap);
        *strp = NULL;
        return -1;
    }
    vsnprintf(*strp, (size_t)len + 1, fmt, ap);
    va_end(ap);
    return len;
}
