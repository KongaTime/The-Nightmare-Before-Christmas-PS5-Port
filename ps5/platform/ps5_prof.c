/*
 * A sampling profiler for the console, through the kernel log.
 *
 * The intro movie ran at a few frames a second and nothing said where the
 * time went. Every thread the runtime starts registers here (the compat
 * layer's thread trampoline, and the main thread from title_main); a sampler
 * thread signals each one about 100 times a second and the handler records
 * the interrupted instruction's offset in our text segment. Every 15 s the
 * hottest offsets (64-byte buckets) and each thread's in-image/outside split
 * go to the kernel log; `llvm-nm -n build/oogie_ps5` names them.
 *
 * Outside-image samples are time in system code: mostly a thread blocked in
 * a wait, which is idle rather than work.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

extern int sceKernelDebugOutText(int channel, const char *text);
extern int oogie_mmio_text_range(uintptr_t *bias, size_t *size);

#define PROF_THREADS 64
#define PROF_BUCKETS 8192

static pthread_t s_threads[PROF_THREADS];
static int s_nthreads;
static int s_ready[PROF_THREADS];
static volatile uint32_t s_in[PROF_THREADS], s_out[PROF_THREADS];
static __thread int s_me = -1;

static volatile uint32_t s_key[PROF_BUCKETS];    /* text offset >> 6, +1 */
static volatile uint32_t s_cnt[PROF_BUCKETS];
static uintptr_t s_bias;
static size_t s_size;

static void prof_sig(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.mc_rip;
    int me = s_me;
    (void)sig; (void)si;
    if (me < 0)
        return;
    if (s_size && rip >= s_bias && rip - s_bias < s_size) {
        uint32_t key = (uint32_t)((rip - s_bias) >> 6) + 1, h = (key * 2654435761u) & (PROF_BUCKETS - 1);
        int probe;
        s_in[me]++;
        for (probe = 0; probe < 32; probe++, h = (h + 1) & (PROF_BUCKETS - 1)) {
            uint32_t k = s_key[h];
            if (k == key) { __sync_fetch_and_add(&s_cnt[h], 1); return; }
            if (k == 0 && __sync_bool_compare_and_swap(&s_key[h], 0, key)) {
                __sync_fetch_and_add(&s_cnt[h], 1);
                return;
            }
        }
    } else {
        s_out[me]++;
    }
}

static void report(int round)
{
    uint32_t keys[24] = {0}, cnts[24] = {0}, total = 0;
    int i, j, n = __atomic_load_n(&s_nthreads, __ATOMIC_RELAXED);
    char line[1600];
    int o;

    for (i = 0; i < PROF_BUCKETS; i++) {
        uint32_t c = s_cnt[i];
        if (!c) continue;
        total += c;
        for (j = 0; j < 24; j++) {
            if (c > cnts[j]) {
                memmove(&cnts[j + 1], &cnts[j], (23 - j) * sizeof cnts[0]);
                memmove(&keys[j + 1], &keys[j], (23 - j) * sizeof keys[0]);
                cnts[j] = c;
                keys[j] = s_key[i];
                break;
            }
        }
    }
    o = snprintf(line, sizeof line, "[PROF] #%d in-image samples %u; top:", round, total);
    for (j = 0; j < 24 && cnts[j] && o < (int)sizeof line - 32; j++)
        o += snprintf(line + o, sizeof line - o, " %x:%u", (keys[j] - 1) << 6, cnts[j]);
    sceKernelDebugOutText(0, line);
    o = snprintf(line, sizeof line, "[PROF] #%d threads in/out:", round);
    for (i = 0; i < n && o < (int)sizeof line - 24; i++)
        o += snprintf(line + o, sizeof line - o, " %d:%u/%u", i, s_in[i], s_out[i]);
    sceKernelDebugOutText(0, line);
    /* Each report covers the 15 s before it. */
    memset((void *)s_cnt, 0, sizeof s_cnt);
    memset((void *)s_key, 0, sizeof s_key);
    memset((void *)s_in, 0, sizeof s_in);
    memset((void *)s_out, 0, sizeof s_out);
}

static void *prof_thread(void *arg)
{
    int tick = 0, round = 0, i;
    (void)arg;
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = prof_sig;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGUSR1, &sa, NULL);
    }
    usleep(2000000);
    oogie_mmio_text_range(&s_bias, &s_size);
    for (;;) {
        int n = __atomic_load_n(&s_nthreads, __ATOMIC_RELAXED);
        /* 30 Hz: the handler's atomic counters were themselves 10% of the
         * samples at 100 Hz, which is time taken from the game. */
        usleep(33333);
        for (i = 0; i < n; i++)
            if (__atomic_load_n(&s_ready[i], __ATOMIC_ACQUIRE))
                pthread_kill(s_threads[i], SIGUSR1);
        if (++tick % 450 == 0) {
            report(++round);
            if (round >= 40)
                return NULL;
        }
    }
}

void oogie_prof_register(void)
{
    static volatile int started;
    int me;

    if (s_me >= 0)
        return;
    /* Off unless asked for: sampling every thread costs a few percent of the
     * frame, which is worth paying only while measuring. OOGIE_PROF=1. */
    if (!getenv("OOGIE_PROF"))
        return;
    do {
        me = __atomic_load_n(&s_nthreads, __ATOMIC_RELAXED);
        if (me >= PROF_THREADS)
            return;
    } while (!__atomic_compare_exchange_n(&s_nthreads, &me, me + 1, 1,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    s_threads[me] = pthread_self();
    s_me = me;
    __atomic_store_n(&s_ready[me], 1, __ATOMIC_RELEASE);
    if (__sync_bool_compare_and_swap(&started, 0, 1)) {
        pthread_t th;
        pthread_create(&th, NULL, prof_thread, NULL);
    }
}
