// The cabinet devices the Linux build reads directly, answered here for the
// games that look for them.  None of it exists on Windows: evdev is Linux's
// input layer, /dev/dsp is OSS, and the interactive controls mapper reads the
// terminal.  The sources that drive those devices (input/evdevInput.c,
// input/controlMapper.c, common/ossDsp.c) are Linux's and are not part of the
// Windows build: what they export is here, saying that there is no such
// device, so the games fall back to SDL's input and no /dev/dsp.

#include <stddef.h>
#include <pthread.h>

#include "loader/input/evdevInput.h"
#include "loader/input/evdevFfb.h"
#include "loader/input/controlMapper.h"
#include "loader/common/webcam.h"
#include "loader/common/ossDsp.h"

// -----------------------------------------------------------------------------
// evdev controllers (input/evdevInput.c)
// -----------------------------------------------------------------------------

// The loader's one set of controllers (input/evdevInput.c has it on Linux),
// which the games' code names from other files.
Controllers controllers;

ControllerStatus initEvdevControllers(Controllers *controllers)
{
    (void)controllers;
    return CONTROLLER_STATUS_ERROR;
}

ControllerStatus loadEvdevControllers(Controllers *controllers)
{
    (void)controllers;
    return CONTROLLER_STATUS_ERROR;
}

ControllerStatus stopEvdevControllers(Controllers *controllers)
{
    (void)controllers;
    return CONTROLLER_STATUS_SUCCESS;
}

Controller *evdevAxisController(Controllers *controllers, const char *name)
{
    (void)controllers;
    (void)name;
    return NULL;
}

void setSdlKeyboardSwitch(int player, int channel, int value)
{
    (void)player;
    (void)channel;
    (void)value;
}

// The loader's own threads run on the pthread emulation either way; there is
// no signal to keep out of them (the game's sigwait() is a Linux one).
int createQuietThread(pthread_t *thread, void *(*start)(void *), void *arg)
{
    return pthread_create(thread, NULL, start, arg);
}

// Force feedback (input/evdevFfb.c's interface) is SDL3's haptics on Windows:
// input/hapticFfb.c.

// -----------------------------------------------------------------------------
// The interactive controls mapper (input/controlMapper.c)
// -----------------------------------------------------------------------------

int runControlMapper(const char *sectionName, const char *controlsPath)
{
    (void)sectionName;
    (void)controlsPath;
    // It waits on the terminal for its answers: not on Windows.
    return 1;
}

// Cameras (common/webcam.c's interface) are emulated on SDL3's on Windows:
// common/webcamWin32.c.

// -----------------------------------------------------------------------------
// /dev/dsp (common/ossDsp.c)
// -----------------------------------------------------------------------------

int ossDspIsPath(const char *path)
{
    (void)path;
    return 0;
}

int ossDspOpen(int flags)
{
    (void)flags;
    return -1;
}

int ossDspIsFd(int fd)
{
    (void)fd;
    return 0;
}

ssize_t ossDspWrite(const void *buf, size_t count)
{
    (void)buf;
    (void)count;
    return -1;
}

int ossDspIoctl(unsigned long request, void *arg)
{
    (void)request;
    (void)arg;
    return -1;
}

void ossDspClose(void)
{
}
