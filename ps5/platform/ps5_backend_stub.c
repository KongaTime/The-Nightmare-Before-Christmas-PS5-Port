/* The console replacements for the PC host seams (M3).
 *
 * The PC build answers these from modules that only make sense on a desktop:
 * a D3D11 back end with its window, an SDL2 audio device, an SDL2/PS5 pad
 * handler and the Win32 front-end menu. None of them compile for the console,
 * but the runtime and the port's overrides call into all of them, so the
 * console needs an answer for each.
 *
 * Each answer here is the one that is *true* on the console in M3:
 *
 *   no GPU device yet      -> M5's Vulkan back end replaces xbox_GetD3DDevice
 *   no window yet          -> M6 replaces the framebuffer window calls
 *   no audio device yet    -> M7 replaces xa2_*
 *   no pad yet             -> M8 replaces xbox_InputGetState
 *   no front-end menu      -> the port's own overlay; the console has none
 *
 * Nothing here fakes a success. A stub that returned a plausible device
 * pointer would send the executor down a path with no renderer behind it and
 * turn "not implemented yet" into a crash deep inside the runtime; these
 * return "absent" and let the guest's own error handling run.
 */
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "pc_settings.h"
#include "launcher_settings.h"
#include "ps5_vk.h"

#include "platform/xbox_winnt.h"
#include "video/video_player.h"
#include "d3d/d3d8_xbox.h"
#include "input/xinput_xbox.h"
#include "ps5_video.h"   /* M4: VideoOut */

/* The kernel log is the reliable channel on the console (stderr lines from
 * this file were not reliably arriving); the M5 frame-path logs use it. */
extern int sceKernelDebugOutText(int channel, const char *text);

/* ── the front-end menu (pc_menu.c on the PC) ────────────────────────────
 * nv2a_pb_replay.c asks whether a menu wants to drive the push buffer, reads
 * the cursor to retitle the window, and sets a present-suppression flag while
 * a film plays. With no menu and no title bar, "no menu, no cursor" is the
 * whole truth; the film flag is honoured because it is the guest's own state,
 * not a window feature. */
int fe_menu_get_pb_state(void) { return 0; }
int g_fe_cursor;
volatile int g_suppress_present;

/* ── the buffer a frame finished in (patch flip_done) ───────────────────
 * Set by the executor, read by the back end to present the buffer the frame
 * was drawn into rather than whichever is free now. There is no back end yet,
 * so the value is recorded and unused. */
volatile uint32_t g_oogie_flip_done;

/* ── the clearance rectangle (patch clear_rect) ─────────────────────────
 * The executor publishes the rect it just cleared so the back end can clear
 * only that part of its own target. With no back end it is recorded only. */
uint32_t g_oogie_clear_rect[2];

/* Frames presented, read by the port's stall detector. */
volatile long g_oogie_presents;

/* Match the PC mixer's classification of one-shot and streamed voices. */
float oogie_apu_voice_gain(int stream, int stereo, int bin)
{
    int pct = !stream ? g_pc.sfx_volume
            : (!stereo && bin != 3) ? g_pc.voice_volume : g_pc.music_volume;
    float gain = (float)pct / 100.0f;
    return gain * gain;
}

/* The window's aspect, which the port's projection fix divides by 4:3. A
 * console title is on a 16:9 TV. */
float oogie_wide_aspect(void) { return ps5_vk_backend_active() ? 16.0f / 9.0f : 4.0f / 3.0f; }

float oogie_level_visibility_width_scale(void)
{
    if (g_pc.aspect != 1)
        return 1.0f;
    float scale = oogie_wide_aspect() / (4.0f / 3.0f);
    static int logged;
    if (scale > 1.0f && !logged) {
        logged = 1;
        sceKernelDebugOutText(0, "[WIDE] level visibility frustum widened\n");
    }
    return scale;
}

float oogie_visibility_x_scale(void)
{
    if (g_pc.aspect != 1 || !ps5_vk_backend_active())
        return 1.0f;

    return (4.0f / 3.0f) / oogie_wide_aspect();
}

