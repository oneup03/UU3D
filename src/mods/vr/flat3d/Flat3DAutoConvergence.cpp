#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

#include "Flat3DAutoConvergence.hpp"

namespace vrmod::flat3d {

namespace {
// --- camera-cut detection ---------------------------------------------------
// How much NEARER the raw sample has to be than the current belief to count as
// a cut rather than motion. Expressed as a RATIO (ema/z), not as the relative
// difference fabs(z-ema)/ema: approaching, that difference is 1 - z/ema, which
// is bounded above by 1 no matter how close the object gets, so any threshold
// at or above 1 built on it fires ONLY for recession - the one direction with
// nothing to protect against. 2.5 mirrors the rel > 1.5 a difference-based
// test would have wanted.
constexpr float kCutRatio = 2.5f;

// Consecutive frames past the ratio before it counts. The GPU readback is
// already a few frames behind, so every confirmation frame is one more spent at
// the wrong convergence - be stingy.
constexpr int kCutFrames = 2;

// Release rate as a fraction of the user's smoothing slider. Pulling the screen
// plane IN protects comfort and gets the full rate; easing back out has nothing
// to protect against and reads as the image drifting if it hurries.
constexpr float kReleaseRatio = 0.45f;
} // namespace

float Flat3DAutoConvergence::median_of_recent(int n) const {
    n = std::min(n, m_z_hist_n);

    if (n <= 0) {
        return -1.0f;
    }

    float recent[kZHist];
    for (int i = 0; i < n; ++i) {
        // m_z_hist_i is the NEXT write slot, so the newest sample is at i-1.
        recent[i] = m_z_hist[((m_z_hist_i - 1 - i) % kZHist + kZHist) % kZHist];
    }

    std::nth_element(recent, recent + n / 2, recent + n);
    return recent[n / 2];
}

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

    // --- camera-cut snap -----------------------------------------------------
    // An EMA tuned for continuous motion needs many frames to cross a large
    // step, and a cut to a close framing is exactly when the wrong convergence
    // is least tolerable. Detect the step and jump instead of easing.
    //
    // Tested on the RAW sample, not z_median: a 5-frame median needs 3 frames
    // before it even starts to cross a step, which is latency the smooth path
    // wants and the cut path must not pay. The persistence requirement below
    // is what rejects single-frame spikes, so the median is not needed here.
    //
    // APPROACH ONLY. Making the ratio symmetric is the obvious next step and
    // buys a worse failure than it cures: anything held close to the camera and
    // moving (a pickup animation, an NPC leaning in, a creature circling)
    // swings the near statistic past 2.5x in BOTH directions within a second,
    // and a symmetric detector answers every crossing with a hard snap - in,
    // out, in - which reads as the image thrashing. Recession has nothing to
    // protect against anyway (convergence sitting nearer than the scene needs
    // costs only positive parallax, bounded by `separation`), so it belongs on
    // the EMA's slow path. Dropping the recede half also breaks the oscillation
    // on its own: after an approach snap the EMA sits at the near value, so the
    // return trip cannot clear the ratio a second time.
    float approach = 1.0f;
    bool cut = false;

    if (nearest_z_uu > 0.0f && m_z_ema_uu > 0.0f) {
        approach = (nearest_z_uu < m_z_ema_uu) ? (m_z_ema_uu / nearest_z_uu) : 1.0f;

        if (approach > kCutRatio) {
            if (++m_approach_frames >= kCutFrames) {
                // Snap to the median of the last few RAW samples: the full
                // history still holds mostly pre-cut values (so the jump would
                // only go part of the way), and a single raw sample puts the
                // whole shot at the mercy of one frame.
                if (const float snap_z = median_of_recent(3); snap_z > 0.0f) {
                    m_z_ema_uu = snap_z;
                    // Reset the reciprocal smoother too, or it eases across the
                    // step we just jumped. < 0 makes it re-latch below.
                    m_inv_conv = -1.0f;
                    cut = true;
                }
                m_approach_frames = 0;
            }
        } else {
            m_approach_frames = 0;
        }
    }

    // --- input EMA (VRto3D constants: fast approach, slow relax, deadband) --
    if (z_median > 0.0f && !cut) {
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
        // Asymmetric, for the same reason the input EMA is - and the two are
        // not redundant: the EMA governs how fast the loop believes the scene
        // changed, this governs how fast the picture follows that belief. A
        // LARGER target reciprocal is a NEARER convergence, so that is the
        // comfort-protecting direction and the one that gets the user's rate.
        const float rate = (target_inv > m_inv_conv) ? smoothing : smoothing * kReleaseRatio;
        m_inv_conv += (target_inv - m_inv_conv) * rate;
    }

    out.convergence_m = std::min(1.0f / std::max(m_inv_conv, 1e-6f), manual_conv_m);

    // A cut is rare and is the whole point of the detector, so it logs on the
    // frame it fires rather than waiting for the periodic trace.
    if (s.logging && ((++m_frames % 60) == 0 || cut)) {
        // Full decision trace: the raw input, the filtered z the solve used,
        // the pop-out it computed at manual convergence vs the target budget,
        // and where the target landed before/after the floor clamp. approach is
        // the camera-cut ratio (ema/z, 1 = not approaching).
        spdlog::info("[Flat3D][autoconv]{} z_in={:.1f} z_med={:.1f} z_ema={:.1f}uu approach={:.2f} sep={:.4f} "
                     "d_manual={:.4f} target={:.4f} "
                     "conv_target={:.3f}m (pre-floor {:.3f}m) -> conv={:.3f}m (manual {:.3f}m, floor {:.3f}m) "
                     "w2m={:.0f}",
                     cut ? " CUT" : "", nearest_z_uu, z_median, m_z_ema_uu, approach, separation,
                     d_at_manual, s.target_disparity,
                     conv_target_uu / w2m, conv_pre_floor_uu / w2m, out.convergence_m,
                     manual_conv_m, conv_floor_uu / w2m, w2m);
    }

    return out;
}

} // namespace vrmod::flat3d
