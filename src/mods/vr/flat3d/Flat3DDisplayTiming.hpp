#pragma once

// HDMI 1.4 frame-packing display-timing support, ported from VRto3D
// (display_timing_helper.{h,cpp} + the FramePackTimingSpec table). Applies a
// custom display timing via NVIDIA NvAPI_DISP_TryCustomDisplay, AMD
// ADL_Display_ModeTimingOverride_Set, or a CRU-preconfigured ChangeDisplay-
// Settings fallback, so a 3D TV sees the HDMI 3D frame-packing InfoFrame.

#include <cstdint>
#include <string>

#include "Flat3DShaders.hpp" // Flat3DOutputMode

namespace vrmod::flat3d {

struct FramePackTimingSpec {
    uint32_t active_w;       // visible horizontal pixels
    uint32_t active_h;       // visible vertical pixels (both eyes + gap)
    uint32_t per_eye_h;      // single-eye height
    uint32_t gap_pixels;     // blanking gap between the two eyes
    float    refresh_hz;

    uint16_t h_total;
    uint16_t h_front_porch;
    uint16_t h_sync_width;
    uint16_t h_back_porch;

    uint16_t v_total;
    uint16_t v_front_porch;
    uint16_t v_sync_width;
    uint16_t v_back_porch;
};

// Returns the timing for a FramePacked output mode, or nullptr otherwise.
const FramePackTimingSpec* GetFramePackTimingSpec(Flat3DOutputMode m);
inline bool IsFramePackedMode(Flat3DOutputMode m) {
    return m == Flat3DOutputMode::FRAMEPACKED_720P60
        || m == Flat3DOutputMode::FRAMEPACKED_1080P24
        || m == Flat3DOutputMode::FRAMEPACKED_1080P60;
}

// Applies/reverts a custom display timing. NVIDIA -> AMD -> CRU fallback,
// all non-fatal. Verbatim port of VRto3D's DisplayTimingHelper.
class Flat3DDisplayTiming {
public:
    Flat3DDisplayTiming() = default;
    ~Flat3DDisplayTiming();

    bool Apply(const FramePackTimingSpec& spec, int32_t display_index);
    void Revert();
    bool IsActive() const { return active_; }

    enum class Backend { None, Nvidia, Amd, CruFallback };
    Backend GetBackend() const { return backend_; }

private:
    bool TryNvidia(const FramePackTimingSpec& spec, int32_t display_index);
    bool TryAmd(const FramePackTimingSpec& spec, int32_t display_index);
    bool TryCruFallback(const FramePackTimingSpec& spec, int32_t display_index);

    void RevertNvidia();
    void RevertAmd();
    void RevertCruFallback();

    bool     active_  = false;
    Backend  backend_ = Backend::None;

    struct NvidiaState;
    struct AmdState;
    struct CruState;
    void* backend_state_ = nullptr;
};

} // namespace vrmod::flat3d
