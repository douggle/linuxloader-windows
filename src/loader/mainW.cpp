#if defined(_WIN32) || defined(__MINGW32__)
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <filesystem>
#include <intrin.h>
#include <winsock2.h>
#include <windows.h>
#include <libgen.h>

#include "glad/gl.h"
#include "elfLoader/alsa2sdlBridge.hpp"
#include "elfLoader/segaapiBridge.hpp"
#include "elfLoader/gccBridge.hpp"
#include "elfLoader/ipcBridge.hpp"
#include "elfLoader/elfLoader.hpp"
#include "elfLoader/filesystemBridge.hpp"
#include "elfLoader/libcBridge.hpp"
#include "elfLoader/gccBridge.hpp"
#include "elfLoader/graphicsBridge.hpp"
#include "elfLoader/pthreadBridge.hpp"
#include "elfLoader/regexBridge.hpp"
#include "elfLoader/termiosBridge.hpp"
#include "elfLoader/networkBridge.hpp"
#include "elfLoader/mathBridge.hpp"
#include "elfLoader/rtLibcBridge.hpp"
#include "elfLoader/symbolResolver.hpp"
#include "elfLoader/pthread/pthreadEmu.hpp"
#include "loader/config/config.h"
#include "log/log.h"
#include "graphics/gpuVendor.h"
#include "init.h"
#include "patching/patch.h"
#include "input/sdlInput.h"
#include "mainShared.h"
#include "selfDebug.h"
extern "C"
{
#include "rawthrills/rtGame.h"
}

std::string g_absoluteElfPath;
uint32_t partialElfCrc = 0;

extern SDLControllers sdlJoysticks;

extern int gGrp;

HMODULE hGlDLL = NULL;

// Crash report: where an unhandled exception happened (the module and offset
// of the faulting instruction, the registers and the return addresses found
// on the stack), so a crash in the game or in the loader can be located from
// the log alone. Written with WriteFile so it reaches the redirected log even
// though the process is going down.
static void crashPrint(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, (DWORD)n, &written, NULL);
}

static void crashDescribeAddress(const char *label, uintptr_t addr)
{
    HMODULE mod = NULL;
    char path[MAX_PATH] = "";
    const char *elfName = NULL;
    uint32_t elfVaddr = 0;
    if (elfImageForAddress(addr, &elfName, &elfVaddr))
        crashPrint("  %s %08lx = %s @ 0x%lx\n", label, (unsigned long)addr, elfName, (unsigned long)elfVaddr);
    else if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) &&
        mod && GetModuleFileNameA(mod, path, sizeof(path)))
    {
        const char *base = strrchr(path, '\\');
        crashPrint("  %s %08lx = %s+0x%lx\n", label, (unsigned long)addr, base ? base + 1 : path,
                   (unsigned long)(addr - (uintptr_t)mod));
    }
    else if (addr >= 0x08000000 && addr < 0x10000000)
        crashPrint("  %s %08lx = game ELF image\n", label, (unsigned long)addr);
    else
        crashPrint("  %s %08lx = (no module: loaded ELF library or heap)\n", label, (unsigned long)addr);
}

// Write watchpoint on the frame the crash report blamed.
//
// Windows does not let user code touch the hardware debug registers: with
// CR4.DE clear, "mov %eax,%dr0" raises STATUS_PRIVILEGED_INSTRUCTION, in a
// 64-bit process and in WOW64 alike (arming Dr0-Dr3 from the allocator is
// how this was first tried). The watch the process can run itself is the
// trap flag: the watched thread single-steps, the watched words are compared
// after every instruction, and the instruction that changes one is named --
// which is what the hardware watchpoint was for.
//
// The window watched is the caller's frame from the call's own return address
// (the lowest word of its argument area) upwards: locals, saved EBP, its
// return address. The layout differs from function to function, so the whole
// window is checked, and only a word that held game code and lost it counts:
// that is a return address or a function pointer, and its frame can no
// longer return. Locals being reused, argument areas being filled and return
// addresses being pushed are written all the time.
//
// Two things to know when reading a trace from it: every single step goes
// through Windows' exception dispatch, which builds its CONTEXT on this
// thread's stack below ESP (those writes are ignored), and stepping the
// thread changes the interleaving enough that some races stop happening.
#define WATCH_SLOTS 32
static struct
{
    volatile LONG active;
    volatile LONG busy;
    DWORD thread;
    uint32_t lastEip; // the instruction that just ran (set after each check)
    uint32_t steps;
    int count;
    uint32_t *base;
    uint32_t value[WATCH_SLOTS];
} g_watch;

