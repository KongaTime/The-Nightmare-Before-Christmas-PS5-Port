/*
 * PS5 trapped-MMIO: the x86-64 POSIX half the runtime does not have.
 *
 * The emulated APU (and the AC'97 bus-master page) answer the guest's
 * hardware registers by faulting on them: the pages are protected, the
 * faulting instruction is decoded, and the access is routed to the device
 * model. Upstream does that on Windows (a VEH and src/platform/mmio_decode.h)
 * and on arm64 POSIX; x86-64 POSIX has no trap, and without the APU
 * DirectSound never completes its DSP handshake -- Oogie's DirectSound thread
 * then walks an uninitialised voice list forever while holding its global
 * lock, and the main thread blocks on that lock before loading anything.
 *
 * The console adds one twist: code pages are execute-only, so the faulting
 * instruction cannot be read where it runs. The build ships a copy of the
 * eboot's text segment (oogie_code.bin) and the address of one anchor
 * function in it (oogie_code.map); the decoder reads the copy instead.
 *
 * The decoder is upstream's, unchanged: CMake writes mmio_decode_ps5.h from
 * src/platform/mmio_decode.h with its Win32 guard opened, and the CONTEXT it
 * expects is mirrored from the FreeBSD-shaped ucontext below.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct {
    uint64_t Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
    uint64_t R8, R9, R10, R11, R12, R13, R14, R15;
    uint64_t Rip;
    uint32_t EFlags;
} OOGIE_CONTEXT, *PCONTEXT;

#include "mmio_decode_ps5.h"

extern int sceKernelDebugOutText(int channel, const char *text);
extern void *xbox_GetMemoryOffset(void);

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *g_apu_state;
uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);
void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size);

#define APU_BASE   0xFE800000u
#define APU_END    0xFE880000u
#define AC97_VA    0xFEC00000u
#define AC97_RR    0x02u

/* The copy stays on disk: the title's flexible memory is tight enough that
 * holding 17 MB of it starved the GPU executor's 8 MB transform buffer. Each
 * trapped access preads the 16 bytes it decodes (pread is a plain syscall,
 * safe in a signal handler). s_code is non-NULL once the copy is usable. */
static int s_code_fd = -1;
static const uint8_t *s_code;      /* non-NULL = trap armed; not dereferenced */
static size_t s_code_size;
static uintptr_t s_code_bias;      /* runtime address of text vaddr 0 */
static uint8_t *s_ac97_page;
static size_t s_ac97_size;
static volatile int s_ac97_lock;
static volatile int s_fail_logs;
static volatile unsigned long s_apu_faults, s_ac97_faults;

/* A build-stamped marker in .text. The map names its vaddr (build.sh reads it
 * with nm); before trusting the copy, the loader preads the copy at that
 * offset and requires these exact bytes. A stale copy -- or a stale map,
 * which /app0 served once after an in-place FTP overwrite -- then fails the
 * check instead of decoding the wrong instructions. The C string below lives
 * in .rodata, a different segment, so it is not in the copy itself. */
#define OOGIE_TEXT_MARK "OOGIE-TEXT-MARK " __DATE__ " " __TIME__
__asm__(".pushsection .text\n"
        ".p2align 4\n"
        ".globl oogie_text_mark\n"
        "oogie_text_mark:\n"
        "ud2\n"
        ".ascii \"" OOGIE_TEXT_MARK "\"\n"
        ".popsection\n");
extern const char oogie_text_mark[];

static void klog(const char *s) { sceKernelDebugOutText(0, s); }

/* ---- the shadow copy of our own code --------------------------------- */
static int open_first(const char *const *paths, int n, const char **used)
{
    int i, fd;
    for (i = 0; i < n; i++) {
        fd = open(paths[i], O_RDONLY);
        if (fd >= 0) { *used = paths[i]; return fd; }
    }
    return -1;
}

