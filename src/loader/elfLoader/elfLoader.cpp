#if defined(_WIN32) || defined(__MINGW32__)
#include "elfLoader.hpp"
#include "../log/log.h"
#include "memoryManager.hpp"
#include "symbolResolver.hpp"
#include "linuxStack.hpp"
#include "libcBridge.hpp"
#include "linuxCall.hpp"

// libc start-up, as the loader provides it (symbolResolver.cpp).
extern "C" int __libc_start_main(int (*main_func)(int, char **, char **), int argc, char **argv, void (*init)(void),
                                 void (*fini)(void), void (*rtld_fini)(void), void *stack_end);
#include <elfio.hpp>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <unordered_set>
#include <vector>
#include <windows.h>
#include <winnt.h>
#include <psapi.h>

using namespace ELFIO;

static std::vector<void *> s_PendingEhFrames;

// Every mapped image, for the crash report: a fixed table, as it is read from
// the exception filter with the process going down.
struct ElfImageRange
{
    uintptr_t start;
    uintptr_t end;
    uint32_t loadBias;
    const char *name; // file name only, kept for the life of the process
};
static ElfImageRange s_ElfImages[64];
static volatile LONG s_ElfImageCount;

bool elfImageForAddress(uintptr_t addr, const char **name, uint32_t *vaddr)
{
    LONG count = s_ElfImageCount;
    if (count > (LONG)(sizeof(s_ElfImages) / sizeof(s_ElfImages[0])))
        count = sizeof(s_ElfImages) / sizeof(s_ElfImages[0]);
    for (LONG i = 0; i < count; i++)
        if (addr >= s_ElfImages[i].start && addr < s_ElfImages[i].end)
        {
            *name = s_ElfImages[i].name;
            *vaddr = (uint32_t)addr - s_ElfImages[i].loadBias;
            return true;
        }
    return false;
}

void ElfLoader::RegisterAllEhFrames()
{
    if (s_PendingEhFrames.empty())
        return;

    HMODULE hGcc = GetModuleHandleA("libgcc_s_dw2-1.dll");
    if (!hGcc)
        hGcc = LoadLibraryA("libgcc_s_dw2-1.dll");

    if (!hGcc)
    {
        log_error("[EH] WARNING: libgcc_s_dw2-1.dll not found.");
        return;
    }

    typedef void (*RegisterFrameInfoFn)(void *, void *);
    RegisterFrameInfoFn registerFrameInfo =
        (RegisterFrameInfoFn)GetProcAddress(hGcc, "__register_frame_info");

    log_info("[EH] libgcc_s_dw2-1.dll: __register_frame_info=%p", registerFrameInfo);

    if (!registerFrameInfo)
    {
        log_error("[EH] WARNING: __register_frame_info not found in libgcc_s_dw2-1.dll");
        return;
    }

    log_info("[EH] Registering %zu .eh_frame sections with MinGW DW2 unwinder...", s_PendingEhFrames.size());

    for (void *ehFrame : s_PendingEhFrames)
    {
        void *ob = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!ob) continue;
        memset(ob, 0, 128);

        log_debug("[EH]   __register_frame_info(%p, %p)", ehFrame, ob);
        registerFrameInfo(ehFrame, ob);
        log_debug("[EH]   OK");
    }

    log_info("[EH] All .eh_frame sections registered");
    s_PendingEhFrames.clear();
}

ElfLoader::ElfLoader()
{
    m_Elfio = new elfio();
}

ElfLoader::~ElfLoader()
{
    if (m_Elfio)
    {
        delete m_Elfio;
        m_Elfio = nullptr;
    }
}

void ElfLoader::PreReserveAddressSpace(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return;

    struct Elf32_Ehdr_Raw
    {
        unsigned char e_ident[16];
        uint16_t e_type;
        uint16_t e_machine;
        uint32_t e_version;
        uint32_t e_entry;
        uint32_t e_phoff;
        uint32_t e_shoff;
        uint32_t e_flags;
        uint16_t e_ehsize;
        uint16_t e_phentsize;
        uint16_t e_phnum;
        uint16_t e_shentsize;
        uint16_t e_shnum;
        uint16_t e_shstrndx;
    } ehdr;

    if (fread(&ehdr, sizeof(ehdr), 1, f) != 1)
    {
        fclose(f);
        return;
    }

    if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' || ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F')
    {
        fclose(f);
        return;
    }

    if (ehdr.e_ident[4] != 1)
    {
        fclose(f);
        return;
    }
    if (ehdr.e_type != ET_EXEC)
    {
        fclose(f);
        return;
    }

    struct Elf32_Phdr_Raw
    {
        uint32_t p_type;
        uint32_t p_offset;
        uint32_t p_vaddr;
        uint32_t p_paddr;
        uint32_t p_filesz;
        uint32_t p_memsz;
        uint32_t p_flags;
        uint32_t p_align;
    } phdr;

    uint32_t minAddr = UINT32_MAX;
    uint32_t maxAddr = 0;

    fseek(f, ehdr.e_phoff, SEEK_SET);
    for (uint16_t i = 0; i < ehdr.e_phnum; ++i)
    {
        if (fread(&phdr, sizeof(phdr), 1, f) != 1)
            break;

        if (ehdr.e_phentsize > sizeof(phdr))
            fseek(f, ehdr.e_phentsize - sizeof(phdr), SEEK_CUR);

        if (phdr.p_type == PT_LOAD)
        {
            if (phdr.p_vaddr < minAddr)
                minAddr = phdr.p_vaddr;
            if (phdr.p_vaddr + phdr.p_memsz > maxAddr)
                maxAddr = phdr.p_vaddr + phdr.p_memsz;
        }
    }

    fclose(f);

    if (maxAddr <= minAddr)
        return;

    uint32_t alignedBase = static_cast<uint32_t>(minAddr) & ~0xFFFF;
    uint32_t alignedMax = (static_cast<uint32_t>(maxAddr) + 0xFFFF) & ~0xFFFF;
    SIZE_T totalSize = alignedMax - alignedBase;

    MemoryManager::GetInstance().ReserveAddressSpace(totalSize, reinterpret_cast<void *>((uintptr_t)alignedBase));
}

