#include "settings_overlay.h"
#include "audio_backend.h"
#include "controller_button_names.h"
#include "controller_mapping_wizard.h"
#include "input_bindings.h"
#include "game_graphics_options.h"
#include "music_attenuation.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "wii_remote_input.h"

#include <imgui.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#endif

#include <dolphin/pad.h>

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled);
#include <dolphin/vi.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>

extern "C" int g_gxFrameCount;

// Defined in runtime/src/hle/audio/ax_mix.cpp. That header is private to the HLE
// directory and is not on this target's include path.
namespace AxDspHle {
void SetMixWorkerEnabled(bool enabled);
}

namespace settings_overlay {
namespace {

/**
 * @brief Retrieves the display name of the current graphics API.
 * @return A string representing the active backend.
 */
const char* GraphicsApiDisplayName() {
    switch (aurora_get_backend()) {
    case BACKEND_D3D11: return "Direct3D 11";
    case BACKEND_D3D12: return "Direct3D 12";
    case BACKEND_METAL: return "Metal";
    case BACKEND_VULKAN: return "Vulkan";
    case BACKEND_OPENGL: return "OpenGL";
    case BACKEND_OPENGLES: return "OpenGL ES";
    case BACKEND_WEBGPU: return "WebGPU";
    case BACKEND_NULL: return "Null";
    case BACKEND_AUTO: return "Automatic";
    }
    return "Unknown";
}

bool g_topBarVisible = false;

// Global UI scale multiplier and visibility state
float g_userUiScale = 1.0f;
static bool g_showKeyboardGuide = true;

/**
 * @brief Computes the effective UI scaling factor.
 * 
 * Combines the automatic viewport-relative base scale with the user multiplier,
 * clamped within the supported visual range.
 * @return The effective UI scaling factor.
 */
float GetUiScale() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport || viewport->Size.y <= 0.0f) return 1.0f;
    float autoBase = std::clamp(viewport->Size.y / 1080.0f, 0.75f, 2.5f);
    return std::clamp(autoBase * g_userUiScale, 0.75f, 2.5f);
}

bool g_rumbleEnabled = RuntimeConfigFile::RumbleEnabled(true);
int g_controllerPort = 0;
float g_resolutionScale = RuntimeConfigFile::ResolutionMultiplier(1.0f);
int g_audioVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::AudioVolume(1.0f) * 100.0f));
int g_musicVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::MusicVolume(1.0f) * 100.0f));
int g_soundEffectsVolumePercent =
    static_cast<int>(std::lround(RuntimeConfigFile::SoundEffectsVolume(1.0f) * 100.0f));
int g_uiVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::UiVolume(1.0f) * 100.0f));
int g_voicesVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::VoicesVolume(1.0f) * 100.0f));
bool g_audioMuted = RuntimeConfigFile::AudioMuted(false);
bool g_audioMixWorker = RuntimeConfigFile::AudioMixWorkerEnabled(true);
bool g_attenuateMusicWhenMediaPlays = RuntimeConfigFile::AttenuateMusicWhenMediaPlays(false);
int g_frameInterpolationMode = [] {
    switch (RuntimeConfigFile::FrameInterpolationFps(0)) {
    case 120:
        return 1;
    case 180:
        return 2;
    default:
        return 0;
    }
}();
int g_displayMode = [] {
    const std::string mode = RuntimeConfigFile::DisplayMode("windowed");
    if (mode == "borderless") {
        return static_cast<int>(AURORA_DISPLAY_MODE_BORDERLESS);
    }
    if (mode == "exclusive") {
        return static_cast<int>(AURORA_DISPLAY_MODE_EXCLUSIVE);
    }
    return static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED);
}();
bool g_skipUnreadyPipelines = RuntimeConfigFile::SkipUnreadyPipelines(true);
bool g_disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
bool g_showFps = RuntimeConfigFile::ShowFps(true);
uint32_t g_disabledPostProcessingPaths = RuntimeConfigFile::DisabledPostProcessingPaths(0);
std::array<int32_t, PAD_MAX_CONTROLLERS> g_configuredControllerIndices = [] {
    std::array<int32_t, PAD_MAX_CONTROLLERS> indices{};
    indices.fill(std::numeric_limits<int32_t>::min());
    return indices;
}();

