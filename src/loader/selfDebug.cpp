#if defined(_WIN32) || defined(__MINGW32__)
#include "selfDebug.h"

#include <windows.h>
#include <psapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See selfDebug.h. The half that runs in the debuggee only writes the address
// it wants watched; the half that runs in the debugger starts the debuggee,
// sets Dr0 on every one of its threads, and reports each write to that
// address (the writer's thread and the instruction after its write).

#define MAX_DEBUG_THREADS 128
#define MAX_WATCH_REPORTS 8

static DWORD g_childPid;
static HANDLE g_childProcess;
static HANDLE g_armedEvent;
static bool g_armed;
#define MAX_WATCH_ADDRS 3
static uint32_t g_watchAddr[MAX_WATCH_ADDRS];
static uint32_t g_watchValue[MAX_WATCH_ADDRS];
static uint32_t g_extraAddr; // LL_WATCH_ADDR: one more word, Dr2
static int g_watchCount;
static DWORD g_watchOwner;
static int g_reports;
static uint32_t g_stackTop, g_stackLimit; // the frame's thread
static bool g_gaveUp; // reported enough: do not arm again

struct DebugThread
{
    DWORD id;
    HANDLE handle;
};

static DebugThread g_threads[MAX_DEBUG_THREADS];
static int g_threadCount;

// The debuggee's request file, in %TEMP%, named for its process id.
static void slotFileName(DWORD pid, char *out, size_t size)
{
    char tmp[MAX_PATH] = "";
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    if (n == 0 || n > MAX_PATH)
        return;
    snprintf(out, size, "%slinuxloader-watch-%lu.txt", tmp, (unsigned long)pid);
}

// The debugger signals this once Dr0 is set on every thread; the debuggee
// waits on it, so that nothing can write the slot between the request and
// the watch being in place.
static void armedEventName(DWORD pid, char *out, size_t size)
{
    snprintf(out, size, "Local\\linuxloader-watch-armed-%lu", (unsigned long)pid);
}

bool selfDebugIsChild(void)
{
    const char *value = getenv("LL_DEBUG_CHILD");
    return value && *value && strcmp(value, "0") != 0;
}

void selfDebugNoteSlot(uint32_t savedEbp, uint32_t returnAddress, uint32_t stackTop, uint32_t stackLimit)
{
    if (!selfDebugIsChild())
        return;
    char path[MAX_PATH] = "";
    slotFileName(GetCurrentProcessId(), path, sizeof(path));
    if (!path[0])
        return;
    FILE *file = fopen(path, "wb");
    if (!file)
    {
        fprintf(stderr, "*** DEBUG WATCH (debuggee): cannot write %s\n", path);
        return;
    }
    fprintf(file, "%08lx %08lx %lu %08lx %08lx\n", (unsigned long)savedEbp, (unsigned long)returnAddress,
            (unsigned long)GetCurrentThreadId(), (unsigned long)stackTop, (unsigned long)stackLimit);
    fclose(file);
    fprintf(stderr, "*** DEBUG WATCH (debuggee): the frame's saved EBP is at %08lx, its return address at %08lx, "
                    "thread %lu\n",
            (unsigned long)savedEbp, (unsigned long)returnAddress, (unsigned long)GetCurrentThreadId());
    fflush(stderr);

    char name[96];
    armedEventName(GetCurrentProcessId(), name, sizeof(name));
    HANDLE armed = NULL;
    for (int i = 0; i < 40 && !armed; i++)
    {
        armed = OpenEventA(SYNCHRONIZE, FALSE, name);
        if (!armed)
            Sleep(5);
    }
    if (armed)
    {
        DWORD waited = WaitForSingleObject(armed, 3000);
        fprintf(stderr, "*** DEBUG WATCH (debuggee): %s; continuing\n",
                waited == WAIT_OBJECT_0 ? "the watch is armed" : "the debugger did not arm in 3s");
        fflush(stderr);
        CloseHandle(armed);
    }
}

static HANDLE threadHandle(DWORD id)
{
    for (int i = 0; i < g_threadCount; i++)
        if (g_threads[i].id == id)
            return g_threads[i].handle;
    return NULL;
}