int oogie_mmio_load_code(const char *bin_path, const char *map_path)
{
    /* The real directory first: /app0 returned a previous build's map after
     * the file was overwritten in place. */
    const char *maps[] = { "/data/homebrew/PPSA90030/oogie_code.map", map_path };
    const char *bins[] = { "/data/homebrew/PPSA90030/oogie_code.bin", bin_path };
    const char *used_map = "", *used_bin = "";
    static const char want[] = OOGIE_TEXT_MARK;
    char buf[128], line[240], got_mark[sizeof want];
    unsigned long long anchor = 0, size = 0;
    int fd, n;

    fd = open_first(maps, 2, &used_map);
    if (fd < 0) { klog("[MMIO] no code map; trapped MMIO disabled"); return 0; }
    n = (int)read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    if (sscanf(buf, "anchor=%llx size=%llx", &anchor, &size) != 2 || !size) {
        klog("[MMIO] bad code map");
        return 0;
    }
    fd = open_first(bins, 2, &used_bin);
    if (fd < 0) { klog("[MMIO] no code copy; trapped MMIO disabled"); return 0; }
    {
        struct stat st;
        if (fstat(fd, &st) != 0 || (unsigned long long)st.st_size != size) {
            snprintf(line, sizeof line, "[MMIO] code copy size mismatch (map %llu, file %lld) %s",
                     size, (long long)st.st_size, used_bin);
            klog(line);
            close(fd);
            return 0;
        }
    }
    /* ud2 (2 bytes) precedes the stamp. */
    if (pread(fd, got_mark, sizeof want - 1, (off_t)anchor + 2) != (ssize_t)(sizeof want - 1)
            || memcmp(got_mark, want, sizeof want - 1) != 0) {
        got_mark[sizeof want - 1] = 0;
        snprintf(line, sizeof line, "[MMIO] code copy is not this build (want '%s', copy has '%.40s') %s",
                 want, got_mark, used_bin);
        klog(line);
        close(fd);
        return 0;
    }
    s_code_fd = fd;
    s_code = (const uint8_t *)1;
    s_code_size = (size_t)size;
    s_code_bias = (uintptr_t)(const void *)oogie_text_mark - (uintptr_t)anchor;
    snprintf(line, sizeof line, "[MMIO] code copy verified: %llu bytes, bias %p, %s",
             size, (void *)s_code_bias, used_bin);
    klog(line);
    return 1;
}

/* ---- AC'97 bus-master page: writes trap, channel reset completes ------ */
void oogie_mmio_arm_ac97(void)
{
    char line[160];
    long pg = sysconf(_SC_PAGESIZE);

    if (!s_code) return;
    if (pg < 0x1000) pg = 0x1000;
    s_ac97_page = (uint8_t *)xbox_GetMemoryOffset() + AC97_VA;
    s_ac97_size = (size_t)pg;
    if (mprotect(s_ac97_page, s_ac97_size, PROT_READ) != 0) {
        s_ac97_page = NULL;
        klog("[MMIO] AC97 write trap: mprotect failed");
        return;
    }
    snprintf(line, sizeof line, "[MMIO] AC97 write trap armed (%ld-byte page)", pg);
    klog(line);
}

static uint64_t ac97_rd(void *dev, uint32_t off, int size)
{
    uint64_t v = 0;
    memcpy(&v, (char *)dev + off, size > 8 ? 8 : size);
    return v;
}
static void ac97_wr(void *dev, uint32_t off, uint64_t v, int size)
{
    memcpy((char *)dev + off, &v, size > 8 ? 8 : size);
}