using ControllerNames::kNativeButtons;
using ControllerNames::NativeButtonItem;
constexpr const auto& kControllerButtons = ControllerNames::kGameCubeButtons;

// Classic Controller Pro layout, indexed like kControllerButtons: the SNES-style
// diamond (A right, B bottom, X top, Y left) with digital bumpers driving the GC
// triggers and Z on Back/Select (the same home the NSO GC default gives it).
constexpr std::array<const char*, PAD_BUTTON_COUNT> kClassicProPreset = {
    "east",           // A
    "south",          // B
    "north",          // X
    "west",           // Y
    "start",          // Start
    "back",           // Z
    "left_shoulder",  // L
    "right_shoulder", // R
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

// PlayStation layout: bumpers drive the GC triggers, Z moves to Create/Share.
constexpr std::array<const char*, PAD_BUTTON_COUNT> kPlayStationPreset = {
    "south", "east", "west", "north", "start", "back",
    "left_shoulder", "right_shoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

struct ResolutionItem {
    const char* label;
    float scale;
};

using Clock = std::chrono::steady_clock;

constexpr auto kCursorAutoHideDelay = std::chrono::seconds(5);
Clock::time_point g_lastMouseActivity{Clock::now()};
bool g_cursorHidden = false;

constexpr std::array<std::string_view, 3> kDisplayModeConfigNames = {
    "windowed", "borderless", "exclusive",
};

uint64_t g_presentedFrame = 0;
std::atomic_bool g_strapInputAccepted = false;
std::atomic_uint64_t g_startupDismissFrame = UINT64_MAX;
constexpr uint64_t kStrapTransitionCoverFrames = 60;

constexpr std::array<ResolutionItem, 8> kResolutions = {{
    {"Auto (window size)", 0.0f}, {"Native (1x)", 1.0f}, {"1.5x", 1.5f}, {"2x", 2.0f},
    {"3x", 3.0f}, {"4x", 4.0f}, {"6x", 6.0f}, {"8x", 8.0f},
}};

constexpr std::array<uint32_t, 3> kFrameInterpolationTargetFps{0, 120, 180};

/**
 * @brief Checks if the scale is considered high resolution.
 * @param scale The resolution multiplier.
 * @return True if scale is 6x or 8x.
 */
bool IsHighResolutionScale(float scale) {
    return std::fabs(scale - 6.0f) < 0.001f || std::fabs(scale - 8.0f) < 0.001f;
}

/**
 * @brief Checks if a high framerate interpolation mode is active.
 * @return True if target FPS is above 60.
 */
bool IsHighFrameRateMode() {
    return kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)] > 60;
}

/**
 * @brief Sets and persists the game resolution scale.
 * @param scale The new resolution multiplier.
 */
void SetResolutionScale(float scale) {
    g_resolutionScale = scale;
    VISetFrameBufferScale(scale);
    RuntimeConfigFile::SetResolutionMultiplier(scale);
}

/**
 * @brief Clamps the resolution to 4x if a high framerate mode is enabled.
 */
void LimitResolutionForFrameRate() {
    if (IsHighFrameRateMode() && IsHighResolutionScale(g_resolutionScale)) {
        SetResolutionScale(4.0f);
    }
}

using ControllerNames::FindNativeButton;

/**
 * @brief Decodes a configured native button token, extracting optional thresholds.
 * @param item The native button definition.
 * @param token The configuration string representing the button.
 * @return The parsed native button identifier.
 */
