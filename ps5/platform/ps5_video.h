/* Oogie's Revenge PS5 (M4) - the VideoOut presentation layer.
 *
 * Two entry points, both idempotent, both safe to call from title_main (the
 * boot thread, before the guest) and from the framebuffer seams
 * (xbox_FramebufferWindowStart/Present, on the executor's thread):
 *
 *   ps5_video_init()               opens VideoOut, registers two 1920x1080
 *                                  buffers, hides the splash and flips one
 *                                  solid-colour buffer, so the TV shows a
 *                                  frame while the guest runs.
 *   ps5_video_present_guest_frame() copies the guest's 640x480 framebuffer
 *                                  (guest VA + pitch) into the other buffer
 *                                  -- centred, 2x-scaled, linear-to-tiled --
 *                                  and flips it. No-op until init succeeded.
 *
 * Returns 0 on success; on init, the failing call's return value. Every step
 * is reported to the kernel log, which is how the console side was verified.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ps5_video_init(void);
int ps5_video_present_guest_frame(uint32_t fb_va, uint32_t pitch);
int ps5_video_is_ready(void);

#ifdef __cplusplus
}
#endif
