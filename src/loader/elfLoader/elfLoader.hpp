#pragma once

#include <string>
#include <cstdint>

// Forward declarations to avoid exposing ELFIO directly in headers if possible,
// but since we'll likely use it heavily, including ELFIO might be easier.
// For now, we keep it simple.
namespace ELFIO
{
    class elfio;
}

// The loaded ELF image an address lies in: its file name and the address as
// the image's own (unbiased) virtual address, for disassembly.
bool elfImageForAddress(uintptr_t addr, const char **name, uint32_t *vaddr);

class ElfLoader
{
  public:
    ElfLoader();
    ~ElfLoader();

    // Pre-reserve the ELF's fixed virtual address range so that DLLs
    // loaded later (e.g. msys-2.0.dll) cannot claim the same region.
    // Must be called before any LoadLibraryA that might conflict.
    static void PreReserveAddressSpace(const std::string &path);

    // Loads the ELF file into memory and resolves dependencies
    bool Load(const std::string &path);

    // Executes the loaded ELF file
    bool Execute(int argc, char **argv, char **envp);

    void SetIsSharedObject(bool isSO)
    {
        m_IsSharedObject = isSO;
    }

    // Returns the runtime base address of the loaded ELF (0x08048000 for standard ELFs or the allocated address for PIE)
    void *GetBaseAddress() const
    {
        return m_ImageBase;
    }

    // The linkage layer's view of this image (elfImages.cpp): the bias its
    // addresses are shifted by, where it was loaded from, and the file it was
    // parsed from - which is where its program headers and dynamic section are
    // read from.
    uint32_t GetLoadBias() const
    {
        return m_LoadBias;
    }

    const std::string &GetPath() const
    {
        return m_Path;
    }

    bool IsSharedObject() const
    {
        return m_IsSharedObject;
    }

    const ELFIO::elfio *GetElfio() const
    {
        return m_Elfio;
    }

    // Look up a symbol in this ELF's dynsym table, returns biased address or nullptr
    void *FindExportedSymbol(const std::string &name) const;

    bool LoadMapAndExport(const std::string &path);
    bool ProcessRelocations();
    bool RunInit();

  private:
    // Register all collected .eh_frame sections with the MinGW DW2 unwinder.
    static void RegisterAllEhFrames();

    ELFIO::elfio *m_Elfio;
    bool m_IsSharedObject = false;
    bool m_Relocated = false;
    bool m_Initialized = false;
    std::string m_Path;

    bool ParseElf(const std::string &path);
    bool MapSegmentsToMemory();
    bool LoadDependencies();
    bool ResolveVTables();
    bool ExportSymbols();

    // Base address where the ELF is loaded
    void *m_BaseAddress = nullptr;
    void *m_ImageBase = nullptr;
    uint32_t m_LoadBias = 0;
};