bool ElfLoader::Load(const std::string &path)
{
    if (m_IsSharedObject)
        return LoadMapAndExport(path);
    if (!LoadMapAndExport(path))
        return false;

    // Mapped: the linkage layer (elfImages.cpp) takes it as the program, for
    // the code that asks the dynamic linker about the image it runs in.
    SymbolResolver::GetInstance().SetMainLoader(this);

    if (!SymbolResolver::GetInstance().ProcessAllRelocations())
        return false;

    if (!ProcessRelocations())
        return false;
    SymbolResolver::GetInstance().PatchAllSOs();

    if (!SymbolResolver::GetInstance().RunAllInits())
        return false;
    RegisterAllEhFrames();

    return true;
}

bool ElfLoader::LoadMapAndExport(const std::string &path)
{
    m_Path = path;

    if (!ParseElf(path))
        return false;

    if (!MapSegmentsToMemory())
        return false;

    if (!ExportSymbols())
        return false;

    if (!ResolveVTables())
        return false;

    if (!LoadDependencies())
        return false;

    return true;
}

bool ElfLoader::ParseElf(const std::string &path)
{
    if (!m_Elfio->load(path))
    {
        log_error("Failed to parse ELF file: %s", path.c_str());
        return false;
    }

    if (m_Elfio->get_class() != ELFCLASS32)
    {
        log_error("Only 32-bit ELF files are supported");
        return false;
    }

    if (m_Elfio->get_machine() != EM_386)
    {
        log_error("Only x86 machine type is supported");
        return false;
    }

    log_info("Successfully parsed 32-bit x86 ELF: %s", path.c_str());
    return true;
}

