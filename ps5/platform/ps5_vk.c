/*
 * M9: Vulkan on the console (RADV, PS5_Vulkan) -- presentation and the NV2A
 * render back end. docs/M9-VULKAN-PLAN.md has the plan.
 *
 * Step 1 (presentation). The sequence is PS5_Vulkan's radv/radv_smoke.c
 * test_display, which the console has run: vk_icdGetInstanceProcAddr (the
 * archive is linked in, no loader), VK_KHR_display on VideoOut, a display-
 * plane surface, a FIFO swapchain. ps5_vk_present_guest() uploads the guest's
 * 640x480 surface and blits it to the swapchain, scaled on the GPU.
 *
 * Step 2 (this file's second half): a render back end for the runtime's
 * push-buffer executor (nv2a_backend.h), a port of Project Oogie's
 * d3d11_backend.c core. The executor keeps decoding the push buffer and
 * transforming vertices; this draws what it hands over on the GPU:
 *  - every guest colour surface is a VkImage, keyed by its physical address
 *    (d3d11_backend's get_rt); a texture at that address samples the image;
 *  - other textures are decoded with nv2a_backend_decode_texture and cached,
 *    re-uploaded when a hash of their guest bytes changes;
 *  - depth/stencil per zeta address (D32S8: AMD has no D24S8);
 *  - one pipeline per blend state; depth, stencil and culling are Vulkan 1.3
 *    dynamic state; texture stage 0 x diffuse, alpha test, Bink YUV;
 *  - flip presents the game's front buffer (its D3D device names them).
 * Not yet: register combiners (Oogie's half-intensity colours are doubled
 * there: things draw darker until step 3), GPU vertex programs.
 *
 * Everything fails soft: if Vulkan does not come up, ps5_video.c presents
 * through VideoOut and the CPU rasteriser draws, as before.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "ps5_vk.h"
#include <unistd.h>

#include "oogie_flags.h"
#include "pc_settings.h"
#include "launcher_settings.h"

extern int oogie_widescreen_active(void);
#include "ps5_hle.h"

extern int sceKernelDebugOutText(int channel, const char *text);
extern int32_t scePthreadGetaffinity(pthread_t thread, uint64_t *mask);
extern int32_t scePthreadSetaffinity(pthread_t thread, uint64_t mask);

int oogie_ps5_d3d_light;
static int s_d3d_cs_fast;

int oogie_ps5_d3d_cs_fast_enabled(void)
{
    return s_d3d_cs_fast;
}

static pthread_t s_guest_entry;
static int s_guest_entry_valid;
static uint64_t s_guest_cpu_snapshot;

void oogie_ps5_affinity(unsigned poll)
{
    pthread_t self = pthread_self();
    if (!poll) {
        s_guest_entry = self;
        s_d3d_cs_fast = access("/app0/test-d3d-cs-fast", F_OK) == 0;
        __atomic_store_n(&s_guest_entry_valid, 1, __ATOMIC_RELEASE);
    }
    if (access("/app0/test-affinity", F_OK) != 0)
        return;

    uint64_t mask = 0, chosen = 0, actual = 0;
    int status = scePthreadGetaffinity(self, &mask);
    if (status == 0 && mask) {
        unsigned first = (unsigned)__builtin_ctzll(mask);
        /* Keep two logical CPUs apart for both adjacent and split SMT numbering. */
        unsigned second = first + 2;
        while (second < 64 && (!(mask & (UINT64_C(1) << second)) || second == first + 8))
            ++second;
        if (second < 64) {
            chosen = UINT64_C(1) << (poll ? second : first);
            status = scePthreadSetaffinity(self, chosen);
        }
        scePthreadGetaffinity(self, &actual);
    }
    char line[180];
    snprintf(line, sizeof line, "[AFFINITY] %s allowed=%llx wanted=%llx actual=%llx rc=%d\n",
             poll ? "NV2A poll" : "guest entry", (unsigned long long)mask,
             (unsigned long long)chosen, (unsigned long long)actual, status);
    sceKernelDebugOutText(0, line);
}

/* This marker is read once, before the title submits its first frame. */
int oogie_ps5_pb_idle(unsigned idle_ticks)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = access("/app0/test-pb-backoff", F_OK) == 0;
    if (enabled && idle_ticks >= 16) {
        const struct timespec pause = { 0, 25000 };
        nanosleep(&pause, NULL);
        return 1;
    }
    return 0;
}

/* These counters are written by guest threads and drained by the renderer. */
static struct {
    uint64_t wall, cpu, calls;
} s_fence_timing[2];

