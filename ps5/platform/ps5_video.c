/* Oogie's Revenge PS5 (M4) - the VideoOut presentation layer.
 *
 * The Xbox framebuffer the executor rasterises into lives in guest RAM, linear
 * X8R8G8B8 (or R5G6B5) at 640x480. VideoOut scans out 1920x1080 buffers in
 * the hardware's tiled layout (64 KiB of 128x128-pixel blocks, tiling mode 0),
 * so presenting is a CPU copy: decode the guest pixels, scale 2x, and write
 * them through the tile swizzle. The swizzle below is PS5_Vulkan's measured
 * SW_64KB_R_X mapping (src/demo_renderer.cpp, tiled_byte_offset), and the
 * buffer allocation/flush/register/flip sequence is its live-target recipe
 * (src/diagnostics.cpp, open_video_target): saturating the first flip with a
 * solid colour is what proves the route before the guest cooperates.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "ps5_video.h"
#include "ps5_vk.h"

/* ---- declarations ---------------------------------------------------------
 * The SDK headers declare none of these but the stubs name every export, and
 * libSceVideoOut is already on the title's link line (see CMakeLists.txt). */

struct ps5_vo_buffer {
    void *data;
    void *metadata;
    void *reserved[2];
};

extern int sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter);
extern int sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
extern void sceVideoOutSetBufferAttribute2(void *attribute, uint64_t pixel_format,
                                           uint32_t tiling, uint32_t width, uint32_t height,
                                           uint64_t reserved1, uint32_t reserved2,
                                           uint64_t reserved3);
extern int sceVideoOutRegisterBuffers2(int32_t handle, int32_t set, int32_t start,
                                       struct ps5_vo_buffer *buffers, int32_t count,
                                       void *attribute, int32_t reserved, void *option);
extern int sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index, uint32_t mode,
                                 int64_t argument);
extern int sceSystemServiceHideSplashScreen(void);

extern int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end,
                                             size_t length, size_t alignment, int memory_type,
                                             int64_t *physical_start);
extern int32_t sceKernelMapDirectMemory(void **address, size_t length, int protection,
                                        int flags, int64_t direct_start, size_t alignment);
extern int64_t sceKernelGetDirectMemorySize(void);

extern int sceKernelDebugOutText(int channel, const char *text);
extern int __real_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter);
extern int sceVideoOutGetOutputStatus(int32_t handle, void *status);

int __wrap_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter)
{
    int handle = __real_sceVideoOutOpen(user, bus, index, parameter);
    if (handle < 0)
        return handle;

    /* Output status starts with 32-bit resolution and dynamic-range fields.
     * Keep the remaining console-owned fields opaque. */
    struct {
        uint32_t resolution, dynamic_range;
        uint64_t refresh_rate, flags;
        uint8_t reserved[232];
    } status = {0};
    int result = sceVideoOutGetOutputStatus(handle, &status);
    char line[160];
    if (result == 0)
        snprintf(line, sizeof line, "[OUTPUTCOLOR] resolution %u dynamic range %u refresh %llu flags 0x%llx\n",
                 status.resolution, status.dynamic_range, (unsigned long long)status.refresh_rate,
                 (unsigned long long)status.flags);
    else
        snprintf(line, sizeof line, "[OUTPUTCOLOR] status query 0x%08x\n", (unsigned)result);
    sceKernelDebugOutText(0, line);
    return handle;
}

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ---- output geometry ------------------------------------------------------ */
#define OUT_W 1920u
#define OUT_H 1080u
#define BUF_BYTES 0x0a00000u   /* over the tiled minimum (135 blocks * 64 KiB), as proven */
#define NBUFFERS 2

/* B8G8R8A8 SDR; a uint32_t guest pixel (0xAARRGGBB, X8R8G8B8) lands in memory
 * as bytes B, G, R, X -- the same order VideoOut scans out. */
#define PIXEL_FORMAT_SDR UINT64_C(0x8000000000000000)
#define DIRECT_TYPE_CPU 12
#define MAP_PROT_RW_GPU 0x33   /* CPU read/write + GPU read/write */
#define ALLOC_ALIGN 0x200000u
#define FLIP_VSYNC 1u          /* mode 0 is refused with 0x80290006 */

/* The solid colour flipped before the guest draws anything: bright green,
 * the most visible thing on a TV. */
#define TEST_COLOUR 0xFF00FF00u