bool ElfLoader::MapSegmentsToMemory()
{
    log_info("Mapping segments to memory...");

    Elf_Half segmentsNum = m_Elfio->segments.size();
    if (segmentsNum == 0)
    {
        log_error("No segments found in the ELF file");
        return false;
    }

    Elf64_Addr minLoadAddr = (Elf64_Addr)-1;
    Elf64_Addr maxLoadAddr = 0;

    for (Elf_Half i = 0; i < segmentsNum; ++i)
    {
        segment *pSegment = m_Elfio->segments[i];
        if (pSegment->get_type() == PT_LOAD)
        {
            Elf64_Addr vaddr = pSegment->get_virtual_address();
            Elf_Xword memsize = pSegment->get_memory_size();

            if (vaddr < minLoadAddr)
                minLoadAddr = vaddr;
            if (vaddr + memsize > maxLoadAddr)
                maxLoadAddr = vaddr + memsize;
        }
    }

    if (maxLoadAddr <= minLoadAddr)
    {
        log_error("Invalid load addresses min: 0x%llx max: 0x%llx", minLoadAddr, maxLoadAddr);
        return false;
    }

    Elf64_Addr originalMinLoadAddr = minLoadAddr;

    size_t totalSize = maxLoadAddr - minLoadAddr;
    log_debug("Total memory required for ELF: %zu bytes", totalSize);

    void *requestedAddress = nullptr;
    if (m_Elfio->get_type() == ET_EXEC)
    {
        uint32_t alignedAddr = static_cast<uint32_t>(minLoadAddr) & ~0xFFFF;
        requestedAddress = reinterpret_cast<void *>(alignedAddr);
        size_t offset = minLoadAddr - alignedAddr;
        totalSize += offset;

        minLoadAddr = alignedAddr;

        log_info("ET_EXEC detected. Adjusted allocation address to 64KB boundary: %p, original was 0x%llx", requestedAddress,
                 minLoadAddr + offset);
    }

    m_BaseAddress = MemoryManager::GetInstance().AllocateExecutable(totalSize, requestedAddress);
    if (!m_BaseAddress)
    {
        log_fatal("Failed to allocate %zu bytes for ELF", totalSize);
        return false;
    }

    if (m_Elfio->get_type() == ET_EXEC && m_BaseAddress != requestedAddress)
    {
        log_fatal("ET_EXEC allocation failed at required fixed address %p. Got %p instead.", requestedAddress, m_BaseAddress);
        return false;
    }

    log_debug("Allocated memory at base address: %p", m_BaseAddress);
    log_info("DIAG: %s mapped at %p", m_Path.c_str(), m_BaseAddress);

    m_LoadBias = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_BaseAddress)) - static_cast<uint32_t>(minLoadAddr);
    m_ImageBase = reinterpret_cast<void *>(originalMinLoadAddr + m_LoadBias);

    {
        LONG slot = InterlockedIncrement(&s_ElfImageCount) - 1;
        if (slot < (LONG)(sizeof(s_ElfImages) / sizeof(s_ElfImages[0])))
        {
            size_t slash = m_Path.find_last_of("/\\");
            ElfImageRange &range = s_ElfImages[slot];
            range.start = reinterpret_cast<uintptr_t>(m_BaseAddress);
            range.end = range.start + totalSize;
            range.loadBias = m_LoadBias;
            range.name = _strdup(slash == std::string::npos ? m_Path.c_str() : m_Path.c_str() + slash + 1);
        }
    }

    for (Elf_Half i = 0; i < segmentsNum; ++i)
    {
        segment *pSegment = m_Elfio->segments[i];
        if (pSegment->get_type() == PT_LOAD)
        {
            Elf64_Addr vaddr = pSegment->get_virtual_address();
            Elf_Xword filesz = pSegment->get_file_size();
            Elf_Xword memsz = pSegment->get_memory_size();

            uint8_t *dest = reinterpret_cast<uint8_t *>(vaddr + m_LoadBias);

            if (filesz > 0)
            {
                memcpy(dest, pSegment->get_data(), filesz);
            }

            if (memsz > filesz)
            {
                memset(dest + filesz, 0, memsz - filesz);
            }

            log_trace("Mapped PT_LOAD segment to %p, size %zu", dest, (size_t)memsz);
        }
    }
    {
        bool collected = false;
        for (Elf_Half i = 0; i < m_Elfio->sections.size(); ++i)
        {
            section *pSec = m_Elfio->sections[i];
            if (pSec->get_name() == ".eh_frame" && pSec->get_size() > 0)
            {
                void *ehFrameAddr = reinterpret_cast<void *>(pSec->get_address() + m_LoadBias);
                s_PendingEhFrames.push_back(ehFrameAddr);
                log_info("[EH] Collected .eh_frame at %p (size %llu) for %s\n", ehFrameAddr, (unsigned long long)pSec->get_size(), m_Path.c_str());
                collected = true;
                break;
            }
        }
        // Images without section headers (the Raw Thrills dumps): .eh_frame
        // through .eh_frame_hdr, which PT_GNU_EH_FRAME locates and whose
        // header points at it. Without it the unwinder knew none of the
        // game's frames, and every exception the game threw ended in
        // std::terminate (Cruis'n Blast's boost coroutines' forced_unwind).
        for (Elf_Half i = 0; !collected && i < segmentsNum; ++i)
        {
            const segment *seg = m_Elfio->segments[i];
            if (seg->get_type() != PT_GNU_EH_FRAME || seg->get_memory_size() < 8)
                continue;
            const uint8_t *hdr = reinterpret_cast<const uint8_t *>(seg->get_virtual_address() + m_LoadBias);
            uint8_t enc = hdr[1];
            int32_t value;
            memcpy(&value, hdr + 4, 4);
            uintptr_t ehFrame = 0;
            switch (enc)
            {
            case 0x1b: // DW_EH_PE_pcrel | DW_EH_PE_sdata4
                ehFrame = reinterpret_cast<uintptr_t>(hdr + 4) + value;
                break;
            case 0x3b: // DW_EH_PE_datarel | DW_EH_PE_sdata4: from the header's start
                ehFrame = reinterpret_cast<uintptr_t>(hdr) + value;
                break;
            case 0x03: // DW_EH_PE_udata4
            case 0x0b: // DW_EH_PE_sdata4
                ehFrame = static_cast<uint32_t>(value) + m_LoadBias;
                break;
            default:
                log_warn("[EH] %s: .eh_frame_hdr pointer encoding 0x%02x not handled", m_Path.c_str(), enc);
                break;
            }
            if (hdr[0] == 1 && ehFrame)
            {
                s_PendingEhFrames.push_back(reinterpret_cast<void *>(ehFrame));
                log_info("[EH] Collected .eh_frame at %p (from .eh_frame_hdr) for %s\n", reinterpret_cast<void *>(ehFrame),
                         m_Path.c_str());
                collected = true;
            }
        }
    }

    return true;
}

