// fefw-ps5 — the floating-point environment a title starts in.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PS5 starts a title with flush-to-zero (MXCSR bit 15) and denormals-are-zero (bit 6) on; code
// built here expects the IEEE state, and the title CRT calls ps5_fp_ieee() as the first thing
// _start does. The SDK declares it but no stub library in the payload SDK exports it, so the
// definition comes from us — weak, so a platform implementation would win if one is ever linked.
//
// This matters for the port beyond this title: nxrecomp's output and nxrt both assume IEEE
// denormals and no FTZ, and a lifted title that ran with the console's default would drift in the
// game's float paths before any of them visibly failed.
extern "C" __attribute__((weak)) void ps5_fp_ieee(void)
{
    unsigned int control;
    __asm__ volatile("stmxcsr %0" : "=m"(control));
    control &= ~0x8040u;      // clear FTZ (bit 15) and DAZ (bit 6)
    __asm__ volatile("ldmxcsr %0" : : "m"(control));
}
