// Wangan Midnight Maximum Tune 4 (WMN4r, rev 1.10.18): what only this game
// needs, from the Pacloader fork's es1Wmmt4.cpp. The game is stripped and
// links the HASP HL API, its JVIO library and its own network diagnostic
// statically: these are found addresses, valid for this build only (the
// launcher knows it by its file CRC).

#include <arpa/inet.h>
#include <dlfcn.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#include <glad/gl.h>
#include <unistd.h>
// The game's own X window, the pbuffer its GL entry points are loaded from
// and the frame it hands to GLX: Linux's display. The Windows build's window
// is the loader's (SDL), and there is no GLX.
#if defined(__linux__)
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif

#include "namcoEs1.h"
#include "namcoEs1Wmmt4Server.h"
#include "../common/importHook.h"
#include "../config/config.h"
#include "../graphics/shaderPatches.h"
#include "../input/evdevFfb.h"
#include "../../minhook/include/MinHook.h"
#include "../log/log.h"

// Overwrite a function's entry with a jmp to the replacement, if its first
// bytes are the expected ones.
static void detour(uint32_t address, const uint8_t *expected, size_t n, void *replacement)
{
    uint8_t *target = (uint8_t *)(uintptr_t)address;
    uintptr_t page = address & ~(uintptr_t)0xfff;

    if (memcmp(target, expected, n) != 0)
    {
        log_error("WMMT4: unexpected code at %#x, not hooked", address);
        return;
    }
    if (mprotect((void *)page, (address + 5 - page + 0xfff) & ~(uintptr_t)0xfff, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
    {
        log_error("WMMT4: cannot make %#x writable", address);
        return;
    }
    target[0] = 0xe9;
    *(int32_t *)(target + 1) = (int32_t)((uint8_t *)replacement - (target + 5));
}

// Overwrite code with other bytes, if it is the expected one.
static void patch(uint32_t address, const uint8_t *expected, const uint8_t *replacement, size_t n)
{
    uint8_t *target = (uint8_t *)(uintptr_t)address;
    uintptr_t page = address & ~(uintptr_t)0xfff;

    if (memcmp(target, expected, n) != 0)
    {
        log_error("WMMT4: unexpected code at %#x, not patched", address);
        return;
    }
    if (mprotect((void *)page, (address + n - page + 0xfff) & ~(uintptr_t)0xfff, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
    {
        log_error("WMMT4: cannot make %#x writable", address);
        return;
    }
    memcpy(target, replacement, n);
}

// ---------------------------------------------------------------------------
// HASP HL: the game reads its dongle's memory file (3392 bytes): its points
// and their limits (big-endian, each with an MPEG-2 CRC32), and the system
// block, the cabinet's serial (its ALL.Net id is ABEN and its last seven
// digits) with a sum and its complement. Writes go to the image only. The
// sixth digit says drive cabinet (0) or terminal (1).

#define HASP_SIZE 3392
#define HASP_SYSTEM 3328
#define HASP_SERIAL "267610069420"

static uint8_t hasp[HASP_SIZE];

// WMMT4_AUTH_TRACE: the dongle calls and the cabinet LAN's datagrams, for
// what the boot check waits on before its authentication.
static int authTrace;
static int bootTrace;

// WMMT4_AUTH_RETRY: the game asks for its 認証 once, during construction, while
// the pcb serial string the request names is still empty - Sys::Network+0xec,
// the std::string 0x80a1980 hands to the request builder 0x8201e20 as its second
// argument, whose length the builder requires to be above 4 (at 0x8201e60 it
// reads the length at data-0xc and logs "no pcb_serial" below that). The game
// fills that string only at network: start(), hundreds of log lines later, so
// the one request is refused and never made again. Set, the request is repeated
// once as soon as the string holds the serial (networkUpdateState below). Only
// this flag installs the entry hook the repeat needs; a plain boot is unchanged.
static int authRetry;
static uint32_t authObject; // the Sys::Network the first request named
static int authRequestBeforeSerial;
static int authRequestRetried;
// The repeat waits for the serial to have been visible for a while: network:
// start() sets the serial before it builds the service URI, and a request made
// in between carries "https://:0/..." (as the first attempt of the run did).
static long long authSerialSeenAt;
static long authRetryWaitMs = -1; // WMMT4_AUTH_RETRY_WAIT; -1 until parsed, 0 = unset

static long long monotonicMs(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static size_t networkSerial(const void *network, char *out, size_t size);

static void haspTrace(const char *call, uint32_t a, uint32_t b, uint32_t c)
{
    if (authTrace)
        fprintf(stderr, "WMMT4 authtrace: hasp %s(%u, %u, %u)\n", call, a, b, c);
}

static uint32_t crc32Mpeg(const uint8_t *data, size_t n)
{
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < n; i++)
    {
        crc ^= (uint32_t)data[i] << 24;
        for (int bit = 0; bit < 8; bit++)
            crc = crc & 0x80000000 ? crc << 1 ^ 0x04c11db7 : crc << 1;
    }
    return crc;
}

static void putBe32(uint8_t *p, uint32_t value)
{
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
    p[3] = value;
}

static void haspImage(void)
{
    uint8_t sum = 0;

    memset(hasp, 0, sizeof(hasp));
    // Point limit: initialised, upper token 5, CRC of its first 12 bytes.
    hasp[0] = 1;
    putBe32(hasp + 4, 5);
    putBe32(hasp + 12, crc32Mpeg(hasp, 12));
    // Two points: cost 1, limit 9999, current 1.
    for (int i = 0; i < 2; i++)
    {
        uint8_t *point = hasp + 16 + 16 * i;
        putBe32(point, 1);
        putBe32(point + 4, 9999);
        putBe32(point + 8, 1);
        putBe32(point + 12, crc32Mpeg(point, 12));
    }
    memcpy(hasp + HASP_SYSTEM, HASP_SERIAL, 12);
    for (int i = 0; i < 62; i++)
        sum += hasp[HASP_SYSTEM + i];
    hasp[HASP_SYSTEM + 62] = sum;
    hasp[HASP_SYSTEM + 63] = ~sum;
}

__attribute__((force_align_arg_pointer)) static int haspLogin(uint32_t feature, const void *vendorCode, uint32_t *handle)
{
    (void)feature;
    (void)vendorCode;
    haspTrace("login", feature, 0, 0);
    if (handle)
        *handle = 1;
    return 0;
}

__attribute__((force_align_arg_pointer)) static int haspNothing(uint32_t handle)
{
    haspTrace("logout/encrypt/decrypt", handle, 0, 0);
    (void)handle;
    return 0;
}

__attribute__((force_align_arg_pointer)) static int haspGetSize(uint32_t handle, uint32_t fileId, uint32_t *size)
{
    haspTrace("get_size", handle, fileId, 0);
    (void)handle;
    (void)fileId;
    if (size)
        *size = sizeof(hasp);
    return 0;
}

static int haspRange(uint32_t offset, uint32_t length)
{
    return offset <= sizeof(hasp) && length <= sizeof(hasp) - offset;
}

__attribute__((force_align_arg_pointer)) static int haspRead(uint32_t handle, uint32_t fileId, uint32_t offset,
                                                             uint32_t length, void *buffer)
{
    haspTrace("read", fileId, offset, length);
    (void)handle;
    (void)fileId;
    if (!buffer || !haspRange(offset, length))
        return 1; // HASP_MEM_RANGE
    memcpy(buffer, hasp + offset, length);
    return 0;
}

__attribute__((force_align_arg_pointer)) static int haspWrite(uint32_t handle, uint32_t fileId, uint32_t offset,
                                                              uint32_t length, const void *buffer)
{
    (void)handle;
    (void)fileId;
    if (!buffer || !haspRange(offset, length))
        return 1;
    memcpy(hasp + offset, buffer, length);
    return 0;
}

// ---------------------------------------------------------------------------
// The JVIO library's monitor (a text dump of the board's state for the
// debug overlay): an empty string. Its send command (to the cabinet's
// althmand daemon, which is not run): done.

__attribute__((force_align_arg_pointer)) static char *jvioMonitor(int a, int b, char *buffer, size_t size)
{
    (void)a;
    (void)b;
    if (buffer && size)
        buffer[0] = '\0';
    return buffer;
}

__attribute__((force_align_arg_pointer)) static int sendCommand(int a, int b, int c, int d, char *e)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    return 0;
}

// The game's log (Sys::LogMes), which goes nowhere on the cabinet: on
// stderr with NAMCO_GAME_LOG set.
static int gameLog;

__attribute__((force_align_arg_pointer)) static void logMes(int type, const char *format, ...)
{
    char message[2048];
    va_list args;

    if (!gameLog)
        return;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format ? format : "", args);
    va_end(args);
    size_t n = strlen(message);
    fprintf(stderr, "WMMT4 [%d]: %s%s", type, message, n && message[n - 1] == '\n' ? "" : "\n");
}

// ---------------------------------------------------------------------------
// The network diagnostic the game runs before going online (cable, gateway,
// shop router, hops...): eth0 is the host's interface, and the local checks
// pass. Authentication is left alone: the game finds itself offline by its
// own means.

// The check stage at [network+0x274] is an index into the game's own dispatch
// table at 0x8be4218 (17 entries, 0..16, jumped through at 0x80a9cd0). The names
// below are the game's: each comes from the arm's own log text and from the
// state word that arm reads, not from an assumed order (an earlier version of
// this enum assumed INTERFACE were stage 1 and shifted everything after it).
//
//   stage  arm         reads            the arm's log text
//   0      0x80a9c00   -                publishes the three state words
//   1      0x80a9d3e   -                "checking cable..." (the check starts)
//   2      0x80a9e10   +0x228           "cable is unknwon" / "checking content router..."
//   3      0x80a9eec   +0x22c           "gateway is unknwon" / "checking shop address..."
//   4      0x80a9f63   +0x1b4, +0x238   "shop router is unknwon"
//   5      0x80a9f8a   +0x238, +0x1b8   "shop router is unknwon"
//   6      0x80a9fe0   +0x1b8, +0x224   - (the word item 0x841a270 polls)
//   7      0x80aa012   +0x224           "ntpdate failed"
//   8      0x80a9c00   -                publishes, the same arm as stage 0
//   9      0x80aa099   +0x220           "renew canceled" / "renew success"
//   10     0x80aa0f5   -                "test done"
//   11     0x80aa167   -                "Wi-Fi check done"
//   12     0x80aa19e   -                "set date done"
//   13     0x80aa1d5   -                "online network check done"
//   14     0x80aa20f   +0x228           the 8 <-> 14 cycle while the cable re-checks
//   15     0x80a9d04   +0x250           "ntp done"
//   16     0x80aa05f   +0x254           "pkg file check done"
//
// There is no INTERFACE stage: the interface record is updated by its own hook
// (0x821d4f0) before the machine runs. Stages 4..7 were not exercised by the
// seeded boot (that run goes 3 -> 8), so their names rest on the arms' field
// reads and log text alone; 9, 12, 15 and 16 are confirmed by the game's own
// lines in a run ("renew success", "set date done", "ntp done", "pkg file
// check done").
enum
{
    NET_NONE,
    NET_CABLE_START,
    NET_CABLE,
    NET_GATEWAY,
    NET_SHOP_ADDRESS,
    NET_SHOP_ROUTER,
    NET_AUTH,
    NET_SYNC_DATE,
    NET_DONE,
    NET_RENEW,
    NET_TEST,
    NET_WIFI,
    NET_SET_DATE,
    NET_ONLINE,
    NET_CABLE_RECHECK,
    NET_NTP,
    NET_PACKAGE_FILE,
};

#define STATE_GOOD 3

typedef struct __attribute__((packed))
{
    uint8_t pad0[436];
    uint32_t resolveState;
    uint8_t pad1[104];
    uint32_t renewState, syncDateState, cableState, gatewayState, contentRouterState, shopRouterState, hopsState;
    uint8_t pad2[4];
    uint32_t hops;
    uint32_t wifiRouterState, onlineState, setDateState, ntpState, packageFileState;
    uint8_t pad3[28];
    uint32_t check;
} NetworkImpl;

_Static_assert(__builtin_offsetof(NetworkImpl, check) == 628, "WMMT4 network state layout");

typedef struct __attribute__((packed))
{
    uint8_t pad0[8];
    uint8_t name[4]; // std::string
    uint32_t address, netmask, gateway;
    uint8_t pad1[12];
    uint8_t mac[4]; // std::string
} NetworkInterface;

_Static_assert(__builtin_offsetof(NetworkInterface, mac) == 36, "WMMT4 network interface layout");

#define NETWORK_GET_INSTANCE 0x080a5c60
#define NETWORK_APPLY_STATE 0x080a9b70
#define STRING_ASSIGN 0x080579c8 // std::string::assign(const char *), PLT

// Sys::Net::Interface::isAddressChange(): never.
__attribute__((force_align_arg_pointer)) static int addressChanged(void *self)
{
    (void)self;
    return 0;
}

static uint32_t pack(const unsigned char octets[4])
{
    return (uint32_t)octets[0] << 24 | (uint32_t)octets[1] << 16 | (uint32_t)octets[2] << 8 | octets[3];
}

// Sys::Net::Interface::update(): a cabinet's address on the shop's LAN
// (192.168.92.11/24, as the Pacloader fork gives it) and the host's MAC. The
// content router check wants the gateway at .254 of that network (E0502
// "Content Router mismatch" otherwise: the host's own network fails it).
__attribute__((force_align_arg_pointer)) static int interfaceUpdate(NetworkInterface *state)
{
    static int logged;
    unsigned char address[4], mask[4], mac[6];
    char macText[13];
    void (*assign)(void *, const char *) = (void (*)(void *, const char *))STRING_ASSIGN;

    if (!state)
        return 0;
    namcoEs1HostInterface(address, mask, mac);
    memcpy(address, namcoEs1CurrentGame()->eth0, 4);
    memcpy(mask, namcoEs1CurrentGame()->eth0 + 4, 4);
    state->address = pack(address);
    state->netmask = pack(mask);
    state->gateway = (pack(address) & pack(mask)) | 254;
    snprintf(macText, sizeof(macText), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    assign(state->name, "eth0");
    assign(state->mac, macText);
    if (!logged)
    {
        logged = 1;
        log_info("WMMT4: eth0 is %u.%u.%u.%u/%u.%u.%u.%u, %s", address[0], address[1], address[2], address[3],
                 mask[0], mask[1], mask[2], mask[3], macText);
    }
    return 1;
}

static const char *const netCheckNames[] = {"NONE",        "CABLE_START", "CABLE",   "GATEWAY", "SHOP_ADDRESS",
                                            "SHOP_ROUTER", "AUTH",        "SYNC_DATE", "DONE",  "RENEW",
                                            "TEST",        "WIFI",        "SET_DATE",  "ONLINE", "CABLE_RECHECK",
                                            "NTP",         "PKG_FILE"};

// WMMT4_BOOT_TRACE: the boot screen's authentication row is the NET_AUTH check
// of this machine, and its "checking" text is that check's result still at 1
// (busy). One line per pump names the check and every result, so a stall shows
// which one never leaves busy.
static void reportNetworkCheck(const NetworkImpl *impl)
{
    static unsigned calls, last;
    uint32_t key;

    if (!bootTrace)
        return;
    key = impl->check;
    key = key * 31 + impl->resolveState;
    key = key * 31 + impl->renewState;
    key = key * 31 + impl->syncDateState;
    key = key * 31 + impl->cableState;
    key = key * 31 + impl->gatewayState;
    key = key * 31 + impl->contentRouterState;
    key = key * 31 + impl->shopRouterState;
    key = key * 31 + impl->hopsState;
    key = key * 31 + impl->hops;
    key = key * 31 + impl->wifiRouterState;
    key = key * 31 + impl->onlineState;
    key = key * 31 + impl->setDateState;
    key = key * 31 + impl->ntpState;
    key = key * 31 + impl->packageFileState;
    // The state usually does not change at all, so a keyed-only line reported
    // the first few pumps and hid the rest (a 5 that read as the total). The
    // count is cumulative: every pump is named up to 12, then every 20th.
    calls++;
    if (key == last && calls > 12 && calls % 20 != 0)
        return;
    last = key;
    fprintf(stderr,
            "WMMT4 boot: netcheck %u %s, pump %u total: resolve %u renew %u syncDate %u cable %u gateway %u "
            "content %u shop %u hops %u/%u wifi %u online %u setDate %u ntp %u file %u\n",
            impl->check, impl->check < 17 ? netCheckNames[impl->check] : "?", calls,
            impl->resolveState, impl->renewState, impl->syncDateState, impl->cableState,
            impl->gatewayState, impl->contentRouterState, impl->shopRouterState, impl->hopsState,
            impl->hops, impl->wifiRouterState, impl->onlineState, impl->setDateState, impl->ntpState,
            impl->packageFileState);
}

// WMMT4_BOOT_TRACE: updateState (0x80a9b70) is Sys::Network_Impl's machine
// (the local checks), and it names no caller in any table we have: the
// wrapper's own version (0x80aa7d0, replaced here) is what calls it. Its entry
// is patched to a stub that reports the return address, resolved back to the
// call instruction when one precedes it, counted per site and in total: a
// caller outside the loader means another path drives the machine, and only
// the loader's own `apply` means it runs exactly as often as the game pumps
// the wrapper.
static unsigned long updateStateCalls;

static uint32_t updateStateCallSite(uint32_t ret)
{
    for (unsigned back = 0; back < 8; back++)
    {
        uint32_t at = ret - 5 - back;
        const uint8_t *code = (const uint8_t *)(uintptr_t)at;
        int32_t rel;

        if (code[0] != 0xe8)
            continue;
        memcpy(&rel, code + 1, sizeof(rel));
        if ((uint32_t)(at + 5 + rel) == 0x080a9b70)
            return at;
    }
    return ret;
}

__attribute__((visibility("hidden"))) void wmmt4UpdateStateSeen(uint32_t ret, uint32_t self, uint32_t states)
{
    static struct
    {
        uint32_t site;
        unsigned calls;
    } sites[8];
    static unsigned siteCount;
    uint32_t site;
    unsigned i;

    static uint32_t lastState = 0xffffffff;
    static uint32_t lastRenew = 0xffffffff;
    uint32_t state = self ? *(const uint32_t *)((const uint8_t *)(uintptr_t)self + 0x274) : 0xffffffff;
    // [self+0x220] is renewState, read only by the NET_RENEW arm (0x80aa099):
    // 2 -> 0x80aa382, 3 -> 0x80aa625, other -> 0x80aa59e, 0 -> the log, the
    // renew step 0x80a8500, 0x809f9d0, check = 1 and then the publish tail. It
    // is not read on the way to the publish itself.
    uint32_t renew = self ? *(const uint32_t *)((const uint8_t *)(uintptr_t)self + 0x220) : 0xffffffff;

    updateStateCalls++;
    if (!bootTrace)
        return;
    if (state != lastState)
    {
        // lastState is a sentinel until the first sighting: the field itself is
        // not -1 at any point, it is NONE from the object's construction on.
        if (lastState == 0xffffffff)
            fprintf(stderr, "WMMT4 boot: check stage %#x at pump %lu (this %#x), first seen\n", state,
                    updateStateCalls, self);
        else
            fprintf(stderr, "WMMT4 boot: check stage %#x -> %#x at pump %lu (this %#x)\n", lastState, state,
                    updateStateCalls, self);
        lastState = state;
    }
    if (renew != lastRenew)
    {
        fprintf(stderr, "WMMT4 boot: renewState [%#x+0x220] %#x -> %#x at pump %lu (NET_RENEW arm only)\n", self,
                lastRenew, renew, updateStateCalls);
        lastRenew = renew;
    }
    site = updateStateCallSite(ret);
    for (i = 0; i < siteCount; i++)
        if (sites[i].site == site)
            break;
    if (i == siteCount)
    {
        if (siteCount == sizeof(sites) / sizeof(sites[0]))
            return;
        sites[siteCount].site = site;
        sites[siteCount].calls = 0;
        siteCount++;
    }
    sites[i].calls++;
    if (updateStateCalls > 12 && updateStateCalls % 100 != 0)
        return;
    fprintf(stderr,
            "WMMT4 boot: updateState %lu from %#x%s (%u calls): this %#x, states %#x, check %u, seq %u/%u\n",
            updateStateCalls, site, site == ret ? " (no call opcode)" : "", sites[i].calls,
            self, states, self ? *(const uint32_t *)((const uint8_t *)(uintptr_t)self + 0x274) : 0,
            self ? *(const uint32_t *)((const uint8_t *)(uintptr_t)self + 0x2a4) : 0,
            self ? *(const uint32_t *)((const uint8_t *)(uintptr_t)self + 0x2a8) : 0);
}

// 0x80a9b70: push ebp; mov ebp,esp; push edi; push esi; push ebx; sub esp,0x8c.
void wmmt4UpdateState(void);
__asm__(".text\n"
        ".globl wmmt4UpdateState\n"
        "wmmt4UpdateState:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  subl $4, %esp\n"
        "  pushl 0x2c(%ebp)\n"
        "  pushl 0x28(%ebp)\n"
        "  pushl 0x24(%ebp)\n"
        "  calll wmmt4UpdateStateSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %edi\n"
        "  pushl %esi\n"
        "  pushl %ebx\n"
        "  subl $0x8c, %esp\n"
        "  movl $0x80a9b7c, %eax\n"
        "  jmp *%eax\n");

// WMMT4_BOOT_TRACE: what the 認証 row actually waits on. The step sets the
// gate's busy flag (0x841a6ba) and then asks 0x82578b0 -- true iff the monitor
// singleton at 0x921531c exists and [obj+0x4c] != 0 -- calling 0x825ab80 when
// it is true. It then copies the monitor record at 0x92204e0+0x678 into a
// local (0x843c1e0), zeroes the local's first word, and copies it back
// (0x843c240). Each of those four call sites is re-issued here, so the result
// and the record are measured instead of inferred.
#define MONITOR_SINGLETON 0x0921531c
#define MONITOR_OBJECT 0x092204e0
#define MONITOR_RECORD 0x678
#define GATE_OBJECT 0x09221ea0

static void authRecord(const char *what, uint32_t object, uint32_t offset)
{
    const uint32_t *record = (const uint32_t *)(uintptr_t)(object + offset);

    fprintf(stderr, "WMMT4 boot: %s %#x + %#x: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n", what,
            object, offset, record[0], record[1], record[2], record[3], record[4], record[5], record[6],
            record[7], record[8], record[9]);
}

__attribute__((visibility("hidden"))) void wmmt4AuthPresentSeen(uint32_t result)
{
    static unsigned n;
    uint32_t monitor = *(const uint32_t *)(uintptr_t)MONITOR_SINGLETON;

    if (!bootTrace)
        return;
    n++;
    if (n > 8 && n % 200 != 0)
        return;
    fprintf(stderr,
            "WMMT4 boot: auth %u: 0x82578b0 -> %u (monitor %#x, [+0x4c] %#x, [+0x50] %#x, gate %#x [+0xc] %u)\n",
            n, result, monitor, monitor ? *(const uint32_t *)((const uint8_t *)(uintptr_t)monitor + 0x4c) : 0,
            monitor ? *(const uint32_t *)((const uint8_t *)(uintptr_t)monitor + 0x50) : 0, GATE_OBJECT,
            *(const uint32_t *)(uintptr_t)(GATE_OBJECT + 0xc));
    if (monitor)
    {
        authRecord("  monitor head", monitor, 0x30);
        authRecord("  monitor tail", monitor, 0x40);
    }
}

__attribute__((visibility("hidden"))) void wmmt4AuthCheckSeen(void)
{
    static unsigned n;
    uint32_t monitor = *(const uint32_t *)(uintptr_t)MONITOR_SINGLETON;

    if (!bootTrace)
        return;
    n++;
    if (n > 8 && n % 200 != 0)
        return;
    fprintf(stderr, "WMMT4 boot: auth %u: 0x825ab80 entered (monitor %#x, [+0x4c] %#x, [+0x50] %#x)\n", n,
            monitor, monitor ? *(const uint32_t *)((const uint8_t *)(uintptr_t)monitor + 0x4c) : 0,
            monitor ? *(const uint32_t *)((const uint8_t *)(uintptr_t)monitor + 0x50) : 0);
}

__attribute__((visibility("hidden"))) void wmmt4AuthRecordReadSeen(uint32_t local, uint32_t monitor)
{
    static unsigned n;

    if (!bootTrace)
        return;
    n++;
    if (n > 8 && n % 200 != 0)
        return;
    fprintf(stderr, "WMMT4 boot: auth %u: record read 0x843c1e0(local %#x, monitor %#x)\n", n, local, monitor);
    authRecord("  monitor record", monitor, MONITOR_RECORD);
    authRecord("  monitor head", monitor, 0x40);
    authRecord("  local only-zeroed", local, 0);
}

__attribute__((visibility("hidden"))) void wmmt4AuthRecordWriteSeen(uint32_t monitor, uint32_t local)
{
    static unsigned n;

    if (!bootTrace)
        return;
    n++;
    if (n > 8 && n % 200 != 0)
        return;
    fprintf(stderr, "WMMT4 boot: auth %u: record write 0x843c240(monitor %#x, local %#x)\n", n, monitor, local);
    authRecord("  local record", local, 0);
    authRecord("  monitor record after", monitor, MONITOR_RECORD);
}

// 0x82578c8: the `ret` of 0x82578b0, whose eax is the bool the step tests. At a
// `ret` the caller's return address is already on top of the stack, so the stub
// saves the registers, logs eax, restores them and returns as the original did.
// (A stub on the call site cannot: detour() writes a jmp, so no return address
// is pushed and esp there is the caller's outgoing argument area, not a frame.)
void wmmt4AuthPresentRet(void);
__asm__(".text\n"
        ".globl wmmt4AuthPresentRet\n"
        "wmmt4AuthPresentRet:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  subl $8, %esp\n"
        "  pushl $0\n"
        "  pushl 28(%ebp)\n"
        "  calll wmmt4AuthPresentSeen\n"
        "  addl $16, %esp\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  ret\n");

// 0x825ab80 takes no arguments and returns nothing (it drives the monitor
// singleton 0x825aa60 or logs that there is none), so its entry is what there
// is to log. Prologue: push ebp; mov ebp,esp; sub esp,0x18.
void wmmt4AuthCheckEntry(void);
__asm__(".text\n"
        ".globl wmmt4AuthCheckEntry\n"
        "wmmt4AuthCheckEntry:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  calll wmmt4AuthCheckSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  subl $0x18, %esp\n"
        "  movl $0x825ab86, %eax\n"
        "  jmp *%eax\n");

// 0x843c1e0(out, monitor): copies the monitor record into the step's local.
// Entered from the step; the entry stub logs both pointers and the record on
// each side. Prologue: push ebp; mov ebp,esp; mov edx,[ebp+0xc]; mov eax,[ebp+8].
void wmmt4AuthRecordReadEntry(void);
__asm__(".text\n"
        ".globl wmmt4AuthRecordReadEntry\n"
        "wmmt4AuthRecordReadEntry:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  subl $8, %esp\n"
        "  pushl 0x2c(%ebp)\n"
        "  pushl 0x28(%ebp)\n"
        "  calll wmmt4AuthRecordReadSeen\n"
        "  addl $16, %esp\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  movl 0xc(%ebp), %edx\n"
        "  movl 0x8(%ebp), %eax\n"
        "  pushl $0x843c1e9\n"
        "  ret\n");

// 0x843c240(monitor, local): copies the local back over the monitor record.
void wmmt4AuthRecordWriteEntry(void);
__asm__(".text\n"
        ".globl wmmt4AuthRecordWriteEntry\n"
        "wmmt4AuthRecordWriteEntry:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  subl $8, %esp\n"
        "  pushl 0x2c(%ebp)\n"
        "  pushl 0x28(%ebp)\n"
        "  calll wmmt4AuthRecordWriteSeen\n"
        "  addl $16, %esp\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  movl 0xc(%ebp), %edx\n"
        "  movl 0x8(%ebp), %eax\n"
        "  pushl $0x843c249\n"
        "  ret\n");

// WMMT4_BOOT_TRACE: the check index is written only inside the machine, and
// every write is an immediate (0x80a4253..0x80aa489); nothing outside it sets
// the field, and the machine's own dispatch leaves NONE and DONE on the common
// tail without advancing. 0x80a8500 is the NET_RENEW stage's own step (check
// 9, 0x80aa099: renewState 0 -> log, 0x80a8500, 0x809f9d0, check = 1, then the
// publish tail); it is not a check start, and it is not where the sequence
// begins. Its entry is counted here rather than inferred from the chain.
static unsigned checkStartCalls;

__attribute__((visibility("hidden"))) void wmmt4CheckStartSeen(uint32_t self)
{
    checkStartCalls++;
    if (!bootTrace || (checkStartCalls > 3 && checkStartCalls % 20 != 0))
        return;
    fprintf(stderr, "WMMT4 boot: renew step 0x80a8500 entry %u (this %#x, check on entry %u)\n",
            checkStartCalls, self, self ? *(const uint32_t *)((const uint8_t *)(uintptr_t)self + 0x274) : 0);
}

// 0x80a8500: push ebp; mov ebp,esp; push esi; push ebx; sub esp,0x30.
void wmmt4CheckStart(void);
__asm__(".text\n"
        ".globl wmmt4CheckStart\n"
        "wmmt4CheckStart:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  subl $4, %esp\n"
        "  pushl 0x28(%ebp)\n"
        "  calll wmmt4CheckStartSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %esi\n"
        "  pushl %ebx\n"
        "  subl $0x30, %esp\n"
        "  movl $0x80a8508, %eax\n"
        "  jmp *%eax\n");

// Sys::Network::UpdateState(): each local check passes before the game
// publishes the states.
__attribute__((force_align_arg_pointer)) static void networkUpdateState(void *states)
{
    static int cableVerified;
    static int checkStageSeed = -1; // WMMT4_CHECK_STAGE_SEED, parsed once; -1 until then, 0 = unset
    static unsigned long seeds;
    void **(*getNetwork)(void) = (void **(*)(void))NETWORK_GET_INSTANCE;
    void (*apply)(NetworkImpl *, void *) = (void (*)(NetworkImpl *, void *))NETWORK_APPLY_STATE;
    void **network = getNetwork();
    NetworkImpl *impl = network ? *network : NULL;

    if (!impl)
        return;
    // WMMT4_AUTH_RETRY: repeat the 認証 request once the serial the builder
    // requires is there. Once only - the repeat is the experiment, not a loop -
    // and the game's own entry is called, so its guards, its session and its
    // request builder run as they would have if the serial had existed then.
    if (authRetry && authObject && authRequestBeforeSerial && !authRequestRetried)
    {
        char serial[64];

        if (authRetryWaitMs < 0)
        {
            const char *value = getenv("WMMT4_AUTH_RETRY_WAIT");

            authRetryWaitMs = value ? atol(value) : 3000;
            if (authRetryWaitMs < 0)
                authRetryWaitMs = 0;
        }
        if (networkSerial((const void *)(uintptr_t)authObject, serial, sizeof(serial)) > 4)
        {
            long long now = monotonicMs();

            // Armed once, when the serial first shows up; the request follows
            // the wait so network: start() has finished building the URI.
            if (!authSerialSeenAt)
            {
                authSerialSeenAt = now;
                fprintf(stderr, "WMMT4: auth retry: pcb serial %s is known, waiting %ld ms\n", serial,
                        authRetryWaitMs);
            }
            else if (now - authSerialSeenAt >= authRetryWaitMs)
            {
                int (*request)(void *) = (int (*)(void *))0x080a1980;
                int r;

                authRequestRetried = 1;
                fprintf(stderr, "WMMT4: auth retry: re-issuing 0x80a1980 for object %#x, pcb serial %s\n", authObject,
                        serial);
                r = request((void *)(uintptr_t)authObject);
                fprintf(stderr, "WMMT4: auth retry: 0x80a1980 returned %d\n", r);
            }
        }
    }
    // WMMT4_CHECK_STAGE_SEED=<n>: experiment, off unless asked for and then
    // only ever a one-way nudge out of NONE. <n> is the game's own stage
    // number (see the enum above): 9 is NET_RENEW, whose arm 0x80aa099 reads
    // renewState at +0x220 and logs "renew canceled"/"renew success", so the
    // switch below hands that arm the 3 it reads. 8 is NET_DONE, the publish. The boot leaves the check stage at
    // NONE and, as the probes showed, nothing in the image moves it from there:
    // every later stage comes from the machine's own arms and every entry into
    // INTERFACE comes from the NET_RENEW arm, which needs the stage at DONE (8)
    // first. Writing <n> whenever the stage is still NONE opens that door; once
    // the game has advanced the stage the seed is not applied again, so the
    // sequence is observed rather than held. Unset: nothing is written.
    if (checkStageSeed < 0)
    {
        const char *value = getenv("WMMT4_CHECK_STAGE_SEED");
        checkStageSeed = value ? atoi(value) : 0;
        if (checkStageSeed > 0)
            fprintf(stderr, "WMMT4: check stage seed %d from WMMT4_CHECK_STAGE_SEED (experiment)\n",
                    checkStageSeed);
    }
    if (checkStageSeed > 0 && impl->check == NET_NONE)
    {
        impl->check = checkStageSeed;
        seeds++;
        if (seeds <= 3 || seeds % 100 == 0)
            fprintf(stderr, "WMMT4: check stage NONE -> %d, seed %lu\n", checkStageSeed, seeds);
    }
    reportNetworkCheck(impl);
    // The game re-checks the cable about once a second (Busy meanwhile): a
    // cable once found stays plugged in.
    if (impl->cableState == STATE_GOOD)
        cableVerified = 1;
    else if (cableVerified)
        impl->cableState = STATE_GOOD;
    switch (impl->check)
    {
    case NET_CABLE:
    case NET_CABLE_RECHECK:
        impl->cableState = STATE_GOOD;
        break;
    case NET_GATEWAY:
        impl->gatewayState = STATE_GOOD;
        break;
    case NET_SHOP_ADDRESS:
        impl->resolveState = STATE_GOOD;
        break;
    // Stage 5 is the shop router check in the game's own text; the arm there
    // (0x80a9f8a) reads +0x238, the word this case writes.
    case NET_SHOP_ROUTER:
        impl->hops = 1;
        impl->hopsState = STATE_GOOD;
        break;
    case NET_SYNC_DATE:
        impl->syncDateState = STATE_GOOD;
        break;
    case NET_RENEW:
        impl->renewState = STATE_GOOD;
        break;
    case NET_TEST:
        impl->cableState = impl->shopRouterState = impl->contentRouterState = impl->hopsState = STATE_GOOD;
        break;
    case NET_WIFI:
        impl->wifiRouterState = STATE_GOOD;
        break;
    case NET_SET_DATE:
        impl->setDateState = STATE_GOOD;
        break;
    case NET_ONLINE:
        impl->onlineState = STATE_GOOD;
        break;
    case NET_NTP:
        impl->ntpState = STATE_GOOD;
        break;
    case NET_PACKAGE_FILE:
        impl->packageFileState = STATE_GOOD;
        break;
    }
    apply(impl, states);
}

// ---------------------------------------------------------------------------
// The loader's GL wrappers (glViewport, glEnable...) call glad's entry
// points, loaded once a context of the game's is current. The game makes GL
// calls on its window before it has one (no-ops on the cabinet's driver):
// they are loaded beforehand, from a context of the loader's own on a
// pbuffer. The entry points are the GL library's dispatch, good for the
// game's context too.

static void loadGlEntryPoints(void)
{
#if defined(__linux__)
    static const int attributes[] = {GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT, None};
    static const int pbufferAttributes[] = {GLX_PBUFFER_WIDTH, 16, GLX_PBUFFER_HEIGHT, 16, None};
    GLXContext (*createNewContext)(Display *, GLXFBConfig, int, GLXContext, Bool) =
        (GLXContext(*)(Display *, GLXFBConfig, int, GLXContext, Bool))dlsym(RTLD_NEXT, "glXCreateNewContext");
    Bool (*makeContextCurrent)(Display *, GLXDrawable, GLXDrawable, GLXContext) =
        (Bool(*)(Display *, GLXDrawable, GLXDrawable, GLXContext))dlsym(RTLD_NEXT, "glXMakeContextCurrent");
    Display *(*openDisplay)(const char *) = (Display * (*)(const char *)) dlsym(RTLD_NEXT, "XOpenDisplay");
    Display *dpy = openDisplay ? openDisplay(NULL) : NULL;
    GLXFBConfig *configs;
    int n = 0;

    if (!dpy || !createNewContext || !makeContextCurrent)
        return;
    if ((configs = glXChooseFBConfig(dpy, DefaultScreen(dpy), attributes, &n)) && n > 0)
    {
        GLXPbuffer pbuffer = glXCreatePbuffer(dpy, configs[0], pbufferAttributes);
        GLXContext context = createNewContext(dpy, configs[0], GLX_RGBA_TYPE, NULL, True);
        if (context && makeContextCurrent(dpy, pbuffer, pbuffer, context))
        {
            if (!gladLoadGL((GLADloadfunc)glXGetProcAddressARB))
                log_error("WMMT4: cannot load the GL entry points");
            makeContextCurrent(dpy, None, None, NULL);
        }
        if (context)
            glXDestroyContext(dpy, context);
        glXDestroyPbuffer(dpy, pbuffer);
    }
    if (configs)
        XFree(configs);
    XCloseDisplay(dpy);
#endif
}

// ---------------------------------------------------------------------------
// The shaders: Cg programs compiled for NVIDIA's GeForce 8 profiles (gp4vp,
// gp4fp: NV_gpu_program4 assembly, data/Shader/SHD_ALL_*.BIN), loaded as
// objects with the latest profile Cg finds. A driver without NV_gpu_program4
// (Mesa) gets them rewritten as ARB programs (namcoEs1Gp4Translate), under
// the ARB profiles; one that cannot be (the loops of a few post-effect
// filters) is a copy of its source picture.

#define CG_OBJECT 0x1011
#define CG_PROFILE_ARBVP1 6150
#define CG_PROFILE_ARBFP1 7000
#define CG_GL_VERTEX 8

static int (*realGetLatestProfile)(int);
static void *(*realCreateProgram)(void *, int, const char *, int, const char *, const char **);

__attribute__((force_align_arg_pointer)) static int getLatestProfile(int profileClass)
{
    if (GLAD_GL_NV_gpu_program4)
        return realGetLatestProfile(profileClass);
    return profileClass == CG_GL_VERTEX ? CG_PROFILE_ARBVP1 : CG_PROFILE_ARBFP1;
}

__attribute__((force_align_arg_pointer)) static void *createProgram(void *context, int type, const char *program,
                                                                    int profile, const char *entry, const char **args)
{
    static int translated, stubbed;
    int vertex;

    if (GLAD_GL_NV_gpu_program4 || type != CG_OBJECT || !program ||
        (strncmp(program, "!!NVvp4.0", 9) && strncmp(program, "!!NVfp4.0", 9)))
        return realCreateProgram(context, type, program, profile, entry, args);
    vertex = program[4] == 'v';
    char *arb = namcoEs1Gp4Translate(program, strlen(program));
    if (arb)
        translated++;
    else
    {
        stubbed++;
        log_warn("WMMT4: a %s program could not be rewritten for this driver: its source picture instead",
                 vertex ? "vertex" : "fragment");
        arb = strdup(vertex ? "!!ARBvp1.0\nMOV result.position, vertex.position;\nEND\n"
                            : "!!ARBfp1.0\nTEX result.color, fragment.texcoord[0], texture[0], 2D;\nEND\n");
    }
    void *result = realCreateProgram(context, type, arb, vertex ? CG_PROFILE_ARBVP1 : CG_PROFILE_ARBFP1, entry, args);
    free(arb);
    if ((translated + stubbed) % 100 == 0)
        log_info("WMMT4: %d NV_gpu_program4 programs rewritten as ARB ones, %d replaced", translated, stubbed);
    return result;
}

// ---------------------------------------------------------------------------
// The window: the game covers the whole screen with an override-redirect
// window of the root's size (then resized to it again), and asks the window
// manager for full screen. Unless [Display]
// FULLSCREEN is set, it is an ordinary window of [Display] WIDTH x HEIGHT
// instead; the game renders at its window's size.

#if defined(__linux__)

// The game opens ":0.0" whatever $DISPLAY says: it gets the display the
// loader is run on (a virtual one in tests).
static Display *(*realOpenDisplay)(const char *);

__attribute__((force_align_arg_pointer)) static Display *openDisplay(const char *name)
{
    return realOpenDisplay(NULL);
}

static Window (*realCreateWindow)(Display *, Window, int, int, unsigned, unsigned, unsigned, int, unsigned, Visual *,
                                  unsigned long, XSetWindowAttributes *);
static int (*realMoveResizeWindow)(Display *, Window, int, int, unsigned, unsigned);
static Window gameWindow;

static void windowSize(unsigned *width, unsigned *height)
{
    int w = getConfig()->width, h = getConfig()->height;
    *width = w > 0 ? w : 1360;
    *height = h > 0 ? h : 768;
}

__attribute__((force_align_arg_pointer)) static Window createWindow(Display *dpy, Window parent, int x, int y,
                                                                    unsigned width, unsigned height, unsigned border,
                                                                    int depth, unsigned cls, Visual *visual,
                                                                    unsigned long mask,
                                                                    XSetWindowAttributes *attributes)
{
    int topLevel = parent == DefaultRootWindow(dpy);

    if (topLevel && !getConfig()->fullscreen)
    {
        windowSize(&width, &height);
        if (attributes)
            attributes->override_redirect = False;
        mask &= ~CWOverrideRedirect;
    }
    Window window = realCreateWindow(dpy, parent, x, y, width, height, border, depth, cls, visual, mask, attributes);
    if (window && topLevel)
    {
        // This process's window, for the loader's quit watch (Esc).
        long pid = getpid();
        XChangeProperty(dpy, window, XInternAtom(dpy, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *)&pid, 1);
        XStoreName(dpy, window, "Wangan Midnight Maximum Tune 4");
        gameWindow = window;
    }
    return window;
}

__attribute__((force_align_arg_pointer)) static int moveResizeWindow(Display *dpy, Window window, int x, int y,
                                                                     unsigned width, unsigned height)
{
    if (window == gameWindow && !getConfig()->fullscreen)
    {
        windowSize(&width, &height);
        return XResizeWindow(dpy, window, width, height);
    }
    return realMoveResizeWindow(dpy, window, x, y, width, height);
}

// The size it renders at comes from the root window's attributes: the
// window's, when that is not the screen.
static Status (*realGetWindowAttributes)(Display *, Window, XWindowAttributes *);

__attribute__((force_align_arg_pointer)) static Status getWindowAttributes(Display *dpy, Window window,
                                                                           XWindowAttributes *attributes)
{
    Status status = realGetWindowAttributes(dpy, window, attributes);
    if (status && attributes && !getConfig()->fullscreen &&
        (window == DefaultRootWindow(dpy) || window == gameWindow))
    {
        unsigned width, height;
        windowSize(&width, &height);
        attributes->width = width;
        attributes->height = height;
    }
    return status;
}

// Its request to the window manager for full screen (_NET_WM_STATE), too.
static int (*realChangeProperty)(Display *, Window, Atom, Atom, int, int, const unsigned char *, int);

__attribute__((force_align_arg_pointer)) static int changeProperty(Display *dpy, Window window, Atom property,
                                                                   Atom type, int format, int mode,
                                                                   const unsigned char *data, int elements)
{
    if (window == gameWindow && !getConfig()->fullscreen && property == XInternAtom(dpy, "_NET_WM_STATE", False))
        return 1;
    return realChangeProperty(dpy, window, property, type, format, mode, data, elements);
}

#endif /* the game's window */

// ---------------------------------------------------------------------------
// The frame pacing: the game waits for each vertical retrace with
// GLX_SGI_video_sync (got through glXGetProcAddress). Mesa's, on XWayland,
// blocks for good while the window is not being presented (behind another):
// the game stopped there. A 60 Hz clock of the loader's instead (the
// cabinet's monitor's rate, whatever the host's).

#define FRAME_RATE 60.0

// The clock in frames (doubles: the loader has no 64-bit division).
static double nowFrames(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec + ts.tv_nsec / 1e9) * FRAME_RATE;
}

__attribute__((force_align_arg_pointer)) static int getVideoSync(unsigned *count)
{
    if (count)
        *count = (unsigned)nowFrames();
    return 0;
}

__attribute__((force_align_arg_pointer)) static int waitVideoSync(int divisor, int remainder, unsigned *count)
{
    unsigned frame = (unsigned)nowFrames() + 1;

    if (divisor > 1)
        while (frame % (unsigned)divisor != (unsigned)remainder % (unsigned)divisor)
            frame++;
    double until = frame / FRAME_RATE;
    struct timespec ts = {(time_t)until, (long)((until - (time_t)until) * 1e9)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0)
        ;
    if (count)
        *count = frame;
    return 0;
}

// NV_fence, which the game syncs its texture uploads with (its 2D layouts
// and menus came out as stale pieces of earlier frames): a driver without it
// (Mesa) gives functions that do nothing, a fence never completing. The
// loader's emulation on sync objects instead (graphics/shaderPatches.c),
// under a lock: the game's threads share it.
#define GL_FENCE_STATUS_NV 0x84F3
#define GL_FENCE_CONDITION_NV 0x84F4
#define GL_ALL_COMPLETED_NV 0x84F2

static pthread_mutex_t fenceLock = PTHREAD_MUTEX_INITIALIZER;

__attribute__((force_align_arg_pointer)) static void genFences(GLsizei n, GLuint *fences)
{
    pthread_mutex_lock(&fenceLock);
    bridgeglGenFencesNV(n, fences);
    pthread_mutex_unlock(&fenceLock);
}

__attribute__((force_align_arg_pointer)) static void deleteFences(GLsizei n, const GLuint *fences)
{
    pthread_mutex_lock(&fenceLock);
    bridgeglDeleteFencesNV(n, fences);
    pthread_mutex_unlock(&fenceLock);
}

__attribute__((force_align_arg_pointer)) static void setFence(GLuint fence, GLenum condition)
{
    pthread_mutex_lock(&fenceLock);
    bridgeglSetFenceNV(fence, condition);
    pthread_mutex_unlock(&fenceLock);
}

__attribute__((force_align_arg_pointer)) static GLboolean testFence(GLuint fence)
{
    pthread_mutex_lock(&fenceLock);
    GLboolean done = bridgeglTestFenceNV(fence);
    pthread_mutex_unlock(&fenceLock);
    return done;
}

__attribute__((force_align_arg_pointer)) static void finishFence(GLuint fence)
{
    pthread_mutex_lock(&fenceLock);
    bridgeglFinishFenceNV(fence);
    pthread_mutex_unlock(&fenceLock);
}

__attribute__((force_align_arg_pointer)) static GLboolean isFence(GLuint fence)
{
    pthread_mutex_lock(&fenceLock);
    GLboolean is = bridgeglIsFenceNV(fence);
    pthread_mutex_unlock(&fenceLock);
    return is;
}

__attribute__((force_align_arg_pointer)) static void getFenceiv(GLuint fence, GLenum name, GLint *value)
{
    if (!value)
        return;
    if (name == GL_FENCE_STATUS_NV)
        *value = testFence(fence);
    else if (name == GL_FENCE_CONDITION_NV)
        *value = GL_ALL_COMPLETED_NV;
}

static const struct
{
    const char *name;
    void *function;
} fenceFunctions[] = {
    {"glGenFencesNV", (void *)genFences},   {"glDeleteFencesNV", (void *)deleteFences},
    {"glSetFenceNV", (void *)setFence},     {"glTestFenceNV", (void *)testFence},
    {"glFinishFenceNV", (void *)finishFence}, {"glIsFenceNV", (void *)isFence},
    {"glGetFenceivNV", (void *)getFenceiv},
};

static void *(*realGetProcAddress)(const char *);

__attribute__((force_align_arg_pointer)) static void *getProcAddress(const char *name)
{
    if (name && !strcmp(name, "glXGetVideoSyncSGI"))
        return (void *)getVideoSync;
    if (name && !strcmp(name, "glXWaitVideoSyncSGI"))
        return (void *)waitVideoSync;
    for (size_t i = 0; name && !GLAD_GL_NV_fence && i < sizeof(fenceFunctions) / sizeof(fenceFunctions[0]); i++)
        if (!strcmp(name, fenceFunctions[i].name))
            return fenceFunctions[i].function;
    return realGetProcAddress(name);
}

// ---------------------------------------------------------------------------
// The cabinet LAN (multicast 225.0.0.1, the terminal's and other cabinets'
// announcements): kept on the loopback interface, where the loader's
// terminal sends. The game joins the group on the default interface, where a
// firewall drops the looped datagrams (and the LAN would reach the network):
// its sockets join on loopback too and send there.

// WMMT4_BOOT_TRACE (see the boot trace below).

static int (*realSetsockopt)(int, int, int, const void *, socklen_t);

__attribute__((force_align_arg_pointer)) static int gameSetsockopt(int fd, int level, int name, const void *value,
                                                                   socklen_t length)
{
    int result = realSetsockopt(fd, level, name, value, length);

    if (result == 0 && level == IPPROTO_IP && name == IP_ADD_MEMBERSHIP && value && length >= sizeof(struct ip_mreq))
    {
        struct ip_mreq request = *(const struct ip_mreq *)value;
        struct in_addr loopback = {htonl(INADDR_LOOPBACK)};
        unsigned char one = 1;
        if (request.imr_interface.s_addr != loopback.s_addr)
        {
            request.imr_interface = loopback;
            realSetsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, sizeof(request));
        }
        realSetsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &loopback, sizeof(loopback));
        realSetsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof(one));
    }
    return result;
}

