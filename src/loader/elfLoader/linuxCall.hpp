#pragma once

// Calls from the loader's (Windows) code into Linux code: the i386 Linux ABI
// has the stack 16-byte aligned at every call, and code built for it keeps
// SSE values there with aligned stores (movaps), while Windows only keeps
// 4-byte alignment. sdl2-compat's SDL2Compat_ApplyQuirks, run as a library
// constructor, faulted on its first such store. These realign the stack for
// the call and put it back after. C and C++ alike (rawthrills/rtSdl.c).

#include <stdint.h>

// fn(arg), on a 16-byte aligned stack.
static inline void *callLinuxAligned(void *(*fn)(void *), void *arg)
{
    void *ret;
    __asm__ volatile("mov %%esp, %%esi\n\t"
                     "and $-16, %%esp\n\t"
                     "sub $12, %%esp\n\t"
                     "push %2\n\t"
                     "call *%1\n\t"
                     "mov %%esi, %%esp\n\t"
                     : "=a"(ret)
                     : "b"(fn), "D"(arg)
                     : "esi", "ecx", "edx", "memory", "cc");
    return ret;
}

// fn(), on a 16-byte aligned stack.
static inline void callLinuxAligned0(void (*fn)(void))
{
    __asm__ volatile("mov %%esp, %%esi\n\t"
                     "and $-16, %%esp\n\t"
                     "call *%0\n\t"
                     "mov %%esi, %%esp\n\t"
                     :
                     : "b"(fn)
                     : "esi", "eax", "ecx", "edx", "memory", "cc");
}