/* The port's stall detector (pc_stall.c) names a guest thread that has held
 * the CPU too long. Its printing is host-side only; the console's watchdog
 * (xbox_WatchdogStart) covers the same ground. */
void pc_stall_print_thread(unsigned long tid, const char *what)
{
    (void)tid; (void)what;
}

/* ── graphics (M4: VideoOut, M5: the D3D back end) ──────────────────────
 * xbox_GetD3DDevice stays absent: the pgraph translator would draw through it
 * into a renderer that does not exist yet, and returning a hollow device
 * turns "not implemented" into a crash inside the runtime. The framebuffer
 * half (Present/Set/Start below) is real now. */
IDirect3DDevice8 *xbox_GetD3DDevice(void) { return NULL; }

/* ── the window (M6) ──────────────────────────────────────────────────── */
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    static int noted;
    (void)fb_va; (void)pitch;
    if (noted++) return;
    fprintf(stderr, "[Oogie] framebuffer window requested (M6: VideoOut)\n");
}

void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }

/* Called by the executor and by the kernel bridge when the first frame is
 * about to be produced. On the PC this opens the window and starts its
 * present thread; M6 starts VideoOut here instead. */
void xbox_FramebufferWindowStart(void)
{
    static int noted;
    if (noted++) return;
    sceKernelDebugOutText(0, "[Oogie] first frame requested (M4: VideoOut)\n");
    fprintf(stderr, "[Oogie] first frame requested (M4: VideoOut)\n");
    /* Idempotent: title_main brings VideoOut up before the guest runs; this
     * also covers a title that never calls back into the boot sequence. */
    ps5_video_init();
}

/* ── full-motion video (M6, with the presentation path) ──────────────────
 * The film player decodes into a surface, so it is part of presentation,
 * not of boot. "Nothing playing" keeps the kernel's wait loops moving. */
int xbox_VideoIsPlaying(void) { return 0; }

int xbox_VideoPlayFile(const char *host_path)
{
    fprintf(stderr, "[Oogie] film requested: %s (M6)\n", host_path ? host_path : "?");
    return 0;
}

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    (void)name; (void)max_chars;   /* no title bar on a console */
}

void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }

/* The flip: the executor calls this with the surface the frame finished in,
 * as a guest VA + pitch. M4 copies it (centred, 2x, linear-to-tiled) into a
 * VideoOut buffer and flips it; until init succeeded it is a no-op, which
 * keeps the executor's frame loop correct either way. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    static int n;
    if (n++ < 5) {
        char line[96];
        snprintf(line, sizeof line, "[Oogie] present #%d fb=0x%08X pitch=%u\n", n,
                 (unsigned)fb_va, (unsigned)pitch);
        sceKernelDebugOutText(0, line);
    }
    ps5_video_present_guest_frame(fb_va, pitch);
}

/* ── controllers (M8) ─────────────────────────────────────────────────── */
/* ERROR_DEVICE_NOT_CONNECTED is what the runtime's own XInput path returns
 * when no pad is plugged in, so the guest sees a familiar "no controller". */
#define OOGIE_DEVICE_NOT_CONNECTED 1167u

/* M8: the DualSense through scePad. The native state layout is the one
 * rexglue-sdk's prospero pad driver uses (from ps5-payload-dev/SDL's PS5
 * joystick ABI): 120 bytes, buttons first, sticks and triggers as bytes. */
typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t l2, r2;
    uint16_t pad0;
    float orientation[4], angular_velocity[3], acceleration[3];
    uint8_t touch[24];
    uint8_t connected;
    uint8_t pad1[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t count;
    uint8_t unknown[15];
} OogiePadData;
_Static_assert(sizeof(OogiePadData) == 120, "scePad state is 120 bytes");
_Static_assert(__builtin_offsetof(OogiePadData, connected) == 76, "connected at 76");

