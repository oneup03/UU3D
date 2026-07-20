#include "Framework.hpp"

#include <algorithm>
#include <cmath>

#include <hooks/D3D11Hook.hpp>
#include <hooks/D3D12Hook.hpp>

#include "../../VR.hpp"

#include "Flat3D.hpp"

namespace runtimes {
VRRuntime::Error Flat3D::update_render_target_size() {
    // The output is the game's own swapchain, held at the display's native
    // size (VR_Flat3D.cpp native-output block); the game's REQUESTED
    // resolution (captured pre-rewrite by the D3D hooks) is the per-eye
    // render resolution — the in-game resolution keeps controlling render
    // cost and the composite upscales to the native output.
    uint32_t req_w = 0;
    uint32_t req_h = 0;
    uint32_t native_w = 0;
    uint32_t native_h = 0;

    if (g_framework->is_dx11()) {
        if (auto& hook = g_framework->get_d3d11_hook(); hook != nullptr) {
            req_w = hook->get_game_requested_width();
            req_h = hook->get_game_requested_height();
            native_w = hook->get_forced_resize_width();
            native_h = hook->get_forced_resize_height();
        }
    } else {
        if (auto& hook = g_framework->get_d3d12_hook(); hook != nullptr) {
            req_w = hook->get_game_requested_width();
            req_h = hook->get_game_requested_height();
            native_w = hook->get_forced_resize_width();
            native_h = hook->get_forced_resize_height();
        }
    }

    // Explicit render-resolution override: a fraction of the native output
    // size, taking priority over the captured game request. This is the
    // escape hatch for games where Auto degrades to native-per-eye: a game
    // that boots already at native (or that adopts/persists the native size
    // our output hold imposes) never issues a sub-native ResizeBuffers, so
    // there is no game request to preserve.
    const auto scale = VR::get()->flat3d_render_scale();

    if (scale > 0.0f) {
        if (native_w == 0 || native_h == 0) {
            // Output hold not armed yet — the current backbuffer is the best
            // native estimate; the per-frame poll re-derives once it is.
            const auto size = g_framework->get_rt_size();
            native_w = (uint32_t)size.x;
            native_h = (uint32_t)size.y;
        }

        // Keep dimensions even so the SbS double-wide and interlaced /
        // checkerboard patterns divide cleanly.
        this->w = std::max<uint32_t>(128, ((uint32_t)std::lround(native_w * scale)) & ~1u);
        this->h = std::max<uint32_t>(128, ((uint32_t)std::lround(native_h * scale)) & ~1u);

        return VRRuntime::Error::SUCCESS;
    }

    if (req_w != 0 && req_h != 0) {
        this->w = req_w;
        this->h = req_h;
    } else {
        const auto size = g_framework->get_rt_size();
        this->w = (uint32_t)size.x;
        this->h = (uint32_t)size.y;
    }

    if (this->w == 0 || this->h == 0) {
        // Fallback until the first present has stamped the RT size.
        this->w = 1920;
        this->h = 1080;
    }

    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error Flat3D::update_matrices(float nearz, float farz) {
    // Only the eye-to-head transforms live here: pure horizontal translation
    // of +/- separation/2 (matches the OpenVR eye-to-head sign convention;
    // calculate_stereo_view_offset negates it and applies world scale).
    // projections[] is NOT built here -- the projection hook keeps the game's
    // own matrix and publishes it through set_game_projection().
    std::unique_lock __{ this->eyes_mtx };

    const auto sep = this->separation_m.load();

    // Index 0 = left, 1 = right (matches vr::Eye_Left/Right and true_index).
    this->eyes[0] = glm::identity<Matrix4x4f>();
    this->eyes[0][3] = Vector4f{-sep * 0.5f, 0.0f, 0.0f, 1.0f};

    this->eyes[1] = glm::identity<Matrix4x4f>();
    this->eyes[1][3] = Vector4f{sep * 0.5f, 0.0f, 0.0f, 1.0f};

    this->last_eye_matrix_nearz = nearz;

    return VRRuntime::Error::SUCCESS;
}

void Flat3D::set_game_projection(uint32_t eye_index, const Matrix4x4f& final_matrix,
                                 float tan_half_h, float tan_half_v, float nearz) {
    if (eye_index > 1) {
        return;
    }

    {
        std::unique_lock __{ this->projections_mtx };
        this->projections[eye_index] = final_matrix;
    }

    this->game_tan_half_h.store(tan_half_h);
    this->game_tan_half_v.store(tan_half_v);
    this->game_nearz.store(nearz);
}
} // namespace runtimes
