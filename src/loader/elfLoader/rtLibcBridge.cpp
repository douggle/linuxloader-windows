// glibc entry points the Raw Thrills games (Cruis'n Blast) import that the
// other bridges do not answer: the _FORTIFY_SOURCE checked variants, the
// C99 scanf names, a few POSIX calls with a Windows equivalent, and the
// Linux-only services (udev, utmp, mount table, timerfd, iconv) answered as
// a system that has none of them. Everything is cdecl with the Linux layouts
// (4-byte wchar_t, glibc's struct tm and struct sysinfo).

#if defined(_WIN32) || defined(__MINGW32__)

#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <windows.h>

#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <direct.h>
#include <io.h>
#include <string>

#include <glad/gl.h>

#include "symbolResolver.hpp"
#include "memoryManager.hpp"
#include "rtLibcBridge.hpp"
#include "../common/sdl3Exports.h"
#include "../log/log.h"
#include "../platform/win32/dlfcn.h"
#include "../platform/win32/sys/mman.h"

// Only where no other bridge answers already (this one registers last).
#define MAP(name, func)                                                                                 \
    do                                                                                                  \
    {                                                                                                   \
        if (!SymbolResolver::GetInstance().GetVTable(name))                                             \
            SymbolResolver::GetInstance().RegisterVTable(name, reinterpret_cast<void *>(func));        \
    } while (0)

// A bridge of another file, by its Linux name, at call time.
static void *bridgeOf(const char *name)
{
    std::string module;
    return SymbolResolver::GetInstance().TryResolveSymbol(name, &module);
}

typedef uint32_t lwchar; // Linux wchar_t