static void rememberThread(DWORD id, HANDLE handle)
{
    if (!handle || threadHandle(id) || g_threadCount >= MAX_DEBUG_THREADS)
        return;
    g_threads[g_threadCount].id = id;
    g_threads[g_threadCount].handle = handle;
    g_threadCount++;
}

// Dr0 and Dr1 watch the two slots, write only, four bytes each.
// Dr7: local enable for Dr0 (bit 0) and Dr1 (bit 2), RW0/RW1 = 01 (write),
// LEN0/LEN1 = 11 (four bytes).
static void armThread(HANDLE handle)
{
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(handle, &ctx))
        return;
    ctx.Dr0 = g_watchCount > 0 ? g_watchAddr[0] : 0;
    ctx.Dr1 = g_watchCount > 1 ? g_watchAddr[1] : 0;
    ctx.Dr2 = g_watchCount > 2 ? g_watchAddr[2] : 0;
    ctx.Dr3 = 0;
    ctx.Dr6 = 0;
    ctx.Dr7 = 0;
    if (g_watchCount > 0)
        ctx.Dr7 |= 0x00000001u | (1u << 16) | (3u << 18);
    if (g_watchCount > 1)
        ctx.Dr7 |= 0x00000004u | (1u << 20) | (3u << 22);
    if (g_watchCount > 2)
        ctx.Dr7 |= 0x00000010u | (1u << 24) | (3u << 26);
    SetThreadContext(handle, &ctx);
}

static void armAllThreads(void)
{
    for (int i = 0; i < g_threadCount; i++)
    {
        // A running thread's context is only settable while it is suspended.
        DWORD suspended = SuspendThread(g_threads[i].handle);
        armThread(g_threads[i].handle);
        if (suspended != (DWORD)-1)
            ResumeThread(g_threads[i].handle);
    }
}

// One-shot software breakpoints: LL_BREAK=<hex>[,<hex>...]. The byte under
// each address is kept, an int3 written in its place once the code is mapped,
// and on the hit the registers and the string at ESI (the path an argument
// points at) are reported before the original byte goes back.
#define MAX_BREAKPOINTS 4
struct Breakpoint
{
    uint32_t addr;
    unsigned char original;
    bool installed;
    bool fired;
};
static Breakpoint g_breaks[MAX_BREAKPOINTS];
static int g_breakCount;

static void installBreaks(void)
{
    if (!g_childProcess)
        return;
    for (int i = 0; i < g_breakCount; i++)
    {
        Breakpoint &b = g_breaks[i];
        if (b.installed || b.fired)
            continue;
        SIZE_T got = 0;
        unsigned char original = 0;
        if (!ReadProcessMemory(g_childProcess, (LPCVOID)(uintptr_t)b.addr, &original, 1, &got) || got != 1)
            continue; // not mapped yet
        if (original == 0xcc)
            continue;
        const unsigned char int3 = 0xcc;
        if (!WriteProcessMemory(g_childProcess, (LPVOID)(uintptr_t)b.addr, &int3, 1, &got) || got != 1)
            continue;
        FlushInstructionCache(g_childProcess, (LPCVOID)(uintptr_t)b.addr, 1);
        b.original = original;
        b.installed = true;
        fprintf(stderr, "*** DEBUG WATCH: breakpoint set at %08lx (was %02x)\n", (unsigned long)b.addr, original);
        fflush(stderr);
    }
}

static void printChildStringLen(const char *label, uint32_t address, size_t limit)
{
    char text[260];
    SIZE_T got = 0;
    if (limit > sizeof(text) - 1)
        limit = sizeof(text) - 1;
    if (!address || !ReadProcessMemory(g_childProcess, (LPCVOID)(uintptr_t)address, text, limit, &got) || !got)
    {
        fprintf(stderr, "    %s at %08lx: (unreadable)\n", label, (unsigned long)address);
        return;
    }
    text[got] = 0;
    for (SIZE_T i = 0; i < got; i++)
        if (text[i] == 0)
        {
            text[i] = 0;
            break;
        }
        else if (text[i] < 0x20 || text[i] > 0x7e)
            text[i] = '.';
    fprintf(stderr, "    %s at %08lx: \"%s\"\n", label, (unsigned long)address, text);
}

static void printChildString(const char *label, uint32_t address)
{
    printChildStringLen(label, address, 259);
}

