// D3D11Component::on_frame_flat3d — the flat-3D-monitor replacement for the
// VR submit: weaves the stereo double-wide into the game's REAL backbuffer
// and lets the game present it normally. Lives in its own TU to keep the
// upstream D3D11Component.cpp diff to a single branch line.

#include <cmath>
#include <filesystem>
#include <string>
#include <thread>

#include <utility/Logging.hpp>
#include <utility/String.hpp>

#include "Framework.hpp"
#include "../../VR.hpp"
#include "../GameDepthCapture.hpp"

#include "../D3D11Component.hpp"

namespace vrmod {
vr::EVRCompositorError D3D11Component::on_frame_flat3d(VR* vr) {
    auto& hook = g_framework->get_d3d11_hook();

    // The game's own vsync setting stays in charge by default (the VSync
    // Override applies below, once the frame params are built). Tearing does
    // not corrupt the 3D patterns — they are all screen-space — it's purely
    // a comfort/latency preference.

    auto device = hook->get_device();

    ComPtr<ID3D11DeviceContext> context{};
    device->GetImmediateContext(&context);

    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D11Texture2D> real_backbuffer{};
    swapchain->GetBuffer(0, IID_PPV_ARGS(&real_backbuffer));

    if (real_backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[Flat3D] Failed to get real backbuffer");
        return vr::VRCompositorError_None;
    }

    // The engine's double-wide stereo render target (both eyes, 2W x H).
    // Extreme compatibility mode suppresses it: the game renders ONE
    // full-width eye per frame straight into the real backbuffer (AFR is
    // forced) — read the backbuffer itself as the eye source. The eye-cache
    // copy is recorded before the repack write, so no scratch copy is needed.
    const bool extreme = vr->is_extreme_compatibility_mode_enabled();

    ComPtr<ID3D11Texture2D> double_wide{};

    if (extreme) {
        double_wide = real_backbuffer;
    } else {
        const auto ue4_texture = vr->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

        if (ue4_texture != nullptr) {
            double_wide = (ID3D11Texture2D*)ue4_texture->get_native_resource();
        }
    }

    if (double_wide == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[Flat3D] No engine render target yet");
        return vr::VRCompositorError_None;
    }

    D3D11_TEXTURE2D_DESC dw_desc{};
    double_wide->GetDesc(&dw_desc);

    const uint32_t src_eye_w = extreme ? dw_desc.Width : dw_desc.Width / 2;

    D3D11_TEXTURE2D_DESC bb_desc{};
    real_backbuffer->GetDesc(&bb_desc);

    // Keep the real-backbuffer RTV up to date (shared with on_post_present's
    // bookkeeping — flat3d skips the clear there but reuses the member).
    if (real_backbuffer.Get() != m_backbuffer.Get() || m_backbuffer_rtv == nullptr) {
        m_backbuffer = real_backbuffer.Get();
        m_backbuffer_rtv.Reset();

        if (FAILED(device->CreateRenderTargetView(real_backbuffer.Get(), nullptr, &m_backbuffer_rtv))) {
            SPDLOG_ERROR_EVERY_N_SEC(1, "[Flat3D] Failed to create backbuffer RTV");
            m_backbuffer_rtv.Reset();
            return vr::VRCompositorError_None;
        }
    }

    // D3D11 has no SetColorSpace1 tracking — 10-bit swapchains are treated
    // as SDR (FP16 still maps to scRGB inside setup).
    if (!m_flat3d_compositor.setup(device, src_eye_w, dw_desc.Height, dw_desc.Format,
                                   bb_desc.Format, false, g_framework->get_window())) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[Flat3D] Compositor setup failed");
        return vr::VRCompositorError_None;
    }

    m_last_rendered_frame = vr->m_render_frame_count;

    // Per-frame parameters (stereo shifts, crosshair, HDR paper white...).
    auto params = vr->build_flat3d_frame_params(src_eye_w, dw_desc.Height);
    params.extreme_backbuffer_src = extreme;

    // VSync Override: force the present interval (consumed one-shot by the
    // present hook, so re-assert every frame). 0 = respect the game.
    if (params.vsync_override == 1) {
        hook->set_next_present_interval(1);
    } else if (params.vsync_override >= 2) {
        // No-Tear Fast: interval 0 with ALLOW_TEARING stripped (see the D3D12
        // component).
        hook->set_next_present_interval(0);
        hook->set_next_present_no_tearing();
    }

