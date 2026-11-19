#include "iso_import.h"
#include "../platform/launcher_settings.h"
#include "../../darkness-ps5/runtime/native/xex_crypto_portable.h"

extern "C" {
#include "../platform/ps5_vk.h"
#include "input/xinput_xbox.h"
DWORD oogie_launcher_input(XBOX_INPUT_STATE *);
int sceSystemServiceHideSplashScreen(void);
int sceKernelDebugOutText(int, const char *);
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr unsigned width = 640, height = 480;
constexpr int renderHeights[] = {480, 720, 1080, 1200, 2160};
constexpr int saveRow = 10;
constexpr int settingsRows = saveRow + 1;
constexpr unsigned glyphWidth = 15, glyphHeight = 27;
constexpr uint32_t cream = 0xfffbe5b7, green = 0xffc3f83a;
constexpr char dataDir[] = "/data/oogie-ps5";
constexpr char menuAsset[] = "/app0/setup-menu.bin";
constexpr char expectedXbe[] = "9a8319c7b1cb5ee29781bf641a0cf549c221fdc57effb8f72085608019f41548";

void verifyXbe(std::span<const uint8_t> executable) {
    if (DarkRecomp::Native::XexCrypto::Sha256Hex(executable) != expectedXbe)
        throw std::runtime_error("Use the supported USA Xbox disc ISO.");
}

class Menu {
    std::vector<uint32_t> backdrop = std::vector<uint32_t>(width * height);
    std::vector<uint32_t> frame = std::vector<uint32_t>(width * height);
    std::vector<uint8_t> font = std::vector<uint8_t>(95 * glyphWidth * glyphHeight);

    void rect(unsigned x, unsigned y, unsigned w, unsigned h, uint32_t color) {
        for (unsigned row = y; row < y + h; ++row)
            std::fill_n(frame.begin() + row * width + x, w, color);
    }

    void text(unsigned x, unsigned y, std::string_view label, uint32_t color, unsigned limit = 37) {
        for (unsigned char c : label.substr(0, limit)) {
            if (c < 32 || c > 126) c = '?';
            const auto* glyph = font.data() + (c - 32) * glyphWidth * glyphHeight;
            for (unsigned row = 0; row < glyphHeight; ++row)
                for (unsigned col = 0; col < glyphWidth; ++col)
                    if (glyph[row * glyphWidth + col] > 100)
                        frame[(y + row) * width + x + col] = color;
            x += glyphWidth - 2;
        }
    }

    void present() {
        ps5_vk_present_guest(reinterpret_cast<const uint8_t*>(frame.data()), width * 4, 4);
    }

public:
    bool load() {
        FILE* file = fopen(menuAsset, "rb");
        if (!file) file = fopen("/data/homebrew/PPSA90030/setup-menu.bin", "rb");
        if (!file) return false;
        bool ok = fread(backdrop.data(), sizeof(uint32_t), backdrop.size(), file) == backdrop.size() &&
                  fread(font.data(), 1, font.size(), file) == font.size();
        fclose(file);
        return ok;
    }

    void settings(const OogieLauncherSettings& draft, int selected, bool failed) {
        frame = backdrop;
        rect(16, 253, 200, 106, 0xff110c12);
        rect(18, 34, 422, 409, 0xff110c12);
        text(30, 45, "SETTINGS", green);
        const int gamma = draft.gamma ? draft.gamma : oogie_launcher_current_gamma();
        const int brightness = static_cast<int>(std::round(100 * std::pow(0.5, gamma / 100.0 - 1)));
        const char* actions[] = {"A", "B", "X", "Y"};
        const std::string rows[] = {
            "Brightness          " + std::to_string(brightness) + "%",
            std::string("Texture filtering   ") + (!oogie_vk_filter_available() ? "Unavailable" :
                                                    draft.filtering ? "Enhanced" : "Original"),
            "Music               " + std::to_string(draft.volume[0]) + "%",
            "Dialogue            " + std::to_string(draft.volume[1]) + "%",
            "Sound effects       " + std::to_string(draft.volume[2]) + "%",
            std::string("Cross               ") + actions[draft.face[0]],
            std::string("Circle              ") + actions[draft.face[1]],
            std::string("Square              ") + actions[draft.face[2]],
            std::string("Triangle            ") + actions[draft.face[3]],
            "Render resolution   " + std::to_string(draft.render_height) + "p",
            "SAVE AND BACK",
        };
        for (int i = 0; i < settingsRows; ++i) {
            const unsigned y = 81 + i * 29;
            if (i == selected) rect(25, y, 408, 27, 0xff354404);
            text(30, y, rows[i], i == selected ? green : cream, 31);
        }
        text(30, 400, failed ? "Could not save. Try again." : "Left / Right   Circle cancels", cream);
        present();
    }

    void draw(bool installed, int selected, std::string_view status = {}, unsigned percentage = 0,
              bool preparing = false, bool choosing = false) {
        frame = backdrop;
        // The supplied mockup has labels painted into it. Cover that area
        // with a dark panel and render the actual selection independently.
        rect(16, 253, 185, 108, 0xff110c12);
        const unsigned row = 257 + selected * 34;
        if (!preparing) {
            rect(24, row, 170, 31, 0xff354404);
            rect(24, row, 170, 1, green);
            rect(24, row + 30, 170, 1, green);
            text(28, row + 2, ">", green);
        }
        text(45, 259, preparing ? "PREPARING" : choosing ? "SELECT ISO" :
             installed ? "PLAY GAME" : "PREPARE GAME", cream, 12);
        if (!preparing)
            text(45, 293, choosing ? "BACK" : "SETTINGS", cream);
        if (!status.empty()) {
            rect(16, 363, 493, 33, 0xff110c12);
            text(24, 365, status, cream);
        }
        if (preparing) {
            rect(16, 400, 249, 10, 0xff332a36);
            rect(16, 400, 249 * percentage / 100, 10, green);
        }
        present();
    }
};

std::vector<fs::path> sources() {
    const std::vector<fs::path> roots{"/mnt/usb0", "/mnt/usb1", "/app0",
                                     "/data/homebrew/PPSA90030", dataDir};
    auto list = Oogie::findGameIsos(roots);
    // Some loaders deny directory enumeration while allowing known paths.
    for (const auto& root : roots) {
        auto path = root / "Oogie.iso";
        if (Oogie::isReadableIso(path) &&
            std::find(list.begin(), list.end(), path) == list.end())
            list.push_back(path);
    }
    // The launcher can see the installed ISO through both the app mount
    // and the underlying title directory. Prefer the app mount in the picker.
    if (std::find(list.begin(), list.end(), fs::path("/app0/Oogie.iso")) != list.end())
        std::erase(list, fs::path("/data/homebrew/PPSA90030/Oogie.iso"));
    return list;
}

enum Button : unsigned { Up = 1, Down = 2, Accept = 4, Back = 8, Left = 16, Right = 32 };

unsigned buttons() {
    XBOX_INPUT_STATE state{};
    if (oogie_launcher_input(&state)) return 0;
    const auto& pad = state.Gamepad;
    unsigned pressed = 0;
    if ((pad.wButtons & XBOX_GAMEPAD_DPAD_UP) || pad.sThumbLY > 18000) pressed |= Up;
    if ((pad.wButtons & XBOX_GAMEPAD_DPAD_DOWN) || pad.sThumbLY < -18000) pressed |= Down;
    if (pad.wButtons & XBOX_GAMEPAD_DPAD_LEFT) pressed |= Left;
    if (pad.wButtons & XBOX_GAMEPAD_DPAD_RIGHT) pressed |= Right;
    if (pad.bAnalogButtons[XBOX_BUTTON_A]) pressed |= Accept;
    if (pad.bAnalogButtons[XBOX_BUTTON_B]) pressed |= Back;
    return pressed;
}

void prepare(Menu& menu, const fs::path& iso) {
    menu.draw(false, 0, "Reading Xbox disc...", 0, true);
    const auto plan = Oogie::inspectGameIso(iso, verifyXbe);
    auto last = Clock::now() - std::chrono::seconds(1);
    Oogie::extractGameIso(plan, fs::path(dataDir) / "game",
        [&](uint64_t done, uint64_t total, const std::string&) {
            const auto now = Clock::now();
            if (now - last < std::chrono::milliseconds(100) && done != total) return;
            last = now;
            const auto percentage = total ? static_cast<unsigned>(done * 100 / total) : 0;
            menu.draw(false, 0, "Copying game files... " + std::to_string(percentage) + "%", percentage, true);
        });
}
}

