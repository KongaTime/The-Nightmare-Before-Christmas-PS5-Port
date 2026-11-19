/* Project Oogie on PS5 — the port's settings, on the console.
 *
 * Replaces upstream port/src/pc_settings.c for this build. The interface is the
 * same (pc_settings.h) because recomp_manual.c — 5,294 lines of hand-written
 * overrides that the recompiler excludes from the generated code — reads g_pc
 * directly. Only the storage differs: a console has no working directory to
 * write beside the exe, so the file is /data/oogie-ps5/settings.ini.
 *
 * The PC file's ~60 keys describe window, monitor and Direct3D options that do
 * not exist here. This keeps the keys that still mean something on a TV
 * (aspect, FOV, cheats, volume, language) and fills the rest with fixed
 * console values, so a key the PC menu writes is still readable if a user
 * copies their settings.ini across.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "pc_settings.h"
#include "launcher_settings.h"
#include "oogie_flags.h"

PcConfig g_pc;

#define OOGIE_DATA_DIR "/data/oogie-ps5"
static char s_path[512];
static int s_loaded;

static const char *path(void)
{
    if (!s_path[0]) snprintf(s_path, sizeof s_path, "%s/settings.ini", OOGIE_DATA_DIR);
    return s_path;
}

const char *pc_settings_path(void) { return path(); }

/* ---- a small INI reader ------------------------------------------------ */

static char s_ini[64 * 1024];

static int read_ini(void)
{
    if (s_loaded) return 1;
    s_loaded = 1;
    FILE *f = fopen(path(), "rb");
    static const char *const fallback[] = {
        "/download0/oogie-ps5/settings.ini",
        "/app0/settings.ini",
    };
    for (size_t i = 0; !f && i < sizeof fallback / sizeof fallback[0]; ++i) {
        f = fopen(fallback[i], "rb");
        if (f)
            snprintf(s_path, sizeof s_path, "%s", fallback[i]);
    }
    if (!f) return 0;
    size_t n = fread(s_ini, 1, sizeof s_ini - 1, f);
    s_ini[n] = '\0';
    fclose(f);
    return 1;
}

/* Case-insensitive lookup of key in [section]. Writes the value into out. */
const char *pc_settings_get(const char *section, const char *key)
{
    static char value[256];
    if (!read_ini()) return NULL;
    char want_section[64];
    size_t i = 0;
    for (; section[i] && i < sizeof want_section - 1; i++)
        want_section[i] = (char)tolower((unsigned char)section[i]);
    want_section[i] = '\0';

    char current[64] = "";
    char *line = s_ini;
    while (line && *line) {
        char *end = strpbrk(line, "\r\n");
        char saved = 0;
        if (end) { saved = *end; *end = '\0'; }
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '[') {
            char *close = strchr(p, ']');
            if (close) {
                size_t n = 0;
                for (char *q = p + 1; q < close && n < sizeof current - 1; q++)
                    current[n++] = (char)tolower((unsigned char)*q);
                current[n] = '\0';
            }
        } else if (*p && *p != ';' && *p != '#' && !strcmp(current, want_section)) {
            char *eq = strchr(p, '=');
            if (eq) {
                char name[64];
                size_t n = 0;
                for (char *q = p; q < eq && n < sizeof name - 1; q++) {
                    if (!isspace((unsigned char)*q))
                        name[n++] = (char)tolower((unsigned char)*q);
                }
                name[n] = '\0';
                char want_key[64];
                size_t k = 0;
                for (; key[k] && k < sizeof want_key - 1; k++)
                    want_key[k] = (char)tolower((unsigned char)key[k]);
                want_key[k] = '\0';
                if (!strcmp(name, want_key)) {
                    char *v = eq + 1;
                    while (*v && isspace((unsigned char)*v)) v++;
                    size_t m = 0;
                    while (v[m] && m < sizeof value - 1) { value[m] = v[m]; m++; }
                    while (m && isspace((unsigned char)value[m - 1])) m--;
                    value[m] = '\0';
                    if (end) *end = saved;
                    return value;
                }
            }
        }
        if (end) *end = saved;
        line = end ? end + 1 : NULL;
        if (line && *line == '\n' && end && saved == '\r') line++;
    }
    return NULL;
}

int pc_settings_int(const char *section, const char *key, int def)
{
    const char *v = pc_settings_get(section, key);
    if (!v || !*v) return def;
    return (int)strtol(v, NULL, 0);
}

/* Port experiments: each key is looked up once and kept. A batch of changes
 * ships in one binary and is switched on one key at a time, so measuring costs
 * a launch rather than a rebuild. */