// On loopback every announcement comes from 127.0.0.1, while the cabinet
// tells its peers apart by address and recognises its own datagram by its
// eth0's (192.168.92.11, see interfaceUpdate): the source the game reads is
// that of the sender's PCB (the packet's second byte) on that network,
// 192.168.92.11 + id (the terminal, 4: .15).
#define LAN_PORT 50765

static void lanSource(struct sockaddr_in *from, const uint8_t *data, ssize_t n)
{
    if (!from || n < 2 || from->sin_family != AF_INET || from->sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
        from->sin_port != htons(LAN_PORT) || data[0] != 1)
        return;
    from->sin_addr.s_addr = htonl(0xc0a85c0b + data[1]); // 192.168.92.11 + PCB id
}

static ssize_t (*realRecvmsg)(int, struct msghdr *, int);
static ssize_t (*realRecvfrom)(int, void *, size_t, int, struct sockaddr *, socklen_t *);

__attribute__((force_align_arg_pointer)) static ssize_t gameRecvmsg(int fd, struct msghdr *msg, int flags)
{
    ssize_t n = realRecvmsg(fd, msg, flags);
    if (n > 0 && authTrace && msg && msg->msg_iovlen > 0)
    {
        // Once per receiving function: the cabinet LAN's reader is the one
        // the terminal's handshake goes through.
        static void *readers[8];
        static int readersSeen;
        void *reader = __builtin_return_address(0);
        int i;

        for (i = 0; i < readersSeen && readers[i] != reader; i++)
            ;
        if (i == readersSeen && readersSeen < 8)
            readers[readersSeen++] = reader;
        if (i == readersSeen - 1 || i == readersSeen)
            fprintf(stderr, "WMMT4 authtrace: recvmsg by %p, %zd bytes, %02x %02x\n", reader, n,
                    ((const uint8_t *)msg->msg_iov[0].iov_base)[0],
                    msg->msg_iov[0].iov_len > 1 ? ((const uint8_t *)msg->msg_iov[0].iov_base)[1] : 0);
    }
    if (n > 0 && msg && msg->msg_name && msg->msg_namelen >= sizeof(struct sockaddr_in) && msg->msg_iovlen > 0)
        lanSource(msg->msg_name, msg->msg_iov[0].iov_base,
                  n < (ssize_t)msg->msg_iov[0].iov_len ? n : (ssize_t)msg->msg_iov[0].iov_len);
    return n;
}