static void watchDisarm(PCONTEXT ctx)
{
    if (ctx)
        ctx->EFlags &= ~0x100;
    g_watch.active = 0;
}

// Dumps the bytes of the instruction that wrote, and the words around it.
static void watchPrintInstruction(uint32_t eip)
{
    const uint8_t *code = (const uint8_t *)(uintptr_t)(eip - 12);
    if (IsBadReadPtr(code, 18))
        return;
    crashPrint("    bytes at %08lx:", (unsigned long)(uintptr_t)code);
    for (int i = 0; i < 18; i++)
        crashPrint("%s%02x", i % 9 ? " " : "\n      ", code[i]);
    crashPrint("\n");
}

// Checks the watched words after one single-stepped instruction; true when
// stepping is to continue.
static bool watchStep(PEXCEPTION_POINTERS info)
{
    PCONTEXT ctx = info->ContextRecord;
    if (IsBadReadPtr(g_watch.base, g_watch.count * 4))
    {
        crashPrint("*** WATCHPOINT: the watched frame at %08lx is gone after %lu steps\n",
                   (unsigned long)(uintptr_t)g_watch.base, (unsigned long)g_watch.steps);
        watchDisarm(ctx);
        return true;
    }
    for (int i = 0; i < g_watch.count; i++)
    {
        uint32_t now = g_watch.base[i];
        uint32_t was = g_watch.value[i];
        if (now == was)
            continue;
        g_watch.value[i] = now;
        // Only a word that held game code being overwritten matters: that is
        // a return address or a function pointer, and the frame it belonged
        // to can no longer return. Locals being reused, and the argument
        // areas of deeper calls, are written all the time.
        bool codeWas = was >= 0x08040000 && was < 0x0a310000;
        bool codeNow = now >= 0x08040000 && now < 0x0a310000;
        if (!codeWas || codeNow)
            continue; // a push of a return address is not a corruption either
        // The exception dispatch that every single step goes through builds
        // its CONTEXT on this thread's stack, below ESP; that is dead space,
        // not the frame, and its writes are this watch's own.
        if ((uintptr_t)(g_watch.base + i) < (uintptr_t)ctx->Esp)
            continue;
        crashPrint("*** WATCHPOINT: %08lx (frame+%02x) was overwritten %08lx -> %08lx by the instruction at %08lx "
                   "(%lu steps after arming)\n",
                   (unsigned long)(uintptr_t)(g_watch.base + i), (unsigned)(i * 4), (unsigned long)was, (unsigned long)now,
                   (unsigned long)g_watch.lastEip, (unsigned long)g_watch.steps);
        crashPrint("    at the write: ESP=%08lx EBP=%08lx EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx\n",
                   (unsigned long)ctx->Esp, (unsigned long)ctx->Ebp, (unsigned long)ctx->Eax, (unsigned long)ctx->Ebx,
                   (unsigned long)ctx->Ecx, (unsigned long)ctx->Edx);
        crashDescribeAddress("writer", (uintptr_t)g_watch.lastEip);
        watchPrintInstruction(g_watch.lastEip);
        watchDisarm(ctx);
        return true;
    }
    g_watch.lastEip = (uint32_t)ctx->Eip;
    if (++g_watch.steps >= 200000)
    {
        crashPrint("*** WATCHPOINT: %lu steps without a change to the watched words; stopped\n",
                   (unsigned long)g_watch.steps);
        watchDisarm(ctx);
        return true;
    }
    // Delivering the trap cleared the flag (the CPU clears TF in the saved
    // EFLAGS), so it has to be set again for the next instruction to trap.
    ctx->EFlags |= 0x100;
    return true;
}

