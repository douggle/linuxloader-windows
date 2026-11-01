// Cabinet paths of the PC-based arcade systems (Raw Thrills, Namco ES1 and
// N2, Teamplay, Global VR, Roxor), mapped to the host by the running game's system:
// the loader's file interposers (redirections/filesystemShared.c) and the
// libc path calls below.

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>

#include "gamePath.h"
#include "../rawthrills/rawthrills.h"
#include "../namco/namcoEs1.h"
#include "../namco/namcoN2.h"
#include "../teamplay/teamplay.h"
#include "../globalvr/gvr.h"
#include "../roxor/roxor.h"

const char *gameRedirectPath(const char *path, char *buf, size_t size)
{
    if (!path)
        return path;
    if (isRawThrillsGame())
        return rtRedirectPath(path, buf, size);
    if (isTeamplayGame())
        return teamplayRedirectPath(path, buf, size);
    if (isGvrGame())
        return gvrRedirectPath(path, buf, size);
    if (isRoxorGame())
        return roxorRedirectPath(path, buf, size);
    if (isNamcoEs1Game())
        return namcoEs1RedirectPath(path, buf, size);
    if (isNamcoN2Game())
        return namcoN2RedirectPath(path, buf, size);
    return path;
}

// The libc call each wrapper below forwards to. On Linux it is the next
// definition (glibc's). On Windows dlsym() goes through the ELF loader's
// symbol table, which can be called before the bridges are registered and
// then hands back a crash stub that the wrapper would keep: the C runtime's
// own call is used instead.
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define NEXT_ACCESS() access_crt
#define NEXT_RENAME() rename_crt
#define NEXT_UNLINK() _unlink
#define NEXT_CHDIR() _chdir
// rename() itself is the wrapper below; MoveFileEx is the CRT's rename with
// replace, as POSIX's.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <errno.h>
// No execute bit on Windows: X_OK is "exists" (the CRT rejects it).
static int access_crt(const char *path, int mode)
{
    return _access(path, mode & 6);
}

static int rename_crt(const char *from, const char *to)
{
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        return 0;
    errno = GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
    return -1;
}
#else
#define NEXT_ACCESS() dlsym(RTLD_NEXT, "access")
#define NEXT_RENAME() dlsym(RTLD_NEXT, "rename")
#define NEXT_UNLINK() dlsym(RTLD_NEXT, "unlink")
#define NEXT_CHDIR() dlsym(RTLD_NEXT, "chdir")
#endif

#define REDIRECT(path)                                                                                                \
    char redirectBuf[PATH_MAX];                                                                                       \
    path = gameRedirectPath(path, redirectBuf, sizeof(redirectBuf))

int access(const char *path, int mode)
{
    static int (*real)(const char *, int);
    if (!real)
        real = NEXT_ACCESS();
    REDIRECT(path);
    return real(path, mode);
}

int rename(const char *from, const char *to)
{
    static int (*real)(const char *, const char *);
    char toBuf[PATH_MAX];
    if (!real)
        real = NEXT_RENAME();
    REDIRECT(from);
    return real(from, gameRedirectPath(to, toBuf, sizeof(toBuf)));
}

int unlink(const char *path)
{
    static int (*real)(const char *);
    if (!real)
        real = NEXT_UNLINK();
    REDIRECT(path);
    return real(path);
}

int chdir(const char *path)
{
    static int (*real)(const char *);
    if (!real)
        real = NEXT_CHDIR();
    REDIRECT(path);
    return real(path);
}

// Old glibc stat entry points: only versioned compat symbols remain.
static void *nextCompat(const char *name)
{
    void *p = dlvsym(RTLD_NEXT, name, "GLIBC_2.0");
    return p ? p : dlsym(RTLD_NEXT, name);
}

int __xstat(int ver, const char *path, struct stat *st)
{
    static int (*real)(int, const char *, struct stat *);
    if (!real)
        real = nextCompat("__xstat");
    REDIRECT(path);
    return real(ver, path, st);
}

int __lxstat(int ver, const char *path, struct stat *st)
{
    static int (*real)(int, const char *, struct stat *);
    if (!real)
        real = nextCompat("__lxstat");
    REDIRECT(path);
    return real(ver, path, st);
}

int __lxstat64(int ver, const char *path, struct stat64 *st)
{
    static int (*real)(int, const char *, struct stat64 *);
    if (!real)
        real = nextCompat("__lxstat64");
    REDIRECT(path);
    return real(ver, path, st);
}