__attribute__((force_align_arg_pointer)) static ssize_t gameRecvfrom(int fd, void *buffer, size_t size, int flags,
                                                                     struct sockaddr *from, socklen_t *length)
{
    ssize_t n = realRecvfrom(fd, buffer, size, flags, from, length);
    if (n > 0 && authTrace)
    {
        const struct sockaddr_in *source = (const struct sockaddr_in *)from;
        fprintf(stderr, "WMMT4 authtrace: recvfrom %zd bytes from %s:%u, %02x %02x\n", n,
                source ? inet_ntoa(source->sin_addr) : "-", source ? ntohs(source->sin_port) : 0,
                ((const uint8_t *)buffer)[0], size > 1 ? ((const uint8_t *)buffer)[1] : 0);
    }
    if (n > 0 && from && length && *length >= sizeof(struct sockaddr_in))
        lanSource((struct sockaddr_in *)from, buffer, n);
    return n;
}

// ---------------------------------------------------------------------------
// WMMT4_BOOT_TRACE: Sys::Monitor::Client talks to its server through the two
// pipes /Sys.Monitor.Command and /Sys.Monitor.Reply (both ends are in the
// game: Client and Server are threads of it). A write is logged before the
// call, so a write that never returns (no reader at the other end) shows as an
// attempt with no result; that is what the boot's 認証 row waits on when the
// client never posts. The pipes are recognised by the fd's own path, so the
// fds need no guessing: an fd is looked up once, and only a monitor one is
// remembered (a reused number cannot hide a pipe).

static ssize_t (*realWrite)(int, const void *, size_t);
static unsigned writeCalls; // positive control: the hook is running at all

static int monitorPipe(int fd)
{
    static int known[64];
    char path[48], target[64];
    ssize_t n;
    int kind = 0;

    if (!bootTrace || fd < 0)
        return 0;
    if (fd < 64 && known[fd])
        return known[fd];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    n = readlink(path, target, sizeof(target) - 1);
    if (n > 0)
    {
        target[n] = '\0';
        if (strstr(target, "Sys.Monitor.Command"))
            kind = 1;
        else if (strstr(target, "Sys.Monitor.Reply"))
            kind = 2;
    }
    if (kind && fd < 64)
        known[fd] = kind;
    return kind;
}

__attribute__((force_align_arg_pointer)) static ssize_t gameWrite(int fd, const void *buffer, size_t size)
{
    static const char *const names[] = {"?", "Command", "Reply"};
    static unsigned calls[3], bytes[3];
    int kind;
    ssize_t n;

    // The boards' descriptors, whose writes are the game's packets.
    if (namcoEs1JvsIsFd(fd))
        return namcoEs1JvsWrite(buffer, size);
    if (namcoEs1KickbackIsFd(fd))
        return namcoEs1KickbackWrite(buffer, size);
    kind = monitorPipe(fd);
    writeCalls++;
    if (bootTrace && (writeCalls <= 5 || writeCalls % 2000 == 0))
    {
        char path[48], target[64];
        ssize_t link;

        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        link = readlink(path, target, sizeof(target) - 1);
        target[link > 0 ? link : 0] = '\0';
        fprintf(stderr, "WMMT4 boot: write %u on fd %d (%s)\n", writeCalls, fd, link > 0 ? target : "?");
    }
    if (kind)
    {
        calls[kind]++;
        if (calls[kind] <= 8 || calls[kind] % 50 == 0)
        {
            const unsigned char *data = (const unsigned char *)buffer;
            size_t i, shown = buffer ? (size < 16 ? size : 16) : 0;
            char hex[3 * 16 + 1];

            for (i = 0; i < shown; i++)
                snprintf(hex + 3 * i, 4, "%02x ", data[i]);
            hex[3 * shown] = '\0';
            fprintf(stderr, "WMMT4 boot: monitor %s write %u, %zu bytes: %s\n", names[kind], calls[kind], size,
                    hex);
        }
    }
    n = realWrite(fd, buffer, size);
    if (kind)
    {
        if (n > 0)
            bytes[kind] += (unsigned)n;
        if (n != (ssize_t)size && (calls[kind] <= 8 || calls[kind] % 50 == 0))
            fprintf(stderr, "WMMT4 boot: monitor %s write %u -> %zd of %zu (total %u)\n", names[kind], calls[kind],
                    n, size, bytes[kind]);
    }
    return n;
}

// WMMT4_BOOT_TRACE: who holds the two pipes, and from where. A pipe's ends
// are told apart by the access its opener asked for (flags & 3 is O_ACCMODE):
// the server reads Command and writes Reply, the client the other way. The
// caller of open() is the code that opens that end, and is printed as the
// address it returns to, so the start path is named by measurement rather
// than by a guess. open is variadic: a call without a mode passes one on
// anyway, which a call without O_CREAT does not look at.

static int (*realOpen)(const char *, int, int);
static int (*realOpen64)(const char *, int, int);

static const char *monitorName(const char *path)
{
    if (!path)
        return NULL;
    if (strstr(path, "Sys.Monitor.Command"))
        return "Command";
    if (strstr(path, "Sys.Monitor.Reply"))
        return "Reply";
    return NULL;
}

static void openSeen(const char *path, int flags, int fd, void *caller)
{
    const char *name = monitorName(path);
    const char *end;

    if (!name || !bootTrace)
        return;
    end = (flags & 3) == 0 ? "read end" : (flags & 3) == 1 ? "write end" : "read/write end";
    fprintf(stderr, "WMMT4 boot: open %s flags %#x = fd %d, %s, caller %p\n", name, flags, fd, end, caller);
}

// The boards' serial ports, by any of their names (namcoEs1JvsIsPath,
// namcoEs1KickbackIsPath): the loader's boards, as they are for every other
// game through the shared open hook. WMMT4 replaces the executable's own
// open() import, so it never reaches that hook and asks here. realOpen is the
// game's import, fixed-argument: the variadic one a board's opener takes is
// this.
static int boardRealOpen(const char *path, int flags, ...)
{
    return realOpen(path, flags, 0);
}

__attribute__((force_align_arg_pointer)) static int gameOpen(const char *path, int flags, int mode)
{
    int fd = namcoEs1JvsIsPath(path)           ? namcoEs1JvsOpen(boardRealOpen)
             : namcoEs1KickbackIsPath(path) ? namcoEs1KickbackOpen(boardRealOpen)
                                            : realOpen(path, flags, mode);
    openSeen(path, flags, fd, __builtin_return_address(0));
    return fd;
}

__attribute__((force_align_arg_pointer)) static int gameOpen64(const char *path, int flags, int mode)
{
    int fd = namcoEs1JvsIsPath(path)           ? namcoEs1JvsOpen(boardRealOpen)
             : namcoEs1KickbackIsPath(path) ? namcoEs1KickbackOpen(boardRealOpen)
                                            : realOpen64(path, flags, mode);
    openSeen(path, flags, fd, __builtin_return_address(0));
    return fd;
}

static ssize_t (*realRead)(int, void *, size_t);

__attribute__((force_align_arg_pointer)) static ssize_t gameRead(int fd, void *buffer, size_t size)
{
    ssize_t n;

    // The boards' descriptors, whose reads are their queued replies (the same
    // diversion the shared read hook makes for the other games).
    if (namcoEs1JvsIsFd(fd))
        return namcoEs1JvsRead(buffer, size);
    if (namcoEs1KickbackIsFd(fd))
        return namcoEs1KickbackRead(buffer, size);
    n = realRead(fd, buffer, size);
    int kind = monitorPipe(fd);

    if (kind && bootTrace)
    {
        const unsigned char *data = (const unsigned char *)buffer;

        fprintf(stderr, "WMMT4 boot: monitor %s read %zd of %zu: %02x %02x\n", kind == 1 ? "Command" : "Reply", n,
                size, n > 0 ? data[0] : 0, n > 1 ? data[1] : 0);
    }
    return n;
}

// WMMT4_BOOT_TRACE: the pipes are held through stdio, not through open() and
// write() (the game imports the whole set and neither of those two is ever
// called on them): fopen("/Sys.Monitor.Command") is where a client opens its
// end, and its caller is the start path that would post the first command -
// logged as the address it returns to, so the path is named by measurement.
// A FILE* of a pipe is remembered by its kind, and the traffic on it (fputs,
// fwrite, fflush, fgets, fread) is logged with it.

static FILE *(*realFopen)(const char *, const char *);
static FILE *(*realFopen64)(const char *, const char *);
static size_t (*realFwrite)(const void *, size_t, size_t, FILE *);
static size_t (*realFread)(void *, size_t, size_t, FILE *);
static int (*realFputs)(const char *, FILE *);
static char *(*realFgets)(char *, int, FILE *);
static int (*realFflush)(FILE *);
static int (*realFclose)(FILE *);

static FILE *monitorFiles[4];
static int monitorKinds[4];

static int monitorKind(FILE *file)
{
    for (int i = 0; file && i < 4; i++)
        if (monitorFiles[i] == file)
            return monitorKinds[i];
    return 0;
}

static const char *monitorKindName(int kind)
{
    return kind == 1 ? "Command" : kind == 2 ? "Reply" : "?";
}

static void monitorFileAdd(FILE *file, int kind)
{
    for (int i = 0; file && i < 4; i++)
        if (!monitorFiles[i])
        {
            monitorFiles[i] = file;
            monitorKinds[i] = kind;
            return;
        }
}

static void monitorFileDrop(FILE *file)
{
    for (int i = 0; i < 4; i++)
        if (file && monitorFiles[i] == file)
        {
            monitorFiles[i] = NULL;
            monitorKinds[i] = 0;
        }
}

static void monitorTraffic(const char *what, FILE *file, const void *data, size_t n)
{
    int kind = monitorKind(file);
    size_t i, shown = n < 16 ? n : 16;
    char hex[3 * 16 + 1];

    if (!kind || !bootTrace)
        return;
    for (i = 0; data && i < shown; i++)
        snprintf(hex + 3 * i, 4, "%02x ", ((const unsigned char *)data)[i]);
    hex[3 * shown] = '\0';
    fprintf(stderr, "WMMT4 boot: monitor %s %s %zu bytes on %p: %s\n", monitorKindName(kind), what, n, (void *)file,
            hex);
}

static unsigned fopenCalls; // positive control: the stdio surface is hooked

static FILE *fopenSeen(const char *path, const char *mode, FILE *file, void *caller)
{
    const char *name = monitorName(path);

    fopenCalls++;
    if (bootTrace && !name && fopenCalls <= 8)
        fprintf(stderr, "WMMT4 boot: fopen %u %s (caller %p)\n", fopenCalls, path ? path : "?", caller);
    if (name && bootTrace)
    {
        int kind = strcmp(name, "Command") == 0 ? 1 : 2;

        monitorFileAdd(file, kind);
        fprintf(stderr, "WMMT4 boot: fopen %s \"%s\" = %p, caller %p\n", name, mode ? mode : "?", (void *)file,
                caller);
    }
    return file;
}

__attribute__((force_align_arg_pointer)) static FILE *gameFopen(const char *path, const char *mode)
{
    return fopenSeen(path, mode, realFopen(path, mode), __builtin_return_address(0));
}

__attribute__((force_align_arg_pointer)) static FILE *gameFopen64(const char *path, const char *mode)
{
    return fopenSeen(path, mode, realFopen64(path, mode), __builtin_return_address(0));
}

__attribute__((force_align_arg_pointer)) static size_t gameFwrite(const void *data, size_t size, size_t count,
                                                                 FILE *file)
{
    size_t n = realFwrite(data, size, count, file);
    monitorTraffic("fwrite", file, data, n * size);
    return n;
}

__attribute__((force_align_arg_pointer)) static int gameFputs(const char *text, FILE *file)
{
    static unsigned calls;
    int result = realFputs(text, file);

    if (bootTrace && ++calls <= 3)
        fprintf(stderr, "WMMT4 boot: fputs %u on %p, %zu bytes\n", calls, (void *)file, text ? strlen(text) : 0);
    monitorTraffic("fputs", file, text, text ? strlen(text) : 0);
    return result;
}

__attribute__((force_align_arg_pointer)) static int gameFflush(FILE *file)
{
    monitorTraffic("fflush", file, NULL, 0);
    return realFflush(file);
}

__attribute__((force_align_arg_pointer)) static size_t gameFread(void *data, size_t size, size_t count, FILE *file)
{
    size_t n = realFread(data, size, count, file);
    monitorTraffic("fread", file, data, n * size);
    return n;
}

__attribute__((force_align_arg_pointer)) static char *gameFgets(char *text, int size, FILE *file)
{
    char *result = realFgets(text, size, file);
    if (result)
        monitorTraffic("fgets", file, text, strlen(text));
    return result;
}

__attribute__((force_align_arg_pointer)) static int gameFclose(FILE *file)
{
    monitorTraffic("fclose", file, NULL, 0);
    monitorFileDrop(file);
    return realFclose(file);
}

// ---------------------------------------------------------------------------
// WMMT4_SHOT=<directory> (debugging): a frame every 4 s (WMMT4_SHOT_PERIOD,
// 0 for none) saved there as a PPM, read back before the swap (a capture of
// the window comes out black); touching <directory>/grab saves the next one
// as grab.ppm.

static const char *shotDir;
static int shotPeriod = 4; // WMMT4_SHOT_PERIOD, 0: on request only
// The shots are taken before the game's own glXSwapBuffers: elsewhere there
// is no such call to take them in (the frame the game draws is the loader's
// window's).
#if defined(__linux__)
static void (*realSwapBuffers)(Display *, GLXDrawable);

__attribute__((force_align_arg_pointer)) static void swapBuffers(Display *dpy, GLXDrawable drawable)
{
    static double next;
    static int count;
    double now = nowFrames() / FRAME_RATE;
    unsigned width, height;

    char path[512];
    snprintf(path, sizeof(path), "%s/grab", shotDir);
    int grab = access(path, F_OK) == 0;
    if (grab)
        unlink(path);
    if ((grab || (shotPeriod && now >= next)) && glad_glReadPixels)
    {
        next = now + shotPeriod;
        if (getConfig()->fullscreen)
        {
            width = DisplayWidth(dpy, DefaultScreen(dpy));
            height = DisplayHeight(dpy, DefaultScreen(dpy));
        }
        else
            windowSize(&width, &height);
        unsigned char *pixels = malloc((size_t)width * height * 3);
        if (grab)
            snprintf(path, sizeof(path), "%s/grab.ppm", shotDir);
        else
            snprintf(path, sizeof(path), "%s/wmmt4_%03d.ppm", shotDir, count++);
        FILE *f = pixels ? fopen(path, "wb") : NULL;
        if (f)
        {
            glad_glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glad_glReadBuffer(GL_BACK);
            glad_glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels);
            fprintf(f, "P6\n%u %u\n255\n", width, height);
            for (unsigned y = height; y-- > 0;)
                fwrite(pixels + (size_t)y * width * 3, 1, (size_t)width * 3, f);
            fclose(f);
        }
        free(pixels);
    }
    realSwapBuffers(dpy, drawable);
}
#endif

// ---------------------------------------------------------------------------
// The network checks' commands (run as root, not here): the link is up and
// the shop's router answers one hop away, else the game stays offline
// ("cable/gateway is unknown").

