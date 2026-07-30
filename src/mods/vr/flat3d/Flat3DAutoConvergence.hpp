#pragma once

// Auto-convergence control loop for the flat 3D monitor mode — the analog of
// VRto3D's auto-depth (hmd_device_driver.cpp FeedAutoDepthSample), with the
// actuator changed from IPD to convergence. API-agnostic pure logic; the
// nearest-scene-depth input comes from the compositors' SceneDepthZ readback.
//
// Control law: choose convergence so the NEAREST significant object's screen
// disparity does not exceed target_disparity (fraction of eye width), while
// separation is scaled by conv/manual_conv so the BACKGROUND disparity stays
// locked at the user's at-manual calibration. Under that constraint the
// pop-out is d = (k/manual)*(conv/z - 1) with k = sep_uu*P00/4, giving the
// closed-form target conv = z * (1 + target*manual/k).
// The manual convergence value is the CEILING; auto only pulls closer, and
// snaps back to manual on disable — VRto3D manual_depth_ semantics.

namespace vrmod::flat3d {

class Flat3DAutoConvergence {
public:
    struct Settings {
        bool enabled{false};
        float target_disparity{0.005f}; // fraction of eye width, 0.001..0.03
        float smoothing{0.08f};         // output lerp per frame, 0.005..0.25
        float min_convergence_m{0.5f};  // floor for the AUTO pull-in only
        bool logging{false};
    };

    struct Output {
        float convergence_m{1.0f};
        // Separation multiplier that keeps the BACKGROUND disparity locked
        // at its at-manual-convergence value while the screen plane pulls in
        // (bg disparity is sep/conv, so scale = conv/manual_conv). 1.0 when
        // auto-convergence is idle.
        float depth_scale{1.0f};
    };

    // nearest_z_uu: robust nearest scene depth this frame (UE units, < 0 =
    // no valid sample). sep_uu: effective separation in UE units. p00: game
    // projection [0][0] (= 1/tan_half_h). manual_conv_m: the user's value
    // (ceiling). conv_floor_m: near-plane clamp. w2m: world-to-meters.
    Output update(float nearest_z_uu, float sep_uu, float p00,
                  float manual_conv_m, float conv_floor_m, float w2m,
                  const Settings& s);

    void reset() {
        m_z_ema_uu = -1.0f;
        m_inv_conv = -1.0f;
        m_frames = 0;
        m_z_hist_n = 0;
        m_z_hist_i = 0;
    }

private:
    float m_z_ema_uu{-1.0f}; // asymmetric EMA of the median-filtered nearest depth
    float m_inv_conv{-1.0f}; // smoothed output in 1/convergence space (< 0 = uninitialized)
    unsigned m_frames{0};

    // Short temporal median over the raw nearest-depth samples: disparity
    // spikes from single-frame outliers (particles, weapon flashes, camera
    // clips) must not yank the control loop.
    static constexpr int kZHist = 5;
    float m_z_hist[kZHist]{};
    int m_z_hist_n{0};
    int m_z_hist_i{0};
};

} // namespace vrmod::flat3d
