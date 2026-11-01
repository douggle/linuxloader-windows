#ifndef LL_WIN32_SYS_MMAN_H
#define LL_WIN32_SYS_MMAN_H

// <sys/mman.h> for MinGW: mmap() & co. over VirtualAlloc, for the code the
// loader maps by hand (trampolines, the game's images).

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define PROT_GROWSDOWN 0x01000000
#define PROT_GROWSUP 0x02000000

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_SHARED_VALIDATE 0x03
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_32BIT 0x40
#define MAP_GROWSDOWN 0x00100
#define MAP_DENYWRITE 0x00800
#define MAP_EXECUTABLE 0x01000
#define MAP_LOCKED 0x02000
#define MAP_NORESERVE 0x04000
#define MAP_POPULATE 0x08000
#define MAP_NONBLOCK 0x10000
#define MAP_STACK 0x20000
#define MAP_HUGETLB 0x40000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED ((void *)-1)

#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4

#define MCL_CURRENT 1
#define MCL_FUTURE 2

#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4

    void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
    int munmap(void *addr, size_t length);
    int mprotect(void *addr, size_t len, int prot);
    int msync(void *addr, size_t length, int flags);
    int madvise(void *addr, size_t length, int advice);
    int mlock(const void *addr, size_t len);
    int munlock(const void *addr, size_t len);

#ifdef __cplusplus
}
#endif

#endif // LL_WIN32_SYS_MMAN_H
