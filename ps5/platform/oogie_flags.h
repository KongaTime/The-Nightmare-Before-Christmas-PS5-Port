/* Project Oogie on PS5 -- switches for port experiments.
 *
 * The planned performance work lands as a batch in ONE binary and is switched
 * on one key at a time from the console, so a measurement costs a launch
 * instead of a build plus an FTP cycle.
 *
 * Values live in [oogie] in /data/oogie-ps5/settings.ini:
 *
 *     [oogie]
 *     frame_hist=1        frame-period histogram at the flip (default on)
 *
 * A key that is missing takes the caller's default, and an environment
 * variable of the same name that is already set wins over the file (that is
 * pc_settings_int's own rule), so a harness keeps full control.
 *
 * Each key is read once and cached: a settings lookup in a hot path would cost
 * what the flag is meant to save.
 */
#ifndef OOGIE_FLAGS_H
#define OOGIE_FLAGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The integer value of [oogie] <key>, or `def` when it is absent. Names are
 * case-insensitive; the result is cached after the first call. */
int oogie_flag(const char *key, int def);

#ifdef __cplusplus
}
#endif

#endif /* OOGIE_FLAGS_H */
