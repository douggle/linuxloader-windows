// The Raw Thrills games' operator settings, as a home setup needs them.
//
// The games read them from files in the game directory (pmuser/adj/save, and
// adj/core/defs for their defaults), so the loader answers those reads with
// a copy holding another value:
//
//  camInvert  the cabinet's cameras hang upside down, so it is 1 by default
//             and the game turns the picture over itself. A webcam sits the
//             right way up: 0 leaves the picture as the camera gives it.
//  freeplay   the dumps ship on free play, set by the operator on a cabinet.
//             A home setup asks for coins instead, unless the ini asks for
//             free play ([Emulation] FREEPLAY = true).
//
// The files are small text documents: the saved ones ("# RTDT Data Type
// AdjValStore", name, version, time_set, val) behind a 20-byte SHA-1 of the
// text and its length, the definitions (name, version, default_value, min,
// max) plain.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "rawthrills.h"
#include "rtSettings.h"
#include "../config/config.h"
#include "../log/log.h"
#include "../platform/win32/posix.h"

#define SETTING_FILE_MAX 1024

// The value the game should see for the setting its basename names, or NULL
// for a file the loader leaves alone. The basename carries the setting's name
// first: camInvert.0.txt, freeplay.0.txt, camInvert.txt in the definitions.
static const char *settingValue(const char *path)
{
    static char freeplay[4];
    const char *base = strrchr(path, '/');

    base = base ? base + 1 : path;
    if (!strncasecmp(base, "camInvert.", 10))
        return "0";
    if (!strncasecmp(base, "freeplay.", 9))
    {
        snprintf(freeplay, sizeof(freeplay), "%d", getConfig()->freeplay == 1);
        return freeplay;
    }
    return NULL;
}