extern "C"
{
    // ---------------------------------------------------------------- checked
    static int rt_printf_chk(int flag, const char *fmt, ...)
    {
        (void)flag;
        va_list ap;
        va_start(ap, fmt);
        int r = vprintf(fmt, ap);
        va_end(ap);
        return r;
    }

    static int rt_fprintf_chk(FILE *fp, int flag, const char *fmt, ...)
    {
        (void)flag;
        va_list ap;
        va_start(ap, fmt);
        int r = vfprintf(fp ? fp : stdout, fmt, ap);
        va_end(ap);
        return r;
    }

    static int rt_vsnprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list ap)
    {
        (void)flag;
        (void)slen;
        return vsnprintf(s, maxlen, fmt, ap);
    }

    static int rt_asprintf_chk(char **strp, int flag, const char *fmt, ...)
    {
        (void)flag;
        va_list ap, copy;
        va_start(ap, fmt);
        va_copy(copy, ap);
        int len = vsnprintf(NULL, 0, fmt, copy);
        va_end(copy);
        // Freed by the game with free(): the game's allocator.
        if (len < 0 || !(*strp = (char *)MemoryManager::customMalloc((size_t)len + 1)))
        {
            va_end(ap);
            return -1;
        }
        vsnprintf(*strp, (size_t)len + 1, fmt, ap);
        va_end(ap);
        return len;
    }

    static void *rt_memcpy_chk(void *d, const void *s, size_t n, size_t dl)
    {
        (void)dl;
        return memcpy(d, s, n);
    }
    static void *rt_memset_chk(void *d, int c, size_t n, size_t dl)
    {
        (void)dl;
        return memset(d, c, n);
    }
    static char *rt_strcpy_chk(char *d, const char *s, size_t dl)
    {
        (void)dl;
        return strcpy(d, s);
    }
    static char *rt_strcat_chk(char *d, const char *s, size_t dl)
    {
        (void)dl;
        return strcat(d, s);
    }
    static char *rt_strncpy_chk(char *d, const char *s, size_t n, size_t dl)
    {
        (void)dl;
        return strncpy(d, s, n);
    }
    static char *rt_strncat_chk(char *d, const char *s, size_t n, size_t dl)
    {
        (void)dl;
        return strncat(d, s, n);
    }
    static char *rt_stpcpy(char *d, const char *s)
    {
        size_t n = strlen(s);
        memcpy(d, s, n + 1);
        return d + n;
    }
    static char *rt_stpcpy_chk(char *d, const char *s, size_t dl)
    {
        (void)dl;
        return rt_stpcpy(d, s);
    }
    static size_t rt_fread_chk(void *p, size_t plen, size_t size, size_t n, FILE *fp)
    {
        (void)plen;
        return fread(p, size, n, fp);
    }
    static long rt_fdelt_chk(long d)
    {
        return d / 32;
    }
    static void rt_stack_chk_fail(void)
    {
        log_fatal("Stack smashing detected (__stack_chk_fail) in the game");
        abort();
    }
    static int rt_open_2(const char *path, int flags)
    {
        int (*open)(const char *, int, ...) = (int (*)(const char *, int, ...))bridgeOf("open");
        return open ? open(path, flags, 0) : -1;
    }

    // ----------------------------------------------------------------- scanf
    static int rt_isoc99_sscanf(const char *s, const char *fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        int r = vsscanf(s, fmt, ap);
        va_end(ap);
        return r;
    }
    static int rt_isoc99_fscanf(FILE *fp, const char *fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        int r = vfscanf(fp, fmt, ap);
        va_end(ap);
        return r;
    }
    static long double rt_strtold_internal(const char *s, char **end, int group)
    {
        (void)group;
        return strtold(s, end);
    }

    // ------------------------------------------------------------------ math
    static float rt_ceilf(float x) { return ceilf(x); }
    static float rt_floorf(float x) { return floorf(x); }
    static double rt_ldexp(double x, int e) { return ldexp(x, e); }
    static void rt_sincos(double x, double *s, double *c)
    {
        *s = sin(x);
        *c = cos(x);
    }
    static void rt_sincosf(float x, float *s, float *c)
    {
        *s = sinf(x);
        *c = cosf(x);
    }
    static int rt_finitef(float x) { return std::isfinite(x) ? 1 : 0; }

    // ------------------------------------------------------- numbers, strings
    static long rt_atol(const char *s) { return atol(s); }
    static unsigned long rt_strtoul(const char *s, char **e, int b) { return strtoul(s, e, b); }
    static unsigned long long rt_strtoull(const char *s, char **e, int b) { return strtoull(s, e, b); }
    static char *rt_strerror_r(int e, char *buf, size_t n)
    {
        // GNU strerror_r: the message (here copied into buf).
        if (buf && n)
        {
            strncpy(buf, strerror(e), n - 1);
            buf[n - 1] = '\0';
        }
        return buf;
    }
    static int rt_putenv(char *s)
    {
        const char *eq = s ? strchr(s, '=') : NULL;
        if (eq)
        {
            std::string name(s, eq - s);
            if (llIsLinuxOnlySdlDriver(name.c_str(), eq + 1))
                return 0;
        }
        return _putenv(s);
    }
    static int rt_rmdir(const char *p) { return _rmdir(p); }

    // ------------------------------------------------------------------ time
    static long rt_clock(void)
    {
        // CLOCKS_PER_SEC is 1000000 on Linux, 1000 here.
        return (long)clock() * (1000000L / CLOCKS_PER_SEC);
    }
    struct linux_timespec
    {
        int32_t tv_sec;
        int32_t tv_nsec;
    };
    static int rt_clock_getres(int clk, linux_timespec *ts)
    {
        (void)clk;
        if (ts)
        {
            ts->tv_sec = 0;
            ts->tv_nsec = 1000;
        }
        return 0;
    }
    // glibc's struct tm: the C fields, then tm_gmtoff and tm_zone.
    struct linux_tm
    {
        int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
        long tm_gmtoff;
        const char *tm_zone;
    };
    static linux_tm *rt_gmtime(const int32_t *t)
    {
        static linux_tm out;
        time_t tt = (time_t)*t;
        struct tm *g = gmtime(&tt);
        if (!g)
            return NULL;
        memcpy(&out, g, 9 * sizeof(int));
        out.tm_gmtoff = 0;
        out.tm_zone = "GMT";
        return &out;
    }
    static char *rt_asctime(const linux_tm *t)
    {
        struct tm w;
        memcpy(&w, t, 9 * sizeof(int));
        return asctime(&w);
    }
    static char *rt_asctime_r(const linux_tm *t, char *buf)
    {
        char *s = rt_asctime(t);
        if (!s)
            return NULL;
        strcpy(buf, s);
        return buf;
    }

    // -------------------------------------------------------- process, user
    static int rt_getppid(void) { return 1; }
    static unsigned int rt_getgid(void) { return 0; }
    static int rt_getrlimit(int resource, uint32_t *rl)
    {
        (void)resource;
        rl[0] = 8u << 20;   // rlim_cur
        rl[1] = 0xffffffffu; // rlim_max: RLIM_INFINITY
        return 0;
    }
    static long rt_sysconf(int name)
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        switch (name)
        {
        case 2: // _SC_CLK_TCK
            return 100;
        case 30: // _SC_PAGESIZE
            return (long)si.dwPageSize;
        case 83: // _SC_NPROCESSORS_CONF
        case 84: // _SC_NPROCESSORS_ONLN
            return (long)si.dwNumberOfProcessors;
        case 85: // _SC_PHYS_PAGES
        case 86: // _SC_AVPHYS_PAGES
        {
            MEMORYSTATUSEX ms;
            ms.dwLength = sizeof(ms);
            GlobalMemoryStatusEx(&ms);
            unsigned long long bytes = name == 85 ? ms.ullTotalPhys : ms.ullAvailPhys;
            return (long)(bytes / si.dwPageSize > 0x7fffffff ? 0x7fffffff : bytes / si.dwPageSize);
        }
        case 0: // _SC_ARG_MAX
            return 131072;
        case 4: // _SC_OPEN_MAX
            return 1024;
        default:
            errno = EINVAL;
            return -1;
        }
    }
    struct linux_sysinfo
    {
        int32_t uptime;
        uint32_t loads[3];
        uint32_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
        uint16_t procs, pad;
        uint32_t totalhigh, freehigh, mem_unit;
        char f[8];
    };
    static int rt_sysinfo(linux_sysinfo *info)
    {
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        memset(info, 0, sizeof(*info));
        info->uptime = (int32_t)(GetTickCount64() / 1000);
        info->mem_unit = 4096;
        info->totalram = (uint32_t)(ms.ullTotalPhys / 4096);
        info->freeram = (uint32_t)(ms.ullAvailPhys / 4096);
        info->totalswap = (uint32_t)(ms.ullTotalPageFile / 4096);
        info->freeswap = (uint32_t)(ms.ullAvailPageFile / 4096);
        info->procs = 1;
        return 0;
    }
    static long rt_syscall(long n, ...)
    {
        if (n == 224) // SYS_gettid
            return (long)GetCurrentThreadId();
        errno = ENOSYS;
        return -1;
    }
    static int rt_getpwuid_r(unsigned int uid, void *pwd, char *buf, size_t n, void **result)
    {
        (void)uid;
        (void)pwd;
        (void)buf;
        (void)n;
        *result = NULL;
        return 0;
    }
    static int rt_getlogin_r(char *buf, size_t n)
    {
        if (n < 7)
            return ERANGE;
        strcpy(buf, "arcade");
        return 0;
    }
    static int rt_ttyname_r(int fd, char *buf, size_t n)
    {
        (void)fd;
        (void)buf;
        (void)n;
        return 25; // ENOTTY
    }
    static char *rt_ttyname(int fd)
    {
        (void)fd;
        return NULL;
    }
    static int rt_execvp(const char *file, char *const argv[])
    {
        (void)argv;
        log_warn("Game asked to run %s (execvp): not supported", file ? file : "?");
        errno = ENOENT;
        return -1;
    }
    static int rt_ioperm(unsigned long from, unsigned long num, int on)
    {
        (void)from;
        (void)num;
        (void)on;
        return 0; // the parallel port board is the loader's (cbStubs)
    }
    static int rt_mprotect(void *addr, size_t len, int prot)
    {
        return mprotect(addr, len, prot);
    }
    static int rt_pipe(int fds[2])
    {
        return _pipe(fds, 65536, 0x8000 /* _O_BINARY */);
    }

    // --------------------------------------------------------------- signals
    static int rt_sigdelset(uint32_t *set, int sig)
    {
        if (sig < 1 || sig > 1024)
        {
            errno = EINVAL;
            return -1;
        }
        set[(sig - 1) / 32] &= ~(1u << ((sig - 1) % 32));
        return 0;
    }
    static int rt_sigprocmask(int how, const void *set, void *old)
    {
        (void)how;
        (void)set;
        if (old)
            memset(old, 0, 128);
        return 0;
    }
    static int rt_pthread_kill(uint32_t thread, int sig)
    {
        (void)thread;
        (void)sig;
        return 0;
    }
    static int rt_libc_current_sigrtmin(void) { return 34; }
    static void *rt_sysv_signal(int sig, void *handler)
    {
        void *(*sig_)(int, void *) = (void *(*)(int, void *))bridgeOf("signal");
        return sig_ ? sig_(sig, handler) : NULL;
    }
    static int rt_pthread_key_create(void *key, void (*dtor)(void *))
    {
        int (*create)(void *, void (*)(void *)) = (int (*)(void *, void (*)(void *)))bridgeOf("pthread_key_create");
        return create ? create(key, dtor) : 11; // EAGAIN
    }
    static int *rt_h_errno_location(void)
    {
        static int h;
        return &h;
    }

    // ---------------------------------------------------- none of these here
    static void rt_noop(void) {}
    static int rt_ret0(void) { return 0; }
    static int rt_retm1(void)
    {
        errno = ENOSYS;
        return -1;
    }
    static void *rt_retnull(void) { return NULL; }
    static int rt_getutent_r(void *ubuf, void **res)
    {
        (void)ubuf;
        *res = NULL;
        errno = ENOENT;
        return -1;
    }
    static int rt_endmntent(void *f)
    {
        (void)f;
        return 1;
    }
    static int rt_backtrace(void **buf, int n)
    {
        (void)buf;
        (void)n;
        return 0;
    }

    // Network: name lookups fail (no online features); the rest answers on
    // the loader's sockets.
    static int rt_getaddrinfo(const char *node, const char *service, const void *hints, void **res)
    {
        (void)node;
        (void)service;
        (void)hints;
        *res = NULL;
        return -2; // EAI_NONAME
    }
    static const char *rt_inet_ntop(int af, const void *src, char *dst, uint32_t size)
    {
        if (af != 2 || size < 16) // AF_INET
            return NULL;
        const unsigned char *a = (const unsigned char *)src;
        snprintf(dst, size, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
        return dst;
    }

    // iconv: no conversions available.
    static void *rt_iconv_open(const char *to, const char *from)
    {
        (void)to;
        (void)from;
        errno = EINVAL;
        return (void *)-1;
    }
    static size_t rt_iconv(void *cd, char **in, size_t *inleft, char **out, size_t *outleft)
    {
        (void)cd;
        (void)in;
        (void)inleft;
        (void)out;
        (void)outleft;
        errno = EILSEQ;
        return (size_t)-1;
    }

    // udev: a system without devices. The handles are distinct non-NULL
    // tokens (the game may not check them), the lists are empty.
    static char udevToken[4];
    static void *rt_udev_handle(void) { return udevToken; }

    // ---------------------------------------- wide characters (4-byte wchar_t)
    static int rt_wcscasecmp(const lwchar *a, const lwchar *b)
    {
        for (;; a++, b++)
        {
            lwchar x = *a < 128 ? (lwchar)tolower((int)*a) : *a;
            lwchar y = *b < 128 ? (lwchar)tolower((int)*b) : *b;
            if (x != y || !x)
                return (int)x - (int)y;
        }
    }
    static lwchar *rt_wcscpy_chk(lwchar *d, const lwchar *s, size_t dl)
    {
        (void)dl;
        lwchar *r = d;
        while ((*d++ = *s++))
            ;
        return r;
    }
    static void narrowW(const lwchar *s, char *out, size_t n)
    {
        size_t i = 0;
        for (; s && s[i] && i + 1 < n; i++)
            out[i] = s[i] < 128 ? (char)s[i] : '?';
        out[i] = '\0';
    }
    static long long rt_wcstoll(const lwchar *s, lwchar **end, int base)
    {
        char buf[128], *e;
        narrowW(s, buf, sizeof(buf));
        long long v = strtoll(buf, &e, base);
        if (end)
            *end = (lwchar *)(s + (e - buf));
        return v;
    }
    static unsigned long long rt_wcstoull(const lwchar *s, lwchar **end, int base)
    {
        char buf[128], *e;
        narrowW(s, buf, sizeof(buf));
        unsigned long long v = strtoull(buf, &e, base);
        if (end)
            *end = (lwchar *)(s + (e - buf));
        return v;
    }
    // Big Buck HD Wild.
    static unsigned long rt_wcstoul(const lwchar *s, lwchar **end, int base)
    {
        char buf[128], *e;
        narrowW(s, buf, sizeof(buf));
        unsigned long v = strtoul(buf, &e, base);
        if (end)
            *end = (lwchar *)(s + (e - buf));
        return v;
    }
    static lwchar *rt_wcstok(lwchar *s, const lwchar *delim, lwchar **save)
    {
        auto isDelim = [&](lwchar c) {
            for (const lwchar *d = delim; *d; d++)
                if (*d == c)
                    return true;
            return false;
        };
        if (!s)
            s = *save;
        while (*s && isDelim(*s))
            s++;
        if (!*s)
        {
            *save = s;
            return NULL;
        }
        lwchar *tok = s;
        while (*s && !isDelim(*s))
            s++;
        if (*s)
            *s++ = 0;
        *save = s;
        return tok;
    }
    // Formatted wide output: the format is narrowed and the result widened
    // again (UTF-8 in between). Not one call to the narrow vsnprintf: in a
    // wide format %ls (and %S) is a wide string, which the host's printf reads
    // as its own 2-byte wchar_t - a 4-byte Linux string "JOE" ended after its
    // first letter (Cruis'n Blast's leaderboard and plates showed initials).
    // Each conversion is formatted on its own, the wide strings and
    // characters converted first.
    static size_t putUtf8(char *out, size_t size, size_t at, lwchar c)
    {
        char b[4];
        size_t len;
        if (c < 0x80)
            b[0] = (char)c, len = 1;
        else if (c < 0x800)
            b[0] = (char)(0xc0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3f)), len = 2;
        else if (c < 0x10000)
            b[0] = (char)(0xe0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 0x3f)), b[2] = (char)(0x80 | (c & 0x3f)), len = 3;
        else
            b[0] = (char)(0xf0 | (c >> 18 & 7)), b[1] = (char)(0x80 | (c >> 12 & 0x3f)),
            b[2] = (char)(0x80 | (c >> 6 & 0x3f)), b[3] = (char)(0x80 | (c & 0x3f)), len = 4;
        for (size_t i = 0; i < len && at + 1 < size; i++)
            out[at++] = b[i];
        return at;
    }
    static void utf8W(const lwchar *s, char *out, size_t size)
    {
        size_t at = 0;
        for (; s && *s && at + 1 < size; s++)
            at = putUtf8(out, size, at, *s);
        out[at] = '\0';
    }
    // The narrow text of a wide format with its arguments, in out (size):
    // the length it has in full, as snprintf.
    static int formatW(char *out, size_t size, const lwchar *wfmt, va_list ap)
    {
        char fmt[512], spec[64], piece[2048], wide[1024];
        size_t at = 0;
        int total = 0;

        utf8W(wfmt, fmt, sizeof(fmt));
        for (const char *p = fmt; *p;)
        {
            if (*p != '%' || p[1] == '%')
            {
                int c = *p == '%' ? (p += 2, '%') : *p++;
                if (at + 1 < size)
                    out[at++] = (char)c;
                total++;
                continue;
            }
            // %[flags][width][.precision][length]conversion
            const char *start = p++;
            int width = 0, precision = 0, hasWidth = 0, hasPrecision = 0;
            while (strchr("-+ #0", *p) && *p)
                p++;
            if (*p == '*')
                width = va_arg(ap, int), hasWidth = 1, p++;
            else
                while (*p >= '0' && *p <= '9')
                    p++;
            if (*p == '.')
            {
                p++;
                if (*p == '*')
                    precision = va_arg(ap, int), hasPrecision = 1, p++;
                else
                    while (*p >= '0' && *p <= '9')
                        p++;
            }
            int lengthL = 0, lengthH = 0, lengthBig = 0, length64 = 0;
            for (;; p++)
            {
                if (*p == 'l')
                    lengthL++;
                else if (*p == 'h')
                    lengthH++;
                else if (*p == 'L' || *p == 'q')
                    lengthBig = 1;
                else if (*p == 'j')
                    length64 = 1;
                else if (*p == 'z' || *p == 't')
                    ;
                else
                    break;
            }
            char conv = *p ? *p++ : '\0';
            // The spec for the narrow snprintf: flags, width and precision as
            // written (a * stays one, its value passed), a narrow conversion.
            size_t specLen = 0;
            for (const char *q = start; q < p && specLen + 4 < sizeof(spec); q++)
                if (!strchr("lhLqjzt", *q) || q == p - 1)
                    spec[specLen++] = *q;
            spec[specLen] = '\0';
            char *convAt = spec + specLen - 1;
            int n = 0;
#define PIECE(...)                                                                                                     \
    (hasWidth && hasPrecision ? snprintf(piece, sizeof(piece), spec, width, precision, __VA_ARGS__)                   \
     : hasWidth               ? snprintf(piece, sizeof(piece), spec, width, __VA_ARGS__)                              \
     : hasPrecision           ? snprintf(piece, sizeof(piece), spec, precision, __VA_ARGS__)                          \
                              : snprintf(piece, sizeof(piece), spec, __VA_ARGS__))
            switch (conv)
            {
            case 's':
            case 'S':
                // In a wide format: %s and %S are wide strings unless %hs.
                *convAt = 's';
                if (lengthH)
                    n = PIECE(va_arg(ap, const char *));
                else
                {
                    const lwchar *ws = va_arg(ap, const lwchar *);
                    utf8W(ws, wide, sizeof(wide));
                    n = PIECE(ws ? wide : "(null)");
                }
                break;
            case 'c':
            case 'C':
                *convAt = 's';
                wide[putUtf8(wide, sizeof(wide), 0, (lwchar)va_arg(ap, unsigned int))] = '\0';
                n = PIECE(wide);
                break;
            case 'd':
            case 'i':
            case 'o':
            case 'u':
            case 'x':
            case 'X':
                if (lengthL >= 2 || length64)
                {
                    // the 64-bit spec: insert ll before the conversion
                    memmove(convAt + 2, convAt, 2);
                    convAt[0] = convAt[1] = 'l';
                    n = PIECE(va_arg(ap, long long));
                }
                else
                    n = PIECE(va_arg(ap, int));
                break;
            case 'e':
            case 'E':
            case 'f':
            case 'F':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
                n = lengthBig ? PIECE((double)va_arg(ap, long double)) : PIECE(va_arg(ap, double));
                break;
            case 'p':
                n = PIECE(va_arg(ap, void *));
                break;
            case 'n':
                *va_arg(ap, int *) = total;
                break;
            default:
                // Unknown: written as it stands.
                n = snprintf(piece, sizeof(piece), "%.*s", (int)(p - start), start);
                break;
            }
#undef PIECE
            if (n > 0)
            {
                for (int i = 0; i < n && piece[i]; i++)
                    if (at + 1 < size)
                        out[at++] = piece[i];
                total += n;
            }
        }
        if (size)
            out[at < size ? at : size - 1] = '\0';
        return total;
    }
    // UTF-8 text into a Linux wide string of n characters (its NUL included):
    // the characters written.
    static size_t widenUtf8(const char *in, lwchar *s, size_t n)
    {
        size_t i = 0;
        const unsigned char *u = (const unsigned char *)in;
        while (*u && i + 1 < n)
        {
            lwchar c = *u++;
            int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
            c &= more == 3 ? 7 : more == 2 ? 0xf : more == 1 ? 0x1f : 0x7f;
            for (; more && (*u & 0xc0) == 0x80; more--)
                c = c << 6 | (*u++ & 0x3f);
            s[i++] = c;
        }
        if (n)
            s[i] = 0;
        return i;
    }
    static int rt_vswprintf_l(lwchar *s, size_t n, const lwchar *fmt, va_list ap)
    {
        char out[2048];
        int r = formatW(out, sizeof(out), fmt, ap);
        size_t len = widenUtf8(out, s, n);
        // swprintf fails when the text does not fit, unlike snprintf.
        return r < 0 || len < strlen(out) ? -1 : (int)len;
    }
    static int rt_swprintf_chk(lwchar *s, size_t n, int flag, size_t sl, const lwchar *fmt, ...)
    {
        (void)flag;
        (void)sl;
        va_list ap;
        va_start(ap, fmt);
        int r = rt_vswprintf_l(s, n, fmt, ap);
        va_end(ap);
        return r;
    }
    static int rt_vfwprintf(FILE *fp, const lwchar *fmt, va_list ap)
    {
        char out[2048];
        int r = formatW(out, sizeof(out), fmt, ap);
        if (r > 0)
            fputs(out, fp ? fp : stdout);
        return r;
    }
    static int rt_fwprintf(FILE *fp, const lwchar *fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        int r = rt_vfwprintf(fp, fmt, ap);
        va_end(ap);
        return r;
    }
    static int rt_fwprintf_chk(FILE *fp, int flag, const lwchar *fmt, ...)
    {
        (void)flag;
        va_list ap;
        va_start(ap, fmt);
        int r = rt_vfwprintf(fp, fmt, ap);
        va_end(ap);
        return r;
    }
    static int rt_swscanf(const lwchar *s, const lwchar *fmt, ...)
    {
        char ns[512], nfmt[256];
        narrowW(s, ns, sizeof(ns));
        narrowW(fmt, nfmt, sizeof(nfmt));
        va_list ap;
        va_start(ap, fmt);
        int r = vsscanf(ns, nfmt, ap);
        va_end(ap);
        return r;
    }
    static int rt_fsetpos(FILE *fp, const fpos_t *pos) { return fsetpos(fp, pos); }
    static int rt_fgetpos(FILE *fp, fpos_t *pos) { return fgetpos(fp, pos); }

    static int rt_sprintf_chk(char *s, int flag, size_t slen, const char *fmt, ...)
    {
        (void)flag;
        (void)slen;
        va_list ap;
        va_start(ap, fmt);
        int r = vsprintf(s, fmt, ap);
        va_end(ap);
        return r;
    }
    static int rt_snprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...)
    {
        (void)flag;
        (void)slen;
        va_list ap;
        va_start(ap, fmt);
        int r = vsnprintf(s, maxlen, fmt, ap);
        va_end(ap);
        return r;
    }
    static int rt_vsprintf_chk(char *s, int flag, size_t slen, const char *fmt, va_list ap)
    {
        (void)flag;
        (void)slen;
        return vsprintf(s, fmt, ap);
    }
    static long long rt_strtoll(const char *s, char **e, int b) { return strtoll(s, e, b); }
    static char *rt_strtok_r(char *s, const char *delim, char **save)
    {
        if (!s)
            s = *save;
        s += strspn(s, delim);
        if (!*s)
        {
            *save = s;
            return NULL;
        }
        char *tok = s;
        s += strcspn(s, delim);
        if (*s)
            *s++ = '\0';
        *save = s;
        return tok;
    }
    // struct utsname: six 65-byte fields.
    static int rt_uname(char *u)
    {
        static const char *const fields[6] = {"Linux", "arcade", "2.6.32", "#1 SMP", "i686", "(none)"};
        for (int i = 0; i < 6; i++)
        {
            memset(u + i * 65, 0, 65);
            strcpy(u + i * 65, fields[i]);
        }
        return 0;
    }
    static lwchar *rt_wcscat(lwchar *d, const lwchar *s)
    {
        lwchar *r = d;
        while (*d)
            d++;
        while ((*d++ = *s++))
            ;
        return r;
    }
    static lwchar *rt_wcsdup(const lwchar *s)
    {
        size_t n = 0;
        while (s[n])
            n++;
        lwchar *d = (lwchar *)MemoryManager::customMalloc((n + 1) * sizeof(lwchar));
        if (d)
            memcpy(d, s, (n + 1) * sizeof(lwchar));
        return d;
    }
    static int rt_pthread_attr_setinheritsched(void *attr, int inherit)
    {
        (void)attr;
        (void)inherit;
        return 0;
    }
    // On the driver: the level, then its mipmaps made by the GPU. Windows'
    // glu32.dll is GLU 1.2, which knows no GL_BGRA and the other formats
    // Linux's GLU takes, and left Big Buck HD Wild's animals with no texture:
    // black silhouettes. glu32 is the fallback for a driver without
    // glGenerateMipmap.
    static int rt_gluBuild2DMipmaps(unsigned target, int components, int w, int h, unsigned format, unsigned type,
                                    const void *data)
    {
        if (glad_glTexImage2D && glad_glGenerateMipmap && w > 0 && h > 0)
        {
            while (glad_glGetError() != GL_NO_ERROR)
                ;
            glad_glTexImage2D(target, 0, components, w, h, 0, format, type, data);
            glad_glGenerateMipmap(target);
            GLenum error = glad_glGetError();
            if (error == GL_NO_ERROR)
                return 0;
            log_warn("gluBuild2DMipmaps(%#x, %d, %dx%d, %#x, %#x): GL error %#x", target, components, w, h, format, type,
                     error);
            return (int)error;
        }
        typedef int(__stdcall * Fn)(unsigned, int, int, int, unsigned, unsigned, const void *);
        static Fn fn;
        if (!fn)
        {
            HMODULE glu = LoadLibraryA("glu32.dll");
            fn = glu ? (Fn)(void *)GetProcAddress(glu, "gluBuild2DMipmaps") : NULL;
        }
        return fn ? fn(target, components, w, h, format, type, data) : 100900; // GLU_INVALID_ENUM
    }

    // ------------------------------------------- Big Buck HD Wild's imports
    static size_t rt_strnlen(const char *s, size_t n)
    {
        const char *end = (const char *)memchr(s, 0, n);
        return end ? (size_t)(end - s) : n;
    }
    static lwchar *rt_wcsncat(lwchar *d, const lwchar *s, size_t n)
    {
        lwchar *r = d;
        while (*d)
            d++;
        while (n-- && *s)
            *d++ = *s++;
        *d = 0;
        return r;
    }
    static void rt_bcopy(const void *src, void *dst, size_t n) { memmove(dst, src, n); }
    static long long rt_atoll(const char *s) { return _atoi64(s); }
    static int rt_iswalpha(lwchar c) { return c <= 0xffff ? iswalpha((wint_t)c) : 0; }
    static double rt_floor(double x) { return floor(x); }
    static double rt_ceil(double x) { return ceil(x); }
    static double rt_trunc(double x) { return trunc(x); }
    static long rt_lrint(double x) { return lrint(x); }
    // glibc's FP_NAN, FP_INFINITE, FP_ZERO, FP_SUBNORMAL, FP_NORMAL: 0 to 4.
    static int rt_fpclassifyf(float x)
    {
        uint32_t bits;
        memcpy(&bits, &x, sizeof(bits));
        uint32_t exponent = bits >> 23 & 0xff, mantissa = bits & 0x7fffff;
        if (exponent == 0xff)
            return mantissa ? 0 : 1;
        if (exponent == 0)
            return mantissa ? 3 : 2;
        return 4;
    }

    static void rt_tzset(void) { _tzset(); }
    static int32_t rt_timegm(linux_tm *t)
    {
        struct tm w;
        memcpy(&w, t, 9 * sizeof(int));
        __time64_t r = _mkgmtime64(&w);
        if (r != -1)
            memcpy(t, &w, 9 * sizeof(int)); // normalised, as glibc does
        return (int32_t)r;
    }
    static char *rt_ctime(const int32_t *t)
    {
        __time64_t tt = *t;
        return _ctime64(&tt);
    }

    static int rt_swprintf(lwchar *s, size_t n, const lwchar *fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        int r = rt_vswprintf_l(s, n, fmt, ap);
        va_end(ap);
        return r;
    }
    // The loader's own vfprintf (its FILE handling), whatever flag asks.
    static int rt_vfprintf_chk(void *fp, int flag, const char *fmt, va_list ap)
    {
        (void)flag;
        typedef int (*Vfprintf)(void *, const char *, va_list);
        static Vfprintf vfprintfBridge;
        if (!vfprintfBridge)
            vfprintfBridge = (Vfprintf)bridgeOf("vfprintf");
        return vfprintfBridge ? vfprintfBridge(fp, fmt, ap) : vfprintf((FILE *)fp, fmt, ap);
    }

    // glibc's sigset_t: a bit per signal, signal 1 first.
    static int rt_sigaddset(uint32_t *set, int sig)
    {
        if (!set || sig < 1 || sig > 64)
        {
            errno = EINVAL;
            return -1;
        }
        set[(sig - 1) / 32] |= 1u << ((sig - 1) % 32);
        return 0;
    }
    static int rt_pthread_setcanceltype(int type, int *oldtype)
    {
        (void)type;
        if (oldtype)
            *oldtype = 0; // PTHREAD_CANCEL_DEFERRED
        return 0;
    }

    // glibc's i386 struct statvfs, filled from the current drive.
    struct linux_statvfs
    {
        uint32_t f_bsize, f_frsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail, f_fsid;
        int32_t unused;
        uint32_t f_flag, f_namemax;
        int32_t spare[6];
    };
    static int rt_statvfs(const char *path, linux_statvfs *buf)
    {
        (void)path;
        ULARGE_INTEGER available, total, unused;
        if (!buf || !GetDiskFreeSpaceExA(NULL, &available, &total, &unused))
        {
            errno = ENOENT;
            return -1;
        }
        auto blocks = [](ULONGLONG bytes) { return (uint32_t)(bytes / 4096 > 0xffffffffULL ? 0xffffffffULL : bytes / 4096); };
        memset(buf, 0, sizeof(*buf));
        buf->f_bsize = buf->f_frsize = 4096;
        buf->f_blocks = blocks(total.QuadPart);
        buf->f_bfree = blocks(unused.QuadPart);
        buf->f_bavail = blocks(available.QuadPart);
        buf->f_files = buf->f_ffree = buf->f_favail = 1000000;
        buf->f_namemax = 255;
        return 0;
    }
}

