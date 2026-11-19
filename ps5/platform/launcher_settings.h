#ifndef OOGIE_LAUNCHER_SETTINGS_H
#define OOGIE_LAUNCHER_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int gamma; /* hundredths; zero follows the existing presentation setting */
    int filtering;
    int render_height;
    int volume[3]; /* music, dialogue, effects */
    int face[4];   /* physical Cross, Circle, Square, Triangle -> Xbox A/B/X/Y */
} OogieLauncherSettings;

extern OogieLauncherSettings oogie_launcher_settings;
void oogie_launcher_settings_load(void);
int oogie_launcher_settings_save(const OogieLauncherSettings *settings);
int oogie_launcher_current_gamma(void);
int oogie_vk_filter_available(void);

#ifdef __cplusplus
}
#endif
#endif
