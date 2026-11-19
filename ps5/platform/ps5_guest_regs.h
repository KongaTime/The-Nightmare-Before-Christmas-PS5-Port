#ifndef OOGIE_PS5_GUEST_REGS_H
#define OOGIE_PS5_GUEST_REGS_H

#include <stdint.h>

/* One compiler-rt TLS object per guest thread. The lifted functions, kernel
 * bridges and interrupt delivery all share these lvalues. */
typedef struct {
    uint32_t reg_eax, reg_ecx, reg_edx, reg_esp;
    uint32_t reg_ebx, reg_esi, reg_edi;
    uint32_t reg_fs_base, reg_seh_ebp, reg_ebp, reg_mxcsr;
    uint16_t reg_fp_control_word, reg_fp_cc;
    int reg_fp_top, reg_fp_cmp, reg_df;
    double reg_fp_stack[8];
    /* Vector registers were left as separate TLS objects, so every use paid a
     * full emulated-TLS resolve instead of riding the one pointer the lifted
     * functions already hold. Storage only: each use casts to the type the
     * generated code expects, so this header needs no knowledge of it. The
     * alignment is the vector width; 8 MMX registers are 8 bytes each. */
    _Alignas(16) unsigned char reg_xmm[8][16];
    _Alignas(8)  unsigned char reg_mm[8][8];
} OogieGuestRegisters;

#ifdef __cplusplus
extern "C" {
#endif
extern __thread OogieGuestRegisters oogie_guest_regs;
#ifdef __cplusplus
}
#endif

/* Host replacement for a hot guest setter; see ps5_hle.c. Declared here
 * because the generated chunks are compiled with this header force-included
 * and the guard is injected by tools/guest_register_overlay.py. */
int oogie_hle_vec_setter(void);
int oogie_ps5_d3d_cs_direct(uint32_t target, OogieGuestRegisters *regs);
typedef struct {
    uint64_t wall, cpu;
    unsigned kind;
} OogieFenceTiming;
OogieFenceTiming oogie_fence_begin(unsigned kind);
void oogie_fence_end(OogieFenceTiming *timing);
int oogie_hle_lock_off(void);

#define OOGIE_GUEST_REGS oogie_guest_regs

#define g_eax     (OOGIE_GUEST_REGS.reg_eax)
#define g_ecx     (OOGIE_GUEST_REGS.reg_ecx)
#define g_edx     (OOGIE_GUEST_REGS.reg_edx)
#define g_esp     (OOGIE_GUEST_REGS.reg_esp)
#define g_ebx     (OOGIE_GUEST_REGS.reg_ebx)
#define g_esi     (OOGIE_GUEST_REGS.reg_esi)
#define g_edi     (OOGIE_GUEST_REGS.reg_edi)
#define g_fs_base (OOGIE_GUEST_REGS.reg_fs_base)
#define g_seh_ebp (OOGIE_GUEST_REGS.reg_seh_ebp)

#define g_ebp             (OOGIE_GUEST_REGS.reg_ebp)
#define g_mxcsr           (OOGIE_GUEST_REGS.reg_mxcsr)
#define g_fp_control_word (OOGIE_GUEST_REGS.reg_fp_control_word)
#define g_fp_cc           (OOGIE_GUEST_REGS.reg_fp_cc)
#define g_fp_top          (OOGIE_GUEST_REGS.reg_fp_top)
#define g_fp_cmp          (OOGIE_GUEST_REGS.reg_fp_cmp)
#define g_df              (OOGIE_GUEST_REGS.reg_df)
#define g_fp_stack        (OOGIE_GUEST_REGS.reg_fp_stack)

/* RecompXmm / RecompMmx come from the generated headers, which are included
 * after this one; the cast happens where the macro is used. */
#define g_xmm0 (*(RecompXmm *)(void *)OOGIE_GUEST_REGS.reg_xmm[0])
#define g_xmm1 (*(RecompXmm *)(void *)OOGIE_GUEST_REGS.reg_xmm[1])
#define g_xmm2 (*(RecompXmm *)(void *)OOGIE_GUEST_REGS.reg_xmm[2])
#define g_xmm3 (*(RecompXmm *)(void *)OOGIE_GUEST_REGS.reg_xmm[3])
#define g_xmm4 (*(RecompXmm *)(void *)OOGIE_GUEST_REGS.reg_xmm[4])
#define g_xmm5 (*(RecompXmm *)(void *)OOGIE_GUEST_REGS.reg_xmm[5])
#define g_xmm6 (*(RecompXmm *)(void *)OOGIE_GUEST_REGS.reg_xmm[6])
#define g_xmm7 (*(RecompXmm *)(void *)OOGIE_GUEST_REGS.reg_xmm[7])

#define g_mm0 (*(RecompMmx *)(void *)OOGIE_GUEST_REGS.reg_mm[0])
#define g_mm1 (*(RecompMmx *)(void *)OOGIE_GUEST_REGS.reg_mm[1])
#define g_mm2 (*(RecompMmx *)(void *)OOGIE_GUEST_REGS.reg_mm[2])
#define g_mm3 (*(RecompMmx *)(void *)OOGIE_GUEST_REGS.reg_mm[3])
#define g_mm4 (*(RecompMmx *)(void *)OOGIE_GUEST_REGS.reg_mm[4])
#define g_mm5 (*(RecompMmx *)(void *)OOGIE_GUEST_REGS.reg_mm[5])
#define g_mm6 (*(RecompMmx *)(void *)OOGIE_GUEST_REGS.reg_mm[6])
#define g_mm7 (*(RecompMmx *)(void *)OOGIE_GUEST_REGS.reg_mm[7])

#endif
