#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    // A Linux function handed to Windows code that will call it back (an SDL3
    // event filter, timer, thread...): the i386 Linux ABI has the stack 16-byte
    // aligned at every call and its code stores SSE values there with movaps,
    // while Windows code keeps 4-byte alignment. This returns a thunk that
    // realigns the stack, passes up to 16 argument words and calls fn - the
    // same thunk for the same fn, so a later Remove call matches. NULL stays
    // NULL.
    //
    // The other way too: a Windows stdcall function (opengl32's) for a Linux
    // caller, which pops the arguments itself. The callee pops the thunk's
    // copy of them, and the thunk returns with the caller's stack as it was.
    void *linuxCallback(void *fn);

    // The Linux function behind a thunk from linuxCallback(), or ptr itself.
    void *linuxCallbackTarget(void *ptr);

#ifdef __cplusplus
}
#endif
