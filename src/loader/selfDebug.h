#ifndef SELF_DEBUG_H
#define SELF_DEBUG_H

#include <stdint.h>

// Self-debugging launcher.
//
// Windows does not let user-mode code touch the debug registers (CR4.DE is
// clear, so "mov %eax,%dr0" raises STATUS_PRIVILEGED_INSTRUCTION), which is
// why the write watchpoint inside the process has to single-step instead and
// only watches its own thread. A debugger can set Dr0-Dr3 on any thread with
// SetThreadContext, so with LL_DEBUG_WATCH set this executable starts a
// second copy of itself under a debugger built into the process and watches
// every thread of it, including a writer on another thread.
//
// The debuggee hands the address to watch to the debugger through a file in
// %TEMP% (selfDebugNoteSlot), because the slot only exists deeper in than
// either of them knows at start-up.

// True when this process is the copy being debugged.
bool selfDebugIsChild(void);

// When LL_DEBUG_WATCH is set and this is not already the debuggee, starts the
// debuggee, runs the debug loop until it exits, and returns true with its
// exit code in *exitCode. False means: run normally, in this process.
bool selfDebugRun(int *exitCode);

// Under the debugger, asks for hardware write watchpoints on two 4-byte slots
// of a frame: its saved EBP and its return address (either may be 0 to skip
// it). Both must be 4-byte aligned and in this process.
// The last two are the calling thread's stack range, so the debugger can tell
// a write that puts a stack address into a watched word from ordinary ones.
void selfDebugNoteSlot(uint32_t savedEbp, uint32_t returnAddress, uint32_t stackTop, uint32_t stackLimit);

#endif // SELF_DEBUG_H
