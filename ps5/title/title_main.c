/* Oogie's Revenge PS5 — the title that boots the recompiled guest (M3).
 *
 * The sequence is upstream port/src/main.c's, minus the Windows halves:
 *
 *   1. load the player's default.xbe from /data/oogie-ps5/game/
 *   2. map the Xbox memory layout (64 MiB pool + mirrors, sections at their
 *      original VAs)
 *   3. bring up the kernel HLE, the path layer and the import bridge
 *   4. point esp at the guest stack and build the flat dispatch table
 *   5. call the guest's own entry point (0x001BEF07)
 *
 * Two things differ from the PC build on purpose:
 *
 *   - the crash handler is a signal handler, not a vectored exception handler:
 *     the console has no SEH, so a fault must be reported by the POSIX route.
 *     It prints the same guest context (registers, guest return addresses).
 *   - no window. The guest's own frames land in guest memory and are executed
 *     by the NV2A push-buffer path; the M4 seam (ps5_video.c) copies them
 *     into a PS5 VideoOut buffer at the flip -- visible the moment the guest
 *     produces one, and a solid colour before that. The D3D11 back end is M5.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <ucontext.h>
#include <sys/stat.h>
#include <sys/mman.h>

/* ---- runtime ------------------------------------------------------------ */
#include "kernel.h"
#include "xbox_memory_layout.h"

/* The generated game entry point (recomp_funcs.h declares the rest). */
extern void xbe_entry_point(void);

/* Registers live in xbox_memory_layout.c, thread-local. */
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __thread uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;

/* Generated dispatch (recomp_dispatch.c). */
extern int recomp_dispatch_init(void);

/* ---- console services --------------------------------------------------- */
extern int sceKernelUsleep(uint32_t microseconds);
/* channel 0 is the standard kernel-log stream; a title's own lines arrive
 * there even when it can write nothing else. */
extern int sceKernelDebugOutText(int channel, const char *text);

#define OOGIE_DATA "/data/oogie-ps5"

/* Where the player's own disc files live. tools/install.py puts the extracted
 * dump in the app folder as Oogie/, which is what the first two are: /app0 is
 * this title's own directory, and the absolute path is the fallback for a
 * loader that does not mount it. The third is a hand-made layout. */
/* The real directory first: /app0 served a stale file after an in-place FTP
 * overwrite, and its directory listing is what the case-insensitive path
 * fallback (patches/path_ci) depends on. */
static const char *const k_game_dirs[] = {
    "/data/homebrew/PPSA90030/Oogie",
    "/app0/Oogie",
    OOGIE_DATA "/game",
};

static const char *find_game_dir(void)
{
    static char path[512];
    for (size_t i = 0; i < sizeof k_game_dirs / sizeof k_game_dirs[0]; i++) {
        snprintf(path, sizeof path, "%s/default.xbe", k_game_dirs[i]);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); return k_game_dirs[i]; }
    }
    return NULL;
}

/* ---- reporting ---------------------------------------------------------- */
/* Every milestone goes to the kernel log *and* a file: the kernel log arrives
 * even when the guest wedges the main thread, and the file survives a reboot. */
static void trace(const char *fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);

    sceKernelDebugOutText(0, line);
    fprintf(stderr, "[Oogie] %s\n", line);

    mkdir(OOGIE_DATA, 0777);
    FILE *f = fopen(OOGIE_DATA "/boot.log", "a");
    if (f) { fprintf(f, "[Oogie] %s\n", line); fclose(f); }
}

static void result(const char *text)
{
    mkdir(OOGIE_DATA, 0777);
    FILE *f = fopen(OOGIE_DATA "/boot-result.txt", "wb");
    if (f) { fprintf(f, "%s\n", text); fclose(f); }
}

[[noreturn]] static void idle(void)
{
    for (;;) sceKernelUsleep(1000000);
}

/* ---- crash reporting ---------------------------------------------------- */
/* Everything the fault path prints goes out through write(2) and the kernel
 * log, never stdio: after a segfault the heap and the stream locks are not
 * things to trust, and evidence lost to a deadlock inside a crash reporter is
 * worse than none. */