/* The model's registers are 32-bit; an 8-byte access is two. */
static uint64_t apu_rd(void *dev, uint32_t off, int size)
{
    if (size == 8) {
        uint64_t lo = mcpx_apu_mmio_read((MCPXAPUState *)dev, off, 4);
        uint64_t hi = mcpx_apu_mmio_read((MCPXAPUState *)dev, off + 4, 4);
        return lo | (hi << 32);
    }
    return mcpx_apu_mmio_read((MCPXAPUState *)dev, off, (unsigned)size);
}
static void apu_wr(void *dev, uint32_t off, uint64_t v, int size)
{
    if (size == 8) {
        mcpx_apu_mmio_write((MCPXAPUState *)dev, off, (uint32_t)v, 4);
        mcpx_apu_mmio_write((MCPXAPUState *)dev, off + 4, (uint32_t)(v >> 32), 4);
        return;
    }
    mcpx_apu_mmio_write((MCPXAPUState *)dev, off, v, (unsigned)size);
}

static void ctx_in(OOGIE_CONTEXT *c, const mcontext_t *m)
{
    c->Rax = m->mc_rax; c->Rcx = m->mc_rcx; c->Rdx = m->mc_rdx; c->Rbx = m->mc_rbx;
    c->Rsp = m->mc_rsp; c->Rbp = m->mc_rbp; c->Rsi = m->mc_rsi; c->Rdi = m->mc_rdi;
    c->R8 = m->mc_r8;   c->R9 = m->mc_r9;   c->R10 = m->mc_r10; c->R11 = m->mc_r11;
    c->R12 = m->mc_r12; c->R13 = m->mc_r13; c->R14 = m->mc_r14; c->R15 = m->mc_r15;
    c->EFlags = (uint32_t)m->mc_rflags;
}
static void ctx_out(const OOGIE_CONTEXT *c, mcontext_t *m)
{
    m->mc_rax = c->Rax; m->mc_rcx = c->Rcx; m->mc_rdx = c->Rdx; m->mc_rbx = c->Rbx;
    m->mc_rbp = c->Rbp; m->mc_rsi = c->Rsi; m->mc_rdi = c->Rdi;
    m->mc_r8 = c->R8;   m->mc_r9 = c->R9;   m->mc_r10 = c->R10; m->mc_r11 = c->R11;
    m->mc_r12 = c->R12; m->mc_r13 = c->R13; m->mc_r14 = c->R14; m->mc_r15 = c->R15;
    m->mc_rflags = (m->mc_rflags & ~0x8C1ull) | (c->EFlags & 0x8C1u);
}

static void decode_fail(const char *what, uintptr_t rip, uint32_t va,
                        const uint8_t *p)
{
    char line[200];
    if (s_fail_logs++ >= 24) return;
    snprintf(line, sizeof line,
             "[MMIO] %s decode FAILED va=%08X rip-text=0x%lx bytes %02X %02X %02X %02X %02X %02X %02X %02X",
             what, va, (unsigned long)(rip - s_code_bias),
             p ? p[0] : 0, p ? p[1] : 0, p ? p[2] : 0, p ? p[3] : 0,
             p ? p[4] : 0, p ? p[5] : 0, p ? p[6] : 0, p ? p[7] : 0);
    klog(line);
}

/* Called first from the title's SIGSEGV/SIGBUS handler. 1: serviced, the
 * thread resumes after the emulated instruction. */
