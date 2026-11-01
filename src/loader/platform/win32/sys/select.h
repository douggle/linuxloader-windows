#ifndef LL_WIN32_SYS_SELECT_H
#define LL_WIN32_SYS_SELECT_H

// <sys/select.h> for MinGW: fd_set and select() are Winsock's, with the same
// shape.

#include <sys/socket.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct timeval timeval;

#ifdef __cplusplus
}
#endif

#ifndef FD_SETSIZE
#define FD_SETSIZE 64
#endif

#endif // LL_WIN32_SYS_SELECT_H
