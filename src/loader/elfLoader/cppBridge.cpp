#if defined(_WIN32) || defined(__MINGW32__)
#include "cppBridge.hpp"
#include "memoryManager.hpp"
#include "symbolResolver.hpp"
#include "../log/log.h"
#include <cstdio>

#define MAP(name, func) SymbolResolver::GetInstance().RegisterVTable(name, reinterpret_cast<void *>(func))

namespace CppBridge
{

    void initBridges()
    {
        log_info("Initializing C++ Bridges...");

        MAP("_ZdlPv", bridgeOperatorDelete);
        MAP("_Znwj", bridgeOperatorNew);
        MAP("_Znaj", bridgeOperatorNewArray);
        MAP("_ZdaPv", bridgeOperatorDelete); 
    }

    // The game's C++ and C allocation must be one allocator: a block it got
    // from malloc() and releases with delete, or the other way round, is a
    // free of a block the other side does not own (Cruis'n Blast frees malloc'd
    // objects with operator delete, which took msvcrt's heap's free list down
    // with it, and msvcrt then handed malloc a block inside the caller's
    // stack).
    void *bridgeOperatorNew(size_t size)
    {
        log_trace("Intercepted operator new, size: %zu", size);
        return MemoryManager::customMalloc(size);
    }

    void *bridgeOperatorNewArray(size_t size)
    {
        log_trace("Intercepted operator new[], size: %zu", size);
        return MemoryManager::customMalloc(size);
    }

    void bridgeOperatorDelete(void *ptr)
    {
        log_trace("Intercepted operator delete, ptr: %p", ptr);
        MemoryManager::customFree(ptr);
    }

} // namespace CppBridge

#endif