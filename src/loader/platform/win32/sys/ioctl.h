#ifndef LL_WIN32_SYS_IOCTL_H
#define LL_WIN32_SYS_IOCTL_H

// <sys/ioctl.h> for MinGW.  The requests the Linux device code makes are
// Linux's (EVIOCG*, VIDIOC*, SIOCGIF*): ioctl() answers them with an error,
// which is what the device-less Windows build wants.  The macros that build
// them are here so the call sites, and their request numbers, still compile.

#include <sys/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

    int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

// A request number out of Linux's _IOC() scheme (asm-generic/ioctl.h).
#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS 2

#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE 0U
#define _IOC_WRITE 1U
#define _IOC_READ 2U

#define _IOC(dir, type, nr, size) \
    (((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))

#define _IO(type, nr) _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, size) _IOC(_IOC_READ, (type), (nr), sizeof(size))
#define _IOW(type, nr, size) _IOC(_IOC_WRITE, (type), (nr), sizeof(size))
#define _IOWR(type, nr, size) _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(size))

#define _IOC_DIR(nr) (((nr) >> _IOC_DIRSHIFT) & ((1U << _IOC_DIRBITS) - 1))
#define _IOC_TYPE(nr) (((nr) >> _IOC_TYPESHIFT) & ((1U << _IOC_TYPEBITS) - 1))
#define _IOC_NR(nr) (((nr) >> _IOC_NRSHIFT) & ((1U << _IOC_NRBITS) - 1))
#define _IOC_SIZE(nr) (((nr) >> _IOC_SIZESHIFT) & ((1U << _IOC_SIZEBITS) - 1))

// The terminal requests the serial (card reader) code makes, and the socket
// ones its readers ask before reading.
#define TIOCMGET 0x5415
#define TIOCMSET 0x5418
#define TIOCM_DTR 0x002
#define TIOCM_RTS 0x004
#define TIOCM_CTS 0x020
#define TIOCM_CAR 0x040
#define TIOCM_RNG 0x080
#define TIOCM_DSR 0x100

#define FIONREAD 0x541B
#define FIONBIO 0x5421
#define TIOCINQ FIONREAD

#endif // LL_WIN32_SYS_IOCTL_H
