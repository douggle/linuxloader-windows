// SDL 1.2 linked into the game (Big Buck HD Wild), on Windows.
//
// Its X11 video driver dlsym()s every Xlib function it uses, and the
// loader's Xlib (graphics/x11Bridge.c) is only what the GLX games need: the
// window never opens. So on Windows the game's own SDL entry points go to
// sdl12-compat instead (libSDL-1.2.so.0, on sdl2-compat and the loader's
// SDL3), as Cruis'n Blast's dynamically linked SDL 1.2 does. Its audio
// (SDL_mixer's) stays on the linked-in SDL: SDL_Init, SDL_WasInit, SDL_Quit
// and SDL_GetError serve both, the linked-in SDL's audio included.

#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

#include "rawthrills.h"
#include "../log/log.h"

#if !defined(__linux__)

#include "../elfLoader/linuxCall.hpp"
#include "../redirections/libcShared.h"

#define SDL_INIT_TIMER 0x1
#define SDL_INIT_AUDIO 0x10
#define SDL_INIT_NOPARACHUTE 0x100000
// What the linked-in SDL keeps: its audio, and the timer SDL_mixer uses.
#define SDL_LINKED_FLAGS (SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_NOPARACHUTE)

// Sent to sdl12-compat as they are: they only concern the video, the window
// and its events.
static const char *const compatNames[] = {
    "SDL_SetVideoMode",  "SDL_PollEvent",       "SDL_EnableKeyRepeat", "SDL_ShowCursor",
    "SDL_FreeSurface",   "SDL_GetVideoSurface", "SDL_GetVideoInfo",    "SDL_WM_SetCaption",
    "SDL_GL_SwapBuffers", "SDL_GL_SetAttribute", NULL,
};

// Both SDLs are Linux code: called on a 16-byte aligned stack (linuxCall.hpp).
typedef void *(*LinuxFn)(void *);

static void *compat;
static LinuxFn compatInit, compatWasInit, compatQuit, compatGetError;
static LinuxFn linkedInit, linkedWasInit, linkedQuit, linkedGetError;

static int sdlInit(uint32_t flags)
{
    uint32_t linked = flags & SDL_LINKED_FLAGS, rest = flags & ~(uint32_t)SDL_INIT_AUDIO;
    int ret = 0;

    // The linked-in audio asking for itself: only the linked-in SDL.
    if (!(rest & ~(uint32_t)(SDL_INIT_TIMER | SDL_INIT_NOPARACHUTE)))
        return (int)(intptr_t)callLinuxAligned(linkedInit, (void *)(uintptr_t)linked);
    if (linked & (SDL_INIT_TIMER | SDL_INIT_AUDIO) &&
        (int)(intptr_t)callLinuxAligned(linkedInit, (void *)(uintptr_t)linked) < 0)
        log_warn("Raw Thrills: the game's SDL audio did not start");
    ret = (int)(intptr_t)callLinuxAligned(compatInit, (void *)(uintptr_t)rest);
    if (ret < 0)
        log_error("Raw Thrills: sdl12-compat SDL_Init(%#x) failed: %s", rest,
                  (const char *)callLinuxAligned(compatGetError, NULL));
    return ret;
}

static uint32_t sdlWasInit(uint32_t flags)
{
    return (uint32_t)(uintptr_t)callLinuxAligned(compatWasInit, (void *)(uintptr_t)flags) |
           (uint32_t)(uintptr_t)callLinuxAligned(linkedWasInit, (void *)(uintptr_t)flags);
}

static void sdlQuit(void)
{
    callLinuxAligned0((void (*)(void))compatQuit);
    callLinuxAligned0((void (*)(void))linkedQuit);
}

// sdl12-compat's error, else the linked-in SDL's (its audio's).
static const char *sdlGetError(void)
{
    const char *error = callLinuxAligned(compatGetError, NULL);
    return error && error[0] ? error : (const char *)callLinuxAligned(linkedGetError, NULL);
}

// No X11 display or window to hand out: the game then leaves out the GLX
// extension checks it makes with them.
static int sdlGetWMInfo(void *info)
{
    (void)info;
    return 0;
}

static LinuxFn compatSymbol(const char *name)
{
    void *p = sharedDlsym(compat, name);
    if (!p)
        log_error("Raw Thrills: sdl12-compat has no %s", name);
    return (LinuxFn)p;
}

void rtInstallStaticSdl(const RtGame *game)
{
    if (!game->staticSdl)
        return;
    // The game's lib/, or the loader's ll-deps (with libSDL2-2.0.so.0).
    compat = sharedDlopen("libSDL-1.2.so.0", RTLD_NOW);
    if (!compat || compat == (void *)0xDEADBEEF)
    {
        compat = NULL;
        log_error("Raw Thrills: libSDL-1.2.so.0 (sdl12-compat) not found in ll-deps or the game's lib folder: "
                  "the game's SDL cannot open a window on Windows");
        return;
    }
    if (!(compatInit = compatSymbol("SDL_Init")) || !(compatWasInit = compatSymbol("SDL_WasInit")) ||
        !(compatQuit = compatSymbol("SDL_Quit")) || !(compatGetError = compatSymbol("SDL_GetError")))
    {
        compat = NULL;
        return;
    }
    // Prologues: "push %ebp; mov %esp,%ebp; push %ebx; lea -0x14(%esp),%esp",
    // "push %ebp; mov $0xffff,%eax" and "push %ebp; mov %esp,%ebp;
    // lea -0x18(%esp),%esp" (SDL_Quit, SDL_GetError).
    linkedInit = (LinuxFn)rtTrampoline("SDL_Init", 8);
    linkedWasInit = (LinuxFn)rtTrampoline("SDL_WasInit", 6);
    linkedQuit = (LinuxFn)rtTrampoline("SDL_Quit", 7);
    linkedGetError = (LinuxFn)rtTrampoline("SDL_GetError", 7);
    if (!linkedInit || !linkedWasInit || !linkedQuit || !linkedGetError)
    {
        log_error("Raw Thrills: the game's SDL entry points are not listed");
        compat = NULL;
        return;
    }
    rtDetour("SDL_Init", (void *)sdlInit);
    rtDetour("SDL_WasInit", (void *)sdlWasInit);
    rtDetour("SDL_Quit", (void *)sdlQuit);
    rtDetour("SDL_GetError", (void *)sdlGetError);
    rtDetour("SDL_GetWMInfo", (void *)sdlGetWMInfo);
    for (const char *const *name = compatNames; *name; name++)
    {
        LinuxFn p = compatSymbol(*name);
        if (p)
            rtDetour(*name, (void *)p);
    }
    log_info("Raw Thrills: the game's SDL 1.2 runs on sdl12-compat");
}

void *rtStaticSdlTarget(const char *name)
{
    return compat && name ? sharedDlsym(compat, name) : NULL;
}

#else

void rtInstallStaticSdl(const RtGame *game)
{
    (void)game;
}

void *rtStaticSdlTarget(const char *name)
{
    (void)name;
    return NULL;
}

#endif
