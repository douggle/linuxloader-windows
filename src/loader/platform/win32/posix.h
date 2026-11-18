#ifndef LL_WIN32_POSIX_H
#define LL_WIN32_POSIX_H

// The POSIX calls the arcade sources make that MinGW has under another name,
// or not at all (platform/win32/win32Posix.c has the ones that need code).
// Included where the Linux headers would have declared them; on Linux it is
// empty, as glibc's headers say all of it.
#if defined(_WIN32) || defined(__MINGW32__)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>

#include <direct.h>
#include <io.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

// Linux's mkdir() takes the mode as well; Windows' own takes only the path,
// and the mode is the directory's ACL.
#ifdef mkdir
#undef mkdir
#endif
#define mkdir(path, mode) _mkdir(path)

// The mode bits the code passes to mkdir() and chmod().
#ifndef S_IRWXU
#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001
#endif

#ifndef S_IRUSR
#define S_IRUSR 00400
#define S_IWUSR 00200
#endif

// The open() flags the code sets that the CRT does not have: the CRT opens
// binary by default here, and close-on-exec has no meaning.
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif

// fcntl.h's AT_FDCWD, which the syscall(SYS_openat) call sites pass.
#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    int setenv(const char *name, const char *value, int overwrite);
    int unsetenv(const char *name);
    int pipe(int fds[2]);

    // Implemented in win32Posix.c.
    struct stat;
    int lstat(const char *path, struct stat *buf);
    int symlink(const char *target, const char *linkpath);
    unsigned int geteuid(void);
    int memfd_create(const char *name, unsigned int flags);
    // Our own, so a CRT that has (or inlines) one does not clash with it.
    int ll_asprintf(char **strp, const char *fmt, ...);
#define asprintf ll_asprintf
    char *ll_strndup(const char *s, size_t n);
#define strndup(s, n) ll_strndup(s, n)

    int pthread_getattr_default_np(pthread_attr_t *attr);
    int pthread_setattr_default_np(const pthread_attr_t *attr);

#ifdef __cplusplus
}
#endif

#endif // _WIN32

#endif // LL_WIN32_POSIX_H