uint32_t ConfiguredNativeButton(const NativeButtonItem& item, const std::string& token) {
    if (!PADIsAxisButton(item.nativeButton)) return item.nativeButton;
    const size_t separator = token.find('@');
    if (separator == std::string::npos) return item.nativeButton;
    uint32_t threshold = 0;
    const char* end = token.data() + token.size();
    const auto parsed = std::from_chars(token.data() + separator + 1, end, threshold);
    if (parsed.ec != std::errc{} || parsed.ptr != end || threshold < 1 || threshold > 100)
        return item.nativeButton;
    return PADAxisButtonIdentity(item.nativeButton) | (threshold << 8);
}

struct ControllerBindingPair {
    std::string primary;
    std::string secondary;
};

/**
 * @brief Splits a comma-separated configuration string into primary and secondary bindings.
 * @param value The configuration string.
 * @return A pair containing primary and secondary binding tokens.
 */
ControllerBindingPair SplitControllerBinding(const std::string& value) {
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return {ControllerNames::TrimToken(value), {}};
    }
    return {ControllerNames::TrimToken(value.substr(0, comma)), ControllerNames::TrimToken(value.substr(comma + 1))};
}

using ControllerNames::NativeButtonForValue;

/**
 * @brief Formats a native binding identifier into a configuration string.
 * @param binding The native button identifier.
 * @return Formatted configuration string.
 */
std::string NativeBindingConfig(uint32_t binding) {
    std::string value = NativeButtonForValue(binding).configName;
    if (PADIsAxisButton(binding)) value += '@' + std::to_string(PADAxisButtonThreshold(binding));
    return value;
}

/**
 * @brief Updates top bar visibility and ensures the controls guide reopens when the overlay appears.
 * @param visible True to show the top menu bar, false to hide it.
 */
void SetTopBarVisible(bool visible) {
    if (g_topBarVisible == visible) {
        return;
    }
    g_topBarVisible = visible;
    if (visible) {
        g_showKeyboardGuide = true;
    }
}

/**
 * @brief Applies configured controller mappings to the emulation core.
 */
void ApplyConfiguredMappings() {
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const int32_t controllerIndex = PADGetIndexForPort(port);
        if (controllerIndex == g_configuredControllerIndices[port]) {
            continue;
        }
        g_configuredControllerIndices[port] = controllerIndex;
        if (controllerIndex < 0) {
            continue;
        }
        // The [controller] bindings are positional and shared by every port, so
        // they describe whatever pad the user set them up with (usually an Xbox
        // layout: a = south). A Wii U Pro Controller has a fixed, known layout
        // (A on the east position) that aurora already maps by name; applying
        // the shared bindings on top swaps A/B and X/Y. (Wii Remotes with any
        // extension never reach the PAD layer: the game reads them through KPAD.)
        if (WiiRemoteInput::KindForPort(port) == WiiRemoteInput::Kind::WiiUPro) {
            continue;
        }

        uint32_t count = 0;
        if (PADGetButtonMappings(port, &count) == nullptr || count != PAD_BUTTON_COUNT) {
            continue;
        }
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            const auto& configured = RuntimeConfigFile::ControllerButton(i);
            if (!configured) {
                continue;
            }
            const ControllerBindingPair binding = SplitControllerBinding(*configured);
            if (const NativeButtonItem* native = FindNativeButton(binding.primary)) {
                PADSetButtonMapping(port, PADButtonMapping{ConfiguredNativeButton(*native, binding.primary), kControllerButtons[i].padButton});
            } else {
                RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                          << " button '" << binding.primary << "'" << std::endl;
            }
            uint32_t altNative = PAD_NATIVE_BUTTON_INVALID;
            if (!binding.secondary.empty()) {
                if (const NativeButtonItem* native = FindNativeButton(binding.secondary)) {
                    altNative = ConfiguredNativeButton(*native, binding.secondary);
                } else {
                    RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                              << " secondary button '" << binding.secondary << "'" << std::endl;
                }
            }
            PADSetAltButtonMapping(port, PADButtonMapping{altNative, kControllerButtons[i].padButton});
        }
    }
}

bool g_wiiRemotesEnabled = RuntimeConfigFile::WiiRemotesEnabled(true);
bool g_wiiContinuousScan = RuntimeConfigFile::WiiContinuousScanEnabled(false);

