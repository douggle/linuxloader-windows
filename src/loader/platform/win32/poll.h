#ifndef LL_WIN32_POLL_H
#define LL_WIN32_POLL_H

// <poll.h> for MinGW: Winsock's WSAPoll, under the names the arcade sources
// use. A pollfd is the same shape as Linux's, and the readiness bits Linux
// spells POLLIN and POLLOUT are Winsock's POLLRDNORM and POLLWRNORM.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>

#ifndef POLLIN
#define POLLIN POLLRDNORM
#endif
#ifndef POLLOUT
#define POLLOUT POLLWRNORM
#endif
#ifndef POLLPRI
#define POLLPRI POLLRDBAND
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    static inline int poll(struct pollfd *fds, unsigned long nfds, int timeout)
    {
        return WSAPoll((WSAPOLLFD *)fds, nfds, timeout);
    }

#ifdef __cplusplus
}
#endif

#endif // LL_WIN32_POLL_H