int oogie_mmio_fault(void *ucv, uintptr_t fault)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    uintptr_t base = (uintptr_t)xbox_GetMemoryOffset();
    uintptr_t rip;
    uint64_t rel;
    uint32_t va;
    OOGIE_CONTEXT c;
    uint8_t insn[16];
    const uint8_t *shadow = insn;

    if (!uc || !s_code || fault < base)
        return 0;
    rel = (uint64_t)fault - (uint64_t)base;
    if (rel >= 0x100000000ull)
        return 0;
    va = (uint32_t)rel;
    rip = (uintptr_t)uc->uc_mcontext.mc_rip;
    if (rip < s_code_bias || rip - s_code_bias + 16 > s_code_size)
        return 0;
    if (pread(s_code_fd, insn, sizeof insn, (off_t)(rip - s_code_bias)) != (ssize_t)sizeof insn)
        return 0;

    if (va >= APU_BASE && va < APU_END && g_apu_state) {
        ctx_in(&c, &uc->uc_mcontext);
        c.Rip = (uint64_t)(uintptr_t)shadow;
        if (!mmio_emulate(&c, va - APU_BASE, g_apu_state, apu_rd, apu_wr)) {
            decode_fail("APU", rip, va, insn);
            return 0;
        }
        ctx_out(&c, &uc->uc_mcontext);
        uc->uc_mcontext.mc_rip = rip + (uintptr_t)(c.Rip - (uint64_t)(uintptr_t)shadow);
        s_apu_faults++;
        return 1;
    }

    {
        /* USB OHCI controllers: the pad. */
        extern void *xbox_OhciPs5Device(uint32_t xbox_va, uint32_t *offset);
        extern uint64_t xbox_OhciPs5Read(void *dev, uint32_t off, int size);
        extern void xbox_OhciPs5Write(void *dev, uint32_t off, uint64_t val, int size);
        uint32_t off;
        void *hc = xbox_OhciPs5Device(va, &off);
        if (hc) {
            ctx_in(&c, &uc->uc_mcontext);
            c.Rip = (uint64_t)(uintptr_t)shadow;
            if (!mmio_emulate(&c, off, hc, xbox_OhciPs5Read, xbox_OhciPs5Write)) {
                decode_fail("OHCI", rip, va, insn);
                return 0;
            }
            ctx_out(&c, &uc->uc_mcontext);
            uc->uc_mcontext.mc_rip = rip + (uintptr_t)(c.Rip - (uint64_t)(uintptr_t)shadow);
            return 1;
        }
    }

    if (s_ac97_page && fault >= (uintptr_t)s_ac97_page
            && fault < (uintptr_t)s_ac97_page + s_ac97_size) {
        int ok;
        uint32_t off;
        while (__atomic_exchange_n(&s_ac97_lock, 1, __ATOMIC_ACQUIRE))
            ;
        if (mprotect(s_ac97_page, s_ac97_size, PROT_READ | PROT_WRITE) != 0) {
            __atomic_store_n(&s_ac97_lock, 0, __ATOMIC_RELEASE);
            return 0;
        }
        ctx_in(&c, &uc->uc_mcontext);
        c.Rip = (uint64_t)(uintptr_t)shadow;
        ok = mmio_emulate(&c, (uint32_t)(fault - (uintptr_t)s_ac97_page),
                          s_ac97_page, ac97_rd, ac97_wr);
        if (ok) {
            /* The controller finishes a channel reset at once: RR (bit 1 of
             * each channel's control byte, +0x10B step 0x10) reads back 0. */
            for (off = 0x10B; off < 0x180; off += 0x10)
                if (s_ac97_page[off] & AC97_RR)
                    s_ac97_page[off] = (uint8_t)(s_ac97_page[off] & ~AC97_RR);
        }
        mprotect(s_ac97_page, s_ac97_size, PROT_READ);
        __atomic_store_n(&s_ac97_lock, 0, __ATOMIC_RELEASE);
        if (!ok) {
            decode_fail("AC97", rip, va, insn);
            return 0;
        }
        ctx_out(&c, &uc->uc_mcontext);
        uc->uc_mcontext.mc_rip = rip + (uintptr_t)(c.Rip - (uint64_t)(uintptr_t)shadow);
        s_ac97_faults++;
        return 1;
    }
    return 0;
}

/* Where our text segment is at run time, for the profiler. */
int oogie_mmio_text_range(uintptr_t *bias, size_t *size)
{
    if (!s_code)
        return 0;
    *bias = s_code_bias;
    *size = s_code_size;
    return 1;
}

unsigned long oogie_mmio_apu_faults(void) { return s_apu_faults; }
unsigned long oogie_mmio_ac97_faults(void) { return s_ac97_faults; }