/**
 * @brief Renders the accelerometer readout and calibration menu for a Wii Remote.
 * @param port Zero-based controller port index.
 */
void DrawWiiRemoteAccelerometer(uint32_t port) {
    ImGui::SeparatorText("Accelerometer");
    float sdlG[3] = {};
    float kpad[3] = {};
    if (WiiRemoteInput::ReadAccelDebug(port, sdlG, kpad)) {
        ImGui::Text("KPAD acc: x %+.2f  y %+.2f  z %+.2f g", kpad[0], kpad[1], kpad[2]);
        ImGui::TextDisabled("Flat, buttons up: (0, -1, 0). Sideways as a wheel: (1, 0, 0); z follows the turn.");
    } else {
        ImGui::TextDisabled("No accelerometer data yet.");
    }
    // SDL's read of the remote's calibration block often times out over Bluetooth
    // and it falls back to a nominal zero point, leaving a small per-axis bias;
    // measured here with the remote at rest.
    if (WiiRemoteInput::IsAccelCalibrating()) {
        ImGui::ProgressBar(WiiRemoteInput::AccelCalibrationProgress(), ImVec2(220.0f, 0.0f), "Hold still...");
    } else if (ImGui::Button("Calibrate (remote lying flat, buttons up)")) {
        WiiRemoteInput::StartAccelCalibration(port);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Put the remote down on a flat surface with the buttons facing up and do not touch it\n"
                          "for about two seconds. Corrects the steering offset of a remote held sideways.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!RuntimeConfigFile::HasWiiAccelOffset() || WiiRemoteInput::IsAccelCalibrating());
    if (ImGui::Button("Clear calibration")) {
        WiiRemoteInput::ClearAccelCalibration();
    }
    ImGui::EndDisabled();
    if (const char* message = WiiRemoteInput::AccelCalibrationMessage()) {
        ImGui::TextWrapped("%s", message);
    } else if (RuntimeConfigFile::HasWiiAccelOffset()) {
        const std::array<double, 3> offset = RuntimeConfigFile::WiiAccelOffset();
        ImGui::TextDisabled("Stored offset: x %+.3f  y %+.3f  z %+.3f g", offset[0], offset[1], offset[2]);
    } else {
        ImGui::TextDisabled("Not calibrated (using SDL's zero point; see console.log for \"fallback accelerometer calibration\").");
    }
}

/**
 * @brief Renders the settings menu for Bluetooth Wii Remotes and Wii U Pro controllers.
 * @param selectedGamePort Zero-based controller port index currently selected for editing.
 */
void DrawWiiRemoteSettings(uint32_t selectedGamePort) {
    if (!ImGui::BeginMenu("Wii Remotes (Bluetooth)")) {
        return;
    }
    if (ImGui::Checkbox("Use Wii Remotes / Wii U Pro Controllers", &g_wiiRemotesEnabled)) {
        RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Takes effect on the next launch. Turn this off if you use a Mayflash DolphinBar.");
    }
    ImGui::TextDisabled("Pairing: Windows Settings > Bluetooth > Add device, then press 1+2");
    ImGui::TextDisabled("(or the red SYNC button) on the remote. Leave the PIN empty.");
    ImGui::TextDisabled("A remote that was paired before also needs to be turned on with 1+2/SYNC.");
    if (ImGui::Checkbox("Keep scanning for Wii Remotes (like Dolphin's Continuous Scanning)",
                        &g_wiiContinuousScan)) {
        RuntimeConfigFile::SetWiiContinuousScanEnabled(g_wiiContinuousScan);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("While no Wii controller is connected, re-check Bluetooth every 2 seconds so a\n"
                          "remote that dropped out (\"Communications with the controller have been\n"
                          "interrupted\") or was turned on after launch comes back by itself.");
    }
    // The driver hint is only read at launch, so a rescan after the user turned
    // the setting off would still re-enumerate Wii devices in this session.
    ImGui::BeginDisabled(!g_wiiRemotesEnabled);
    if (ImGui::Button("Rescan now")) {
        WiiRemoteInput::RescanNow();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (WiiRemoteInput::IsScanning()) {
        ImGui::TextDisabled("Scanning... (%u so far) - press 1+2 on the remote", WiiRemoteInput::ScanCount());
    } else {
        ImGui::TextDisabled("Not scanning");
    }
    ImGui::Separator();

    const WiiRemoteInput::Kind kind = WiiRemoteInput::KindForPort(selectedGamePort);
    ImGui::Text("Port %u: %s", static_cast<unsigned>(selectedGamePort + 1), WiiRemoteInput::KindLabel(kind));
    if (kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        WiiRemoteInput::KpadSample sample;
        if (WiiRemoteInput::ReadKpadSample(selectedGamePort, sample)) {
            // WPAD_CL_BUTTON_* bits, in the game's own layout (no mapping involved).
            const auto held = [&](uint32_t bit, const char* on, const char* off) { return (sample.clHold & bit) ? on : off; };
            ImGui::Text("Classic: %s %s %s %s  %s %s  %s %s  %s %s  %s %s %s %s", held(0x0010, "A", "a"),
                        held(0x0040, "B", "b"), held(0x0008, "X", "x"), held(0x0020, "Y", "y"), held(0x2000, "L", "l"),
                        held(0x0200, "R", "r"), held(0x0080, "ZL", "zl"), held(0x0004, "ZR", "zr"),
                        held(0x0400, "PLUS", "plus"), held(0x1000, "MINUS", "minus"), held(0x0001, "UP", "up"),
                        held(0x4000, "DOWN", "down"), held(0x0002, "LEFT", "left"), held(0x8000, "RIGHT", "right"));
            ImGui::Text("Sticks: L %+.2f %+.2f (WPAD %+d %+d)  R %+.2f %+.2f (WPAD %+d %+d)", sample.clLStick[0],
                        sample.clLStick[1], static_cast<int>(sample.clLStickRaw[0]),
                        static_cast<int>(sample.clLStickRaw[1]), sample.clRStick[0], sample.clRStick[1],
                        static_cast<int>(sample.clRStickRaw[0]), static_cast<int>(sample.clRStickRaw[1]));
            ImGui::TextDisabled("Capitals = held. The game reads this Classic Controller through KPAD, as on the");
            ImGui::TextDisabled("console: its buttons mean what the game says they mean, no mapping applies.");
        }
    }
    if (kind == WiiRemoteInput::Kind::WiiUPro) {
        if (SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(selectedGamePort))) {
            // SDL's Wii driver posts the D-pad as joystick buttons 11-14 (the
            // SDL_GAMEPAD_BUTTON_DPAD_* values) while its default HIDAPI mapping
            // expects a hat, so SDL_GetGamepadButton never sees them; read the
            // joystick directly, like the fallback in aurora's PADRead does.
            SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
            const auto rawButton = [&](int index) {
                return joystick != nullptr && SDL_GetJoystickButton(joystick, index);
            };
            ImGui::Text("Raw D-pad: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_DPAD_UP) ? "UP" : "up",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? "DOWN" : "down",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? "LEFT" : "left",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? "RIGHT" : "right");
            ImGui::Text("Raw face buttons: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_EAST) ? "A" : "a",
                        rawButton(SDL_GAMEPAD_BUTTON_SOUTH) ? "B" : "b", rawButton(SDL_GAMEPAD_BUTTON_NORTH) ? "X" : "x",
                        rawButton(SDL_GAMEPAD_BUTTON_WEST) ? "Y" : "y");
            ImGui::Text("Raw ZL/ZR: %d / %d (pressed above 0)",
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            ImGui::TextDisabled("Capitals = held. If a button never turns to capitals while physically held,");
            ImGui::TextDisabled("that press is not reaching SDL at all (a driver-level issue, not a mapping one).");
            ImGui::TextDisabled("This pad uses Nintendo's own layout (a/b/x/y as labelled); the shared");
            ImGui::TextDisabled("button mapping above does not apply to it.");
        }
    }
    if (kind == WiiRemoteInput::Kind::Remote || kind == WiiRemoteInput::Kind::RemoteWithNunchuk ||
        kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        DrawWiiRemoteAccelerometer(selectedGamePort);
    }

    ImGui::EndMenu();
}

/**
 * @brief Returns the human-readable name of an input scancode or mouse button.
 * @param scancode SDL scancode or custom negative mouse button code.
 * @return String representation of the input name.
 */
const char* KeyBindingName(int scancode) {
    switch (scancode) {
    case PAD_KEY_MOUSE_LEFT: return "Mouse left";
    case PAD_KEY_MOUSE_RIGHT: return "Mouse right";
    case PAD_KEY_MOUSE_MIDDLE: return "Mouse middle";
    case PAD_KEY_MOUSE_X1: return "Mouse side 1";
    case PAD_KEY_MOUSE_X2: return "Mouse side 2";
    case PAD_KEY_INVALID: return "Unmapped";
    default:
        return scancode >= 0 && scancode < SDL_SCANCODE_COUNT
            ? SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)) : "Unknown";
    }
}

enum class RebindKind { KeyboardButton, KeyboardAxis, Controller };
struct RebindState {
    bool active = false;
    bool openPopup = false;
    RebindKind kind{};
    uint32_t port = 0;
    uint16_t target = 0;
    bool secondary = false;
    SDL_JoystickID instance = 0;
    Clock::time_point deadline{};
    std::string label;
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    uint32_t mouse = 0;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<bool, SDL_GAMEPAD_AXIS_COUNT> axesReady{};
} g_rebind;

// Tracks if a rebind prompt was triggered from the visual guide window
static bool g_rebindFromGuide = false;

/**
 * @brief Initiates input rebinding capture for a specific target control.
 * @param kind Category of rebind (controller, keyboard button, or keyboard axis).
 * @param target Identifier of the control being rebound.
 * @param label Display label of the target control.
 * @param secondary True if capturing a secondary/alternative binding.
 */
void BeginRebind(RebindKind kind, uint16_t target, const char* label, bool secondary = false) {
    g_rebind = {};
    g_rebind.active = true;
    g_rebind.openPopup = true;
    g_rebind.kind = kind;
    g_rebind.port = static_cast<uint32_t>(g_controllerPort);
    g_rebind.target = target;
    g_rebind.secondary = secondary;
    g_rebind.label = label;
    g_rebind.deadline = Clock::now() + std::chrono::seconds(10);
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    std::copy_n(keys, std::min(count, static_cast<int>(g_rebind.keys.size())), g_rebind.keys.begin());
    g_rebind.mouse = SDL_GetMouseState(nullptr, nullptr);
    const int index = PADGetIndexForPort(g_rebind.port);
    if (kind == RebindKind::Controller && index >= 0) {
        if (auto* pad = PADGetSDLGamepadForIndex(index)) {
            g_rebind.instance = SDL_GetGamepadID(pad);
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
                g_rebind.buttons[i] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i)
                g_rebind.axesReady[i] = std::abs(static_cast<int>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i)))) < 8000;
        }
    }
}