static void fault_note(const char *line)
{
    char out[1100];
    int n = snprintf(out, sizeof out, "[Oogie] %s\n", line);
    if (n > 0) write(2, out, (size_t)(n < (int)sizeof out ? n : (int)sizeof out - 1));
    sceKernelDebugOutText(0, out);
}

/* A console has no VEH. The guest is native code, so a fault is an ordinary
 * SIGSEGV/SIGILL/SIGBUS; report the guest view of it and stop, rather than
 * letting the default handler tear the process down with no evidence. */
static void fault_handler(int sig, siginfo_t *info, void *ctx)
{
    /* The PS5 SDK's ucontext is FreeBSD-shaped: the registers are named
     * members of mcontext_t, not an indexed gregs[] array. */
    ucontext_t *uc = (ucontext_t *)ctx;
    uintptr_t rip = 0, rsp = 0;
    unsigned long long syscall_no = 0;
    /* A trapped device register (APU, AC'97) is not a crash: emulate the
     * access and resume after it (platform/ps5_mmio.c). */
    if ((sig == SIGSEGV || sig == SIGBUS) && info) {
        extern int oogie_mmio_fault(void *ucv, uintptr_t fault);
        if (oogie_mmio_fault(ctx, (uintptr_t)info->si_addr))
            return;
    }
    if (uc) {
        rip = (uintptr_t)uc->uc_mcontext.mc_rip;
        rsp = (uintptr_t)uc->uc_mcontext.mc_rsp;
        /* The console rejects a disallowed syscall by raising SIGSYS, and the
         * trap frame still holds the syscall number in RAX. */
        syscall_no = (unsigned long long)uc->uc_mcontext.mc_rax;
    }
    char line[512], summary[256];
    snprintf(summary, sizeof summary, "CRASH signal %d at host rip 0x%llX, fault addr %p, syscall %llu",
             sig, (unsigned long long)rip, info ? info->si_addr : NULL, syscall_no);
    fault_note(summary);

    /* RIP's offset from a known function in our own binary, ASLR-proof: if
     * the fault is inside our image, this is a fixed .text offset that can be
     * looked up in the unstripped build.
     * NOTE: a byte dump around RIP was removed -- on the console itfaults
     * in the handler itself (the faulting page is execute-only), so the
     * register dump below never printed and the kernel report described the
     * handler, not the crash. */
    {
        long rel = (long)(rip - (uintptr_t)(void *)fault_handler);
        snprintf(line, sizeof line, "  rip minus &fault_handler = %ld (0x%lX)", rel, (unsigned long)rel);
        fault_note(line);
    }

    snprintf(line, sizeof line,
             "  guest regs eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X",
             g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp);
    fault_note(line);

    if (g_xbox_mem_offset && g_esp) {
        fault_note("  guest return addresses (innermost first):");
        const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0;
        for (int i = 0; i < 256 && shown < 20; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                snprintf(line, sizeof line, "    [esp+%-4d] 0x%08X", i * 4, v);
                fault_note(line);
                shown++;
            }
        }
        if (!shown) fault_note("    (none in range: the frame is gone)");
    }
    snprintf(line, sizeof line, "  host rsp 0x%llX", (unsigned long long)rsp);
    fault_note(line);
    result(summary);
    idle();
}

static void install_fault_handlers(void)
{
    static char altstack[64 * 1024];
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    /* The title sandbox rejects a disallowed syscall with SIGSYS and kills
     * the process without telling anyone; without a handler there is no
     * evidence at all. */
    sigaction(SIGSYS, &sa, NULL);
}

/* ---- XBE ---------------------------------------------------------------- */
static int load_xbe(const char *path, void **out, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { trace("cannot open %s", path); return 0; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long size = ftell(f);
    if (size <= 0) { fclose(f); return 0; }
    rewind(f);
    void *data = malloc((size_t)size);
    if (!data) { fclose(f); return 0; }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) { free(data); fclose(f); return 0; }
    fclose(f);
    *out = data;
    *out_size = (size_t)size;
    return 1;
}