#define OOGIE_FLAG_MAX 24
static struct {
    char key[32];
    int val;
} s_flags[OOGIE_FLAG_MAX];
static int s_flags_n;

int oogie_flag(const char *key, int def)
{
    int i;

    for (i = 0; i < s_flags_n; i++)
        if (!strcmp(s_flags[i].key, key))
            return s_flags[i].val;
    if (s_flags_n == OOGIE_FLAG_MAX)
        return def;
    snprintf(s_flags[s_flags_n].key, sizeof s_flags[0].key, "%s", key);
    s_flags[s_flags_n].val = pc_settings_int("oogie", key, def);
    return s_flags[s_flags_n++].val;
}

/* A console writes no settings back yet: the PC menu (pc_menu.c) is not part
 * of this build, and a half-written ini would be worse than none. */
int pc_settings_set(const char *section, const char *key, const char *value)
{
    (void)section; (void)key; (void)value;
    return 0;
}

void pc_config_save_int(const char *section, const char *key, int value)
{
    (void)section; (void)key; (void)value;
}

/* ---- defaults ---------------------------------------------------------- */

void pc_config_load(void)
{
    memset(&g_pc, 0, sizeof g_pc);
    /* Console values first: a TV is 16:9, the panel is the only monitor, and
     * there is one fixed output mode. */
    g_pc.fullscreen = 1;
    g_pc.monitor = 0;
    g_pc.window_scale = 0;
    g_pc.aspect = 0;               /* 4:3: ps5_vk.c pillarboxes the frame */
    g_pc.fov = 100;
    g_pc.hide_borders = 1;
    g_pc.side_fill = 0;            /* 4:3 films get black bars */
    g_pc.vsync = 1;
    g_pc.fps_cap = 60;             /* the game's own logic rate */
    g_pc.render_scale = 480;       /* original guest render height */
    g_pc.aniso = 4;
    g_pc.aa = 0;
    g_pc.sharpen = 0;
    g_pc.mipmaps = 1;
    g_pc.hud_edges = 1;
    g_pc.tex_dump = 0;
    g_pc.ps2_hd = 0;
    g_pc.button_style = 3;         /* PlayStation prompts: DualSense is the pad */
    g_pc.audio_buffer = 1;
    g_pc.show_fps = 0;
    g_pc.rumble = 1;
    g_pc.auto_repeat = 0;
    g_pc.colour_filter = 0;
    g_pc.other_pads = 0;
    g_pc.volume = 100;
    g_pc.music_volume = 100;
    g_pc.voice_volume = 100;
    g_pc.sfx_volume = 100;
    g_pc.mute_bg = 0;
    g_pc.pause_bg = 0;
    g_pc.skip_intro = 0;
    g_pc.discord = 0;
    g_pc.language = 1;             /* English (Xbox number) */

    /* A settings.ini from the PC build (or a console one, later) can override
     * the keys that mean the same thing on a TV. */
    g_pc.aspect        = pc_settings_int("Display", "aspect", g_pc.aspect);
    g_pc.render_scale  = pc_settings_int("Graphics", "render_scale", g_pc.render_scale);
    if (g_pc.render_scale != 720 && g_pc.render_scale != 1080 &&
        g_pc.render_scale != 1200 && g_pc.render_scale != 2160)
        g_pc.render_scale = 480;
    g_pc.fov           = pc_settings_int("Display", "fov", g_pc.fov);
    g_pc.hud_edges     = pc_settings_int("Display", "hud_edges", g_pc.hud_edges) != 0;
    g_pc.hide_borders  = pc_settings_int("Display", "hide_borders", g_pc.hide_borders);
    g_pc.side_fill     = pc_settings_int("Display", "side_fill", g_pc.side_fill);
    g_pc.show_fps      = pc_settings_int("Display", "show_fps", g_pc.show_fps);
    g_pc.skip_intro    = pc_settings_int("Display", "skip_intro", g_pc.skip_intro);
    g_pc.volume        = pc_settings_int("Audio", "volume", g_pc.volume);
    g_pc.music_volume  = pc_settings_int("Audio", "music_volume", g_pc.music_volume);
    g_pc.voice_volume  = pc_settings_int("Audio", "voice_volume", g_pc.voice_volume);
    g_pc.sfx_volume    = pc_settings_int("Audio", "sfx_volume", g_pc.sfx_volume);
    g_pc.rumble        = pc_settings_int("Input", "rumble", g_pc.rumble);
    g_pc.auto_repeat   = pc_settings_int("Controller", "auto_repeat",
                             pc_settings_int("Input", "auto_repeat", g_pc.auto_repeat));
    if (g_pc.auto_repeat < 0 || g_pc.auto_repeat > 4)
        g_pc.auto_repeat = 0;
    g_pc.button_style  = pc_settings_int("Mods", "button_style", g_pc.button_style);
    g_pc.language      = pc_settings_int("Game", "language", g_pc.language);
    g_pc.cheat_health  = pc_settings_int("Cheats", "cheat_health", 0);
    g_pc.cheat_souls   = pc_settings_int("Cheats", "cheat_souls", 0);
    g_pc.cheat_exclaim = pc_settings_int("Cheats", "cheat_exclaim", 0);
    g_pc.cheat_combo   = pc_settings_int("Cheats", "cheat_combo", 0);
    g_pc.cheat_freeze  = pc_settings_int("Cheats", "cheat_freeze", 0);
    g_pc.cheat_oneshot = pc_settings_int("Cheats", "cheat_oneshot", 0);
    g_pc.cheat_speed   = pc_settings_int("Cheats", "cheat_speed", 0);
    /* Same range as upstream: a 0 here makes the camera's projection infinite. */
    if (g_pc.fov < 70 || g_pc.fov > 140) g_pc.fov = 100;
}