static uint64_t fence_clock(clockid_t id)
{
    struct timespec now;
    if (clock_gettime(id, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

OogieFenceTiming oogie_fence_begin(unsigned kind)
{
    uint64_t cpu = fence_clock(CLOCK_THREAD_CPUTIME_ID);
    if (kind == 0 && __atomic_load_n(&s_guest_entry_valid, __ATOMIC_ACQUIRE) &&
        pthread_equal(pthread_self(), s_guest_entry))
        __atomic_store_n(&s_guest_cpu_snapshot, cpu, __ATOMIC_RELAXED);
    return (OogieFenceTiming){fence_clock(CLOCK_MONOTONIC), cpu, kind};
}

void oogie_fence_end(OogieFenceTiming *timing)
{
    uint64_t wall = fence_clock(CLOCK_MONOTONIC);
    uint64_t cpu = fence_clock(CLOCK_THREAD_CPUTIME_ID);
    unsigned kind = timing->kind;
    if (timing->wall && wall >= timing->wall)
        __atomic_fetch_add(&s_fence_timing[kind].wall, wall - timing->wall, __ATOMIC_RELAXED);
    if (timing->cpu && cpu >= timing->cpu)
        __atomic_fetch_add(&s_fence_timing[kind].cpu, cpu - timing->cpu, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_fence_timing[kind].calls, 1, __ATOMIC_RELAXED);
}

#if !defined(OOGIE_HAVE_VULKAN)

void oogie_ps5_timed_pb_scan(uint32_t put)
{
    extern void nv2a_pb_scan(uint32_t);
    nv2a_pb_scan(put);
}


int ps5_vk_init(void) { return 0; }
int oogie_vk_filter_available(void) { return 0; }
int oogie_launcher_current_gamma(void) { return 100; }
int ps5_vk_ready(void) { return 0; }
void ps5_vk_release_launcher_display(void) {}
int ps5_vk_backend_active(void) { return 0; }
int ps5_vk_start_backend(void) { return 0; }
int ps5_vk_present_guest(const uint8_t *src, uint32_t pitch, uint32_t bpp)
{
    (void)src; (void)pitch; (void)bpp;
    return -1;
}

#else

#define VK_NO_PROTOTYPES   /* every entry point comes through vk_icdGetInstanceProcAddr */
#include <vulkan/vulkan.h>
#include "nv2a_backend.h"
#include "nv2a_combiner.h"
#include "nv2a_vsh_interp.h"

extern uint64_t oogie_vsh_constant_generation(void);
#include "vk_shaders.h"     /* build/gen: SPIR-V of ps5/shaders/be.vert, be.frag, be_vsh.vert */
#include "oogie_gpu_vsh.h"   /* M9 step 5: a whole vertex-program batch, drawn here */

/* port/patches/backend_extra_stages, backend_vertex_extra_*: the combiner
 * program of the batch, and per-vertex what it reads beyond Nv2aVertex (the
 * layout d3d11_backend.c declares). */
int oogie_backend_combiner(Nv2aCombiner *out, float fog_rgb[3]);
typedef struct {
    float spec[4];
    float fog, q0;
    float tc[3][4];
} OogieVertexExtra;
const OogieVertexExtra *oogie_backend_vertex_extra(void);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
extern ptrdiff_t xbox_GetMemoryOffset(void);
/* port/patches/backend_extra_stages */
int oogie_backend_stage_texture(int stage, Nv2aTexture *out);
int oogie_backend_combiner_stages(void);
void oogie_gpu_vsh_register(OogieVshDrawFn fn);

/* Vertex-program batches this back end drew, and how many vertices they held:
 * the proof that the interpreter left the profile. */
static unsigned long s_vsh_draws, s_vsh_verts, s_vsh_failed;

#define GUEST_W 640u
#define GUEST_H 480u

#define INSTANCE_FNS(X)                                \
    X(EnumeratePhysicalDevices)                        \
    X(GetPhysicalDeviceProperties)                     \
    X(GetPhysicalDeviceFeatures)                       \
    X(GetPhysicalDeviceQueueFamilyProperties)          \
    X(GetPhysicalDeviceMemoryProperties)               \
    X(GetDeviceProcAddr)                               \
    X(CreateDevice)                                    \
    X(GetPhysicalDeviceDisplayPropertiesKHR)           \
    X(GetDisplayModePropertiesKHR)                     \
    X(CreateDisplayPlaneSurfaceKHR)                    \
    X(GetPhysicalDeviceSurfaceSupportKHR)              \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR)

#define DEVICE_FNS(X)                                  \
    X(GetDeviceQueue)                                  \
    X(CreateCommandPool)                               \
    X(AllocateCommandBuffers)                          \
    X(ResetCommandBuffer)                              \
    X(BeginCommandBuffer)                              \
    X(EndCommandBuffer)                                \
    X(QueueSubmit)                                     \
    X(CreateFence)                                     \
    X(ResetFences)                                     \
    X(WaitForFences)                                   \
    X(CreateSemaphore)                                 \
    X(CreateBuffer)                                    \
    X(GetBufferMemoryRequirements)                     \
    X(BindBufferMemory)                                \
    X(CreateImage)                                     \
    X(DestroyBuffer)                                   \
    X(DestroyImage)                                    \
    X(CreateImageView)                                 \
    X(DestroyImageView)                                \
    X(GetImageMemoryRequirements)                      \
    X(BindImageMemory)                                 \
    X(AllocateMemory)                                  \
    X(FreeMemory)                                      \
    X(MapMemory)                                       \
    X(UnmapMemory)                                     \
    X(CreateSampler)                                   \
    X(CreateShaderModule)                              \
    X(CreateDescriptorSetLayout)                       \
    X(CreatePipelineLayout)                            \
    X(DestroyShaderModule)                             \
    X(CreateGraphicsPipelines)                         \
    X(CmdPipelineBarrier)                              \
    X(CmdCopyBufferToImage)                            \
    X(CmdCopyImageToBuffer)                            \
    X(CmdCopyImage)                                    \
    X(CmdBlitImage)                                    \
    X(CmdClearColorImage)                              \
    X(CmdClearDepthStencilImage)                       \
    X(CmdBeginRendering)                               \
    X(CmdEndRendering)                                 \
    X(CmdClearAttachments)                             \
    X(CmdBindPipeline)                                 \
    X(CmdBindVertexBuffers)                            \
    X(CmdBindIndexBuffer)                              \
    X(CmdPushConstants)                                \
    X(CmdPushDescriptorSetKHR)                         \
    X(CmdSetViewport)                                  \
    X(CmdSetScissor)                                   \
    X(CmdSetCullMode)                                  \
    X(CmdSetFrontFace)                                 \
    X(CmdSetDepthTestEnable)                           \
    X(CmdSetDepthWriteEnable)                          \
    X(CmdSetDepthCompareOp)                            \
    X(CmdSetStencilTestEnable)                         \
    X(CmdSetStencilOp)                                 \
    X(CmdSetStencilCompareMask)                        \
    X(CmdSetStencilWriteMask)                          \
    X(CmdSetStencilReference)                          \
    X(CmdSetBlendConstants)                            \
    X(CmdDraw)                                         \
    X(CmdDrawIndexed)                                  \
    X(CreateSwapchainKHR)                              \
    X(GetSwapchainImagesKHR)                           \
    X(AcquireNextImageKHR)                             \
    X(QueuePresentKHR)

#define DECLARE(n) static PFN_vk##n vk##n;
INSTANCE_FNS(DECLARE)
DEVICE_FNS(DECLARE)
#undef DECLARE

static PFN_vkCreateQueryPool vkCreateQueryPool;
static PFN_vkCmdResetQueryPool vkCmdResetQueryPool;
static PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp;
static PFN_vkGetQueryPoolResults vkGetQueryPoolResults;

static struct {
    int ready, failed;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkQueryPool timestamps;
    uint32_t timestamp_bits;
    float timestamp_period;
    int timestamp_active;
    double gpu_ns;
    unsigned gpu_samples;
    VkSemaphore acquired, rendered;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkImage images[8];
    uint32_t image_count;
    VkImageUsageFlags surface_usage;
    VkExtent2D extent;
    int depth_clamp;
    /* step 1: the guest frame on the GPU */
    VkBuffer staging;
    void *staging_map;
    VkImage frame;
    int frame_initialised;
} V;

static void vlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void vlog(const char *fmt, ...)
{
    char line[320];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    __builtin_va_end(ap);
    sceKernelDebugOutText(0, line);
}

void oogie_ps5_trace_gamma(uint32_t address)
{
    static uint8_t previous[3][256];
    static int seen;
    if (address < 0x10000 ||
        (address >= 0x03fffd00 && (address < 0x80010000 || address >= 0x83fffd00)))
        return;
    const uint8_t (*ramp)[256] = (const void *)
        ((const uint8_t *)xbox_GetMemoryOffset() + address);
    if (seen && memcmp(previous, ramp, sizeof previous) == 0)
        return;
    memcpy(previous, ramp, sizeof previous);
    seen = 1;
    unsigned changed = 0;
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned i = 0; i < 256; ++i)
            changed += previous[c][i] != i;
    vlog("[GAMMA] nonidentity=%u R=%u,%u,%u,%u G=%u,%u,%u,%u B=%u,%u,%u,%u\n",
         changed, previous[0][0], previous[0][64], previous[0][128], previous[0][255],
         previous[1][0], previous[1][64], previous[1][128], previous[1][255],
         previous[2][0], previous[2][64], previous[2][128], previous[2][255]);
}

static int fail(const char *what, VkResult r)
{
    vlog("[VK] %s failed (%d); presenting through VideoOut\n", what, (int)r);
    V.failed = 1;
    return 0;
}

static int memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(V.physical, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static int alloc_bind(VkMemoryRequirements req, VkMemoryPropertyFlags want, VkDeviceMemory *out)
{
    int type = memory_type(req.memoryTypeBits, want);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    if (type < 0)
        return 0;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    return vkAllocateMemory(V.device, &ai, NULL, out) == VK_SUCCESS;
}

/* One barrier, conservative stages and access: correct first, fast later. */
static void barrier_aspect(VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(V.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void barrier(VkImage image, VkImageLayout from, VkImageLayout to)
{
    barrier_aspect(image, VK_IMAGE_ASPECT_COLOR_BIT, from, to);
}

static float anisotropy_limit;
int ps5_vk_ready(void) { return V.ready; }

void ps5_vk_release_launcher_display(void)
{
    if (!V.swapchain) return;
    PFN_vkDestroySwapchainKHR destroy =
        (PFN_vkDestroySwapchainKHR)vkGetDeviceProcAddr(V.device, "vkDestroySwapchainKHR");
    if (destroy) {
        /* The PS5 driver drains pending flips and returns VideoOut here. */
        destroy(V.device, V.swapchain, NULL);
        V.swapchain = VK_NULL_HANDLE;
        V.ready = 0;
    }
}
int oogie_vk_filter_available(void) { return anisotropy_limit >= 2.0f; }

int ps5_vk_init(void)
{
    PFN_vkCreateInstance create_instance;
    VkResult r;
    uint32_t n;

    if (V.ready)
        return 1;
    if (V.failed)
        return 0;

    create_instance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create_instance)
        return fail("vk_icdGetInstanceProcAddr(vkCreateInstance)", VK_ERROR_INITIALIZATION_FAILED);
    {
        const char *exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME };
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        app.pApplicationName = "Oogie's Revenge";
        app.apiVersion = VK_API_VERSION_1_3;
        ii.pApplicationInfo = &app;
        ii.enabledExtensionCount = 2;
        ii.ppEnabledExtensionNames = exts;
        if ((r = create_instance(&ii, NULL, &V.instance)) != VK_SUCCESS)
            return fail("vkCreateInstance", r);
    }
#define LOAD_I(nm) if (!(vk##nm = (PFN_vk##nm)vk_icdGetInstanceProcAddr(V.instance, "vk" #nm))) return fail("load vk" #nm, VK_ERROR_INITIALIZATION_FAILED);
    INSTANCE_FNS(LOAD_I)
#undef LOAD_I
    n = 1;
    r = vkEnumeratePhysicalDevices(V.instance, &n, &V.physical);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0)
        return fail("vkEnumeratePhysicalDevices", r);
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(V.physical, &p);
        V.timestamp_period = p.limits.timestampPeriod;
        vlog("[VK] device: %s, Vulkan %u.%u\n", p.deviceName,
             VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
    }
    {
        VkQueueFamilyProperties fam[8];
        uint32_t i;
        n = 8;
        vkGetPhysicalDeviceQueueFamilyProperties(V.physical, &n, fam);
        V.family = UINT32_MAX;
        for (i = 0; i < n; i++)
            if (fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { V.family = i; break; }
        if (V.family == UINT32_MAX)
            return fail("a graphics queue", VK_ERROR_INITIALIZATION_FAILED);
        V.timestamp_bits = fam[V.family].timestampValidBits;
    }
    {
        float prio = 1.0f;
        const char *dexts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
        VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceFeatures supported, want;
        vkGetPhysicalDeviceFeatures(V.physical, &supported);
        memset(&want, 0, sizeof want);
        want.depthClamp = supported.depthClamp;
        want.samplerAnisotropy = supported.samplerAnisotropy;
        if (want.samplerAnisotropy) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(V.physical, &properties);
            anisotropy_limit = properties.limits.maxSamplerAnisotropy;
        }
        V.depth_clamp = supported.depthClamp == VK_TRUE;
        f13.dynamicRendering = VK_TRUE;
        qi.queueFamilyIndex = V.family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;
        di.pNext = &f13;
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = 2;
        di.ppEnabledExtensionNames = dexts;
        di.pEnabledFeatures = &want;
        if ((r = vkCreateDevice(V.physical, &di, NULL, &V.device)) != VK_SUCCESS)
            return fail("vkCreateDevice", r);
    }
#define LOAD_D(nm) if (!(vk##nm = (PFN_vk##nm)vkGetDeviceProcAddr(V.device, "vk" #nm))) return fail("load vk" #nm, VK_ERROR_INITIALIZATION_FAILED);
    DEVICE_FNS(LOAD_D)
#undef LOAD_D
    vkGetDeviceQueue(V.device, V.family, 0, &V.queue);
#define LOAD_QUERY(nm) vk##nm = (PFN_vk##nm)vkGetDeviceProcAddr(V.device, "vk" #nm)
    LOAD_QUERY(CreateQueryPool);
    LOAD_QUERY(CmdResetQueryPool);
    LOAD_QUERY(CmdWriteTimestamp);
    LOAD_QUERY(GetQueryPoolResults);
#undef LOAD_QUERY
    if (V.timestamp_bits && vkCreateQueryPool && vkCmdResetQueryPool &&
        vkCmdWriteTimestamp && vkGetQueryPoolResults) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2;
        if (vkCreateQueryPool(V.device, &qi, NULL, &V.timestamps) != VK_SUCCESS)
            V.timestamps = VK_NULL_HANDLE;
    }
    vlog("[VKGPU] execution timing %s (%u timestamp bits)\n",
         V.timestamps ? "enabled" : "unavailable", V.timestamp_bits);

    {
        VkDisplayPropertiesKHR disp;
        VkDisplayModePropertiesKHR modes[4];
        VkDisplaySurfaceCreateInfoKHR si = { VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR };
        VkSurfaceCapabilitiesKHR caps = {0};
        VkBool32 ok = VK_FALSE;
        n = 1;
        if ((r = vkGetPhysicalDeviceDisplayPropertiesKHR(V.physical, &n, &disp)) != VK_SUCCESS || n == 0)
            return fail("vkGetPhysicalDeviceDisplayPropertiesKHR", r);
        n = 4;
        if ((r = vkGetDisplayModePropertiesKHR(V.physical, disp.display, &n, modes)) != VK_SUCCESS || n == 0)
            return fail("vkGetDisplayModePropertiesKHR", r);
        si.displayMode = modes[0].displayMode;
        si.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        si.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
        si.imageExtent = disp.physicalResolution;
        if ((r = vkCreateDisplayPlaneSurfaceKHR(V.instance, &si, NULL, &V.surface)) != VK_SUCCESS)
            return fail("vkCreateDisplayPlaneSurfaceKHR", r);
        vkGetPhysicalDeviceSurfaceSupportKHR(V.physical, V.family, V.surface, &ok);
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.physical, V.surface, &caps);
        if (!ok)
            return fail("surface support", VK_ERROR_SURFACE_LOST_KHR);
        V.surface_usage = caps.supportedUsageFlags;
        V.extent = caps.currentExtent.width ? caps.currentExtent : disp.physicalResolution;
        vlog("[VK] display %ux%u, %.2f Hz\n", V.extent.width, V.extent.height,
             modes[0].parameters.refreshRate / 1000.0);
    }
    {
        VkSwapchainCreateInfoKHR sc = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        sc.surface = V.surface;
        sc.minImageCount = 3;
        sc.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        sc.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        sc.imageExtent = V.extent;
        sc.imageArrayLayers = 1;
        sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sc.imageUsage |= V.surface_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        vlog("[COLORFRAME] swapchain usage 0x%x (surface 0x%x)\n",
             (unsigned)sc.imageUsage, (unsigned)V.surface_usage);
        sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sc.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sc.clipped = VK_TRUE;
        if ((r = vkCreateSwapchainKHR(V.device, &sc, NULL, &V.swapchain)) != VK_SUCCESS)
            return fail("vkCreateSwapchainKHR", r);
        V.image_count = 8;
        if ((r = vkGetSwapchainImagesKHR(V.device, V.swapchain, &V.image_count, V.images)) != VK_SUCCESS)
            return fail("vkGetSwapchainImagesKHR", r);
    }
    {
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo ci = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSemaphoreCreateInfo s = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = V.family;
        if ((r = vkCreateCommandPool(V.device, &pi, NULL, &V.pool)) != VK_SUCCESS)
            return fail("vkCreateCommandPool", r);
        ci.commandPool = V.pool;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        if ((r = vkAllocateCommandBuffers(V.device, &ci, &V.cmd)) != VK_SUCCESS)
            return fail("vkAllocateCommandBuffers", r);
        if (vkCreateFence(V.device, &fi, NULL, &V.fence) != VK_SUCCESS
                || vkCreateSemaphore(V.device, &s, NULL, &V.acquired) != VK_SUCCESS
                || vkCreateSemaphore(V.device, &s, NULL, &V.rendered) != VK_SUCCESS)
            return fail("fence/semaphores", VK_ERROR_INITIALIZATION_FAILED);
    }
    {
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        VkMemoryRequirements req;
        VkDeviceMemory mem;
        bi.size = (VkDeviceSize)GUEST_W * GUEST_H * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if ((r = vkCreateBuffer(V.device, &bi, NULL, &V.staging)) != VK_SUCCESS)
            return fail("staging buffer", r);
        vkGetBufferMemoryRequirements(V.device, V.staging, &req);
        if (!alloc_bind(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &mem)
                || vkBindBufferMemory(V.device, V.staging, mem, 0) != VK_SUCCESS
                || vkMapMemory(V.device, mem, 0, VK_WHOLE_SIZE, 0, &V.staging_map) != VK_SUCCESS)
            return fail("staging memory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_B8G8R8A8_UNORM;
        ii.extent.width = GUEST_W;
        ii.extent.height = GUEST_H;
        ii.extent.depth = 1;
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if ((r = vkCreateImage(V.device, &ii, NULL, &V.frame)) != VK_SUCCESS)
            return fail("frame image", r);
        vkGetImageMemoryRequirements(V.device, V.frame, &req);
        if (!alloc_bind(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mem)
                || vkBindImageMemory(V.device, V.frame, mem, 0) != VK_SUCCESS)
            return fail("frame image memory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    }
    V.ready = 1;
    vlog("[VK] ready: %u swapchain images, depth clamp %s\n", V.image_count,
         V.depth_clamp ? "on" : "off");
    return 1;
}

/* Blit `src` (in TRANSFER_SRC layout, w x h) to an acquired swapchain image,
 * 4:3 centred on black, and present. Records into V.cmd, which must be open;
 * ends, submits and waits for it. */
typedef struct {
    uint64_t acquire, submit, present, fence;
} PresentTimings;

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static uint64_t s_scan_ns, s_scan_started;
static unsigned s_scan_count;

/* Called on the NV2A polling thread. The flip may occur inside a scan. */
void oogie_ps5_timed_pb_scan(uint32_t put)
{
    extern void nv2a_pb_scan(uint32_t);
    s_scan_started = monotonic_ns();
    nv2a_pb_scan(put);
    s_scan_ns += monotonic_ns() - s_scan_started;
    s_scan_started = 0;
    ++s_scan_count;
}

static void timestamp_end(void)
{
    if (V.timestamp_active)
        vkCmdWriteTimestamp(V.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, V.timestamps, 1);
}

static void timestamp_read(void)
{
    if (!V.timestamp_active)
        return;
    V.timestamp_active = 0;
    uint64_t times[2];
    if (vkGetQueryPoolResults(V.device, V.timestamps, 0, 2, sizeof times, times,
                              sizeof times[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    uint64_t ticks = times[1] - times[0];
    if (V.timestamp_bits < 64)
        ticks &= (UINT64_C(1) << V.timestamp_bits) - 1;
    V.gpu_ns += (double)ticks * V.timestamp_period;
    ++V.gpu_samples;
}

typedef struct {
    VkImage img;
    VkImageView view;
    VkDeviceMemory mem;
    VkImageLayout layout;
    uint32_t w, h;
    uint64_t guest_content_hash;
} Img;

static int img_create(Img *im, uint32_t w, uint32_t h, VkFormat fmt,
                      VkImageUsageFlags usage, VkImageAspectFlags aspect);
static void img_destroy(Img *im);
static void img_to(Img *im, VkImageAspectFlags aspect, VkImageLayout to);

enum { FRAME_WIDTH = 96, FRAME_HEIGHT = 72, FRAME_STRIP = 24 };
typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    Img thumbnail;
    int recorded;
} ColorFrame;

static int color_frame_request(unsigned flips)
{
    static int last_token;
    if (flips % 30)
        return 0;
    FILE *marker = fopen("/app0/color-capture", "r");
    if (!marker)
        return 0;
    int token = fgetc(marker);
    fclose(marker);
    if (((token < 'A' || token > 'Z') && (token < 'a' || token > 'z')) ||
        token == last_token)
        return 0;
    last_token = token;

    return token;
}

static void record_color_frame(ColorFrame *frame, VkImage image, const VkOffset3D offsets[2])
{
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements requirements;
    info.size = FRAME_WIDTH * FRAME_HEIGHT * 4;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(V.device, &info, NULL, &frame->buffer) != VK_SUCCESS)
        return;
    vkGetBufferMemoryRequirements(V.device, frame->buffer, &requirements);
    if (!alloc_bind(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &frame->memory) ||
        vkBindBufferMemory(V.device, frame->buffer, frame->memory, 0) != VK_SUCCESS ||
        vkMapMemory(V.device, frame->memory, 0, VK_WHOLE_SIZE, 0, &frame->mapped) != VK_SUCCESS ||
        !img_create(&frame->thumbnail, FRAME_WIDTH, FRAME_HEIGHT, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return;

    VkImageBlit blit = {0};
    VkBufferImageCopy copy = {0};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[0] = offsets[0];
    blit.srcOffsets[1] = offsets[1];
    blit.dstOffsets[1] = (VkOffset3D) { FRAME_WIDTH, FRAME_HEIGHT, 1 };
    img_to(&frame->thumbnail, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdBlitImage(V.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   frame->thumbnail.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    img_to(&frame->thumbnail, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    copy.imageSubresource = blit.dstSubresource;
    copy.imageExtent = (VkExtent3D) { FRAME_WIDTH, FRAME_HEIGHT, 1 };
    vkCmdCopyImageToBuffer(V.cmd, frame->thumbnail.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           frame->buffer, 1, &copy);
    frame->recorded = 1;
}

static void finish_color_frame(ColorFrame *frame, int token, int ready)
{
    if (ready && frame->recorded) {
        static const char hex[] = "0123456789abcdef";
        const uint8_t *pixels = frame->mapped;
        for (unsigned y = 0; y < FRAME_HEIGHT; ++y) {
            for (unsigned x = 0; x < FRAME_WIDTH; x += FRAME_STRIP) {
                char encoded[FRAME_STRIP * 6 + 1];
                for (unsigned i = 0; i < FRAME_STRIP * 3; ++i) {
                    uint8_t value = pixels[(y * FRAME_WIDTH + x + i / 3) * 4 + 2 - i % 3];
                    encoded[2 * i] = hex[value >> 4];
                    encoded[2 * i + 1] = hex[value & 15];
                }
                encoded[sizeof encoded - 1] = 0;
                vlog("[COLORFRAME] %c %u %u %s\n", token, y, x, encoded);
            }
        }
        vlog("[COLORFRAME] end %c\n", token);
    }
    img_destroy(&frame->thumbnail);
    if (frame->mapped)
        vkUnmapMemory(V.device, frame->memory);
    if (frame->buffer)
        vkDestroyBuffer(V.device, frame->buffer, NULL);
    if (frame->memory)
        vkFreeMemory(V.device, frame->memory, NULL);
}

/* Compare corresponding source and swapchain pixels in the same frame.
 * The output uses linear filtering, so exact equality is expected only at
 * locations where source texel centers align with output pixel centers. */
enum { PRESENT_SAMPLE_GRID = 8, PRESENT_SAMPLE_COUNT = PRESENT_SAMPLE_GRID * PRESENT_SAMPLE_GRID };
typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint32_t *pixels;
} PresentSample;

static void record_present_sample(PresentSample *sample, VkImage source, VkImage output,
                                  const VkImageBlit *blit)
{
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = 2 * PRESENT_SAMPLE_COUNT * sizeof(uint32_t);
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(V.device, &info, NULL, &sample->buffer) != VK_SUCCESS)
        return;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(V.device, sample->buffer, &req);
    if (!alloc_bind(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &sample->memory) ||
        vkBindBufferMemory(V.device, sample->buffer, sample->memory, 0) != VK_SUCCESS ||
        vkMapMemory(V.device, sample->memory, 0, VK_WHOLE_SIZE, 0, (void **)&sample->pixels) != VK_SUCCESS)
        return;

    VkBufferImageCopy regions[PRESENT_SAMPLE_COUNT];
    for (unsigned image = 0; image < 2; ++image) {
        const VkOffset3D *rect = image ? blit->dstOffsets : blit->srcOffsets;
        for (unsigned i = 0; i < PRESENT_SAMPLE_COUNT; ++i) {
            int x = 2 * (i % PRESENT_SAMPLE_GRID) + 1;
            int y = 2 * (i / PRESENT_SAMPLE_GRID) + 1;
            regions[i] = (VkBufferImageCopy) {
                .bufferOffset = (image * PRESENT_SAMPLE_COUNT + i) * sizeof(uint32_t),
                .imageSubresource = blit->srcSubresource,
                .imageOffset = {
                    rect[0].x + x * (rect[1].x - rect[0].x) / (2 * PRESENT_SAMPLE_GRID),
                    rect[0].y + y * (rect[1].y - rect[0].y) / (2 * PRESENT_SAMPLE_GRID),
                    0
                },
                .imageExtent = {1, 1, 1}
            };
        }
        vkCmdCopyImageToBuffer(V.cmd, image ? output : source,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sample->buffer,
                               PRESENT_SAMPLE_COUNT, regions);
    }
}

static void finish_present_sample(PresentSample *sample, int ready)
{
    if (ready && sample->pixels) {
        unsigned src[3] = {0}, dst[3] = {0};
        for (unsigned i = 0; i < PRESENT_SAMPLE_COUNT; ++i) {
            for (unsigned c = 0; c < 3; ++c) {
                src[c] += (sample->pixels[i] >> (8 * (2 - c))) & 255;
                dst[c] += (sample->pixels[PRESENT_SAMPLE_COUNT + i] >> (8 * (2 - c))) & 255;
            }
        }
        vlog("[PRESENTCOLOR] mean RGB source %.2f %.2f %.2f output %.2f %.2f %.2f\n",
             src[0] / (double)PRESENT_SAMPLE_COUNT, src[1] / (double)PRESENT_SAMPLE_COUNT,
             src[2] / (double)PRESENT_SAMPLE_COUNT, dst[0] / (double)PRESENT_SAMPLE_COUNT,
             dst[1] / (double)PRESENT_SAMPLE_COUNT, dst[2] / (double)PRESENT_SAMPLE_COUNT);
    }
    if (sample->pixels)
        vkUnmapMemory(V.device, sample->memory);
    if (sample->buffer)
        vkDestroyBuffer(V.device, sample->buffer, NULL);
    if (sample->memory)
        vkFreeMemory(V.device, sample->memory, NULL);
}

/* The diagnostic override is optional and is read while the title is running. */
static float present_warmth, present_cool_reduction;

static float present_gamma(void)
{
    static float gamma = 1.0f;
    static float configured = 1.0f;
    static unsigned frames;
    if (!frames) {
        const char *value = pc_settings_get("Graphics", "presentation_gamma");
        if (value) {
            char *end;
            float parsed = strtof(value, &end);
            if (*end == '\0' && parsed >= 1.0f && parsed <= 2.4f)
                configured = parsed;
        }
    }
    if (frames++ % 30 == 0) {
        float next = configured, warmth = 0.0f, cool_reduction = 0.0f;
        FILE *marker = fopen("/app0/present-gamma", "r");
        if (marker) {
            int fields = fscanf(marker, "%f %f %f", &next, &warmth, &cool_reduction);
            if (fields < 1 || !(next >= 1.0f && next <= 2.4f))
                next = configured;
            if (fields < 2 || !(warmth >= 0.0f && warmth <= 0.25f))
                warmth = 0.0f;
            if (fields < 3 || !(cool_reduction >= 0.0f && cool_reduction <= 0.35f))
                cool_reduction = 0.0f;
            fclose(marker);
        }
        if (oogie_launcher_settings.gamma)
            next = oogie_launcher_settings.gamma / 100.0f;
        if (next != gamma || warmth != present_warmth || cool_reduction != present_cool_reduction)
            vlog("[PRESENTGAMMA] %.2f, warm hue %.2f, cool reduction %.2f\n",
                 next, warmth, cool_reduction);
        gamma = next;
        present_warmth = warmth;
        present_cool_reduction = cool_reduction;
    }
    return gamma;
}

int oogie_launcher_current_gamma(void)
{
    return (int)(present_gamma() * 100.0f + 0.5f);
}

static VkDescriptorSetLayout gamma_set;
static VkPipelineLayout gamma_layout;
static VkPipeline gamma_pipeline;
static VkSampler gamma_sampler;
static VkImageView gamma_views[8];

static int init_gamma_pipeline(void)
{
    const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                            VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo set = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    set.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    set.bindingCount = 1;
    set.pBindings = &binding;
    VkPushConstantRange range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 5 * sizeof(float) };
    VkPipelineLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &gamma_set;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateDescriptorSetLayout(V.device, &set, NULL, &gamma_set) != VK_SUCCESS ||
        vkCreatePipelineLayout(V.device, &layout, NULL, &gamma_layout) != VK_SUCCESS ||
        vkCreateSampler(V.device, &sampler, NULL, &gamma_sampler) != VK_SUCCESS)
        return 0;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
          VK_SHADER_STAGE_VERTEX_BIT, VK_NULL_HANDLE, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
          VK_SHADER_STAGE_FRAGMENT_BIT, VK_NULL_HANDLE, "main", NULL }
    };
    VkShaderModuleCreateInfo shader = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    shader.codeSize = sizeof spv_present_vert;
    shader.pCode = spv_present_vert;
    if (vkCreateShaderModule(V.device, &shader, NULL, &stages[0].module) != VK_SUCCESS)
        return 0;
    shader.codeSize = sizeof spv_present_frag;
    shader.pCode = spv_present_frag;
    VkResult result = vkCreateShaderModule(V.device, &shader, NULL, &stages[1].module);
    if (result == VK_SUCCESS) {
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment = {0};
        attachment.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
        const VkDynamicState states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = states;
        VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo pipeline = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipeline.pNext = &rendering;
        pipeline.stageCount = 2;
        pipeline.pStages = stages;
        pipeline.pVertexInputState = &vi;
        pipeline.pInputAssemblyState = &ia;
        pipeline.pViewportState = &vp;
        pipeline.pRasterizationState = &rs;
        pipeline.pMultisampleState = &ms;
        pipeline.pColorBlendState = &blend;
        pipeline.pDynamicState = &dynamic;
        pipeline.layout = gamma_layout;
        result = vkCreateGraphicsPipelines(V.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &gamma_pipeline);
        vkDestroyShaderModule(V.device, stages[1].module, NULL);
    }
    vkDestroyShaderModule(V.device, stages[0].module, NULL);
    return result == VK_SUCCESS;
}

static int draw_gamma(uint32_t index, VkImage source, VkImageView view,
                      const VkImageBlit *blit, float gamma)
{
    static int failed;
    if (failed)
        return 0;
    if (!gamma_pipeline && !init_gamma_pipeline()) {
        failed = 1;
        vlog("[PRESENTGAMMA] pipeline initialization failed\n");
        return 0;
    }
    if (!gamma_views[index]) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = V.images[index];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_B8G8R8A8_UNORM;
        vi.subresourceRange = (VkImageSubresourceRange) { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(V.device, &vi, NULL, &gamma_views[index]) != VK_SUCCESS) {
            failed = 1;
            vlog("[PRESENTGAMMA] swapchain view creation failed\n");
            return 0;
        }
    }
    barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(V.images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView = gamma_views[index];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color.float32[3] = 1.0f;
    VkRenderingInfo rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    rendering.renderArea.extent = V.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(V.cmd, &rendering);
    VkOffset3D first = blit->dstOffsets[0], last = blit->dstOffsets[1];
    VkRect2D bounds = { { first.x, first.y }, { last.x - first.x, last.y - first.y } };
    VkViewport viewport = { first.x, first.y, bounds.extent.width, bounds.extent.height, 0, 1 };
    vkCmdSetViewport(V.cmd, 0, 1, &viewport);
    vkCmdSetScissor(V.cmd, 0, 1, &bounds);
    vkCmdBindPipeline(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gamma_pipeline);
    VkDescriptorImageInfo image = { gamma_sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkCmdPushDescriptorSetKHR(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gamma_layout, 0, 1, &write);
    const float params[] = { blit->srcOffsets[1].x, blit->srcOffsets[1].y, gamma, present_warmth, present_cool_reduction };
    vkCmdPushConstants(V.cmd, gamma_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof params, params);
    vkCmdDraw(V.cmd, 3, 1, 0, 0);
    vkCmdEndRendering(V.cmd);
    barrier(source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return 1;
}

static int present_image(VkImage src, VkImageView src_view, uint32_t w, uint32_t h, int wide,
                         PresentTimings *timing, int capture_token)
{
    PresentSample sample = {0};
    ColorFrame capture = {0};
    uint32_t index;
    uint64_t start = timing ? monotonic_ns() : 0;
    VkResult r = vkAcquireNextImageKHR(V.device, V.swapchain, UINT64_MAX, V.acquired, VK_NULL_HANDLE, &index);
    if (timing) timing->acquire += monotonic_ns() - start;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        static int logged;
        if (logged++ < 4)
            vlog("[VK] vkAcquireNextImageKHR %d\n", (int)r);
        V.timestamp_active = 0;
        vkEndCommandBuffer(V.cmd);
        {
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1;
            si.pCommandBuffers = &V.cmd;
            vkResetFences(V.device, 1, &V.fence);
            vkQueueSubmit(V.queue, 1, &si, V.fence);
            vkWaitForFences(V.device, 1, &V.fence, VK_TRUE, UINT64_MAX);
        }
        return -1;
    }
    {
        VkImageBlit blit;
        VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        int32_t screen_w = (int32_t)V.extent.width, screen_h = (int32_t)V.extent.height;
        int32_t ar_w = wide ? 16 : 4, ar_h = wide ? 9 : 3;
        int32_t hh = screen_h, ww = hh * ar_w / ar_h;
        if (ww > screen_w) {
            ww = screen_w;
            hh = ww * ar_h / ar_w;
        }
        int32_t x0 = (screen_w - ww) / 2, y0 = (screen_h - hh) / 2;
        memset(&blit, 0, sizeof blit);
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1].x = (int32_t)w;
        blit.srcOffsets[1].y = (int32_t)h;
        blit.srcOffsets[1].z = 1;
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[0].x = x0;
        blit.dstOffsets[1].x = x0 + ww;
        blit.dstOffsets[0].y = y0;
        blit.dstOffsets[1].y = y0 + hh;
        blit.dstOffsets[1].z = 1;
        float gamma = src_view ? present_gamma() : 1.0f;
        VkImageLayout shown_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (!src_view || ((gamma == 1.0f && present_warmth == 0.0f && present_cool_reduction == 0.0f) ||
                          !draw_gamma(index, src, src_view, &blit, gamma))) {
            shown_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier(V.images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(V.cmd, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            barrier(V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdBlitImage(V.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           V.images[index], shown_layout, 1, &blit, VK_FILTER_LINEAR);
        }
        if (capture_token >= 'A' && capture_token <= 'Z')
            record_color_frame(&capture, src, blit.srcOffsets);
        if (capture_token && (V.surface_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
            barrier(V.images[index], shown_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            shown_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            record_present_sample(&sample, src, V.images[index], &blit);
            if (capture_token >= 'a' && capture_token <= 'z')
                record_color_frame(&capture, V.images[index], blit.dstOffsets);
        }
        barrier(V.images[index], shown_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        timestamp_end();
        vkEndCommandBuffer(V.cmd);
    }
    {
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &V.acquired;
        si.pWaitDstStageMask = &wait;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &V.cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &V.rendered;
        vkResetFences(V.device, 1, &V.fence);
        if (timing) start = monotonic_ns();
        r = vkQueueSubmit(V.queue, 1, &si, V.fence);
        if (timing) timing->submit += monotonic_ns() - start;
        if (r != VK_SUCCESS) {
            V.timestamp_active = 0;
            vlog("[VK] vkQueueSubmit %d\n", (int)r);
            finish_present_sample(&sample, 0);
            finish_color_frame(&capture, capture_token, 0);
            return -1;
        }
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &V.rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &V.swapchain;
        pi.pImageIndices = &index;
        if (timing) start = monotonic_ns();
        r = vkQueuePresentKHR(V.queue, &pi);
        if (timing) {
            uint64_t now = monotonic_ns();
            timing->present += now - start;
            start = now;
        }
        VkResult fence_result = vkWaitForFences(V.device, 1, &V.fence, VK_TRUE, UINT64_MAX);
        if (sample.buffer)
            finish_present_sample(&sample, fence_result == VK_SUCCESS);
        if (capture_token)
            finish_color_frame(&capture, capture_token, fence_result == VK_SUCCESS);
        if (timing) timing->fence += monotonic_ns() - start;
        timestamp_read();
    }
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR ? 0 : -1;
}

int ps5_vk_present_guest(const uint8_t *src, uint32_t pitch, uint32_t bpp)
{
    uint32_t y, x;
    uint32_t *dst = (uint32_t *)V.staging_map;

    if (!V.ready)
        return -1;
    for (y = 0; y < GUEST_H; y++) {
        const uint8_t *row = src + (size_t)y * pitch * (bpp == 8 ? 2 : 1);
        uint32_t *out = dst + (size_t)y * GUEST_W;
        if (bpp == 4) {
            memcpy(out, row, GUEST_W * 4);
        } else if (bpp == 8) {
            for (x = 0; x < GUEST_W; x++)
                out[x] = *(const uint32_t *)(row + (size_t)x * 8);
        } else {
            for (x = 0; x < GUEST_W; x++) {
                uint16_t v = *(const uint16_t *)(row + (size_t)x * 2);
                uint32_t r5 = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g6 = (uint32_t)((v >> 5) & 0x3F) * 255u / 63u;
                uint32_t b5 = (uint32_t)(v & 0x1F) * 255u / 31u;
                out[x] = 0xFF000000u | (r5 << 16) | (g6 << 8) | b5;
            }
        }
    }
    {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkBufferImageCopy copy;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(V.cmd, 0);
        vkBeginCommandBuffer(V.cmd, &bi);
        barrier(V.frame, V.frame_initialised ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&copy, 0, sizeof copy);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = GUEST_W;
        copy.imageExtent.height = GUEST_H;
        copy.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(V.cmd, V.staging, V.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(V.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        V.frame_initialised = 1;
    }
    return present_image(V.frame, VK_NULL_HANDLE, GUEST_W, GUEST_H, g_pc.aspect == 1, NULL, 0);
}

/* ===================================================================== */
/* Step 2: the render back end                                           */
/* ===================================================================== */

#define PHYS(va) ((va) & 0x0FFFFFFFu)
#define MAX_RT 64
#define MAX_DT 16
#define MAX_TEX 768
#define TEX_HASH 2048        /* open-addressed index over B.tex, power of two */
#define MAX_PIPE 1024
#define PIPE_KEY 44          /* blend/mask/depth, src, dst, eq, combiner use + 38 words
                              * + the GPU vertex shader flag */
#define RING_BYTES (96u << 20)

typedef struct {
    uint32_t key, w, h;
    Img im;
    uint32_t drawn_frame, wide_frame, movie_frame;
} RenderTarget;

typedef struct {
    uint32_t key, w, h;
    Img im;
} DepthTarget;

typedef struct {
    uint32_t off, w, h, color, pitch;
    uint64_t hash;
    uint32_t checked_frame;
    Img im;
} TexEntry;

typedef struct {
    uint32_t k[PIPE_KEY];
    VkPipeline p;
} PipeEntry;

/* The combiner constants (be.frag's uniform block RC), per draw. */
typedef struct {
    float k0[8][4], k1[8][4];
    float fk0[4], fk1[4], fogc[4];
} RcConsts;

/* What a draw's pixel side is: fixed (stage 0 x diffuse / YUV), or the
 * combiner program whose words become the pipeline's constants. */
typedef struct {
    int use;
    uint32_t words[38];          /* control, stage program, final 0/1, 8 x 4 stage words */
    RcConsts c;
} RcState;

typedef struct {
    uint32_t cull, front, depth_test, depth_write, depth_compare;
    uint32_t stencil_test, stencil_fail, stencil_pass, stencil_depth_fail, stencil_compare;
    uint32_t stencil_compare_mask, stencil_write_mask, stencil_reference, blend_color;
} DrawDynamicState;

typedef struct { float rt[4], tx[4], at[4], tas[4]; } PushConsts;

static struct {
    int active;
    uint32_t frame;
    int cmd_open, rendering;
    VkPipeline bound_pipeline;
    int dynamic_valid;
    DrawDynamicState dynamic;
    PushConsts pushed_constants;
    VkDescriptorImageInfo pushed_images[4];
    int push_constants_valid, push_images_valid;
    unsigned push_images_reused, push_constants_reused;
    RenderTarget rt[MAX_RT]; int nrt;
    DepthTarget dt[MAX_DT]; int ndt;
    TexEntry tex[MAX_TEX]; int ntex;
    uint16_t texmap[TEX_HASH];      /* B.tex index + 1; 0 is an empty slot */
    unsigned long texlookups;
    PipeEntry pipe[MAX_PIPE]; int npipe;
    RenderTarget *cur_rt; DepthTarget *cur_dt;
    RenderTarget *target;           /* the draw in progress's colour target */
    RenderTarget *frame_disp, *frame_big, *shown;
    VkBuffer ring; uint8_t *ring_map; uint32_t ring_used;
    uint64_t ring_bytes;
    unsigned ring_flushes;
    unsigned vsh_const_hits, vsh_const_uploads;
    int vsh_const_valid;
    uint32_t vsh_const_offset;
    uint64_t vsh_const_generation;
    float vsh_constants[196][4];
    uint32_t vsh_program[1 + NV2A_VSH_SLOTS][4];
    uint32_t vsh_program_offset, vsh_rc_offset;
    RcConsts vsh_rc;
    int vsh_program_valid, vsh_rc_valid;
    unsigned vsh_program_hits, vsh_rc_hits;
    unsigned vsh_timing_samples;
    uint64_t vsh_upload_ns, vsh_record_ns;
    unsigned vsh_guest_batches;
    VkSampler samp[5];              /* [linear][clamp], plus anisotropic repeat */
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkShaderModule vs, fs, vsvsh;   /* vsvsh: the uber vertex-program shader */
    Img white;
    uint32_t disp[3]; int ndisp;
    uint32_t *decode_buf; size_t decode_cap;
    unsigned long draws, tris, uploads;
    unsigned long white_why[5];      /* size, hazard copy, image, decode buffer, ring */
} B;

static int img_create(Img *im, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements req;
    memset(im, 0, sizeof *im);
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = w ? w : 1;
    ii.extent.height = h ? h : 1;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(V.device, &ii, NULL, &im->img) != VK_SUCCESS)
        return 0;
    vkGetImageMemoryRequirements(V.device, im->img, &req);
    if (!alloc_bind(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &im->mem)
            || vkBindImageMemory(V.device, im->img, im->mem, 0) != VK_SUCCESS) {
        vkDestroyImage(V.device, im->img, NULL);
        im->img = VK_NULL_HANDLE;
        return 0;
    }
    vi.image = im->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(V.device, &vi, NULL, &im->view) != VK_SUCCESS)
        return 0;
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    im->w = ii.extent.width;
    im->h = ii.extent.height;
    return 1;
}

static void img_destroy(Img *im)
{
    if (im->view) vkDestroyImageView(V.device, im->view, NULL);
    if (im->img) vkDestroyImage(V.device, im->img, NULL);
    if (im->mem) vkFreeMemory(V.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

/* ---- the frame's command buffer ------------------------------------- */

static void frame_open(void)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    if (B.cmd_open)
        return;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(V.cmd, 0);
    vkBeginCommandBuffer(V.cmd, &bi);
    V.timestamp_active = V.timestamps && (B.frame % 16 == 0);
    if (V.timestamp_active) {
        vkCmdResetQueryPool(V.cmd, V.timestamps, 0, 2);
        vkCmdWriteTimestamp(V.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, V.timestamps, 0);
    }
    B.cmd_open = 1;
    B.ring_used = 0;
    B.vsh_const_valid = B.vsh_program_valid = B.vsh_rc_valid = 0;
    B.push_constants_valid = B.push_images_valid = 0;
    B.bound_pipeline = VK_NULL_HANDLE;
    B.dynamic_valid = 0;
}

static void end_rendering(void)
{
    if (B.rendering) {
        vkCmdEndRendering(V.cmd);
        B.rendering = 0;
    }
}

static void flush_vsh_batch(void);

static void img_to(Img *im, VkImageAspectFlags aspect, VkImageLayout to)
{
    if (im->layout == to)
        return;
    flush_vsh_batch();
    end_rendering();
    barrier_aspect(im->img, aspect, im->layout, to);
    im->layout = to;
}

/* Submit what is recorded and wait: the ring and the command buffer are
 * reused afterwards. Used at flip (via present) and when the ring fills. */
static void frame_flush(void)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    if (!B.cmd_open)
        return;
    flush_vsh_batch();
    end_rendering();
    timestamp_end();
    vkEndCommandBuffer(V.cmd);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &V.cmd;
    vkResetFences(V.device, 1, &V.fence);
    vkQueueSubmit(V.queue, 1, &si, V.fence);
    vkWaitForFences(V.device, 1, &V.fence, VK_TRUE, UINT64_MAX);
    timestamp_read();
    B.cmd_open = 0;
    B.cur_rt = NULL;
    B.cur_dt = NULL;
}

/* Space in the frame's upload ring; flushes the frame when it is full. */
static uint32_t ring_alloc(uint32_t bytes, uint32_t align)
{
    uint32_t off = (B.ring_used + align - 1) & ~(align - 1);
    if (bytes > RING_BYTES)
        return UINT32_MAX;
    if (off + bytes > RING_BYTES) {
        ++B.ring_flushes;
        frame_flush();
        frame_open();
        off = 0;
    }
    B.ring_bytes += off + bytes - B.ring_used;
    B.ring_used = off + bytes;
    return off;
}

/* ---- render targets, depth, textures -------------------------------- */

static void surface_dims(const Nv2aSurface *s, uint32_t *w, uint32_t *h)
{
    uint32_t bpp = s->bytes_per_pixel ? s->bytes_per_pixel : 4;
    uint32_t pw = s->pitch ? s->pitch / bpp : s->width;
    *w = (s->pitch && pw >= 16) ? pw : (pw > s->width ? pw : s->width);
    *h = s->height;
    if (*w >= 320 && *h < (*w * 3) / 4)
        *h = (*w * 3) / 4;
    if (*w > 4096) *w = 4096;
    if (*h > 4096) *h = 4096;
    if (!*w) *w = 1;
    if (!*h) *h = 1;
}

static RenderTarget *get_rt(const Nv2aSurface *s)
{
    uint32_t key = PHYS(s->color_va), w, h;
    int i;
    RenderTarget *r;
    surface_dims(s, &w, &h);
    for (i = 0; i < B.nrt; i++)
        if (B.rt[i].key == key && B.rt[i].w == w && B.rt[i].h >= h)
            return &B.rt[i];
    for (i = 0; i < B.nrt; i++)
        if (B.rt[i].key == key)
            break;
    if (i == B.nrt) {
        if (B.nrt == MAX_RT) i = 0; else B.nrt++;
    }
    r = &B.rt[i];
    if (r->im.img) {
        frame_flush();
        frame_open();
        img_destroy(&r->im);
    }
    memset(r, 0, sizeof *r);
    /* Keep guest positions in their original units and scale the raster target. */
    uint32_t render_w = w, render_h = h;
    if (w == 640 && h == 480) {
        render_h = (uint32_t)g_pc.render_scale;
        render_w = render_h * 4 / 3;
    }
    if (!img_create(&r->im, render_w, render_h, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT))
        return NULL;
    r->key = key; r->w = w; r->h = h;
    {
        VkClearColorValue black = { { 0, 0, 0, 0 } };
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        img_to(&r->im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(V.cmd, r->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    }
    vlog("[VKBE] render target %08X %ux%u guest, %ux%u internal\n", key, w, h, render_w, render_h);
    return r;
}

static RenderTarget *find_rt(uint32_t va)
{
    int i;
    for (i = 0; i < B.nrt; i++)
        if (B.rt[i].key == PHYS(va))
            return &B.rt[i];
    return NULL;
}

static DepthTarget *get_dt(uint32_t zva, uint32_t w, uint32_t h)
{
    uint32_t key = PHYS(zva);
    int i;
    DepthTarget *d;
    for (i = 0; i < B.ndt; i++)
        if (B.dt[i].key == key && B.dt[i].w == w && B.dt[i].h == h)
            return &B.dt[i];
    for (i = 0; i < B.ndt; i++)
        if (B.dt[i].key == key) break;
    if (i == B.ndt) {
        if (B.ndt == MAX_DT) i = 0; else B.ndt++;
    }
    d = &B.dt[i];
    if (d->im.img) {
        frame_flush();
        frame_open();
        img_destroy(&d->im);
    }
    memset(d, 0, sizeof *d);
    if (!img_create(&d->im, w, h, VK_FORMAT_D32_SFLOAT_S8_UINT,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        return NULL;
    d->key = key; d->w = w; d->h = h;
    {
        VkClearDepthStencilValue z = { 1.0f, 0 };
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
        img_to(&d->im, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearDepthStencilImage(V.cmd, d->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &range);
    }
    return d;
}

static uint32_t tex_bytes(const Nv2aTexture *t)
{
    uint32_t w = t->width, h = t->height, n;
    switch (t->color) {
    case 0x0C: n = ((w + 3) / 4) * ((h + 3) / 4) * 8; break;          /* DXT1 */
    case 0x0E: case 0x0F: n = ((w + 3) / 4) * ((h + 3) / 4) * 16; break;  /* DXT3/5 */
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        n = t->pitch ? t->pitch * h : w * h; break;                  /* 8-bit */
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x1C:
    case 0x1D: case 0x24: case 0x25:
        n = t->pitch ? t->pitch * h : w * h * 2; break;              /* 16-bit */
    default:
        n = t->pitch ? t->pitch * h : w * h * 4; break;
    }
    return n > (16u << 20) ? (16u << 20) : n;
}

static uint64_t hash_guest(uint32_t va, uint32_t n, int split_alpha)
{
    const uint8_t *p = (const uint8_t *)xbox_GetMemoryOffset() + va;
    uint64_t h = 1469598103934665603ull;
    uint32_t i, step = n > (256u << 10) ? 128 : 32;
    for (i = 0; i + 8 <= n; i += step) {
        uint64_t v;
        /* DXT3/5 blocks start with eight alpha bytes. Sample colour on
         * alternating blocks, keeping the same number of memory reads. */
        uint32_t offset = i + (split_alpha && (i & step) ? 8 : 0);
        memcpy(&v, p + offset, sizeof v);
        h = (h ^ v) * 1099511628211ull;
    }
    return h ^ n;
}

/* Everything a cache entry is matched on, folded into a slot. */
static uint32_t tex_hash(uint32_t off, uint32_t w, uint32_t h, uint32_t color, uint32_t pitch)
{
    uint32_t x = off * 2654435761u ^ (w * 40503u) ^ (h * 257u) ^ (color * 131u) ^ pitch;
    return (x ^ (x >> 13)) & (TEX_HASH - 1);
}

static void texmap_put(uint16_t idx, uint32_t h)
{
    while (B.texmap[h])
        h = (h + 1) & (TEX_HASH - 1);
    B.texmap[h] = (uint16_t)(idx + 1);
}

static Img *get_texture(const Nv2aTexture *t)
{
    RenderTarget *r;
    TexEntry *e = NULL;
    uint32_t key = PHYS(t->offset);
    int i;

    if (!t->width || !t->height || t->width > 4096 || t->height > 4096)
        { B.white_why[0]++; return &B.white; }
    /* A render target sampled as a texture: the GPU's picture, not the
     * guest bytes (which this back end never writes). */
    if ((r = find_rt(t->offset)) != NULL && r->drawn_frame) {
        flush_vsh_batch();
        if (r == B.target) {
            /* Sampling the target being drawn: a copy. */
            static Img hz;
            if (!hz.img || hz.w != r->im.w || hz.h != r->im.h) {
                if (hz.img) { frame_flush(); frame_open(); img_destroy(&hz); }
                if (!img_create(&hz, r->im.w, r->im.h, VK_FORMAT_B8G8R8A8_UNORM,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT))
                    { B.white_why[1]++; return &B.white; }
            }
            img_to(&r->im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            img_to(&hz, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            {
                VkImageCopy c;
                memset(&c, 0, sizeof c);
                c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
                c.extent.width = r->im.w; c.extent.height = r->im.h; c.extent.depth = 1;
                vkCmdCopyImage(V.cmd, r->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               hz.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            }
            img_to(&hz, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            B.cur_rt = NULL;          /* its layout changed: rendering restarts */
            return &hz;
        }
        img_to(&r->im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return &r->im;
    }
    /* Indexed, not scanned: a level binds a couple of hundred textures and a
     * frame draws a few thousand batches, four stages each, so the old linear
     * scan cost millions of field comparisons per frame. A probe that lands on
     * another texture keeps probing, so collisions stay correct. */
    B.texlookups++;
    {
        uint32_t h = tex_hash(key, t->width, t->height, t->color, t->pitch), n;
        for (n = 0; n < TEX_HASH; n++) {
            uint16_t s = B.texmap[h];
            if (!s)
                break;
            if (B.tex[s - 1].off == key && B.tex[s - 1].w == t->width
                    && B.tex[s - 1].h == t->height && B.tex[s - 1].color == t->color
                    && B.tex[s - 1].pitch == t->pitch) {
                e = &B.tex[s - 1];
                break;
            }
            h = (h + 1) & (TEX_HASH - 1);
        }
    }
    if (e && e->checked_frame == B.frame + 1)
        return &e->im;
    {
        uint32_t n = tex_bytes(t);
        int split_alpha = t->color == 0x0E || t->color == 0x0F;
        uint64_t h = hash_guest(t->offset, n, split_alpha);
        size_t px = (size_t)t->width * t->height;
        if (e && e->hash == h) {
            e->checked_frame = B.frame + 1;
            return &e->im;
        }
        if (!e) {
            if (B.ntex == MAX_TEX) {
                /* Full: start over (simplest eviction; textures re-decode). */
                frame_flush();
                frame_open();
                for (i = 0; i < B.ntex; i++) img_destroy(&B.tex[i].im);
                B.ntex = 0;
                memset(B.texmap, 0, sizeof B.texmap);
            }
            e = &B.tex[B.ntex++];
            memset(e, 0, sizeof *e);
            e->off = key; e->w = t->width; e->h = t->height; e->color = t->color; e->pitch = t->pitch;
            texmap_put((uint16_t)(e - B.tex),
                       tex_hash(key, t->width, t->height, t->color, t->pitch));
            if (!img_create(&e->im, t->width, t->height, VK_FORMAT_B8G8R8A8_UNORM,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) {
                B.ntex--;
                { B.white_why[2]++; return &B.white; }
            }
        }
        if (B.decode_cap < px) {
            free(B.decode_buf);
            B.decode_buf = (uint32_t *)malloc(px * 4);
            B.decode_cap = B.decode_buf ? px : 0;
            if (!B.decode_buf)
                { B.white_why[3]++; return &B.white; }
        }
        if (!nv2a_backend_decode_texture(t, B.decode_buf)) {
            static int logged;
            if (logged++ < 16)
                vlog("[VKBE] texture format %02X not decodable (%ux%u)\n", t->color, t->width, t->height);
            for (i = 0; i < (int)px; i++) B.decode_buf[i] = 0xFFFFFFFFu;
        }
        {
            flush_vsh_batch();
            uint32_t off = ring_alloc((uint32_t)(px * 4), 16);
            VkBufferImageCopy c;
            if (off == UINT32_MAX)
                { B.white_why[4]++; return &B.white; }
            memcpy(B.ring_map + off, B.decode_buf, px * 4);
            img_to(&e->im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            memset(&c, 0, sizeof c);
            c.bufferOffset = off;
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.layerCount = 1;
            c.imageExtent.width = t->width;
            c.imageExtent.height = t->height;
            c.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(V.cmd, B.ring, e->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            img_to(&e->im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        e->hash = h;
        /* Keep the fingerprint used to identify the captured HUD atlases. */
        if (t->color == 0x0F && t->width == t->height &&
            (t->width == 128 || t->width == 256))
            e->im.guest_content_hash = hash_guest(t->offset, n, 0);
        e->checked_frame = B.frame + 1;
        B.uploads++;
        return &e->im;
    }
}

static int sampler_for(const Nv2aTexture *t)
{
    int clamp = (t->addr_u == 3 || t->addr_u == 4) ? 1 : 0;
    int linear = ((t->filter >> 16) & 0xFF) == 1 ? 0 : 1;
    if (linear && !clamp && B.samp[4] && oogie_launcher_settings.filtering)
        return 4;
    return linear * 2 + clamp;
}

/* ---- pipelines ------------------------------------------------------- */

static VkBlendFactor blend_factor(uint32_t f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkStencilOp stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static VkColorComponentFlags write_mask(uint32_t cm)
{
    VkColorComponentFlags m = 0;
    if (cm & 0x00010000u) m |= VK_COLOR_COMPONENT_R_BIT;
    if (cm & 0x00000100u) m |= VK_COLOR_COMPONENT_G_BIT;
    if (cm & 0x00000001u) m |= VK_COLOR_COMPONENT_B_BIT;
    if (cm & 0x01000000u) m |= VK_COLOR_COMPONENT_A_BIT;
    return m;
}

static VkPipeline get_pipeline(uint32_t blend_en, uint32_t src, uint32_t dst, uint32_t eq,
                               uint32_t cmask, int has_depth, const RcState *rc,
                               int vsh)
{
    uint32_t k[PIPE_KEY];
    int i;
    memset(k, 0, sizeof k);
    k[0] = blend_en | (has_depth << 1) | ((uint32_t)write_mask(cmask) << 4);
    k[1] = src; k[2] = dst; k[3] = eq;
    k[4] = rc && rc->use ? 1u : 0u;
    if (k[4])
        memcpy(&k[5], rc->words, sizeof rc->words);
    k[PIPE_KEY - 1] = vsh ? 1u : 0u;
    for (i = 0; i < B.npipe; i++)
        if (!memcmp(B.pipe[i].k, k, sizeof k))
            return B.pipe[i].p;
    if (B.npipe == MAX_PIPE) {
        static int logged;
        if (logged++ < 4)
            vlog("[VKBE] pipeline cache full (%d)\n", MAX_PIPE);
        return VK_NULL_HANDLE;
    }
    {
        VkPipelineShaderStageCreateInfo st[2];
        VkVertexInputBindingDescription vb[2] = {
            { 0, sizeof(Nv2aVertex), VK_VERTEX_INPUT_RATE_VERTEX },
            { 1, sizeof(OogieVertexExtra), VK_VERTEX_INPUT_RATE_VERTEX },
        };
        VkVertexInputAttributeDescription va[8] = {
            { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
            { 1, 0, VK_FORMAT_B8G8R8A8_UNORM, 16 },
            { 2, 0, VK_FORMAT_R32G32_SFLOAT, 20 },
            { 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },     /* specular */
            { 4, 1, VK_FORMAT_R32G32_SFLOAT, 16 },          /* fog, q0 */
            { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 24 },    /* stage 1 */
            { 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 40 },    /* stage 2 */
            { 7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 56 },    /* stage 3 */
        };
        /* The combiner program as be.frag's specialization constants:
         * RC_USE, control, stage program, final 0/1, then 8 x 4 words. */
        uint32_t sdata[37];
        VkSpecializationMapEntry smap[37];
        VkSpecializationInfo spec;
        int j;
        memset(sdata, 0, sizeof sdata);
        sdata[0] = k[4];
        if (k[4])
            memcpy(&sdata[1], rc->words, 36 * sizeof(uint32_t));
        for (j = 0; j < 37; j++) {
            smap[j].constantID = (uint32_t)j;
            smap[j].offset = (uint32_t)(j * sizeof(uint32_t));
            smap[j].size = sizeof(uint32_t);
        }
        spec.mapEntryCount = 37;
        spec.pMapEntries = smap;
        spec.dataSize = sizeof sdata;
        spec.pData = sdata;
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        VkPipelineColorBlendAttachmentState cba;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        VkDynamicState dyn[] = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
            VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        };
        VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        VkFormat color_fmt = VK_FORMAT_B8G8R8A8_UNORM;
        VkPipelineRenderingCreateInfo pr = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        VkPipeline p;

        memset(st, 0, sizeof st);
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vsh ? B.vsvsh : B.vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = B.fs; st[1].pName = "main";
        st[1].pSpecializationInfo = &spec;
        /* A GPU-vertex batch is drawn from its own buffer, read in the
         * shader: there are no vertex attributes at all. */
        vi.vertexBindingDescriptionCount = vsh ? 0u : 2u;
        vi.pVertexBindingDescriptions = vsh ? NULL : vb;
        vi.vertexAttributeDescriptionCount = vsh ? 0u : 8u;
        vi.pVertexAttributeDescriptions = vsh ? NULL : va;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        rs.depthClampEnable = V.depth_clamp ? VK_TRUE : VK_FALSE;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        memset(&cba, 0, sizeof cba);
        cba.blendEnable = blend_en ? VK_TRUE : VK_FALSE;
        cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = blend_factor(src);
        cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = blend_factor(dst);
        cba.colorBlendOp = cba.alphaBlendOp = blend_op(eq);
        cba.colorWriteMask = write_mask(cmask);
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        dy.dynamicStateCount = sizeof dyn / sizeof dyn[0];
        dy.pDynamicStates = dyn;
        pr.colorAttachmentCount = 1;
        pr.pColorAttachmentFormats = &color_fmt;
        pr.depthAttachmentFormat = has_depth ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_UNDEFINED;
        pr.stencilAttachmentFormat = has_depth ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_UNDEFINED;
        gi.pNext = &pr;
        gi.stageCount = 2;
        gi.pStages = st;
        gi.pVertexInputState = &vi;
        gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vp;
        gi.pRasterizationState = &rs;
        gi.pMultisampleState = &ms;
        gi.pDepthStencilState = &ds;
        gi.pColorBlendState = &cb;
        gi.pDynamicState = &dy;
        gi.layout = B.layout;
        if (vkCreateGraphicsPipelines(V.device, VK_NULL_HANDLE, 1, &gi, NULL, &p) != VK_SUCCESS) {
            vlog("[VKBE] pipeline creation failed\n");
            return VK_NULL_HANDLE;
        }
        memcpy(B.pipe[B.npipe].k, k, sizeof k);
        B.pipe[B.npipe].p = p;
        B.npipe++;
        return p;
    }
}

/* ---- drawing ----------------------------------------------------------- */

static void begin_rendering(RenderTarget *r, DepthTarget *d)
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkViewport vp;
    VkRect2D sc;

    if (B.rendering && B.cur_rt == r && B.cur_dt == d)
        return;
    end_rendering();
    img_to(&r->im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (d)
        img_to(&d->im, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    ca.imageView = r->im.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = r->im.w;
    ri.renderArea.extent.height = r->im.h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (d) {
        da.imageView = d->im.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = &da;
    }
    vkCmdBeginRendering(V.cmd, &ri);
    B.rendering = 1;
    B.cur_rt = r;
    B.cur_dt = d;
    vp.x = 0; vp.y = 0; vp.width = (float)r->im.w; vp.height = (float)r->im.h;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(V.cmd, 0, 1, &vp);
    sc.offset.x = 0; sc.offset.y = 0;
    sc.extent.width = r->im.w; sc.extent.height = r->im.h;
    vkCmdSetScissor(V.cmd, 0, 1, &sc);
}

static void set_dynamic_state(const Nv2aRenderState *rs, int has_depth)
{
    DrawDynamicState state = {
        .cull = VK_CULL_MODE_NONE,
        .front = rs->front_face == 0x0901 ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                          : VK_FRONT_FACE_CLOCKWISE,
        .depth_test = has_depth && rs->depth_test_enable,
        .depth_write = has_depth && rs->depth_write,
        .depth_compare = (rs->depth_func >= 0x200 && rs->depth_func <= 0x207)
                         ? rs->depth_func - 0x200 : VK_COMPARE_OP_ALWAYS,
        .stencil_test = has_depth && rs->stencil_enable,
        .stencil_fail = stencil_op(rs->stencil_fail),
        .stencil_pass = stencil_op(rs->stencil_zpass),
        .stencil_depth_fail = stencil_op(rs->stencil_zfail),
        .stencil_compare = (rs->stencil_func >= 0x200 && rs->stencil_func <= 0x207)
                           ? rs->stencil_func - 0x200 : VK_COMPARE_OP_ALWAYS,
        .stencil_compare_mask = rs->stencil_func_mask & 0xFF,
        .stencil_write_mask = rs->stencil_write_mask & 0xFF,
        .stencil_reference = rs->stencil_ref & 0xFF,
        .blend_color = rs->blend_color,
    };
    if (rs->cull_enable && (rs->cull_face == 0x0404 || rs->cull_face == 0x0405))
        state.cull = rs->cull_face == 0x0404 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004 ||
        rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004)
        state.blend_color = (rs->blend_color >> 24) * 0x01010101u;

    /* Both backend pipelines declare the same dynamic state. Consecutive
     * draws with the same effective state can reuse the recorded commands. */
    if (B.dynamic_valid && memcmp(&B.dynamic, &state, sizeof state) == 0)
        return;

    vkCmdSetCullMode(V.cmd, state.cull);
    vkCmdSetFrontFace(V.cmd, state.front);
    vkCmdSetDepthTestEnable(V.cmd, state.depth_test);
    vkCmdSetDepthWriteEnable(V.cmd, state.depth_write);
    vkCmdSetDepthCompareOp(V.cmd, state.depth_compare);
    vkCmdSetStencilTestEnable(V.cmd, state.stencil_test);
    vkCmdSetStencilOp(V.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.stencil_fail,
                      state.stencil_pass, state.stencil_depth_fail, state.stencil_compare);
    vkCmdSetStencilCompareMask(V.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.stencil_compare_mask);
    vkCmdSetStencilWriteMask(V.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.stencil_write_mask);
    vkCmdSetStencilReference(V.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.stencil_reference);
    float blend[4] = {
        ((state.blend_color >> 16) & 0xFF) / 255.0f,
        ((state.blend_color >> 8) & 0xFF) / 255.0f,
        (state.blend_color & 0xFF) / 255.0f,
        (state.blend_color >> 24) / 255.0f,
    };
    vkCmdSetBlendConstants(V.cmd, blend);
    B.dynamic = state;
    B.dynamic_valid = 1;
}

static void bind_draw_pipeline(VkPipeline pipeline)
{
    if (B.bound_pipeline == pipeline)
        return;
    vkCmdBindPipeline(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    B.bound_pipeline = pipeline;
}

static void push_draw_constants(const PushConsts *pc)
{
    if (B.push_constants_valid && memcmp(&B.pushed_constants, pc, sizeof *pc) == 0) {
        ++B.push_constants_reused;
        return;
    }
    vkCmdPushConstants(V.cmd, B.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof *pc, pc);
    B.pushed_constants = *pc;
    B.push_constants_valid = 1;
}

static void note_drawn(RenderTarget *r)
{
    r->drawn_frame = B.frame + 1;
    {
        int i;
        for (i = 0; i < B.ndisp; i++)
            if (B.disp[i] == r->key) { B.frame_disp = r; return; }
    }
    if (r->w >= 640 && (!B.frame_big || r->w * r->h >= B.frame_big->w * B.frame_big->h))
        B.frame_big = r;
}

static void draw_vertices(RenderTarget *r, DepthTarget *d, const Nv2aRenderState *rs,
                          const Nv2aVertex *v, const OogieVertexExtra *x, uint32_t count,
                          const PushConsts *pc, const RcState *rc,
                          Img *tex[4], int samp[4], uint32_t blend_en, uint32_t cmask)
{
    VkPipeline p;
    uint32_t off, xoff, uoff;
    VkDescriptorImageInfo ii[4];
    VkDescriptorBufferInfo ub;
    VkWriteDescriptorSet w[2];
    VkDeviceSize voff[2];
    VkBuffer bufs[2];
    int i;

    p = get_pipeline(blend_en, rs->blend_src, rs->blend_dst, rs->blend_eq, cmask, d != NULL, rc, 0);
    if (!p)
        return;
    {
        /* One reservation for all three: a ring that fills between two
         * separate ones would flush and hand the second the first's space. */
        uint32_t vbytes = count * (uint32_t)sizeof(Nv2aVertex);
        uint32_t xbytes = count * (uint32_t)sizeof(OogieVertexExtra);
        uint32_t xo = (vbytes + 15u) & ~15u;
        uint32_t uo = (xo + xbytes + 255u) & ~255u;
        uint32_t base = ring_alloc(uo + (uint32_t)sizeof(RcConsts), 256);
        if (base == UINT32_MAX)
            return;
        off = base;
        xoff = base + xo;
        uoff = base + uo;
    }
    memcpy(B.ring_map + off, v, count * sizeof(Nv2aVertex));
    if (x) {
        memcpy(B.ring_map + xoff, x, count * sizeof(OogieVertexExtra));
    } else {
        /* No extras: no specular, fog factor 1, q 1. */
        OogieVertexExtra *dx = (OogieVertexExtra *)(B.ring_map + xoff);
        memset(dx, 0, count * sizeof *dx);
        for (i = 0; i < (int)count; i++) { dx[i].fog = 1.0f; dx[i].q0 = 1.0f; }
    }
    if (rc && rc->use)
        memcpy(B.ring_map + uoff, &rc->c, sizeof rc->c);
    else
        memset(B.ring_map + uoff, 0, sizeof(RcConsts));
    /* Layout changes for the textures happened in get_texture, which may
     * have ended rendering; begin (again) only now. */
    begin_rendering(r, d);
    bind_draw_pipeline(p);
    set_dynamic_state(rs, d != NULL);
    for (i = 0; i < 4; i++) {
        ii[i].sampler = B.samp[samp[i]];
        ii[i].imageView = tex[i]->view;
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    memset(w, 0, sizeof w);
    w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 4;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = ii;
    ub.buffer = B.ring;
    ub.offset = uoff;
    ub.range = sizeof(RcConsts);
    w[1].dstBinding = 4;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[1].pBufferInfo = &ub;
    vkCmdPushDescriptorSetKHR(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, B.layout, 0, 2, w);
    B.push_images_valid = 0;
    push_draw_constants(pc);
    bufs[0] = bufs[1] = B.ring;
    voff[0] = off;
    voff[1] = xoff;
    vkCmdBindVertexBuffers(V.cmd, 0, 2, bufs, voff);
    vkCmdDraw(V.cmd, count, 1, 0, 0);
    B.draws++;
    B.tris += count / 3;
}

/* Batch B, split: how much of the frame the renderer itself costs. be_draw
 * runs on the render thread and is where every draw is decoded, uploaded and
 * recorded, so if this is most of the frame's work the answer is in the
 * renderer; if it is a small part, the frame is waiting on guest threads and
 * no renderer tuning can help. Compared against work p50 in be_flip. */
static uint64_t s_render_ns;
static int s_render_timing = -1;
/* When the last vsh batch finished, and the idle gap it leaves before the flip
 * request. A frame whose gap is most of its period is waiting on a guest
 * thread, not on the GPU or the renderer. */
static uint64_t s_last_submit_ns;
static uint64_t s_gap_counted;
static uint64_t s_gap_ns, s_gap_max_ns;
static unsigned s_gap_n;

/* The backend's draw entry. This GPU path never reaches it -- every batch
 * arrives at submit_vsh_batch, which is where the renderer is timed -- so
 * there is deliberately no timing here, which would double-count the frame. */
static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    RenderTarget *r;
    DepthTarget *d = NULL;
    const Nv2aRenderState *rs = b->state;
    const Nv2aTexture *tex0 = b->texture;
    Img *tex[4];
    int samp[4] = { 3, 3, 3, 3 }, i, yuv = 0;
    PushConsts pc;
    RcState rc;

    if (!s->color_va || b->count < 3 || !b->vertices)
        return;
    flush_vsh_batch();
    frame_open();
    r = get_rt(s);
    if (!r)
        return;
    B.target = r;
    if (rs->zeta_va)
        d = get_dt(rs->zeta_va, r->im.w, r->im.h);
    for (i = 0; i < 4; i++)
        tex[i] = &B.white;
    memset(&pc, 0, sizeof pc);
    pc.tas[0] = 1.0f;
    if (tex0) {
        tex[0] = get_texture(tex0);
        samp[0] = sampler_for(tex0);
        pc.tx[0] = tex0->width ? 1.0f / (float)tex0->width : 1.0f;
        pc.tx[1] = tex0->height ? 1.0f / (float)tex0->height : 1.0f;
        pc.tx[2] = 1.0f;
        {
            /* Bink's YUV pattern (d3d11_backend.c prep_draw): Y8 planes on
             * stages 0-2 and a combiner of 5+ stages doing the conversion. */
            Nv2aTexture t1, t2;
            if (tex0->color == 0x13 && oogie_backend_combiner_stages() >= 5
                && oogie_backend_stage_texture(1, &t1) && oogie_backend_stage_texture(2, &t2)
                && t1.color == 0x13 && t2.color == 0x13) {
                tex[1] = get_texture(&t1);
                tex[2] = get_texture(&t2);
                samp[1] = sampler_for(&t1);
                samp[2] = sampler_for(&t2);
                yuv = 1;
            }
        }
    } else {
        pc.tx[0] = pc.tx[1] = 1.0f;
    }
    pc.tas[1] = pc.tas[2] = pc.tas[3] = 1.0f;
    /* The register combiners, once the title has programmed them
     * (d3d11_backend.c fill_combiner); Bink's YUV keeps its own path.
     * OOGIE_NO_RC=1: the fixed path only (diagnostic). */
    memset(&rc, 0, sizeof rc);
    {
        static int no_rc = -1;
        Nv2aCombiner c;
        float fog[3];
        if (no_rc < 0) { const char *e = getenv("OOGIE_NO_RC"); no_rc = e && *e == '1'; }
        if (!yuv && !no_rc && oogie_backend_combiner(&c, fog)) {
            uint32_t n = c.control & 0xFFu, st;
            rc.use = 1;
            if (n > 8) n = 8;
            rc.words[0] = c.control;
            rc.words[1] = c.stage_program;
            rc.words[2] = c.final0;
            rc.words[3] = c.final1;
            for (st = 0; st < n; st++) {
                rc.words[4 + st * 4 + 0] = c.color_icw[st];
                rc.words[4 + st * 4 + 1] = c.alpha_icw[st];
                rc.words[4 + st * 4 + 2] = c.color_ocw[st];
                rc.words[4 + st * 4 + 3] = c.alpha_ocw[st];
            }
            for (st = 0; st < 8; st++) {
                nv2a_rc_unpack(c.factor0[st], rc.c.k0[st]);
                nv2a_rc_unpack(c.factor1[st], rc.c.k1[st]);
            }
            nv2a_rc_unpack(c.final_c0, rc.c.fk0);
            nv2a_rc_unpack(c.final_c1, rc.c.fk1);
            rc.c.fogc[0] = fog[0]; rc.c.fogc[1] = fog[1]; rc.c.fogc[2] = fog[2];
            rc.c.fogc[3] = 1.0f;
            for (st = 1; st < 4; st++) {
                Nv2aTexture t;
                if (oogie_backend_stage_texture((int)st, &t)) {
                    tex[st] = get_texture(&t);
                    samp[st] = sampler_for(&t);
                }
            }
        }
    }
    pc.tx[3] = (float)yuv;
    pc.rt[0] = 1.0f / (float)r->w;
    pc.rt[1] = 1.0f / (float)r->h;
    pc.rt[2] = rs->depth_max > 1.0f ? 1.0f / rs->depth_max : 1.0f / 16777215.0f;
    pc.at[0] = rs->alpha_test_enable ? 1.0f : 0.0f;
    pc.at[1] = (float)(rs->alpha_func - 0x200);
    pc.at[2] = (float)rs->alpha_ref;
    {
        /* Diagnostic: 60 draws in gameplay (from frame 1500), all of what
         * decides their colour. "Everything white" in a level. */
        static int logged;
        /* Meshes only (more than one quad): cutscene subtitles filled the
         * first log with glyphs. */
        if (B.frame >= 1500 && b->count > 6 && logged < 60) {
            const Nv2aVertex *v0 = b->vertices;
            const OogieVertexExtra *x0 = oogie_backend_vertex_extra();
            logged++;
            vlog("[DRAW+] st0 %08X %08X %08X %08X st1 %08X %08X %08X %08X tex1-3 %s/%s/%s k1=(%.2f,%.2f,%.2f,%.2f) fk0=%.2f,%.2f,%.2f fk1=%.2f,%.2f,%.2f fog=%.2f,%.2f,%.2f tc=(%.2f,%.2f,%.2f,%.2f)\n",
                 rc.words[4], rc.words[5], rc.words[6], rc.words[7],
                 rc.words[8], rc.words[9], rc.words[10], rc.words[11],
                 tex[1] == &B.white ? "W" : "t", tex[2] == &B.white ? "W" : "t", tex[3] == &B.white ? "W" : "t",
                 rc.c.k1[0][0], rc.c.k1[0][1], rc.c.k1[0][2], rc.c.k1[0][3],
                 rc.c.fk0[0], rc.c.fk0[1], rc.c.fk0[2], rc.c.fk1[0], rc.c.fk1[1], rc.c.fk1[2],
                 rc.c.fogc[0], rc.c.fogc[1], rc.c.fogc[2],
                 x0 ? x0->tc[0][0] : 0.0f, x0 ? x0->tc[0][1] : 0.0f, x0 ? x0->tc[0][2] : 0.0f,
                 x0 ? x0->tc[0][3] : 0.0f);
            vlog("[DRAW] f=%u tgt=%08X n=%u tex0=%08X fmt=%02X %ux%u %s yuv=%d rc=%d ctl=%08X sp=%05X f0=%08X f1=%08X "
                 "blend=%u %X/%X cm=%08X at=%u z=%u/%u zeta=%08X v0=(%.1f,%.1f,%.3f,%.4f) d=%08X uv=(%.2f,%.2f) "
                 "x=%s spec=%08X fog=%.2f q=%.2f t1=(%.2f,%.2f) k0=(%.2f,%.2f,%.2f,%.2f) white=%lu/%lu/%lu/%lu/%lu\n",
                 B.frame, r->key, b->count, tex0 ? PHYS(tex0->offset) : 0, tex0 ? tex0->color : 0,
                 tex0 ? tex0->width : 0, tex0 ? tex0->height : 0,
                 !tex0 ? "-" : tex[0] == &B.white ? "WHITE" : find_rt(tex0->offset) ? "rt" : "tex",
                 yuv, rc.use, rc.words[0], rc.words[1], rc.words[2], rc.words[3],
                 rs->blend_enable, rs->blend_src, rs->blend_dst, rs->color_mask, rs->alpha_test_enable,
                 rs->depth_test_enable, rs->depth_write, rs->zeta_va,
                 v0->x, v0->y, v0->z, v0->rhw, v0->diffuse, v0->u, v0->v,
                 x0 ? "y" : "n",
                 x0 ? (((uint32_t)(x0->spec[3] * 255) << 24) | ((uint32_t)(x0->spec[0] * 255) << 16)
                       | ((uint32_t)(x0->spec[1] * 255) << 8) | (uint32_t)(x0->spec[2] * 255)) : 0u,
                 x0 ? x0->fog : 0.0f, x0 ? x0->q0 : 0.0f, x0 ? x0->tc[0][0] : 0.0f, x0 ? x0->tc[0][1] : 0.0f,
                 rc.c.k0[0][0], rc.c.k0[0][1], rc.c.k0[0][2], rc.c.k0[0][3],
                 B.white_why[0], B.white_why[1], B.white_why[2], B.white_why[3], B.white_why[4]);
        }
    }
    draw_vertices(r, d, rs, b->vertices, oogie_backend_vertex_extra(), b->count, &pc, &rc,
                  tex, samp, rs->blend_enable && !(rs->blend_src == 1 && rs->blend_dst == 0),
                  rs->color_mask);
    note_drawn(r);
}

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    RenderTarget *r;
    DepthTarget *d = NULL;

    if (!s->color_va)
        return;
    flush_vsh_batch();
    frame_open();
    r = get_rt(s);
    if (!r)
        return;
    if (rs && rs->zeta_va)
        d = get_dt(rs->zeta_va, r->im.w, r->im.h);
    if ((flags & 0xF0u) == 0xF0u) {
        VkClearColorValue c;
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        c.float32[0] = ((argb >> 16) & 0xFF) / 255.0f;
        c.float32[1] = ((argb >> 8) & 0xFF) / 255.0f;
        c.float32[2] = (argb & 0xFF) / 255.0f;
        c.float32[3] = (argb >> 24) / 255.0f;
        img_to(&r->im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(V.cmd, r->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
        r->drawn_frame = B.frame + 1;
    } else if (flags & 0xF0u) {
        /* A partial clear (Oogie clears alpha alone): a full-surface quad
         * with only those channels written. */
        uint32_t cm = 0;
        Nv2aVertex v[6];
        Nv2aRenderState q;
        PushConsts pc;
        Img *tex[4] = { &B.white, &B.white, &B.white, &B.white };
        int samp[4] = { 0, 0, 0, 0 }, i;
        float W = (float)r->w, H = (float)r->h;
        if (flags & 0x10u) cm |= 0x00010000u;
        if (flags & 0x20u) cm |= 0x00000100u;
        if (flags & 0x40u) cm |= 0x00000001u;
        if (flags & 0x80u) cm |= 0x01000000u;
        memset(v, 0, sizeof v);
        v[0].x = 0; v[0].y = 0; v[1].x = W; v[1].y = 0; v[2].x = 0; v[2].y = H;
        v[3].x = W; v[3].y = 0; v[4].x = W; v[4].y = H; v[5].x = 0; v[5].y = H;
        for (i = 0; i < 6; i++) { v[i].rhw = 1.0f; v[i].diffuse = argb; }
        memset(&q, 0, sizeof q);
        q.blend_src = 1; q.blend_dst = 0; q.blend_eq = 0x8006;
        q.depth_func = 0x207; q.stencil_func = 0x207;
        memset(&pc, 0, sizeof pc);
        pc.rt[0] = 1.0f / W; pc.rt[1] = 1.0f / H; pc.rt[2] = 1.0f;
        pc.tx[0] = pc.tx[1] = 1.0f;
        draw_vertices(r, NULL, &q, v, NULL, 6, &pc, NULL, tex, samp, 0, cm);
    }
    if (d && (flags & 0x03u)) {
        VkClearDepthStencilValue z;
        VkImageSubresourceRange range = { 0, 0, 1, 0, 1 };
        if (flags & 0x01u) range.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (flags & 0x02u) range.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        z.depth = (float)(zstencil >> 8) / 16777215.0f;
        z.stencil = zstencil & 0xFF;
        img_to(&d->im, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearDepthStencilImage(V.cmd, d->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &range);
    }
}

/* The game's frame buffers, from its D3D device (d3d11_backend.c
 * disp_refresh): the pointer at 0x001E25E8, +0x1A10 the buffer count and
 * +0x1A14 the surfaces, whose +4 is the data address. */
static void disp_refresh(void)
{
    const uint8_t *m = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t dev, n, i, keys[3];
    B.ndisp = 0;
    dev = *(const volatile uint32_t *)(m + 0x001E25E8u);
    if (!dev || dev >= 0x04000000u - 0x1A20u) return;
    n = *(const volatile uint32_t *)(m + dev + 0x1A10u);
    if (n < 1 || n > 3) return;
    for (i = 0; i < n; i++) {
        uint32_t surf = *(const volatile uint32_t *)(m + dev + 0x1A14u + 4 * i);
        if (!surf || surf >= 0x04000000u - 8) return;
        keys[i] = PHYS(*(const volatile uint32_t *)(m + surf + 4));
        if (!keys[i]) return;
    }
    for (i = 0; i < n; i++) B.disp[i] = keys[i];
    B.ndisp = (int)n;
}

/* Batch A: where a frame's time goes. 0.1 ms buckets to 200 ms, so the tail
 * (a lock convoy, a stalled fence) is visible where a one-second average hides
 * it. p50 says whether the frame is fundamentally too slow; p99 says whether
 * it is occasionally terrible. */
#define FRAME_HIST_BUCKETS 2001

static double hist_pct(const uint32_t *hist, uint64_t n, double pct)
{
    uint64_t want, acc = 0;
    uint32_t i;

    if (!n)
        return 0.0;
    want = (uint64_t)(n * pct + 0.999);
    for (i = 0; i < FRAME_HIST_BUCKETS; i++) {
        acc += hist[i];
        if (acc >= want)
            return i * 0.1;
    }
    return (FRAME_HIST_BUCKETS - 1) * 0.1;
}

/* Batch B: why a merged run ended. Each flush costs a draw call, a pipeline
 * bind and a descriptor push, so which guard trips decides whether merging is
 * already optimal or bleeding. Counted, never timed. */
enum {
    FLUSH_STATE, FLUSH_PROGRAM, FLUSH_IMAGE, FLUSH_TEXTURE, FLUSH_CONST,
    FLUSH_CAP, FLUSH_SPACE, FLUSH_OTHER, FLUSH_REASONS
};
static unsigned s_flush_reason[FLUSH_REASONS];
static int s_merge_stats = -1;      /* [oogie] merge_stats, default on */
static int s_merge_cap = -1;        /* [oogie] merge_cap, default 64 */

/* How many draws one merged group may hold. The group's buffers bound it for
 * real (VSH_GROUP_FLOATS / VSH_GROUP_INDICES), so raising this is safe: the
 * space guard still ends the batch before an overflow. */
static unsigned merge_cap(void)
{
    if (s_merge_cap < 0) {
        int v = oogie_flag("merge_cap", 64);
        if (v < 1)
            v = 1;
        if (v > 256)
            v = 256;
        s_merge_cap = v;
    }
    return (unsigned)s_merge_cap;
}

/* One-shot probe of the guest's D3D8 device.
 *
 * The functions that dominate the guest's call count are all "push
 * MEM32(0x40B0E0); call [0x25F8E0] ... call [0x25F8E4]": a device object and
 * the lock/unlock pair around it. Those three words are the whole boundary an
 * HLE would have to stand in front of, so print them once, with the first
 * vtable entries, before replacing anything.
 *
 * Guest memory is mapped at a fixed offset from the VA, which is how the
 * kernel layer reads guest structures too. */
extern ptrdiff_t g_xbox_mem_offset;

static int s_probe_done;
static unsigned s_probe_tick;
/* Slow-flip accounting: which counters moved during a frame that took longer
 * than a vsync period, so a stall can be attributed without a profiler. */
static unsigned long s_st_draws, s_st_tris, s_st_uploads, s_st_tex, s_st_batches;
static unsigned s_stall_log;

/* Nothing here needs a bounds check: these are guest VAs, which cannot leave
 * the low 256 MB, and a zero device pointer just means the deal was not made
 * yet. Whether a printed target is guest code or one of our thunks is visible
 * in the value itself -- guest text lives in the low megabytes, the runtime
 * image high -- so it is not annotated. */
static void probe_d3d_device(void)
{
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t dev = *(const uint32_t *)(0x40B0E0u + base);
    int i;

    vlog("[Oogie] d3d dev=0x%08X lock=0x%08X unlock=0x%08X vtable=",
         dev, *(const uint32_t *)(0x25F8E0u + base), *(const uint32_t *)(0x25F8E4u + base));
    if (dev) {
        uint32_t vt = *(const uint32_t *)(dev + base);
        for (i = 0; i < 8; i++)
            vlog("%s%08X", i ? " " : "", *(const uint32_t *)(vt + 4u * i + base));
    }
    vlog("\n");
}

static void be_flip(void)
{
    RenderTarget *r = B.frame_disp ? B.frame_disp : (B.frame_big ? B.frame_big : B.shown);
    static unsigned flips, presented;
    static PresentTimings timing;
    static uint64_t last_flip;
    /* Frame period is flip to flip; "blocked" is the part spent inside present
     * (image acquire plus fence wait), so work = period - blocked. Only if the
     * guest itself blocked between flips would that overstate work, and the
     * guest never waits on the flip here. */
    static int hist_on = -1;
    static uint32_t hist[FRAME_HIST_BUCKETS];
    static uint32_t work_hist[FRAME_HIST_BUCKETS];
    static uint64_t hist_n, blocked_ns, frame_blocked_ns, prev_entry_ns;
    uint64_t t_entry = 0, t_pres = 0;

    if (hist_on < 0)
        hist_on = oogie_flag("frame_hist", 1);
    if (hist_on)
        t_entry = monotonic_ns();
    /* Idle between the frame's last batch and this flip. Charged once per
     * submission; a frame that submits nothing contributes nothing. */
    if (hist_on && s_last_submit_ns != s_gap_counted) {
        uint64_t gap = t_entry - s_last_submit_ns;

        s_gap_ns += gap;
        if (gap > s_gap_max_ns)
            s_gap_max_ns = gap;
        s_gap_n++;
        s_gap_counted = s_last_submit_ns;
    }

    flush_vsh_batch();
    frame_open();
    end_rendering();
    if (r) {
        img_to(&r->im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        int capture_token = color_frame_request(flips);
        int shown_ok;

        if (hist_on)
            t_pres = monotonic_ns();
        /* Fill the display for gameplay, menus, and cutscenes in widescreen
         * mode. The wide_frame/movie_frame tags still control gameplay HUD
         * correction independently of the final presentation aspect. */
        int wide = g_pc.aspect == 1;
        static int last_wide = -1;
        if (wide != last_wide) {
            vlog("[VKBE] present aspect %s\n", wide ? "16:9" : "4:3");
            last_wide = wide;
        }
        shown_ok = present_image(r->im.img, r->im.view, (r->w > 640 ? 640 : r->w) * r->im.w / r->w,
                                 (r->h > 480 ? 480 : r->h) * r->im.h / r->h, wide, &timing, capture_token) == 0;
        if (hist_on) {
            frame_blocked_ns = monotonic_ns() - t_pres;
            blocked_ns += frame_blocked_ns;
        }
        if (shown_ok)
            ++presented;
        B.shown = r;
    } else {
        frame_flush();
    }
    if (hist_on && t_entry) {
        if (prev_entry_ns) {
            uint64_t dt = t_entry - prev_entry_ns;
            uint64_t work = dt > frame_blocked_ns ? dt - frame_blocked_ns : 0;
            uint32_t b = (uint32_t)(dt / 100000u);
            uint32_t w = (uint32_t)(work / 100000u);

            if (b >= FRAME_HIST_BUCKETS)
                b = FRAME_HIST_BUCKETS - 1;
            if (w >= FRAME_HIST_BUCKETS)
                w = FRAME_HIST_BUCKETS - 1;
            hist[b]++;
            work_hist[w]++;
            hist_n++;
            /* A frame that missed its period: name what moved during it. Only
             * the first sixteen, so a scene that runs slow throughout cannot
             * flood the log; the counters below stay exact either way. */
            if (dt > 40000000u && s_stall_log < 16) {
                s_stall_log++;
                vlog("[Oogie] stall %.1f ms (work %.1f, blocked %.2f): draws +%lu tris +%lu uploads +%lu texloops +%lu batches +%lu, targets %d textures %d pipelines %d\n",
                     dt / 1e6, work / 1e6, frame_blocked_ns / 1e6,
                     B.draws - s_st_draws, B.tris - s_st_tris,
                     B.uploads - s_st_uploads, B.texlookups - s_st_tex,
                     s_vsh_draws - s_st_batches, B.nrt, B.ntex, B.npipe);
            }
            s_st_draws = B.draws;
            s_st_tris = B.tris;
            s_st_uploads = B.uploads;
            s_st_tex = B.texlookups;
            s_st_batches = s_vsh_draws;
        }
        prev_entry_ns = t_entry;
        frame_blocked_ns = 0;
    }
    B.cmd_open = 0;
    B.cur_rt = NULL;
    B.cur_dt = NULL;
    B.frame_disp = NULL;
    B.frame_big = NULL;
    B.frame++;
    disp_refresh();
    /* The console's log ring is only a few KB deep, so a line printed once at
     * start-up is evicted long before anyone reads it. Repeat it about once a
     * minute, which is what makes the probe readable at all. */
    if (!s_probe_done || ++s_probe_tick >= 3000) {
        s_probe_done = 1;
        s_probe_tick = 0;
        probe_d3d_device();
    }
    if (++flips == 1 || flips % 600 == 0)
        vlog("[VKBE] flip %u: %lu draws, %lu tris, %lu uploads, %lu texture lookups, %lu vsh batches (%lu verts, %lu dropped) so far; %d targets, %d textures, %d pipelines, showing %08X\n",
             flips, B.draws, B.tris, B.uploads, B.texlookups, s_vsh_draws, s_vsh_verts,
             s_vsh_failed, B.nrt, B.ntex, B.npipe, r ? r->key : 0);
    if (flips % 300 == 0) {
        uint64_t now = monotonic_ns();
        if (last_flip && now > last_flip) {
            vlog("[Oogie] fps %.1f over the last 300 flips (GPU)\n", 300e9 / (now - last_flip));
            struct timespec cpu;
            static uint64_t last_cpu_ns;
            if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) == 0) {
                uint64_t cpu_ns = (uint64_t)cpu.tv_sec * 1000000000u + (uint64_t)cpu.tv_nsec;
                if (last_cpu_ns && cpu_ns >= last_cpu_ns)
                    vlog("[PBPERF] poll thread CPU %.2f ms/flip\n",
                         (cpu_ns - last_cpu_ns) / 300e6);
                last_cpu_ns = cpu_ns;
            }
            static uint64_t last_guest_cpu;
            uint64_t guest_cpu = __atomic_load_n(&s_guest_cpu_snapshot, __ATOMIC_RELAXED);
            if (last_guest_cpu && guest_cpu > last_guest_cpu)
                vlog("[GUESTCPU] entry thread %.2f ms CPU/flip from fence snapshots\n",
                     (guest_cpu - last_guest_cpu) / 300e6);
            last_guest_cpu = guest_cpu;
            uint64_t current_scan = s_scan_started ? now - s_scan_started : 0;
            vlog("[PBPERF] DMA scan wall %.2f ms/flip (%u completed scans)\n",
                 (s_scan_ns + current_scan) / 300e6, s_scan_count);

            if (presented) {
                double scale = 1.0 / (1e6 * presented);
                vlog("[VKPERF] ms/present acquire %.2f submit %.2f present %.2f fence %.2f (%u/300); ring MiB/flip %.2f, wraps %u, constant uploads %u reused %u\n",
                     timing.acquire * scale, timing.submit * scale, timing.present * scale,
                     timing.fence * scale, presented, B.ring_bytes / (300.0 * 1048576.0),
                     B.ring_flushes, B.vsh_const_uploads, B.vsh_const_hits);
            }
        }
        if (s_render_timing && hist_n) {
            vlog("[Oogie] render %.2f ms/flip (work p50 %.1f, so %.0f%% renderer)\n",
                 s_render_ns / (1e6 * (double)hist_n),
                 hist_pct(work_hist, hist_n, 0.50),
                 100.0 * (s_render_ns / (1e6 * (double)hist_n)) /
                     (hist_pct(work_hist, hist_n, 0.50) > 0.01
                          ? hist_pct(work_hist, hist_n, 0.50) : 0.01));
            s_render_ns = 0;
        }
        if (s_gap_n) {
            vlog("[Oogie] gap last-submit->flip avg %.2f ms max %.2f (n=%u)\n",
                 s_gap_ns / (1e6 * (double)s_gap_n), s_gap_max_ns / 1e6, s_gap_n);
            s_gap_ns = 0;
            s_gap_max_ns = 0;
            s_gap_n = 0;
        }
        if (s_merge_stats) {
            unsigned merged = 0, i;

            for (i = 0; i < FLUSH_REASONS; i++)
                merged += s_flush_reason[i];
            if (merged) {
                vlog("[Oogie] merges ended: state %u program %u image %u tex %u const %u cap %u space %u other %u (of %u)\n",
                     s_flush_reason[FLUSH_STATE], s_flush_reason[FLUSH_PROGRAM],
                     s_flush_reason[FLUSH_IMAGE], s_flush_reason[FLUSH_TEXTURE],
                     s_flush_reason[FLUSH_CONST], s_flush_reason[FLUSH_CAP],
                     s_flush_reason[FLUSH_SPACE], s_flush_reason[FLUSH_OTHER], merged);
                memset(s_flush_reason, 0, sizeof s_flush_reason);
            }
        }
        if (hist_n) {
            /* work gets its own histogram: period p50 minus mean blocked was a
             * percentile minus a mean, which reads as a number but is not one. */
            vlog("[Oogie] frame ms p50 %.1f p90 %.1f p99 %.1f max %.1f; work p50 %.1f p90 %.1f p99 %.1f; blocked %.2f (n=%llu)\n",
                 hist_pct(hist, hist_n, 0.50), hist_pct(hist, hist_n, 0.90),
                 hist_pct(hist, hist_n, 0.99), hist_pct(hist, hist_n, 0.999),
                 hist_pct(work_hist, hist_n, 0.50), hist_pct(work_hist, hist_n, 0.90),
                 hist_pct(work_hist, hist_n, 0.99),
                 blocked_ns / (1e6 * (double)hist_n), (unsigned long long)hist_n);
            memset(hist, 0, sizeof hist);
            memset(work_hist, 0, sizeof work_hist);
            hist_n = 0;
            blocked_ns = 0;
        }
        if (last_flip && B.vsh_timing_samples) {
            double scale = 1.0 / (1000.0 * B.vsh_timing_samples);
            vlog("[VKCPU] us/draw upload %.2f record %.2f (%u samples); guest batches %u, reused images %u push %u program %u combiner %u\n",
                 B.vsh_upload_ns * scale, B.vsh_record_ns * scale,
                 B.vsh_timing_samples, B.vsh_guest_batches,
                 B.push_images_reused, B.push_constants_reused, B.vsh_program_hits, B.vsh_rc_hits);
        }
        B.vsh_upload_ns = B.vsh_record_ns = 0;
        B.vsh_guest_batches = 0;
        B.vsh_timing_samples = 0;
        B.push_images_reused = B.push_constants_reused = 0;
        B.vsh_program_hits = B.vsh_rc_hits = 0;
        s_scan_ns = 0;
        s_scan_count = 0;
        if (s_scan_started)
            s_scan_started = now;
        for (unsigned kind = 0; kind < 2; ++kind) {
            uint64_t calls = __atomic_exchange_n(&s_fence_timing[kind].calls, 0, __ATOMIC_RELAXED);
            uint64_t wall = __atomic_exchange_n(&s_fence_timing[kind].wall, 0, __ATOMIC_RELAXED);
            uint64_t cpu = __atomic_exchange_n(&s_fence_timing[kind].cpu, 0, __ATOMIC_RELAXED);
            if (last_flip && calls)
                vlog("[FENCE] %s %.2f ms wall, %.2f ms CPU per flip (%lu calls)\n",
                     kind == 0 ? "D3D fence" : "GPU position", wall / 300e6, cpu / 300e6,
                     (unsigned long)calls);
        }
        last_flip = now;
        timing = (PresentTimings){0};
        presented = B.ring_flushes = B.vsh_const_uploads = B.vsh_const_hits = 0;
        B.ring_bytes = 0;
        extern uint64_t oogie_ps5_d3d_cs_fast_count(void);
        extern uint64_t oogie_ps5_d3d_cs_direct_count(void);
        if (s_d3d_cs_fast)
            vlog("[D3DCS] %lu fast lock calls so far (%lu direct)\n",
                 (unsigned long)oogie_ps5_d3d_cs_fast_count(),
                 (unsigned long)oogie_ps5_d3d_cs_direct_count());
        unsigned long hle_calls = oogie_hle_count();
        if (hle_calls)
            vlog("[D3DHLE] %lu native vector uploads so far\n", hle_calls);
        if (V.gpu_samples)
            vlog("[VKGPU] %.2f ms per sampled submission (%u samples)\n",
                 V.gpu_ns / (1e6 * V.gpu_samples), V.gpu_samples);
        V.gpu_ns = 0;
        V.gpu_samples = 0;
    }
}

/* â”€â”€ M9 step 5: vertex programs on the GPU â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 *
 * The runtime's `backend_gpu_vsh` patch hands a whole vertex-program batch
 * over (port/src/oogie_gpu_vsh.h) instead of the interpreter's per-vertex
 * output: the raw attribute inputs the program reads, its words, its
 * constants, and the batch as a triangle list. be_vsh.vert runs the program
 * once per vertex; this draws it. The profiler put ~35% of the draw thread's
 * samples in nv2a_vsh_run, which this replaces.
 */
#define VSH_ALIGN(n) (((n) + 255u) & ~255u)

static void draw_vsh_batch(RenderTarget *r, DepthTarget *d, const Nv2aRenderState *rs,
                           const OogieVshBatch *b,
                           uint32_t in_off, uint32_t c_off, uint32_t p_off,
                           uint32_t ib_off, uint32_t rc_off,
                           Img *tex[4], const int samp[4], const PushConsts *pc, const RcState *rc,
                           uint32_t blend_en, uint32_t cmask)
{
    VkPipeline p = get_pipeline(blend_en, rs->blend_src, rs->blend_dst, rs->blend_eq,
                                cmask, d != NULL, rc, 1);
    VkDescriptorImageInfo ii[4] = {0};
    VkDescriptorBufferInfo ub, sb[3];
    VkWriteDescriptorSet w[3];
    int i;

    if (!p)
        return;
    begin_rendering(r, d);
    bind_draw_pipeline(p);
    set_dynamic_state(rs, d != NULL);
    for (i = 0; i < 4; i++) {
        ii[i].sampler = B.samp[samp[i]];
        ii[i].imageView = tex[i]->view;
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    ub.buffer = B.ring; ub.offset = rc_off; ub.range = sizeof(RcConsts);
    sb[0].buffer = B.ring; sb[0].offset = in_off; sb[0].range = b->nverts * b->ninputs * 16u;
    sb[1].buffer = B.ring; sb[1].offset = c_off;  sb[1].range = 196u * 16u;
    sb[2].buffer = B.ring; sb[2].offset = p_off;  sb[2].range = (b->ninsn + 1u) * 16u;
    if (!sb[0].range)
        sb[0].range = 16u;
    memset(w, 0, sizeof w);
    w[0].sType = w[1].sType = w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstBinding = 0; w[0].descriptorCount = 4;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = ii;
    w[1].dstBinding = 4; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[1].pBufferInfo = &ub;
    w[2].dstBinding = 5; w[2].descriptorCount = 3;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[2].pBufferInfo = sb;
    /* The draw pipelines share one layout. Updating the buffer bindings
     * leaves the image bindings live until this command buffer closes. */
    if (B.push_images_valid && memcmp(B.pushed_images, ii, sizeof ii) == 0) {
        vkCmdPushDescriptorSetKHR(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, B.layout, 0, 2, w + 1);
        ++B.push_images_reused;
    } else {
        vkCmdPushDescriptorSetKHR(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, B.layout, 0, 3, w);
        memcpy(B.pushed_images, ii, sizeof ii);
        B.push_images_valid = 1;
    }
    push_draw_constants(pc);
    vkCmdBindIndexBuffer(V.cmd, B.ring, ib_off, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(V.cmd, b->nindices, 1, 0, 0, 0);
    B.draws++;
    B.tris += b->nindices / 3;
    s_vsh_draws++;
    s_vsh_verts += b->nverts;
}

static int submit_vsh_batch_impl(RenderTarget *r, DepthTarget *d, const Nv2aRenderState *rs,
                                 Img *tex[4], const int samp[4], const PushConsts *pc, const RcState *rc,
                                 const OogieVshBatch *b, const float extra_constants[4][4],
                                 uint64_t constant_generation);

/* This backend never calls be_draw, which is why the renderer split read 0.00
 * ms/flip no matter what the frame cost: every batch arrives here instead.
 * Time the real path, and mark when it finished so be_flip can report the idle
 * gap between the frame's last submission and the flip request. */
static int submit_vsh_batch(RenderTarget *r, DepthTarget *d, const Nv2aRenderState *rs,
                            Img *tex[4], const int samp[4], const PushConsts *pc, const RcState *rc,
                            const OogieVshBatch *b, const float extra_constants[4][4],
                            uint64_t constant_generation)
{
    uint64_t t0;
    int drew;

    if (s_render_timing < 0)
        s_render_timing = oogie_flag("render_ms", 1);
    if (!s_render_timing) {
        drew = submit_vsh_batch_impl(r, d, rs, tex, samp, pc, rc, b, extra_constants,
                                     constant_generation);
        s_last_submit_ns = monotonic_ns();
        return drew;
    }
    t0 = monotonic_ns();
    drew = submit_vsh_batch_impl(r, d, rs, tex, samp, pc, rc, b, extra_constants,
                                 constant_generation);
    s_render_ns += monotonic_ns() - t0;
    s_last_submit_ns = monotonic_ns();
    return drew;
}

static int submit_vsh_batch_impl(RenderTarget *r, DepthTarget *d, const Nv2aRenderState *rs,
                                 Img *tex[4], const int samp[4], const PushConsts *pc, const RcState *rc,
                                 const OogieVshBatch *b, const float extra_constants[4][4],
                                 uint64_t constant_generation)
{
    uint32_t base;
    uint32_t in_bytes, vin_span, c_span, p_span, ib_bytes, ib_span;
    uint32_t in_off, c_off, p_off, ib_off, rc_off;
    uint32_t batch_span;
    int reuse_constants, reuse_program, reuse_rc;
    uint32_t program_header[4] = { b->ninsn, b->ninputs, b->input_mask, 0 };
    RcConsts empty_rc = {0};
    const RcConsts *rc_data;
    int sample = (s_vsh_draws & 255u) == 0;
    uint64_t start = sample ? monotonic_ns() : 0, upload_end = 0;
    /* One reservation for the whole batch: a ring that filled between two
     * allocations would flush and invalidate the first one's offset. */
    in_bytes = b->nverts * b->ninputs * 16u;
    vin_span = VSH_ALIGN(in_bytes ? in_bytes : 16u);
    c_span = VSH_ALIGN(196u * 16u);
    p_span = VSH_ALIGN((b->ninsn + 1u) * 16u);
    ib_bytes = b->nindices * 4u;
    ib_span = VSH_ALIGN(ib_bytes ? ib_bytes : 16u);
    uint32_t rc_span = VSH_ALIGN(sizeof(RcConsts));
    uint32_t prog_bytes = b->ninsn * 16u;
    rc_data = rc->use ? &rc->c : &empty_rc;
    /* Ring offsets stay live only inside the same command buffer. Compare
     * exact state in regular CPU memory and reserve all misses together. */
    reuse_constants = B.vsh_const_valid &&
        memcmp(B.vsh_constants + 192, extra_constants, sizeof(float) * 16u) == 0 &&
        B.vsh_const_generation == constant_generation;
    reuse_program = b->ninsn <= NV2A_VSH_SLOTS && B.vsh_program_valid &&
        memcmp(B.vsh_program[0], program_header, sizeof program_header) == 0 &&
        memcmp(B.vsh_program + 1, b->program, prog_bytes) == 0;
    reuse_rc = B.vsh_rc_valid && memcmp(&B.vsh_rc, rc_data, sizeof B.vsh_rc) == 0;
    batch_span = vin_span + ib_span + (reuse_constants ? 0 : c_span)
               + (reuse_program ? 0 : p_span) + (reuse_rc ? 0 : rc_span);
    if (batch_span > RING_BYTES || VSH_ALIGN(B.ring_used) > RING_BYTES - batch_span) {
        reuse_constants = reuse_program = reuse_rc = 0;
        batch_span = vin_span + ib_span + c_span + p_span + rc_span;
    }
    base = ring_alloc(batch_span, 256);
    if (base == UINT32_MAX) {
        s_vsh_failed++;
        return 0;
    }
    in_off = base;
    ib_off = base + vin_span;
    uint32_t cursor = ib_off + ib_span;

    if (in_bytes)
        memcpy(B.ring_map + in_off, b->inputs, in_bytes);
    memcpy(B.ring_map + ib_off, b->indices, ib_bytes);
    if (reuse_constants) {
        c_off = B.vsh_const_offset;
        ++B.vsh_const_hits;
    } else {
        c_off = cursor;
        cursor += c_span;
        memcpy(B.vsh_constants, b->constants, 192u * 16u);
        memcpy(B.vsh_constants + 192, extra_constants, sizeof(float) * 16u);
        memcpy(B.ring_map + c_off, B.vsh_constants, sizeof B.vsh_constants);
        B.vsh_const_offset = c_off;
        B.vsh_const_generation = constant_generation;
        B.vsh_const_valid = 1;
        ++B.vsh_const_uploads;
    }
    if (reuse_program) {
        p_off = B.vsh_program_offset;
        ++B.vsh_program_hits;
    } else {
        p_off = cursor;
        cursor += p_span;
        memcpy(B.ring_map + p_off, program_header, sizeof program_header);
        memcpy(B.ring_map + p_off + sizeof program_header, b->program, prog_bytes);
        if (b->ninsn <= NV2A_VSH_SLOTS) {
            memcpy(B.vsh_program[0], program_header, sizeof program_header);
            memcpy(B.vsh_program + 1, b->program, prog_bytes);
            B.vsh_program_offset = p_off;
            B.vsh_program_valid = 1;
        }
    }
    if (reuse_rc) {
        rc_off = B.vsh_rc_offset;
        ++B.vsh_rc_hits;
    } else {
        rc_off = cursor;
        memcpy(B.ring_map + rc_off, rc_data, sizeof B.vsh_rc);
        memcpy(&B.vsh_rc, rc_data, sizeof B.vsh_rc);
        B.vsh_rc_offset = rc_off;
        B.vsh_rc_valid = 1;
    }

    if (sample)
        upload_end = monotonic_ns();
    draw_vsh_batch(r, d, rs, b, in_off, c_off, p_off, ib_off, rc_off,
                   tex, samp, pc, rc,
                   rs->blend_enable && !(rs->blend_src == 1 && rs->blend_dst == 0),
                   rs->color_mask);
    if (sample) {
        B.vsh_upload_ns += upload_end - start;
        B.vsh_record_ns += monotonic_ns() - upload_end;
        ++B.vsh_timing_samples;
    }
    return 1;
}

#define VSH_GROUP_FLOATS (16u * 1024u)
#define VSH_GROUP_INDICES (4u * 1024u)

static struct {
    RenderTarget *color;
    DepthTarget *depth;
    Nv2aRenderState state;
    Img *images[4];
    int samplers[4];
    PushConsts push;
    RcState combiner;
    OogieVshBatch batch;
    uint32_t program[NV2A_VSH_SLOTS][4];
    float constants[192][4], extra[4][4];
    uint64_t constant_generation;
    float inputs[VSH_GROUP_FLOATS];
    uint32_t indices[VSH_GROUP_INDICES];
    unsigned draws;
} pending_vsh;

static void flush_vsh_batch(void)
{
    if (!pending_vsh.draws)
        return;
    /* Mark the queue empty before submission so a ring wrap or image barrier
     * inside submission cannot recursively drain the same primitives. */
    pending_vsh.draws = 0;
    submit_vsh_batch(pending_vsh.color, pending_vsh.depth, &pending_vsh.state,
                     pending_vsh.images, pending_vsh.samplers, &pending_vsh.push,
                     &pending_vsh.combiner, &pending_vsh.batch, pending_vsh.extra,
                     pending_vsh.constant_generation);
}

static int queue_vsh_batch(RenderTarget *r, DepthTarget *d, const Nv2aRenderState *rs,
                           Img *images[4], const int samplers[4], const PushConsts *pc,
                           const RcState *rc, const OogieVshBatch *b, const float extra[4][4])
{
    uint32_t floats = b->nverts * b->ninputs * 4u;
    uint64_t constant_generation = oogie_vsh_constant_generation();
    if (floats > VSH_GROUP_FLOATS || b->nindices > VSH_GROUP_INDICES ||
        b->ninsn > NV2A_VSH_SLOTS || b->nindices % 3u) {
        flush_vsh_batch();
        return submit_vsh_batch(r, d, rs, images, samplers, pc, rc, b, extra, constant_generation);
    }

    if (pending_vsh.draws) {
        /* Same decision as the single || chain this replaced, but staged so the
         * costliest compares (program, constants) only run when the cheap ones
         * match, and so the reason can be counted. */
        int reason = -1;

        if (s_merge_stats < 0)
            s_merge_stats = oogie_flag("merge_stats", 1);
        if (pending_vsh.color != r || pending_vsh.depth != d)
            reason = FLUSH_OTHER;
        else if (pending_vsh.batch.ninputs != b->ninputs ||
                 pending_vsh.batch.input_mask != b->input_mask ||
                 pending_vsh.batch.ninsn != b->ninsn)
            reason = FLUSH_PROGRAM;
        else if (pending_vsh.draws >= merge_cap())
            reason = FLUSH_CAP;
        else if (pending_vsh.batch.nverts * b->ninputs * 4u > VSH_GROUP_FLOATS - floats ||
                 pending_vsh.batch.nindices > VSH_GROUP_INDICES - b->nindices)
            reason = FLUSH_SPACE;
        else if (memcmp(&pending_vsh.state, rs, sizeof *rs))
            reason = FLUSH_STATE;
        else if (memcmp(pending_vsh.images, images, sizeof pending_vsh.images))
            reason = FLUSH_IMAGE;
        else if (memcmp(pending_vsh.samplers, samplers, sizeof pending_vsh.samplers))
            reason = FLUSH_TEXTURE;
        else if (memcmp(&pending_vsh.push, pc, sizeof *pc) ||
                 pending_vsh.constant_generation != constant_generation ||
                 memcmp(pending_vsh.extra, extra, sizeof pending_vsh.extra))
            reason = FLUSH_CONST;
        else if (memcmp(&pending_vsh.combiner, rc, sizeof *rc))
            reason = FLUSH_OTHER;
        else if (memcmp(pending_vsh.program, b->program, b->ninsn * 16u))
            reason = FLUSH_PROGRAM;
        if (reason >= 0) {
            if (s_merge_stats)
                s_flush_reason[reason]++;
            flush_vsh_batch();
        }
    }

    if (!pending_vsh.draws) {
        pending_vsh.color = r;
        pending_vsh.depth = d;
        pending_vsh.state = *rs;
        memcpy(pending_vsh.images, images, sizeof pending_vsh.images);
        memcpy(pending_vsh.samplers, samplers, sizeof pending_vsh.samplers);
        pending_vsh.push = *pc;
        pending_vsh.combiner = *rc;
        memcpy(pending_vsh.program, b->program, b->ninsn * 16u);
        memcpy(pending_vsh.constants, b->constants, sizeof pending_vsh.constants);
        pending_vsh.constant_generation = constant_generation;
        memcpy(pending_vsh.extra, extra, sizeof pending_vsh.extra);
        pending_vsh.batch = *b;
        pending_vsh.batch.nverts = pending_vsh.batch.nindices = 0;
        pending_vsh.batch.program = pending_vsh.program;
        pending_vsh.batch.constants = pending_vsh.constants;
        pending_vsh.batch.inputs = pending_vsh.inputs;
        pending_vsh.batch.indices = pending_vsh.indices;
    }
    uint32_t vertex_base = pending_vsh.batch.nverts;
    if (floats)
        memcpy(pending_vsh.inputs + vertex_base * b->ninputs * 4u,
               b->inputs, floats * sizeof(float));
    uint32_t *indices = pending_vsh.indices + pending_vsh.batch.nindices;
    for (uint32_t i = 0; i < b->nindices; ++i)
        indices[i] = b->indices[i] + vertex_base;
    pending_vsh.batch.nverts += b->nverts;
    pending_vsh.batch.nindices += b->nindices;
    ++pending_vsh.draws;
    return 1;
}

/* Cache the packed constants and program. The executor can rewrite the
 * same registers between draws, so compare their values. Texture contents
 * and render-target hazards are still resolved for each draw. */
static const RcState *vsh_combiner(int yuv)
{
    static const RcState disabled;
    static Nv2aCombiner previous;
    static float previous_fog[3];
    static RcState packed;
    Nv2aCombiner c;
    float fog[3];
    if (yuv || !oogie_backend_combiner(&c, fog))
        return &disabled;
    if (packed.use && !memcmp(&previous, &c, sizeof c)
        && !memcmp(previous_fog, fog, sizeof fog))
        return &packed;

    previous = c;
    memcpy(previous_fog, fog, sizeof fog);
    RcState *rc = &packed;
    memset(rc, 0, sizeof *rc);
    uint32_t n = c.control & 0xFFu, st;
    rc->use = 1;
    if (n > 8) n = 8;
    rc->words[0] = c.control;
    rc->words[1] = c.stage_program;
    rc->words[2] = c.final0;
    rc->words[3] = c.final1;
    for (st = 0; st < n; st++) {
        rc->words[4 + st * 4 + 0] = c.color_icw[st];
        rc->words[4 + st * 4 + 1] = c.alpha_icw[st];
        rc->words[4 + st * 4 + 2] = c.color_ocw[st];
        rc->words[4 + st * 4 + 3] = c.alpha_ocw[st];
    }
    for (st = 0; st < 8; st++) {
        nv2a_rc_unpack(c.factor0[st], rc->c.k0[st]);
        nv2a_rc_unpack(c.factor1[st], rc->c.k1[st]);
    }
    nv2a_rc_unpack(c.final_c0, rc->c.fk0);
    nv2a_rc_unpack(c.final_c1, rc->c.fk1);
    rc->c.fogc[0] = fog[0]; rc->c.fogc[1] = fog[1]; rc->c.fogc[2] = fog[2];
    rc->c.fogc[3] = 1.0f;
    return rc;
}

static int be_draw_vsh(const Nv2aSurface *s, const Nv2aRenderState *rs,
                       const Nv2aTexture *tex0, const OogieVshBatch *b)
{
    RenderTarget *r;
    DepthTarget *d = NULL;
    Img *tex[4];
    int samp[4] = { 3, 3, 3, 3 }, yuv = 0, i;
    PushConsts pc;
    const RcState *rc;
    float extra_constants[4][4] = {
        { b->fog_enable ? 1.0f : 0.0f, (float)b->fog_mode, b->fog_param[0], b->fog_param[1] },
        { b->aa_sx, b->aa_sy, 0.0f, 0.0f },
        { b->tscale[0][0], b->tscale[0][1], b->tscale[1][0], b->tscale[1][1] },
        { b->tscale[2][0], b->tscale[2][1], b->tscale[3][0], b->tscale[3][1] },
    };

    if (!s->color_va || b->nindices < 3)
        return 1;                       /* nothing to draw: the batch is done */
    frame_open();
    r = get_rt(s);
    if (!r) {
        s_vsh_failed++;
        return 0;                       /* the CPU path draws it */
    }
    B.target = r;
    if (rs->zeta_va)
        d = get_dt(rs->zeta_va, r->im.w, r->im.h);

    /* Textures and the combiner program, as be_draw resolves them: the pixel
     * side is the same shader either way. */
    for (i = 0; i < 4; i++)
        tex[i] = &B.white;
    memset(&pc, 0, sizeof pc);
    pc.tas[0] = 1.0f;
    pc.tx[0] = pc.tx[1] = 1.0f;
    if (tex0) {
        tex[0] = get_texture(tex0);
        samp[0] = sampler_for(tex0);
        pc.tx[0] = tex0->width ? 1.0f / (float)tex0->width : 1.0f;
        pc.tx[1] = tex0->height ? 1.0f / (float)tex0->height : 1.0f;
        pc.tx[2] = 1.0f;
        {
            Nv2aTexture t1, t2;
            if (tex0->color == 0x13 && oogie_backend_combiner_stages() >= 5
                && oogie_backend_stage_texture(1, &t1) && oogie_backend_stage_texture(2, &t2)
                && t1.color == 0x13 && t2.color == 0x13) {
                tex[1] = get_texture(&t1);
                tex[2] = get_texture(&t2);
                samp[1] = sampler_for(&t1);
                samp[2] = sampler_for(&t2);
                yuv = 1;
            }
        }
    }
    pc.tas[1] = pc.tas[2] = pc.tas[3] = 1.0f;
    rc = vsh_combiner(yuv);
    if (rc->use) {
        uint32_t st;
        for (st = 1; st < 4; st++) {
            Nv2aTexture t;
            if (oogie_backend_stage_texture((int)st, &t)) {
                tex[st] = get_texture(&t);
                samp[st] = sampler_for(&t);
            }
        }
    }
    pc.tx[3] = (float)yuv;
    pc.rt[0] = 1.0f / (float)r->w;
    pc.rt[1] = 1.0f / (float)r->h;
    pc.rt[2] = rs->depth_max > 1.0f ? 1.0f / rs->depth_max : 1.0f / 16777215.0f;
    pc.at[0] = rs->alpha_test_enable ? 1.0f : 0.0f;
    pc.at[1] = (float)(rs->alpha_func - 0x200);
    pc.at[2] = (float)rs->alpha_ref;

    if (g_pc.aspect == 1 && r->w >= 640 && r->h >= 480) {
        if (yuv) {
            r->movie_frame = B.frame + 1;
        } else if (b->ninsn > 4 && r->wide_frame != B.frame + 1 &&
                   oogie_widescreen_active()) {
            r->wide_frame = B.frame + 1;
        } else if (b->ninsn <= 4 && r->wide_frame == B.frame + 1 &&
                   r->movie_frame != B.frame + 1 && b->ninputs && (b->input_mask & 1)) {
            /* Upstream draws HUD quads in screen pixels. Preserve their
             * proportions in a wide frame while keeping full-frame effects. */
            float left = 1e9f, right = -1e9f, top = 1e9f, bottom = -1e9f;
            for (uint32_t v = 0; v < b->nverts; ++v) {
                const float *position = b->inputs + (size_t)v * b->ninputs * 4;
                if (position[0] < left) left = position[0];
                if (position[0] > right) right = position[0];
                if (position[1] < top) top = position[1];
                if (position[1] > bottom) bottom = position[1];
            }
            float width = right - left;
            uint64_t texture_hash = tex[0]->guest_content_hash;
            /* Both rank words share one atlas. Zero's panel, lettering,
             * and button use three atlases distinct from subtitle text. */
            int combo_rank = top >= 100.0f && bottom <= 245.0f &&
                             texture_hash == 0x024b0e42c30430cdull;
            int zero_prompt = top >= 370.0f &&
                              (texture_hash == 0x294ed1ca5c32b264ull ||
                               texture_hash == 0x35c755eb457f407cull ||
                               texture_hash == 0x960d01dbc577b29full);
            if (g_pc.hud_edges && (combo_rank || zero_prompt)) {
                /* Keep the same offset when a piece animates past x=0. */
                pc.rt[3] = 0.75f;
                pc.at[3] = -(1.0f - pc.rt[3]);
            } else if (left >= -0.5f && right <= 640.5f && width < 636.0f) {
                pc.rt[3] = 0.75f;
                /* The vertical boss gauge is drawn in pieces. Its cap can
                 * meet the top-corner test when the fill and base do not.
                 * Keep right-side pieces on the same centered transform. */
                float centre_y = (top + bottom) * 0.5f;
                float centre_x = (left + right) * 0.5f;
                if (g_pc.hud_edges && centre_y <= 168.0f && width <= 240.0f &&
                    centre_x <= 224.0f)
                    pc.at[3] = -(1.0f - pc.rt[3]);
            }
        }
    }
    if (!queue_vsh_batch(r, d, rs, tex, samp, &pc, rc, b, extra_constants))
        return 0;
    ++B.vsh_guest_batches;
    note_drawn(r);
    return 1;
}

static const Nv2aBackend s_backend_vk = { be_clear, be_draw, be_flip };

int ps5_vk_backend_active(void) { return B.active; }

/* Called once Vulkan is up. OOGIE_VK_BACKEND=0 keeps the CPU rasteriser. */
static int backend_start(void)
{
    VkResult r;
    const char *e = getenv("OOGIE_VK_BACKEND");
    if (e && *e == '0')
        return 0;
    {
        VkShaderModuleCreateInfo si = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        si.codeSize = sizeof spv_be_vert;
        si.pCode = spv_be_vert;
        if ((r = vkCreateShaderModule(V.device, &si, NULL, &B.vs)) != VK_SUCCESS)
            return fail("backend vertex shader", r);
        si.codeSize = sizeof spv_be_frag;
        si.pCode = spv_be_frag;
        if ((r = vkCreateShaderModule(V.device, &si, NULL, &B.fs)) != VK_SUCCESS)
            return fail("backend fragment shader", r);
        si.codeSize = sizeof spv_be_vsh_vert;
        si.pCode = spv_be_vsh_vert;
        if ((r = vkCreateShaderModule(V.device, &si, NULL, &B.vsvsh)) != VK_SUCCESS)
            return fail("vertex-program shader", r);
    }
    {
        VkDescriptorSetLayoutBinding b[8];
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConsts) };
        VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        int i;
        memset(b, 0, sizeof b);
        for (i = 0; i < 4; i++) {
            b[i].binding = (uint32_t)i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        b[4].binding = 4;                    /* the combiner constants */
        b[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[4].descriptorCount = 1;
        b[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        /* 5-7: a GPU-vertex batch's inputs, constants and program words
         * (be_vsh.vert). The other pipelines do not declare them. */
        for (i = 5; i < 8; i++) {
            b[i].binding = (uint32_t)i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        }
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        li.bindingCount = 8;
        li.pBindings = b;
        if ((r = vkCreateDescriptorSetLayout(V.device, &li, NULL, &B.dsl)) != VK_SUCCESS)
            return fail("descriptor set layout", r);
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &B.dsl;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pcr;
        if ((r = vkCreatePipelineLayout(V.device, &pl, NULL, &B.layout)) != VK_SUCCESS)
            return fail("pipeline layout", r);
    }
    {
        int i;
        for (i = 0; i < 4; i++) {
            VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            VkSamplerAddressMode am = (i & 1) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                              : VK_SAMPLER_ADDRESS_MODE_REPEAT;
            si.magFilter = si.minFilter = (i & 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = si.addressModeV = si.addressModeW = am;
            si.maxLod = 0.0f;
            if ((r = vkCreateSampler(V.device, &si, NULL, &B.samp[i])) != VK_SUCCESS)
                return fail("sampler", r);
        }
        if (oogie_vk_filter_available() && oogie_launcher_settings.filtering) {
            VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            si.magFilter = si.minFilter = VK_FILTER_LINEAR;
            si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            si.anisotropyEnable = VK_TRUE;
            si.maxAnisotropy = anisotropy_limit < 4.0f ? anisotropy_limit : 4.0f;
            if (vkCreateSampler(V.device, &si, NULL, &B.samp[4]) != VK_SUCCESS) {
                B.samp[4] = VK_NULL_HANDLE;
                vlog("[VKBE] anisotropic sampler unavailable\n");
            }
        }
    }
    {
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        VkMemoryRequirements req;
        VkDeviceMemory mem;
        void *map;
        bi.size = RING_BYTES;
        /* INDEX_BUFFER and STORAGE_BUFFER: a GPU-vertex batch is drawn from
         * an index buffer and read by be_vsh.vert through three storage
         * buffers, all of them this ring. */
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                 | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                 | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if ((r = vkCreateBuffer(V.device, &bi, NULL, &B.ring)) != VK_SUCCESS)
            return fail("upload ring", r);
        vkGetBufferMemoryRequirements(V.device, B.ring, &req);
        if (!alloc_bind(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &mem)
                || vkBindBufferMemory(V.device, B.ring, mem, 0) != VK_SUCCESS
                || vkMapMemory(V.device, mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)
            return fail("upload ring memory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
        B.ring_map = (uint8_t *)map;
    }
    /* A 1x1 white texture for unused stages. */
    frame_open();
    if (!img_create(&B.white, 1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return fail("white texture", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    {
        VkClearColorValue white = { { 1, 1, 1, 1 } };
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        img_to(&B.white, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(V.cmd, B.white.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
        img_to(&B.white, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    frame_flush();
    disp_refresh();
    B.active = 1;
    nv2a_backend_register(&s_backend_vk);
    /* M9 step 5: hand the executor's vertex-program batches to the GPU. The
     * runtime patch registers nothing by default, so until this call every
     * program ran in nv2a_vsh_run on the draw thread. OOGIE_GPU_VSH=0 keeps
     * the CPU interpreter, for comparing one against the other. */
    {
        const char *e = getenv("OOGIE_GPU_VSH");
        if (!e || *e != '0')
            oogie_gpu_vsh_register(be_draw_vsh);
    }
    vlog("[VKBE] render back end registered: the GPU rasterises\n");
    return 1;
}

int ps5_vk_start_backend(void)
{
    if (!V.ready || B.active)
        return B.active;
    return backend_start();
}

#endif /* OOGIE_HAVE_VULKAN */
