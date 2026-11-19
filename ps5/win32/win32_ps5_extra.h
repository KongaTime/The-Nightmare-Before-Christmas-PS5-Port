/* Win32 pieces the runtime's *patched* sources use that win32_compat does not
 * provide. Kept in the PS5 layer so the xboxrecomp checkout stays pristine
 * (its sources are copied and patched into the build tree, never edited).
 *
 *   SleepConditionVariableSRW
 *       kernel_bridge.c's guest CPU lock is an SRWLOCK; its slow path waits
 *       on a CONDITION_VARIABLE paired with that lock. win32_compat has the
 *       SRW lock (over a pthread rwlock) and the CS-paired wait, but not this
 *       combination — pthread_cond_wait cannot take an rwlock — so it is done
 *       the way Win32 does it: release the lock, wait, take it again.
 *
 *       One caveat that matters: WakeAllConditionVariable signals the
 *       pthread condvar directly under Win32's own bookkeeping, so a wake
 *       that lands between "release" and "enter the wait" cannot be seen.
 *       Rather than risk a waiter that never wakes (the lock is taken
 *       INFINITE on this path), the underlying wait is sliced at
 *       200 ms: a lost wake costs that much latency, never a hang, and the
 *       caller's own deadline is still honoured exactly.
 *
 *   SYSTEM_INFO / GetSystemInfo
 *       Read only for dwNumberOfProcessors (nv2a_pb_exec.c sizes its vertex
 *       helper pool from it).
 */
#ifndef OOGIE_WIN32_PS5_EXTRA_H
#define OOGIE_WIN32_PS5_EXTRA_H

#include "platform/xbox_winnt.h"
#include "platform/win32_compat.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* win32_compat keeps a CONDITION_VARIABLE usable straight from INIT. */
#ifndef CONDITION_VARIABLE_INIT
#define CONDITION_VARIABLE_INIT { 0 }
#endif

static inline pthread_cond_t *oogie_cv_cond(CONDITION_VARIABLE *cv)
{
    if (!cv->Ptr) {
        pthread_cond_t *c = (pthread_cond_t *)calloc(1, sizeof *c);
        if (!c) abort();
        pthread_cond_init(c, NULL);
        cv->Ptr = c;
    }
    return (pthread_cond_t *)cv->Ptr;
}

/* TRUE when woken, FALSE when the caller's deadline expired — Win32's
 * contract, and what the scheduler patch tests to spot a starved waiter. */
static inline BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *lock,
                                            DWORD ms, ULONG flags)
{
    static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t *cond = oogie_cv_cond(cv);
    const BOOL infinite = (ms == 0xFFFFFFFFu);
    struct timespec deadline;
    const long SLICE_MS = 200;
    (void)flags;   /* CONDITION_VARIABLE_LOCKMODE_SHARED: every use is exclusive */

    clock_gettime(CLOCK_REALTIME, &deadline);
    if (!infinite) {
        deadline.tv_sec += ms / 1000;
        deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec += 1; deadline.tv_nsec -= 1000000000L; }
    }

    /* Hold the private mutex across the release so the handoff to the condvar
     * is a single step, not two. */
    pthread_mutex_lock(&wait_mutex);
    ReleaseSRWLockExclusive(lock);

    for (;;) {
        struct timespec slice, now;
        long wait_ms = SLICE_MS;
        clock_gettime(CLOCK_REALTIME, &now);
        if (!infinite) {
            long left = (long)(deadline.tv_sec - now.tv_sec) * 1000
                      + (long)(deadline.tv_nsec - now.tv_nsec) / 1000000;
            if (left <= 0) {                    /* the caller's deadline went by */
                pthread_mutex_unlock(&wait_mutex);
                AcquireSRWLockExclusive(lock);
                return FALSE;
            }
            if (left < wait_ms) wait_ms = left;
        }
        slice.tv_sec = now.tv_sec + wait_ms / 1000;
        slice.tv_nsec = now.tv_nsec + (wait_ms % 1000) * 1000000L;
        if (slice.tv_nsec >= 1000000000L) { slice.tv_sec += 1; slice.tv_nsec -= 1000000000L; }

        int rc = pthread_cond_timedwait(cond, &wait_mutex, &slice);
        if (rc == 0) {                          /* a real wake */
            pthread_mutex_unlock(&wait_mutex);
            AcquireSRWLockExclusive(lock);
            return TRUE;
        }
        /* A timeout end either the caller's (handled above) or one slice's,
         * which is re-checked: the wake may simply have been missed. */
    }
}

/* ---- SYSTEM_INFO ------------------------------------------------------- */

#ifndef _SYSTEM_INFO_DEFINED_
#define _SYSTEM_INFO_DEFINED_
typedef struct _SYSTEM_INFO {
    union { DWORD dwOemId; struct { WORD wProcessorArchitecture; WORD wReserved; }; };
    DWORD     dwPageSize;
    void     *lpMinimumApplicationAddress;
    void     *lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD     dwNumberOfProcessors;
    DWORD     dwProcessorType;
    DWORD     dwAllocationGranularity;
    WORD      wProcessorLevel;
    WORD      wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

/* PS5: a title gets roughly 13 of the 16 hardware threads (7 of 8 Zen 2
 * cores, SMT). The cap was 6 (a PS4-era figure), which left the software
 * rasteriser on dwNumberOfProcessors - 4 = 2 threads and the intro movie at
 * about 5 fps. nv2a_pb_exec sizes its vertex helpers as n - 2 (capped at 6)
 * and its raster pool as n - 4. */
static inline void GetSystemInfo(LPSYSTEM_INFO si)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 12) n = 12;
    si->dwOemId = 0;
    si->dwPageSize = 16384;
    si->lpMinimumApplicationAddress = (void *)0x10000;
    si->lpMaximumApplicationAddress = (void *)0x7FFFFFFFFFFFULL;
    si->dwActiveProcessorMask = (DWORD_PTR)((1u << n) - 1u);
    si->dwNumberOfProcessors = (DWORD)n;
    si->dwProcessorType = 586;
    si->dwAllocationGranularity = 65536;
    si->wProcessorLevel = 6;
    si->wProcessorRevision = 0;
}
#endif

#endif /* OOGIE_WIN32_PS5_EXTRA_H */