const char *namcoEs1Wmmt4CommandOutput(const char *command)
{
    if (strstr(command, "ethtool"))
        return "Settings for eth0:\n\tSpeed: 100Mb/s\n\tDuplex: Full\n\tLink detected: yes\n";
    if (strstr(command, "traceroute"))
    {
        static char hop[128];
        const char *target = strstr(command, "-m 99 ");
        unsigned a, b, c, d;
        if (!target || sscanf(target + 6, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
            return NULL;
        snprintf(hop, sizeof(hop), " 1  %u.%u.%u.%u  0.215 ms  0.180 ms  0.172 ms\n", a, b, c, d);
        return hop;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// The panel (as the Pacloader fork maps it): the loader's player 1 buttons
// become the cabinet's switches. Button 1 is Enter, 2 view change (switch
// 9), 5 intrude (switch 10); 3 and 4 shift up and down a gear. The cabinet's
// shifter is an H pattern whose gears each close two switches: 5 the left
// column (1, 2), 6 the right one (5, 6), 3 the top row (1, 3, 5), 4 the
// bottom one (2, 4, 6); neutral closes none. It starts in neutral; shifting
// up first selects first, and down never goes below it. Switch 2 (a terminal
// cabinet) stays open.

void namcoEs1Wmmt4Panel(JVSIO *io)
{
    static int gear, lastUp, lastDown;
    int in = io->state.inputSwitch[PLAYER_1];
    int up = (in & BUTTON_3) != 0, down = (in & BUTTON_4) != 0;

    if (up && !lastUp && gear < 6)
        gear++;
    if (down && !lastDown && gear > 1)
        gear--;
    lastUp = up;
    lastDown = down;

    int out = in & ~(BUTTON_2 | BUTTON_3 | BUTTON_4 | BUTTON_5 | BUTTON_6 | BUTTON_7 | BUTTON_8 | BUTTON_9 | BUTTON_10);
    if (in & BUTTON_2)
        out |= BUTTON_9;
    if (in & BUTTON_5)
        out |= BUTTON_10;
    if (gear == 1 || gear == 2)
        out |= BUTTON_5;
    if (gear == 5 || gear == 6)
        out |= BUTTON_6;
    if (gear == 1 || gear == 3 || gear == 5)
        out |= BUTTON_3;
    if (gear == 2 || gear == 4 || gear == 6)
        out |= BUTTON_4;
    io->state.inputSwitch[PLAYER_1] = out;
}

// ---------------------------------------------------------------------------
// The steering wheel's force feedback, as the Pacloader fork takes it
// (es1Wmmt4Ffb.cpp): the game's force feedback object hands its output to
// its steering board driver once a frame (force, spring, damper and
// vibration, floats), which goes to the evdev wheel while the game has the
// motor powered. Its power on/off is the board's motor power too (the
// cabinet switches it through its I/O board: State_PowerOffWait fails with
// E2212 otherwise), and its self-check start completes the driver's check.
// The vibration is not played (no such effect on evdev here).
// NAMCO_FFB_TRACE set: the output on stderr every second.

typedef void (*FfbApply)(void *, float, float, float, float);
typedef void (*FfbPower)(void *, int);
typedef void (*FfbSelfCheck)(void *);

static FfbApply realFfbApply;
static FfbPower realFfbPowerOn, realFfbPowerOff;
static FfbSelfCheck realFfbSelfCheck;
static volatile int ffbPowered, ffbTrace;

static float clampUnit(float value)
{
    return value > 1.0f ? 1.0f : value < -1.0f ? -1.0f : value;
}

static void ffbOff(void)
{
    evdevFfbConstant(0);
    evdevFfbSpring(0);
    evdevFfbDamper(0);
}

__attribute__((force_align_arg_pointer)) static void ffbApply(void *object, float force, float spring, float damper,
                                                              float vibration)
{
    static unsigned frames;

    realFfbApply(object, force, spring, damper, vibration);
    if (ffbTrace && frames++ % 60 == 0)
        fprintf(stderr, "WMMT4 FFB: %s force %.3f spring %.3f damper %.3f vibration %.3f\n",
                ffbPowered ? "on" : "off", force, spring, damper, vibration);
    if (!ffbPowered)
        return;
    evdevFfbConstant(clampUnit(force));
    evdevFfbSpring(clampUnit(spring < 0 ? 0 : spring));
    evdevFfbDamper(clampUnit(damper < 0 ? 0 : damper));
}

__attribute__((force_align_arg_pointer)) static void ffbPowerOn(void *object, int running)
{
    namcoEs1KickbackReportMotorPower(1);
    realFfbPowerOn(object, running);
    ffbPowered = (running & 0xff) != 0;
}

__attribute__((force_align_arg_pointer)) static void ffbPowerOff(void *object, int running)
{
    namcoEs1KickbackReportMotorPower(0);
    realFfbPowerOff(object, running);
    ffbPowered = 0;
    ffbOff();
}

// The driver's check, completed as the board's own answer would (the
// fork's completeVirtualSelfCheck): its driver is at +4 in the object.
__attribute__((force_align_arg_pointer)) static void ffbSelfCheck(void *object)
{
    static const uint8_t completeEntry[] = {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0xc6, 0x40, 0x40, 0x00};
    int (*complete)(void *) = (int (*)(void *))0x08366230;

    realFfbSelfCheck(object);
    if (object && *(void **)((uint8_t *)object + 4) &&
        !memcmp((const void *)(uintptr_t)0x08366230, completeEntry, sizeof(completeEntry)))
    {
        complete(*(void **)((uint8_t *)object + 4));
        ((uint8_t *)object)[0x50] = 0;
    }
    namcoEs1KickbackReportSelfCheck();
}

static void hookWithOriginal(uint32_t address, const uint8_t *expected, size_t n, void *replacement, void *original)
{
    if (memcmp((const void *)(uintptr_t)address, expected, n) != 0)
    {
        log_error("WMMT4: unexpected code at %#x, not hooked", address);
        return;
    }
    if (MH_CreateHook((void *)(uintptr_t)address, replacement, (void **)original) != MH_OK ||
        MH_EnableHook((void *)(uintptr_t)address) != MH_OK)
        log_error("WMMT4: cannot hook %#x", address);
}

static void installFfb(void)
{
    static const uint8_t applyEntry[] = {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x8b, 0x40,
                                         0x04, 0x89, 0x45, 0x08, 0x5d, 0xe9};
    static const uint8_t powerEntry[] = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x08, 0x0f, 0xb6,
                                         0x45, 0x0c, 0x89, 0x45, 0x0c, 0x8b, 0x45, 0x08};
    static const uint8_t selfCheckEntry[] = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x08, 0x8b, 0x45,
                                             0x08, 0xc6, 0x40, 0x50, 0x01, 0x8b, 0x40, 0x04};

    ffbTrace = getenv("NAMCO_FFB_TRACE") != NULL;
    MH_Initialize(); // already done is fine
    hookWithOriginal(0x08366e30, applyEntry, sizeof(applyEntry), (void *)ffbApply, &realFfbApply);
    hookWithOriginal(0x08367aa0, powerEntry, sizeof(powerEntry), (void *)ffbPowerOn, &realFfbPowerOn);
    hookWithOriginal(0x08367ac0, powerEntry, sizeof(powerEntry), (void *)ffbPowerOff, &realFfbPowerOff);
    hookWithOriginal(0x08367ee0, selfCheckEntry, sizeof(selfCheckEntry), (void *)ffbSelfCheck, &realFfbSelfCheck);
    evdevFfbOpen("WMMT4");
}

// The board's card-vendor task (the card reader behind the JVS board). It never
// writes a request of its own over the board's serial pass-through: it only
// polls for the reader's reports, so without them the drive check ends in E2301
// ("IC card Vendor disconnect error") and the game stops on "call the
// attendant". The reader's answers are the JVS emulation's (jvs.c), and these
// states are where the vendor asks for each of them.
static void (*realCardVendorResetWait)(void *);
static void (*realCardVendorResetReply)(void *);
static void (*realCardVendorStatusWait)(void *);
static int (*realCardVendorPoll)(void *, void *);
static int (*realBoardReadAvail)(void *, int);
static int cardVendorTrace;
static unsigned long cardVendorTicks;

// The report the reader hands over, by state (the JVS emulation's list, which
// WMMT4_VENDOR_FRAME can replace for testing): 0 is the status report, 1 the
// result of the reset the vendor asked the reader for.
extern int namcoJvsVendorReport;

// The reader answers when the vendor enters the state that asks it something,
// not on every tick of that state. The object's state pointer is how that is
// seen: it is already the new state when the state's own function runs, and it
// changes when the machine moves on (to states that ask nothing, as most do),
// so counting those changes tells a state's first tick from a later one.
static void *cardVendorSeenState;
static unsigned long cardVendorStateGeneration;
static uint32_t cardVendorAnsweredState;
static unsigned long cardVendorAnsweredGeneration;

static void cardVendorObserveState(void *self)
{
    void *state = self ? *(void **)self : NULL;

    if (state == cardVendorSeenState)
        return;
    cardVendorSeenState = state;
    cardVendorStateGeneration++;
}

static void cardVendorEnterState(void *self, uint32_t state, int report)
{
    if (!self)
        return;
    cardVendorObserveState(self);
    if (*(void **)self != (void *)state)
        return;
    // Already answered this entry: a later tick of the same state. A state the
    // machine has since come back to is a new entry, and asks again (a card
    // read, say, after the startup check has moved on and returned).
    if (cardVendorAnsweredState == state && cardVendorAnsweredGeneration == cardVendorStateGeneration)
        return;
    cardVendorAnsweredState = state;
    cardVendorAnsweredGeneration = cardVendorStateGeneration;
    namcoJvsVendorReport = report;
    if (cardVendorTrace)
        fprintf(stderr, "WMMT4: card vendor: state %#x, report %d handed over\n", state, report);
}

// state_ResetStatusWait: the vendor wants the reader's status.
__attribute__((force_align_arg_pointer)) static void cardVendorResetWait(void *self)
{
    cardVendorEnterState(self, 0x08361450, 0);
    realCardVendorResetWait(self);
}

// state_ResetWait: the reader's answer to the reset it was asked for, the reset
// result (command '0'), instead of the status report.
__attribute__((force_align_arg_pointer)) static void cardVendorResetReply(void *self)
{
    cardVendorEnterState(self, 0x08360490, 1);
    realCardVendorResetReply(self);
}

// state_StatusWait: the vendor asks for its status again.
__attribute__((force_align_arg_pointer)) static void cardVendorStatusWait(void *self)
{
    cardVendorEnterState(self, 0x08360ab0, 0);
    realCardVendorStatusWait(self);
}

// The wait's poll: its result decides the state's transition. The fields
// logged are its own (state pointer, then the counters the parser fills in).
__attribute__((force_align_arg_pointer)) static int cardVendorPollTrace(void *out, void *self)
{
    cardVendorObserveState(self);

    int result = realCardVendorPoll(out, self);

    if (cardVendorTrace && (++cardVendorTicks <= 30 || cardVendorTicks % 200 == 0))
    {
        uint8_t *p = self;
        fprintf(stderr, "WMMT4 CV: tick %lu st=%p f10=%d f14=%d f18=%d f1c=%d f2c=%d result=%d\n", cardVendorTicks,
                *(void **)p, *(int *)(p + 0x10), *(int *)(p + 0x14), *(int *)(p + 0x18), *(int *)(p + 0x1c),
                *(int *)(p + 0x2c), result);
    }
    return result;
}

// The board's receive call, which forwards to the receiver the serial device
// registered (a null pointer means nothing would ever arrive).
__attribute__((force_align_arg_pointer)) static int readAvailTrace(void *buf, int count)
{
    int result = realBoardReadAvail(buf, count);

    if (cardVendorTrace)
        fprintf(stderr, "WMMT4 CV: readAvail(%d) -> %d\n", count, result);
    return result;
}

static void installCardVendor(void)
{
    static const uint8_t entry[] = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x48, 0x89, 0x5d,
                                    0xf4, 0x8b, 0x5d, 0x08, 0xc7, 0x44, 0x24, 0x04};

    static const uint8_t pollEntry[] = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x58, 0x89, 0x5d,
                                        0xf4, 0x8b, 0x5d, 0x0c, 0x8d, 0x45, 0xe4, 0x89};
    static const uint8_t availEntry[] = {0x55, 0x31, 0xc0, 0x89, 0xe5, 0x83, 0xec, 0x18};

    static const uint8_t replyEntry[] = {0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x40,
                                         0x8b, 0x5d, 0x08, 0xc7, 0x44, 0x24, 0x04, 0x19};
    static const uint8_t statusEntry[] = {0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x24, 0x8b,
                                          0x5d, 0x08, 0xc7, 0x44, 0x24, 0x04, 0xb3, 0x78};

    cardVendorTrace = getenv("NAMCO_KB_TRACE") != NULL;
    hookWithOriginal(0x08361450, entry, sizeof(entry), (void *)cardVendorResetWait, &realCardVendorResetWait);
    hookWithOriginal(0x08360800, pollEntry, sizeof(pollEntry), (void *)cardVendorPollTrace, &realCardVendorPoll);
    hookWithOriginal(0x08371440, availEntry, sizeof(availEntry), (void *)readAvailTrace, &realBoardReadAvail);
    // The card reader behind the JVS board, whose conversation with the vendor
    // the JVS emulation answers (see jvs.c). Its replies belong to the states
    // here that ask for them.
    hookWithOriginal(0x08360490, replyEntry, sizeof(replyEntry), (void *)cardVendorResetReply,
                     &realCardVendorResetReply);
    hookWithOriginal(0x08360ab0, statusEntry, sizeof(statusEntry), (void *)cardVendorStatusWait,
                     &realCardVendorStatusWait);
}

// ---------------------------------------------------------------------------
// The test menu's settings (save/setting00/Setting.lua, a checksummed file):
// with the loader's server the cabinet is not in offline mode (mOfflineMode,
// set in the dump) and has its card reader (mIcCardRw). They are read, with
// the other yes/no settings, by two readers of the game's (one per copy of
// the file), which store a byte: called, then the byte set for those two.

typedef void (*SettingRead)(void *, uint8_t *, const char *);

// WMMT4_NO_CARD=1 tells the game the cabinet has no IC card reader: the
// startup's IC card checks are then skipped (a board whose card vendor does
// not answer, as with some packs, otherwise stops at its vendor check).
static int cardReaderSetting = 1;

static void settingOverride(const char *key, uint8_t *setting)
{
    uint8_t read;

    if (!key || !setting)
        return;
    read = *setting;
    if (!strcmp(key, "mOfflineMode"))
        *setting = 0;
    else if (!strcmp(key, "mIcCardRw"))
        *setting = (uint8_t)cardReaderSetting;
    else
        return;
    // WMMT4_BOOT_TRACE: what the game's own Setting.lua said, and what it is
    // given instead (offline 0 = online, card reader 1 = present).
    if (bootTrace)
        fprintf(stderr, "WMMT4 boot: setting %s read %u -> %u\n", key, read, *setting);
}

static SettingRead realReadSetting1, realReadSetting2;

__attribute__((force_align_arg_pointer)) static void readSetting1(void *self, uint8_t *setting, const char *key)
{
    realReadSetting1(self, setting, key);
    settingOverride(key, setting);
}

__attribute__((force_align_arg_pointer)) static void readSetting2(void *self, uint8_t *setting, const char *key)
{
    realReadSetting2(self, setting, key);
    settingOverride(key, setting);
}

static void installSettings(void);

// WMMT4_BOOT_TRACE: the StartUp machine reaches its steps through a function
// pointer ([machine+4]), so no call site names them. The step that reads the
// terminal's peer entry (0x841cc10) has its entry patched to a stub that counts
// the call, then runs the prologue it replaced and resumes inside the step,
// with every register and flag intact: that it never fires is what shows the
// boot stalls before the terminal check, not in it.
static unsigned long stepCalls;

__attribute__((visibility("hidden"))) void wmmt4StepSeen(void *self)
{
    unsigned frames = 0, next = 0;

    stepCalls++;
    if (!bootTrace)
        return;
    if (self)
    {
        frames = *(const unsigned *)((const uint8_t *)self + 0xc);
        next = *(const unsigned *)((const uint8_t *)self + 0x4);
    }
    if (stepCalls <= 3 || stepCalls % 600 == 0)
        fprintf(stderr, "WMMT4 boot: terminal step call %lu (frames %u, machine next %#x)\n", stepCalls, frames,
                next);
}

// The StartUp mode's wait step (0x841a530) leaves only when three gates pass,
// and it polls the boot check module last. It runs a handful of times, so
// each call reports the gates it is stopped at and who pumped it (it is
// entered indirectly, so its caller names the scheduler that stops calling it).
static unsigned modeWaitCalls;

__attribute__((visibility("hidden"))) void wmmt4ModeWaitSeen(uint32_t caller)
{
    uint32_t gate1 = 0, gate2 = 0, gate3 = 0;

    if (!bootTrace)
        return;
    modeWaitCalls++;
    gate1 = ((uint32_t(*)(void))(uintptr_t)0x80c4a90)();
    gate2 = ((uint32_t(*)(void))(uintptr_t)0x835c260)();
    gate3 = ((uint32_t(*)(void))(uintptr_t)0x8919e90)();
    fprintf(stderr, "WMMT4 boot: StartUp wait %u from %#x: gates %u/%u/%u, check remaining %u\n", modeWaitCalls,
            caller, gate1, gate2, gate3, *(const uint32_t *)(uintptr_t)(0x92204e0 + 0x6b0));
}

void wmmt4ModeWait(void);
__asm__(".text\n"
        ".globl wmmt4ModeWait\n"
        "wmmt4ModeWait:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl 0x24(%esp), %eax\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl %eax\n"
        "  calll wmmt4ModeWaitSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %ebx\n"
        "  xorl %ebx, %ebx\n"
        "  subl $0x14, %esp\n"
        "  movl $0x841a539, %eax\n"
        "  jmp *%eax\n");

// 0x841cc10 saves esi as well: push ebp; mov ebp,esp; push esi; push ebx;
// sub esp,0x20.
void wmmt4Stepcc10(void);
__asm__(".text\n"
        ".globl wmmt4Stepcc10\n"
        "wmmt4Stepcc10:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl 0x28(%esp), %eax\n"
        "  pushl %eax\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  calll wmmt4StepSeen\n"
        "  movl %ebp, %esp\n"
        "  addl $4, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %esi\n"
        "  pushl %ebx\n"
        "  subl $0x20, %esp\n"
        "  movl $0x841cc18, %eax\n"
        "  jmp *%eax\n");

// The StartUp machine keeps its next step in [machine+4] and its mode in
// [machine+0xf8], and its own driver (0x8422bd0) rewrites the step at four
// points. Logging those writes shows the transition the machine really makes
// after its wait step, instead of inferring it from the steps that stayed
// silent. The driver is entered indirectly, so its entry is logged too: a
// machine looping in one mode is then visible even when no write happens.
__attribute__((visibility("hidden"))) void wmmt4StepSet(void *machine, uint32_t step, uint32_t site)
{
    static unsigned calls;

    calls++;
    if (!bootTrace || (calls > 24 && calls % 200 != 0))
        return;
    fprintf(stderr, "WMMT4 boot: step set %u at %#x: %#x -> %#x (mode %u, frames %u)\n", calls, site,
            machine ? *(const uint32_t *)((const uint8_t *)machine + 0x4) : 0, step,
            machine ? *(const uint32_t *)((const uint8_t *)machine + 0xf8) : 0,
            machine ? *(const uint32_t *)((const uint8_t *)machine + 0xc) : 0);
}

__attribute__((visibility("hidden"))) void wmmt4StepMachineSeen(void *machine)
{
    static unsigned calls;

    calls++;
    if (!bootTrace || (calls > 8 && calls % 120 != 0))
        return;
    fprintf(stderr, "WMMT4 boot: machine step %u: step %#x (mode %u, frames %u)\n", calls,
            machine ? *(const uint32_t *)((const uint8_t *)machine + 0x4) : 0,
            machine ? *(const uint32_t *)((const uint8_t *)machine + 0xf8) : 0,
            machine ? *(const uint32_t *)((const uint8_t *)machine + 0xc) : 0);
}

// Each write is `movl $step,0x4(%ebx)` (7 bytes); the stub logs it, performs
// it, and resumes at the instruction after it.
void wmmt4StepSetC5E(void);
__asm__(".text\n"
        ".globl wmmt4StepSetC5E\n"
        "wmmt4StepSetC5E:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl $0x8422c5e\n"
        "  pushl $0x841a9f0\n"
        "  pushl %ebx\n"
        "  calll wmmt4StepSet\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  movl $0x841a9f0, 0x4(%ebx)\n"
        "  movl $0x8422c65, %eax\n"
        "  jmp *%eax\n");

void wmmt4StepSetD47(void);
__asm__(".text\n"
        ".globl wmmt4StepSetD47\n"
        "wmmt4StepSetD47:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl $0x8422d47\n"
        "  pushl $0x8419630\n"
        "  pushl %ebx\n"
        "  calll wmmt4StepSet\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  movl $0x8419630, 0x4(%ebx)\n"
        "  movl $0x8422d4e, %eax\n"
        "  jmp *%eax\n");

void wmmt4StepSetDA4(void);
__asm__(".text\n"
        ".globl wmmt4StepSetDA4\n"
        "wmmt4StepSetDA4:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl $0x8422da4\n"
        "  pushl $0x841cc10\n"
        "  pushl %ebx\n"
        "  calll wmmt4StepSet\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  movl $0x841cc10, 0x4(%ebx)\n"
        "  movl $0x8422dab, %eax\n"
        "  jmp *%eax\n");

void wmmt4StepSetE7D(void);
__asm__(".text\n"
        ".globl wmmt4StepSetE7D\n"
        "wmmt4StepSetE7D:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl $0x8422e7d\n"
        "  pushl $0x8419630\n"
        "  pushl %ebx\n"
        "  calll wmmt4StepSet\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  movl $0x8419630, 0x4(%ebx)\n"
        "  movl $0x8422e84, %eax\n"
        "  jmp *%eax\n");

// 0x8422bd0: push ebp; mov ebp,esp; sub esp,0xd8 — the machine's own step.
void wmmt4StepMachine(void);
__asm__(".text\n"
        ".globl wmmt4StepMachine\n"
        "wmmt4StepMachine:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl 0x28(%esp), %eax\n"
        "  pushl %eax\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  calll wmmt4StepMachineSeen\n"
        "  movl %ebp, %esp\n"
        "  addl $4, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  subl $0xd8, %esp\n"
        "  movl $0x8422bd9, %eax\n"
        "  jmp *%eax\n");

// WMMT4_BOOT_TRACE: the check object's "run the list" step (0x841af40, its
// vtable slot 5). It walks a callback chain (the current node in [obj+4], its
// offset in [obj+8]) calling each item's check; the first item that returns 0
// drops it into the gate path at 0x841af6c, which asks 0x8439600(0x9221ea0)
// and otherwise returns -1, leaving the item on screen at チェック中. Logging
// the chain, the item that returns 0 and the gate's answer names what holds it.
static uint32_t checkItemNode, checkItemTarget, checkItemObject;
static unsigned checkItemCalls;

// WMMT4_BOOT_TRACE: the start-up machine's own state, read at every item
// dispatch. [machine+4] is the step it will run next (0x841b490 writes
// 0x84210c0 there), and [+0x134] is the flag the waiting items test: 0x841aec0
// returns 0 at once while it is 0, without doing anything else.
static uint32_t chainNextStep = 0xffffffff, chainWaitFlag = 0xffffffff;

// WMMT4_BOOT_TRACE: the descriptor cursor the machine dispatches, and the
// session object's state word. The machine's loop is
//   0x841b050: node = esi, descriptor = [ebp-0x2c], [descriptor] = node,
//              [descriptor+4] = offset of the next descriptor;
// an odd node id is dispatched through [[descriptor] + node - 1] instead of
// being called directly, which is how the 認証 state handlers (installed at
// +0x7e48.. by 0x8424ac7) can be reached. Those handlers test the WORD at
// [object+0x7e38] against 1 and 3, so that word is logged with the cursor.
static uint32_t sessionObject;
static unsigned chainState = 0xffffu;

__attribute__((visibility("hidden"))) void wmmt4SessionInitSeen(void *object)
{
    sessionObject = (uint32_t)(uintptr_t)object;
    if (!bootTrace)
        return;
    fprintf(stderr,
            "WMMT4 boot: auth session %#x: state %u, handlers %#x %#x %#x %#x %#x\n",
            sessionObject,
            object ? (unsigned)*(const uint16_t *)((const uint8_t *)object + 0x7e38) : 0,
            object ? *(const uint32_t *)((const uint8_t *)object + 0x7e48) : 0,
            object ? *(const uint32_t *)((const uint8_t *)object + 0x7e50) : 0,
            object ? *(const uint32_t *)((const uint8_t *)object + 0x7e58) : 0,
            object ? *(const uint32_t *)((const uint8_t *)object + 0x7e60) : 0,
            object ? *(const uint32_t *)((const uint8_t *)object + 0x7e68) : 0);
}

// WMMT4_BOOT_TRACE: the 認証 session object is built by the factory at
// 0x8430260, as case 8 of its jump table (0x8c5a500[8] = 0x8430d8c): that case
// allocates 0x7ea0 bytes and calls the constructor 0x84249d0, which installs
// the handler table at +0x7e48. The factory takes the type id in its second
// argument, so logging every type it is asked for shows whether type 8 is ever
// requested, and by whom.
static unsigned factoryCalls;
static unsigned factoryTypes[0x47];

__attribute__((visibility("hidden"))) void wmmt4FactorySeen(void *self, uint32_t type, uint32_t caller)
{
    factoryCalls++;
    if (type < 0x47)
        factoryTypes[type]++;
    if (!bootTrace || (factoryCalls > 40 && factoryCalls % 200 != 0))
        return;
    fprintf(stderr, "WMMT4 boot: factory %u: self %#x type %u from %#x%s\n", factoryCalls,
            (uint32_t)(uintptr_t)self, type, caller, type == 8 ? "  <- the auth session" : "");
}