bool ElfLoader::LoadDependencies()
{
    log_info("Loading dependencies...");
    bool foundDynamicSection = false;
    static bool librariesRegistered = false;
    if (!librariesRegistered)
    {
        librariesRegistered = true;

        SymbolResolver::GetInstance().RegisterLibrary("libopenal.so.0", "openal32.dll");
        SymbolResolver::GetInstance().RegisterLibrary("libopenal.so.1", "openal32.dll");
        SymbolResolver::GetInstance().RegisterLibrary("libCg.so", "Cg.dll");
        SymbolResolver::GetInstance().RegisterLibrary("libCgGL.so", "CgGL.dll");

        SymbolResolver::GetInstance().RegisterLibrary("libz.so.1", "zlib1.dll");

        SymbolResolver::GetInstance().RegisterLibrary("libc.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libc.so.6", "INTERNAL");

        SymbolResolver::GetInstance().RegisterLibrary("libpthread.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libpthread.so.0", "INTERNAL");

        SymbolResolver::GetInstance().RegisterLibrary("libgcc.so", "libgcc_s_dw2-1.dll");
        SymbolResolver::GetInstance().RegisterLibrary("libgcc_s.so.1", "libgcc_s_dw2-1.dll");

        SymbolResolver::GetInstance().RegisterLibrary("libkswapapi.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libsegaapi.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("ld-linux.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("ld-linux.so.2", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libasound.so.2", "INTERNAL");
        // PulseAudio: Big Buck HD Wild dlopen()s libpulse-simple for sound.
        // pulse2sdlBridge.cpp provides pa_simple over the same SDL sink.
        SymbolResolver::GetInstance().RegisterLibrary("libpulse-simple.so.0", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libpulse-simple.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libpulse.so.0", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libpulse.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libXxf86vm.so.1", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libX11.so.6", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libXext.so.6", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libXmu.so.6", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libXi.so.6", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libGL.so.1", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libGLU.so.1", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libglut.so.3", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libm.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libm.so.6", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libdl.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libdl.so.2", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libimf.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libirc.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libsvml.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libalpb.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libbdlog.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libsama.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libposixtime.so", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libposixtime.so.1", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libposixtime.so.2.4", "INTERNAL");
        // Raw Thrills: their functions are bridges (rtLibcBridge.cpp).
        SymbolResolver::GetInstance().RegisterLibrary("librt.so.1", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libudev.so.1", "INTERNAL");
        SymbolResolver::GetInstance().RegisterLibrary("libudev.so.0", "INTERNAL");
    }

    for (Elf_Half i = 0; i < m_Elfio->sections.size(); ++i)
    {
        section *pSection = m_Elfio->sections[i];
        if (pSection->get_type() == SHT_DYNAMIC)
        {
            dynamic_section_accessor dynamic(*m_Elfio, pSection);
            Elf_Xword dyn_num = dynamic.get_entries_num();
            for (Elf_Xword j = 0; j < dyn_num; ++j)
            {
                Elf_Xword tag;
                Elf_Xword value;
                std::string str;
                dynamic.get_entry(j, tag, value, str);

                if (tag == DT_NEEDED)
                {
                    log_info("ELF needs library: %s", str.c_str());
                    SymbolResolver::GetInstance().LoadNeededLibrary(str);
                    foundDynamicSection = true;
                }
            }
        }
    }

    // Images without section headers (the Raw Thrills and Namco cabinet
    // dumps) still have their dynamic segment: its DT_NEEDED entries, read
    // from the mapped image. Libraries the cabinet named by absolute path
    // (/pm/lib/libfmod.so) are looked up by file name in the search paths.
    if (!foundDynamicSection)
    {
        for (Elf_Half i = 0; i < m_Elfio->segments.size(); ++i)
        {
            const segment *seg = m_Elfio->segments[i];
            if (seg->get_type() != PT_DYNAMIC)
                continue;
            const uint32_t *dyn = (const uint32_t *)(uintptr_t)(seg->get_virtual_address() + m_LoadBias);
            const char *strtab = nullptr;
            for (const uint32_t *d = dyn; d[0] != DT_NULL; d += 2)
                if (d[0] == DT_STRTAB)
                    strtab = (const char *)(uintptr_t)(d[1] + m_LoadBias);
            if (!strtab)
                break;
            for (const uint32_t *d = dyn; d[0] != DT_NULL; d += 2)
            {
                if (d[0] != DT_NEEDED)
                    continue;
                std::string name = strtab + d[1];
                size_t slash = name.find_last_of('/');
                if (slash != std::string::npos)
                    name.erase(0, slash + 1);
                log_info("ELF needs library (program headers): %s", name.c_str());
                SymbolResolver::GetInstance().LoadNeededLibrary(name);
            }
            break;
        }
    }
    return true;
}

#ifndef DT_RELRSZ
#define DT_RELRSZ 35
#endif
#ifndef DT_RELR
#define DT_RELR   36
#endif

#define R_386_32        1
#define R_386_PC32      2
#define R_386_COPY      5
#define R_386_GLOB_DAT  6
#define R_386_JMP_SLOT  7
#define R_386_RELATIVE  8

bool ElfLoader::ProcessRelocations()
{
    if (m_Relocated)
        return true;
    m_Relocated = true;

    log_info("Processing relocations...");

    Elf64_Addr relrAddr = 0;
    Elf_Xword relrSz = 0;
    std::unordered_set<uintptr_t> relrPatchedAddrs;

    for (Elf_Half i = 0; i < m_Elfio->sections.size(); ++i)
    {
        section *pSection = m_Elfio->sections[i];
        if (pSection->get_type() == SHT_DYNAMIC)
        {
            dynamic_section_accessor dynamic(*m_Elfio, pSection);
            for (Elf_Xword j = 0; j < dynamic.get_entries_num(); ++j)
            {
                Elf_Xword tag;
                Elf_Xword value;
                std::string str;
                dynamic.get_entry(j, tag, value, str);
                if (tag == DT_RELR)
                    relrAddr = value;
                if (tag == DT_RELRSZ)
                    relrSz = value;
            }
        }
    }

    if (relrAddr != 0 && relrSz > 0)
    {
        uint8_t *actualRelr = reinterpret_cast<uint8_t *>(relrAddr + m_LoadBias);
        uint32_t *relrData = reinterpret_cast<uint32_t *>(actualRelr);
        size_t count = relrSz / sizeof(uint32_t);

        uint32_t base = 0;
        for (size_t i = 0; i < count; ++i)
        {
            uint32_t entry = relrData[i];
            if ((entry & 1) == 0)
            {
                uint32_t *patchAddr = reinterpret_cast<uint32_t *>(entry + m_LoadBias);
                *patchAddr += m_LoadBias;
                relrPatchedAddrs.insert(reinterpret_cast<uintptr_t>(patchAddr));
                base = entry + sizeof(uint32_t);
            }
            else
            {
                uint32_t bitmap = entry >> 1;
                uint32_t offset = base;
                while (bitmap != 0)
                {
                    if (bitmap & 1)
                    {
                        uint32_t *patchAddr = reinterpret_cast<uint32_t *>(offset + m_LoadBias);
                        *patchAddr += m_LoadBias;
                        relrPatchedAddrs.insert(reinterpret_cast<uintptr_t>(patchAddr));
                    }
                    offset += sizeof(uint32_t);
                    bitmap >>= 1;
                }
                base += 31 * sizeof(uint32_t);
            }
        }
        log_info("Processed %zu bytes of DT_RELR packed relative relocations (%zu addresses)", (size_t)relrSz, relrPatchedAddrs.size());
    }

    for (Elf_Half i = 0; i < m_Elfio->sections.size(); ++i)
    {
        section *pSection = m_Elfio->sections[i];
        if (pSection->get_type() == SHT_REL || pSection->get_type() == SHT_RELA)
        {
            log_debug("Found relocation section: %s", pSection->get_name().c_str());

            relocation_section_accessor rel(*m_Elfio, pSection);
            log_debug("  Entries: %d", rel.get_entries_num());
            for (Elf_Xword j = 0; j < rel.get_entries_num(); ++j)
            {
                Elf64_Addr offset;
                Elf_Word symbolIdx;
                Elf_Word type;
                Elf_Sxword addend;

                if (rel.get_entry(j, offset, symbolIdx, type, addend))
                {
                    std::string symbolName = "";
                    Elf_Xword symSize = 0;
                    section *symSec = m_Elfio->sections[pSection->get_link()];
                    if (symSec)
                    {
                        symbol_section_accessor symbols(*m_Elfio, symSec);
                        Elf64_Addr value;
                        unsigned char bind;
                        unsigned char symcat;
                        Elf_Half shndx;
                        unsigned char other;
                        symbols.get_symbol(symbolIdx, symbolName, value, symSize, bind, symcat, shndx, other);
                    }

                    uint8_t *targetAddr = reinterpret_cast<uint8_t *>(offset + m_LoadBias);
                    uint32_t *patchAddr = reinterpret_cast<uint32_t *>(targetAddr);
                    if (type == R_386_RELATIVE)
                    {
                        if (relrPatchedAddrs.count(reinterpret_cast<uintptr_t>(patchAddr)))
                        {
                            log_trace("Skipping R_386_RELATIVE at %p (already handled by DT_RELR)", patchAddr);
                            continue;
                        }
                        *patchAddr += m_LoadBias;
                        log_trace("Applied R_386_RELATIVE at %p (offset: 0x%llx), new val: 0x%08X", patchAddr, offset, *patchAddr);
                        continue;
                    }

                    if (!symbolName.empty())
                    {
                        std::string moduleName;
                        void *resolvedFunc = SymbolResolver::GetInstance().ResolveSymbol(symbolName, &moduleName);

                        if (resolvedFunc)
                        {
                            if (moduleName == "UNRESOLVED_STUB")
                            {
                                log_info("Symbol \"%s\" was not found, assigned crash stub at %p", symbolName.c_str(), resolvedFunc);
                            }
                            else if (moduleName == "WEAK_SYMBOL")
                            {
                                log_info("Symbol \"%s\" was resolved to NULL (Not Implemented/Required)", symbolName.c_str());
                            }
                            else
                            {
                                log_info("Symbol \"%s\" was resolved in \"%s\"", symbolName.c_str(), moduleName.c_str());
                            }
                            uint32_t symValue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(resolvedFunc));

                            uint32_t inlineAddend = (pSection->get_type() == SHT_REL) ? *patchAddr : addend;
                            switch (type)
                            {
                                case R_386_32:
                                    *patchAddr = symValue + inlineAddend;
                                    log_trace("Applied R_386_32 for %s at %p (addend=%d -> val=0x%x)", symbolName.c_str(), patchAddr,
                                              inlineAddend, *patchAddr);
                                    break;
                                case R_386_PC32:
                                {
                                    *patchAddr = symValue + inlineAddend - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(patchAddr));
                                    log_trace("Applied R_386_PC32 for %s at %p (offset: 0x%x)", symbolName.c_str(), patchAddr, *patchAddr);
                                    break;
                                }
                                case R_386_COPY:
                                {
                                    if (symSize > 0)
                                    {
                                        void *srcAddr = m_IsSharedObject
                                                            ? resolvedFunc
                                                            : SymbolResolver::GetInstance().ResolveSymbolInSharedLibs(symbolName);

                                        if (srcAddr && srcAddr != patchAddr)
                                        {
                                            memcpy(patchAddr, srcAddr, symSize);
                                            log_info("Applied R_386_COPY for %s (%llu bytes) from %p to %p", symbolName.c_str(), symSize,
                                                     srcAddr, patchAddr);
                                            SymbolResolver::GetInstance().RegisterVTable(symbolName, patchAddr);
                                        }
                                        else
                                        {
                                            log_info("R_386_COPY for %s: source not found in shared libs (srcAddr=%p)", symbolName.c_str(),
                                                     srcAddr);
                                        }
                                    }
                                    else
                                    {
                                        log_info("R_386_COPY for %s has 0 size", symbolName.c_str());
                                    }
                                    break;
                                }
                                case R_386_GLOB_DAT:
                                case R_386_JMP_SLOT:
                                    *patchAddr = symValue;
                                    log_trace("Applied R_386_GLOB_DAT / R_386_JMP_SLOT for %s at %p", symbolName.c_str(), patchAddr);
                                    break;
                                default:
                                    log_info("Unhandled relocation type %lu for symbol %s", type, symbolName.c_str());
                                    break;
                            }
                        }
                        else
                        {
                            log_error("Failed to resolve symbol %s and failed to create stub", symbolName.c_str());
                        }
                    }
                    else
                    {
                        log_info("Unhandled anonymous relocation type %lu at %p", type, patchAddr);
                    }
                }
            }
        }
    }
    return true;
}

bool ElfLoader::ExportSymbols()
{
    log_info("Exporting dynamic symbols for resolution...");

    section *dynSymSec = nullptr;
    for (Elf_Half i = 0; i < m_Elfio->sections.size(); ++i)
    {
        if (m_Elfio->sections[i]->get_type() == SHT_DYNSYM)
        {
            dynSymSec = m_Elfio->sections[i];
            break;
        }
    }

    if (!dynSymSec)
    {
        log_info("No SHT_DYNSYM section found - nothing to export");
        return true;
    }

    symbol_section_accessor symbols(*m_Elfio, dynSymSec);

    for (Elf_Xword k = 0; k < symbols.get_symbols_num(); ++k)
    {
        std::string symbolName = "";
        Elf64_Addr value;
        Elf_Xword size;
        unsigned char bind;
        unsigned char type;
        Elf_Half shndx;
        unsigned char other;

        if (symbols.get_symbol(k, symbolName, value, size, bind, type, shndx, other))
        {
            if (value != 0 && shndx != 0 && (bind == STB_GLOBAL || bind == STB_WEAK || bind == 10) &&
                (type == STT_FUNC || type == STT_OBJECT || type == 10))
            {
                void *symbolAddr = reinterpret_cast<void *>(value + m_LoadBias);
                SymbolResolver::GetInstance().RegisterNativeSymbol(symbolName, symbolAddr);

                size_t atPos = symbolName.find('@');
                if (atPos != std::string::npos)
                {
                    std::string baseName = symbolName.substr(0, atPos);
                    SymbolResolver::GetInstance().RegisterNativeSymbol(baseName, symbolAddr);
                }
            }
        }
    }

    log_info("Exporting static symbols for findStaticFnAddr...");
    section *symSec = nullptr;
    for (Elf_Half i = 0; i < m_Elfio->sections.size(); ++i)
    {
        if (m_Elfio->sections[i]->get_type() == SHT_SYMTAB)
        {
            symSec = m_Elfio->sections[i];
            break;
        }
    }

    if (symSec)
    {
        symbol_section_accessor staticSymbols(*m_Elfio, symSec);
        for (Elf_Xword k = 0; k < staticSymbols.get_symbols_num(); ++k)
        {
            std::string symbolName = "";
            Elf64_Addr value;
            Elf_Xword size;
            unsigned char bind;
            unsigned char type;
            Elf_Half shndx;
            unsigned char other;
            if (staticSymbols.get_symbol(k, symbolName, value, size, bind, type, shndx, other))
            {
                if (value != 0 && shndx != 0 && type == STT_FUNC)
                {
                    void *symbolAddr = reinterpret_cast<void *>(value + m_LoadBias);
                    SymbolResolver::GetInstance().RegisterNativeSymbol(symbolName, symbolAddr);
                }
            }
        }
    }

    return true;
}

void *ElfLoader::FindExportedSymbol(const std::string &name) const
{
    if (!m_Elfio)
        return nullptr;

    for (Elf_Half i = 0; i < m_Elfio->sections.size(); ++i)
    {
        if (m_Elfio->sections[i]->get_type() == SHT_DYNSYM)
        {
            symbol_section_accessor symbols(*m_Elfio, m_Elfio->sections[i]);
            for (Elf_Xword k = 0; k < symbols.get_symbols_num(); ++k)
            {
                std::string symName;
                Elf64_Addr value;
                Elf_Xword size;
                unsigned char bind, type;
                Elf_Half shndx;
                unsigned char other;

                if (symbols.get_symbol(k, symName, value, size, bind, type, shndx, other))
                {
                    if (symName == name && value != 0 && shndx != 0)
                    {
                        return reinterpret_cast<void *>(value + m_LoadBias);
                    }
                }
            }
            break;
        }
    }
    return nullptr;
}

bool ElfLoader::RunInit()
{
    // Once: set before the init functions run, as a library's constructor
    // may dlopen() another library, and every dlopen() runs the pending
    // inits again (sdl2-compat loading SDL3 re-entered itself until the
    // stack ran out).
    if (m_Initialized)
        return true;
    m_Initialized = true;
    log_info("Running initialization for shared object: %s", m_Path.c_str());

    for (Elf_Half i = 0; i < m_Elfio->sections.size(); ++i)
    {
        section *pSection = m_Elfio->sections[i];
        if (pSection->get_type() == SHT_DYNAMIC)
        {
            dynamic_section_accessor dynamic(*m_Elfio, pSection);
            Elf_Xword dyn_num = dynamic.get_entries_num();

            Elf64_Addr initAddr = 0;
            Elf64_Addr initArrayAddr = 0;
            Elf_Xword initArraySz = 0;

            for (Elf_Xword j = 0; j < dyn_num; ++j)
            {
                Elf_Xword tag;
                Elf_Xword value;
                std::string str;
                dynamic.get_entry(j, tag, value, str);

                if (tag == DT_INIT)
                {
                    initAddr = value;
                }
                else if (tag == DT_INIT_ARRAY)
                {
                    initArrayAddr = value;
                }
                else if (tag == DT_INIT_ARRAYSZ)
                {
                    initArraySz = value;
                }
            }

            if (initAddr != 0)
            {
                void *actualInit = reinterpret_cast<void *>(initAddr + m_LoadBias);
                log_debug("Calling DT_INIT at %p", actualInit);
                typedef void (*InitFunc)(void);
                InitFunc init = reinterpret_cast<InitFunc>(actualInit);
                callLinuxAligned0(init);
            }

            if (initArrayAddr != 0 && initArraySz > 0)
            {
                void *actualArray = reinterpret_cast<void *>(initArrayAddr + m_LoadBias);
                log_debug("Calling DT_INIT_ARRAY at %p (size %llu bytes)", actualArray, initArraySz);

                size_t numFuncs = initArraySz / sizeof(uint32_t);
                uint32_t *funcArray = reinterpret_cast<uint32_t *>(actualArray);
                for (size_t k = 0; k < numFuncs; ++k)
                {
                    log_debug("Calling DT_INIT_ARRAY function %zu at %p", k, funcArray[k]);
                    uint32_t funcPtrVal = funcArray[k];
                    if (funcPtrVal == 0 || funcPtrVal == 0xFFFFFFFF)
                        continue;

                    if (funcPtrVal < 0x10000)
                    {
                        log_info("DT_INIT_ARRAY[%zu] has suspiciously low address 0x%08X — skipping (possibly un-relocated)", k, funcPtrVal);
                        continue;
                    }

                    log_debug("Calling DT_INIT_ARRAY function %zu at 0x%08X", k, funcPtrVal);
                    typedef void (*InitArrayFunc)(void);
                    InitArrayFunc func = reinterpret_cast<InitArrayFunc>(reinterpret_cast<uintptr_t>(funcPtrVal));
                    callLinuxAligned0(func);
                }
            }
        }
    }

    return true;
}

bool ElfLoader::ResolveVTables()
{
    log_info("Resolving VTables...");

    for (Elf_Half i = 0; i < m_Elfio->sections.size(); ++i)
    {
        section *pSection = m_Elfio->sections[i];
        if (pSection->get_name() == ".rodata" || pSection->get_name() == ".data")
        {
            for (Elf_Half j = 0; j < m_Elfio->sections.size(); ++j)
            {
                if (m_Elfio->sections[j]->get_type() != SHT_SYMTAB && m_Elfio->sections[j]->get_type() != SHT_DYNSYM)
                    continue;

                section *symSec = m_Elfio->sections[j];
                symbol_section_accessor symbols(*m_Elfio, symSec);
                for (Elf_Xword k = 0; k < symbols.get_symbols_num(); ++k)
                {
                    std::string symbolName = "";
                    Elf64_Addr value;
                    Elf_Xword size;
                    unsigned char bind;
                    unsigned char type;
                    Elf_Half shndx;
                    unsigned char other;

                    if (symbols.get_symbol(k, symbolName, value, size, bind, type, shndx, other))
                    {
                        if (shndx == i && (symbolName.rfind("_ZTV", 0) == 0 || symbolName.rfind("_ZTT", 0) == 0))
                        {
                            log_debug("Found VTable/VTT symbol: %s at 0x%llx", symbolName.c_str(), value);

                            void *vtableAddr = reinterpret_cast<void *>(value + m_LoadBias);

                            SymbolResolver::GetInstance().RegisterVTable(symbolName, vtableAddr);
                        }
                    }
                }
            }
        }
    }

    return true;
}

bool ElfLoader::Execute(int argc, char **argv, char **envp)
{
    if (m_IsSharedObject)
    {
        log_info("Shared Object loaded successfully. Skipping entrypoint execution.");
        return true;
    }

    log_info("Preparing to execute main ELF...");
    Elf64_Addr entryAddr = m_Elfio->get_entry();
    if (entryAddr == 0)
    {
        log_error("No entry point found - cannot execute");
        return false;
    }

    log_info("Entry point original address: 0x%llx", entryAddr);

    void *actualEntry = reinterpret_cast<void *>(entryAddr + m_LoadBias);

    log_info("Executing Main ELF at mapped address: %p", actualEntry);
    ElfAuxvInfo auxvInfo = {};
    auxvInfo.AtPageSz = 4096;
    auxvInfo.AtEntry = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(actualEntry));
    auxvInfo.AtPhent = 32;
    auxvInfo.AtPhnum = static_cast<uint32_t>(m_Elfio->segments.size());

    for (Elf_Half i = 0; i < m_Elfio->segments.size(); ++i)
    {
        if (m_Elfio->segments[i]->get_type() == PT_PHDR)
        {
            auxvInfo.AtPhdr = static_cast<uint32_t>(m_Elfio->segments[i]->get_virtual_address() + m_LoadBias);
            break;
        }
    }
    log_info("Auxv: AT_PHDR=0x%08X AT_PHENT=%u AT_PHNUM=%u AT_PAGESZ=%u AT_ENTRY=0x%08X",
             auxvInfo.AtPhdr, auxvInfo.AtPhent, auxvInfo.AtPhnum, auxvInfo.AtPageSz, auxvInfo.AtEntry);

    const uint32_t STACK_SIZE = 8 * 1024 * 1024;
    const uint32_t RESERVE_SIZE = 0x10000;

    uint32_t stackBase = 0;
    uint32_t stackLimit = 0;

    uint32_t esp = LinuxStack::Setup(STACK_SIZE, argc, argv, &stackBase, &stackLimit, RESERVE_SIZE, &auxvInfo);

    __writefsdword(0x04, stackBase);
    __writefsdword(0x08, stackLimit);

    log_info("Stack Alloc: %p - %p", (void *)stackLimit, (void *)stackBase);

    typedef void (*EntryPointFunc)(void);
    EntryPointFunc entry = reinterpret_cast<EntryPointFunc>(actualEntry);
    log_info("Jumping to entry point %p", entry);
    {
        // The entry code, to see how the program reaches main().
        const uint8_t *code = reinterpret_cast<const uint8_t *>(actualEntry);
        char hex[3 * 64 + 1];
        for (int i = 0; i < 64; i++)
            snprintf(hex + 3 * i, 4, "%02x ", code[i]);
        log_info("Entry code: %s", hex);
    }
    log_info("Stack ESP: 0x%08X", esp);

    Sleep(100);

    __asm__ volatile("push $0x1F80\n\t"
                     "ldmxcsr (%%esp)\n\t"
                     "add $4, %%esp\n\t"
                     :
                     :
                     : "memory");

    // An entry that is not glibc's _start ("xor %ebp,%ebp; pop %esi; ...")
    // but main itself (Cruis'n Blast dumps whose entry was set to main): do
    // what _start would, so main gets argc and argv as arguments and the
    // libc start-up (__libc_start_main: thread emulation, sockets) runs.
    const uint8_t *entryCode = reinterpret_cast<const uint8_t *>(actualEntry);
    bool entryIsMain = !m_IsSharedObject && !(entryCode[0] == 0x31 && entryCode[1] == 0xed) &&
                       entryCode[0] == 0x55 && entryCode[1] == 0x89 && entryCode[2] == 0xe5;
    if (entryIsMain)
    {
        log_info("The entry point is main() itself, not _start: calling it through __libc_start_main");
        void *startMain = reinterpret_cast<void *>(&__libc_start_main);
        __asm__ volatile("mov %%eax, %%esp\n\t"
                         "xor %%ebp, %%ebp\n\t"
                         "pop %%esi\n\t"      // argc
                         "mov %%esp, %%ecx\n\t" // argv
                         "and $-16, %%esp\n\t"
                         "push %%eax\n\t"     // padding, as _start
                         "push %%esp\n\t"     // stack_end
                         "push $0\n\t"        // rtld_fini
                         "push $0\n\t"        // fini
                         "push $0\n\t"        // init
                         "push %%ecx\n\t"     // argv
                         "push %%esi\n\t"     // argc
                         "push %%ebx\n\t"     // main
                         "call *%%edx\n\t"
                         "hlt\n\t"
                         :
                         : "a"(esp), "b"(entry), "d"(startMain)
                         : "ecx", "esi", "memory");
        __builtin_unreachable();
    }

    __asm__ volatile("mov %0, %%esp\n\t"
                     "xor %%eax, %%eax\n\t"
                     "xor %%ebx, %%ebx\n\t"
                     "xor %%ecx, %%ecx\n\t"
                     "jmp *%1\n\t"
                     :
                     : "r"(esp), "r"(entry)
                     : "eax", "ebx", "ecx", "memory");

    __builtin_unreachable();
}

extern "C" void *GetStaticFunctionAddress(const char *name)
{
    std::string moduleName;
    void *result = SymbolResolver::GetInstance().ResolveSymbol(name, &moduleName);
    if (!result)
    {
        log_error("Failed to resolve symbol: %s\n", name);
        return NULL;
    }
    return result;
}

#endif