/**
 * @brief Completes the active rebinding capture, committing the new value to configuration.
 * @param value Scancode, mouse button, or native controller button to map.
 */
void CompleteRebind(uint32_t value) {
    const auto& capture = g_rebind;
    if (capture.kind == RebindKind::Controller) {
        const int index = PADGetIndexForPort(capture.port);
        auto* pad = index >= 0 ? PADGetSDLGamepadForIndex(index) : nullptr;
        if (pad == nullptr || SDL_GetGamepadID(pad) != capture.instance) {
            g_rebind.active = false;
            return;
        }
        if (capture.secondary) PADSetAltButtonMapping(capture.port, {value, capture.target});
        else PADSetButtonMapping(capture.port, {value, capture.target});
        uint32_t count = 0, altCount = 0;
        auto* primary = PADGetButtonMappings(capture.port, &count);
        auto* alternate = PADGetAltButtonMappings(capture.port, &altCount);
        uint32_t primaryValue = PAD_NATIVE_BUTTON_INVALID, alternateValue = PAD_NATIVE_BUTTON_INVALID;
        for (uint32_t i = 0; i < count; ++i)
            if (primary[i].padButton == capture.target) primaryValue = primary[i].nativeButton;
        for (uint32_t i = 0; i < altCount; ++i)
            if (alternate[i].padButton == capture.target) alternateValue = alternate[i].nativeButton;
        std::string config = NativeBindingConfig(primaryValue);
        if (alternateValue != PAD_NATIVE_BUTTON_INVALID) config += ',' + NativeBindingConfig(alternateValue);
        for (size_t i = 0; i < kControllerButtons.size(); ++i)
            if (kControllerButtons[i].padButton == capture.target) RuntimeConfigFile::SetControllerButton(i, config);
    } else if (capture.kind == RebindKind::KeyboardButton) {
        PADSetKeyButtonBinding(capture.port, {static_cast<int32_t>(value), capture.target});
    } else {
        PADSetKeyAxisBinding(capture.port, {static_cast<int32_t>(value), capture.target, 1});
    }
    PADSerializeMappings();
    g_rebind.active = false;
}

