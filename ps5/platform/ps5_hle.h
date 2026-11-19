/* Host-side replacements for the guest's D3D8 state setters.
 *
 * The sets of functions that dominate the guest's call count are small
 * wrappers: take the device lock, drop a record into the push-buffer ring, take
 * the lock off again. The ring, its write pointer and its limit are plain guest
 * memory, so the whole record can be written here instead of by emulated code.
 *
 * Everything is behind [oogie] d3d_hle (default off). With it off these
 * functions return 0, the generated body runs exactly as before, and the only
 * cost is one call and one cached flag read.
 */
#ifndef OOGIE_PS5_HLE_H
#define OOGIE_PS5_HLE_H

/* sub_001D2490_gen: upload float4 at EDX into vector slot ECX.
 * Returns 1 when it handled the call, in which case the guest return word has
 * already been popped and the caller must not fall through to the original. */
int oogie_hle_vec_setter(void);

/* 1 when [oogie] lock_elide is on: the generated setters skip the device
 * lock/unlock indirect calls. Off by default. */
int oogie_hle_lock_off(void);

/* Number of calls handled here, for the status line. */
unsigned long oogie_hle_count(void);

#endif /* OOGIE_PS5_HLE_H */
