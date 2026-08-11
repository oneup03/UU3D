#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

#include "VRRuntime.hpp"

namespace runtimes {
// "Flat 3D monitor" pseudo-runtime: engages UEVR's whole stereo pipeline
// (loaded == true -> is_hmd_active()) without any headset or VR API. The
// stereo pair is composited into the game's real backbuffer by the D3D
// components (Flat3DCompositor*) instead of being submitted to a compositor,
// and the projection hook injects an off-axis shear into the GAME's own
// projection matrix rather than replacing it (game keeps its FoV).
struct Flat3D final : public VRRuntime {
    virtual ~Flat3D() {
        this->destroy();
    }

    std::string_view name() const override {
        return "Flat3D";
    }

    VRRuntime::Type type() const override {
        return VRRuntime::Type::FLAT3D;
    }

    // No blocking sync: there is no compositor to wait on. The game's own
    // Present paces the frame loop.
    VRRuntime::Error synchronize_frame(
        std::optional<uint32_t> frame_count = std::nullopt,
        SyncFrameCallsite callsite = SyncFrameCallsite::Unknown) override
    {
        this->got_first_sync = true;
        this->frame_synced = true;
        return VRRuntime::Error::SUCCESS;
    }

    // There is no HMD pose. The view path applies only the horizontal eye
    // separation from eyes[]; VR::get_position/get_rotation degrade to
    // zero/identity for unknown runtime types already.
    VRRuntime::Error update_poses(bool from_view_extensions = false, uint32_t frame_count = 0) override {
        this->got_first_poses = true;
        this->got_first_valid_poses = true;
        this->needs_pose_update = false;
        return VRRuntime::Error::SUCCESS;
    }

    // Latch which engine frame the render thread is processing (called by
    // the generic RHI command hook, same as OpenVR's render-pose latch).
    // VR::m_render_frame_count derives from this at present time — it is the
    // AFR/Synced-Sequential eye parity the compositor keys on. Without it
    // internal_render_frame_count stays 0 and the parity is stuck: every
    // frame's view is copied into the LEFT eye while the engine alternates
    // eyes (left eye flickering between both views, right eye frozen).
    void on_pre_render_render_thread(uint32_t frame_count) override {
        this->internal_render_frame_count = frame_count;
    }

    // Same latch for the paths that can't hijack the RHI command at all.
    // PreRenderViewFamily_RenderThread falls back to enqueue_render_poses when
    // the command list has no usable root (Elliot/UE5.6: "Bad root or command
    // list, falling back to Slate thread hook"), and the command-candidate
    // rejection path does too. OpenVR and OpenXR both latch the frame here for
    // exactly that reason; without it internal_render_frame_count stays 0 in
    // those titles and the eye parity is stuck as described above.
    void enqueue_render_poses(uint32_t frame_count) override {
        this->internal_render_frame_count = frame_count;
    }

    VRRuntime::Error update_render_target_size() override;

    // Per-eye render size == real backbuffer size, so each eye renders at
    // native display resolution (double-wide is 2W x H).
    uint32_t get_width() const override {
        return this->w;
    }

    uint32_t get_height() const override {
        return this->h;
    }

    VRRuntime::Error update_matrices(float nearz, float farz) override;

    void destroy() override {
        this->loaded = false;
    }

    // Called by FFakeStereoRenderingHook::calculate_stereo_projection_matrix
    // after it has injected the off-axis shear into the game's projection.
    // Publishes the final matrix (so get_projection_matrix() consumers stay
    // consistent) and captures the game's live FoV for auto-scaling.
    void set_game_projection(uint32_t eye_index, const Matrix4x4f& final_matrix,
                             float tan_half_h, float tan_half_v, float nearz);

    // --- live stereo parameters -------------------------------------------
    // Written on the game/UI threads, read on the render thread each frame.
    //
    // separation_m: EFFECTIVE eye separation in meters (user depth x FoV
    // auto-scale). convergence_m: EFFECTIVE zero-parallax distance in meters
    // (manual value or auto-convergence output).
    std::atomic<float> separation_m{0.1f};
    std::atomic<float> convergence_m{1.0f};

    // Live game-camera FoV in degrees (horizontal), sampled on the game
    // thread via APlayerCameraManager::GetFOVAngle (includes ADS zoom and
    // cine cameras). Holds the last good value when no camera is available.
    std::atomic<float> game_fov_deg{90.0f};

    // Game FoV as tan of half-angles (P00 = 1/tan_half_h), derived from
    // game_fov_deg by the projection hook each frame.
    std::atomic<float> game_tan_half_h{1.0f};
    std::atomic<float> game_tan_half_v{1.0f};
    std::atomic<float> game_nearz{0.01f};

