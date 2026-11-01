// The dynamic linker for the arcade support: dlopen(), dlsym(), dlvsym(),
// dladdr() and getauxval(), over the loader's linkage layer.  The Linux code
// asks the linker about the image it runs in and about the libraries it has
// loaded; here the game's image is ElfLoader's, the libraries are the ones
// SymbolResolver mapped to Windows DLLs, and the answers are its own view of
// them (elfImages.cpp's, through SymbolResolver).

#define _CRT_SECURE_NO_WARNINGS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dlfcn.h"
#include "sys/auxv.h"

#include "loader/elfLoader/elfLoader.hpp"
#include "loader/elfLoader/symbolResolver.hpp"
#include "loader/log/log.h"
#include "loader/common/sdl3Exports.h"

// The dlerror() text of the last failed call.
static std::string g_lastError;

// The Windows modules a Linux symbol may be the one of: the loader hooks
// these by their Linux names (setsockopt, XOpenDisplay, glXGetProcAddress).
static const char *const kSystemModules[] = {
    "kernel32.dll", "user32.dll",  "gdi32.dll",  "advapi32.dll", "winmm.dll",     "ole32.dll",     "ws2_32.dll",
    "opengl32.dll", "psapi.dll",   "version.dll", "shell32.dll",  "iphlpapi.dll", "setupapi.dll",  "imm32.dll",
    "hid.dll",      "winhttp.dll", "crypt32.dll", "msvcrt.dll",   "ucrtbase.dll", "winpthread-1.dll",
};

static const size_t kSystemModuleCount = sizeof(kSystemModules) / sizeof(kSystemModules[0]);

// The program's program headers, as the auxiliary vector reports them: made
// from the image ElfLoader mapped (rtFrame.c asks for them).
static std::vector<Elf32_Phdr> g_phdrs;
static bool g_phdrsBuilt;

// The image's program headers, read out of the file it was loaded from (the
// loader keeps them there: ElfLoader maps the segments, not the header).
static void buildProgramHeaders()
{
    ElfLoader *loader = SymbolResolver::GetInstance().GetMainLoader();
    FILE *file;
    Elf32_Ehdr header;

    g_phdrsBuilt = true;
    if (!loader)
        return;
    file = fopen(loader->GetPath().c_str(), "rb");
    if (!file)
        return;
    if (fread(&header, sizeof(header), 1, file) != 1 || memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_phentsize != sizeof(Elf32_Phdr) || header.e_phnum == 0)
    {
        fclose(file);
        return;
    }
    g_phdrs.resize(header.e_phnum);
    if (fseek(file, (long)header.e_phoff, SEEK_SET) != 0 ||
        fread(g_phdrs.data(), sizeof(Elf32_Phdr), header.e_phnum, file) != header.e_phnum)
        g_phdrs.clear();
    fclose(file);
}

unsigned long getauxval(unsigned long type)
{
    switch (type)
    {
    case AT_PHDR:
        if (!g_phdrsBuilt)
            buildProgramHeaders();
        return g_phdrs.empty() ? 0 : (unsigned long)(uintptr_t)g_phdrs.data();
    case AT_PHNUM:
        if (!g_phdrsBuilt)
            buildProgramHeaders();
        return (unsigned long)g_phdrs.size();
    case AT_PHENT:
        return sizeof(Elf32_Phdr);
    case AT_PAGESZ:
    {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return (unsigned long)info.dwPageSize;
    }
    case AT_ENTRY:
    case AT_BASE:
    case AT_UID:
    case AT_EUID:
    case AT_GID:
    case AT_EGID:
    case AT_CLKTCK:
    case AT_NULL:
    default:
        return 0;
    }
}

// The symbol, as the loader's linkage layer sees it: the game's own image
// first, then the libraries it loaded, then the loader's bridges.
static void *resolveLoaderSymbol(const char *name)
{
    if (!name || !*name)
        return NULL;
    // The loader's bridges and the libraries it loaded. Not the Windows
    // modules' exports of the same names: those have Windows ABIs (msvcrt's
    // struct stat, opengl32's stdcall), so a Linux caller must not get them.
    return bridgeTryResolveSymbol(name);
}

// The game's image as a link_map (what dlopen(NULL) and dlinfo() give on
// Linux): where it was loaded, what it was loaded from, and its dynamic
// section.
static struct link_map *mainLinkMap()
{
    static struct link_map mainMap;
    static std::string mainMapName;
    static bool built;
    ElfLoader *loader = SymbolResolver::GetInstance().GetMainLoader();

