#pragma once

// Borderlands 4 reticle layer (Borderlands4.exe only).
//
// BL4's crosshair is Coherent Prysm HTML drawn into the game UI, so it reaches VR only on the flat UI quad, centred
// in the view. The profile Lua sets the game's crosshair colours to key colours (shaders/bl4_reticle_key.fx); this
// feature takes only the key-coloured pixels from the centre of the game UI texture, recolours them to the player's
// real crosshair colours in a small OpenXR swapchain shown as its own quad layer at the gun's aim point (posed from
// the live controller pose every frame), and erases just those pixels from the UI image - everything else near the
// centre (item cards, prompts, damage numbers) stays on the HUD. Being an OpenXR layer it is composited after the
// game's tonemapper, exactly like the HUD, so it keeps the HUD's real colours.
//
// The profile Lua drives it through uevr.params.vr.set_mod_value("BL4_ReticleLayer", "<csv>") every tick:
//   active, distance_cm, origin_x, origin_y, origin_z, dir_x, dir_y, dir_z, crop_px, scale, pose_type
//   [, crop_dx, crop_dy [, neutral r,g,b, enemy r,g,b, friendly r,g,b]]
//   active      1 = show the layer and move the key-coloured crosshair out of the UI, 0 = normal HUD
//   distance_cm distance from the aim origin to the hit point along dir (game units)
//   origin      aim origin (muzzle) in the right motion controller component's local space (game units, UE axes)
//   dir         aim direction in the same local space (UE axes, normalised by the backend)
//   crop_px     side of the square crop in UI pixels
//   scale       size multiplier on top of the crosshair's angular size on the UI quad
//   pose_type   0 = controller grip pose, 1 = aim pose (whichever the game's motion controller component follows)
//   crop_dx/dy  optional crop centre offset from the UI centre in UI pixels
//   colours     the player's real crosshair colours per key category, in the UI texture's encoding (0-1)
// The value is never written to or read from the config, and it goes inactive when Lua stops updating it (script
// reset, error), so the HUD always comes back.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

#include <glm/glm.hpp>
#include <utility/Module.hpp>

#include "Mod.hpp"
#include "../GameSpecific.hpp"

namespace vrmod::bl4_reticle {
inline bool is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && uevr::games::is_borderlands4_executable_path(*exe_path);
    }();

    return result;
}

// the reticle swapchain is this size; the crop is clamped to it
constexpr uint32_t SWAPCHAIN_SIZE = 512;
// Lua updates every engine tick; older than this = treat as inactive
constexpr auto STALE_AFTER = std::chrono::milliseconds{500};

struct State {
    bool active{false};
    float distance_cm{0.0f};
    glm::vec3 origin_ue{0.0f};
    glm::vec3 dir_ue{1.0f, 0.0f, 0.0f};
    float crop_px{256.0f};
    float scale{1.0f};
    int pose_type{0};
    float crop_dx{0.0f};
    float crop_dy{0.0f};
    // real crosshair colours for the key categories: neutral, enemy, friendly
    glm::vec3 colors[3]{{1.0f, 1.0f, 1.0f}, {1.0f, 0.42f, 0.45f}, {0.52f, 0.97f, 0.71f}};
};

class StateValue : public IModValue {
public:
    explicit StateValue(std::string_view config_name)
        : m_config_name{config_name}
    {
    }

    bool draw(std::string_view) override { return false; }
    void draw_value(std::string_view) override {}
    void config_load(const utility::Config&, bool) override {}
    void config_save(utility::Config&) override {}

    void set(const std::string& value) override {
        float v[22]{};
        const auto count = std::sscanf(value.c_str(), "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f",
            &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12],
            &v[13], &v[14], &v[15], &v[16], &v[17], &v[18], &v[19], &v[20], &v[21]);

        State parsed{};
        if (count >= 11) {
            parsed.active = v[0] != 0.0f;
            parsed.distance_cm = v[1];
            parsed.origin_ue = {v[2], v[3], v[4]};
            parsed.dir_ue = {v[5], v[6], v[7]};
            parsed.crop_px = v[8];
            parsed.scale = v[9];
            parsed.pose_type = (int)v[10];
            if (count >= 13) {
                parsed.crop_dx = v[11];
                parsed.crop_dy = v[12];
            }
            if (count >= 22) {
                for (int i = 0; i < 3; ++i) {
                    const glm::vec3 c{v[13 + i * 3], v[14 + i * 3], v[15 + i * 3]};
                    if (std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z)) {
                        parsed.colors[i] = glm::clamp(c, glm::vec3{0.0f}, glm::vec3{1.0f});
                    }
                }
            }
        }

        const auto finite = std::isfinite(parsed.distance_cm) && std::isfinite(parsed.crop_px) && std::isfinite(parsed.scale) &&
            std::isfinite(parsed.origin_ue.x) && std::isfinite(parsed.origin_ue.y) && std::isfinite(parsed.origin_ue.z) &&
            std::isfinite(parsed.dir_ue.x) && std::isfinite(parsed.dir_ue.y) && std::isfinite(parsed.dir_ue.z);
        if (!finite || glm::length(parsed.dir_ue) < 1.0e-4f || parsed.distance_cm <= 0.0f || parsed.crop_px < 8.0f) {
            parsed.active = false;
        }

        std::scoped_lock _{m_mtx};
        m_state = parsed;
        m_raw = value;
        m_updated = std::chrono::steady_clock::now();
    }

    std::string get() const override {
        std::scoped_lock _{m_mtx};
        return m_raw;
    }

    std::string get_config_name() const override { return m_config_name; }
    std::string_view get_config_name_view() const override { return m_config_name; }

    // the state to use this frame: inactive outside Borderlands 4 or when Lua has stopped updating it
    State snapshot() const {
        if (!is_current_game()) {
            return State{};
        }

        std::scoped_lock _{m_mtx};
        if (!m_state.active || std::chrono::steady_clock::now() - m_updated > STALE_AFTER) {
            return State{};
        }

        return m_state;
    }

private:
    const std::string m_config_name;
    mutable std::mutex m_mtx{};
    State m_state{};
    std::string m_raw{};
    std::chrono::steady_clock::time_point m_updated{};
};

// what the last UI copy cropped, for the layer built at submit time (AFR copies the UI on right-eye frames only)
struct CropInfo {
    bool valid{false};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t ui_width{0};
    uint32_t ui_height{0};
};
}
