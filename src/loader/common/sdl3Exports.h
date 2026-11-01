#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    // The loader's SDL3 function of that name, or NULL (Windows only).
    void *sdl3Export(const char *name);

    // name=value asks SDL for a Linux-only video/audio driver (to ignore).
    int llIsLinuxOnlySdlDriver(const char *name, const char *value);

    // The handle libSDL3.so.0 opens as: the loader's own SDL3.
#define LL_SDL3_HANDLE ((void *)0x5D130003)

#ifdef __cplusplus
}
#endif