    // Native-stereo-fix titles render the RIGHT eye into a dedicated
    // scene-capture target (the double-wide's right half is never written).
    ComPtr<ID3D11Texture2D> right_eye_src{};
    if (params.native_stereo_layout) {
        const auto scene_capture = vr->m_fake_stereo_hook->get_render_target_manager()->get_scene_capture_render_target();
        if (scene_capture != nullptr) {
            right_eye_src = (ID3D11Texture2D*)scene_capture->get_native_resource();
        }

        // A stale capture (pre-resolution-change, mid-recreation) renders
        // into a corner of the eye — treat it as missing (stable mono) until
        // its size matches again.
        if (right_eye_src != nullptr) {
            D3D11_TEXTURE2D_DESC sc_desc{};
            right_eye_src->GetDesc(&sc_desc);
            if (sc_desc.Width != src_eye_w || sc_desc.Height != dw_desc.Height) {
                SPDLOG_INFO_EVERY_N_SEC(1, "[Flat3D] Scene capture {}x{} != eye {}x{} — waiting for recreation",
                                        sc_desc.Width, sc_desc.Height, src_eye_w, dw_desc.Height);
                right_eye_src.Reset();
            }
        }
    }

    // The redirected game UI layer (Slate/UMG). Null when redirection is off
    // or the game draws UI into the scene — the compositor skips it then.
    ID3D11ShaderResourceView* ui_srv = nullptr;
    const auto ui_target = vr->m_fake_stereo_hook->get_render_target_manager()->get_ui_target();

    if (ui_target != nullptr) {
        m_engine_ui_ref.set((ID3D11Texture2D*)ui_target->get_native_resource(),
                            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM);
        ui_srv = m_engine_ui_ref;

        // The UI target must match the size the ENGINE draws Slate at — its
        // own requested resolution (the swapchain itself is held at native by
        // the 3D Display output override). After an in-game resolution change
        // it goes stale and Slate draws cropped; ask the hook to recreate it
        // (once per size).
        if (auto* ui_native = (ID3D11Texture2D*)ui_target->get_native_resource(); ui_native != nullptr) {
            uint32_t expect_w = bb_desc.Width;
            uint32_t expect_h = bb_desc.Height;

            if (const auto& d3d11_hook = g_framework->get_d3d11_hook(); d3d11_hook != nullptr) {
                if (d3d11_hook->get_engine_believed_width() != 0 && d3d11_hook->get_engine_believed_height() != 0) {
                    expect_w = d3d11_hook->get_engine_believed_width();
                    expect_h = d3d11_hook->get_engine_believed_height();
                }
            }

            D3D11_TEXTURE2D_DESC ui_desc{};
            ui_native->GetDesc(&ui_desc);

            if (ui_desc.Width != expect_w || ui_desc.Height != expect_h) {
                static uint32_t last_requested_w = 0;
                static uint32_t last_requested_h = 0;
                if (last_requested_w != expect_w || last_requested_h != expect_h) {
                    last_requested_w = expect_w;
                    last_requested_h = expect_h;
                    spdlog::info("[Flat3D] UI target {}x{} != engine viewport {}x{} — requesting recreate",
                                 ui_desc.Width, ui_desc.Height, expect_w, expect_h);
                    vr->m_fake_stereo_hook->set_should_recreate_textures(true);
                }
                ui_srv = nullptr; // skip the stale layer this frame
            }
        }
    } else {
        m_engine_ui_ref.reset();
    }