// Arms the watch on the frame that called the allocator: base is the word
// holding the call's return address, and everything above it up to the end
// of the window is the caller's frame.
static void watchArm(uint32_t *base)
{
    g_watch.thread = GetCurrentThreadId();
    g_watch.count = WATCH_SLOTS;
    g_watch.base = base;
    for (int i = 0; i < WATCH_SLOTS; i++)
        g_watch.value[i] = base[i];
    g_watch.steps = 0;
    g_watch.lastEip = 0;
    crashPrint("*** WATCHPOINT: thread %lu, watching %08lx..%08lx (return address %08lx + the calling frame)\n",
               (unsigned long)g_watch.thread, (unsigned long)(uintptr_t)base,
               (unsigned long)(uintptr_t)(base + WATCH_SLOTS), (unsigned long)base[0]);
    g_watch.active = 1;
    // The trap flag goes on in the last instruction here, and 1: -- the first
    // instruction to trap -- is recorded as the one that ran before it.
    __asm__ volatile("lea 1f, %%eax\n\t"
                     "mov %%eax, %0\n\t"
                     "pushf\n\t"
                     "orl $0x100, (%%esp)\n\t"
                     "popf\n"
                     "1:\n\t"
                     : "=m"(g_watch.lastEip)
                     :
                     : "eax", "memory", "cc");
}

// Called by the allocator at the end of the malloc whose caller's frame the
// crash was in: caller is the return address into the game, esp the
// allocator's stack pointer when it was entered.
//
// Off unless LL_WATCH_SITE names the call site to watch (the return address
// in the game, as hex; Cruis'n Blast's buffer helper is 088a46f3): stepping
// every instruction makes the game some fifty times slower, and for this
// crash it also stops it happening at all.
extern "C" void watchArmGameCall(uint32_t caller, uint32_t esp)
{
    if (g_watch.active || !caller)
        return;
    static uint32_t site;
    static int siteRead;
    if (!siteRead)
    {
        const char *env = getenv("LL_WATCH_SITE");
        if (env)
            site = (uint32_t)strtoul(env, NULL, 16);
        else if (getenv("LL_DEBUG_WATCH")) // the debugger side's default site
            site = 0x088a46f3u;
        siteRead = 1;
    }
    if (!site || caller != site)
        return;

    // The return address sits at the top of the allocator's frame. The same
    // value also appears lower down, spilled by this function's own calls and
    // left behind by earlier calls of the same chain, so the highest match in
    // the window is the real slot.
    uint32_t *words = (uint32_t *)(uintptr_t)esp;
    uint32_t *found = nullptr;
    int matches = 0;
    for (int i = 0; i < 0x400; i++)
    {
        if (IsBadReadPtr(words + i, 4))
            break;
        if (words[i] == caller)
        {
            found = &words[i];
            matches++;
        }
    }
    if (!found)
    {
        crashPrint("*** WATCHPOINT: the return address %08lx is not on the stack from %08lx; not armed\n",
                   (unsigned long)caller, (unsigned long)esp);
        return;
    }    if (IsBadReadPtr(found, WATCH_SLOTS * 4))
        return;

    // Under the self-debugging launcher the watch is a hardware one (Dr0/Dr1
    // set from the debugger side, on every thread): it only needs the two
    // slots of the calling frame -- its saved EBP at 0x1c above the word the
    // call left its return address in (the caller opens with "push %ebp;
    // mov %esp,%ebp; sub $0x18,%esp"), and its own return address after it.
    if (selfDebugIsChild())
    {
        uint32_t *savedEbp = found + 7;
        uint32_t *returnAddress = found + 8;
        if (IsBadReadPtr(savedEbp, 8))
            return;
        // If that is not the frame's own return address, watch the slot the
        // call itself used instead of guessing.
        bool looksLikeFrame = *returnAddress >= 0x08040000 && *returnAddress < 0x0a310000;
        selfDebugNoteSlot((uint32_t)(uintptr_t)(looksLikeFrame ? savedEbp : found),
                          (uint32_t)(uintptr_t)(looksLikeFrame ? returnAddress : 0), __readfsdword(0x04),
                          __readfsdword(0x08));
        return;
    }

    crashPrint("*** WATCHPOINT: %d copies of the return address on the stack; the highest at %08lx\n", matches,
               (unsigned long)(uintptr_t)found);
    watchArm(found);
}

static volatile LONG crashReportBusy;
static volatile LONG crashReportCount;