/* ---- main --------------------------------------------------------------- */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    install_fault_handlers();

    trace("main start (M3: guest boot)");

    /* The port's settings (g_pc). Upstream main.c loads them; without this
     * every field stayed 0, and the camera hook in recomp_manual.c scaled
     * the field of view by fov/100 = 0: an infinite projection, NaN
     * positions, and a white screen in gameplay. */
    {
        extern void pc_settings_load(void);
        pc_settings_load();
    }

    const char *game = find_game_dir();
    extern int oogie_game_setup(int installed);
    if (!oogie_game_setup(game != NULL)) {
        extern void ps5_vk_release_launcher_display(void);
        trace("launcher could not start");
        ps5_vk_release_launcher_display();
        _exit(0);
    }
    game = find_game_dir();
    if (!game) {
        trace("Setup completed without readable game data");
        return 1;
    }

    /* Upstream's default switches: the push-buffer DMA walk and one guest
     * thread at a time by priority. Both are what the PC build ships. */
    if (!getenv("RECOMP_PB_WALK")) setenv("RECOMP_PB_WALK", "dma", 1);
    if (!getenv("RECOMP_GUEST_LOCK")) setenv("RECOMP_GUEST_LOCK", "1", 1);
    const char *guest_lock = getenv("RECOMP_GUEST_LOCK");
    extern int oogie_ps5_d3d_light;
    if (guest_lock && *guest_lock == '1' && access("/app0/test-d3d-light", F_OK) == 0) {
        if (!getenv("OOGIE_FRAME_ORDER_MS"))
            setenv("OOGIE_FRAME_ORDER_MS", "0", 0);
        const char *frame_order = getenv("OOGIE_FRAME_ORDER_MS");
        oogie_ps5_d3d_light = frame_order && strcmp(frame_order, "0") == 0;
        if (oogie_ps5_d3d_light)
            trace("D3D per-call trace and frame-order sampling disabled");
    }

    /* M5: a console title has no shell to set these in, so all gates are
     * on here -- executing the guest's push buffers (without RECOMP_PB_EXEC
     * the scan returns at the door and no GPU work is ever executed),
     * delivering vblank to the guest's D3D8 ISR (without RECOMP_VBLANK the
     * frame loop waits for an interrupt that never fires), and shadowing
     * the KEVENTs the title builds by hand (without RECOMP_TITLE_KEVENTS a
     * raw guest address reached the Win32 wait layer as a HANDLE -- the
     * fault at the event's own address+8). */
    if (!getenv("RECOMP_PB_EXEC")) setenv("RECOMP_PB_EXEC", "1", 1);

    if (!getenv("RECOMP_VBLANK")) setenv("RECOMP_VBLANK", "1", 1);
    if (!getenv("RECOMP_TITLE_KEVENTS")) setenv("RECOMP_TITLE_KEVENTS", "1", 1);

    /* The emulated APU, as the PC build ships it (play.bat): report the
     * AC'97 codec and trap the APU's registers. Without it DirectSound never
     * finishes its DSP handshake; its thread then walks an uninitialised
     * voice list forever while holding DirectSound's global lock, and the
     * main thread blocks on that lock before loading a single file. Needs
     * the x86-64 trap in platform/ps5_mmio.c and its copy of our code. */
    if (!getenv("RECOMP_AC97_READY")) setenv("RECOMP_AC97_READY", "1", 1);
    if (!getenv("RECOMP_HEAP_RECLAIM")) setenv("RECOMP_HEAP_RECLAIM", "1", 1);
    /* The Xbox USB controller model, so the title enumerates a pad (fed by
     * the DualSense in platform/ps5_backend_stub.c). Its registers trap
     * through the same decoder as the APU's. */
    if (!getenv("RECOMP_USB")) setenv("RECOMP_USB", "1", 1);
    /* Host thread budget. The console caps a title's threads, and the game
     * itself starts ~15 workers: with 9 raster workers plus 6 vertex helpers
     * its eighth worker failed (PsCreateSystemThreadEx -> ENOMEM) and the
     * main thread waited on it forever. Rasterising is where the time goes
     * (one thread was 97% busy in the combiners); the vertex helpers buy
     * little on 2D movie quads, so they go and the rasteriser gets 5. */
    if (!getenv("RECOMP_RASTER_THREADS")) setenv("RECOMP_RASTER_THREADS", "6", 1);
    /* A marker under the title data directory enables the 15 s CPU samples. */
    if (!getenv("OOGIE_PROF") &&
        (access(OOGIE_DATA "/profile-cpu", F_OK) == 0 ||
         access("/app0/profile-cpu", F_OK) == 0))
        setenv("OOGIE_PROF", "1", 1);
    if (!getenv("OOGIE_XF_THREADS")) setenv("OOGIE_XF_THREADS", "0", 1);
    {
        extern int oogie_mmio_load_code(const char *bin, const char *map);
        if (!oogie_mmio_load_code("/app0/oogie_code.bin", "/app0/oogie_code.map")) {
            /* No trap: a trapped register would be a crash, so stay on the
             * untrapped configuration. */
            unsetenv("RECOMP_AC97_READY");
            unsetenv("RECOMP_USB");
            trace("MMIO trap unavailable: APU and USB emulation off");
        }
    }

    trace("game data: %s", game);

    void *xbe = NULL;
    size_t xbe_size = 0;
    char xbe_path[512];
    snprintf(xbe_path, sizeof xbe_path, "%s/default.xbe", game);
    if (!load_xbe(xbe_path, &xbe, &xbe_size)) {
        trace("cannot read %s", xbe_path);
        result("FAILED: unreadable default.xbe");
        idle();
    }
    trace("XBE loaded: %zu bytes", xbe_size);

    trace("mapping the Xbox memory layout");
    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        /* The runtime reports through stderr, which the console's kernel log
         * does not relay, so this is the only line that says why. */
        extern DWORD GetLastError(void);
        int err = errno;
        trace("xbox_MemoryLayoutInit failed: GetLastError=%u (%s)",
              (unsigned long)GetLastError(), strerror(err));
        result("FAILED: memory layout");
        free(xbe);
        idle();
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    trace("guest memory mapped, offset 0x%llX", (unsigned long long)g_xbox_mem_offset);
    if (getenv("RECOMP_AC97_READY")) {
        extern void oogie_mmio_arm_ac97(void);
        oogie_mmio_arm_ac97();
    }

    /* Spot-map the top of guest space (guest 0xFFFF0000..0xFFFFFFFF). The
     * PC runtime maps the whole 4GB window (committed on demand), so guest
     * code that computes NULL-minus-small (guest audio walks an empty
     * voice list when the host audio stub reports no device) lands in
     * writable memory there. Our PS5 span is clamped below the contiguous
     * window, so the top page is unmapped VA and faults. 64KB anon RW at
     * host = guest + 0x2000000. */
    {
        unsigned char *top = (unsigned char *)(0xFFFF0000ULL + (unsigned long long)g_xbox_mem_offset);
        errno = 0;
        void *tv = mmap(top, 0x10000u, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        trace("top-page map %p (0x%x errno=%d)", tv, (unsigned)(uintptr_t)tv,
              (int)(uintptr_t)tv == 0xFFFFFFFFFFFFFFFFu ? errno : 0);
    }

    /* Probe the tiled aperture at 0xF0000000 (titles render through it): a
     * guest surface copy read via it and faulted, so check whether the view
     * the runtime mapped is actually readable. sceKernelQueryMemoryProtection
     * answers without touching the page.
     *
     * Opt-in now (OOGIE_TILED_PROBE=1): its "file view" test maps MAP_FIXED
     * over the runtime's real tiled view, and a MAP_FIXED that fails still
     * drops what was there -- with RADV linked the budget is tight, the test
     * failed, and the game faulted at the tiled aperture. Its chunk test also
     * left 54 MB of mappings behind. */
    if (getenv("OOGIE_TILED_PROBE")) {
        extern int sceKernelQueryMemoryProtection(void *addr, void **start, void **end,
                                                  uint32_t *protection);
        void *start, *end;
        uint32_t prot;
        void *tiled = (void *)((uintptr_t)g_xbox_mem_offset + 0xF0000000u);
        int r = sceKernelQueryMemoryProtection(tiled, &start, &end, &prot);
        trace("tiled probe base r=%d [%p-%p] prot=0x%x", r, start, end, prot);
        r = sceKernelQueryMemoryProtection((char *)tiled + 0x6E4000, &start, &end, &prot);
        trace("tiled probe +0x6E4000 r=%d [%p-%p] prot=0x%x", r, start, end, prot);
        /* The runtime's own reserve at this address: succeed = region free,
         * so the file map step is what refuses; fail = something owns it. */
        void *trial = mmap(tiled, 0x4000000u, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        trace("tiled anon reserve %p (0x%x errno=%d)", trial, (unsigned)(uintptr_t)trial,
              (int)(uintptr_t)trial == 0xFFFFFFFFFFFFFFFFu ? errno : 0);
        if (trial != (void *)-1) munmap(trial, 0x4000000u);
        /* The file view itself, offset 0, of a throwaway object: same shape
         * as the runtime's, to name the failing half. */
        {
            int fd = shm_open("/oogie_alias_probe", O_CREAT | O_RDWR | O_EXCL, 0600);
            if (fd >= 0) {
                shm_unlink("/oogie_alias_probe");
                int ftr = ftruncate(fd, 0x4000000u);
                trace("probe ftruncate r=%d errno=%d", ftr,
                      ftr != 0 ? errno : 0);
                void *fv = mmap(tiled, 0x4000000u, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_FIXED, fd, 0);
                trace("tiled file view %p (0x%x errno=%d)", fv,
                      (unsigned)(uintptr_t)fv,
                      (int)(uintptr_t)fv == 0xFFFFFFFFFFFFFFFFu ? errno : 0);
                if (fv != (void *)-1) munmap(fv, 0x4000000u);
                close(fd);
                /* Mirror state: are the layout's mirror views actually
                 * mapped on the console? Queries at mirror m / their bases
                 * (host = 0x2000000 + (m+1)*64MB). */
                const char *mir_names[] = { "ram-base 0x2000000", "mirror0 0x6000000",
                                            "mirror1 0xa000000", "mirror2 0xe000000" };
                const unsigned char *mir_addr[] = { (unsigned char *)0x2000000u,
                                                    (unsigned char *)0x6000000u,
                                                    (unsigned char *)0xa000000u,
                                                    (unsigned char *)0xe000000u };
                for (int mi = 0; mi < 4; mi++) {
                    void *qs, *qe; uint32_t pr;
                    int r = sceKernelQueryMemoryProtection((void *)mir_addr[mi], &qs, &qe, &pr);
                    trace("mirror %s r=%d [%p-%p] prot=0x%x", mir_names[mi], r, qs, qe, pr);
                }
                /* Address-dependence test: map the SAME 64MB shape at a
                 * different free address. If that works, the problem is
                 * the 0xF2000000 site (span/reservation state), not a
                 * global pool. If it also fails, it is global capacity. */
                int fd2 = shm_open("/oogie_alias_probe2", O_CREAT | O_RDWR | O_EXCL, 0600);
                if (fd2 >= 0) {
                    shm_unlink("/oogie_alias_probe2");
                    ftruncate(fd2, 0x4000000u);
                    unsigned char *alt = (unsigned char *)0x10000000u;
                    void *pv = mmap(alt, 0x4000000u, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd2, 0);
                    trace("probe alt addr %p (0x%x errno=%d)", pv, (unsigned)(uintptr_t)pv,
                          (int)(uintptr_t)pv == 0xFFFFFFFFFFFFFFFFu ? errno : 0);
                    if (pv != (void *)-1) munmap(pv, 0x4000000u);
                    /* 64MB outside the span entirely (above every layout
                     * window). Succeeding here means file maps fail only
                     * inside the span's reserved-but-uncommitted region;
                     * failing means a true global budget. */
                    unsigned char *hi = (unsigned char *)0x500000000ull;
                    void *hv = mmap(hi, 0x4000000u, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd2, 0);
                    trace("probe high addr %p (0x%x errno=%d)", hv, (unsigned)(uintptr_t)hv,
                          (int)(uintptr_t)hv == 0xFFFFFFFFFFFFFFFFu ? errno : 0);
                    if (hv != (void *)-1) munmap(hv, 0x4000000u);
                    /* 1MB-chunk capacity: how much remains in total. */
                    unsigned char *run = (unsigned char *)0xF2000000u;
                    int chunks = 0, fail_errno = 0;
                    int fd3 = shm_open("/oogie_alias_probe3", O_CREAT | O_RDWR | O_EXCL, 0600);
                    if (fd3 >= 0) {
                        shm_unlink("/oogie_alias_probe3");
                        ftruncate(fd3, 0x4000000u);
                        while (chunks < 64) {
                            errno = 0;
                            void *cv = mmap(run, 0x100000u, PROT_READ | PROT_WRITE,
                                            MAP_SHARED | MAP_FIXED, fd3, 0);
                            if (cv == (void *)-1) { fail_errno = errno; break; }
                            chunks++;
                            run += 0x100000u;
                        }
                        trace("probe chunks: %d MB ok, final errno=%d", chunks, fail_errno);
                        close(fd3);
                    }
                    close(fd2);
                }
            } else {
                trace("tiled probe: shm_open failed errno=%d", errno);
            }
        }
    }

    if (getenv("RECOMP_AC97_READY")) {
        typedef struct MCPXAPUState MCPXAPUState;
        extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram);
        extern MCPXAPUState *g_apu_state;
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        trace("emulated APU %s", g_apu_state ? "up" : "failed");
    }

    {
        extern void xbox_OhciInit(void);
        xbox_OhciInit();
        trace("USB OHCI init done");
    }

    {
        /* Diagnostic: the recompiled x87 trig calls these (fptan -> tan).
         * Gameplay's projection matrices came out with infinite x/y scale,
         * i.e. cot(fov/2) = inf. Runtime inputs, so nothing is folded. */
        extern double tan(double), sin(double), cos(double), atan2(double, double);
        volatile double h = 0.5, one = 1.0;
        trace("libm check: tan(0.5)=%.9f (0.546302490) sin=%.9f (0.479425539) cos=%.9f (0.877582562) atan2(1,1)=%.9f (0.785398163)",
              tan(h), sin(h), cos(h), atan2(one, one));
    }
    trace("kernel init");
    xbox_kernel_init();

    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        const char *save_dir = OOGIE_DATA "/saves";
        int fd = open(OOGIE_DATA "/saves/Partition1.img", O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            int data_error = errno;
            /* /download0 is the title's writable data mount. Its use keeps
             * the existing save files intact when /data is unavailable. */
            (void)mkdir("/download0/oogie-ps5", 0777);
            (void)mkdir("/download0/oogie-ps5/saves", 0777);
            fd = open("/download0/oogie-ps5/saves/Partition1.img",
                      O_RDWR | O_CREAT, 0644);
            if (fd >= 0) {
                save_dir = "/download0/oogie-ps5/saves";
                trace("using title data mount for saves (/data errno %d)", data_error);
            } else {
                trace("no writable save partition (/data errno %d, /download0 errno %d)",
                      data_error, errno);
            }
        }
        if (fd >= 0)
            close(fd);
        xbox_path_init(game, save_dir);
        trace("paths: game=%s saves=%s", game, save_dir);
    }

    {
        /* The main thread runs the guest's main loop: profile it too. */
        extern void oogie_prof_register(void);
        oogie_prof_register();
    }
    trace("kernel bridge init");
    xbox_kernel_bridge_init();

    g_esp = XBOX_STACK_TOP;
    trace("guest esp 0x%08X", g_esp);

    if (!recomp_dispatch_init())
        trace("flat dispatch unavailable; indirect calls use the binary search");

    {
        extern void xbox_WatchdogStart(void);
        xbox_WatchdogStart();
    }

    /* M4: up goes VideoOut before the guest runs, so the TV shows a frame
     * (a solid colour) even while the guest is stuck before its first flip;
     * the executor's flips take over through FramebufferWindowPresent. */
    {
        extern int ps5_video_init(void);
        int vr = ps5_video_init();
        trace("video init %s (%d)", vr == 0 ? "up" : "FAILED", vr);
    }

    extern void oogie_ps5_affinity(unsigned);
    oogie_ps5_affinity(0);
    trace("calling the guest entry point (0x001BEF07)");
    result("guest entry reached");
    xbe_entry_point();

    trace("the guest returned");
    result("guest returned");
    idle();
}
