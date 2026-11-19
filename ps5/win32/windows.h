/* <windows.h> for the PS5 build.
 *
 * xboxrecomp is written Linux-first: src/platform/xbox_winnt.h carries the
 * Win32/NT *type* vocabulary and src/platform/win32_compat.c implements the
 * generic primitives (threads, events, virtual memory, the file API, time,
 * MessageBox/OutputDebugString stubs) on POSIX. The port layer includes
 * <windows.h> and expects both halves, so this header is that pair.
 *
 * It is deliberately thin: types plus the POSIX-backed functions and nothing
 * else. Anything Windows-only that the port still calls (dialogs, the
 * registry, dbghelp, Direct3D) is *not* here on purpose -- a compile error is
 * the census of what the PS5 layer must replace.
 */
#ifndef OOGIE_PS5_WINDOWS_H
#define OOGIE_PS5_WINDOWS_H

#include "platform/xbox_winnt.h"
#include "platform/win32_compat.h"

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

/* The SRW lock, its condition variable and SYSTEM_INFO: used by the
 * runtime's patched sources, absent from win32_compat. */
#include "win32_ps5_extra.h"

/* ---- The handful of Win32 calls the port layer makes that              */
/*      win32_compat.h does not carry ------------------------------------- */
/*                                                                        */
/* Each one is a console-correct answer, not a silent no-op: the port's    */
/* hand-written overrides (recomp_manual.c) call them on paths that decide  */
/* how the guest is scheduled, so an honest cheap implementation beats a   */
/* stub that lies.                                                        */

#include <sched.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>

/* w.xbox_winnt.h already declares SwitchToThread as VOID; the port assigns
 * its result to a BOOL, so provide a real one. */
static inline int OogieSwitchToThread(void)
{
    sched_yield();
    return 1;
}
#define SwitchToThread() OogieSwitchToThread()

/* CPU affinity: the console's scheduler owns placement and a title cannot
 * pin itself to a core. Report success with a single-CPU mask so the port's
 * "shared processor" path stays on. */
static inline BOOL GetProcessAffinityMask(HANDLE process, DWORD_PTR *process_mask,
                                          DWORD_PTR *system_mask)
{
    (void)process;
    if (process_mask) *process_mask = 1;
    if (system_mask) *system_mask = 1;
    return TRUE;
}

static inline DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask)
{
    (void)thread;
    return mask ? mask : 1;
}

/* Interlocked extras (Add/And/Or) the port uses; the base ones are in
 * win32_compat.c. */
static inline LONG InterlockedAdd(volatile LONG *addend, LONG value)
{
    return __sync_add_and_fetch(addend, value);
}

static inline LONG InterlockedAnd(volatile LONG *dest, LONG value)
{
    return __sync_and_and_fetch(dest, value);
}

static inline LONG InterlockedOr(volatile LONG *dest, LONG value)
{
    return __sync_or_and_fetch(dest, value);
}

#define YieldProcessor() __builtin_ia32_pause()
#define MemoryBarrier()  __sync_synchronize()

/* Module lookups: a console title is one image with no loader, so there is no
 * module base and no exported procedure to resolve. NULL is the truthful
 * answer; the port's Wine detection then reads as "not Wine", which it is. */
static inline HMODULE GetModuleHandleA(const char *name)
{
    (void)name;
    return NULL;
}

static inline void *GetProcAddress(HMODULE module, const char *name)
{
    (void)module; (void)name;
    return NULL;
}

/* WaitOnAddress/WakeByAddress: used by the port's task-runner poll (a 1 ms
 * sleep when a task has nothing to do). The console has no futex, and the
 * semantics the port needs are "go to sleep briefly, wake on a change", so
 * this bounds the wait at 1 ms and re-checks. The caller's loop re-tests the
 * value, which is what WaitOnAddress promises. */
static inline BOOL WaitOnAddress(volatile void *address, void *compare,
                                 size_t size, DWORD milliseconds)
{
    if (!address || !compare || size == 0) return TRUE;
    if (memcmp((const void *)address, compare, size) != 0) return TRUE;
    struct timespec ts;
    long ms = (milliseconds == 0xFFFFFFFFu) ? 1 : (long)milliseconds;
    if (ms > 1) ms = 1;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
    return TRUE;
}

static inline void WakeByAddressAll(void *address) { (void)address; }
static inline void WakeByAddressSingle(void *address) { (void)address; }

/* Backtrace capture is diagnostic only; the port prints guest stack words
 * right beside it, and those are the frames that identify the guest function. */
static inline USHORT RtlCaptureStackBackTrace(ULONG skip, ULONG frames_to_skip,
                                              void **back_trace, ULONG *back_trace_hash)
{
    (void)skip; (void)frames_to_skip; (void)back_trace; (void)back_trace_hash;
    return 0;
}

/* CreateDirectoryA: one level, as Win32 promises. */
static inline BOOL CreateDirectoryA(const char *path, void *security)
{
    (void)security;
    return mkdir(path, 0777) == 0 || 1;
}

#endif /* OOGIE_PS5_WINDOWS_H */