// WMMT4_BOOT_TRACE: the message/string object the crashing callers build from
// is a global: 0x921f8e8 holds its data pointer and 0x921f8ec its control
// block. 0x83c7240 copies that pair (and bumps the refcount at [+4]); the
// only initialiser is the static setup at 0x83c9330, which allocates 0x155c
// bytes, constructs the object, links it into 0x921f8e8/0x921f8ec and
// registers the destructor 0x83c9ad0 with __cxa_atexit. Logging both the
// initialiser and each copy shows whether the copy runs against a constructed
// object or against the zeroed BSS pair.
static unsigned msgInitCalls, msgCopyCalls;

__attribute__((visibility("hidden"))) void wmmt4MsgInitSeen(void *data, uint32_t caller)
{
    msgInitCalls++;
    if (!bootTrace)
        return;
    fprintf(stderr, "WMMT4 boot: message object init %u: 0x921f8e8 <- %p from %#x\n", msgInitCalls, data, caller);
}

// WMMT4_GATE_PRIME (opt-in causal test, off by default): set from the
// environment at install time; every hook below that primes something checks
// it, and none of them is installed without it.
static int gatePrime;

// WMMT4_GATE_PRIME: the crashing path copies the global message object
// (0x921f8e8/0x921f8ec) without ever having called the accessor 0x83c92f0
// that lazily constructs it. With the prime on, run that accessor once, right
// before the copy reads the global. It ends in `ret 4`, so the call is written
// out: push the argument, let the callee pop one dword, and drop the other.
__attribute__((visibility("hidden"))) void wmmt4MsgInitNow(void)
{
    static uint32_t scratch[2];
    static unsigned calls;

    if (!gatePrime || *(const uint32_t *)(uintptr_t)0x921f8e8 != 0)
        return;
    calls++;
    __asm__ volatile("pushl $0\n\t"
                     "pushl %0\n\t"
                     "movl $0x83c92f0, %%eax\n\t"
                     "calll *%%eax\n\t"
                     "addl $4, %%esp\n\t"
                     :
                     : "r"(scratch)
                     : "eax", "ecx", "edx", "memory");
    fprintf(stderr, "WMMT4 boot: message accessor 0x83c92f0 run %u: [0x921f8e8] %#x, [0x921f8ec] %#x\n", calls,
            *(const uint32_t *)(uintptr_t)0x921f8e8, *(const uint32_t *)(uintptr_t)0x921f8ec);
}

// WMMT4_BOOT_TRACE: the singleton at 0x9215870 is a reference-counted object
// built by 0x836fe20 (allocate 0x10, construct via 0x836fb50, link a control
// block with vtable 0x8c29950 at 0x9215874, then store the object at
// 0x9215870). Its only game callers are inside the two machine steps
// 0x841f390 and 0x841fc40 — the same steps that clear the check row's gate
// busy flag — so it is built by those steps, not by a startup phase.
static unsigned singletonCalls;

__attribute__((visibility("hidden"))) void wmmt4SingletonSeen(void *value, uint32_t caller)
{
    singletonCalls++;
    if (!bootTrace || singletonCalls > 8)
        return;
    fprintf(stderr, "WMMT4 boot: singleton 0x9215870 <- %p from %#x (call %u)\n", value, caller, singletonCalls);
}

// 0x836fe77: mov ds:0x9215870, ebx — the store, with eax holding the control block.
void wmmt4SingletonInit(void);
__asm__(".text\n"
        ".globl wmmt4SingletonInit\n"
        "wmmt4SingletonInit:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl 0x24(%ebp)\n"
        "  pushl %ebx\n"
        "  calll wmmt4SingletonSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  movl %ebx, 0x9215870\n"
        "  pushl %eax\n"
        "  movl $0x836fe7d, %eax\n"
        "  xchgl %eax, (%esp)\n"
        "  ret\n");

__attribute__((visibility("hidden"))) void wmmt4MsgCopySeen(uint32_t caller)
{
    wmmt4MsgInitNow();
    msgCopyCalls++;
    if (!bootTrace || msgCopyCalls > 20)
        return;
    fprintf(stderr, "WMMT4 boot: message copy %u from %#x: [0x921f8e8] %#x, [0x921f8ec] %#x\n", msgCopyCalls, caller,
            *(const uint32_t *)(uintptr_t)0x921f8e8, *(const uint32_t *)(uintptr_t)0x921f8ec);
}

// WMMT4_BOOT_TRACE: the lazy initialiser 0x83c9330 is only reachable from
// the accessor 0x83c92f0, whose two call sites are inside item 0x8420600 and
// sit behind `if ([0x92203c4] == 0)`. Logging that guard shows whether the
// item skipped the accessor (nonzero) or never reached it at all.
static unsigned msgGuardCalls;

__attribute__((visibility("hidden"))) void wmmt4MsgGuardSeen(uint32_t guard, uint32_t frameC0)
{
    msgGuardCalls++;
    if (!bootTrace || msgGuardCalls > 12)
        return;
    fprintf(stderr, "WMMT4 boot: message guard %u: [0x92203c4] %u, [obj+0xc0] %u -> %s\n", msgGuardCalls, guard,
            frameC0, guard ? "accessor skipped (initialiser not reached)" : "accessor runs");
}

// 0x8420953: mov edx,ds:0x92203c4 — the guard in front of the accessor.
void wmmt4MsgGuard(void);
__asm__(".text\n"
        ".globl wmmt4MsgGuard\n"
        "wmmt4MsgGuard:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl 0xc0(%esi)\n"
        "  pushl 0x92203c4\n"
        "  calll wmmt4MsgGuardSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  movl 0x92203c4, %edx\n"
        "  pushl %eax\n"
        "  movl $0x8420959, %eax\n"
        "  xchgl %eax, (%esp)\n"
        "  ret\n");

// 0x83c9364: mov ds:0x921f8e8, esi — the static initialiser's link.
void wmmt4MsgInit(void);
__asm__(".text\n"
        ".globl wmmt4MsgInit\n"
        "wmmt4MsgInit:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl 0x24(%ebp)\n"
        "  pushl %esi\n"
        "  calll wmmt4MsgInitSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  movl %esi, 0x921f8e8\n"
        // eax holds the newly allocated control block here and the original
        // code stores it at 0x921f8ec two instructions later, so resume with
        // eax intact: push it, aim the stack slot at the resume address, and
        // return through it.
        "  pushl %eax\n"
        "  movl $0x83c936a, %eax\n"
        "  xchgl %eax, (%esp)\n"
        "  ret\n");

// 0x83c7240: push ebp; mov edx,ds:0x921f8e8 — the copy.
void wmmt4MsgCopy(void);
__asm__(".text\n"
        ".globl wmmt4MsgCopy\n"
        "wmmt4MsgCopy:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl 0x24(%ebp)\n"
        "  calll wmmt4MsgCopySeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl 0x921f8e8, %edx\n"
        "  pushl %eax\n"
        "  movl $0x83c7247, %eax\n"
        "  xchgl %eax, (%esp)\n"
        "  ret\n");

// WMMT4_BOOT_TRACE: state 2 of the auth machine (0x84328c8) is what fills the
// pending type: it saves the current 0x92204e4 into 0x92204e8, copies the last
// poll answer 0x92204ec into 0x92204e4, and resets 0x92204ec to -1. State 4
// then hands that value to the factory unless it is 0x48. So the queued type
// is never chosen by the machine — it is whatever the polled object answered.
__attribute__((visibility("hidden"))) void wmmt4PendingSetSeen(uint32_t value, uint32_t previous)
{
    static unsigned calls;

    calls++;
    if (!bootTrace || calls > 20)
        return;
    fprintf(stderr, "WMMT4 boot: pending type <- %u (%#x), previous %u, poll answer cleared to -1\n", value,
            value, previous);
}

// 0x84328e1: mov ds:0x92204e4, eax — the copy of the poll answer.
void wmmt4PendingSet(void);
__asm__(".text\n"
        ".globl wmmt4PendingSet\n"
        "wmmt4PendingSet:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl 0x92204e8\n"
        "  pushl %eax\n"
        "  calll wmmt4PendingSetSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  movl %eax, 0x92204e4\n"
        "  movl $0x84328e6, %eax\n"
        "  jmp *%eax\n");

// WMMT4_BOOT_TRACE: the machine that would ask the factory for the auth
// session. 0x8432660 ticks it; its state is [machine+4] (jump table
// 0x8c5a61c, states 0..9) and the type it will create is queued in the global
// 0x92204e4 (state 5 hands it to the factory at 0x8432910, and state 3's
// helper at 0x8432b61 passes its own argument instead). Logging both shows the
// state it stops in and the type it never queues.
static unsigned authMachineCalls, authMachineState = 0xffffffff, authMachinePending = 0xffffffff;

__attribute__((visibility("hidden"))) void wmmt4AuthMachineSeen(void *machine)
{
    unsigned state = machine ? *(const uint32_t *)((const uint8_t *)machine + 4) : 0xffffffff;
    unsigned pending = *(const uint32_t *)(uintptr_t)0x92204e4;
    unsigned polled = *(const uint32_t *)(uintptr_t)0x92204ec;

    authMachineCalls++;
    if (!bootTrace)
        return;
    if (authMachineCalls <= 12 || state != authMachineState || pending != authMachinePending)
        fprintf(stderr, "WMMT4 boot: auth machine %u: state %u, pending type %u (%#x), check row answered %d\n",
                authMachineCalls, state, pending, pending, (int)polled);
    authMachineState = state;
    authMachinePending = pending;
}

__attribute__((visibility("hidden"))) void wmmt4SessionHandlerSeen(void *object)
{
    static unsigned calls;

    calls++;
    if (!bootTrace || (calls > 8 && calls % 60 != 0))
        return;
    fprintf(stderr, "WMMT4 boot: auth handler 0x84241e0 %u: object %#x, state %u\n", calls,
            (uint32_t)(uintptr_t)object,
            object ? (unsigned)*(const uint16_t *)((const uint8_t *)object + 0x7e38) : 0);
}

__attribute__((visibility("hidden"))) void wmmt4CheckItemSeen(void *object, uint32_t node, uint32_t offset,
                                                              uint32_t frameEbp)
{
    uint32_t address = (uint32_t)(uintptr_t)object + offset;
    uint32_t target = node;

    checkItemCalls++;
    if (node & 1)
    {
        uint32_t table = *(const uint32_t *)(uintptr_t)address;
        if (table >= 0x10000)
            target = *(const uint32_t *)(uintptr_t)(table + node - 1);
    }
    checkItemNode = node;
    checkItemTarget = target;
    checkItemObject = address;
    if (bootTrace)
    {
        uint32_t nextStep = object ? *(const uint32_t *)((const uint8_t *)(uintptr_t)object + 4) : 0;
        uint32_t waitFlag = object ? *(const uint32_t *)((const uint8_t *)(uintptr_t)object + 0x134) : 0;
        uint32_t descriptor = frameEbp ? *(const uint32_t *)(uintptr_t)(frameEbp - 0x2c) : 0;
        uint32_t descNode = descriptor >= 0x10000 ? *(const uint32_t *)(uintptr_t)descriptor : 0;
        uint32_t descNext = descNode >= 0x10000 ? *(const uint32_t *)(uintptr_t)(descriptor + 4) : 0;
        unsigned state = sessionObject ? (unsigned)*(const uint16_t *)(uintptr_t)(sessionObject + 0x7e38) : 0xffffu;

        if (checkItemCalls <= 10 || (node & 1) || nextStep != chainNextStep || waitFlag != chainWaitFlag ||
            state != chainState)
        {
            fprintf(stderr,
                    "WMMT4 boot: check item %u: node %#x offset %#x -> %#x on %#x; odd %u, cursor %#x node %#x "
                    "next %#x; next step %#x, wait flag %u, session state %u\n",
                    checkItemCalls, node, offset, target, address, node & 1, descriptor, descNode, descNext,
                    nextStep, waitFlag, state);
            chainNextStep = nextStep;
            chainWaitFlag = waitFlag;
            chainState = state;
        }
    }
}

__attribute__((visibility("hidden"))) void wmmt4CheckItemFailedSeen(void)
{
    static unsigned calls;

    calls++;
    if (!bootTrace || (calls > 6 && calls % 300 != 0))
        return;
    fprintf(stderr,
            "WMMT4 boot: check item returned 0: %#x (node %#x on %#x), failures %u; result slots %u %u %u, "
            "state %u flag %u\n",
            checkItemTarget, checkItemNode, checkItemObject, calls,
            *(const uint32_t *)(uintptr_t)0x9220c58, *(const uint32_t *)(uintptr_t)0x9220c5c,
            *(const uint32_t *)(uintptr_t)0x9220c60, *(const uint32_t *)(uintptr_t)0x9220c48,
            *(const uint32_t *)(uintptr_t)0x9220c4c);
}

__attribute__((visibility("hidden"))) void wmmt4CheckGateSeen(uint32_t result)
{
    static unsigned calls;

    calls++;
    if (!bootTrace || (calls > 6 && calls % 300 != 0))
        return;
    fprintf(stderr, "WMMT4 boot: gate 0x8439600(0x9221ea0) -> %u ([0x9221ea0] %u, [+0xc] %u), calls %u\n", result,
            *(const uint32_t *)(uintptr_t)0x9221ea0, *(const uint32_t *)(uintptr_t)0x9221eac, calls);
}

// Each hook sits on an instruction boundary of that function and re-runs the
// instruction it replaced, so no site is patched mid-instruction.
// 0x841b050: test esi,1. The machine object, the node id and its offset are
// in ebx/esi/edi; the dispatcher's own ebp (saved by pushal at 8(%ebp)) gives
// its frame, where [ebp-0x2c] holds the descriptor being dispatched.
void wmmt4CheckItem(void);
__asm__(".text\n"
        ".globl wmmt4CheckItem\n"
        "wmmt4CheckItem:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  movl 8(%ebp), %eax\n"
        "  pushl %eax\n"
        "  pushl %edi\n"
        "  pushl %esi\n"
        "  pushl %ebx\n"
        "  calll wmmt4CheckItemSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  testl $1, %esi\n"
        "  movl $0x841b056, %eax\n"
        "  jmp *%eax\n");

// 0x8432660: push ebp; mov ebp,esp; push edi; push esi; push ebx; sub esp,0x2c
// — the machine's per-frame tick; its object is the only argument.
void wmmt4AuthMachine(void);
__asm__(".text\n"
        ".globl wmmt4AuthMachine\n"
        "wmmt4AuthMachine:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl 0x28(%esp), %eax\n"
        "  pushl %eax\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  calll wmmt4AuthMachineSeen\n"
        "  movl %ebp, %esp\n"
        "  addl $4, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %edi\n"
        "  pushl %esi\n"
        "  pushl %ebx\n"
        "  subl $0x2c, %esp\n"
        "  movl $0x8432669, %eax\n"
        "  jmp *%eax\n");

// 0x8430260: push ebp; mov ebp,esp; sub esp,0x38 — the factory whose second
// argument selects the case; the return address names the caller.
void wmmt4Factory(void);
__asm__(".text\n"
        ".globl wmmt4Factory\n"
        "wmmt4Factory:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  movl 0x24(%ebp), %eax\n"
        "  pushl %eax\n"
        "  movl 0x2c(%ebp), %eax\n"
        "  pushl %eax\n"
        "  movl 0x28(%ebp), %eax\n"
        "  pushl %eax\n"
        "  calll wmmt4FactorySeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  subl $0x38, %esp\n"
        "  movl $0x8430266, %eax\n"
        "  jmp *%eax\n");

// 0x8424ac7: cmp dword [0x9221a10], 0x68 — the session object's own setup,
// where ebx is the object whose handler table (+0x7e48..) is then filled in.
void wmmt4SessionInit(void);
__asm__(".text\n"
        ".globl wmmt4SessionInit\n"
        "wmmt4SessionInit:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl %ebx\n"
        "  calll wmmt4SessionInitSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  cmpl $0x68, 0x9221a10\n"
        "  movl $0x8424ace, %eax\n"
        "  jmp *%eax\n");

// 0x84241e0: push ebp; mov ebp,esp; sub esp,0x28 — the handler that waits for
// state 3 (it returns 1 while [object+0x7e38] is not 3).
void wmmt4SessionHandler(void);
__asm__(".text\n"
        ".globl wmmt4SessionHandler\n"
        "wmmt4SessionHandler:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl 0x28(%esp), %eax\n"
        "  pushl %eax\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  calll wmmt4SessionHandlerSeen\n"
        "  movl %ebp, %esp\n"
        "  addl $4, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  subl $0x28, %esp\n"
        "  movl $0x84241e6, %eax\n"
        "  jmp *%eax\n");

// 0x841b06b: test al,al; je 0x841af6c (the item failed) else 0x841b073.
void wmmt4CheckFail(void);
__asm__(".text\n"
        ".globl wmmt4CheckFail\n"
        "wmmt4CheckFail:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  calll wmmt4CheckItemFailedSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  testb %al, %al\n"
        "  je 1f\n"
        "  movl $0x841b073, %eax\n"
        "  jmp *%eax\n"
        "1:\n"
        "  movl $0x841af6c, %eax\n"
        "  jmp *%eax\n");

// 0x841af78: test eax,eax; jne 0x841af90 (gate passed) else 0x841af7c.
void wmmt4CheckGate(void);
__asm__(".text\n"
        ".globl wmmt4CheckGate\n"
        "wmmt4CheckGate:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl %eax\n"
        "  calll wmmt4CheckGateSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  testl %eax, %eax\n"
        "  jne 1f\n"
        "  movl $0x841af7c, %eax\n"
        "  jmp *%eax\n"
        "1:\n"
        "  movl $0x841af90, %eax\n"
        "  jmp *%eax\n");

// WMMT4_BOOT_TRACE: the object at 0x9221ea0 is a small "busy" struct: its
// 0x8439600 gate returns [+0] only while [+0xc] is 0, and eight call sites set
// that field through the setter 0x84395f0 (1 = busy, 0 = idle) while the
// constructor 0x8439560 clears it. 0x84395c0 also writes 1 but has no caller.
// Logging the writes that reach this object shows who leaves it busy.
// Each write to the field comes from one of these call sites: five steps set
// it busy, three steps and the constructor clear it. Logging every write with
// its site, plus a mask of the sites seen so far, shows which steps actually
// run and therefore which clear never gets its turn.
static const struct
{
    uint32_t caller;
    const char *what;
} gateSites[] = {
    {0x8419fe4, "step 0x8419fd0 set"},   {0x841a084, "step 0x841a070 set"},
    {0x841a6bf, "step 0x841a690 set"},   {0x841cb88, "step 0x841cb74 set"},
    {0x841d124, "step 0x841d110 set"},   {0x841f475, "step 0x841f461 clear"},
    {0x841fd5d, "step 0x841fd49 clear"}, {0x856debc, "step 0x856dea8 clear"},
    {0x87b0d75, "ctor 0x8439560 clear"},
};

#define GATE_SITES (sizeof(gateSites) / sizeof(gateSites[0]))
static unsigned gateWrites, gateSeen;

__attribute__((visibility("hidden"))) void wmmt4GateBusySeen(void *object, uint32_t value, uint32_t site,
                                                            uint32_t caller)
{
    const char *what = "unlisted";
    unsigned i;

    // The writes that matter are the ones to the global the check row reads;
    // the others are logged too (briefly) so a constructor that builds a
    // different instance, leaving the global at its zeroed BSS value, shows up.
    if ((uintptr_t)object != 0x9221ea0)
    {
        static unsigned others;

        others++;
        if (bootTrace && others <= 12)
            fprintf(stderr, "WMMT4 boot: gate write -> %u on other object %#x (site %#x, from %#x), %u\n", value,
                    (uint32_t)(uintptr_t)object, site, caller, others);
        return;
    }
    for (i = 0; i < GATE_SITES; i++)
        if (gateSites[i].caller == caller)
        {
            what = gateSites[i].what;
            gateSeen |= 1u << i;
        }
    gateWrites++;
    if (!bootTrace || gateWrites > 40)
        return;
    fprintf(stderr, "WMMT4 boot: gate flag -> %u by %s (from %#x), write %u, sites seen %#x of 0x%x\n", value, what,
            caller, gateWrites, gateSeen, (1u << GATE_SITES) - 1);
}

// WMMT4_GATE_PRIME (opt-in causal test, off by default): the check row polls
// 0x8439600(0x9221ea0), which answers [obj+0] only while [obj+0xc] is 0. Both
// were measured wrong for this boot — the constructor that sets [+0]=1 never
// runs and the only write sets [+0xc]=1 — so priming the object here, right
// before each poll, tests whether that alone lets the row return something
// other than -1.
static unsigned gatePrimeWrites;

__attribute__((visibility("hidden"))) void wmmt4GatePrimed(void *object)
{
    if (!gatePrime || (uintptr_t)object != 0x9221ea0)
        return;
    *(uint32_t *)((uint8_t *)object + 0x0) = 1;
    *(uint32_t *)((uint8_t *)object + 0xc) = 0;
    gatePrimeWrites++;
    if (bootTrace && gatePrimeWrites <= 10)
        fprintf(stderr, "WMMT4 boot: gate primed %u: [+0] 1, [+0xc] 0, polled by %p\n", gatePrimeWrites,
                __builtin_return_address(0));
}

// WMMT4_GATE_PRIME, second half: with the gate open, the row takes the
// advance path, sets [row+0x14]=2 and [row+0x18]=1, then branches on the
// message flag [row+0x134]. Measured 0, that branch builds a message from
// uninitialised state and faults in 0x83c98a0; priming the flag takes the
// other branch (0x841afe5), which assembles the pending-message object on
// 0x92204e0 and returns.
static unsigned rowFlagPrimes;

__attribute__((visibility("hidden"))) void wmmt4RowFlagPrimed(void *row)
{
    if (!gatePrime || !row || *(uint32_t *)((uint8_t *)row + 0x134) != 0)
        return;
    *(uint32_t *)((uint8_t *)row + 0x134) = 1;
    rowFlagPrimes++;
    if (bootTrace && rowFlagPrimes <= 10)
        fprintf(stderr, "WMMT4 boot: row flag primed %u: [%p+0x134] 0 -> 1\n", rowFlagPrimes, row);
}

// 0x841afb7: mov esi,[ebx+0x134] — the row's message-flag read.
void wmmt4RowFlag(void);
__asm__(".text\n"
        ".globl wmmt4RowFlag\n"
        "wmmt4RowFlag:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl %ebx\n"
        "  calll wmmt4RowFlagPrimed\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  movl 0x134(%ebx), %esi\n"
        "  movl $0x841afbd, %eax\n"
        "  jmp *%eax\n");

// 0x8439600: push ebp; xor eax,eax; mov ebp,esp; mov edx,[ebp+8] — the getter.
void wmmt4GatePoll(void);
__asm__(".text\n"
        ".globl wmmt4GatePoll\n"
        "wmmt4GatePoll:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl 0x28(%ebp)\n"
        "  calll wmmt4GatePrimed\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  xorl %eax, %eax\n"
        "  movl 0x8(%ebp), %edx\n"
        "  movl $0x8439608, %eax\n"
        "  jmp *%eax\n");

