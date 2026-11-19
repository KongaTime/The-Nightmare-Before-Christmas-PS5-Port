/* M9: Vulkan (RADV) presentation on the console. See ps5_vk.c. */
#ifndef OOGIE_PS5_VK_H
#define OOGIE_PS5_VK_H

#include <stdint.h>

/* Bring Vulkan up and take over the display. 1 on success; 0 leaves the
 * VideoOut path in charge (no RADV linked, or a step failed). */
int ps5_vk_init(void);
int ps5_vk_ready(void);
void ps5_vk_release_launcher_display(void);

/* Present the guest's 640x480 colour surface (bpp 2, 4, or 8 for a 2x-wide
 * anti-aliased surface). 0 on success. */
int ps5_vk_present_guest(const uint8_t *src, uint32_t pitch, uint32_t bpp);

/* M9 step 2: register the GPU render back end with the push-buffer executor
 * (OOGIE_VK_BACKEND=0 keeps the CPU rasteriser). 1 when it is active; from
 * then on the back end presents at each flip. */
int ps5_vk_start_backend(void);
int ps5_vk_backend_active(void);

#endif