enum {
    PS_L3 = 0x0002, PS_R3 = 0x0004, PS_OPTIONS = 0x0008,
    PS_UP = 0x0010, PS_RIGHT = 0x0020, PS_DOWN = 0x0040, PS_LEFT = 0x0080,
    PS_L2 = 0x0100, PS_R2 = 0x0200, PS_L1 = 0x0400, PS_R1 = 0x0800,
    PS_TRIANGLE = 0x1000, PS_CIRCLE = 0x2000, PS_CROSS = 0x4000,
    PS_SQUARE = 0x8000, PS_TOUCHPAD = 0x100000,
};

extern int scePadInit(void);
extern int scePadOpen(int user_id, int type, int index, void *parameter);
extern int scePadGetHandle(int user_id, int type, int index);
extern int scePadReadState(int handle, OogiePadData *state);
extern int sceUserServiceInitialize(void *parameter);
extern int sceUserServiceGetInitialUser(int *user_id);

static int s_pad_handle = -1;
static int s_pad_tried;

static int pad_open(void)
{
    char line[128];
    int user = -1, r;

    if (s_pad_handle >= 0)
        return 1;
    /* Retry once a second or so: the call rate is the USB report rate. */
    if (s_pad_tried++ % 500 != 0)
        return 0;
    sceUserServiceInitialize(NULL);          /* already-initialised is fine */
    scePadInit();
    if (sceUserServiceGetInitialUser(&user) != 0 || user < 0)
        return 0;
    r = scePadOpen(user, 0, 0, NULL);
    if (r < 0)
        r = scePadGetHandle(user, 0, 0);
    if (s_pad_tried < 5000) {
        snprintf(line, sizeof line, "[Oogie] pad: user 0x%X handle %d\n", user, r);
        sceKernelDebugOutText(0, line);
    }
    if (r < 0)
        return 0;
    s_pad_handle = r;
    return 1;
}

