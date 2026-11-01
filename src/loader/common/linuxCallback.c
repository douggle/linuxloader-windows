// Stack-realigning thunks for Linux callbacks called from Windows code (see
// linuxCallback.h). Cruis'n Blast's sdl2-compat registers EventFilter3to2
// with SDL3 as an event watch; SDL3 called it on a stack 8 bytes off and its
// first movaps faulted, a few seconds after the game window opened.

#if defined(_WIN32) || defined(__MINGW32__)

#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "linuxCallback.h"
#include "../log/log.h"

// 16 argument words: the longest GL entry points take 15 (a vertex, its
// normal, colour and texture coordinate). 2048 thunks: a game's GL loader
// looks up a thousand extension functions (see glHooks.cpp).
#define THUNK_SIZE 256 // 130 bytes of code at 16 words
#define THUNK_COUNT 2048
#define THUNK_ARGS 16

typedef struct
{
    void *target;
    unsigned char *thunk;
} CallbackThunk;

static CallbackThunk thunks[THUNK_COUNT];
static int thunkCount;
static unsigned char *thunkMemory;
static SRWLOCK thunkLock = SRWLOCK_INIT;

// push ebp; mov ebp,esp; and esp,-16; sub esp,4*THUNK_ARGS; copy the caller's
// argument words; mov eax,target; call eax; leave; ret. cdecl: the caller
// pops its own arguments, and eax:edx (or st0) carry the result back.
static void writeThunk(unsigned char *p, void *target)
{
    *p++ = 0x55;
    *p++ = 0x89, *p++ = 0xE5;
    *p++ = 0x83, *p++ = 0xE4, *p++ = 0xF0;
    *p++ = 0x83, *p++ = 0xEC, *p++ = 4 * THUNK_ARGS;
    for (int i = 0; i < THUNK_ARGS; i++)
    {
        *p++ = 0x8B, *p++ = 0x45, *p++ = (unsigned char)(8 + 4 * i);       // mov eax,[ebp+8+4i]
        *p++ = 0x89, *p++ = 0x44, *p++ = 0x24, *p++ = (unsigned char)(4 * i); // mov [esp+4i],eax
    }
    *p++ = 0xB8;
    memcpy(p, &target, 4);
    p += 4;
    *p++ = 0xFF, *p++ = 0xD0;
    *p++ = 0xC9;
    *p++ = 0xC3;
}

void *linuxCallback(void *fn)
{
    void *result = NULL;
    if (!fn)
        return NULL;

    AcquireSRWLockExclusive(&thunkLock);
    for (int i = 0; i < thunkCount; i++)
        if (thunks[i].target == fn || thunks[i].thunk == fn)
        {
            result = thunks[i].thunk;
            break;
        }
    if (!result)
    {
        if (!thunkMemory)
            thunkMemory = (unsigned char *)VirtualAlloc(NULL, THUNK_SIZE * THUNK_COUNT, MEM_RESERVE | MEM_COMMIT,
                                                        PAGE_EXECUTE_READWRITE);
        if (thunkMemory && thunkCount < THUNK_COUNT)
        {
            unsigned char *thunk = thunkMemory + THUNK_SIZE * thunkCount;
            writeThunk(thunk, fn);
            FlushInstructionCache(GetCurrentProcess(), thunk, THUNK_SIZE);
            thunks[thunkCount].target = fn;
            thunks[thunkCount].thunk = thunk;
            thunkCount++;
            result = thunk;
        }
    }
    ReleaseSRWLockExclusive(&thunkLock);

    if (!result)
    {
        log_error("Linux callback %p: no thunk left, called unaligned", fn);
        return fn;
    }
    return result;
}

void *linuxCallbackTarget(void *ptr)
{
    void *result = ptr;
    AcquireSRWLockShared(&thunkLock);
    for (int i = 0; i < thunkCount; i++)
        if (thunks[i].thunk == ptr)
        {
            result = thunks[i].target;
            break;
        }
    ReleaseSRWLockShared(&thunkLock);
    return result;
}

#endif