static LONG WINAPI crashReportFilter(PEXCEPTION_POINTERS info)
{
    // Once at a time (the report's own probing may fault), and only a few:
    // the vectored handler also sees exceptions that end up handled.
    if (InterlockedExchange(&crashReportBusy, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    bool hangReport = info->ExceptionRecord->ExceptionCode == 0; // see hangWatchdog
    if (!hangReport && InterlockedIncrement(&crashReportCount) > 3)
    {
        crashReportBusy = 0;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    PEXCEPTION_RECORD rec = info->ExceptionRecord;
    PCONTEXT ctx = info->ContextRecord;

    fflush(stdout);
    fflush(stderr);
    if (hangReport)
        crashPrint("\n*** HANG REPORT: where the game's main thread is ***\n");
    else
        crashPrint("\n*** CRASH: exception 0x%08lx at %p", (unsigned long)rec->ExceptionCode, rec->ExceptionAddress);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        crashPrint(" (%s address %08lx)", rec->ExceptionInformation[0] == 0 ? "reading" : rec->ExceptionInformation[0] == 1 ? "writing" : "executing",
                   (unsigned long)rec->ExceptionInformation[1]);
    if (!hangReport)
        crashPrint(" ***\n");
    crashDescribeAddress("EIP", (uintptr_t)ctx->Eip);
    crashPrint("  EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx\n  ESI=%08lx EDI=%08lx EBP=%08lx ESP=%08lx\n",
               ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
    // What the write watchpoint had seen when this happened.
    if (g_watch.active && g_watch.thread == GetCurrentThreadId())
    {
        crashPrint("  watchpoint: %lu steps, last instruction %08lx\n", (unsigned long)g_watch.steps,
                   (unsigned long)g_watch.lastEip);
        for (int i = 0; i < g_watch.count; i++)
            if (!IsBadReadPtr(g_watch.base + i, 4))
                crashPrint("    watched word %08lx: %08lx (armed %08lx)\n",
                           (unsigned long)(uintptr_t)(g_watch.base + i), (unsigned long)g_watch.base[i],
                           (unsigned long)g_watch.value[i]);
    }
    // The faulting thread's own stack, and what the faulting address is part
    // of: an ELF thread stack is a heap block of ours, not a Windows one.
    {
        uint32_t stackBase = __readfsdword(0x04);
        uint32_t stackLimit = __readfsdword(0x08);
        crashPrint("  thread %lu, stack [%08lx, %08lx), EIP in it: %s\n", (unsigned long)GetCurrentThreadId(),
                   (unsigned long)stackLimit, (unsigned long)stackBase,
                   ((uintptr_t)ctx->Eip >= stackLimit && (uintptr_t)ctx->Eip < stackBase) ? "yes" : "no");
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)(uintptr_t)ctx->Eip, &mbi, sizeof(mbi)) == sizeof(mbi))
            crashPrint("  EIP region: base %p size 0x%lx state %lx protect %lx type %lx\n", mbi.AllocationBase,
                       (unsigned long)mbi.RegionSize, (unsigned long)mbi.State, (unsigned long)mbi.Protect,
                       (unsigned long)mbi.Type);
    }
    // The code around the faulting instruction (64 bytes before, 32 after)
    // and the top of the stack, to disassemble from the log.
    {
        uint8_t *code = (uint8_t *)ctx->Eip - 64;
        if (!IsBadReadPtr(code, 96))
        {
            crashPrint("  Code at %08lx (EIP-64):", (unsigned long)(uintptr_t)code);
            for (int i = 0; i < 96; i++)
                crashPrint("%s%02x", i % 32 ? " " : "\n    ", code[i]);
            crashPrint("\n");
        }
        uint32_t *stack = (uint32_t *)ctx->Esp;
        if (!IsBadReadPtr(stack, 32 * 4))
        {
            crashPrint("  Stack at %08lx:", (unsigned long)ctx->Esp);
            for (int i = 0; i < 32; i++)
                crashPrint("%s%08lx", i % 8 ? " " : "\n    ", (unsigned long)stack[i]);
            crashPrint("\n");
        }
    }
    // Return addresses into the game and its Linux libraries (ELF images,
    // not Windows modules), with the bytes before each: its call.
    {
        uint32_t *stack = (uint32_t *)ctx->Esp;
        int shownElf = 0;
        crashPrint("  Return addresses into ELF code:\n");
        for (int i = 0; i < 2048 && shownElf < 12; i++)
        {
            if (IsBadReadPtr(stack + i, 4))
                break;
            uint32_t v = stack[i];
            MEMORY_BASIC_INFORMATION mbi;
            if (v < 0x01000000 || !VirtualQuery((LPCVOID)(uintptr_t)v, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
                continue;
            if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                continue;
            HMODULE mod = NULL;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(uintptr_t)v, &mod))
                continue;
            const uint8_t *site = (const uint8_t *)(uintptr_t)v - 8;
            if (IsBadReadPtr(site, 8))
                continue;
            // A call ends right before: e8 rel32, or ff /2 forms (2-7 bytes).
            bool isCall = site[3] == 0xe8 || site[6] == 0xff || site[5] == 0xff || site[4] == 0xff || site[2] == 0xff ||
                          site[1] == 0xff;
            if (!isCall)
                continue;
            crashPrint("    [esp+%03x] %08lx  call site:", i * 4, (unsigned long)v);
            for (int k = 0; k < 8; k++)
                crashPrint(" %02x", site[k]);
            crashPrint("\n");
            shownElf++;
        }
    }
    // Where the game's own data holds the address it jumped to: a stale
    // pointer in a global (or a struct) is then named by its address.
    {
        const uint32_t wanted = (uint32_t)ctx->Eip;
        int found = 0;
        uintptr_t addr = 0x08040000;
        const uintptr_t end = 0x0a320000;
        while (addr < end && found < 4)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) != sizeof(mbi))
                break;
            uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)) &&
                !(mbi.Protect & PAGE_GUARD))
            {
                const uint32_t *p = (const uint32_t *)mbi.BaseAddress;
                size_t n = mbi.RegionSize / 4;
                for (size_t i = 0; i < n && found < 4; i++)
                    if (p[i] == wanted)
                    {
                        crashPrint("  game data at %08lx holds the fault address\n",
                                   (unsigned long)((uintptr_t)p + i * 4));
                        found++;
                    }
            }
            addr = regionEnd > addr ? regionEnd : addr + 0x1000;
        }
        if (!found)
            crashPrint("  (the fault address is in no readable part of the game's data)\n");
    }

    crashPrint("  Code addresses on the stack:\n");
    uintptr_t *sp = (uintptr_t *)ctx->Esp;
    int shown = 0;
    for (int i = 0; i < 512 && shown < 16; i++)
    {
        if (IsBadReadPtr(sp + i, sizeof(uintptr_t)))
            break;
        uintptr_t v = sp[i];
        MEMORY_BASIC_INFORMATION mbi;
        if (v < 0x10000 || !VirtualQuery((LPCVOID)v, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            continue;
        if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            continue;
        char label[24];
        snprintf(label, sizeof(label), "[esp+%03x]", i * 4);
        crashDescribeAddress(label, v);
        shown++;
    }
    crashPrint("*** end of crash report ***\n");
    crashReportBusy = 0;
    return EXCEPTION_CONTINUE_SEARCH;
}

// Hang report (LL_HANG_REPORT=1): a game that stops logging may be waiting or
// looping. After 20 seconds, and twice more 20 seconds apart, the main thread
// is paused for a moment and where it is gets printed as a crash report
// would. Asked for only: in a game that runs, it reads as a crash.
static HANDLE gameMainThread;

static DWORD WINAPI hangWatchdog(LPVOID)
{
    for (int i = 0; i < 3; i++)
    {
        Sleep(20000);
        if (!gameMainThread || SuspendThread(gameMainThread) == (DWORD)-1)
            return 0;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(gameMainThread, &ctx))
        {
            EXCEPTION_RECORD rec;
            memset(&rec, 0, sizeof(rec));
            rec.ExceptionAddress = (PVOID)(uintptr_t)ctx.Eip;
            EXCEPTION_POINTERS info = {&rec, &ctx};
            crashReportFilter(&info);
        }
        ResumeThread(gameMainThread);
    }
    return 0;
}