namespace RtLibcBridge
{
    void initBridges()
    {
        log_info("Initializing Raw Thrills libc bridges...");

        MAP("__printf_chk", rt_printf_chk);
        MAP("__fprintf_chk", rt_fprintf_chk);
        MAP("__vsnprintf_chk", rt_vsnprintf_chk);
        MAP("__asprintf_chk", rt_asprintf_chk);
        MAP("__memcpy_chk", rt_memcpy_chk);
        MAP("__memset_chk", rt_memset_chk);
        MAP("__strcpy_chk", rt_strcpy_chk);
        MAP("__strcat_chk", rt_strcat_chk);
        MAP("__strncpy_chk", rt_strncpy_chk);
        MAP("__strncat_chk", rt_strncat_chk);
        MAP("__stpcpy_chk", rt_stpcpy_chk);
        MAP("stpcpy", rt_stpcpy);
        MAP("__fread_chk", rt_fread_chk);
        MAP("__fdelt_chk", rt_fdelt_chk);
        MAP("__stack_chk_fail", rt_stack_chk_fail);
        MAP("__open_2", rt_open_2);

        MAP("__isoc99_sscanf", rt_isoc99_sscanf);
        MAP("__isoc99_fscanf", rt_isoc99_fscanf);
        MAP("__strtold_internal", rt_strtold_internal);

        MAP("ceilf", rt_ceilf);
        MAP("floorf", rt_floorf);
        MAP("ldexp", rt_ldexp);
        MAP("sincos", rt_sincos);
        MAP("sincosf", rt_sincosf);
        MAP("__finitef", rt_finitef);

        MAP("atol", rt_atol);
        MAP("strtoul", rt_strtoul);
        MAP("strtoull", rt_strtoull);
        MAP("strerror_r", rt_strerror_r);
        MAP("putenv", rt_putenv);
        MAP("rmdir", rt_rmdir);

        MAP("clock", rt_clock);
        MAP("clock_getres", rt_clock_getres);
        MAP("gmtime", rt_gmtime);
        MAP("asctime", rt_asctime);
        MAP("asctime_r", rt_asctime_r);

        MAP("getppid", rt_getppid);
        MAP("getgid", rt_getgid);
        MAP("getrlimit", rt_getrlimit);
        MAP("sysconf", rt_sysconf);
        MAP("sysinfo", rt_sysinfo);
        MAP("syscall", rt_syscall);
        MAP("getpwuid_r", rt_getpwuid_r);
        MAP("getlogin_r", rt_getlogin_r);
        MAP("ttyname_r", rt_ttyname_r);
        MAP("ttyname", rt_ttyname);
        MAP("execvp", rt_execvp);
        MAP("_Exit", bridgeOf("_exit") ? bridgeOf("_exit") : (void *)_exit);
        MAP("ioperm", rt_ioperm);
        MAP("mprotect", rt_mprotect);
        MAP("pipe", rt_pipe);

        MAP("sigdelset", rt_sigdelset);
        MAP("sigprocmask", rt_sigprocmask);
        MAP("pthread_sigmask", rt_sigprocmask);
        MAP("pthread_kill", rt_pthread_kill);
        MAP("__libc_current_sigrtmin", rt_libc_current_sigrtmin);
        MAP("__sysv_signal", rt_sysv_signal);
        MAP("__pthread_key_create", rt_pthread_key_create);
        MAP("__h_errno_location", rt_h_errno_location);

        MAP("dladdr", dladdr);
        MAP("dlvsym", dlvsym);
        MAP("backtrace", rt_backtrace);
        MAP("backtrace_symbols", rt_retnull);

        MAP("setutent", rt_noop);
        MAP("endutent", rt_noop);
        MAP("getutent_r", rt_getutent_r);
        MAP("setmntent", rt_retnull);
        MAP("getmntent", rt_retnull);
        MAP("endmntent", rt_endmntent);
        MAP("timerfd_create", rt_retm1);
        MAP("timerfd_settime", rt_retm1);

        MAP("getaddrinfo", rt_getaddrinfo);
        MAP("freeaddrinfo", rt_noop);
        MAP("getsockname", rt_retm1);
        MAP("inet_ntop", rt_inet_ntop);

        MAP("iconv_open", rt_iconv_open);
        MAP("iconv", rt_iconv);
        MAP("iconv_close", rt_ret0);

        MAP("wcscasecmp", rt_wcscasecmp);
        MAP("__wcscpy_chk", rt_wcscpy_chk);
        MAP("wcstoll", rt_wcstoll);
        MAP("wcstoull", rt_wcstoull);
        MAP("wcstoul", rt_wcstoul);

        // Big Buck HD Wild.
        MAP("strnlen", rt_strnlen);
        MAP("wcsncat", rt_wcsncat);
        MAP("bcopy", rt_bcopy);
        MAP("atoll", rt_atoll);
        MAP("iswalpha", rt_iswalpha);
        MAP("floor", rt_floor);
        MAP("ceil", rt_ceil);
        MAP("trunc", rt_trunc);
        MAP("lrint", rt_lrint);
        MAP("__fpclassifyf", rt_fpclassifyf);
        MAP("tzset", rt_tzset);
        MAP("timegm", rt_timegm);
        MAP("ctime", rt_ctime);
        MAP("swprintf", rt_swprintf);
        MAP("__isoc99_swscanf", rt_swscanf);
        MAP("__vfprintf_chk", rt_vfprintf_chk);
        MAP("sigaddset", rt_sigaddset);
        MAP("pthread_setcanceltype", rt_pthread_setcanceltype);
        MAP("statvfs", rt_statvfs);
        MAP("setrlimit", rt_ret0);
        MAP("geteuid", rt_ret0);
        MAP("ptrace", rt_ret0);
        MAP("dl_iterate_phdr", rt_ret0);
        MAP("symlink", rt_retm1);
        MAP("getnetbyname", rt_retnull);
        MAP("wcstok", rt_wcstok);
        MAP("__swprintf_chk", rt_swprintf_chk);
        MAP("fwprintf", rt_fwprintf);
        MAP("__fwprintf_chk", rt_fwprintf_chk);
        MAP("swscanf", rt_swscanf);
        MAP("fsetpos", rt_fsetpos);
        MAP("fgetpos", rt_fgetpos);
        MAP("__sprintf_chk", rt_sprintf_chk);
        MAP("__snprintf_chk", rt_snprintf_chk);
        MAP("__vsprintf_chk", rt_vsprintf_chk);
        MAP("strtoll", rt_strtoll);
        MAP("__strtok_r", rt_strtok_r);
        MAP("strtok_r", rt_strtok_r);
        MAP("uname", rt_uname);
        MAP("wcscat", rt_wcscat);
        MAP("wcsdup", rt_wcsdup);
        MAP("pthread_attr_setinheritsched", rt_pthread_attr_setinheritsched);
        MAP("gluBuild2DMipmaps", rt_gluBuild2DMipmaps);

        // udev: no devices.
        MAP("udev_new", rt_udev_handle);
        MAP("udev_unref", rt_retnull);
        MAP("udev_monitor_new_from_netlink", rt_udev_handle);
        MAP("udev_monitor_filter_add_match_subsystem_devtype", rt_ret0);
        MAP("udev_monitor_enable_receiving", rt_ret0);
        MAP("udev_monitor_get_fd", rt_retm1);
        MAP("udev_monitor_receive_device", rt_retnull);
        MAP("udev_monitor_unref", rt_retnull);
        MAP("udev_enumerate_new", rt_udev_handle);
        MAP("udev_enumerate_add_match_subsystem", rt_ret0);
        MAP("udev_enumerate_scan_devices", rt_ret0);
        MAP("udev_enumerate_get_list_entry", rt_retnull);
        MAP("udev_enumerate_unref", rt_retnull);
        MAP("udev_list_entry_get_next", rt_retnull);
        MAP("udev_list_entry_get_name", rt_retnull);
        MAP("udev_device_new_from_syspath", rt_retnull);
        MAP("udev_device_new_from_devnum", rt_retnull);
        MAP("udev_device_get_parent_with_subsystem_devtype", rt_retnull);
        MAP("udev_device_get_sysattr_value", rt_retnull);
        MAP("udev_device_get_sysname", rt_retnull);
        MAP("udev_device_get_action", rt_retnull);
        MAP("udev_device_get_devnode", rt_retnull);
        MAP("udev_device_unref", rt_retnull);
    }
} // namespace RtLibcBridge

#endif