extern "C" int oogie_game_setup(int installed) {
    oogie_launcher_settings_load();
    Menu menu;
    if (!menu.load() || !ps5_vk_init()) {
        sceKernelDebugOutText(0, "[SETUP] Menu artwork or Vulkan unavailable\n");
        return installed;
    }
    sceSystemServiceHideSplashScreen();
    sceKernelDebugOutText(0, "[SETUP] Launcher ready\n");
    unsigned previous = buttons();
    int selected = 0;
    std::string status;
    bool editing = false, saveFailed = false;
    int setting = 0;
    OogieLauncherSettings draft{};
    std::vector<fs::path> choices;
    size_t choice = 0;
    for (;;) {
        const bool choosing = !choices.empty();
        if (choosing) {
            status = std::to_string(choice + 1) + "/" + std::to_string(choices.size()) +
                     " " + choices[choice].filename().string();
        }
        if (editing) menu.settings(draft, setting, saveFailed);
        else menu.draw(installed, selected, status, 0, false, choosing);
        unsigned pressed = 0;
        while (!pressed) {
            usleep(16000);
            const unsigned current = buttons();
            pressed = current & ~previous;
            previous = current;
        }
        if (editing) {
            if (pressed & Back) {
                editing = false;
                continue;
            }
            if (pressed & Up) setting = (setting + settingsRows - 1) % settingsRows;
            if (pressed & Down) setting = (setting + 1) % settingsRows;
            if (setting == saveRow && (pressed & Accept)) {
                saveFailed = !oogie_launcher_settings_save(&draft);
                editing = saveFailed;
            }
            const int direction = (pressed & Right) ? 1 : (pressed & Left) ? -1 : 0;
            if (!direction) continue;
            if (setting == 0) {
                const int gamma = draft.gamma ? draft.gamma : oogie_launcher_current_gamma();
                draft.gamma = std::clamp(gamma - 5 * direction, 100, 240);
            } else if (setting == 1 && oogie_vk_filter_available()) {
                draft.filtering = !draft.filtering;
            } else if (setting >= 2 && setting <= 4) {
                int& volume = draft.volume[setting - 2];
                volume = std::clamp(volume + 5 * direction, 0, 100);
            } else if (setting >= 5 && setting <= 8) {
                int& action = draft.face[setting - 5];
                const int next = (action + direction + 4) % 4;
                auto other = std::find(std::begin(draft.face), std::end(draft.face), next);
                std::swap(action, *other);
            } else if (setting == saveRow - 1) {
                const auto current = std::find(std::begin(renderHeights), std::end(renderHeights),
                                               draft.render_height);
                const int index = static_cast<int>(current - std::begin(renderHeights));
                const int last = static_cast<int>(std::size(renderHeights)) - 1;
                draft.render_height = renderHeights[std::clamp(index + direction, 0, last)];
            }
            continue;
        }
        if (pressed & (Up | Down)) selected = 1 - selected;
        if (!choosing && selected == 1 && (pressed & Accept)) {
            draft = oogie_launcher_settings;
            editing = true;
            saveFailed = false;
            setting = 0;
            continue;
        }
        if (choosing && (pressed & Left))
            choice = (choice + choices.size() - 1) % choices.size();
        if (choosing && (pressed & Right))
            choice = (choice + 1) % choices.size();
        if (choosing && ((pressed & Back) || ((pressed & Accept) && selected))) {
            choices.clear();
            status.clear();
            selected = 0;
            continue;
        }
        if (!(pressed & Accept)) continue;
        if (installed) {
            while (buttons() & Accept) usleep(16000);
            sceKernelDebugOutText(0, "[SETUP] Play selected\n");
            return 1;
        }
        try {
            if (!choosing) {
                choices = sources();
                choice = 0;
                if (choices.empty()) {
                    status = "Place Oogie.iso on USB or in app.";
                    continue;
                }
                if (choices.size() > 1) continue;
            }
            prepare(menu, choices[choice]);
            installed = true;
            status = "Ready. Press Cross to play.";
            sceKernelDebugOutText(0, "[SETUP] ISO import complete\n");
        } catch (const std::exception& e) {
            status = e.what();
            const std::string log = "[SETUP] " + status + "\n";
            sceKernelDebugOutText(0, log.c_str());
        }
        choices.clear();
        previous = buttons();
    }
}