void pc_settings_load(void)
{
    pc_config_load();
    g_pc.gen++;
}

#define LAUNCHER_SETTINGS OOGIE_DATA_DIR "/launcher-settings.txt"
OogieLauncherSettings oogie_launcher_settings = {0, 0, 480, {100, 100, 100}, {0, 1, 2, 3}};

static int launcher_valid(const OogieLauncherSettings *s)
{
    if (s->render_height != 480 && s->render_height != 720 &&
        s->render_height != 1080 && s->render_height != 1200 &&
        s->render_height != 2160)
        return 0;
    if ((s->gamma && (s->gamma < 100 || s->gamma > 240)) ||
        s->filtering < 0 || s->filtering > 1)
        return 0;
    for (int i = 0; i < 3; ++i)
        if (s->volume[i] < 0 || s->volume[i] > 100) return 0;
    unsigned seen = 0;
    for (int i = 0; i < 4; ++i) {
        if (s->face[i] < 0 || s->face[i] > 3 || (seen & (1u << s->face[i])))
            return 0;
        seen |= 1u << s->face[i];
    }
    return 1;
}

static void launcher_apply(const OogieLauncherSettings *s)
{
    oogie_launcher_settings = *s;
    g_pc.render_scale = s->render_height;
    g_pc.music_volume = s->volume[0];
    g_pc.voice_volume = s->volume[1];
    g_pc.sfx_volume = s->volume[2];
}

void oogie_launcher_settings_load(void)
{
    oogie_launcher_settings.render_height = g_pc.render_scale;
    oogie_launcher_settings.volume[0] = g_pc.music_volume;
    oogie_launcher_settings.volume[1] = g_pc.voice_volume;
    oogie_launcher_settings.volume[2] = g_pc.sfx_volume;
    FILE *f = fopen(LAUNCHER_SETTINGS, "r");
    if (!f) return;
    OogieLauncherSettings s = oogie_launcher_settings;
    int version;
    int count = fscanf(f, "v%d %d %d %d %d %d %d %d %d %d",
        &version, &s.gamma, &s.filtering, &s.volume[0], &s.volume[1],
        &s.volume[2], &s.face[0], &s.face[1], &s.face[2], &s.face[3]);
    int valid = count == 10 && version == 1;
    if (count == 10 && version == 2)
        valid = fscanf(f, "%d", &s.render_height) == 1;
    fclose(f);
    if (valid && launcher_valid(&s)) launcher_apply(&s);
}

int oogie_launcher_settings_save(const OogieLauncherSettings *s)
{
    static const char temp[] = LAUNCHER_SETTINGS ".tmp";
    if (!launcher_valid(s)) return 0;
    FILE *f = fopen(temp, "w");
    if (!f) return 0;
    int written = fprintf(f, "v2 %d %d %d %d %d %d %d %d %d %d\n",
        s->gamma, s->filtering, s->volume[0], s->volume[1], s->volume[2],
        s->face[0], s->face[1], s->face[2], s->face[3], s->render_height);
    int closed = fclose(f);
    if (written < 0 || closed || rename(temp, LAUNCHER_SETTINGS)) {
        remove(temp);
        return 0;
    }
    launcher_apply(s);
    return 1;
}