/**
 * @brief Renders the modal prompt for capturing a new controller or keyboard binding.
 */
void DrawRebindPrompt() {
    if (g_rebind.openPopup) {
        ImGui::OpenPopup("Rebind input");
        g_rebind.openPopup = false;
    }

    if (!ImGui::BeginPopupModal("Rebind input", &g_rebind.active, ImGuiWindowFlags_AlwaysAutoResize)) {
        g_rebind.active = false;
        return;
    }

    // Apply UI scale to the rebind modal window
    ImGui::SetWindowFontScale(GetUiScale());

    if (g_rebind.active) {
        ImGui::Text("Rebind: %s", g_rebind.label.c_str());
        ImGui::TextUnformatted(g_rebind.kind == RebindKind::Controller
            ? "Press a controller button, pull a trigger, or move a stick."
            : "Press a keyboard key or click a mouse button.");
        ImGui::TextUnformatted("Release any held input first. Backspace or Delete clears the mapping.");
        ImGui::TextUnformatted("Escape can be bound. F10 is reserved for settings.");
        const float remaining = std::chrono::duration<float>(g_rebind.deadline - Clock::now()).count();
        ImGui::Text("Unmapped in %d seconds", std::max(0, static_cast<int>(std::ceil(remaining))));
        const bool clear = ImGui::Button("Clear mapping");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) g_rebind.active = false;
        // UI clicks must not become mouse bindings (buttons activate on release).
        const bool overControl = ImGui::IsAnyItemHovered();
        if (g_rebind.active && (clear || remaining <= 0.0f)) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind != RebindKind::Controller) {
            int count = 0;
            const bool* keys = SDL_GetKeyboardState(&count);
            for (int i = 1; i < std::min(count, static_cast<int>(SDL_SCANCODE_COUNT)) && g_rebind.active; ++i) {
                if (keys[i] && !g_rebind.keys[i] && i != SDL_SCANCODE_F10) CompleteRebind(i);
                g_rebind.keys[i] = keys[i];
            }
            const uint32_t mouse = SDL_GetMouseState(nullptr, nullptr);
            for (int i = 1; i <= 5 && g_rebind.active; ++i)
                if (!overControl && (mouse & ~g_rebind.mouse & (1u << (i - 1))) != 0) CompleteRebind(static_cast<uint32_t>(-i - 1));
            g_rebind.mouse = mouse;
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind == RebindKind::Controller) {
            auto* pad = SDL_GetGamepadFromID(g_rebind.instance);
            if (pad != nullptr) {
                for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && g_rebind.active; ++i) {
                    const bool pressed = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
                    if (pressed && !g_rebind.buttons[i]) CompleteRebind(i);
                    g_rebind.buttons[i] = pressed;
                }
                for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT && g_rebind.active; ++i) {
        