    // --- UI aspect / top-left crop -----------------------------------------
    // Each output mode gives each EYE a slice of the backbuffer. The UI overlay
    // is reconciled to that eye so it isn't squished, and under Native Render +
    // Upscale it's sampled from the top-left perceived region (see below).
    {
        const auto m = (flat3d::Flat3DOutputMode)params.mode;
        const bool sbs_like = m == flat3d::Flat3DOutputMode::SBS ||
                              m == flat3d::Flat3DOutputMode::LEIA_SR ||
                              m == flat3d::Flat3DOutputMode::DUAL_DISPLAY ||
                              m == flat3d::Flat3DOutputMode::DUAL_DISPLAY_FLIP;
        const bool tab_like = m == flat3d::Flat3DOutputMode::TAB ||
                              m == flat3d::Flat3DOutputMode::FRAMEPACKED_720P60 ||
                              m == flat3d::Flat3DOutputMode::FRAMEPACKED_1080P24 ||
                              m == flat3d::Flat3DOutputMode::FRAMEPACKED_1080P60;
        const float eye_slice_w = sbs_like ? (float)bb_desc.Width * 0.5f : (float)bb_desc.Width;
        const float eye_slice_h = tab_like ? (float)bb_desc.Height * 0.5f : (float)bb_desc.Height;

        const float scene_aspect = dw_desc.Height > 0 ? (float)src_eye_w / (float)dw_desc.Height : 0.0f;

        uint32_t ui_w = 0, ui_h = 0; float ui_aspect = 0.0f;
        if (auto* ui_native = (ID3D11Texture2D*)(vr->m_fake_stereo_hook->get_render_target_manager()->get_ui_target()
                              ? vr->m_fake_stereo_hook->get_render_target_manager()->get_ui_target()->get_native_resource()
                              : nullptr)) {
            D3D11_TEXTURE2D_DESC d{};
            ui_native->GetDesc(&d);
            ui_w = d.Width; ui_h = d.Height;
            ui_aspect = ui_h > 0 ? (float)ui_w / (float)ui_h : 0.0f;
        }

        // Reconcile the UI-target aspect to the EYE-TEXTURE aspect (scene_aspect =
        // eye_w/eye_h), NOT the output slice — the overlay is drawn into the eye
        // texture, so that's the right reference. Using the slice would wrongly
        // crop packed modes (half-SbS/half-TaB) where the slice is squished and
        // the display stretches it back. No-op when ui_aspect==eye_aspect.
        if (params.ui_topleft_crop && ui_w > 0 && ui_h > 0) {
            // Native Render + Upscale: sample the PERCEIVED-client-sized top-left
            // region of the UI target (eye_slice = spoof's half-panel width x
            // full height), NOT the eye RENDER size — a game rendering its scene
            // below native (DLSS/dynamic-res) still draws UI at full perceived size.
            const float cx = eye_slice_w / (float)ui_w;
            const float cy = eye_slice_h / (float)ui_h;
            params.ui_crop_x = cx < 1.0f ? cx : 1.0f;
            params.ui_crop_y = cy < 1.0f ? cy : 1.0f;
        } else {
            // Non-full-SbS: ui_aspect ~= eye aspect => no-op central crop.
            vrmod::flat3d::compute_ui_aspect_crop(
                0, ui_aspect, scene_aspect, params.ui_crop_x, params.ui_crop_y);
        }
    }

    // SceneDepthZ: readback for the adaptive crosshair + auto-convergence,
    // and/or sampled directly by the depth-adaptive HUD mode.
    ComPtr<ID3D11Texture2D> scene_depth{};

    if (params.want_depth || params.hud_depth_mode == 1) {
        if (vr->flat3d_use_engine_depth()) {
            // Engine pool: read the game's live SceneDepthZ by name (finds a
            // load-time depth buffer the API path can't see). The pool's inline
            // FindFreeElement hook installs on the tick after activate().
            if (auto& rt_pool = vr->get_render_target_pool_hook(); rt_pool != nullptr) {
                rt_pool->activate();
                scene_depth = rt_pool->get_texture<ID3D11Texture2D>(L"SceneDepthZ");
            }
        } else {
            // API-level per-draw capture: watches the game's own D3D11 rendering
            // and identifies scene depth by per-draw counting, independent of RDG
            // resource names (which shipping titles strip/rename).
            auto& gdc = GameDepthCapture::get();
            if (auto* dev = g_framework->get_d3d11_hook()->get_device()) {
                gdc.ensure_installed_d3d11(dev);
                uint32_t rw = 0, rh = 0;
                if (double_wide != nullptr) {
                    D3D11_TEXTURE2D_DESC dwd{};
                    double_wide->GetDesc(&dwd);
                    rw = dwd.Width;
                    rh = dwd.Height;
                }
                gdc.end_frame_d3d11(rw, rh);
            }
            scene_depth = gdc.get_native<ID3D11Texture2D>();
        }
    }

    if (params.want_depth) {
        if (scene_depth != nullptr) {
            float center_uu = -1.0f;
            float nearest_uu = -1.0f;
            m_flat3d_compositor.sample_depth(context.Get(), scene_depth.Get(),
                                             vr->get_flat3d_runtime()->game_nearz.load(),
                                             &center_uu, &nearest_uu);

            auto* flat3d = vr->get_flat3d_runtime();
            flat3d->center_depth_uu.store(center_uu);
            if (nearest_uu > 0.0f) {
                flat3d->nearest_depth_uu.store(nearest_uu);
            }
        } else {
            vr->get_flat3d_runtime()->center_depth_uu.store(-1.0f);
            vr->get_flat3d_runtime()->nearest_depth_uu.store(-1.0f);
        }
    }

