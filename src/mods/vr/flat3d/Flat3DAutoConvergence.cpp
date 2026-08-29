#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

#include "Flat3DAutoConvergence.hpp"

namespace vrmod::flat3d {

Flat3DAutoConvergence::Output Flat3DAutoConvergence::update(
    float nearest_z_uu, float separation,
    float manual_conv_m, float conv_floor_m, float w2m,
    const Settings& s) {

    Output out{};
    out.convergence_m = manual_conv_m;

    if (!s.enabled) {
        reset();
        return out; // snap back to the manual value
    }

    if (w2m <= 0.0f || separation <= 0.0f) {
        return out;
    }

    // --- temporal median prefilter -------------------------------------------
    // A single-frame near outlier (particle, muzzle flash, camera clip) can
    // otherwise slam convergence to the floor before the EMA can resist it.
    if (nearest_z_uu > 0.0f) {
        m_z_hist[m_z_hist_i] = nearest_z_uu;
        m_z_hist_i = (m_z_hist_i + 1) % kZHist;
        if (m_z_hist_n < kZHist) {
            ++m_z_hist_n;
        }
    }

    float z_median = -1.0f;
    if (m_z_hist_n > 0) {
        float sorted[kZHist];
        std::copy(m_z_hist, m_z_hist + m_z_hist_n, sorted);
        std::nth_element(sorted, sorted + m_z_hist_n / 2, sorted + m_z_hist_n);
        z_median = sorted[m_z_hist_n / 2];
    }

    // --- input EMA (VRto3D constants: fast approach, slow relax, deadband) --
    if (z_median > 0.0f) {
        if (m_z_ema_uu <= 0.0f) {
            m_z_ema_uu = z_median;
        } else {
            const float rel = std::fabs(z_median - m_z_ema_uu) / m_z_ema_uu;
            constexpr float kDeadband = 0.005f;

            if (rel > kDeadband) {
                // "up" = disparity rising = object got CLOSER (z decreased).
                const float alpha = (z_median < m_z_ema_uu) ? 0.20f : 0.05f;
                m_z_ema_uu += (z_median - m_z_ema_uu) * alpha;
            }
        }
    }

    if (m_z_ema_uu <= 0.0f) {
        return out; // no depth signal yet
    }

    // --- target convergence --------------------------------------------------
    const float manual_conv_uu = manual_conv_m * w2m;
    const float z = m_z_ema_uu;

    // Pop-out (crossed) disparity of the nearest object at a given
    // convergence, as a fraction of eye width — POSITIVE when the object is
    // nearer than the screen plane. Background disparity is `separation` and
    // is invariant under convergence, so the whole family is simply
    //     d(conv) = (separation/2) * (conv/z - 1)
    // and the convergence that puts the nearest object exactly at the budget
    // has the closed form conv = z * (1 + 2*target/separation).
    const float half_sep = separation * 0.5f;

    float conv_target_uu = manual_conv_uu;
    const float d_at_manual = half_sep * (manual_conv_uu / z - 1.0f);

    if (d_at_manual > s.target_disparity) {
        conv_target_uu = z * (1.0f + 2.0f * s.target_disparity / separation);
        conv_target_uu = std::min(conv_target_uu, manual_conv_uu); // ceiling
    }

    // Near clamp: the larger of the near-plane-derived floor (caller supplies
    // nearz*1.5) and the user's minimum — the AUTO pull-in never drags the
    // screen plane below this even when a close object would ask for it.
    const float conv_pre_floor_uu = conv_target_uu;
    const float conv_floor_uu = std::max(conv_floor_m, s.min_convergence_m) * w2m;
    conv_target_uu = std::max(conv_target_uu, conv_floor_uu);

    // --- output smoothing ----------------------------------------------------
    // Smoothed in 1/convergence space: disparity is linear in 1/conv, so a
    // meter-space lerp accelerates perceptually as convergence gets small —
    // the "lunges way too far" failure mode.
    const float smoothing = std::clamp(s.smoothing, 0.005f, 0.25f);
    const float target_inv = w2m / conv_target_uu; // = 1 / conv_target_m

    if (m_inv_conv <= 0.0f) {
        m_inv_conv = target_inv;
    } else {
        m_inv_conv += (target_inv - m_inv_conv) * smoothing;
    }

    out.convergence_m = std::min(1.0f / std::max(m_inv_conv, 1e-6f), manual_conv_m);

    if (s.logging && (++m_frames % 60) == 0) {
        // Full decision trace: the raw input, the filtered z the solve used,
        // the pop-out it computed at manual convergence vs the target budget,
        // and where the target landed before/after the floor clamp.
        spdlog::info("[Flat3D][autoconv] z_in={:.1f} z_med={:.1f} z_ema={:.1f}uu sep={:.4f} d_manual={:.4f} target={:.4f} "
                     "conv_target={:.3f}m (pre-floor {:.3f}m) -> conv={:.3f}m (manual {:.3f}m, floor {:.3f}m) "
                     "w2m={:.0f}",
                     nearest_z_uu, z_median, m_z_ema_uu, separation, d_at_manual, s.target_disparity,
                     conv_target_uu / w2m, conv_pre_floor_uu / w2m, out.convergence_m,
                     manual_conv_m, conv_floor_uu / w2m, w2m);
    }

    return out;
}

} // namespace vrmod::flat3d