// True when the breakpoint was one of ours (and has now been put back).
static bool handleOurBreakpoint(DEBUG_EVENT &event)
{
    const uint32_t address = (uint32_t)(uintptr_t)event.u.Exception.ExceptionRecord.ExceptionAddress;
    for (int i = 0; i < g_breakCount; i++)
    {
        Breakpoint &b = g_breaks[i];
        if (!b.installed || b.addr != address)
            continue;

        HANDLE thread = threadHandle(event.dwThreadId);
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        if (thread && GetThreadContext(thread, &ctx))
        {
            fprintf(stderr, "*** DEBUG WATCH: breakpoint at %08lx hit by thread %lu: eip %08lx esp %08lx ebp %08lx\n"
                            "    eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx\n",
                    (unsigned long)address, (unsigned long)event.dwThreadId, (unsigned long)ctx.Eip,
                    (unsigned long)ctx.Esp, (unsigned long)ctx.Ebp, (unsigned long)ctx.Eax, (unsigned long)ctx.Ebx,
                    (unsigned long)ctx.Ecx, (unsigned long)ctx.Edx, (unsigned long)ctx.Esi, (unsigned long)ctx.Edi);
            printChildString("esi", (uint32_t)ctx.Esi);
            printChildString("edi", (uint32_t)ctx.Edi);
            // A return value that is a string (getenv and friends), short
            // enough not to dump a buffer of unrelated data.
            printChildStringLen("eax", (uint32_t)ctx.Eax, 96);
        }
        fflush(stderr);

        SIZE_T wrote = 0;
        WriteProcessMemory(g_childProcess, (LPVOID)(uintptr_t)b.addr, &b.original, 1, &wrote);
        FlushInstructionCache(g_childProcess, (LPCVOID)(uintptr_t)b.addr, 1);
        b.installed = false;
        b.fired = true;
        if (thread && ctx.Eip == b.addr + 1)
        {
            // Run the instruction that is under the breakpoint again.
            ctx.Eip = b.addr;
            SetThreadContext(thread, &ctx);
        }
        return true;
    }
    return false;
}

static void disarmThread(HANDLE handle)
{
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(handle, &ctx))
        return;
    ctx.Dr0 = 0;
    ctx.Dr7 = 0;
    SetThreadContext(handle, &ctx);
}

// The debuggee's request file says which address to watch; the debugger also
// reads its current value, so a report can show what it was overwritten from.
static void tryArm(void)
{
    if (g_armed || g_gaveUp || !g_childProcess)
        return;
    char path[MAX_PATH] = "";
    slotFileName(g_childPid, path, sizeof(path));
    if (!path[0])
        return;
    FILE *file = fopen(path, "rb");
    if (!file)
        return;
    unsigned long first = 0, second = 0, owner = 0, top = 0, limit = 0;
    int read = fscanf(file, "%lx %lx %lu %lx %lx", &first, &second, &owner, &top, &limit);
    fclose(file);
    if (read < 2 || (!first && !second))
        return;

    g_armed = true;
    g_watchOwner = (DWORD)owner;
    g_stackTop = (uint32_t)top;
    g_stackLimit = (uint32_t)limit;
    g_watchCount = 0;
    if (first)
        g_watchAddr[g_watchCount++] = (uint32_t)first;
    if (second)
        g_watchAddr[g_watchCount++] = (uint32_t)second;
    if (g_extraAddr)
        g_watchAddr[g_watchCount++] = g_extraAddr;
    for (int i = 0; i < g_watchCount; i++)
    {
        SIZE_T got = 0;
        g_watchValue[i] = 0;
        ReadProcessMemory(g_childProcess, (LPCVOID)(uintptr_t)g_watchAddr[i], &g_watchValue[i], 4, &got);
        fprintf(stderr, "*** DEBUG WATCH: the %s at %08lx holds %08lx (frame thread %lu)\n",
                g_watchAddr[i] == g_extraAddr ? "extra word (LL_WATCH_ADDR)" : (i == 0 ? "saved EBP" : "return address"),
                (unsigned long)g_watchAddr[i], (unsigned long)g_watchValue[i], (unsigned long)g_watchOwner);
    }
    fprintf(stderr, "*** DEBUG WATCH: arming Dr0-Dr%d (write, 4 bytes) on %d threads\n", g_watchCount - 1, g_threadCount);
    fflush(stderr);
    armAllThreads();
    if (g_armedEvent)
        SetEvent(g_armedEvent);
}