    // Scene depths (UE units) produced by the compositor's SceneDepthZ
    // readback. < 0 while no valid sample exists. center = aim point
    // (crosshair); nearest = nearest significant object in the ROI
    // (auto-convergence input).
    std::atomic<float> center_depth_uu{-1.0f};
    std::atomic<float> nearest_depth_uu{-1.0f};

    // OpenTrack head pose (v2), smoothed. Rotation in radians, position in
    // meters, mapped to the game camera frame. Written by the UDP receiver
    // thread (VR_Flat3D.cpp), read on the render thread. Zero when tracking
    // is off / uncentered.
    std::atomic<float> head_yaw{0.0f};
    std::atomic<float> head_pitch{0.0f};
    std::atomic<float> head_roll{0.0f};
    std::atomic<float> head_x{0.0f};
    std::atomic<float> head_y{0.0f};
    std::atomic<float> head_z{0.0f};
    std::atomic<bool>  opentrack_active{false};

    // APlayerController::bShowMouseCursor, sampled on the game thread by the
    // projection hook. Drives the stereo cursor overlay.
    std::atomic<bool>  game_wants_cursor{false};

    // UGameplayStatics::IsGamePaused, sampled on the game thread by the
    // projection hook. A cursor-independent "full-screen menu / pause up"
    // signal (controller-driven pause menus don't arm a mouse cursor).
    std::atomic<bool>  game_paused{false};

    // Screen coverage of the redirected UI (fraction 0..1), produced by the
    // compositor's alpha reduction and read back with a couple frames' latency.
    // Drives the full-screen-GUI detector (high coverage == menu up).
    std::atomic<float> ui_coverage{0.0f};

    // Camera position + forward (UE units / world axes), sampled per frame
    // on the game thread. Used to derive view depth for HUD marker anchors.
    std::atomic<float> cam_x{0.0f}, cam_y{0.0f}, cam_z{0.0f};
    std::atomic<float> cam_fwd_x{1.0f}, cam_fwd_y{0.0f}, cam_fwd_z{0.0f};

    // Per-game-frame camera rotation deltas (radians) — drive the mode-1
    // HUD world/static classification (predicted screen flow).
    std::atomic<float> cam_dyaw{0.0f};
    std::atomic<float> cam_dpitch{0.0f};

    // Lateral (perpendicular-to-forward) camera translation magnitude this
    // frame, in world units. World markers parallax-slide across the screen as
    // the character walks even with no look-rotation, so this feeds the mode-1
    // classifier as a second "the world moved under the UI" signal.
    std::atomic<float> cam_dtrans_lat{0.0f};

    // World-marker HUD anchors {u, v, 1/z_uu}: the projection-call hooks
    // append to `anchors_collecting` (game thread), the per-frame sample
    // block swaps it into `anchors_published`, and the compositor params
    // copy from there each present.
    std::mutex anchors_mtx;
    std::vector<std::array<float, 3>> anchors_collecting;
    std::vector<std::array<float, 3>> anchors_published;

    // Set by the screenshot hotkey; the D3D component saves the SbS pair to a
    // PNG on the next composited frame and clears it.
    std::atomic<bool>  screenshot_requested{false};

    // Camera-identity pair detector (Synced Sequential): the forced second
    // draw of a pair runs on unticked game state, so its raw game camera
    // (pre-eye-offset view location + rotation) is bit-identical to the first
    // draw's. Each per-eye draw pushes whether its camera matched the previous
    // draw's; the present path pops one record per composited frame (FIFO
    // keeps the draw->present alignment that a latest-value read would lose).
    // When the FIFO has no record for a present, the consumer publishes
    // NAIVELY (no pair hold) — never guess an alignment. prev_cam_sig is
    // game-thread-only; the counters feed the 5s diagnostic line.
    std::mutex pair_mtx;
    // Frame-KEYED records: consumed only by the present whose latched engine
    // frame matches, so pipeline lead can never become a permanent order
    // offset (order-keyed FIFO alignment inverted every eye slot when the
    // in-flight count at engagement was odd). flags: bit0 = camera matched
    // previous draw (pair second), bit1 = the eye the draw rendered.
    struct PairRecord {
        uint32_t frame;
        uint8_t flags;
    };
    std::deque<PairRecord> pair_second_fifo;
    // Draws where the engine frame number STALLED and the per-draw eye
    // alternation took the complement path (diagnostics).
    std::atomic<uint32_t> pair_stall_count{0};
    uint8_t prev_cam_sig[48]{};
    size_t prev_cam_sig_len{0};
    uint32_t pair_push_count{0};  // draws that pushed a record (under pair_mtx)
    uint32_t pair_match_count{0}; // of those, camera matched previous draw
    uint32_t pair_pop_count{0};   // presents that consumed a record
    uint32_t pair_empty_count{0}; // presents that found the FIFO empty (naive)

    uint32_t w{0};
    uint32_t h{0};
};
} // namespace runtimes
