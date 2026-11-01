#ifndef LL_WIN32_SYS_SYSCALL_H
#define LL_WIN32_SYS_SYSCALL_H

// <sys/syscall.h> for MinGW.  The arcade support reaches past libc for a
// thread id, and (rtSettings.c) for open/read/write/close: syscall() answers
// the ones that have a Windows equivalent, and fails the rest.

#include <sys/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

    long syscall(long number, ...);

#ifdef __cplusplus
}
#endif

// i386's call numbers: the values the sources name, and syscall() switches on.
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_lseek 19
#define SYS_getpid 20
#define SYS_access 33
#define SYS_gettid 224
#define SYS_getdents64 220
#define SYS_openat 295
#define SYS_readlinkat 305
#define SYS_getrandom 355

#define SYS_futex 240
#define SYS_mmap 90
#define SYS_munmap 91
#define SYS_clock_gettime 265
#define SYS_nanosleep 162

#endif // LL_WIN32_SYS_SYSCALL_H
