#include "Framework.hpp"

#include <algorithm>
#include <cmath>

#include <hooks/D3D11Hook.hpp>
#include <hooks/D3D12Hook.hpp>

#include "../../VR.hpp"
#include "../flat3d/Flat3DShaders.hpp"

#include "Flat3D.hpp"

namespace runtimes {
namespace {
// Each output mode gives each EYE a slice of the composited output. When the
// game renders a DOUBLE-WIDE frame (full-SbS: a 3840x1080 = 32:9 render that
// gets split into two 16:9 halves) the per-eye render must carry the half's
// aspect, or the composite squishes it 2x. SbS splits width, TaB splits height.
//
// But we must NOT split a HALF-packed target: half-SbS renders a normal 16:9
// frame, packs each eye into a 960x1080 half, and the 3D display's own hardware
// STRETCHES each half back to full width — there the eye is correctly the full
// backbuffer width and the composite squish is undone by the display. Splitting
// it would shrink the eye to 8:9 and halve its horizontal resolution.
//
// The two cases are told apart by the RESULT: splitting a double-wide frame
// yields a normal landscape per-eye aspect (~16:9); "splitting" a half-packed
// frame yields an implausible portrait/ultrawide aspect. Only split when the
// result stays in a plausible single-eye landscape band. Interlaced /
// checkerboard / anaglyph / Katanga carry a full-frame eye, so never split.
void apply_eye_split(uint32_t& w, uint32_t& h) {
    if (w == 0 || h == 0) {
        return;
    }

    using Mode = vrmod::flat3d::Flat3DOutputMode;
    const auto m = (Mode)VR::get()->flat3d_output_mode_value();

    const bool sbs_like = m == Mode::SBS || m == Mode::LEIA_SR ||
                          m == Mode::DUAL_DISPLAY || m == Mode::DUAL_DISPLAY_FLIP;
    const bool tab_like = m == Mode::TAB || m == Mode::FRAMEPACKED_720P60 ||
                          m == Mode::FRAMEPACKED_1080P24 || m == Mode::FRAMEPACKED_1080P60;

    // Plausible per-eye aspect band for a real display. Outside it, the "split"
    // result is nonsense => the frame is half-packed, leave it (display stretch
    // handles the un-squish). Keep dimensions even so the double-wide and
    // interlaced/checkerboard patterns still divide cleanly.
    constexpr float kMinAspect = 1.2f; // narrower (portrait-ish) => half-SbS
    constexpr float kMaxAspect = 2.5f; // wider (ultrawide-ish)   => half-TaB

    if (sbs_like) {
        const float split_aspect = ((float)w * 0.5f) / (float)h;
        if (split_aspect >= kMinAspect && split_aspect <= kMaxAspect) {
            w = std::max<uint32_t>(128, (w / 2) & ~1u);
        }
    } else if (tab_like) {
        const float split_aspect = (float)w / ((float)h * 0.5f);
        if (split_aspect >= kMinAspect && split_aspect <= kMaxAspect) {
            h = std::max<uint32_t>(128, (h / 2) & ~1u);
        }
    }
}
} // namespace

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

    // An explicit render-resolution percentage normally does NOT size this
    // target: it drives a screen-percentage cvar (VR::flat3d_apply_screen_percentage),
    // which is what actually makes the engine render the scene smaller, and this
    // target stays native so the engine's viewport and the surface it renders
    // into agree. Only the LEGACY stage sizes it here — that is the mode where a
    // smaller target can receive a 1:1 corner of a full-size render (the
    // top-left crop), which is why it is not the default.
    const auto scale = VR::get()->flat3d_render_scale();

    if (scale > 0.0f) {
        if (native_w == 0 || native_h == 0) {
            // Output hold not armed yet — the current backbuffer is the best
            // native estimate; the per-frame poll re-derives once it is.
            const auto size = g_framework->get_rt_size();
            native_w = (uint32_t)size.x;
            native_h = (uint32_t)size.y;
        }

        // The legacy stage is the exception: it sizes THIS target by the
        // percentage, which is what titles that honour neither screen-percentage
        // cvar are left with (Jedi Survivor).
        const float target_scale = VR::get()->flat3d_render_scale_sizes_target() ? scale : 1.0f;

        // Keep dimensions even so the SbS double-wide and interlaced /
        // checkerboard patterns divide cleanly.
        this->w = std::max<uint32_t>(128, ((uint32_t)std::lround(native_w * target_scale)) & ~1u);
        this->h = std::max<uint32_t>(128, ((uint32_t)std::lround(native_h * target_scale)) & ~1u);

        // Give each eye its per-mode slice so the composite doesn't stretch it.
        apply_eye_split(this->w, this->h);
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

    // The game requests a resolution that fills the (forced) output window, so
    // its aspect is the WHOLE output's — split it down to the per-eye slice
    // (e.g. a 3840x1080 SbS window renders each eye at 1920x1080, not 3840).
    apply_eye_split(this->w, this->h);

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
