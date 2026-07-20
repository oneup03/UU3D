// D3D12Component::on_frame_flat3d — flat-3D-monitor replacement for the VR
// submit (see D3D11Component_Flat3D.cpp for the D3D11 counterpart). Own TU
// to keep the upstream D3D12Component.cpp diff to a single branch line.

#include <filesystem>

#include <wincodec.h>
#include <../../directxtk12-src/Inc/ScreenGrab.h>

#include <utility/Logging.hpp>
#include <utility/String.hpp>

#include "Framework.hpp"
#include "../../VR.hpp"
#include "../GameDepthCapture.hpp"

#include "../D3D12Component.hpp"

namespace vrmod {
vr::EVRCompositorError D3D12Component::on_frame_flat3d(VR* vr) {
    auto& hook = g_framework->get_d3d12_hook();

    // Leave the game's vsync alone (interlaced modes need tear-free scanout).

    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    // DSV-observer depth: snapshot the live scene depth at the API level (no
    // engine hook, sees depth allocated at any time). Only the flat3d on_frame
    // path registers this observer, so there is no contention over the slot.
    const bool wants_dsv_depth = vr->flat3d_wants_dsv_depth();
    m_flat3d_depth_observer.set_ue5_rdg_depth_capture_enabled(wants_dsv_depth);
    hook->set_depth_stencil_observer(wants_dsv_depth ? &m_flat3d_depth_observer : nullptr);

    ComPtr<ID3D12Resource> real_backbuffer{};
    if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer))) ||
        real_backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[Flat3D] Failed to get real backbuffer");
        return vr::VRCompositorError_None;
    }

    // Extreme compatibility mode suppresses the double-wide: the game renders
    // ONE full-width eye per frame straight into the real backbuffer (AFR is
    // forced) — read the backbuffer itself as the eye source. The eye-cache
    // copy is recorded before the repack write in the same command list.
    const bool extreme = vr->is_extreme_compatibility_mode_enabled();

    ComPtr<ID3D12Resource> double_wide{};

    if (extreme) {
        double_wide = real_backbuffer;
    } else {
        const auto ue4_texture = vr->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

        if (ue4_texture != nullptr) {
            double_wide = (ID3D12Resource*)ue4_texture->get_native_resource();
        }
    }

    if (double_wide == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[Flat3D] No engine render target yet");
        return vr::VRCompositorError_None;
    }

    const auto dw_desc = double_wide->GetDesc();
    const auto bb_desc = real_backbuffer->GetDesc();

    const auto eye_w = (uint32_t)(extreme ? dw_desc.Width : dw_desc.Width / 2);
    const auto eye_h = (uint32_t)dw_desc.Height;

    // The observer selects a DSV/RDG candidate by matching the scene source
    // extent.
    if (wants_dsv_depth) {
        m_flat3d_depth_observer.set_depth_trace_expected_extent((uint32_t)dw_desc.Width, dw_desc.Height);
    }

    if (!m_flat3d_compositor.setup(device, eye_w, eye_h, dw_desc.Format, bb_desc.Format, hook->is_swapchain_pq())) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[Flat3D] Compositor setup failed");
        return vr::VRCompositorError_None;
    }

    m_last_rendered_frame = vr->m_render_frame_count;

    auto params = vr->build_flat3d_frame_params(eye_w, eye_h);
    params.extreme_backbuffer_src = extreme;

    // VSync Override: force the present interval (consumed one-shot by the
    // present hook, so re-assert every frame). 0 = respect the game.
    if (params.vsync_override == 1) {
        hook->set_next_present_interval(1);
    } else if (params.vsync_override >= 2) {
        hook->set_next_present_interval(0);
    }

    // Native-stereo-fix titles render the RIGHT eye into a dedicated
    // scene-capture target (the double-wide's right half is never written).
    ComPtr<ID3D12Resource> right_eye_src{};
    if (params.native_stereo_layout) {
        const auto scene_capture = vr->m_fake_stereo_hook->get_render_target_manager()->get_scene_capture_render_target();
        if (scene_capture != nullptr) {
            right_eye_src = (ID3D12Resource*)scene_capture->get_native_resource();
        }

        // A stale capture (pre-resolution-change, mid-recreation) renders
        // into a corner of the eye — treat it as missing (stable mono) until
        // its size matches again.
        if (right_eye_src != nullptr) {
            const auto sc_desc = right_eye_src->GetDesc();
            if (sc_desc.Width != eye_w || sc_desc.Height != eye_h) {
                SPDLOG_INFO_EVERY_N_SEC(1, "[Flat3D] Scene capture {}x{} != eye {}x{} — waiting for recreation",
                                        sc_desc.Width, sc_desc.Height, eye_w, eye_h);
                right_eye_src.Reset();
            }
        }
    }

    // Redirected game UI layer (may be null).
    ID3D12Resource* ui_tex = nullptr;
    const auto ui_target = vr->m_fake_stereo_hook->get_render_target_manager()->get_ui_target();

    if (ui_target != nullptr) {
        ui_tex = (ID3D12Resource*)ui_target->get_native_resource();
    }

    // The UI target must match the size the ENGINE draws Slate at — its own
    // requested resolution (the swapchain itself is held at native by the
    // 3D Display output override). After an in-game resolution change the
    // target goes stale and Slate draws cropped; ask the hook to recreate it
    // (once per size — same trigger the HMD paths fire via their
    // UI-swapchain size checks).
    if (ui_tex != nullptr) {
        uint64_t expect_w = bb_desc.Width;
        uint32_t expect_h = bb_desc.Height;

        if (const auto& d3d12_hook = g_framework->get_d3d12_hook(); d3d12_hook != nullptr) {
            if (d3d12_hook->get_engine_believed_width() != 0 && d3d12_hook->get_engine_believed_height() != 0) {
                expect_w = d3d12_hook->get_engine_believed_width();
                expect_h = d3d12_hook->get_engine_believed_height();
            }
        }

        const auto ui_desc = ui_tex->GetDesc();
        if (ui_desc.Width != expect_w || ui_desc.Height != expect_h) {
            static uint64_t last_requested_w = 0;
            static uint32_t last_requested_h = 0;
            if (last_requested_w != expect_w || last_requested_h != expect_h) {
                last_requested_w = expect_w;
                last_requested_h = expect_h;
                spdlog::info("[Flat3D][D3D12] UI target {}x{} != engine viewport {}x{} — requesting recreate",
                             ui_desc.Width, ui_desc.Height, expect_w, expect_h);
                vr->m_fake_stereo_hook->set_should_recreate_textures(true);
            }
            ui_tex = nullptr; // skip the stale layer this frame
        }
    }

    // SceneDepthZ for the adaptive crosshair / auto-convergence and/or the
    // depth-adaptive HUD mode.
    ComPtr<ID3D12Resource> scene_depth{};

    // State the depth resource sits in outside our copies: UE keeps pooled
    // SceneDepthZ in a depth-read state, while the DSV-observer snapshot is an
    // owned plain copy (no DEPTH_READ capability) parked in PSR|NPSR.
    constexpr auto kPoolDepthState = D3D12_RESOURCE_STATE_DEPTH_READ |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto kShaderReadState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto scene_depth_state = kPoolDepthState;

    if (params.want_depth || params.hud_depth_mode == 1) {
        switch (vr->flat3d_depth_source()) {
        case VR::FLAT3D_DEPTH_DSV_OBSERVER:
            if (wants_dsv_depth) {
                // Owned snapshot from the D3D12Hook DSV/barrier observer. No
                // engine hook (safe where the pool's FindFreeElement hook
                // crashes) and visible even when the game allocated depth once
                // at load. The observer captures one verified window per frame;
                // re-arm after consuming this frame's snapshot.
                scene_depth = m_flat3d_depth_observer.captured_depth_snapshot();
                scene_depth_state = kShaderReadState;
                m_flat3d_depth_observer.request_ue5_rdg_depth_capture();
            }
            break;

        case VR::FLAT3D_DEPTH_ENGINE_POOL:
            // Engine pool: read the game's live SceneDepthZ by name. Because it
            // reads UE's own render-target pool it finds a depth buffer allocated
            // once at load — which the API-level path never observes being created
            // (e.g. SMT5V). The pool's inline FindFreeElement hook installs on the
            // engine tick after activate(), so the first frames may be null.
            if (auto& rt_pool = vr->get_render_target_pool_hook(); rt_pool != nullptr) {
                rt_pool->activate();
                scene_depth = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");
            }
            break;

        default: {
            // API-level per-draw capture: watches the game's own D3D12 rendering
            // and identifies scene depth by per-draw counting, independent of RDG
            // resource names (which shipping titles strip/rename). No engine hook,
            // so it is safe on every title but blind to depth created before our
            // D3D hook installed.
            auto& gdc = GameDepthCapture::get();
            if (auto* dev = g_framework->get_d3d12_hook()->get_device()) {
                gdc.ensure_installed_d3d12(dev);
                uint32_t rw = 0, rh = 0;
                if (double_wide != nullptr) {
                    const auto dwd = double_wide->GetDesc();
                    rw = (uint32_t)dwd.Width;
                    rh = (uint32_t)dwd.Height;
                }
                gdc.end_frame_d3d12(rw, rh);
            }
            scene_depth = gdc.get_native<ID3D12Resource>();
            // Only trust the published depth for the CPU readback if it was
            // selected by real draw attribution. On a loading screen the scene
            // stops rendering and GameDepthCapture falls back to a size-matched
            // registry resource whose live GPU state we cannot know; issuing our
            // COPY_SOURCE barrier against it with the wrong StateBefore fails the
            // command-list Close and used to leave the compositor flickering for
            // the rest of the session. Skip it — auto-convergence / adaptive
            // crosshair simply hold their last values until real depth returns.
            if (scene_depth != nullptr && !gdc.last_publish_via_draws()) {
                scene_depth = nullptr;
            }
            break;
        }
        }
    }

    float center_uu = -1.0f;
    float nearest_uu = -1.0f;
    float ui_coverage = 0.0f;

    // UEVR's own ImGui menu: composite it into the eyes at menu depth
    // (Framework skips its flat backbuffer draw when we consumed it).
    ID3D12Resource* menu_tex = g_framework->get_rendertarget_d3d12().Get();

    if (!m_flat3d_compositor.composite(double_wide.Get(), right_eye_src.Get(), ui_tex, menu_tex, real_backbuffer.Get(),
                                       (uint32_t)bb_desc.Width, bb_desc.Height, params,
                                       scene_depth.Get(), vr->get_flat3d_runtime()->game_nearz.load(),
                                       &center_uu, &nearest_uu, &ui_coverage, g_framework->get_window(),
                                       scene_depth_state)) {
        return vr::VRCompositorError_None;
    }

    vr->get_flat3d_runtime()->ui_coverage.store(ui_coverage);

    if (menu_tex != nullptr) {
        g_framework->set_flat3d_menu_composited(true);
    }

    if (params.want_depth) {
        auto* flat3d = vr->get_flat3d_runtime();
        flat3d->center_depth_uu.store(center_uu);
        if (nearest_uu > 0.0f) {
            flat3d->nearest_depth_uu.store(nearest_uu);
        }
    }

    // Katanga handoff via the D3D11On12 bridge. Uses the composited per-eye
    // D3D12 textures; the local window shows a left-eye preview.
    if (params.mode == (int32_t)flat3d::Flat3DOutputMode::KATANGA) {
        if (m_flat3d_katanga12.ensure(device, hook->get_command_queue(),
                                      m_flat3d_compositor.eye_width(), m_flat3d_compositor.eye_height(),
                                      m_flat3d_compositor.eye_format())) {
            const int l = params.eye_swap ? 1 : 0;
            m_flat3d_katanga12.present(m_flat3d_compositor.eye_texture(l),
                                       m_flat3d_compositor.eye_texture(1 - l));
        }
    }

    // 3D screenshot: save the engine double-wide (canonical SbS pair) as PNG.
    if (vr->get_flat3d_runtime()->screenshot_requested.exchange(false)) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const auto dir = fs::path(Framework::get_persistent_dir()) / "flat3d_screenshots";
        fs::create_directories(dir, ec);

        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t name[64];
        swprintf_s(name, L"sbs_%04u%02u%02u_%02u%02u%02u_%03u.png",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        const auto path = (dir / name).wstring();

        // The engine double-wide sits in RENDER_TARGET between our copies.
        const auto hr = DirectX::SaveWICTextureToFile(
            hook->get_command_queue(), double_wide.Get(),
            GUID_ContainerFormatPng, path.c_str(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (SUCCEEDED(hr)) {
            spdlog::info("[Flat3D] Saved 3D screenshot: {}", utility::narrow(path));
        } else {
            SPDLOG_ERROR_EVERY_N_SEC(1, "[Flat3D] Screenshot save failed (hr=0x{:x})", (uint32_t)hr);
        }
    }

    // Drives the one-time window activation / mouse centering; overlay
    // component post-submit paths early-out for non-OpenVR runtimes.
    vr->m_submitted = true;

    return vr::VRCompositorError_None;
}
} // namespace vrmod