/* The guest's framebuffer, as the runtime's own presenters state it. */
#define GUEST_W 640u
#define GUEST_H 480u
#define GUEST_SCALE 2u
#define GUEST_OX ((OUT_W - GUEST_W * GUEST_SCALE) / 2u)   /* 320 */
#define GUEST_OY ((OUT_H - GUEST_H * GUEST_SCALE) / 2u)   /* 60 */

static int32_t g_video = -1;          /* VideoOut handle, -1 until open */
static void *g_map;                   /* direct-memory mapping holding both buffers */
static void *g_bufs[NBUFFERS];
static int g_next_buf;                /* buffer the next flip writes */
static int64_t g_flip_arg;            /* the flip argument, reported back after each flip */
static int g_ready;                   /* init succeeded */
static int g_failed;                  /* init's failing return, latched */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void vlog(const char *what, long long value)
{
    char line[128];
    snprintf(line, sizeof line, "[Oogie] video: %s = %lld\n", what, value);
    sceKernelDebugOutText(0, line);
    fprintf(stderr, "%s", line);
}

/* CPU and GPU share physical memory; flush the cache lines before the GPU
 * (or the display's scan path) reads what the CPU wrote. */
static void flush_gpu_data(const void *address, size_t bytes)
{
    const uint8_t *at = (const uint8_t *)address;
    const uint8_t *end = at + bytes;
    for (; at < end; at += 64)
        __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

/* Byte offset of pixel (x, y) in the tiled layout VideoOut scans out. */
static size_t tiled_byte_offset(unsigned x, unsigned y)
{
    uint32_t offset = ((y << 4) & 0x70U) ^ ((y << 5) & 0xf00U) ^ ((y << 9) & 0x1000U) ^
                      ((y << 8) & 0x4000U) ^ ((x << 2) & 0xcU) ^ ((x << 5) & 0x380U) ^
                      ((x << 4) & 0x400U) ^ ((x << 6) & 0x800U) ^ ((x << 9) & 0xa000U);
    uint32_t blocks_per_row = (OUT_W + 127U) >> 7;
    uint32_t block_index = (y >> 7) * blocks_per_row + (x >> 7);
    return ((size_t)block_index << 16) + offset;
}

static void fill_buffer(void *buf, uint32_t colour)
{
    uint32_t *pixels = (uint32_t *)buf;
    size_t count = BUF_BYTES / sizeof(uint32_t);
    for (size_t i = 0; i < count; i++)
        pixels[i] = colour;
    flush_gpu_data(buf, BUF_BYTES);
}

int ps5_video_init(void)
{
    int32_t r;
    int64_t direct, phys = -1;
    uint64_t attr[10];   /* PS5_VIDEO_OUT_ATTRIBUTE_BYTES = 80 */
    struct ps5_vo_buffer regs[NBUFFERS];

    pthread_mutex_lock(&g_lock);
    if (g_ready) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (g_failed) {
        int failed = g_failed;
        pthread_mutex_unlock(&g_lock);
        return failed;
    }

    /* M9: Vulkan first. When RADV comes up it owns the display (VK_KHR_display
     * opens VideoOut itself), so the VideoOut path below must not run. */
    if (ps5_vk_init()) {
        sceSystemServiceHideSplashScreen();
        /* M9 step 2: the GPU draws from here on, and presents at each flip. */
        ps5_vk_start_backend();
        g_ready = 1;
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    g_video = sceVideoOutOpen(0xff, 0, 0, NULL);   /* the title's own display */
    vlog("sceVideoOutOpen", g_video);
    if (g_video < 0) {
        g_failed = g_video;
        pthread_mutex_unlock(&g_lock);
        return g_failed;
    }
    r = sceVideoOutSetFlipRate(g_video, 0);       /* a flip may show at every vblank */
    vlog("sceVideoOutSetFlipRate", r);
    if (r != 0) {
        g_failed = r;
        pthread_mutex_unlock(&g_lock);
        return g_failed;
    }

    direct = sceKernelGetDirectMemorySize();
    r = sceKernelAllocateDirectMemory(0, direct, BUF_BYTES * NBUFFERS, ALLOC_ALIGN,
                                      DIRECT_TYPE_CPU, &phys);
    vlog("sceKernelAllocateDirectMemory", r);
    if (r == 0) {
        r = sceKernelMapDirectMemory(&g_map, BUF_BYTES * NBUFFERS, MAP_PROT_RW_GPU, 0, phys,
                                     ALLOC_ALIGN);
        vlog("sceKernelMapDirectMemory", r);
    }
    if (r != 0) {
        g_failed = r;
        pthread_mutex_unlock(&g_lock);
        return g_failed;
    }
    for (int i = 0; i < NBUFFERS; i++)
        g_bufs[i] = (uint8_t *)g_map + (size_t)i * BUF_BYTES;

    for (int i = 0; i < NBUFFERS; i++)
        fill_buffer(g_bufs[i], TEST_COLOUR);

    memset(attr, 0, sizeof attr);
    sceVideoOutSetBufferAttribute2(attr, PIXEL_FORMAT_SDR, 0, OUT_W, OUT_H, 0, 0, 0);
    for (int i = 0; i < NBUFFERS; i++) {
        regs[i].data = g_bufs[i];
        regs[i].metadata = NULL;
        regs[i].reserved[0] = regs[i].reserved[1] = NULL;
    }
    r = sceVideoOutRegisterBuffers2(g_video, 0, 0, regs, NBUFFERS, attr, 0, NULL);
    vlog("sceVideoOutRegisterBuffers2", r);
    if (r != 0) {
        g_failed = r;
        pthread_mutex_unlock(&g_lock);
        return g_failed;
    }

    /* The Shell splash covers the app until this runs; hide it before the
     * first flip so the test colour is what the TV shows. */
    r = sceSystemServiceHideSplashScreen();
    vlog("sceSystemServiceHideSplashScreen", r);

    r = sceVideoOutSubmitFlip(g_video, 0, FLIP_VSYNC, g_flip_arg++);
    vlog("sceVideoOutSubmitFlip(test colour)", r);
    if (r != 0) {
        g_failed = r;
        pthread_mutex_unlock(&g_lock);
        return g_failed;
    }
    g_next_buf = 1;
    g_ready = 1;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int ps5_video_is_ready(void)
{
    return g_ready;
}

int ps5_video_present_guest_frame(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    uint8_t *dst;
    uint32_t bpp;
    int32_t r;
    int idx;
    static int s_presents;

    if (!g_ready || !fb_va || !pitch)
        return 0;
    bpp = pitch / GUEST_W;
    if (bpp != 2 && bpp != 4 && bpp != 8)
        return 0;

    pthread_mutex_lock(&g_lock);
    idx = g_next_buf;
    dst = (uint8_t *)g_bufs[idx];
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    if (ps5_vk_backend_active()) {
        /* The render back end draws on the GPU and presents at the flip;
         * guest memory no longer holds the picture. */
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (ps5_vk_ready()) {
        /* M9: the GPU scales and presents; the CPU only uploads. */
        static struct timespec s_vt300;
        struct timespec now;
        r = ps5_vk_present_guest(src, pitch, bpp);
        if (++s_presents % 300 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (s_vt300.tv_sec) {
                double span = (now.tv_sec - s_vt300.tv_sec) + (now.tv_nsec - s_vt300.tv_nsec) / 1e9;
                char ln[120];
                snprintf(ln, sizeof ln, "[Oogie] fps %.1f over the last 300 presents (Vulkan)\n",
                         300.0 / span);
                sceKernelDebugOutText(0, ln);
            }
            s_vt300 = now;
        }
        pthread_mutex_unlock(&g_lock);
        return r;
    }
    /* Framebuffer probe: is the guest writing pixels at all? Count nonzero
     * bytes across the whole frame in three horizontal bands and name the
     * first 4 bytes. The first 8 presents, then every 300th: per-frame
     * lines filled the 128 KB kernel log within a minute. */
    if (++s_presents <= 8 || s_presents % 300 == 0) {
        unsigned long bands[3] = {0, 0, 0};
        const size_t frame = (size_t)GUEST_W * bpp * GUEST_H;
        for (size_t i = 0; i < frame; i++) {
            if (src[i]) {
                uint32_t row = (uint32_t)(i / ((size_t)GUEST_W * bpp));
                bands[row < GUEST_H / 3 ? 0 : (row < 2 * GUEST_H / 3 ? 1 : 2)]++;
            }
        }
        char line[220];
        snprintf(line, sizeof line,
                 "[Oogie] fb probe #%d va=0x%08X pitch=%u bands=%lu/%lu/%lu first=%02X %02X %02X %02X\n",
                 s_presents, fb_va, pitch, bands[0], bands[1], bands[2],
                 src[0], src[1], src[2], src[3]);
        sceKernelDebugOutText(0, line);
    }

    /* The tiled address splits into an x half and a y half joined by XOR
     * (tiled_byte_offset): the x half of every output column is the same on
     * every row, so it is computed once. Per-pixel address arithmetic for 1.2
     * million writes, and a clflush of the whole 8 MB 1080p buffer, were a
     * large share of the frame; now each row is a table walk and only the
     * 64 KB tiles the 2x box touches are flushed. */
    static uint32_t s_xpart[GUEST_W * GUEST_SCALE], s_xblk[GUEST_W * GUEST_SCALE];
    static int s_xinit;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (!s_xinit) {
        for (uint32_t i = 0; i < GUEST_W * GUEST_SCALE; i++) {
            uint32_t x = GUEST_OX + i;
            s_xpart[i] = ((x << 2) & 0xcU) ^ ((x << 5) & 0x380U) ^
                         ((x << 4) & 0x400U) ^ ((x << 6) & 0x800U) ^ ((x << 9) & 0xa000U);
            s_xblk[i] = (x >> 7) << 16;
        }
        s_xinit = 1;
    }
    for (uint32_t gy = 0; gy < GUEST_H; gy++) {
        const uint8_t *row = src + (size_t)gy * pitch * (bpp == 8 ? 2 : 1);
        uint32_t line[GUEST_W];
        for (uint32_t gx = 0; gx < GUEST_W; gx++) {
            if (bpp == 4) {
                line[gx] = *(const uint32_t *)(row + (size_t)gx * 4);
            } else if (bpp == 8) {
                /* 2x-wide anti-aliased surface: every other pixel. */
                line[gx] = *(const uint32_t *)(row + (size_t)gx * 8);
            } else {   /* bpp == 2, R5G6B5 */
                uint16_t v = *(const uint16_t *)(row + (size_t)gx * 2);
                uint32_t r5 = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g6 = (uint32_t)((v >> 5) & 0x3F) * 255u / 63u;
                uint32_t b5 = (uint32_t)(v & 0x1F) * 255u / 31u;
                line[gx] = (r5 << 16) | (g6 << 8) | b5;
            }
        }
        /* One guest row -> two output rows, each guest pixel two wide. */
        for (uint32_t dy = 0; dy < GUEST_SCALE; dy++) {
            uint32_t y = GUEST_OY + gy * GUEST_SCALE + dy;
            uint32_t ypart = ((y << 4) & 0x70U) ^ ((y << 5) & 0xf00U) ^
                             ((y << 9) & 0x1000U) ^ ((y << 8) & 0x4000U);
            uint8_t *yb = dst + ((size_t)((y >> 7) * ((OUT_W + 127U) >> 7)) << 16);
            for (uint32_t i = 0; i < GUEST_W * GUEST_SCALE; i++)
                *(uint32_t *)(yb + s_xblk[i] + (ypart ^ s_xpart[i])) = line[i / GUEST_SCALE];
        }
    }
    {
        /* Flush the 64 KB tiles (128x128 pixels) the box covers. */
        uint32_t bx0 = GUEST_OX >> 7, bx1 = (GUEST_OX + GUEST_W * GUEST_SCALE - 1) >> 7;
        uint32_t by0 = GUEST_OY >> 7, by1 = (GUEST_OY + GUEST_H * GUEST_SCALE - 1) >> 7;
        uint32_t bpr = (OUT_W + 127U) >> 7;
        for (uint32_t by = by0; by <= by1; by++)
            flush_gpu_data(dst + ((size_t)(by * bpr + bx0) << 16),
                           (size_t)(bx1 - bx0 + 1) << 16);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    {
        /* Real frame rate and the cost of this copy, every 300 presents. */
        static struct timespec s_t300;
        static double s_copy_ms;
        s_copy_ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (s_presents % 300 == 0) {
            if (s_t300.tv_sec) {
                double span = (t1.tv_sec - s_t300.tv_sec) + (t1.tv_nsec - s_t300.tv_nsec) / 1e9;
                char ln[160];
                snprintf(ln, sizeof ln, "[Oogie] fps %.1f over the last 300 presents, copy %.2f ms/frame\n",
                         300.0 / span, s_copy_ms / 300.0);
                sceKernelDebugOutText(0, ln);
            }
            s_t300 = t1;
            s_copy_ms = 0;
        }
    }

    r = sceVideoOutSubmitFlip(g_video, idx, FLIP_VSYNC, g_flip_arg++);
    if (r == 0)
        g_next_buf = idx ^ 1;
    pthread_mutex_unlock(&g_lock);
    return r;
}
