// Threads of the cabinet's games.
//
// The games ran on a machine where their own user had real-time priorities:
// their I/O threads ask for SCHED_FIFO (a dongle poll thread, sound, the
// board's loop). A desktop user has RLIMIT_RTPRIO 0, so pthread_create fails
// with EPERM and the game treats it as fatal -- Cruis'n Blast reports
// "unable to start dongle write thread" and gives up on its dongle.
//
// An explicit real-time policy becomes the inherited one instead: the thread
// runs in the caller's policy, at a normal priority. Nothing in these games
// depends on the priority being granted, only on the thread existing.
//
// The loader is preloaded, so this is what rtFixImports resolves the games'
// pthread_create import to.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>

#include "rawthrills.h"
#include "../log/log.h"

#ifdef __linux__

typedef int (*CreateFn)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    static CreateFn real;
    pthread_attr_t fixed;
    int inherit, policy;

    if (!real && !(real = (CreateFn)dlsym(RTLD_NEXT, "pthread_create")))
    {
        log_error("Raw Thrills: cannot find pthread_create");
        return -1;
    }

    if (attr && pthread_attr_getinheritsched(attr, &inherit) == 0 && inherit == PTHREAD_EXPLICIT_SCHED &&
        pthread_attr_getschedpolicy(attr, &policy) == 0 && policy != SCHED_OTHER)
    {
        log_debug("Raw Thrills: thread asking for scheduling policy %d runs at normal priority", policy);
        fixed = *attr;
        pthread_attr_setinheritsched(&fixed, PTHREAD_INHERIT_SCHED);
        return real(thread, &fixed, start, arg);
    }
    return real(thread, attr, start, arg);
}

#endif /* __linux__ */