// The loader interposes the file calls these settings are read with
// (filesystemShared.c), and dlsym(RTLD_NEXT, "fopen") hands back the
// interposer's own fopen when the game imports it from the loader: a read
// that goes through it comes back here, and round again until the stack is
// gone. These go straight to the kernel instead, past every one of them.
static int readFile(const char *path, void *buf, size_t size)
{
    int fd = (int)syscall(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
    ssize_t len;

    if (fd < 0)
        return -1;
    len = syscall(SYS_read, fd, buf, size);
    syscall(SYS_close, fd);
    return (int)len;
}

static int writeFile(const char *path, const void *data, size_t len)
{
    int fd = (int)syscall(SYS_openat, AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    const char *p = data;
    size_t done = 0;

    if (fd < 0)
        return -1;
    while (done < len)
    {
        ssize_t n = syscall(SYS_write, fd, p + done, len - done);
        if (n <= 0)
        {
            syscall(SYS_close, fd);
            return -1;
        }
        done += (size_t)n;
    }
    syscall(SYS_close, fd);
    return 0;
}

// ---------------------------------------------------------------------------
// SHA-1 (FIPS 180-1), for the saved settings' header.

typedef struct
{
    uint32_t h[5];
    uint64_t length;
    unsigned char block[64];
    size_t used;
} Sha1;

static uint32_t rol(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1Block(Sha1 *s, const unsigned char *p)
{
    uint32_t w[80], a, b, c, d, e, t;

    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++)
    {
        if (i < 20)
            t = (b & c) | (~b & d);
        else if (i < 40)
            t = b ^ c ^ d;
        else if (i < 60)
            t = (b & c) | (b & d) | (c & d);
        else
            t = b ^ c ^ d;
        uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
        t = rol(a, 5) + t + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e;
}

static void sha1Init(Sha1 *s)
{
    s->h[0] = 0x67452301, s->h[1] = 0xEFCDAB89, s->h[2] = 0x98BADCFE, s->h[3] = 0x10325476, s->h[4] = 0xC3D2E1F0;
    s->length = 0;
    s->used = 0;
}

static void sha1Add(Sha1 *s, const void *data, size_t len)
{
    const unsigned char *p = data;

    s->length += len;
    while (len)
    {
        size_t n = 64 - s->used;
        if (n > len)
            n = len;
        memcpy(s->block + s->used, p, n);
        s->used += n, p += n, len -= n;
        if (s->used == 64)
        {
            sha1Block(s, s->block);
            s->used = 0;
        }
    }
}

static void sha1Finish(Sha1 *s, unsigned char out[20])
{
    uint64_t bits = s->length * 8;
    unsigned char pad = 0x80, zero = 0, tail[8];

    sha1Add(s, &pad, 1);
    while (s->used != 56)
        sha1Add(s, &zero, 1);
    for (int i = 0; i < 8; i++)
        tail[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha1Add(s, tail, 8);
    for (int i = 0; i < 5; i++)
    {
        out[4 * i] = (unsigned char)(s->h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(s->h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(s->h[i] >> 8);
        out[4 * i + 3] = (unsigned char)s->h[i];
    }
}

// ---------------------------------------------------------------------------

// key's value replaced with value: out is the new text, its length returned
// (-1 when key is not there).
static int replaceValue(const char *text, size_t textLen, const char *key, const char *value, char *out, size_t outSize)
{
    const char *p = strstr(text, key);
    size_t start, end, tail, valueLen;

    if (!p)
        return -1;
    p += strlen(key);
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '=')
        return -1;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    start = (size_t)(p - text);
    for (end = start; end < textLen && ((text[end] >= '0' && text[end] <= '9') || text[end] == '-'); end++)
        ;
    if (end == start)
        return -1;
    valueLen = strlen(value);
    tail = textLen - end;
    if (start + valueLen + tail > outSize)
        return -1;
    memcpy(out, text, start);
    memcpy(out + start, value, valueLen);
    memcpy(out + start + valueLen, text + end, tail);
    return (int)(start + valueLen + tail);
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8), p[2] = (unsigned char)(v >> 16), p[3] = (unsigned char)(v >> 24);
}

// Where the patched copies go: beside the squashfs mounts, in the login's
// runtime directory, or /tmp when there is none.
static void copyPath(const char *base, char *out, size_t outLen)
{
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    char dir[512], parent[512];
#ifdef _WIN32
    // No /tmp on Windows ("\tmp" of whichever drive is current): the user's
    // temporary directory.
    const char *temp = getenv("TEMP");
    if (temp && temp[0])
    {
        snprintf(parent, sizeof(parent), "%s\\linuxloader", temp);
        snprintf(dir, sizeof(dir), "%s\\settings", parent);
        mkdir(parent, 0700);
        mkdir(dir, 0700);
        snprintf(out, outLen, "%s\\%s", dir, base);
        return;
    }
#endif

    if (xdg && xdg[0] == '/')
    {
        snprintf(parent, sizeof(parent), "%s/linuxloader", xdg);
        snprintf(dir, sizeof(dir), "%s/settings", parent);
    }
    else
    {
        snprintf(parent, sizeof(parent), "/tmp/linuxloader-%lu", (unsigned long)geteuid());
        snprintf(dir, sizeof(dir), "%s/settings", parent);
    }
    mkdir(parent, 0700); // either one is there after the first run
    mkdir(dir, 0700);
    snprintf(out, outLen, "%s/%s", dir, base);
}

// The file the game should read instead of path, or NULL to leave the read
// alone. The games read these from several threads, and their stacks can be
// small: everything here is thread-local, not on the stack.
const char *rtSettingFile(const char *path)
{
    static __thread char out[512], text[SETTING_FILE_MAX], patched[SETTING_FILE_MAX];
    static __thread unsigned char raw[SETTING_FILE_MAX + 32];
    static __thread char logged[32];
    const char *name, *value;
    int len;

    if (!path || !isRawThrillsGame())
        return NULL;
    if (!strstr(path, "/adj/"))
        return NULL;
    if (!strstr(path, "/save/") && !strstr(path, "/defs/"))
        return NULL;
    if (!(name = strrchr(path, '/')))
        return NULL;
    name++;
    if (!(value = settingValue(path)))
        return NULL;

    if ((len = readFile(path, raw, sizeof(raw) - 1)) <= 0 || (size_t)len >= sizeof(text))
        return NULL;

    copyPath(name, out, sizeof(out));
    if (strstr(path, "/adj/save/"))
    {
        // The saved value, with the document's SHA-1 and lengths rebuilt
        // around it: 4-byte header length, the SHA-1, 4-byte text length,
        // then the text (its terminating NUL included in the length).
        uint32_t headerLen = get32(raw), textLen;
        unsigned char sha[20], doc[8 + 32 + SETTING_FILE_MAX];
        int newLen;
        Sha1 s;

        if (headerLen != 20 || len < 8 + headerLen)
            return NULL;
        textLen = get32(raw + 4 + headerLen);
        if (!textLen || 8 + headerLen + textLen > len || textLen > sizeof(text))
            return NULL;
        memcpy(text, raw + 8 + headerLen, textLen);
        if ((newLen = replaceValue(text, textLen, "val", value, patched, sizeof(patched))) < 0)
            return NULL;
        textLen = (uint32_t)newLen;
        sha1Init(&s);
        sha1Add(&s, patched, textLen);
        sha1Finish(&s, sha);
        put32(doc, headerLen);
        memcpy(doc + 4, sha, 20);
        put32(doc + 4 + headerLen, textLen);
        memcpy(doc + 8 + headerLen, patched, textLen);
        if (writeFile(out, doc, 8 + headerLen + textLen) < 0)
            return NULL;
    }
    else
    {
        // The definition's default, for a setting the game never saved.
        int newLen;

        memcpy(text, raw, len);
        text[len] = '\0';
        if ((newLen = replaceValue(text, len, "default_value", value, patched, sizeof(patched))) < 0)
            return NULL;
        if (writeFile(out, patched, (size_t)newLen) < 0)
            return NULL;
    }
    if (strcmp(logged, name) != 0)
    {
        log_info("Setting: %s read as %s", name, value);
        snprintf(logged, sizeof(logged), "%s", name);
    }
    return out;
}
