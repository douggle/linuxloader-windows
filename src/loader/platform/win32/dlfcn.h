#ifndef LL_WIN32_DLFCN_H
#define LL_WIN32_DLFCN_H

// <dlfcn.h> for MinGW, over the loader's linkage layer: dlopen() of a Linux
// .so reaches SymbolResolver (elfLoader/symbolResolver.cpp), which maps it to
// the Windows DLL the loader ships, and dlsym() searches the game's image, the
// libraries it loaded, and the loader's own bridges - the same order the
// Linux loader would.

#include <stddef.h>

#include "link.h"

#ifdef __cplusplus
extern "C"
{
#endif

// The search scope of dlsym(): the whole chain (glibc's RTLD_DEFAULT), and
// everything searched after the caller (RTLD_NEXT).  Both are the chain here:
// the loader has no libc to look past.
#define RTLD_LAZY 0x00001
#define RTLD_NOW 0x00002
#define RTLD_BINDING_MASK 0x3
#define RTLD_NOLOAD 0x00004
#define RTLD_DEEPBIND 0x00008
#define RTLD_GLOBAL 0x00100
#define RTLD_LOCAL 0
#define RTLD_NODELETE 0x01000
#define RTLD_NEXT ((void *)-1L)
#define RTLD_DEFAULT ((void *)0)

    typedef struct
    {
        const char *dli_fname;  // the path of the object holding the address
        void *dli_fbase;        // the address it was loaded at
        const char *dli_sname;  // the name of the symbol, when one is known
        void *dli_saddr;        // and its address
    } Dl_info;

    // dlinfo(): what the caller wants to know about a handle.
#define RTLD_DI_LINKMAP 2
#define RTLD_DL_SYMENT 1
#define RTLD_DL_LINKMAP 2
#define RTLD_DL_PHDR 3

    void *dlopen(const char *file, int mode);
    int dlclose(void *handle);
    void *dlsym(void *handle, const char *name);
    void *dlvsym(void *handle, const char *name, const char *version);
    char *dlerror(void);
    int dladdr(const void *addr, Dl_info *info);
    // The additional information glibc's dladdr1() gives: the symbol's own
    // entry out of the image's symbol table.
    int dladdr1(void *addr, Dl_info *info, void **extra_info, int flags);
    int dlinfo(void *handle, int request, void *info);

#ifdef __cplusplus
}
#endif

#endif // LL_WIN32_DLFCN_H