// 0x84395f0: mov [eax+0xc],edx (the setter: 1 busy, 0 idle).
void wmmt4GateSet(void);
__asm__(".text\n"
        ".globl wmmt4GateSet\n"
        "wmmt4GateSet:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl 0x24(%esp), %ecx\n"
        "  movl 0x28(%esp), %eax\n"
        "  movl 0x2c(%esp), %edx\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl %ecx\n"
        "  pushl $0x84395f0\n"
        "  pushl %edx\n"
        "  pushl %eax\n"
        "  calll wmmt4GateBusySeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  movl 0xc(%ebp), %edx\n"
        "  movl 0x8(%ebp), %eax\n"
        "  pushl $0x84395f9\n"
        "  ret\n");

// 0x8439560: the constructor, which clears the field.
void wmmt4GateCtor(void);
__asm__(".text\n"
        ".globl wmmt4GateCtor\n"
        "wmmt4GateCtor:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl 0x24(%esp), %ecx\n"
        "  movl 0x28(%esp), %eax\n"
        "  xorl %edx, %edx\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl %ecx\n"
        "  pushl $0x8439560\n"
        "  pushl %edx\n"
        "  pushl %eax\n"
        "  calll wmmt4GateBusySeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  movl 0x8(%ebp), %eax\n"
        "  pushl $0x8439566\n"
        "  ret\n");

// WMMT4_BOOT_TRACE: the functions that contain the three clear call sites.
// Logging their entry separates "the step is never scheduled" from "the step
// ran and its branch skipped the clear", which the write log alone cannot tell.
__attribute__((visibility("hidden"))) void wmmt4ClearStepSeen(uint32_t entry)
{
    static unsigned counts[3];
    static const uint32_t entries[3] = {0x841f390, 0x841fc40, 0x856de60};
    unsigned i, calls;

    for (i = 0; i < 3 && entries[i] != entry; i++)
        ;
    if (i == 3)
        return;
    calls = ++counts[i];
    if (!bootTrace || (calls > 3 && calls % 300 != 0))
        return;
    fprintf(stderr, "WMMT4 boot: clear step %#x entered, call %u\n", entry, calls);
}

// WMMT4_BOOT_TRACE: what schedules the two clear steps. The check row's item
// 0x8421840 is the only dispatcher found: it returns 0 at 0x8421aed unless
// 0x83c78a0() finds content in the object copied from the global pair at
// 0x921f8e8/0x921f8ec (0x83c7240 copies 0x921f8e8, and the predicate walks
// the 11 slots at that copied data pointer + 0xc; 0x921f8ec is only the
// object's control block), and it
// switches on [machine+0xc0] (1 -> 0x8421c68, 2 -> 0x8421ca0; its tail block
// then hands over with 1 -> 0x8419290 -> 0x841c260 -> 0x841fc40 and
// 2 -> 0x8419450 -> 0x841bf00 -> 0x841f390). Nothing in the image stores 1 or
// 2 there: the only writes are 0xffffffff (0x841d53e) and 0 (0x843a5d5).
// Logging the predicate's answer, the sub-state, and the four writes that hand
// over to those chains shows which wait the boot is sitting on.
__attribute__((visibility("hidden"))) void wmmt4SelectRow(void *machine)
{
    static unsigned calls;
    const uint8_t *m = machine;

    calls++;
    if (!bootTrace || (calls > 40 && calls % 200 != 0))
        return;
    // Both globals get neutral names: neither is the predicate's table base.
    // The predicate copies 0x921f8e8 (the object pointer) and indexes that
    // object's data; 0x921f8ec is the reference-counted control block.
    fprintf(stderr, "WMMT4 boot: select row %u: sub %d state %d event %#x global[0x921f8e8] %#x global[0x921f8ec] %#x\n",
            calls, m ? *(const int32_t *)(m + 0xc0) : 0, m ? *(const int32_t *)(m + 0xc4) : 0,
            *(const volatile uint32_t *)0x92203c4, *(const volatile uint32_t *)0x921f8e8,
            *(const volatile uint32_t *)0x921f8ec);
}

// WMMT4_BOOT_TRACE: which slot the predicate 0x83c7750 rejects. 0x83c78a0
// copies the global 0x921f8e8 (the message object's data pointer, not the
// control block at 0x921f8ec) into a local and passes that copy as 0x83c7750's
// argument, so the argument is the base: entry i is the dword at data+0xc+i*8,
// a pointer whose first dword is its type. A non-null entry whose type is
// neither 0 nor 1 is rejected, and the first such entry fails the whole call.
// Byte data+0x1558 short-circuits the call to true before any entry is read.
__attribute__((visibility("hidden"))) void wmmt4SelectSlots(const void *data, uint32_t global)
{
    static unsigned calls, prints;
    static const uint8_t *lastBase;
    static uint32_t lastGlobal, lastSticky, lastBad, lastType;
    const uint8_t *d = data;
    uint32_t sticky = 0, bad = 11, type = 0;
    unsigned i;

    calls++;
    if (!bootTrace)
        return;
    if (!d)
    {
        if (prints++ < 4)
            fprintf(stderr, "WMMT4 boot: select slots %u: null base (global 0x921f8e8 %#x)\n", calls, global);
        return;
    }
    sticky = *(const uint8_t *)(const void *)(d + 0x1558);
    if (!sticky)
        for (i = 0; i < 11; i++)
        {
            const uint8_t *entry = *(const uint8_t *const *)(const void *)(d + 0xc + i * 8);
            uint32_t t;

            if (!entry)
                continue;
            t = *(const uint32_t *)(const void *)entry;
            if (t == 0 || t == 1)
                continue;
            bad = i;
            type = t;
            break;
        }
    if (prints && d == lastBase && global == lastGlobal && sticky == lastSticky && bad == lastBad && type == lastType)
        return;
    lastBase = d;
    lastGlobal = global;
    lastSticky = sticky;
    lastBad = bad;
    lastType = type;
    prints++;
    fprintf(stderr, "WMMT4 boot: select slots %u: base %p (global 0x921f8e8 %#x) sticky %u: %s\n", calls, d, global, sticky,
            sticky ? "passes before any slot is read"
                   : bad < 11 ? "first reject slot" : "all 11 slots null or type 0/1");
    if (bad < 11)
        fprintf(stderr, "WMMT4 boot:   slot %u [+%#x] type %u is the reject\n", bad, 0xc + bad * 8, type);
    for (i = 0; i < 11; i++)
    {
        const uint8_t *entry = *(const uint8_t *const *)(const void *)(d + 0xc + i * 8);

        if (!entry)
            fprintf(stderr, "WMMT4 boot:   slot %u [+%#x] null\n", i, 0xc + i * 8);
        else if (i < bad)
            fprintf(stderr, "WMMT4 boot:   slot %u [+%#x] %p type %u\n", i, 0xc + i * 8, entry,
                    *(const uint32_t *)(const void *)entry);
        else if (i == bad)
            fprintf(stderr, "WMMT4 boot:   slot %u [+%#x] %p type %u next %#x  <- the reject\n", i, 0xc + i * 8, entry,
                    *(const uint32_t *)(const void *)entry, *(const uint32_t *)(const void *)(entry + 4));
        else
            fprintf(stderr, "WMMT4 boot:   slot %u [+%#x] %p (left unread)\n", i, 0xc + i * 8, entry);
    }
}

__attribute__((visibility("hidden"))) void wmmt4SelectPredicate(unsigned present)
{
    static unsigned calls;

    calls++;
    if (!bootTrace || (calls > 40 && calls % 200 != 0))
        return;
    fprintf(stderr, "WMMT4 boot: select predicate %u: %u\n", calls, present);
}

__attribute__((visibility("hidden"))) void wmmt4SelectStep(void *machine, uint32_t step, uint32_t site)
{
    static unsigned calls;
    const uint8_t *m = machine;

    calls++;
    if (!bootTrace || (calls > 40 && calls % 200 != 0))
        return;
    fprintf(stderr, "WMMT4 boot: select step %u at %#x: %#x -> %#x (sub %d state %d)\n", calls, site,
            m ? *(const uint32_t *)(m + 0x4) : 0, step, m ? *(const int32_t *)(m + 0xc0) : 0,
            m ? *(const int32_t *)(m + 0xc4) : 0);
}

// 0x83c7750: push ebp; mov eax,1 (6 bytes). The argument is the copy of
// 0x921f8e8, logged before the two replayed instructions.
void wmmt4SelectSlotsEntry(void);
__asm__(".text\n"
        ".globl wmmt4SelectSlotsEntry\n"
        "wmmt4SelectSlotsEntry:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl 0x28(%esp), %eax\n"
        "  movl 0x921f8e8, %edx\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl %edx\n"
        "  pushl %eax\n"
        "  calll wmmt4SelectSlots\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl $1, %eax\n"
        "  movl $0x83c7756, %edx\n"
        "  jmp *%edx\n");

// WMMT4_BOOT_TRACE: the branch inside 0x841c260 that skips the 0x841c2fe
// write. The body is: 0x88dbce0() (=[0x92e38d0] ? [net+8] : 0) nonzero ->
// [machine+0xc4]=4 and install 0x841fc40; else 0x8370c00() (= card object's
// byte +0xfb) true -> retry counter [machine+0xc] 0..9 path whose 0x841c2fe
// write installs 0x841b920 once the count wraps, the record word read at
// buf+4 has no 0x100 bit and 0x8370c30() is false; else 0x8370c30() (= card
// byte +0xfc) true -> the sub-2 branch that installs 0x8419630; else return
// with no write. Logging those three inputs names the branch taken. The card
// record byte +0x2a lives in the 0x841b920/0x841b840 steps, which this branch
// never installs, so it is not the condition here.
__attribute__((visibility("hidden"))) void wmmt4SelectC260(void *machine)
{
    static unsigned calls, prints;
    static uint32_t lastNet, lastFb, lastFc, lastCounter, lastStep, lastSub;
    const uint8_t *m = machine;
    const uint32_t net = *(const volatile uint32_t *)0x92e38d0;
    const uint32_t netWord = net ? *(const volatile uint32_t *)(net + 8) : 0;
    const uint32_t table = *(const volatile uint32_t *)0x921589c;
    const uint32_t card = table ? *(const volatile uint32_t *)(table + 8) : 0;
    const uint32_t fb = card ? *(const volatile uint8_t *)(card + 0xfb) : 0;
    const uint32_t fc = card ? *(const volatile uint8_t *)(card + 0xfc) : 0;
    const uint32_t card0 = card ? *(const volatile uint8_t *)card : 0;
    const uint32_t cardF8 = card ? *(const volatile uint8_t *)(card + 0xf8) : 0;
    const uint32_t card100 = card ? *(const volatile uint32_t *)(card + 0x100) : 0;
    const uint32_t e0 = *(const volatile uint8_t *)0x92158e0;
    const uint32_t step = m ? *(const uint32_t *)(m + 4) : 0;
    const uint32_t counter = m ? *(const uint32_t *)(m + 0xc) : 0;
    const uint32_t sub = m ? *(const int32_t *)(m + 0xc4) : 0;
    const char *branch;

    calls++;
    if (!bootTrace)
        return;
    if (netWord)
        branch = "net word set -> +0xc4=4, install 0x841fc40";
    else if (fb)
        branch = "card +0xfb set -> retry path, may install 0x841b920 at 0x841c2fe";
    else if (fc)
        branch = "card +0xfb clear, +0xfc set -> +0xc4=2, install 0x8419630";
    else
        branch = "card +0xfb and +0xfc clear -> return, no write";
    if (prints >= 8 && calls % 300 != 0)
        return;
    if (prints && netWord == lastNet && fb == lastFb && fc == lastFc && counter == lastCounter && step == lastStep &&
        sub == lastSub)
        return;
    lastNet = netWord;
    lastFb = fb;
    lastFc = fc;
    lastCounter = counter;
    lastStep = step;
    lastSub = sub;
    prints++;
    fprintf(stderr,
            "WMMT4 boot: c260 entry %u: step %#x counter %u sub %u | net [0x92e38d0] %#x word %#x | card "
            "[0x921589c] %#x obj %#x +0 %u +0xf8 %u +0xfb %u +0xfc %u +0x100 %#x | [0x92158e0] %u | %s\n",
            calls, step, counter, sub, net, netWord, table, card, card0, cardF8, fb, fc, card100, e0, branch);
}

// 0x841c260: push ebp; mov ebp,esp; sub esp,0x178 (9 bytes).
void wmmt4SelectC260Entry(void);
__asm__(".text\n"
        ".globl wmmt4SelectC260Entry\n"
        "wmmt4SelectC260Entry:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl 0x28(%esp), %eax\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl %eax\n"
        "  calll wmmt4SelectC260\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  subl $0x178, %esp\n"
        "  pushl $0x841c269\n"
        "  ret\n");

// WMMT4_BOOT_TRACE: why the card object's +0xfb never sets, and which write
// raises +0xfc. +0xfb has a single setter, 0x8375aef inside 0x8375a90, and it
// runs only after 0x8375110() returns true (that function first requires byte
// [obj+0] != 0, then does card I/O). 0x8375a90 has no direct caller or
// immediate reference in the image, so the entry probe logs whether it runs at
// all and the return address it was entered from. +0xfc has six card-side
// writers; the only one outside 0x8375a90 is 0x8375791 in 0x8375070 (called
// from 0x8371952), taken when the byte at 0x92158e0 is nonzero, and it also
// stores 0x12d (301) into +0x108 - the "Code 301" the boot prints.
__attribute__((visibility("hidden"))) void wmmt4CardPoll(const void *obj, uint32_t caller)
{
    static unsigned calls;
    const uint8_t *c = obj;

    calls++;
    if (!bootTrace || (calls > 6 && calls % 300 != 0))
        return;
    fprintf(stderr,
            "WMMT4 boot: card poll %u from %#x: obj %p +0 %u handle +4 %#x +0xf8 %u +0xfb %u +0xfc %u\n",
            calls, caller, c, c ? c[0] : 0, c ? *(const uint32_t *)(const void *)(c + 4) : 0, c ? c[0xf8] : 0,
            c ? c[0xfb] : 0, c ? c[0xfc] : 0);
}

__attribute__((visibility("hidden"))) void wmmt4CardFcSet(const void *obj, uint32_t site)
{
    static unsigned calls;
    const uint8_t *c = obj;

    calls++;
    if (!bootTrace || calls > 6)
        return;
    fprintf(stderr,
            "WMMT4 boot: card +0xfc=1 at %#x call %u: obj %p +0xf8 %u +0xfb %u [0x92158e0] %u "
            "[0x92158e8] %#x\n",
            site, calls, c, c ? c[0xf8] : 0, c ? c[0xfb] : 0, *(const volatile uint8_t *)0x92158e0,
            *(const volatile uint32_t *)0x92158e8);
}

// 0x8375a90: push ebp; mov ebp,esp; push edi; push esi (5 bytes).
void wmmt4CardPollEntry(void);
__asm__(".text\n"
        ".globl wmmt4CardPollEntry\n"
        "wmmt4CardPollEntry:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl 0x28(%esp), %eax\n"
        "  movl 0x24(%esp), %edx\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl %edx\n"
        "  pushl %eax\n"
        "  calll wmmt4CardPoll\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %edi\n"
        "  pushl %esi\n"
        "  pushl $0x8375a95\n"
        "  ret\n");

// WMMT4_BOOT_TRACE: which call inside 0x838f530 fails. That function opens the
// serial device named by [0x921e9f8] twice (0x8057d08 with 0x902), sets
// TIOCGSERIAL/TIOCSSERIAL between the opens, and only the open results are
// tested for negative: a js after either open jumps to 0x838f994/0x838f934,
// which return -1. The ioctl returns are never checked, and the later
// formatting/read/write failures return -2 and -3 (0xfffffffe/0xfffffffd). So
// a -1 from 0x838f530 can only be one of the two opens. These call sites keep
// their arguments in the caller's reserved frame (no push), so at a stub that
// replaces the call the first argument is at esp+0x24, not esp+0x28.
__attribute__((visibility("hidden"))) uint32_t wmmt4CardSerialCall(uint32_t target, uint32_t site, uint32_t a,
                                                                  uint32_t b, uint32_t c)
{
    static unsigned calls;
    int (*fn)(uint32_t, uint32_t, uint32_t) = (int (*)(uint32_t, uint32_t, uint32_t))(uintptr_t)target;
    int r = fn(a, b, c);

    calls++;
    if (!bootTrace || calls > 12)
        return (uint32_t)r;
    if (target == 0x8057d08)
        fprintf(stderr, "WMMT4 boot: serial open %u at %#x: path \"%s\" flags %#x -> %d\n", calls, site,
                (const char *)(uintptr_t)a, b, r);
    else
        fprintf(stderr, "WMMT4 boot: serial call %u at %#x(%#x, %#x, %#x) -> %d\n", calls, site, a, b, c, r);
    return (uint32_t)r;
}

#define WMMT4_CARD_CALL_STUB(name, target, site, resume)                                                           \
    __asm__(".text\n"                                                                                              \
            ".globl " #name "\n"                                                                                   \
            #name ":\n"                                                                                            \
            "  pushfl\n"                                                                                           \
            "  pushal\n"                                                                                           \
            "  movl 0x24(%esp), %eax\n"                                                                            \
            "  movl 0x28(%esp), %edx\n"                                                                            \
            "  movl 0x2c(%esp), %ecx\n"                                                                            \
            "  movl %esp, %ebp\n"                                                                                  \
            "  andl $-16, %esp\n"                                                                                  \
            "  pushl %ecx\n"                                                                                       \
            "  pushl %edx\n"                                                                                       \
            "  pushl %eax\n"                                                                                       \
            "  pushl $" site "\n"                                                                                 \
            "  pushl $" target "\n"                                                                               \
            "  calll wmmt4CardSerialCall\n"                                                                        \
            "  movl %eax, 0x1c(%ebp)\n"                                                                            \
            "  movl %ebp, %esp\n"                                                                                  \
            "  popal\n"                                                                                            \
            "  popfl\n"                                                                                            \
            "  pushl $" resume "\n"                                                                               \
            "  ret\n")

// 0x837516e: call 0x838f530(path 0x8ee9fe2, buf, 0x400); 0x8375177 tests it.
void wmmt4CardReadCall(void);
WMMT4_CARD_CALL_STUB(wmmt4CardReadCall, "0x838f530", "0x837516e", "0x8375173");

// 0x838f562 and 0x838f5d6: the two 0x8057d08 opens inside 0x838f530.
void wmmt4CardOpen1(void);
WMMT4_CARD_CALL_STUB(wmmt4CardOpen1, "0x8057d08", "0x838f562", "0x838f567");
void wmmt4CardOpen2(void);
WMMT4_CARD_CALL_STUB(wmmt4CardOpen2, "0x8057d08", "0x838f5d6", "0x838f5db");

// 0x8375791: movb $1,0xfc(%esi) (7 bytes).
void wmmt4CardFcSetEntry(void);
__asm__(".text\n"
        ".globl wmmt4CardFcSetEntry\n"
        "wmmt4CardFcSetEntry:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esi, %edx\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl $0x8375791\n"
        "  pushl %edx\n"
        "  calll wmmt4CardFcSet\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  movb $1, 0xfc(%esi)\n"
        "  pushl $0x8375798\n"
        "  ret\n");

// 0x8421840: push ebp; mov ebp,esp; push edi; push esi.
void wmmt4SelectRowEntry(void);
__asm__(".text\n"
        ".globl wmmt4SelectRowEntry\n"
        "wmmt4SelectRowEntry:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl 0x28(%esp), %eax\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl %eax\n"
        "  calll wmmt4SelectRow\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %edi\n"
        "  pushl %esi\n"
        "  pushl $0x8421845\n"
        "  ret\n");

// 0x8421854: test al,al; je 0x8421aed. The answer of 0x83c78a0() is logged,
// then the two instructions are replayed.
void wmmt4SelectTest(void);
__asm__(".text\n"
        ".globl wmmt4SelectTest\n"
        "wmmt4SelectTest:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movzbl %al, %eax\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl %eax\n"
        "  calll wmmt4SelectPredicate\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  testb %al, %al\n"
        "  jne .Lwmmt4SelectTestContinue\n"
        "  movl $0x8421aed, %eax\n"
        "  jmp *%eax\n"
        ".Lwmmt4SelectTestContinue:\n"
        "  movl $0x842185c, %eax\n"
        "  jmp *%eax\n");

// 0x841bf9e and 0x841c2fe: `movl $step,0x4(%ebx)`; 0x8419460 and 0x84192a0
// are the same write through %eax.
void wmmt4SelectStep840(void);
__asm__(".text\n"
        ".globl wmmt4SelectStep840\n"
        "wmmt4SelectStep840:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl $0x841bf9e\n"
        "  pushl $0x841b840\n"
        "  pushl %ebx\n"
        "  calll wmmt4SelectStep\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  movl $0x841b840, 0x4(%ebx)\n"
        "  movl $0x841bfa5, %eax\n"
        "  jmp *%eax\n");

void wmmt4SelectStep920(void);
__asm__(".text\n"
        ".globl wmmt4SelectStep920\n"
        "wmmt4SelectStep920:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl $0x841c2fe\n"
        "  pushl $0x841b920\n"
        "  pushl %ebx\n"
        "  calll wmmt4SelectStep\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  movl $0x841b920, 0x4(%ebx)\n"
        "  movl $0x841c305, %eax\n"
        "  jmp *%eax\n");

void wmmt4SelectStepBF00(void);
__asm__(".text\n"
        ".globl wmmt4SelectStepBF00\n"
        "wmmt4SelectStepBF00:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl $0x8419460\n"
        "  pushl $0x841bf00\n"
        "  pushl %eax\n"
        "  calll wmmt4SelectStep\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  movl $0x841bf00, 0x4(%eax)\n"
        "  movl $0x8419467, %edx\n"
        "  jmp *%edx\n");

void wmmt4SelectStepC260(void);
__asm__(".text\n"
        ".globl wmmt4SelectStepC260\n"
        "wmmt4SelectStepC260:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl $0x84192a0\n"
        "  pushl $0x841c260\n"
        "  pushl %eax\n"
        "  calll wmmt4SelectStep\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  movl $0x841c260, 0x4(%eax)\n"
        "  movl $0x84192a7, %edx\n"
        "  jmp *%edx\n");

// 0x841f390: push ebp; mov ebp,esp; push edi; push esi; push ebx; sub esp,0x22c.
void wmmt4ClearStepA(void);
__asm__(".text\n"
        ".globl wmmt4ClearStepA\n"
        "wmmt4ClearStepA:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl $0x841f390\n"
        "  calll wmmt4ClearStepSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %edi\n"
        "  pushl %esi\n"
        "  pushl %ebx\n"
        "  subl $0x22c, %esp\n"
        "  pushl $0x841f39c\n"
        "  ret\n");

// 0x841fc40: the same prologue with 0x24c.
void wmmt4ClearStepB(void);
__asm__(".text\n"
        ".globl wmmt4ClearStepB\n"
        "wmmt4ClearStepB:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl $0x841fc40\n"
        "  calll wmmt4ClearStepSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %edi\n"
        "  pushl %esi\n"
        "  pushl %ebx\n"
        "  subl $0x24c, %esp\n"
        "  pushl $0x841fc4c\n"
        "  ret\n");

