#if defined(_WIN32) || defined(__MINGW32__)
#include "memoryManager.hpp"
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <wchar.h>

#include "../log/log.h"
#include <stdio.h>

void *MemoryManager::AllocateExecutable(size_t size, void *requestedAddr)
{
    void* ptr = nullptr;
    if (requestedAddr) {
        ptr = VirtualAlloc(requestedAddr, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
    if (!ptr) {
        ptr = VirtualAlloc(requestedAddr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!ptr && requestedAddr != nullptr)
    {
        uint8_t *scan = static_cast<uint8_t *>(requestedAddr);
        uint8_t *end = scan + size;
        bool canUse = true;

        while (scan < end)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(scan, &mbi, sizeof(mbi)))
            {
                canUse = false;
                break;
            }

            SIZE_T regionSize = mbi.RegionSize;
            uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + regionSize;
            if (regionEnd > reinterpret_cast<uintptr_t>(end))
                regionSize = end - scan;

            if (mbi.State == MEM_COMMIT)
            {
                DWORD oldProtect;
                if (!VirtualProtect(scan, regionSize, PAGE_EXECUTE_READWRITE, &oldProtect))
                {
                    log_error("AllocateExecutable: VirtualProtect failed at %p (%zu bytes). Error: %lu",
                              scan, regionSize, GetLastError());
                    canUse = false;
                    break;
                }
            }
            else if (mbi.State == MEM_FREE)
            {
                if (!VirtualAlloc(scan, regionSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                {
                    log_error("AllocateExecutable: VirtualAlloc(FREE) failed at %p (%zu bytes). Error: %lu",
                              scan, regionSize, GetLastError());
                    canUse = false;
                    break;
                }
            }
            else if (mbi.State == MEM_RESERVE)
            {
                if (!VirtualAlloc(scan, regionSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                {
                    log_error("AllocateExecutable: VirtualAlloc(RESERVE) failed at %p (%zu bytes). Error: %lu",
                              scan, regionSize, GetLastError());
                    canUse = false;
                    break;
                }
            }

            scan = reinterpret_cast<uint8_t *>(mbi.BaseAddress) + mbi.RegionSize;
        }

        if (canUse)
        {
            log_warn("AllocateExecutable: Using taken-over memory at %p (%zu bytes)", requestedAddr, size);
            ptr = requestedAddr;
        }
    }
    if (!ptr && requestedAddr != nullptr)
    {
        DWORD err = GetLastError();
        log_error("VirtualAlloc failed for address %p, size %zu. Error: %lu", requestedAddr, size, err);
        
        uint8_t* scan = static_cast<uint8_t*>(requestedAddr);
        uint8_t* end = scan + size;
        
        log_error("Scanning region %p to %p for conflicts...", scan, end);
        while (scan < end)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(scan, &mbi, sizeof(mbi)))
            {
                if (mbi.State != MEM_FREE)
                {
                    log_error("Conflict found at %p - RegionSize: 0x%zx, State: 0x%lx, Type: 0x%lx",
                              mbi.BaseAddress, mbi.RegionSize, mbi.State, mbi.Type);
                    break;
                }
                scan += mbi.RegionSize;
            }
            else
            {
                log_error("VirtualQuery failed at %p", scan);
                break;
            }
        }
    }
    return ptr;
}

#include <psapi.h>

void* MemoryManager::ReserveAddressSpace(size_t size, void* requestedAddr)
{
    void* result = VirtualAlloc(requestedAddr, size, MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (result) {
        log_info("MemoryManager: Pre-reserved address space: %p - %p (%zu bytes)", result, (char *)result + size, size);
        return result;
    }

    if (!requestedAddr)
        return nullptr;

    log_warn("MemoryManager: Pre-reserve failed at %p (%zu bytes). Taking over existing committed memory...",
             requestedAddr, size);

    uintptr_t scanAddr = reinterpret_cast<uintptr_t>(requestedAddr);
    uintptr_t alignedMax = scanAddr + size;
    bool allOk = true;

    while (scanAddr < alignedMax)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void *>(scanAddr), &mbi, sizeof(mbi)))
            break;

        if (mbi.State == MEM_FREE)
        {
            SIZE_T freeSize = mbi.RegionSize;
            uintptr_t freeEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + freeSize;
            if (freeEnd > alignedMax)
                freeSize = alignedMax - scanAddr;

            void *allocated = VirtualAlloc(reinterpret_cast<void *>(scanAddr), freeSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (allocated)
            {
                log_info("  Claimed free region at %p (%zu bytes)", allocated, freeSize);
            }
            else
            {
                log_error("  Failed to claim free region at %p (%zu bytes). Error: %lu", reinterpret_cast<void *>(scanAddr), freeSize, GetLastError());
                allOk = false;
            }
        }
        else if (mbi.State == MEM_COMMIT)
        {
            char moduleName[MAX_PATH] = {0};
            if (mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED)
                GetMappedFileNameA(GetCurrentProcess(), reinterpret_cast<void *>(scanAddr), moduleName, MAX_PATH);

            log_warn("  Taking over committed region at %p (Size=0x%zx, Type=0x%lx) %s%s", mbi.BaseAddress, mbi.RegionSize, mbi.Type,
                     moduleName[0] ? "Module: " : "", moduleName[0] ? moduleName : "");

            DWORD oldProtect;
            SIZE_T protectSize = mbi.RegionSize;
            uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + protectSize;
            if (regionEnd > alignedMax)
                protectSize = alignedMax - scanAddr;

            if (!VirtualProtect(reinterpret_cast<void *>(scanAddr), protectSize, PAGE_EXECUTE_READWRITE, &oldProtect))
            {
                log_error("  VirtualProtect failed at %p (%zu bytes). Error: %lu", reinterpret_cast<void *>(scanAddr), protectSize, GetLastError());
                allOk = false;
            }
        }
        else if (mbi.State == MEM_RESERVE)
        {
            SIZE_T commitSize = mbi.RegionSize;
            uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + commitSize;
            if (regionEnd > alignedMax)
                commitSize = alignedMax - scanAddr;

            void *committed = VirtualAlloc(reinterpret_cast<void *>(scanAddr), commitSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (committed)
            {
                log_info("  Committed reserved region at %p (%zu bytes)", committed, commitSize);
            }
            else
            {
                log_error("  Failed to commit reserved region at %p (%zu bytes). Error: %lu", reinterpret_cast<void *>(scanAddr), commitSize, GetLastError());
                allOk = false;
            }
        }

        scanAddr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    return allOk ? requestedAddr : nullptr;
}

bool MemoryManager::CommitSegment(uintptr_t vaddr, uintptr_t memsz)
{
    uintptr_t alignedVAddr = vaddr & ~0xFFF;
    uintptr_t vEnd = vaddr + memsz;
    uintptr_t alignedEnd = (vEnd + 0xFFF) & ~0xFFF;
    uintptr_t alignedSize = alignedEnd - alignedVAddr;

    LPVOID result = VirtualAlloc((LPVOID)alignedVAddr, alignedSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!result) {
        log_error("MemoryManager: VirtualAlloc MEM_COMMIT failed! VAddr: %p, Size: 0x%zx, Error: %lu",
                  (void*)alignedVAddr, alignedSize, GetLastError());
    }
    return result != NULL;
}

void *MemoryManager::customMemmove(void *dest, const void *src, size_t n)
{
    return memmove(dest, src, n);
}

wchar_t *MemoryManager::customWmemmove(wchar_t *dest, const wchar_t *src, size_t n)
{
    return wmemmove(dest, src, n);
}

int MemoryManager::customStrcoll(const char *s1, const char *s2)
{
    return strcoll(s1, s2);
}

size_t MemoryManager::customStrxfrm(char *dest, const char *src, size_t n)
{
    return strxfrm(dest, src, n);
}

// The game's blocks: a free() of something it never allocated (a pointer of
// the CRT's, a buffer of ours, the stack) is reported instead of taking the
// heap's free list down with it — one such free made msvcrt hand out a block
// inside the caller's stack on Cruis'n Blast. Each block also carries a guard
// word before and after its bytes, so a write past either end is caught and
// reported with the block it broke. A table of fixed size, reserved once with
// VirtualAlloc, as this is called from the game's own threads and cannot use
// the heap it manages. Cruis'n Blast keeps well over 32768 blocks alive (FMOD,
// PhysX): a table that size filled up, and every block after that was
// "unknown" on its free and leaked.
namespace
{
constexpr size_t ALLOC_SLOTS = 1 << 20;
// Live records plus tombstones past this many slots: the table is rebuilt
// without its tombstones, so probes stay short and still end on a free slot.
constexpr size_t ALLOC_REBUILD_AT = ALLOC_SLOTS / 4 * 3;
constexpr size_t GUARD = 16;

// state: 0 never used (the probe's end), 1 live, 2 freed (a tombstone: the
// probe goes on past it, or a record inserted after it becomes unreachable).
struct AllocRecord
{
    void *ptr;  // the block handed to the game
    void *base; // the allocation itself, guard word first
    size_t size;
    unsigned char state;
};

AllocRecord *g_allocs;
AllocRecord *g_allocsScratch; // the live records, while the table is rebuilt
size_t g_allocsLive;
size_t g_allocsTombs;
CRITICAL_SECTION g_allocsLock;
volatile LONG g_allocsLockReady;

void lockAllocs()
{
    if (!g_allocsLockReady)
    {
        // Two threads may race here; both initialise the same critical section.
        if (InterlockedExchange(&g_allocsLockReady, 1) == 0)
        {
            InitializeCriticalSection(&g_allocsLock);
            // Committed pages read as zero: every slot starts never used.
            g_allocs = (AllocRecord *)VirtualAlloc(nullptr, ALLOC_SLOTS * sizeof(AllocRecord), MEM_RESERVE | MEM_COMMIT,
                                                   PAGE_READWRITE);
            g_allocsScratch = (AllocRecord *)VirtualAlloc(nullptr, ALLOC_SLOTS * sizeof(AllocRecord),
                                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!g_allocs || !g_allocsScratch)
                log_fatal("MemoryManager: cannot reserve the allocation table");
        }
        else
            while (g_allocsLockReady != 2)
                Sleep(0);
        InterlockedExchange(&g_allocsLockReady, 2);
    }
    while (g_allocsLockReady != 2)
        Sleep(0);
    EnterCriticalSection(&g_allocsLock);
}

void unlockAllocs()
{
    LeaveCriticalSection(&g_allocsLock);
}

size_t allocSlot(void *ptr)
{
    return (((uintptr_t)ptr >> 4) * 2654435761u) & (ALLOC_SLOTS - 1);
}

unsigned char guardByte(size_t size, size_t at)
{
    return (unsigned char)(0xA5 ^ (size * 31) >> (at & 7));
}

void writeGuards(AllocRecord &r)
{
    unsigned char *base = (unsigned char *)r.base;
    for (size_t i = 0; i < GUARD; i++)
    {
        base[i] = guardByte(r.size, i);
        base[GUARD + r.size + i] = guardByte(r.size, i + GUARD);
    }
}

// 1 if neither guard word of a live record was written over.
int guardsIntact(const AllocRecord &r)
{
    const unsigned char *base = (const unsigned char *)r.base;
    for (size_t i = 0; i < GUARD; i++)
        if (base[i] != guardByte(r.size, i) || base[GUARD + r.size + i] != guardByte(r.size, i + GUARD))
            return 0;
    return 1;
}

// Logs (once per broken block) that a live block's guard word was overwritten.
void reportOverrun(size_t slot, const char *when, void *from)
{
    const AllocRecord &r = g_allocs[slot];
    const unsigned char *base = (const unsigned char *)r.base;
    int front = 0;
    for (size_t i = 0; i < GUARD; i++)
        if (base[i] != guardByte(r.size, i))
            front = 1;
    log_warn("MemoryManager: heap overrun of block %p (%zu bytes), broken at its %s on %s from %p", r.ptr, r.size,
             front ? "start" : "end", when, from);
    // Once only: mark the record checked by rewriting the guards, so the
    // rest of the run is not flooded with the same report.
    AllocRecord *mutableRecord = &g_allocs[slot];
    writeGuards(*mutableRecord);
}

void checkAllGuards(void *from);

// Called with the lock held: puts the live records back without the
// tombstones between them.
void rebuildAllocs()
{
    size_t live = 0;
    for (size_t i = 0; i < ALLOC_SLOTS; i++)
        if (g_allocs[i].state == 1)
            g_allocsScratch[live++] = g_allocs[i];
    memset(g_allocs, 0, ALLOC_SLOTS * sizeof(AllocRecord));
    for (size_t n = 0; n < live; n++)
    {
        size_t i = allocSlot(g_allocsScratch[n].ptr);
        while (g_allocs[i].state != 0)
            i = (i + 1) & (ALLOC_SLOTS - 1);
        g_allocs[i] = g_allocsScratch[n];
    }
    g_allocsLive = live;
    g_allocsTombs = 0;
}

size_t noteAlloc(void *base, size_t size)
{
    if (!base)
        return (size_t)-1;
    AllocRecord record = {(void *)((unsigned char *)base + GUARD), base, size, 1};
    lockAllocs();
    if (g_allocsLive + g_allocsTombs >= ALLOC_REBUILD_AT && g_allocsTombs > 0)
        rebuildAllocs();
    size_t slot = (size_t)-1;
    if (g_allocsLive < ALLOC_REBUILD_AT)
    {
        size_t i = allocSlot(record.ptr);
        for (size_t n = 0; n < ALLOC_SLOTS; n++, i = (i + 1) & (ALLOC_SLOTS - 1))
            if (g_allocs[i].state != 1)
            {
                if (g_allocs[i].state == 2)
                    g_allocsTombs--;
                g_allocs[i] = record;
                g_allocsLive++;
                slot = i;
                break;
            }
    }
    unlockAllocs();
    if (slot == (size_t)-1)
    {
        // Not recorded: its free will be reported as unknown, and leak.
        static volatile LONG reported;
        if (InterlockedExchange(&reported, 1) == 0)
            log_error("MemoryManager: allocation table full (%zu live blocks)", g_allocsLive);
    }

    // A write past a block that is still in use is only seen on its free: the
    // sweep catches the rest within a few thousand allocations of the write.
    // Rarer than it was, as the sweep reads every slot of a larger table.
    static volatile LONG allocations;
    if ((InterlockedIncrement(&allocations) & 0xFFFF) == 0)
        checkAllGuards((void *)__builtin_return_address(0));
    return slot;
}

// Takes a live record out of the table, checking its guards on the way (when
// names the call, for the report): the allocation's base, or nullptr if ptr
// was not one of the game's blocks. All under the lock, as a rebuild on
// another thread moves the records.
void *takeAlloc(void *ptr, const char *when, void *from)
{
    if (!ptr)
        return nullptr;
    lockAllocs();
    size_t i = allocSlot(ptr);
    for (size_t n = 0; n < ALLOC_SLOTS; n++, i = (i + 1) & (ALLOC_SLOTS - 1))
    {
        if (g_allocs[i].state == 0)
            break;
        if (g_allocs[i].state == 1 && g_allocs[i].ptr == ptr)
        {
            if (!guardsIntact(g_allocs[i]))
                reportOverrun(i, when, from);
            g_allocs[i].state = 2;
            g_allocsLive--;
            g_allocsTombs++;
            void *base = g_allocs[i].base;
            unlockAllocs();
            return base;
        }
    }
    unlockAllocs();
    return nullptr;
}

// Catches a write past a live block that is still in use, between frees.
void checkAllGuards(void *from)
{
    lockAllocs();
    for (size_t i = 0; i < ALLOC_SLOTS; i++)
        if (g_allocs[i].state == 1 && !guardsIntact(g_allocs[i]))
            reportOverrun(i, "a periodic check", from);
    unlockAllocs();
}
} // namespace

// A block of the game's heap: size usable bytes, a guard word either side.
static void *guardedAlloc(size_t size, size_t alignment)
{
    if (alignment < 16)
        alignment = 16;
    unsigned char *base = (unsigned char *)_aligned_malloc(size + 2 * GUARD, alignment);
    if (!base)
        return nullptr;
    AllocRecord record = {(void *)(base + GUARD), base, size, 1};
    writeGuards(record);
    noteAlloc(base, size);
    return base + GUARD;
}

// The write watchpoint in mainW.cpp: watches the saved EBP and return address
// of the frame that called this allocator (the game's growable-buffer
// helper), so the instruction that overwrites them is named.
extern "C" void watchArmGameCall(uint32_t caller, uint32_t esp);

void *MemoryManager::customMalloc(size_t size)
{
    // Taken before anything else: the caller's frame lies above it.
    uint32_t espHere;
    __asm__ volatile("mov %%esp, %0" : "=r"(espHere));

    void *ptr = guardedAlloc(size, 16);

    // A block of the CRT's heap cannot lie in the caller's own stack: when it
    // does, the heap's free list was corrupted by a free of a pointer that
    // never came from it, and the caller will write its frame away.
    uint32_t stackTop = __readfsdword(0x04);
    uint32_t stackLimit = __readfsdword(0x08);
    if ((uintptr_t)ptr >= stackLimit && (uintptr_t)ptr < stackTop)
        log_error("MemoryManager: malloc(%zu) returned %p, inside the caller's stack [%08lx, %08lx), called from %p", size,
                  ptr, (unsigned long)stackLimit, (unsigned long)stackTop, __builtin_return_address(0));

    // Armed last, so the watch starts at the caller's epilogue rather than
    // slowing down the allocation itself.
    watchArmGameCall((uint32_t)(uintptr_t)__builtin_return_address(0), espHere);

    return ptr;
}

struct mallinfo MemoryManager::customMallinfo(void)
{
    struct mallinfo Info = {};

    HANDLE Heap = GetProcessHeap();
    if (!Heap)
        return Info;

    HeapLock(Heap);

    PROCESS_HEAP_ENTRY Entry = {};
    while (HeapWalk(Heap, &Entry))
    {
        if (Entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)
        {
            Info.uordblks += (int)Entry.cbData;
        }
        else
        {
            Info.fordblks += (int)Entry.cbData;
        }
    }

    HeapUnlock(Heap);

    Info.arena = Info.uordblks + Info.fordblks;
    return Info;
}

void MemoryManager::customFree(void *ptr)
{
    if (!ptr)
        return;
    void *base = takeAlloc(ptr, "free", __builtin_return_address(0));
    if (!base)
    {
        // Not one of the game's: freeing it would corrupt the heap's lists.
        log_warn("MemoryManager: free of unknown pointer %p (called from %p)", ptr, __builtin_return_address(0));
        return;
    }
    _aligned_free(base);
}

void *MemoryManager::customCalloc(size_t nmemb, size_t size)
{
    if (nmemb != 0 && size > SIZE_MAX / nmemb)
    {
        return NULL;
    }
    size_t total = nmemb * size;
    void *ptr = guardedAlloc(total, 16);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void *MemoryManager::customRealloc(void *ptr, size_t size)
{
    if (!ptr)
        return customMalloc(size);
    void *base = takeAlloc(ptr, "realloc", __builtin_return_address(0));
    if (!base)
    {
        log_warn("MemoryManager: realloc of unknown pointer %p (called from %p)", ptr, __builtin_return_address(0));
        return customMalloc(size);
    }
    unsigned char *moved = (unsigned char *)_aligned_realloc(base, size + 2 * GUARD, 16);
    if (!moved)
        return nullptr;
    AllocRecord record = {(void *)(moved + GUARD), moved, size, 1};
    writeGuards(record);
    noteAlloc(moved, size);
    return moved + GUARD;
}

void *MemoryManager::customMemalign(size_t alignment, size_t size)
{
    return guardedAlloc(size, alignment);
}

int MemoryManager::customPosixMemalign(void **ptr, size_t alignment, size_t size)
{
    if (alignment % sizeof(void*) != 0 || (alignment & (alignment - 1)) != 0)
    {
        return EINVAL;
    }

    *ptr = guardedAlloc(size, alignment);
    return *ptr ? 0 : ENOMEM;
}

char *MemoryManager::customStrndup(const char *src, size_t size)
{
    size_t len = strnlen(src, size);
    len = len < size ? len : size;
    char *dst = (char *)guardedAlloc(len + 1, 16);
    if (!dst)
        return NULL;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

char *MemoryManager::customStrdup(const char *s)
{
    if (!s)
        return nullptr;
    size_t len = strlen(s) + 1;
    char *ptr = (char *)guardedAlloc(len, 16);
    if (ptr)
        memcpy(ptr, s, len);
    return ptr;
}

#endif