static void reportWatchHit(DWORD tid)
{
    HANDLE handle = threadHandle(tid);
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
    if (!handle || !GetThreadContext(handle, &ctx))
        return;

    for (int i = 0; i < g_watchCount; i++)
    {
        SIZE_T got = 0;
        uint32_t now = g_watchValue[i];
        ReadProcessMemory(g_childProcess, (LPCVOID)(uintptr_t)g_watchAddr[i], &now, 4, &got);
        if (now == g_watchValue[i])
            continue;
        // The extra word is a global, written all the time. What matters is a
        // stack address arriving in it: that is a frame put where an object
        // pointer belongs.
        if (g_watchAddr[i] == g_extraAddr && (now < g_stackLimit || now >= g_stackTop))
        {
            g_watchValue[i] = now;
            continue;
        }

        fprintf(stderr, "*** DEBUG WATCH: the %s at %08lx was written %08lx -> %08lx by thread %lu%s, by the "
                        "instruction ending at eip %08lx (esp %08lx ebp %08lx)\n",
                g_watchAddr[i] == g_extraAddr ? "extra word (LL_WATCH_ADDR)" : (i == 0 ? "saved EBP" : "return address"),
                (unsigned long)g_watchAddr[i], (unsigned long)g_watchValue[i], (unsigned long)now,
                (unsigned long)tid, tid == g_watchOwner ? " (the frame's own thread)" : " (ANOTHER THREAD)",
                (unsigned long)ctx.Eip, (unsigned long)ctx.Esp, (unsigned long)ctx.Ebp);
        unsigned char code[16];
        if (ReadProcessMemory(g_childProcess, (LPCVOID)(uintptr_t)(ctx.Eip - sizeof(code)), code, sizeof(code), &got))
        {
            fprintf(stderr, "    bytes before eip at %08lx:", (unsigned long)(ctx.Eip - sizeof(code)));
            for (SIZE_T k = 0; k < sizeof(code); k++)
                fprintf(stderr, "%s%02x", k % 8 ? " " : "\n      ", code[k]);
            fprintf(stderr, "\n");
        }
        g_watchValue[i] = now;
    }
    fflush(stderr);

    ctx.Dr6 = 0; // the breakpoint flag is sticky; clear it or it never reports again
    SetThreadContext(handle, &ctx);

    if (++g_reports >= MAX_WATCH_REPORTS)
    {
        fprintf(stderr, "*** DEBUG WATCH: %d writes reported; disarming\n", g_reports);
        for (int i = 0; i < g_threadCount; i++)
            disarmThread(g_threads[i].handle);
        g_armed = false;
        g_gaveUp = true;
    }
}

// LL_DEBUG_CRASH: the deaths the process cannot report itself - a fail-fast
// (0xc0000409: the C runtime's abort(), a /GS security check, which skip
// every handler) and the second chance of a fault - reported from here:
// the code, the registers and the code addresses on the thread's stack, by
// module.
static bool g_crashWatch;

static void describeChildAddress(uint32_t addr, char *out, size_t size)
{
    HMODULE modules[512];
    DWORD needed = 0;
    snprintf(out, size, addr >= 0x08000000 && addr < 0x10000000 ? "game ELF image" : "(no module: ELF library or heap)");
    if (!EnumProcessModules(g_childProcess, modules, sizeof(modules), &needed))
        return;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 512; i++)
    {
        MODULEINFO info;
        if (!GetModuleInformation(g_childProcess, modules[i], &info, sizeof(info)))
            continue;
        uintptr_t base = (uintptr_t)info.lpBaseOfDll;
        if (addr >= base && addr < base + info.SizeOfImage)
        {
            char name[MAX_PATH] = "?";
            GetModuleBaseNameA(g_childProcess, modules[i], name, sizeof(name));
            snprintf(out, size, "%s+0x%lx", name, (unsigned long)(addr - base));
            return;
        }
    }
}

