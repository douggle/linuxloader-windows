#ifndef LL_WIN32_LINK_H
#define LL_WIN32_LINK_H

// <link.h> for MinGW.  The arcade support asks the dynamic linker about the
// image it runs in: this is the loader's own view of it, over ElfLoader
// (platform/win32/win32Posix.c), not glibc's.

#include <elf.h>

#ifdef __cplusplus
extern "C"
{
#endif

    struct link_map
    {
        Elf32_Addr l_addr;        // the bias its addresses are shifted by
        char *l_name;             // where it was loaded from
        Elf32_Dyn *l_ld;          // its dynamic section
        struct link_map *l_next;  // the images, in the order they joined
        struct link_map *l_prev;
    };

    struct dl_phdr_info
    {
        Elf32_Addr dlpi_addr;
        const char *dlpi_name;
        const Elf32_Phdr *dlpi_phdr;
        Elf32_Half dlpi_phnum;
    };

    typedef int (*dl_iterate_phdr_callback)(struct dl_phdr_info *info, size_t size, void *data);

    int dl_iterate_phdr(dl_iterate_phdr_callback callback, void *data);

#ifdef __cplusplus
}
#endif

#endif // LL_WIN32_LINK_H