    if (!loader)
        return NULL;
    if (!built)
    {
        built = true;
        if (!g_phdrsBuilt)
            buildProgramHeaders();
        mainMapName = loader->GetPath();
        mainMap.l_addr = (Elf32_Addr)loader->GetLoadBias();
        mainMap.l_name = (char *)"";
        for (size_t i = 0; i < g_phdrs.size(); i++)
            if (g_phdrs[i].p_type == PT_DYNAMIC)
                mainMap.l_ld = (Elf32_Dyn *)(uintptr_t)(g_phdrs[i].p_vaddr + mainMap.l_addr);
        if (!mainMap.l_ld)
        {
            // No dynamic section: an empty one, so walks of it end at once.
            static Elf32_Dyn none = {DT_NULL, {0}};
            mainMap.l_ld = &none;
        }
        mainMap.l_next = NULL;
        mainMap.l_prev = NULL;
    }
    return &mainMap;
}

// A symbol the game's image defines, from its dynamic symbol table (the
// dumps have no section headers, so ElfLoader exported none of them).
static void *mainImageSymbol(const char *name)
{
    struct link_map *map = mainLinkMap();
    const Elf32_Sym *symtab = NULL;
    const char *strtab = NULL;
    const uint32_t *hash = NULL;

    if (!map || !name)
        return NULL;
    for (const Elf32_Dyn *dyn = map->l_ld; dyn->d_tag != DT_NULL; dyn++)
    {
        if (dyn->d_tag == DT_SYMTAB)
            symtab = (const Elf32_Sym *)(uintptr_t)dyn->d_un.d_ptr;
        else if (dyn->d_tag == DT_STRTAB)
            strtab = (const char *)(uintptr_t)dyn->d_un.d_ptr;
        else if (dyn->d_tag == DT_HASH)
            hash = (const uint32_t *)(uintptr_t)dyn->d_un.d_ptr;
    }
    if (!symtab || !strtab || !hash)
        return NULL;
    for (uint32_t i = 1; i < hash[1]; i++)
    {
        const Elf32_Sym *sym = &symtab[i];
        if (sym->st_shndx == SHN_UNDEF || sym->st_value == 0)
            continue;
        if (ELF32_ST_TYPE(sym->st_info) != STT_FUNC && ELF32_ST_TYPE(sym->st_info) != STT_OBJECT)
            continue;
        if (!strcmp(strtab + sym->st_name, name))
            return (void *)(uintptr_t)(map->l_addr + sym->st_value);
    }
    return NULL;
}

extern "C" void *dlopen(const char *file, int mode)
{
    (void)mode;

    // dlopen(NULL) is the program's own image: the game's, which the Linux
    // code reads as a link_map (its dynamic section).
    if (!file || !*file)
        return (void *)mainLinkMap();

    g_lastError.clear();

    // A Linux library the loader maps to one of its DLLs is loaded by
    // SymbolResolver under its Linux name; one that is already a Windows
    // module (opengl32.dll) loads as itself.
    {
        std::string name(file);
        HMODULE module = LoadLibraryA(name.c_str());

        if (module)
            return (void *)module;
    }
    bridgeLoadNeededLibrary(file);

    // The library is registered with the linkage layer; the handle stands for
    // it, and dlsym() resolves through the layer.
    {
        std::string name(file);
        size_t slash = name.find_last_of("/\\");

        if (slash != std::string::npos)
            name.erase(0, slash + 1);
        if (SymbolResolver::GetInstance().IsKnownLibrary(name) || SymbolResolver::GetInstance().IsKnownLibrary(file))
            return (void *)-2; // a Linux library the loader provides
    }

    g_lastError = std::string("dlopen: ") + file + ": the library is not one the loader provides";
    log_debug("dlopen: %s: not a library the loader provides", file);
    return NULL;
}

extern "C" int dlclose(void *handle)
{
    if (handle && handle != (void *)-1 && handle != (void *)-2 && handle != (void *)mainLinkMap())
        FreeLibrary((HMODULE)handle);
    return 0;
}