    // UEVR's own ImGui menu: composite it into the eyes at menu depth
    // (Framework skips its flat backbuffer draw when we consumed it).
    ID3D11Texture2D* menu_tex = g_framework->get_rendertarget_d3d11().Get();

    // 3D screenshot: begin a capture (composite hides the UEVR menu while it
    // runs) the present the hotkey / menu button fired.
    if (vr->get_flat3d_runtime()->screenshot_requested.exchange(false)) {
        m_flat3d_compositor.begin_screenshot();
    }

    float ui_coverage = 0.0f;
    const bool composited = m_flat3d_compositor.composite(context.Get(), double_wide.Get(), right_eye_src.Get(), ui_srv,
                                                          menu_tex, scene_depth.Get(), m_backbuffer_rtv.Get(),
                                                          bb_desc.Width, bb_desc.Height, params, &ui_coverage);

    vr->get_flat3d_runtime()->ui_coverage.store(ui_coverage);

    if (composited && menu_tex != nullptr) {
        g_framework->set_flat3d_menu_composited(true);
    }

    // Clear the redirected UI target after consuming it — Slate only draws
    // deltas on top, so without this the HUD accumulates ghost trails and a
    // fresh target shows uninitialized VRAM (the HMD paths clear it the same
    // way after their UI submit).
    if (m_engine_ui_ref.has_texture()) {
        // Clear empty regions to alpha = ui_invert_alpha (not 0) so the
        // UI_InvertAlpha (1-a) shader distinguishes drawn content (a=0 -> opaque)
        // from untouched empty screen (-> transparent). See the D3D12 counterpart.
        const float inv = VR::get()->get_overlay_component().get_ui_invert_alpha();
        float ui_clear[4]{0.0f, 0.0f, 0.0f, inv};
        m_engine_ui_ref.clear_rtv(ui_clear);
    }

    if (!composited) {
        return vr::VRCompositorError_None;
    }

    // Katanga handoff: after compositing (eyes now hold the final per-eye
    // images incl. UI/crosshair), publish the SbS pair to the shared texture
    // for Katanga.exe / VRScreenCap. The local window shows a left-eye preview.
    if (params.mode == (int32_t)flat3d::Flat3DOutputMode::KATANGA) {
        if (m_flat3d_katanga.ensure(device, m_flat3d_compositor.eye_width(), m_flat3d_compositor.eye_height(),
                                    m_flat3d_compositor.eye_format())) {
            const int l = params.eye_swap ? 1 : 0;
            m_flat3d_katanga.present(context.Get(),
                                     m_flat3d_compositor.eye_texture(l),
                                     m_flat3d_compositor.eye_texture(1 - l));
        }
    }

    // 3D screenshot: once both eyes have been refreshed with the UEVR menu
    // hidden (AFR-correct), save the composited pair — game HUD, crosshair,
    // cursor and convergence, but no UEVR menu — as a parallel-view PNG plus a
    // cross-view companion.
    if (m_flat3d_compositor.screenshot_capture_complete()) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const auto dir = fs::path(Framework::get_persistent_dir()) / "flat3d_screenshots";
        fs::create_directories(dir, ec);

        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t stamp[32];
        swprintf_s(stamp, L"%04u%02u%02u_%02u%02u%02u_%03u",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        const auto parallel_path = (dir / (std::wstring(L"sbs_") + stamp + L".png")).wstring();
        const auto crossview_path = (dir / (std::wstring(L"sbs_") + stamp + L"_crossview.png")).wstring();

        if (m_flat3d_compositor.save_screenshot(context.Get(), params, parallel_path, crossview_path)) {
            spdlog::info("[Flat3D] Saved 3D screenshot: {}", utility::narrow(parallel_path));
            // Success chime (mirrors VRto3D's BeepSuccess). Detached: Beep()
            // blocks for its full duration and this runs on the present thread.
            std::thread([] { Beep(400, 400); }).detach();
        } else {
            SPDLOG_ERROR_EVERY_N_SEC(1, "[Flat3D] Screenshot save failed");
        }
        m_flat3d_compositor.end_screenshot();
    }

    // Drives the one-time window activation / mouse centering. The overlay
    // component's post-submit paths early-out for non-OpenVR runtimes.
    vr->m_submitted = true;

    return vr::VRCompositorError_None;
}
} // namespace vrmod