static bool isChildCode(uint32_t addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQueryEx(g_childProcess, (LPCVOID)(uintptr_t)addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static void reportChildCrash(const DEBUG_EVENT &event)
{
    const EXCEPTION_RECORD &rec = event.u.Exception.ExceptionRecord;
    char where[MAX_PATH + 32];
    HANDLE thread = NULL;
    for (int i = 0; i < g_threadCount; i++)
        if (g_threads[i].id == event.dwThreadId)
            thread = g_threads[i].handle;

    describeChildAddress((uint32_t)(uintptr_t)rec.ExceptionAddress, where, sizeof(where));
    fprintf(stderr, "\n*** DEBUG CRASH: exception 0x%08lx (%s chance) at %p = %s, thread %lu",
            (unsigned long)rec.ExceptionCode, event.u.Exception.dwFirstChance ? "first" : "second", rec.ExceptionAddress,
            where, (unsigned long)event.dwThreadId);
    if (rec.ExceptionCode == 0xC0000409 && rec.NumberParameters)
        fprintf(stderr, ", fail-fast code %lu", (unsigned long)rec.ExceptionInformation[0]);
    fprintf(stderr, " ***\n");

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    if (thread && GetThreadContext(thread, &ctx))
    {
        fprintf(stderr, "  EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx\n  ESI=%08lx EDI=%08lx EBP=%08lx ESP=%08lx EIP=%08lx\n",
                ctx.Eax, ctx.Ebx, ctx.Ecx, ctx.Edx, ctx.Esi, ctx.Edi, ctx.Ebp, ctx.Esp, ctx.Eip);
        // The code addresses on the stack, newest first: the call chain.
        static uint32_t stack[2048];
        SIZE_T got = 0;
        ReadProcessMemory(g_childProcess, (LPCVOID)(uintptr_t)ctx.Esp, stack, sizeof(stack), &got);
        int shown = 0;
        for (size_t i = 0; i < got / 4 && shown < 48; i++)
        {
            if (stack[i] < 0x10000 || !isChildCode(stack[i]))
                continue;
            describeChildAddress(stack[i], where, sizeof(where));
            fprintf(stderr, "  [esp+%03zx] %08lx = %s\n", i * 4, (unsigned long)stack[i], where);
            shown++;
        }
    }
    fprintf(stderr, "*** end of debug crash report ***\n");
    fflush(stderr);
}

bool selfDebugRun(int *exitCode)
{
    const char *enabled = getenv("LL_DEBUG_WATCH");
    bool watch = enabled && *enabled && strcmp(enabled, "0") != 0;
    const char *crash = getenv("LL_DEBUG_CRASH");
    g_crashWatch = crash && *crash && strcmp(crash, "0") != 0;
    const char *breaks = getenv("LL_BREAK");
    if (!watch && !g_crashWatch && (!breaks || !*breaks))
        return false;
    if (selfDebugIsChild())
        return false;

    if (breaks && *breaks)
    {
        const char *p = breaks;
        while (*p && g_breakCount < MAX_BREAKPOINTS)
        {
            g_breaks[g_breakCount].addr = (uint32_t)strtoul(p, NULL, 16);
            if (g_breaks[g_breakCount].addr)
                g_breakCount++;
            p = strchr(p, ',');
            if (!p)
                break;
            p++;
        }
    }

    char exe[MAX_PATH] = "";
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH))
        return false;

    // This executable, with the arguments it was given.
    char command[MAX_PATH * 2];
    snprintf(command, sizeof(command), "\"%s\"", exe);
    const char *tail = GetCommandLineA();
    if (*tail == '"')
    {
        tail = strchr(tail + 1, '"');
        if (tail)
            tail++;
    }
    else
    {
        tail = strchr(tail, ' ');
    }
    if (tail && *tail)
        snprintf(command + strlen(command), sizeof(command) - strlen(command), " %s", tail);

    // One more word to watch, straight from this (the debugger's) environment.
    // It is armed together with the frame slots: arming it at process start,
    // while the loader is still mapping the game's data over it, stops the
    // process dead.
    {
        const char *extra = getenv("LL_WATCH_ADDR");
        if (extra)
            g_extraAddr = (uint32_t)strtoul(extra, NULL, 16);
    }

    SetEnvironmentVariableA("LL_DEBUG_CHILD", "1");

    STARTUPINFOA startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child;
    memset(&child, 0, sizeof(child));
    fprintf(stderr, "*** DEBUG WATCH: starting %s under this process's debugger\n", command);
    fflush(stderr);
    if (!CreateProcessA(NULL, command, NULL, NULL, TRUE, DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &startup, &child))
    {
        fprintf(stderr, "*** DEBUG WATCH: CreateProcess failed (%lu); running normally\n", (unsigned long)GetLastError());
        return false;
    }
    g_childPid = child.dwProcessId;
    g_childProcess = child.hProcess;
    // Created now, signalled once the watch is set: the debuggee opens it by
    // name and waits, so the slot cannot be written in between.
    {
        char name[96];
        armedEventName(g_childPid, name, sizeof(name));
        g_armedEvent = CreateEventA(NULL, TRUE, FALSE, name);
    }

    // If this process is killed, the debuggee should not be left behind.
    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
        memset(&limits, 0, sizeof(limits));
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job, child.hProcess))
        {
            CloseHandle(job);
            job = NULL;
        }
    }

    int code = 1;
    bool running = true;
    while (running)
    {
        installBreaks();
        DEBUG_EVENT event;
        if (!WaitForDebugEvent(&event, g_armed || g_breakCount ? 1000 : 30))
        {
            tryArm();
            continue;
        }
        installBreaks();

        DWORD status = DBG_CONTINUE;
        switch (event.dwDebugEventCode)
        {
        case CREATE_PROCESS_DEBUG_EVENT:
            if (event.u.CreateProcessInfo.hFile)
                CloseHandle(event.u.CreateProcessInfo.hFile);
            rememberThread(event.dwThreadId, event.u.CreateProcessInfo.hThread);
            break;

        case CREATE_THREAD_DEBUG_EVENT:
            rememberThread(event.dwThreadId, event.u.CreateThread.hThread);
            if (g_armed)
            {
                armThread(event.u.CreateThread.hThread);
                fprintf(stderr, "*** DEBUG WATCH: new thread %lu armed\n", (unsigned long)event.dwThreadId);
                fflush(stderr);
            }
            break;

        case EXIT_THREAD_DEBUG_EVENT:
            break;

        case LOAD_DLL_DEBUG_EVENT:
            if (event.u.LoadDll.hFile)
                CloseHandle(event.u.LoadDll.hFile);
            break;

        case EXCEPTION_DEBUG_EVENT:
        {
            const DWORD exceptionCode = event.u.Exception.ExceptionRecord.ExceptionCode;
            if (exceptionCode == EXCEPTION_BREAKPOINT)
            {
                if (!handleOurBreakpoint(event))
                {
                    // The debuggee's own int3 handling is its business.
                    fprintf(stderr, "*** DEBUG WATCH: breakpoint at %p in thread %lu\n",
                            event.u.Exception.ExceptionRecord.ExceptionAddress, (unsigned long)event.dwThreadId);
                    fflush(stderr);
                }
            }
            else if (exceptionCode == EXCEPTION_SINGLE_STEP && g_armed)
            {
                reportWatchHit(event.dwThreadId);
            }
            else
            {
                // Faults belong to the debuggee: its own vectored handler and
                // crash report are what name them, so pass the exception on.
                // Those it cannot report (see LL_DEBUG_CRASH) are reported here.
                if (g_crashWatch && (exceptionCode == 0xC0000409 || !event.u.Exception.dwFirstChance))
                    reportChildCrash(event);
                status = DBG_EXCEPTION_NOT_HANDLED;
            }
            break;
        }

        case EXIT_PROCESS_DEBUG_EVENT:
            code = (int)event.u.ExitProcess.dwExitCode;
            running = false;
            break;

        case OUTPUT_DEBUG_STRING_EVENT:
            break;

        default:
            break;
        }

        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
        if (!g_armed)
            tryArm();
    }

    if (g_armedEvent)
        CloseHandle(g_armedEvent);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    if (job)
    {
        // Closing the job (and this process with it) takes the debuggee down.
        CloseHandle(job);
    }
    char path[MAX_PATH] = "";
    slotFileName(g_childPid, path, sizeof(path));
    if (path[0])
        DeleteFileA(path);

    if (exitCode)
        *exitCode = code;
    return true;
}

#endif /* _WIN32 */