extern "C" void *dlsym(void *handle, const char *name)
{
    if (!name)
        return NULL;

    // The loader's SDL3, and the native Linux libraries by their handles.
    if (handle == LL_SDL3_HANDLE)
        return sdl3Export(name);
    {
        void *own = bridgeNativeLibrarySymbol(handle, name);
        if (own)
            return own;
        for (ElfLoader *lib : SymbolResolver::GetInstance().GetNativeLoaders())
            if ((void *)lib == handle)
                return resolveLoaderSymbol(name);
    }
    if (handle == (void *)0xDEADBEEF)
        return resolveLoaderSymbol(name);

    // The global scope (and the game's own handle) starts with the game's
    // image, as on Linux; RTLD_NEXT, asked from the loader, skips it.
    if (handle == RTLD_DEFAULT || handle == (void *)mainLinkMap())
    {
        void *symbol = mainImageSymbol(name);

        if (symbol)
            return symbol;
    }

    // A Windows module the caller opened: its own exports first.
    if (handle && handle != RTLD_NEXT && handle != RTLD_DEFAULT && handle != (void *)-2 &&
        handle != (void *)mainLinkMap())
    {
        FARPROC address = GetProcAddress((HMODULE)handle, name);

        if (address)
            return (void *)address;
    }

    {
        void *symbol = resolveLoaderSymbol(name);

        if (symbol)
            return symbol;
    }

    g_lastError = std::string("dlsym: ") + name + ": symbol not found";
    return NULL;
}

extern "C" void *dlvsym(void *handle, const char *name, const char *version)
{
    (void)version;
    // The version the code asks for (GLIBC_2.0, GLIBC_2.3) is the Linux
    // library's: the loader's own symbols are not versioned.
    return dlsym(handle, name);
}

extern "C" char *dlerror(void)
{
    static char message[512];

    if (g_lastError.empty())
        return NULL;
    snprintf(message, sizeof(message), "%s", g_lastError.c_str());
    g_lastError.clear();
    return message;
}

extern "C" int dladdr(const void *addr, Dl_info *info)
{
    MEMORY_BASIC_INFORMATION region;
    HMODULE module = NULL;
    char path[MAX_PATH];

    if (!addr || !info)
        return 0;
    memset(info, 0, sizeof(*info));

    // The module the address is in, or the mapping ElfLoader made for the
    // game's image (which is not a module).
    if (VirtualQuery(addr, &region, sizeof(region)))
        module = (HMODULE)region.AllocationBase;
    if (module && GetModuleFileNameA(module, path, sizeof(path)))
    {
        info->dli_fname = path;
        info->dli_fbase = (void *)module;
        return 1;
    }
    if (region.AllocationBase)
    {
        // The game's image: where it was mapped, and so where its addresses
        // are relative to.
        static char imageName[MAX_PATH];
        ElfLoader *loader = SymbolResolver::GetInstance().GetMainLoader();

        info->dli_fbase = region.AllocationBase;
        if (loader)
        {
            snprintf(imageName, sizeof(imageName), "%s", loader->GetPath().c_str());
            info->dli_fname = imageName;
        }
        return 1;
    }
    return 0;
}

extern "C" int dladdr1(void *addr, Dl_info *info, void **extra_info, int flags)
{
    (void)flags;
    if (extra_info)
        // The symbol's own table entry, when the caller asks for it: the ones
        // that do (the Namco ES1 dlsym) only need to know that there is none,
        // and fall back.
        *extra_info = NULL;
    return dladdr(addr, info);
}

// The image, as the loader sees it: where it was loaded, what it was loaded
// from, and its dynamic section.
extern "C" int dlinfo(void *handle, int request, void *info)
{
    struct link_map *map = mainLinkMap();

    if (!handle || !map)
        return -1;
    switch (request)
    {
    case RTLD_DI_LINKMAP:
        *(struct link_map **)info = map;
        return 0;
    default:
        return -1;
    }
}

extern "C" int dl_iterate_phdr(dl_iterate_phdr_callback callback, void *data)
{
    struct dl_phdr_info info;
    ElfLoader *loader = SymbolResolver::GetInstance().GetMainLoader();

    if (!callback)
        return 0;
    if (!g_phdrsBuilt)
        buildProgramHeaders();
    if (!loader || g_phdrs.empty())
        return 0;

    memset(&info, 0, sizeof(info));
    info.dlpi_addr = (Elf32_Addr)loader->GetLoadBias();
    info.dlpi_name = loader->GetPath().c_str();
    info.dlpi_phdr = g_phdrs.data();
    info.dlpi_phnum = (Elf32_Half)g_phdrs.size();
    return callback(&info, sizeof(info), data);
}
