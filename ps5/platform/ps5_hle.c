#include "ps5_hle.h"

#include "oogie_flags.h"
#include "ps5_guest_regs.h"

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

/* Guest memory is mapped at a fixed offset from the VA; this is the same
 * translation the generated code's MEM32 performs. */
extern ptrdiff_t g_xbox_mem_offset;
extern int guest_cpu_held(void);

#define GW(va) (*(volatile uint32_t *)((uintptr_t)(va) + (uintptr_t)g_xbox_mem_offset))

/* Push-buffer ring, as the guest keeps it: next free byte and the limit. */
#define RING_WP    0x1E25F0u
#define RING_LIMIT 0x1E25F4u
/* Mirror of the uploaded vectors, 16 bytes per slot. */
#define VEC_CACHE  0x1E1530u
/* Method tokens the guest emits for this record shape. */
#define TOKEN_A    0x41EA4u
#define TOKEN_B    0x100B80u
#define RECORD     0x1Cu

static int s_hle = -1;
static int s_lock_off = -1;
static unsigned long s_hle_count;

int oogie_hle_lock_off(void)
{
    if (s_lock_off < 0)
        s_lock_off = oogie_flag("lock_elide", 0);
    return s_lock_off && guest_cpu_held();
}

unsigned long oogie_hle_count(void)
{
    return __atomic_load_n(&s_hle_count, __ATOMIC_RELAXED);
}

/* sub_001D2490_gen uploads the float4 at EDX to slot ECX. Its caller
 * still runs D3D enter/leave and handles the device lock. */
int oogie_hle_vec_setter(void)
{
    uint32_t wp, limit, slot, src, dst;
    unsigned i;

    if (s_hle < 0)
        s_hle = oogie_flag("d3d_hle", access("/app0/test-d3d-hle", F_OK) == 0);
    if (!s_hle || !guest_cpu_held())
        return 0;

    slot = g_ecx;
    src = g_edx;
    if (!src || slot >= 32u || src - (g_esp - 4u) < 4u || (g_esp - 4u) - src < 16u)
        return 0;

    wp = GW(RING_WP);
    limit = GW(RING_LIMIT);
    if (wp >= limit || limit - wp <= RECORD)
        return 0; /* ring full: the guest's own flush path must run */

    GW(RING_WP) = wp + RECORD;
    GW(wp) = TOKEN_A;
    GW(wp + 4u) = slot;
    GW(wp + 8u) = TOKEN_B;
    dst = VEC_CACHE + (slot << 4);
    for (i = 0; i < 4u; i++) {
        uint32_t value = GW(src + 4u * i);

        GW(wp + 12u + 4u * i) = value;
        GW(dst + 4u * i) = value;
    }
    g_eax = wp + RECORD;
    g_ecx = dst;
    g_esp += 4;
    __atomic_fetch_add(&s_hle_count, 1, __ATOMIC_RELAXED);
    return 1;
}
