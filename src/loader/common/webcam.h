#ifndef WEBCAM_H
#define WEBCAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Webcams (/dev/videoN) of the Raw Thrills and Namco ES1 games: colour
// cameras only, held open (see webcam.c).
int webcamIsPath(const char *path);
int webcamOpen(const char *path, int flags, int mode, int (*realOpen)(const char *, int, ...));
// A colour camera is there (for the games that can do without one).
int webcamHasCamera(void);
// The game is capturing from this descriptor, and this is one of the picture
// settings the games tune for their cabinets' cameras (see webcam.c).
int webcamIsFd(int fd);
int webcamIsPictureSetting(unsigned int id);

#ifdef _WIN32
// Windows has no V4L2: the cameras are emulated on SDL3's (webcamWin32.c).
// The ioctls on a camera's descriptor, its buffers' mappings (NULL / 0: not
// a camera's), and its close.
int webcamIoctl(int fd, unsigned long request, void *arg);
void *webcamMmap(int fd, size_t length, uint32_t offset);
int webcamMunmap(void *addr);
void webcamClose(int fd);
#endif

#ifdef __cplusplus
}
#endif

#endif // WEBCAM_H