static int16_t stick(uint8_t v, int invert)
{
    int s = ((int)v - 128) * 256;
    if (s > -2800 && s < 2800)               /* small deadzone */
        s = 0;
    if (invert)
        s = -s - 1;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

/* The game's rumble packet (USB OUT on the pad's endpoint 2, from the
 * pad_rumble patch to ohci.c): 00 06, then the low- and high-frequency motor
 * speeds as 16-bit values. The DualSense takes two 8-bit motor strengths. */
extern int scePadSetVibration(int handle, const uint8_t vibration[2]);
extern int scePadSetVibrationMode(int handle, int mode);

void oogie_pad_rumble(int port, const uint8_t *p, int len)
{
    static uint8_t last[2] = { 0xFF, 0xFF };
    static int mode_set;
    uint8_t v[2];

    if (port != 0 || !p || len < 6 || p[0] != 0 || p[1] < 6 || s_pad_handle < 0)
        return;
    if (!g_pc.rumble)
        return;
    v[0] = p[3];                             /* large motor: low frequency */
    v[1] = p[5];                             /* small motor: high frequency */
    if (v[0] == last[0] && v[1] == last[1])
        return;
    if (!mode_set) {
        scePadSetVibrationMode(s_pad_handle, 2);
        mode_set = 1;
    }
    last[0] = v[0];
    last[1] = v[1];
    scePadSetVibration(s_pad_handle, v);
}

static DWORD read_pad(DWORD dwPort, XBOX_INPUT_STATE *pState, int gameplay)
{
    static uint32_t packet;
    static int repeat_button = -1;
    static uint64_t repeat_start;
    static const unsigned face_buttons[] = {
        XBOX_BUTTON_A, XBOX_BUTTON_B, XBOX_BUTTON_X, XBOX_BUTTON_Y,
    };
    OogiePadData d;
    XBOX_GAMEPAD *g;
    uint32_t b;

    if (pState) {
        pState->dwPacketNumber = 0;
        pState->Gamepad = (XBOX_GAMEPAD){ 0 };
    }
    if (dwPort != 0 || !pState || !pad_open())
        return OOGIE_DEVICE_NOT_CONNECTED;
    if (scePadReadState(s_pad_handle, &d) != 0 || !d.connected) {
        repeat_button = -1;
        return OOGIE_DEVICE_NOT_CONNECTED;
    }

    /* The PC build's layout: Cross = A, Circle = B, Square = X, Triangle = Y,
     * L1/R1 = Black/White, L2/R2 = the triggers, Options = START, touchpad
     * press = BACK (the map). */
    b = d.buttons;
    g = &pState->Gamepad;
    if (b & PS_UP)       g->wButtons |= XBOX_GAMEPAD_DPAD_UP;
    if (b & PS_DOWN)     g->wButtons |= XBOX_GAMEPAD_DPAD_DOWN;
    if (b & PS_LEFT)     g->wButtons |= XBOX_GAMEPAD_DPAD_LEFT;
    if (b & PS_RIGHT)    g->wButtons |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (b & PS_OPTIONS)  g->wButtons |= XBOX_GAMEPAD_START;
    if (b & PS_TOUCHPAD) g->wButtons |= XBOX_GAMEPAD_BACK;
    if (b & PS_L3)       g->wButtons |= XBOX_GAMEPAD_LEFT_THUMB;
    if (b & PS_R3)       g->wButtons |= XBOX_GAMEPAD_RIGHT_THUMB;
    g->bAnalogButtons[XBOX_BUTTON_A]        = (b & PS_CROSS)    ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_B]        = (b & PS_CIRCLE)   ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_X]        = (b & PS_SQUARE)   ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_Y]        = (b & PS_TRIANGLE) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_BLACK]    = (b & PS_L1)       ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_WHITE]    = (b & PS_R1)       ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_LTRIGGER] = d.l2;
    g->bAnalogButtons[XBOX_BUTTON_RTRIGGER] = d.r2;
    g->sThumbLX = stick(d.lx, 0);
    g->sThumbLY = stick(d.ly, 1);            /* Xbox +Y is up */
    g->sThumbRX = stick(d.rx, 0);
    g->sThumbRY = stick(d.ry, 1);
    if (gameplay) {
        uint8_t physical[4];
        for (unsigned i = 0; i < 4; ++i) physical[i] = g->bAnalogButtons[face_buttons[i]];
        for (unsigned i = 0; i < 4; ++i)
            g->bAnalogButtons[face_buttons[oogie_launcher_settings.face[i]]] = physical[i];
    }
    unsigned repeat = gameplay ? (unsigned)g_pc.auto_repeat : 0;
    if (repeat && repeat <= sizeof face_buttons / sizeof face_buttons[0] &&
        g->bAnalogButtons[face_buttons[repeat - 1]]) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            uint64_t ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            if (repeat_button != (int)repeat) {
                repeat_button = (int)repeat;
                repeat_start = ms;
            }
            if (((ms - repeat_start) / 50) & 1u)
                g->bAnalogButtons[face_buttons[repeat - 1]] = 0;
        }
    } else {
        repeat_button = -1;
    }
    pState->dwPacketNumber = ++packet;
    {
        /* Confirm button traffic reaches the guest: the first few presses. */
        static uint32_t last_b;
        static int logs;
        if (b != last_b && b && logs < 12) {
            char line[96];
            logs++;
            snprintf(line, sizeof line, "[Oogie] pad buttons 0x%06X -> xbox 0x%04X\n",
                     (unsigned)b, (unsigned)g->wButtons);
            sceKernelDebugOutText(0, line);
        }
        last_b = b;
    }
    return 0;
}

DWORD oogie_launcher_input(XBOX_INPUT_STATE *state)
{
    return read_pad(0, state, 0);
}

DWORD xbox_InputGetState(DWORD port, XBOX_INPUT_STATE *state)
{
    return read_pad(port, state, 1);
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    (void)dwPort; (void)pVibration;
    return OOGIE_DEVICE_NOT_CONNECTED;
}