LONG CALLBACK myVectoredHandler(PEXCEPTION_POINTERS ExceptionInfo)
{
    DWORD exceptionCode = ExceptionInfo->ExceptionRecord->ExceptionCode;
    PCONTEXT ctx = ExceptionInfo->ContextRecord;


    // The write watchpoint's single steps (see g_watch): each one checks the
    // watched words, and stepping continues until one of them changes.
    if (exceptionCode == EXCEPTION_SINGLE_STEP && g_watch.active && g_watch.thread == GetCurrentThreadId())
    {
        if (InterlockedExchange(&g_watch.busy, 1) == 0)
        {
            watchStep(ExceptionInfo);
            InterlockedExchange(&g_watch.busy, 0);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (exceptionCode == EXCEPTION_ACCESS_VIOLATION || exceptionCode == EXCEPTION_PRIV_INSTRUCTION) {
        uint8_t* pint = (uint8_t*)ctx->Eip;
        if (pint && !IsBadReadPtr(pint, 2) && pint[0] == 0xCD && pint[1] == 0x80) {        
            if (ctx->Eax == 0xE0) { 
                ctx->Eax = (DWORD)PthreadEmu::pthreadSelf();
                ctx->Eip += 2;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }

    // The game runs on a stack of the loader's, where Windows' own unhandled
    // exception handling may not get to run: report the faults here, as they
    // happen (most often fatal: see crashReportFilter).
    switch (exceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        crashReportFilter(ExceptionInfo);
        break;
    default:
        break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}   


void initBridges()
{
    FileSystemBridge::initBridges();
    GraphicsBridge::initBridges();
    LibcBridge::initBridges();
    PthreadBridge::initBridges();
    TermiosBridge::initBridges();
    GccBridge::initBridges();
    NetworkBridge::initBridges();
    IpcBridge::initBridges();
    SegaApiBridge::initBridges();
    Alsa2SdlBridge::initBridges();
    RegexBridge::initBridges();
    MathBridge::initBridges();
    // Last: only names the bridges above leave unanswered.
    RtLibcBridge::initBridges();
}

int main(int argc, char *argv[], char *envp[])
{
    // With LL_DEBUG_WATCH, this starts a second copy of itself under the
    // debugger in selfDebug.cpp, which sets the hardware write watchpoint.
    {
        int childExit = 0;
        if (selfDebugRun(&childExit))
            return childExit;
    }

    // Unbuffered, so a crash does not lose what was logged before it.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    SetUnhandledExceptionFilter(crashReportFilter);

    // LL_LOG_LEVEL=trace|debug|info|warn (default warn).
    {
        const char *level = getenv("LL_LOG_LEVEL");
        int min = LOG_WARN;
        if (level && !_stricmp(level, "trace"))
            min = LOG_TRACE;
        else if (level && !_stricmp(level, "debug"))
            min = LOG_DEBUG;
        else if (level && !_stricmp(level, "info"))
            min = LOG_INFO;
        logSetMinLevel(min);
    }

    char command[MAX_PATH_LENGTH] = {0};
    char originalDir[MAX_PATH_LENGTH] = {0};
    char gameELF[MAX_PATH_LENGTH] = {0};
    char libraryPath[MAX_PATH_LENGTH] = {0};
    char gamePath[MAX_PATH_LENGTH] = {0};

    log_info("Parsing arguments...\n");
    if (parseArgs(argc, argv, command, originalDir, gameELF, libraryPath) != PARSE_ARGS_SUCCESS)
        return EXIT_SUCCESS;

    // The configuration paths, kept in the loader's own storage: the heap
    // copies parseArgs made were found overwritten by the time initConfig()
    // read them (garbage file names in the log).
    static char savedConfigPath[MAX_PATH_LENGTH];
    static char savedControlsPath[MAX_PATH_LENGTH];
    snprintf(savedConfigPath, sizeof(savedConfigPath), "%s", configPath ? configPath : "");
    snprintf(savedControlsPath, sizeof(savedControlsPath), "%s", controlsPath ? controlsPath : "");
    configPath = savedConfigPath;
    controlsPath = savedControlsPath;
    log_info("Config file: '%s', controls file: '%s'", configPath, controlsPath);

    char elfPath[MAX_PATH_LENGTH];
    char elfArgs[MAX_PATH_LENGTH] = "";

    char *spacePos = strchr(gameELF, ' ');
    if (spacePos != NULL)
    {
        size_t elfPathLen = spacePos - gameELF;
        strncpy(elfPath, gameELF, elfPathLen);
        elfPath[elfPathLen] = '\0';
        strcpy(elfArgs, spacePos + 1);
    }
    else
    {
        strcpy(elfPath, gameELF);
    }

    ElfLoader::PreReserveAddressSpace(elfPath);

    log_info("Initializing library search paths...\n");
    // InitSearchPaths() takes the game's executable and searches its folder
    // (and lib\ there): the current directory is the game's.
    GetCurrentDirectoryA(MAX_PATH_LENGTH, gamePath);
    {
        size_t len = strlen(gamePath);
        snprintf(gamePath + len, MAX_PATH_LENGTH - len, "\\%s", elfPath);
    }
    SymbolResolver::GetInstance().InitSearchPaths(libraryPath, gamePath);
    // What readlink("/proc/self/exe") answers (games find their folder by it).
    g_absoluteElfPath = gamePath;
    for (char &c : g_absoluteElfPath)
        if (c == '\\')
            c = '/';

    // Install Exception Handler (BEFORE any C++ code runs)
    if (!AddVectoredExceptionHandler(1, myVectoredHandler))
        log_error("Failed to register MyVectoredHandler");

    log_info("Initializing bridges...\n");
    initBridges();

    if (!std::filesystem::exists("tmp"))
        std::filesystem::create_directory("tmp");
    if (!std::filesystem::exists("tmp\\segaboot"))
        std::filesystem::create_directory("tmp\\segaboot");

    log_info("Loading ELF file: %s\n", elfPath);
    ElfLoader loader;

    if (!loader.Load(elfPath))
    {
        log_fatal("Failed to load ELF file: %s", elfPath);
        return 1;
    }

    uint8_t *baseAddr = (uint8_t *)loader.GetBaseAddress();
    if (baseAddr)
        partialElfCrc = rtPatchedCrc(getCrc32Mem(baseAddr + 10, 0x4000));

    // The configuration paths were found overwritten by the time the game was
    // loaded: point them at the loader's copies again.
    configPath = savedConfigPath;
    controlsPath = savedControlsPath;
    initConfig(configPath);

    if (partialElfCrc == PRIMEVAL_HUNT_SBPP && getConfig()->phUseZink)
    {
        SymbolResolver::GetInstance().AddDllSearchSubPath("zink");
        SetEnvironmentVariableA("GALLIUM_DRIVER", "zink");
        std::string zinkGlPath = SymbolResolver::GetInstance().GetDllSearchBase() + "\\zink\\opengl32.dll";
        hGlDLL = LoadLibraryA(zinkGlPath.c_str());
    }
    else
    {
        hGlDLL = LoadLibraryA("opengl32.dll");
    }

    if (!hGlDLL)
    {
        printf("Failed to load OpenGL!\n");
        return -1;
    }

    log_debug("Initializing main...");
    configPath = savedConfigPath;
    controlsPath = savedControlsPath;
    initMain(configPath, controlsPath);

    int final_argc = 0;
    char *final_argv_arr[10];

    final_argv_arr[final_argc++] = elfPath;

    // Raw Thrills games told their render size on the command line, as the
    // Linux launcher does (rawthrills/rtLaunch.c): else they draw at their
    // cabinet's size in a corner of the window.
    static char sizeArgument[64];
    const RtGame *rtGame = rtGetGame(partialElfCrc);
    if (rtGame && rtGame->sizeArgument)
    {
        int width = rtGame->frameTurn ? rtGame->frameTurnWidth : getConfig()->width;
        int height = rtGame->frameTurn ? rtGame->frameTurnHeight : getConfig()->height;
        if (width > 0 && height > 0)
        {
            snprintf(sizeArgument, sizeof(sizeArgument), rtGame->sizeArgument, width, height);
            final_argv_arr[final_argc++] = sizeArgument;
        }
    }

    if (strlen(elfArgs) > 0)
    {
        char *token = strtok(elfArgs, " ");
        while (token != NULL && final_argc < 9)
        {
            final_argv_arr[final_argc++] = token;
            token = strtok(NULL, " ");
        }
    }
    final_argv_arr[final_argc] = NULL;

    for (int i = 0; i < final_argc; i++)
        log_info("Game argv[%d] = '%s'", i, final_argv_arr[i] ? final_argv_arr[i] : "(null)");
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &gameMainThread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    {
        const char *hang = getenv("LL_HANG_REPORT");
        if (hang && *hang && strcmp(hang, "0") != 0)
            CloseHandle(CreateThread(NULL, 0, hangWatchdog, NULL, 0, NULL));
    }
    if (!loader.Execute(final_argc, final_argv_arr, envp))
    {
        log_error("Failed to execute ELF file.");
        return 1;
    }

    log_info("ELF Execution Finished");
    return 0;
}

#endif