// 0x856de60: push ebp; mov ebp,esp; sub esp,0x18.
void wmmt4ClearStepC(void);
__asm__(".text\n"
        ".globl wmmt4ClearStepC\n"
        "wmmt4ClearStepC:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl $0x856de60\n"
        "  calll wmmt4ClearStepSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  subl $0x18, %esp\n"
        "  pushl $0x856de66\n"
        "  ret\n");

// The cabinet LAN's peer table (the object behind Sys::Net's LAN): the boot
// check's terminal step reads its entries (52 bytes each, the terminal being
// entry 4: a message pointer at +0x24 and flags at +0x30, bit 0x80 being the
// terminal's batch settings) and advances once the terminal's is there.
// WMMT4_LAN_TRACE=1 says what the game made of the terminal's datagrams.
#define PEER_SIZE 0x34
static int lanTrace;

// Sys::Net's LAN object's peer vector: the game's own
// `mov eax,[arg]; mov eax,[eax]; add eax,0xab0; ret`.
__attribute__((force_align_arg_pointer)) static void *peerVectorGet(void *self)
{
    void *vector = (uint8_t *)*(void **)self + 0xab0;
    static int logged = -1;

    if (lanTrace && vector)
    {
        const uint8_t *begin = *(const uint8_t *const *)vector;
        const uint8_t *end = *(const uint8_t *const *)((const uint8_t *)vector + 4);
        long count = end > begin ? (long)(end - begin) / PEER_SIZE : 0;
        int signature = (int)count;
        long i;

        for (i = 0; i < count && i < 8; i++)
        {
            const uint8_t *entry = begin + i * PEER_SIZE;
            signature = (int)(signature * 31 + *(const uint32_t *)(entry + 0x24) + *(const uint32_t *)(entry + 0x30));
        }
        if (signature != logged)
        {
            logged = signature;
            fprintf(stderr, "WMMT4 LAN: %ld peer entries\n", count);
            for (i = 0; i < count && i < 8; i++)
            {
                const uint8_t *entry = begin + i * PEER_SIZE;
                uint32_t message = *(const uint32_t *)(entry + 0x24);
                fprintf(stderr, "WMMT4 LAN:   entry %ld %08x %08x %08x flags %08x msg %08x", i,
                        ((const uint32_t *)entry)[0], ((const uint32_t *)entry)[1],
                        ((const uint32_t *)entry)[6], *(const uint32_t *)(entry + 0x30), message);
                if (message >= 0x10000)
                    fprintf(stderr, " (msg flags %02x %02x time %d)", *(const uint8_t *)(message + 0x6e),
                            *(const uint8_t *)(message + 0x6f), *(const int32_t *)(message + 0x54));
                fputc('\n', stderr);
            }
        }
    }
    return vector;
}

// Sys::Network::SetOfflineMode(int), which the game calls with that
// setting: online with the loader's server.
static void (*realSetOffline)(int);

__attribute__((force_align_arg_pointer)) static void setOffline(int offline)
{
    int forced = getConfig()->wmmt4Server ? 0 : offline;

    // WMMT4_BOOT_TRACE: the offline flag the game sets, and the one it keeps.
    if (bootTrace)
        fprintf(stderr, "WMMT4 boot: SetOfflineMode(%d) -> %d\n", offline, forced);
    realSetOffline(forced);
}

static void installSettings(void)
{
    static const uint8_t entry[] = {0x55, 0x89, 0xe5, 0x57, 0x56};
    hookWithOriginal(0x08144b90, entry, sizeof(entry), (void *)readSetting1, &realReadSetting1);
    hookWithOriginal(0x08144600, entry, sizeof(entry), (void *)readSetting2, &realReadSetting2);
}

// ---------------------------------------------------------------------------
// WMMT4_BOOT_TRACE: the result register the boot check's item 0x841a270 polls.
// The global at 0x9220c48 is one of the game's small lock-guarded records
// (0x84408b0..0x8440ef0): a state at +0 (0x8440930 reads it, 0x8440dc0 sets it
// and its flag at +4), a two-word value at +8 (0x8440a70 copies one in,
// 0x84408b0 out) and three result words at +0x10/+0x14/+0x18 that 0x8440ef0
// writes by index and 0x8440990 reads by index. The check step asks for index
// 1, i.e. +0x14, and goes on only when it is 2 or 3.
//
// One writer sets those words: 0x8440ef0, and the only calls that name this
// register are the three inside 0x844a400 (reached from 0x844ca8b and
// 0x844cf73). That one copies three codes of the object it is given - its
// +0xabc, +0xac0 and +0xac4, each 0..3: the table it maps them through at
// 0x8c63418 is the identity - into slots 0, 1 and 2. So the slot the check
// waits on is that object's second code, and it reaches 2 or 3 only if the
// object records it there and the publish then runs.
//
// Those three words are the `states` block Sys::Network::UpdateState fills.
// It publishes them from the stage arm 0x80a9c00, which the jump table at
// 0x8be4218 gives for check 0 (NONE) and check 8 (DONE) and which every other
// arm jumps back to. An empty request queue does not skip that: the entry at
// 0x80a9b8e sends it to 0x80aa22c, which calls 0x80a9200 and falls into
// 0x80a9b94, the same dispatch. That is why the publish repeats every pump
// with the queue empty (seq 0/0) and check 0 keeps republishing the same
// three words.
static const uintptr_t resultRegister = 0x09220c48;

__attribute__((visibility("hidden"))) void wmmt4ResultSlotSeen(uint32_t index, uint32_t value)
{
    static unsigned calls;

    calls++;
    if (!bootTrace || (calls > 24 && calls % 200 != 0))
        return;
    fprintf(stderr, "WMMT4 boot: result slot %u <- %u, call %u\n", index, value, calls);
}

__attribute__((visibility("hidden"))) void wmmt4ResultsPublished(uint32_t source)
{
    static unsigned calls;
    static uint32_t last[3];
    const uint32_t *codes = (const uint32_t *)(uintptr_t)(source + 0xabc);

    calls++;
    // The codes change rarely (the manager records them, then repeats them
    // every tick), so a change is printed whenever it happens.
    if (!bootTrace || (calls > 24 && calls % 200 != 0 && (source == 0 || codes[0] == last[0]) &&
                       (source == 0 || codes[1] == last[1]) && (source == 0 || codes[2] == last[2])))
        return;
    if (source)
    {
        last[0] = codes[0];
        last[1] = codes[1];
        last[2] = codes[2];
    }
    if (!source)
    {
        fprintf(stderr, "WMMT4 boot: results published %u from a null object\n", calls);
        return;
    }
    fprintf(stderr,
            "WMMT4 boot: results published %u from %#x: codes %u %u %u, slots before %u %u %u\n", calls, source,
            codes[0], codes[1], codes[2], *(const uint32_t *)(resultRegister + 0x10),
            *(const uint32_t *)(resultRegister + 0x14), *(const uint32_t *)(resultRegister + 0x18));
}

// WMMT4_BOOT_TRACE: the manager object the register's three words come from.
// 0x844b490 is its constructor/reset (called from 0x844e600 right after the
// 0xb14-byte allocation, itself called from 0x844e8ec); 0x844c5d0 is the
// update that logs "netmanager: change state: %s => %s" and publishes the
// words with 0x844a400. Its words sit at +0xabc/+0xac0/+0xac4, but the class's
// own code only clears them (0x844b66c..0x844b6ac), so what fills them is not
// in the class: logging the whole +0xab0 block, the vtable and every change
// of it shows what the three words really are and when they move.
static void managerBlockChanged(uint32_t object, uint32_t update)
{
    static unsigned calls;
    static uint32_t last[6];
    const uint32_t *words = (const uint32_t *)(uintptr_t)(object + 0xab0);
    unsigned i;
    int changed = update <= 2;

    if (!bootTrace || !object)
        return;
    calls++;
    for (i = 0; i < 6; i++)
        if (words[i] != last[i])
        {
            last[i] = words[i];
            changed = 1;
        }
    if (!changed)
        return;
    fprintf(stderr,
            "WMMT4 boot: manager %#x update %u, block %08x %08x %08x %08x %08x %08x (%u changes)\n", object, update,
            words[0], words[1], words[2], words[3], words[4], words[5], calls);
}

__attribute__((visibility("hidden"))) void wmmt4ManagerCreated(uint32_t object)
{
    if (!bootTrace || !object)
        return;
    fprintf(stderr, "WMMT4 boot: manager constructed %#x, vtable %#x\n", object,
            *(const uint32_t *)(uintptr_t)object);
}

__attribute__((visibility("hidden"))) void wmmt4ManagerUpdateSeen(uint32_t object, uint32_t caller)
{
    static unsigned updates;

    updates++;
    if (bootTrace && updates == 1)
        fprintf(stderr, "WMMT4 boot: manager update called from %#x\n", caller);
    managerBlockChanged(object, updates);
}

// 0x844b490: push %ebp; mov %esp,%ebp; push %edi; push %esi (5 bytes), object
// at 0x28. 0x844c5d0: the same prologue, object at 0x28.
void wmmt4ManagerCtor(void);
__asm__(".text\n"
        ".globl wmmt4ManagerCtor\n"
        "wmmt4ManagerCtor:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl 0x28(%ebp)\n"
        "  calll wmmt4ManagerCreated\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %edi\n"
        "  pushl %esi\n"
        "  pushl $0x844b495\n"
        "  ret\n");

void wmmt4ManagerUpdate(void);
__asm__(".text\n"
        ".globl wmmt4ManagerUpdate\n"
        "wmmt4ManagerUpdate:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl 0x24(%ebp)\n"
        "  pushl 0x28(%ebp)\n"
        "  calll wmmt4ManagerUpdateSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %edi\n"
        "  pushl %esi\n"
        "  pushl $0x844c5d5\n"
        "  ret\n");

// 0x8440ef0: push %ebp; mov %esp,%ebp; push %edi; push %esi (5 bytes). Its
// caller's return address is at 0x24(%esp) after pushfl/pushal, so the object
// is at 0x28, the index at 0x2c and the value at 0x30.
void wmmt4ResultSlotSet(void);
__asm__(".text\n"
        ".globl wmmt4ResultSlotSet\n"
        "wmmt4ResultSlotSet:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl 0x30(%ebp)\n"
        "  pushl 0x2c(%ebp)\n"
        "  calll wmmt4ResultSlotSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %edi\n"
        "  pushl %esi\n"
        "  pushl $0x8440ef5\n"
        "  ret\n");

// 0x844a400: push %ebp; mov %esp,%ebp; push %esi; push %ebx (5 bytes), its
// object at 0x28(%esp).
void wmmt4ResultPublish(void);
__asm__(".text\n"
        ".globl wmmt4ResultPublish\n"
        "wmmt4ResultPublish:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  pushl 0x28(%ebp)\n"
        "  calll wmmt4ResultsPublished\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %esi\n"
        "  pushl %ebx\n"
        "  pushl $0x844a405\n"
        "  ret\n");

// WMMT4_BOOT_TRACE: where the game decides it is offline. Two separate places
// say so, and only one of them is about the network machine:
//
//   Netservice::updateFileList() (0x8452fa0):
//       8452faa  cmp [this+0x90], 1        already run -> return
//       8452fb3  call 0x8440930(0x9220c48)  the result register's state word
//       8452fc1  jne 0x8452fe8             state != 0 -> "%s: %s(): not work(offline mode)"
//       8452fc8  [this+0x90] = 1, start the update
//     So the file list runs only while the register the boot check polls is at
//     state 0; in a seeded run it is at 1 and every update is skipped.
//
//   the auth request (0x80a1980):
//       80a1992  cmp [this+0x64], 0       non-zero -> "%s: auth: not work(offline mode)"
//       80a199d  cmp [this+0x110], 0      zero -> no session
//     i.e. an ALL.Net/mucha session is requested only while that marker is 0.
//
// The three probes below log the guard, the auth entry and every write of the
// register's state word (with the site that made it), so a run shows what sets
// the state and when the guards are consulted.
void wmmt4OfflineGuardSeen(uint32_t self, uint32_t done, uint32_t state, uint32_t flag)
{
    static unsigned calls;

    calls++;
    if (!bootTrace || (calls > 6 && calls % 300 != 0))
        return;
    fprintf(stderr, "WMMT4 boot: offline guard 0x8452fa0 entry %u: object %#x done %u, result state %u flag %u\n",
            calls, self, done, state, flag);
}

// The pcb serial string at [Sys::Network+0xec], read the way the request
// builder reads it: a libstdc++ COW string, its length at data-0xc. Returns the
// string's length as the builder sees it, and copies up to size-1 bytes.
static size_t networkSerial(const void *network, char *out, size_t size)
{
    uint32_t data = 0, length = 0;
    size_t n;

    if (!out || !size)
        return 0;
    out[0] = 0;
    if (!network)
        return 0;
    memcpy(&data, (const uint8_t *)network + 0xec, sizeof(data));
    if (!data)
        return 0;
    memcpy(&length, (const uint8_t *)(uintptr_t)data - 0xc, sizeof(length));
    n = length < size - 1 ? (size_t)length : size - 1;
    if (n)
        memcpy(out, (const void *)(uintptr_t)data, n);
    out[n] = 0;
    return length;
}

void wmmt4AuthRequestSeen(uint32_t self, uint32_t offline, uint32_t session)
{
    static unsigned calls;

    // WMMT4_AUTH_RETRY: remember the object and whether the serial existed yet,
    // before the trace's own throttling can skip this call.
    if (authRetry && !authObject)
    {
        char serial[64];

        authObject = self;
        authRequestBeforeSerial = networkSerial((const void *)(uintptr_t)self, serial, sizeof(serial)) <= 4;
        fprintf(stderr, "WMMT4: auth retry: first request names object %#x, pcb serial %s%s\n", self,
                serial[0] ? serial : "(empty)",
                authRequestBeforeSerial ? " - before the serial, it will be repeated" : "");
    }
    calls++;
    if (!bootTrace || (calls > 6 && calls % 300 != 0))
        return;
    fprintf(stderr, "WMMT4 boot: auth request 0x80a1980 entry %u: object %#x, offline marker %u, session %#x\n",
            calls, self, offline, session);
}

// The state word itself is written only by 0x8440dc0 (register, value), whose
// callers are the boot check row (0x8421703, 0x8422234, 0x8422281), the Test
// Mode screen (0x840b05c, 0x840b0ff, 0x840b11f) and a later subsystem
// (0x858ec49, 0x858ee99, 0x858eed9, 0x858ef7f). A probe on that setter was
// tried and removed: patching it destabilised the run (segfault / a pthread
// priority assertion within a minute), so the writers are listed here instead.

// 0x8452fa0: push %ebp; mov %esp,%ebp; push %ebx; sub $0x14,%esp (7 bytes).
void wmmt4OfflineGuard(void);
__asm__(".text\n"
        ".globl wmmt4OfflineGuard\n"
        "wmmt4OfflineGuard:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  subl $12, %esp\n"
        "  movl 0x28(%ebp), %ebx\n"
        "  movl 0x90(%ebx), %eax\n"
        "  pushl 0x9220c4c\n"
        "  pushl 0x9220c48\n"
        "  pushl %eax\n"
        "  pushl %ebx\n"
        "  calll wmmt4OfflineGuardSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %ebx\n"
        "  subl $0x14, %esp\n"
        "  pushl $0x8452fa7\n"
        "  ret\n");

// 0x80a1980: push %ebp; mov %esp,%ebp; sub $0x78,%esp; mov %ebx,-0xc(%ebp);
//            mov 0x8(%ebp),%ebx; mov %esi,-0x8(%ebp) (15 bytes).
void wmmt4AuthRequest(void);
__asm__(".text\n"
        ".globl wmmt4AuthRequest\n"
        "wmmt4AuthRequest:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  subl $12, %esp\n"
        "  movl 0x28(%ebp), %ebx\n"
        "  pushl 0x110(%ebx)\n"
        "  pushl 0x64(%ebx)\n"
        "  pushl %ebx\n"
        "  calll wmmt4AuthRequestSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  subl $0x78, %esp\n"
        "  movl %ebx, -0xc(%ebp)\n"
        "  movl 0x8(%ebp), %ebx\n"
        "  movl %esi, -0x8(%ebp)\n"
        "  pushl $0x80a198f\n"
        "  ret\n");

// WMMT4_BOOT_TRACE: the only path to the renew request. 0x80a77f0 is the thunk
// that calls Sys::Network's renew request 0x80a7480; its only caller is
// 0x840b098, inside 0x840af20 - the virtual update of the class whose vtable is
// at 0x8c54560 and whose name string at 0x8c54584 reads "...TestModeNetworkStatus",
// i.e. Mode::TestModeNetworkStatus, the Test Mode -> Network Status screen. It is
// a three-phase machine on [this+0x4c]:
//
//   phase 0 (0x840b070): 0x8411350(this) walks its inbound-event table
//                        ([this+0x20], 0x30 bytes an entry, [this+0x2c] the entry
//                        index, [this+0x30] the count), then 0x8411c80(this, 0xe7)
//                        asks whether the current entry's id (its +4) is 0xe7.
//                        Only if it is does the screen call the renew request and
//                        move to phase 1 with [this+0x50] and [this+0x54] cleared.
//   phase 1 (0x840aff8): waits on 0x80a5f00() and on the status text at
//                        0x9220b84; unless that byte is set it calls
//                        0x8440dc0(0x9220c48, 1) and goes to phase 3.
//   phase 3 (0x840afd0): counts 60 updates ([this+0x50] up to 0x3b) and returns
//                        to phase 0.
//
// So before it asks for renewal the screen expects inbound network message 0xe7
// to be the current entry of that table - a message from the far end, not a
// state the game can reach on its own.
void wmmt4RenewScreenSeen(uint32_t self, uint32_t phase, uint32_t count, uint32_t index, uint32_t event)
{
    static unsigned calls;

    calls++;
    if (!bootTrace || (calls > 6 && calls % 300 != 0))
        return;
    fprintf(stderr,
            "WMMT4 boot: renew screen update %u: object %#x phase %u, events %u index %u, current event %#x\n",
            calls, self, phase, count, index, event);
}

// 0x840af20: push %ebp; mov %esp,%ebp; sub $0xb8,%esp; mov %ebx,-0xc(%ebp);
//            mov 0x8(%ebp),%ebx (15 bytes).
void wmmt4RenewScreenUpdate(void);
__asm__(".text\n"
        ".globl wmmt4RenewScreenUpdate\n"
        "wmmt4RenewScreenUpdate:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  subl $24, %esp\n"
        "  movl 0x28(%ebp), %ebx\n"
        "  movl 0x4c(%ebx), %ecx\n"
        "  movl 0x30(%ebx), %edx\n"
        "  movl $-1, %edi\n"
        "  testl %edx, %edx\n"
        "  je 2f\n"
        "  movl 0x2c(%ebx), %esi\n"
        "  imull $0x30, %esi, %esi\n"
        "  addl 0x20(%ebx), %esi\n"
        "  movl 4(%esi), %edi\n"
        "2:\n"
        "  pushl %edi\n"
        "  movl 0x2c(%ebx), %esi\n"
        "  pushl %esi\n"
        "  pushl %edx\n"
        "  pushl %ecx\n"
        "  pushl %ebx\n"
        "  calll wmmt4RenewScreenSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  subl $0xb8, %esp\n"
        "  movl %ebx, -0xc(%ebp)\n"
        "  movl 0x8(%ebp), %ebx\n"
        "  pushl $0x840af2f\n"
        "  ret\n");

// WMMT4_BOOT_TRACE: who can touch Sys::Network's check stage (+0x274). The
// image assigns it 1 (INTERFACE) at exactly two places, both in the NET_RENEW
// arm: 0x80a8770 inside the renew step 0x80a8500, and 0x80aa0e6 in UpdateState
// right after that step returns. Every other assignment is either a later
// stage of the sequence (2..16, from the arms of 0x80a9b70 and from the check
// routines 0x80a4110/0x80a4400/0x80a45a0/0x80a4750/0x80a48b0/0x80a7480/
// 0x80a7810/0x80a79f0/0x80a7d40/0x80a8060) or a reset to 0 (0x809fd40,
// 0x80a6880, 0x80a87a0 and UpdateState's own guard for a stage above 0x10).
// So the sequence is only (re)entered at INTERFACE by the renew step, and the
// renew step is only reached from stage 9 - which 0x80a7480 sets, and only
// when the stage is already 8 (DONE). Logging the three entries that set the
// stage to 0 or 9 shows whether any boot path touches it at all.
__attribute__((visibility("hidden"))) void wmmt4CheckStageSeen(uint32_t which, uint32_t self)
{
    static const char *const names[] = {"setAddress 0x809fd40", "reset 0x80a87a0", "renew request 0x80a7480"};
    static unsigned calls;

    calls++;
    if (!bootTrace || (calls > 9 && calls % 100 != 0))
        return;
    fprintf(stderr, "WMMT4 boot: network %s entry %u: object %#x, check stage %d\n", names[which], calls, self,
            self ? (int)*(const uint32_t *)((const uint8_t *)(uintptr_t)self + 0x274) : -1);
}

// All three are entry hooks with argument 1 at 0x28 after pushfl/pushal.
// 0x80a87a0: push %ebp; mov %esp,%ebp; push %edi; push %esi; push %ebx; sub $0x3c,%esp (9 bytes).
void wmmt4CheckStageReset(void);
__asm__(".text\n"
        ".globl wmmt4CheckStageReset\n"
        "wmmt4CheckStageReset:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  subl $4, %esp\n"
        "  pushl 0x28(%ebp)\n"
        "  pushl $1\n"
        "  calll wmmt4CheckStageSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %edi\n"
        "  pushl %esi\n"
        "  pushl %ebx\n"
        "  subl $0x3c, %esp\n"
        "  pushl $0x80a87a9\n"
        "  ret\n");

// 0x80a7480: the same prologue with sub $0x8c,%esp (12 bytes).
void wmmt4CheckStageRenew(void);
__asm__(".text\n"
        ".globl wmmt4CheckStageRenew\n"
        "wmmt4CheckStageRenew:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  subl $4, %esp\n"
        "  pushl 0x28(%ebp)\n"
        "  pushl $2\n"
        "  calll wmmt4CheckStageSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %edi\n"
        "  pushl %esi\n"
        "  pushl %ebx\n"
        "  subl $0x8c, %esp\n"
        "  pushl $0x80a748c\n"
        "  ret\n");

// 0x809fd40: push %ebp; mov %esp,%ebp; push %esi; push %ebx; sub $0x70,%esp (8 bytes).
void wmmt4CheckStageAddress(void);
__asm__(".text\n"
        ".globl wmmt4CheckStageAddress\n"
        "wmmt4CheckStageAddress:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  subl $4, %esp\n"
        "  pushl 0x28(%ebp)\n"
        "  pushl $0\n"
        "  calll wmmt4CheckStageSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  pushl %esi\n"
        "  pushl %ebx\n"
        "  subl $0x70, %esp\n"
        "  pushl $0x809fd48\n"
        "  ret\n");

// WMMT4_BOOT_TRACE: the VS net common's own mode setters, the ones the
// strings name (set_state 0x845b500, SetVSStart 0x845b610). They set an
// integer mode in their own object (its +4, through 0x844a8e0), not the words
// the boot check polls, and their callers are the VS gameplay code
// (0x83e8a7b, 0x83e8afe, 0x84e430b, 0x84e43b3). Logging them shows whether
// any runs during a boot at all.
__attribute__((visibility("hidden"))) void wmmt4VsModeSeen(uint32_t which, uint32_t self, uint32_t value)
{
    static unsigned calls;

    calls++;
    if (!bootTrace || (calls > 12 && calls % 200 != 0))
        return;
    fprintf(stderr, "WMMT4 boot: VSNetCommon %s(%u) call %u on %#x\n",
            which ? "SetVSStart" : "set_state", value, calls, self);
}

// Both: push %ebp; mov %esp,%ebp; sub $0x18,%esp (6 bytes), so argument 1 is
// at 0x28 and argument 2 at 0x2c after pushfl/pushal.
void wmmt4VsSetState(void);
__asm__(".text\n"
        ".globl wmmt4VsSetState\n"
        "wmmt4VsSetState:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  subl $4, %esp\n"
        "  pushl 0x2c(%ebp)\n"
        "  pushl 0x28(%ebp)\n"
        "  pushl $0\n"
        "  calll wmmt4VsModeSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  subl $0x18, %esp\n"
        "  pushl $0x845b506\n"
        "  ret\n");

void wmmt4VsStart(void);
__asm__(".text\n"
        ".globl wmmt4VsStart\n"
        "wmmt4VsStart:\n"
        "  pushfl\n"
        "  pushal\n"
        "  movl %esp, %ebp\n"
        "  andl $-16, %esp\n"
        "  subl $4, %esp\n"
        "  pushl 0x2c(%ebp)\n"
        "  pushl 0x28(%ebp)\n"
        "  pushl $1\n"
        "  calll wmmt4VsModeSeen\n"
        "  movl %ebp, %esp\n"
        "  popal\n"
        "  popfl\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  subl $0x18, %esp\n"
        "  pushl $0x845b616\n"
        "  ret\n");

// ---------------------------------------------------------------------------

// Bisecting aid: WMMT4_NO_HOOK=<name>[,<name>...] leaves the hook with that
// name out, so a boot can be compared with one hook missing. Unset: all on.
static int hookOff(const char *name)
{
    const char *list = getenv("WMMT4_NO_HOOK");
    size_t length = strlen(name);

    for (const char *at = list ? strstr(list, name) : NULL; at; at = strstr(at + length, name))
        if ((at == list || at[-1] == ',') && (at[length] == '\0' || at[length] == ','))
            return 1;
    return 0;
}

void namcoEs1Wmmt4Install(void)
{
    static const uint8_t haspEntry[] = {0x83, 0xec}; // sub $imm8,%esp
    static const uint8_t frameEntry[] = {0x55, 0x89, 0xe5}; // push %ebp; mov %esp,%ebp

    gameLog = getenv("NAMCO_GAME_LOG") != NULL;
    cardReaderSetting = getenv("WMMT4_NO_CARD") ? 0 : 1;
    authTrace = getenv("WMMT4_AUTH_TRACE") != NULL;
    // WMMT4_AUTH_RETRY: off unless asked for; the retry itself is in
    // networkUpdateState and the entry hook below.
    authRetry = getenv("WMMT4_AUTH_RETRY") != NULL;
    if (authRetry)
        fprintf(stderr, "WMMT4: auth retry enabled by WMMT4_AUTH_RETRY (experiment)\n");
    loadGlEntryPoints();
    haspImage();
    detour(0x08921fd0, haspEntry, sizeof(haspEntry), haspLogin);
    detour(0x08922070, haspEntry, sizeof(haspEntry), haspNothing);  // hasp_logout
    detour(0x0892215c, haspEntry, sizeof(haspEntry), haspNothing);  // hasp_encrypt
    detour(0x08922248, haspEntry, sizeof(haspEntry), haspNothing);  // hasp_decrypt
    detour(0x08922f60, haspEntry, sizeof(haspEntry), haspGetSize);
    detour(0x08922dc8, haspEntry, sizeof(haspEntry), haspRead);
    detour(0x08922e94, haspEntry, sizeof(haspEntry), haspWrite);
    detour(0x08390ba0, frameEntry, sizeof(frameEntry), jvioMonitor);
    detour(0x089e5c80, frameEntry, sizeof(frameEntry), sendCommand);
    detour(0x0809ddb0, frameEntry, sizeof(frameEntry), logMes);
    detour(0x0821c920, (const uint8_t[]){0x55}, 1, addressChanged);
    detour(0x0821d4f0, frameEntry, sizeof(frameEntry), interfaceUpdate);
    detour(0x080aa7d0, frameEntry, sizeof(frameEntry), networkUpdateState);
    // The 認証 check's own functions: the bool at its `ret`, the follow-up call at
    // its entry, and the two record copies at their entries.
    // Every hook below only logs what the boot does, so none of them is
    // installed unless the trace is asked for: a plain boot patches nothing in
    // the game at all except the functional peer hook and its ghost patch.
    bootTrace = getenv("WMMT4_BOOT_TRACE") != NULL;
    gatePrime = getenv("WMMT4_GATE_PRIME") != NULL;
    // WMMT4_NO_NET_HOOKS: bisecting aid, off by default (the hooks are on).
    if (bootTrace && !getenv("WMMT4_NO_NET_HOOKS"))
    {
    detour(0x082578c8, (const uint8_t[]){0xc3}, 1, (void *)wmmt4AuthPresentRet);
    detour(0x0825ab80, (const uint8_t[]){0x55, 0x89, 0xe5, 0x83, 0xec, 0x18}, 6, (void *)wmmt4AuthCheckEntry);
    detour(0x0843c1e0, (const uint8_t[]){0x55, 0x89, 0xe5, 0x8b, 0x55, 0x0c, 0x8b, 0x45, 0x08}, 9,
           (void *)wmmt4AuthRecordReadEntry);
    detour(0x0843c240, (const uint8_t[]){0x55, 0x89, 0xe5, 0x8b, 0x55, 0x0c, 0x8b, 0x45, 0x08}, 9,
           (void *)wmmt4AuthRecordWriteEntry);
    detour(0x080a8500,
           (const uint8_t[]){0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x30}, 8, (void *)wmmt4CheckStart);
    detour(0x080a9b70,
           (const uint8_t[]){0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0x8c, 0x00, 0x00, 0x00}, 12,
           (void *)wmmt4UpdateState);
    }
    lanTrace = getenv("WMMT4_LAN_TRACE") != NULL;
    // The step and check hooks below only log what the boot does, so they are
    // installed only when the trace is asked for: a plain boot patches nothing
    // here at all (only the peer-table hook and its ghost-table patch remain,
    // which are functional). WMMT4_GATE_PRIME keeps its own two hooks so the
    // causal test still works without the trace, silently.
    // WMMT4_NO_STEP_HOOKS: bisecting aid, off by default (the hooks are on).
    if (bootTrace && !getenv("WMMT4_NO_STEP_HOOKS"))
    {
    detour(0x0841cc10, (const uint8_t[]){0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x20}, 8, (void *)wmmt4Stepcc10);
    detour(0x0841a530, (const uint8_t[]){0x55, 0x89, 0xe5, 0x53, 0x31, 0xdb, 0x83, 0xec, 0x14}, 9,
           (void *)wmmt4ModeWait);
    detour(0x08422bd0, (const uint8_t[]){0x55, 0x89, 0xe5, 0x81, 0xec, 0xd8, 0x00, 0x00, 0x00}, 9,
           (void *)wmmt4StepMachine);
    detour(0x08439560, (const uint8_t[]){0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0xc7, 0x00}, 8, (void *)wmmt4GateCtor);
    detour(0x084395f0, (const uint8_t[]){0x55, 0x89, 0xe5, 0x8b, 0x55, 0x0c, 0x8b, 0x45, 0x08}, 9, (void *)wmmt4GateSet);
    detour(0x0841f390, (const uint8_t[]){0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0x2c, 0x02, 0x00, 0x00},
           12, (void *)wmmt4ClearStepA);
    detour(0x0841fc40, (const uint8_t[]){0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0x4c, 0x02, 0x00, 0x00},
           12, (void *)wmmt4ClearStepB);
    detour(0x0856de60, (const uint8_t[]){0x55, 0x89, 0xe5, 0x83, 0xec, 0x18}, 6, (void *)wmmt4ClearStepC);
    // The result register the stalled check item 0x841a270 polls: its slot
    // writer and the one publisher that names it (see the block above).
    if (!hookOff("resultslot"))
        detour(0x08440ef0, (const uint8_t[]){0x55, 0x89, 0xe5, 0x57, 0x56}, 5, (void *)wmmt4ResultSlotSet);
    if (!hookOff("resultpublish"))
        detour(0x0844a400, (const uint8_t[]){0x55, 0x89, 0xe5, 0x56, 0x53}, 5, (void *)wmmt4ResultPublish);
    // The manager those words come from: its construction and its update.
    if (!hookOff("managerctor"))
        detour(0x0844b490, (const uint8_t[]){0x55, 0x89, 0xe5, 0x57, 0x56}, 5, (void *)wmmt4ManagerCtor);
    if (!hookOff("managerupdate"))
        detour(0x0844c5d0, (const uint8_t[]){0x55, 0x89, 0xe5, 0x57, 0x56}, 5, (void *)wmmt4ManagerUpdate);
    // Where the game decides it is offline: the file-list guard, the auth
    // request's guard and every write of the result register's state.
    if (!hookOff("offlineguard"))
        detour(0x08452fa0, (const uint8_t[]){0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14}, 7, (void *)wmmt4OfflineGuard);
    if (!hookOff("authrequest"))
    {
        detour(0x080a1980,
               (const uint8_t[]){0x55, 0x89, 0xe5, 0x83, 0xec, 0x78, 0x89, 0x5d, 0xf4, 0x8b, 0x5d, 0x08, 0x89, 0x75, 0xf8},
               15, (void *)wmmt4AuthRequest);
    }
    // The Test Mode -> Network Status screen, the only caller of the renew
    // request: set the check stage to 0 or to 9 (see the block above).
    if (!hookOff("renewscreen"))
    {
        detour(0x0840af20,
               (const uint8_t[]){0x55, 0x89, 0xe5, 0x81, 0xec, 0xb8, 0x00, 0x00, 0x00, 0x89, 0x5d, 0xf4, 0x8b, 0x5d,
                                 0x08},
               15, (void *)wmmt4RenewScreenUpdate);
    }
    // Who can touch Sys::Network's check stage: the three entries that set it
    // to 0 or 9 (see the block above).
    if (!hookOff("checkstage"))
    {
        detour(0x0809fd40, (const uint8_t[]){0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x70}, 8,
               (void *)wmmt4CheckStageAddress);
        detour(0x080a87a0, (const uint8_t[]){0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x3c}, 9,
               (void *)wmmt4CheckStageReset);
        detour(0x080a7480, (const uint8_t[]){0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0x8c, 0x00, 0x00, 0x00},
               12, (void *)wmmt4CheckStageRenew);
    }
    // The VS net common's mode setters, whose strings named the mode states.
    if (!hookOff("vsmode"))
    {
        detour(0x0845b500, (const uint8_t[]){0x55, 0x89, 0xe5, 0x83, 0xec, 0x18}, 6, (void *)wmmt4VsSetState);
        detour(0x0845b610, (const uint8_t[]){0x55, 0x89, 0xe5, 0x83, 0xec, 0x18}, 6, (void *)wmmt4VsStart);
    }
    }
    // WMMT4_AUTH_RETRY: the request's entry is needed even when the trace is
    // off, so the object the first request names is known; the repeat above
    // calls that entry again. A plain boot still installs nothing here.
    if (authRetry && !bootTrace && !hookOff("authrequest"))
        detour(0x080a1980,
               (const uint8_t[]){0x55, 0x89, 0xe5, 0x83, 0xec, 0x78, 0x89, 0x5d, 0xf4, 0x8b, 0x5d, 0x08, 0x89, 0x75, 0xf8},
               15, (void *)wmmt4AuthRequest);
    // WMMT4_NO_CHECK_HOOKS: bisecting aid, off by default (the hooks are on).
    if (!getenv("WMMT4_NO_CHECK_HOOKS"))
    {
        // WMMT4_NO_CHECKITEM_HOOKS: bisecting aid, on by default.
        if ((bootTrace || gatePrime) && !getenv("WMMT4_NO_CHECKITEM_HOOKS"))
        {
    if (!hookOff("checkitem"))
        detour(0x0841b050, (const uint8_t[]){0xf7, 0xc6, 0x01, 0x00, 0x00, 0x00}, 6, (void *)wmmt4CheckItem);
    if (!hookOff("checkfail"))
        detour(0x0841b06b, (const uint8_t[]){0x84, 0xc0, 0x0f, 0x84, 0xf9, 0xfe, 0xff, 0xff}, 8, (void *)wmmt4CheckFail);
    if (!hookOff("sessioninit"))
        detour(0x08424ac7, (const uint8_t[]){0x83, 0x3d, 0x10, 0x1a, 0x22, 0x09, 0x68}, 7, (void *)wmmt4SessionInit);
    if (!hookOff("msgguard"))
        detour(0x08420953, (const uint8_t[]){0x8b, 0x15, 0xc4, 0x03, 0x22, 0x09}, 6, (void *)wmmt4MsgGuard);
    if (!hookOff("singleton"))
        detour(0x0836fe77, (const uint8_t[]){0x89, 0x1d, 0x70, 0x58, 0x21, 0x09}, 6, (void *)wmmt4SingletonInit);
    if (!hookOff("msginit"))
        detour(0x083c9364, (const uint8_t[]){0x89, 0x35, 0xe8, 0xf8, 0x21, 0x09}, 6, (void *)wmmt4MsgInit);
    if (!hookOff("msgcopy"))
        detour(0x083c7240, (const uint8_t[]){0x55, 0x8b, 0x15, 0xe8, 0xf8, 0x21, 0x09}, 7, (void *)wmmt4MsgCopy);
    if (!hookOff("pending"))
        detour(0x084328e1, (const uint8_t[]){0xa3, 0xe4, 0x04, 0x22, 0x09}, 5, (void *)wmmt4PendingSet);
    if (!hookOff("authmachine"))
        detour(0x08432660, (const uint8_t[]){0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c}, 9,
               (void *)wmmt4AuthMachine);
    if (gatePrime)
    {
        detour(0x08439600, (const uint8_t[]){0x55, 0x31, 0xc0, 0x89, 0xe5, 0x8b, 0x55, 0x08}, 8,
               (void *)wmmt4GatePoll);
        detour(0x0841afb7, (const uint8_t[]){0x8b, 0xb3, 0x34, 0x01, 0x00, 0x00}, 6, (void *)wmmt4RowFlag);
    }
    if (!hookOff("factory"))
        detour(0x08430260, (const uint8_t[]){0x55, 0x89, 0xe5, 0x83, 0xec, 0x38}, 6, (void *)wmmt4Factory);
    if (!hookOff("sessionhandler"))
        detour(0x084241e0, (const uint8_t[]){0x55, 0x89, 0xe5, 0x83, 0xec, 0x28}, 6, (void *)wmmt4SessionHandler);
    // 0x841af78 is `test eax,eax; jne 0x841af90; mov edi,[ebx+0x18]`: a window
    // with a conditional jump in it cannot be replayed by a stub that returns to
    // addr+n, because the taken branch leaves before its restore. Hooked, the
    // boot stops right after the check item (the driver's -1) and never reaches
    // network: start(); the gate's value is visible in the 0x8439600 callers
    // instead. Off unless WMMT4_CHECKGATE_HOOK asks for it.
    if (getenv("WMMT4_CHECKGATE_HOOK"))
        detour(0x0841af78, (const uint8_t[]){0x85, 0xc0, 0x75, 0x14, 0x8b, 0x7b, 0x18}, 7, (void *)wmmt4CheckGate);
        }
        // WMMT4_NO_STEPSET_HOOKS: bisecting aid, on by default.
        if (bootTrace && !getenv("WMMT4_NO_STEPSET_HOOKS"))
        {
    detour(0x08422c5e, (const uint8_t[]){0xc7, 0x43, 0x04, 0xf0, 0xa9, 0x41, 0x08}, 7, (void *)wmmt4StepSetC5E);
    detour(0x08422d47, (const uint8_t[]){0xc7, 0x43, 0x04, 0x30, 0x96, 0x41, 0x08}, 7, (void *)wmmt4StepSetD47);
    detour(0x08422da4, (const uint8_t[]){0xc7, 0x43, 0x04, 0x10, 0xcc, 0x41, 0x08}, 7, (void *)wmmt4StepSetDA4);
    detour(0x08422e7d, (const uint8_t[]){0xc7, 0x43, 0x04, 0x30, 0x96, 0x41, 0x08}, 7, (void *)wmmt4StepSetE7D);
        }
        // WMMT4_NO_SELECT_HOOKS: bisecting aid, on by default. The row item that
        // alone hands over to the two clear-step chains, its early-out
        // predicate, and the four writes that install 0x841b840/0x841b920/
        // 0x841bf00/0x841c260.
        if (bootTrace && !getenv("WMMT4_NO_SELECT_HOOKS"))
        {
    detour(0x08421840, (const uint8_t[]){0x55, 0x89, 0xe5, 0x57, 0x56}, 5, (void *)wmmt4SelectRowEntry);
    detour(0x08421854, (const uint8_t[]){0x84, 0xc0, 0x0f, 0x84, 0x91, 0x02, 0x00, 0x00}, 8,
           (void *)wmmt4SelectTest);
    detour(0x083c7750, (const uint8_t[]){0x55, 0xb8, 0x01, 0x00, 0x00, 0x00}, 6, (void *)wmmt4SelectSlotsEntry);
    detour(0x0841c260, (const uint8_t[]){0x55, 0x89, 0xe5, 0x81, 0xec, 0x78, 0x01, 0x00, 0x00}, 9,
           (void *)wmmt4SelectC260Entry);
    detour(0x08375a90, (const uint8_t[]){0x55, 0x89, 0xe5, 0x57, 0x56}, 5, (void *)wmmt4CardPollEntry);
    detour(0x08375791, (const uint8_t[]){0xc6, 0x86, 0xfc, 0x00, 0x00, 0x00, 0x01}, 7,
           (void *)wmmt4CardFcSetEntry);
    detour(0x0837516e, (const uint8_t[]){0xe8, 0xbd, 0xa3, 0x01, 0x00}, 5, (void *)wmmt4CardReadCall);
    detour(0x0838f562, (const uint8_t[]){0xe8, 0xa1, 0x87, 0xcc, 0xff}, 5, (void *)wmmt4CardOpen1);
    detour(0x0838f5d6, (const uint8_t[]){0xe8, 0x2d, 0x87, 0xcc, 0xff}, 5, (void *)wmmt4CardOpen2);
    detour(0x0841bf9e, (const uint8_t[]){0xc7, 0x43, 0x04, 0x40, 0xb8, 0x41, 0x08}, 7,
           (void *)wmmt4SelectStep840);
    detour(0x0841c2fe, (const uint8_t[]){0xc7, 0x43, 0x04, 0x20, 0xb9, 0x41, 0x08}, 7,
           (void *)wmmt4SelectStep920);
    detour(0x08419460, (const uint8_t[]){0xc7, 0x40, 0x04, 0x00, 0xbf, 0x41, 0x08}, 7,
           (void *)wmmt4SelectStepBF00);
    detour(0x084192a0, (const uint8_t[]){0xc7, 0x40, 0x04, 0x60, 0xc2, 0x41, 0x08}, 7,
           (void *)wmmt4SelectStepC260);
        }
        // WMMT4_NO_PEER_HOOKS: bisecting aid, on by default.
        if (!getenv("WMMT4_NO_PEER_HOOKS"))
        {
    detour(0x0844abe0,
           (const uint8_t[]){0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x5d, 0x8b, 0x00, 0x05, 0xb0, 0x0a, 0x00,
                             0x00, 0xc3},
           15, (void *)peerVectorGet);
    // The default ghosts (a ghost battle's opponents where the server gives
    // none) are read from the "JP" table, loaded only by the English and
    // Chinese locales (a Japanese server always gives them all): loaded
    // whatever the locale (je skipped; the file: fileFallbacks).
    patch(0x08432bce, (const uint8_t[]){0x74, 0x15}, (const uint8_t[]){0x90, 0x90}, 2);
        }
    }
#if defined(__linux__)
    *(void **)&realOpenDisplay = hookExecutableImport("XOpenDisplay", (void *)openDisplay);
    *(void **)&realCreateWindow = hookExecutableImport("XCreateWindow", (void *)createWindow);
#endif
    *(void **)&realSetsockopt = hookExecutableImport("setsockopt", (void *)gameSetsockopt);
    *(void **)&realRecvmsg = hookExecutableImport("recvmsg", (void *)gameRecvmsg);
    *(void **)&realRecvfrom = hookExecutableImport("recvfrom", (void *)gameRecvfrom);
    *(void **)&realWrite = hookExecutableImport("write", (void *)gameWrite);
    *(void **)&realOpen = hookExecutableImport("open", (void *)gameOpen);
    *(void **)&realOpen64 = hookExecutableImport("open64", (void *)gameOpen64);
    *(void **)&realRead = hookExecutableImport("read", (void *)gameRead);
    *(void **)&realFopen = hookExecutableImport("fopen", (void *)gameFopen);
    *(void **)&realFopen64 = hookExecutableImport("fopen64", (void *)gameFopen64);
    *(void **)&realFwrite = hookExecutableImport("fwrite", (void *)gameFwrite);
    *(void **)&realFread = hookExecutableImport("fread", (void *)gameFread);
    *(void **)&realFputs = hookExecutableImport("fputs", (void *)gameFputs);
    *(void **)&realFgets = hookExecutableImport("fgets", (void *)gameFgets);
    *(void **)&realFflush = hookExecutableImport("fflush", (void *)gameFflush);
    *(void **)&realFclose = hookExecutableImport("fclose", (void *)gameFclose);
    *(void **)&realGetProcAddress = hookExecutableImport("glXGetProcAddress", (void *)getProcAddress);
#if defined(__linux__)
    *(void **)&realGetWindowAttributes = hookExecutableImport("XGetWindowAttributes", (void *)getWindowAttributes);
    if (getenv("WMMT4_SHOT_PERIOD"))
        shotPeriod = atoi(getenv("WMMT4_SHOT_PERIOD"));
    if ((shotDir = getenv("WMMT4_SHOT")))
        *(void **)&realSwapBuffers = hookExecutableImport("glXSwapBuffers", (void *)swapBuffers);
    *(void **)&realChangeProperty = hookExecutableImport("XChangeProperty", (void *)changeProperty);
    *(void **)&realMoveResizeWindow = hookExecutableImport("XMoveResizeWindow", (void *)moveResizeWindow);
#endif
    *(void **)&realGetLatestProfile = hookExecutableImport("cgGLGetLatestProfile", (void *)getLatestProfile);
    *(void **)&realCreateProgram = hookExecutableImport("cgCreateProgram", (void *)createProgram);
    MH_Initialize(); // already done is fine
    static const uint8_t setOfflineEntry[] = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x08, 0xe8};
    hookWithOriginal(0x080a5f40, setOfflineEntry, sizeof(setOfflineEntry), (void *)setOffline, &realSetOffline);
    installFfb();
    installCardVendor();
    if (getConfig()->wmmt4Server)
    {
        installSettings();
        namcoEs1Wmmt4NetInstall();
        namcoEs1Wmmt4CardInstall();
    }
    namcoEs1Wmmt4StartTerminal(HASP_SERIAL);
    log_info("WMMT4: dongle serial %s", HASP_SERIAL);
}
