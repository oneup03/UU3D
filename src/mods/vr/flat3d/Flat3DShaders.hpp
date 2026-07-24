#pragma once

// Shared (D3D11/D3D12) definitions for the Flat 3D monitor compositor:
// output-mode enum, per-frame parameter block, and the repack HLSL.
//
// The repack pixel shader is a 1:1 port of VRto3D's repack shader
// (VRto3D/vrto3d/shaders/repack.frag + the HLSL mirror in
// presenter/window_presenter.cpp) adapted to two separate per-eye textures
// instead of one double-wide SbS input. Anaglyph matrices: 3DToElse /
// Dubois / iaian7+vectorform — same constants as VRto3D.
//
// IMPORTANT: the pattern for interlaced/checkerboard is selected from the
// OUTPUT pixel coordinate (SV_Position) AFTER the per-eye texture has been
// (bilinearly) sampled at output resolution — i.e. upscale-then-pattern, so
// the line pattern stays display-pixel-exact regardless of render resolution.

#include <cstdint>

namespace vrmod::flat3d {

// Keep in sync with VR::s_flat3d_output_mode_names (VR.hpp) AND the k*
// constants in the repack shader below.
enum class Flat3DOutputMode : int32_t {
    SBS = 0,
    TAB,
    ROW_INTERLACED,
    COL_INTERLACED,
    CHECKERBOARD,
    LEIA_SR,             // handled by the SR weaver when available; SbS fallback in the PS
    FRAMEPACKED_720P60,  // HDMI 1.4 TaB + blank gap (30 rows)
    FRAMEPACKED_1080P24, // HDMI 1.4 TaB + blank gap (45 rows)
    FRAMEPACKED_1080P60, // HDMI 1.4 TaB + blank gap (45 rows)
    DUAL_DISPLAY,        // horizontal split spanning two monitors (left=L, right=R)
    DUAL_DISPLAY_FLIP,   // as DUAL_DISPLAY but the left image is vertically flipped
    KATANGA,             // shares a SbS texture with Katanga.exe/VRScreenCap; forces windowed; left-eye preview
    // Anaglyph family grouped at the bottom of the list.
    ANAGLYPH_RC,
    ANAGLYPH_RC_DUBOIS,
    ANAGLYPH_RC_DEGHOSTED,
    ANAGLYPH_RC_COMPROMISE,
    ANAGLYPH_GM,
    ANAGLYPH_GM_DUBOIS,
    ANAGLYPH_GM_DEGHOSTED,
    ANAGLYPH_BLUE_AMBER,
    COUNT,
};

// Output color space of the game's swapchain (for the HDR-aware passes).
enum class Flat3DColorSpace : int32_t {
    SDR = 0,    // 8-bit UNORM sRGB-encoded — byte pass-through
    HDR10_PQ,   // R10G10B10A2 + ST-2084 (PQ) encoding
    SCRGB,      // R16G16B16A16_FLOAT linear (1.0 = 80 nits)
};

// Computed once per frame on the CPU (VR::build_flat3d_frame_params) and
// consumed by both compositors.
struct Flat3DFrameParams {
    int32_t mode{0};            // Flat3DOutputMode
    // 0 = respect the game's present interval, 1 = force vsync on,
    // 2/3 = force vsync off (3 additionally caps t.MaxFPS at 2x refresh).
    int32_t vsync_override{0};
    bool eye_swap{false};
    bool afr_frame{false};      // only one eye is fresh this frame
    bool afr_left_eye{false};   // which eye is fresh (when afr_frame OR warp_frame)
    bool native_stereo_layout{false}; // native-stereo-fix: sample left half
    // AFW: the engine rendered one eye (the afr_left_eye half of the double-wide)
    // and the OTHER eye is supplied as a discrete warped texture (via right_eye_src,
    // arriving in ALL_SHADER_RESOURCE state). Both eyes are refreshed this frame.
    bool warp_frame{false};

    // GUI layer (per-eye parallax; dir = +1 left eye, -1 right eye).
    bool ui_enabled{false};
    float ui_shift_px{0.0f};    // horizontal shift in EYE pixels
    float ui_scale{1.0f};       // auto-scale so the UI fills the eye after the shift
    float ui_invert_alpha{0.0f}; // UI_InvertAlpha (0=off..1=full); flips the game UI alpha in the overlay shader

    // HUD depth mode: 0 = flat at GUI depth, 1 = depth-adaptive with
    // camera-flow auto-classification (world-tracking UI tiles get scene
    // depth, static HUD stays flat), 2 = world-marker anchors (engine
    // projection-call hook; non-anchor pixels stay flat).
    int32_t hud_depth_mode{0};
    float hud_k_px{0.0f};         // pixel shift per (1/z_uu - 1/conv_uu): sep_uu*p00*eye_w/4
    float hud_inv_conv_uu{0.0f};
    float hud_nearz_uu{0.0f};     // reversed-Z: z_uu = nearz / device_depth
    float hud_marker_radius{0.06f}; // anchor influence radius (eye-width UV)

    // Predicted screen-space flow of WORLD-anchored UI content this frame
    // (from the camera yaw/pitch delta), in UV units. Drives the mode-1
    // world/static classification.
    float hud_flow_du{0.0f};
    float hud_flow_dv{0.0f};
    bool hud_flow_valid{false};
    bool hud_translating{false}; // lateral camera translation this frame (mode-1)
    bool hud_debug{false}; // visualize the world/static classification
    // Dilation radius around classified tiles (fraction of screen width) so
    // a whole icon (+ its text) moves as one piece and brief classification
    // gaps while it travels across tiles don't cut it apart.
    float hud_icon_radius{0.035f};

    // Mode-1 classification tuning (false-positive rejection). See the classify
    // shader: gates are live-tunable; anim/occupancy suppress animated HUD and
    // permanent panels; exclusion rects hard-flatten fixed screen regions.
    float hud_trans_gate{0.06f};   // #3 translation-world zero-offset change gate
    float hud_rot_gate{0.05f};     // #3 rotation-world flow-tracking gate
    float hud_occ_gate{0.85f};     // #2 permanent-panel occupancy threshold
    int32_t hud_halo_tiles{0};     // #2 permanent-panel halo dilation (0 = off)
    float hud_occ_safe_hw{0.0f};   // #2 central marker safe-zone half-extents (occupancy off inside)
    float hud_occ_safe_hh{0.0f};
    float hud_fill_radius{6.0f};   // #5 areal-fill reject block half-extent (tiles)
    float hud_fill_gate{2.0f};     // #5 coverage above which a tile is a panel/backdrop (>1 = off)
    int32_t hud_excl_count{0};     // #4 active exclusion rects
    float hud_excl[4][4]{};        // #4 cx, cy, half_w, half_h in UV

    // World-marker anchors (mode 2): xy = eye UV, z = 1/z_uu.
    static constexpr uint32_t kMaxHudAnchors = 32;
    uint32_t anchor_count{0};
    float anchors[kMaxHudAnchors][3]{};

    // UEVR's own ImGui menu, composited into the eyes at its own depth
    // (the flat backbuffer draw is skipped while this layer is consumed).
    float menu_shift_px{0.0f};
    float menu_scale{1.0f};

    // Stereo cursor: replaces the (suppressed) hardware cursor with a
    // per-eye arrow at GUI depth. cursor_uv = position in output/window UV.
    bool cursor_enabled{false};
    float cursor_uv[2]{0.5f, 0.5f};
    float cursor_size_px{32.0f}; // arrow height in eye pixels
    int32_t cursor_depth_mode{0}; // 0 = GUI depth, 1 = geometry depth under the tip (this frame)

    // Crosshair.
    uint32_t crosshair_mode{0}; // 0=off 1=game-crosshair 2=laser
    float crosshair_shift_px{0.0f};
    float crosshair_region_radius{0.04f}; // fraction of eye width (mode 1)
    float crosshair_region_center_y{0.5f}; // output-UV vertical center of the region (mode 1)
    float crosshair_size_px{8.0f};        // laser dot radius-ish (mode 2)
    uint32_t crosshair_color_argb{0xFFFFFFFF};

    // HDR.
    int32_t colorspace{0};      // Flat3DColorSpace
    float paper_white_nits{200.0f};

    // Display color correction (SDR only).
    bool correction_enabled{false};
    float lift[3]{0.0f, 0.0f, 0.0f};
    float gamma[3]{1.0f, 1.0f, 1.0f};
    float gain[3]{1.0f, 1.0f, 1.0f};
    float curve{1.0f};
    float off_low{0.0f};
    float off_high{0.0f};
    float off_both{0.0f};

    // Symmetric-projection mode: per-eye matrices identical (no shear);
    // convergence applied as a compositor image shift instead.
    float scene_shift_px{0.0f}; // LEFT-eye value (right eye = -)
    float scene_scale{1.0f};

    // Extreme compatibility mode: the eye source is the REAL BACKBUFFER
    // (one full-width eye per frame, AFR forced; no double-wide exists).
    bool extreme_backbuffer_src{false};

    // Depth sampling wishes (drives the SceneDepthZ readback).
    bool want_depth{false};
};

// Matches the cbuffer in the repack shader below. 12 dwords (3x float4).
struct RepackConstants {
    int32_t out_size[2]{};
    int32_t mode{0};
    int32_t eye_swap{0};
    int32_t colorspace{0};   // Flat3DColorSpace of the OUTPUT (backbuffer)
    float paper_white{200.0f};
    int32_t src_srgb{0};     // eye textures are 8-bit sRGB while output is HDR
    int32_t correction_enabled{0};
    // Display color correction (VRto3D lift/gamma/gain + S-curve; SDR only).
    float lift[3]{0.0f, 0.0f, 0.0f};
    float curve{1.0f};
    float gamma[3]{1.0f, 1.0f, 1.0f};
    float off_low{0.0f};
    float gain[3]{1.0f, 1.0f, 1.0f};
    float off_high{0.0f};
    float off_both{0.0f};
    // Symmetric-projection convergence (identity when the mode is off).
    float scene_shift_uv{0.0f};
    float scene_scale{1.0f};
    float pad{0.0f};
};

// Matches the cbuffer in the overlay shader below (16-byte aligned).
// One draw per (eye, layer). uv_scale/uv_offset map output-pixel UV ->
// source-texture UV (precomputed CPU-side).
struct OverlayConstants {
    float uv_scale[2]{1.0f, 1.0f};
    float uv_offset[2]{0.0f, 0.0f};
    float color[4]{1.0f, 1.0f, 1.0f, 1.0f}; // laser tint (straight alpha)
    int32_t layer{0};        // 0 = UI, 1 = UI crosshair region, 2 = laser dot, 3 = UEVR menu, 4 = cursor
    int32_t colorspace{0};   // Flat3DColorSpace of the EYE texture (== swapchain family)
    float paper_white{200.0f};
    float region_radius_uv{0.0f}; // mode 0: cutout hole; mode 1: region bound (in eye-U units)
    float region_center[2]{0.5f, 0.5f}; // in output UV (post-shift center; cursor hotspot for layer 4)
    float dot_radius_px{4.0f};   // laser dot radius / cursor arrow height (layer 4)
    float eye_width_px{0.0f};
    float eye_height_px{0.0f};
    // HUD depth mode for THIS draw (layer 0 only; 0 for everything else).
    // Low 4 bits = mode (0/1/2, 3 = mode 1 + debug tint); high bits = the
    // dilation radius in TILES (packed — the constant block is full).
    // k/bias/flat_shift are per-eye SIGNED and pre-divided by scene_scale.
    int32_t hud_mode{0};
    float hud_k_px{0.0f};
    float hud_bias_px{0.0f};
    float hud_inv_conv_uu{0.0f};
    float hud_nearz_uu{0.0f};
    float hud_depth_uscale{1.0f}; // 0.5 when SceneDepthZ is double-wide
    float hud_flat_shift_uv{0.0f};
    int32_t cursor_depth{0};      // layer 4: 1 = sample geometry depth under the tip
    float ui_invert_alpha{0.0f};  // UI_InvertAlpha: 0 = off, 1 = full alpha invert (game UI layers only)
    float _pad[2]{};              // keep the block a 16-byte multiple (D3D11 cbuffer)
};

// Anchor constant buffer (b1) for the world-marker HUD mode. Bound only for
// the overlay pass; identity/empty when the mode is off.
struct HudAnchorConstants {
    int32_t count{0};
    float radius_uv{0.06f};
    float feather_uv{0.02f};
    float pad{0.0f};
    float anchors[Flat3DFrameParams::kMaxHudAnchors][4]{}; // xy = uv, z = 1/z_uu
};

// Constants for the HUD world/static classification pass (32 dwords; laid out
// on 16-byte cbuffer rows so the C++ struct matches HLSL packing exactly).
// Persistent per-tile state lives in the RGBA8 mask: r = world/static class,
// g = self-animation score, b = occupancy (long-term presence).
struct HudClassifyConstants {
    // row 0
    float   flow_uv[2]{0.0f, 0.0f}; // predicted world-content flow (cur ~= prev shifted by this)
    float   blend_alpha{0.15f};     // temporal EMA rate when the signal is valid
    int32_t flow_valid{0};
    // row 1
    float   inv_mask_size[2]{1.0f / 64.0f, 1.0f / 36.0f}; // tile size in UV
    int32_t translating{0};         // lateral camera translation this frame (parallax)
    float   trans_d0_gate{0.06f};   // #3: zero-offset change needed for translation-world
    // row 2
    float   rot_move_gate{0.05f};   // #3: flow-tracking evidence needed for rotation-world
    float   occ_gate{0.85f};        // #2: occupancy above which a tile is a permanent HUD panel
    float   halo_tiles{0.0f};       // #2: dilation (tiles) of the permanent-panel halo (0 = off)
    int32_t excl_count{0};          // #4: number of active exclusion rects
    // row 3
    float   occ_safe_hw{0.0f};      // #2: central marker safe-zone half-width (occupancy OFF inside)
    float   occ_safe_hh{0.0f};      // #2: central marker safe-zone half-height
    float   fill_radius{6.0f};      // #5: areal-fill reject block half-extent (tiles); 0 = off
    float   fill_gate{2.0f};        // #5: coverage fraction above which a tile is a panel/backdrop (>1 = off)
    // rows 4-7: exclusion rects (cx, cy, half_w, half_h) in UV
    float   excl[4][4]{};
    // row 8
    float   ui_invert_alpha{0.0f}; // UI_InvertAlpha: undo the game's inverted UI
                                   // alpha (empty=1, drawn=0) before the silhouette
                                   // signal, else the whole screen reads as HUD.
    float   _pad[3]{};             // keep 16-byte aligned for the D3D11 constant buffer
};
static_assert(sizeof(HudClassifyConstants) == 36 * sizeof(uint32_t), "hud classify constant size");

// HUD classification mask resolution (small: one texel per UI tile).
constexpr uint32_t kHudMaskW = 64;
constexpr uint32_t kHudMaskH = 36;

// Per-tile depth pre-pass (mode 1): resolve one nearest-surface depth per world
// tile, then min-z-flood it across CONTIGUOUS world tiles so a whole icon shifts
// at ONE depth (no per-tile shear). Runs at the mask resolution; output R32F
// stores inv-z (1/z_uu), 0 == no surface / non-world tile (safe "flat").
struct HudDepthConstants {
    int32_t pass_idx{0};           // 0 = resolve (radial search), >=1 = diffuse ('pass' is an HLSL keyword)
    float mask_thr{0.2f};          // world-tile threshold on the classification mask
    float inv_mask_size[2]{1.0f / 64.0f, 1.0f / 36.0f};
    float hud_depth_uscale{1.0f};  // 0.5 when scene depth is double-wide
    float hud_nearz_uu{0.0f};      // reversed-Z: z_uu = nearz / device_depth
    float aspect_xy{0.5625f};      // eye_h/eye_w, for circular search rings
    float pad_{0.0f};
};
static_assert(sizeof(HudDepthConstants) == 8 * sizeof(uint32_t), "hud depth constant size");

// Min-z diffusion iterations: floods the nearest depth across a contiguous
// marker up to ~this many tiles of radius (8-neighbourhood per pass).
constexpr uint32_t kHudDepthDiffusePasses = 6;

// Fullscreen-triangle VS + repack PS. Compiled at runtime with D3DCompile
// (vs_5_0 / ps_5_0) — same approach as VRto3D's DX11 presenter.
static const char* const g_flat3d_repack_hlsl = R"(
cbuffer RepackParams : register(b0) {
    int2  out_size;
    int   mode;
    int   eye_swap;
    int   colorspace;   // OUTPUT space: 0 = SDR, 1 = HDR10/PQ, 2 = scRGB
    float paper_white;  // nits
    int   src_srgb;     // eye textures are 8-bit sRGB while output is HDR
    int   correction_enabled;
    float3 lift;
    float  curve;
    float3 gamma_;
    float  off_low;
    float3 gain;
    float  off_high;
    float  off_both;
    // Symmetric-projection mode: convergence applied here instead of the
    // projection shear (identical per-eye matrices fix one-eye effects).
    float  scene_shift_uv; // LEFT-eye image shift in eye-UV units (right = -)
    float  scene_scale;    // mild zoom so the shifted image still fills the eye
    float  pad;
};

// VRto3D display color correction (SCurve + LiftGammaGain), operating on
// sRGB-encoded [0,1] color. 1:1 port of dx11_renderer kAdjustPsHlsl.
float3 ApplyCorrection(float3 col) {
    float3 low  = pow(abs(col), curve)       + off_low;
    float3 high = pow(abs(col), 1.0 / curve) + off_high;
    float3 t    = saturate(col + off_both);
    col = lerp(low, high, t);

    col = col * (1.5 - 0.5 * lift) + 0.5 * lift - 0.5;
    col = saturate(col);
    col *= gain;
    col = pow(abs(col), 1.0 / gamma_);
    return saturate(col);
}

float3 srgb_to_linear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

float3 linear_to_srgb(float3 c) {
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return lerp(hi, lo, step(c, 0.0031308));
}

// ---- color-space helpers (HDR anaglyph support) ---------------------------
// Resampling modes never touch pixel values; only the anaglyph matrix modes
// convert to a normalized display-referred space first, apply the matrix,
// and convert back. SDR path is byte-exact pass-through.
static const float PQ_M1 = 0.1593017578125;
static const float PQ_M2 = 78.84375;
static const float PQ_C1 = 0.8359375;
static const float PQ_C2 = 18.8515625;
static const float PQ_C3 = 18.6875;

float3 pq_to_nits(float3 e) {
    float3 ep = pow(max(e, 0.0), 1.0 / PQ_M2);
    float3 num = max(ep - PQ_C1, 0.0);
    float3 den = PQ_C2 - PQ_C3 * ep;
    return 10000.0 * pow(num / max(den, 1e-6), 1.0 / PQ_M1);
}

float3 nits_to_pq(float3 n) {
    float3 y = pow(max(n, 0.0) / 10000.0, PQ_M1);
    return pow((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y), PQ_M2);
}

// Normalize a SOURCE (eye-texture) color to ~[0,1] display-referred range
// for the anaglyph matrices (paper-white maps to 1.0). The source is either
// in the output's space, or 8-bit sRGB when src_srgb is set (SDR engine
// target under an HDR swapchain).
float3 to_normalized(float3 c) {
    if (src_srgb != 0 || colorspace == 0) {
        return c; // sRGB-encoded [0,1]: anaglyph matrices are defined for this
    }
    if (colorspace == 1) { // PQ
        return saturate(linear_to_srgb(pq_to_nits(c) / paper_white));
    }
    // scRGB: 1.0 == 80 nits, linear
    return saturate(linear_to_srgb(c * (80.0 / paper_white)));
}

// Encode a normalized (sRGB-encoded display-referred) color to the OUTPUT
// swapchain space.
float3 from_normalized(float3 c) {
    if (colorspace == 1) {
        return nits_to_pq(srgb_to_linear(c) * paper_white);
    }
    if (colorspace == 2) {
        return srgb_to_linear(c) * (paper_white / 80.0);
    }
    return c;
}

Texture2D eye_left  : register(t0);
Texture2D eye_right : register(t1);
SamplerState samp   : register(s0);

// Keep in sync with Flat3DOutputMode.
static const int kSbS                = 0;
static const int kTaB                = 1;
static const int kRowInterlaced      = 2;
static const int kColInterlaced      = 3;
static const int kCheckerboard       = 4;
static const int kLeiaSR             = 5;
static const int kFramePacked720p60  = 6;
static const int kFramePacked1080p24 = 7;
static const int kFramePacked1080p60 = 8;
static const int kDualDisplay        = 9;
static const int kDualDisplayFlip    = 10;
static const int kKatanga            = 11;
static const int kAnaRC              = 12;
static const int kAnaRCDubois        = 13;
static const int kAnaRCDeghosted     = 14;
static const int kAnaRCCompromise    = 15;
static const int kAnaGM              = 16;
static const int kAnaGMDubois        = 17;
static const int kAnaGMDeghosted     = 18;
static const int kAnaBlueAmber       = 19;

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

// half_idx: 0 = left half of the OUTPUT layout, 1 = right/bottom half.
// Samples the corresponding per-eye texture at (u, v) within the eye image.
float4 SampleEye(int half_idx, float u, float v) {
    int eye = (eye_swap != 0) ? (1 - half_idx) : half_idx;

    // Symmetric-projection convergence: shift each eye's image horizontally
    // (equivalent to the [2][0] shear, which adds a constant NDC offset) and
    // zoom slightly so the shifted image still covers the eye.
    float dir = (eye == 0) ? 1.0 : -1.0;
    float2 uv = float2((u - 0.5 - dir * scene_shift_uv) / scene_scale + 0.5,
                       (v - 0.5) / scene_scale + 0.5);

    return (eye == 0) ? eye_left.Sample(samp, uv) : eye_right.Sample(samp, uv);
}

float4 Repack(float2 uv, float2 pixel) {
    int m = mode;

    // SbS (+ LeiaSR fallback until the weaver takes over, + DualDisplay:
    // output window spans two monitors, left half = left eye).
    if (m == kSbS || m == kLeiaSR || m == kDualDisplay) {
        int   half_idx = (uv.x < 0.5) ? 0 : 1;
        float u_half   = (uv.x < 0.5) ? (uv.x * 2.0) : ((uv.x - 0.5) * 2.0);
        return SampleEye(half_idx, u_half, uv.y);
    }

    // Katanga: the stereo pair goes out via the shared texture; the local
    // window is a normal desktop preview showing the LEFT eye only (mono,
    // eye_swap-aware).
    if (m == kKatanga) {
        return SampleEye(0, uv.x, uv.y);
    }

    // DualDisplayFlip: SbS split, left half vertically flipped.
    if (m == kDualDisplayFlip) {
        int   half_idx = (uv.x < 0.5) ? 0 : 1;
        float u_half   = (uv.x < 0.5) ? (uv.x * 2.0) : ((uv.x - 0.5) * 2.0);
        float v_src    = (half_idx == 0) ? (1.0 - uv.y) : uv.y;
        return SampleEye(half_idx, u_half, v_src);
    }

    // TaB: top rows = left eye, bottom rows = right eye, no gap.
    // Frame-packed: HDMI 1.4 TaB with a blank active-space gap between the two
    // half-frames (30 rows @720p, 45 rows @1080p). Gap fraction derives from
    // the output height so the eye halves line up with the display's stereo
    // frame structure.
    if (m == kTaB || m == kFramePacked720p60 || m == kFramePacked1080p24 || m == kFramePacked1080p60) {
        float gap_px = 0.0;
        if (m == kFramePacked720p60)  gap_px = 30.0;
        else if (m != kTaB)           gap_px = 45.0; // 1080p24 / 1080p60
        float gap = (gap_px > 0.0) ? (gap_px / (float)out_size.y) : 0.0;
        float half_h = (1.0 - gap) * 0.5;
        if (uv.y < half_h) {
            return SampleEye(0, uv.x, uv.y / half_h);
        } else if (uv.y >= half_h + gap) {
            return SampleEye(1, uv.x, (uv.y - half_h - gap) / half_h);
        }
        return float4(0.0, 0.0, 0.0, 1.0); // frame-pack blanking gap
    }

    // Interlaced/checkerboard: pattern in OUTPUT pixel space (after the
    // full-res sample) so lines are display-pixel-exact at any render res.
    if (m == kRowInterlaced) {
        int row = (int)pixel.y;
        return SampleEye(row & 1, uv.x, uv.y);
    }

    if (m == kColInterlaced) {
        int col = (int)pixel.x;
        return SampleEye(col & 1, uv.x, uv.y);
    }

    if (m == kCheckerboard) {
        int row = (int)pixel.y;
        int col = (int)pixel.x;
        return SampleEye((row + col) & 1, uv.x, uv.y);
    }

    // ----- Anaglyph variants: full-frame sample of each eye ---------------
    // Work in normalized display-referred space (HDR-aware); re-encode at
    // the end. In SDR both conversions are identity.
    float4 cA = SampleEye(0, uv.x, uv.y); // left
    float4 cB = SampleEye(1, uv.x, uv.y); // right
    cA.rgb = to_normalized(cA.rgb);
    cB.rgb = to_normalized(cB.rgb);

    if (m == kAnaRC) {
        return float4(from_normalized(float3(cA.r, cB.g, cB.b)), 1.0);
    }

    if (m == kAnaRCDubois) {
        float r = saturate( 0.437 * cA.r + 0.449 * cA.g + 0.164 * cA.b
                           - 0.011 * cB.r - 0.032 * cB.g - 0.007 * cB.b);
        float g = saturate(-0.062 * cA.r - 0.062 * cA.g - 0.024 * cA.b
                           + 0.377 * cB.r + 0.761 * cB.g + 0.009 * cB.b);
        float b = saturate(-0.048 * cA.r - 0.050 * cA.g - 0.017 * cA.b
                           - 0.026 * cB.r - 0.093 * cB.g + 1.234 * cB.b);
        return float4(from_normalized(float3(r, g, b)), 1.0);
    }

    if (m == kAnaRCDeghosted) {
        float Contrast = 1.0;
        float DeGhost  = 0.06 * 0.1;
        float contrast = (Contrast * 0.5) + 0.5;
        float LOne = contrast * 0.45;
        float ROne = contrast;

        float4 image = float4(0.0, 0.0, 0.0, 1.0);
        float4 accum;
        accum = saturate(cA * float4(LOne, (1.0 - LOne) * 0.5, (1.0 - LOne) * 0.5, 1.0));
        image.r = pow(accum.r + accum.g + accum.b, 1.00);
        accum = saturate(cB * float4(1.0 - ROne, ROne, 0.0, 1.0));
        image.g = pow(accum.r + accum.g + accum.b, 1.15);
        accum = saturate(cB * float4(1.0 - ROne, 0.0, ROne, 1.0));
        image.b = pow(accum.r + accum.g + accum.b, 1.15);

        accum = image;
        image.r = accum.r + (accum.r * DeGhost)           + (accum.g * (DeGhost * -0.5))  + (accum.b * (DeGhost * -0.5));
        image.g = accum.g + (accum.r * (DeGhost * -0.25)) + (accum.g * (DeGhost * 0.5))   + (accum.b * (DeGhost * -0.25));
        image.b = accum.b + (accum.r * (DeGhost * -0.25)) + (accum.g * (DeGhost * -0.25)) + (accum.b * (DeGhost * 0.5));
        image.a = 1.0;
        return float4(from_normalized(image.rgb), 1.0);
    }

    if (m == kAnaRCCompromise) {
        float r = dot(float3( 0.439,  0.447,  0.148), cA.rgb);
        float g = dot(float3( 0.095,  0.934, -0.005), cB.rgb);
        float b = dot(float3(-0.018, -0.028,  1.057), cB.rgb);
        return float4(from_normalized(saturate(float3(r, g, b))), 1.0);
    }

    if (m == kAnaGM) {
        return float4(from_normalized(float3(cB.r, cA.g, cB.b)), 1.0);
    }

    if (m == kAnaGMDubois) {
        float r = saturate(-0.062 * cA.r - 0.158 * cA.g - 0.039 * cA.b
                           + 0.529 * cB.r + 0.705 * cB.g + 0.024 * cB.b);
        float g = saturate( 0.284 * cA.r + 0.668 * cA.g + 0.143 * cA.b
                           - 0.016 * cB.r - 0.015 * cB.g + 0.065 * cB.b);
        float b = saturate(-0.015 * cA.r - 0.027 * cA.g + 0.021 * cA.b
                           + 0.009 * cB.r + 0.075 * cB.g + 0.937 * cB.b);
        return float4(from_normalized(float3(r, g, b)), 1.0);
    }

    if (m == kAnaGMDeghosted) {
        float Contrast = 1.0;
        float DeGhost  = 0.06 * 0.275;
        float contrast = (Contrast * 0.5) + 0.5;
        float LOne = contrast * 0.45;
        float ROne = contrast * 0.8;

        float4 image = float4(0.0, 0.0, 0.0, 1.0);
        float4 accum;
        accum = saturate(cB * float4(ROne, 1.0 - ROne, 0.0, 1.0));
        image.r = pow(accum.r + accum.g + accum.b, 1.15);
        accum = saturate(cA * float4((1.0 - LOne) * 0.5, LOne, (1.0 - LOne) * 0.5, 1.0));
        image.g = pow(accum.r + accum.g + accum.b, 1.05);
        accum = saturate(cB * float4(0.0, 1.0 - ROne, ROne, 1.0));
        image.b = pow(accum.r + accum.g + accum.b, 1.15);

        accum = image;
        image.r = accum.r + (accum.r * (DeGhost * 0.5))   + (accum.g * (DeGhost * -0.25)) + (accum.b * (DeGhost * -0.25));
        image.g = accum.g + (accum.r * (DeGhost * -0.5))  + (accum.g * (DeGhost * 0.25))  + (accum.b * (DeGhost * -0.5));
        image.b = accum.b + (accum.r * (DeGhost * -0.25)) + (accum.g * (DeGhost * -0.25)) + (accum.b * (DeGhost * 0.5));
        image.a = 1.0;
        return float4(from_normalized(image.rgb), 1.0);
    }

    if (m == kAnaBlueAmber) {
        float Contrast = 1.0;
        float DeGhost  = 0.06 * 0.275;
        float contrast = (Contrast * 0.5) + 0.5;
        float LOne = contrast * 0.45;
        float ROne = contrast;

        float4 image = float4(0.0, 0.0, 0.0, 1.0);
        float4 accum;
        accum = saturate(cA * float4(ROne, 0.0, 1.0 - ROne, 1.0));
        image.r = pow(accum.r + accum.g + accum.b, 1.05);
        accum = saturate(cA * float4(0.0, ROne, 1.0 - ROne, 1.0));
        image.g = pow(accum.r + accum.g + accum.b, 1.10);
        accum = saturate(cB * float4((1.0 - LOne) * 0.5, (1.0 - LOne) * 0.5, LOne, 1.0));
        image.b = pow(accum.r + accum.g + accum.b, 1.0);
        image.b = lerp(pow(image.b, (DeGhost * 0.15) + 1.0),
                       1.0 - pow(abs(1.0 - image.b), (DeGhost * 0.15) + 1.0),
                       image.b);

        accum = image;
        image.r = accum.r + (accum.r * (DeGhost * 1.5))   + (accum.g * (DeGhost * -0.75)) + (accum.b * (DeGhost * -0.75));
        image.g = accum.g + (accum.r * (DeGhost * -0.75)) + (accum.g * (DeGhost * 1.5))   + (accum.b * (DeGhost * -0.75));
        image.b = accum.b + (accum.r * (DeGhost * -1.5))  + (accum.g * (DeGhost * -1.5))  + (accum.b * (DeGhost * 3.0));
        image = saturate(image);
        return float4(from_normalized(image.rgb), 1.0);
    }

    // Fallback (debug)
    return float4(uv, 0.0, 1.0);
}

float4 ps_main(VSOut input) : SV_Target {
    float4 c = Repack(input.uv, input.pos.xy);

    // Display color correction (SDR only — it's defined in sRGB byte space;
    // for SDR every mode returns sRGB-encoded [0,1] here).
    if (correction_enabled != 0 && colorspace == 0) {
        c.rgb = ApplyCorrection(c.rgb);
    }

    // Pass-through / resampling modes with an sRGB source and an HDR output
    // still need the space conversion (anaglyph modes already returned
    // output-space; the frame-pack gap returns black which encodes to black).
    bool passthrough = (mode < kAnaRC); // every non-anaglyph mode (anaglyphs are grouped at the end)
    if (passthrough && src_srgb != 0 && colorspace != 0) {
        c.rgb = from_normalized(c.rgb);
    }

    return c;
}
)";

// Overlay pass: composites the game UI layer (at GUI depth), the extracted
// game-crosshair region (at aim depth), or a procedural laser dot into ONE
// eye texture. Drawn with premultiplied-alpha blending, once per (eye,
// layer). uv_scale/uv_offset map the eye's output UV to the source UV
// (shift + auto-scale precomputed CPU-side).
static const char* const g_flat3d_overlay_hlsl = R"(
cbuffer OverlayParams : register(b0) {
    float2 uv_scale;
    float2 uv_offset;
    float4 tint;             // laser color, straight alpha
    int    layer;            // 0 = UI, 1 = UI crosshair region, 2 = laser dot, 3 = UEVR menu, 4 = cursor
    int    colorspace;       // EYE texture space: 0 = sRGB bytes, 1 = PQ, 2 = scRGB
    float  paper_white;
    float  region_radius_uv; // layer 0: cutout hole; layer 1: region bound (eye-U units)
    float2 region_center;    // output UV of the (shifted) crosshair center / cursor hotspot
    float  dot_radius_px;    // laser dot radius / cursor arrow height (layer 4)
    float  eye_width_px;
    float  eye_height_px;
    // HUD depth mode (layer 0): low 4 bits = mode (0 flat, 1 depth-adaptive,
    // 2 anchors, 3 = mode 1 + debug tint); high bits = dilation radius in
    // tiles. k/bias/flat_shift are per-eye SIGNED, pre-divided by scene_scale.
    int    hud_mode;
    float  hud_k_px;          // pixel shift per (1/z_uu - 1/conv_uu)
    float  hud_bias_px;       // symmetric-mode scene shift bias
    float  hud_inv_conv_uu;
    float  hud_nearz_uu;      // reversed-Z: z_uu = nearz / device_depth
    float  hud_depth_uscale;  // 0.5 when SceneDepthZ is double-wide
    float  hud_flat_shift_uv; // fallback drawn shift (non-anchor pixels)
    int    cursor_depth;      // layer 4: 1 = geometry depth under the tip
    float  ui_invert_alpha;   // UI_InvertAlpha: 0 = off .. 1 = full alpha invert
};

cbuffer HudAnchors : register(b1) {
    int    anchor_count;
    float  anchor_radius_uv;
    float  anchor_feather_uv;
    float  anchor_pad;
    float4 anchors[32]; // xy = eye UV, z = 1/z_uu
};

Texture2D ui_tex       : register(t0);
Texture2D menu_tex     : register(t1); // UEVR's own ImGui render target
Texture2D scene_depth  : register(t2); // depth-adaptive HUD (mode 1) - now unused here; resolved in the pre-pass
Texture2D hud_tiledepth : register(t4); // mode 1: per-tile unified inv-z (0 = flat)
Texture2D hud_mask    : register(t3); // world/static classification (mode 1)
SamplerState samp     : register(s0);

int HudMode() { return hud_mode & 15; }
int HudTiles() { return max(hud_mode >> 4, 1); }

// Per-pixel HUD shift (eye-U units) for the depth modes. For depth-adaptive
// the depth is sampled in a short strip BELOW the pixel — world markers
// float above the thing they track, so the object is usually underneath.
float ComputeHudShiftUV(float2 uv) {
    if (HudMode() == 1 || HudMode() == 3) {
        // Everything is quantized PER MASK TILE so a classified element
        // shifts rigidly — per-pixel mask gradients / depth variation shear
        // and squish the glyph. UI-TEXTURE space (not screen space) so both
        // eyes read the same classification for the same UI content. Keep
        // the tile grid in sync with kHudMaskW/H.
        float2 ui_uv = uv * uv_scale + uv_offset;
        float2 tile_uv = (floor(ui_uv * float2(64.0, 36.0)) + 0.5) / float2(64.0, 36.0);

        // Dilate by the icon-region radius: the whole glyph (+ its text)
        // must move as ONE piece, and the halo also bridges the brief gaps
        // while a moving marker's new tiles accumulate evidence — without
        // it the icon tears/flickers at tile boundaries.
        int tiles_x = HudTiles();
        int tiles_y = max(1, (tiles_x * 9 + 8) / 16); // 36/64 aspect

        float m = 0.0;
        float invz = 0.0; // unified nearest inv-z across the dilated marker

        [loop]
        for (int ty = -tiles_y; ty <= tiles_y; ++ty) {
            [loop]
            for (int tx = -tiles_x; tx <= tiles_x; ++tx) {
                float2 nuv = saturate(tile_uv + float2((float)tx / 64.0, (float)ty / 36.0));
                m = max(m, hud_mask.SampleLevel(samp, nuv, 0).r);
                // Tile-depth is already min-z-flooded across each contiguous
                // marker, so the dilation max just picks up the marker's one
                // shared depth and carries it out to the fringe tiles.
                invz = max(invz, hud_tiledepth.SampleLevel(samp, nuv, 0).r);
            }
        }

        // Full commitment on the first latch hit (the latch writes ~0.45):
        // partial ramp values half-shift the icon, which reads as flicker.
        float world_ness = smoothstep(0.2, 0.45, m);

        if (world_ness < 0.02) {
            return hud_flat_shift_uv;
        }

        // ONE depth for the whole contiguous icon (resolved + min-z-flooded in
        // the tile-depth pre-pass) so it shifts rigidly instead of shearing
        // tile-by-tile. invz ~0 => no non-sky surface under the marker => flat.
        if (invz <= 1e-7) {
            return hud_flat_shift_uv;
        }

        float s_px = hud_k_px * (invz - hud_inv_conv_uu) - hud_bias_px;
        float depth_shift = clamp(s_px / max(eye_width_px, 1.0), -0.05, 0.05);
        return lerp(hud_flat_shift_uv, depth_shift, world_ness);
    }

    // Mode 2: nearest world-marker anchor, blended toward the flat shift at
    // the influence radius edge (soft island around each marker).
    float aspect = eye_height_px / max(eye_width_px, 1.0);
    float best_d = 1e9;
    float best_inv_z = 0.0;

    for (int i = 0; i < anchor_count; ++i) {
        float d = length((uv - anchors[i].xy) * float2(1.0, aspect));
        if (d < best_d) {
            best_d = d;
            best_inv_z = anchors[i].z;
        }
    }

    if (best_inv_z > 0.0 && best_d < anchor_radius_uv + anchor_feather_uv) {
        float s_px = hud_k_px * (best_inv_z - hud_inv_conv_uu) - hud_bias_px;
        float a_shift = clamp(s_px / max(eye_width_px, 1.0), -0.05, 0.05);
        float t = saturate((anchor_radius_uv + anchor_feather_uv - best_d) / max(anchor_feather_uv, 1e-4));
        return lerp(hud_flat_shift_uv, a_shift, t);
    }

    return hud_flat_shift_uv;
}

static const float PQ_M1 = 0.1593017578125;
static const float PQ_M2 = 78.84375;
static const float PQ_C1 = 0.8359375;
static const float PQ_C2 = 18.8515625;
static const float PQ_C3 = 18.6875;

float3 nits_to_pq(float3 n) {
    float3 y = pow(max(n, 0.0) / 10000.0, PQ_M1);
    return pow((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y), PQ_M2);
}

float3 srgb_to_linear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

// Convert an sRGB-encoded (UI) color to the eye texture's space.
float3 ui_to_eye_space(float3 c) {
    if (colorspace == 1) {
        return nits_to_pq(srgb_to_linear(c) * paper_white);
    }
    if (colorspace == 2) {
        return srgb_to_linear(c) * (paper_white / 80.0);
    }
    return c;
}

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

// Point-in-triangle via edge cross products (consistent winding either way).
float cross2(float2 a, float2 b) { return a.x * b.y - a.y * b.x; }

bool in_tri(float2 p, float2 a, float2 b, float2 c) {
    float c1 = cross2(b - a, p - a);
    float c2 = cross2(c - b, p - b);
    float c3 = cross2(a - c, p - c);
    return (c1 <= 0.0 && c2 <= 0.0 && c3 <= 0.0) || (c1 >= 0.0 && c2 >= 0.0 && c3 >= 0.0);
}

// Navigation arrow pointing UP-LEFT like a normal cursor: tip/hotspot at the
// origin, body extends down-right (x right, y down), split by a DIAGONAL centre
// crease (the arrow axis). The two facets shade light/dark and the crease is
// given a per-eye disparity so the cursor reads as a folded 3D triangle whose
// ridge pops toward the viewer. kAxis = tip->back, kPerp = across the crease.
static const float2 kAxis     = float2(0.70711, 0.70711);
static const float2 kPerp     = float2(-0.70711, 0.70711);
static const float2 kNavTip   = float2(0.0, 0.0);
static const float2 kNavNotch = float2(0.495, 0.495);
static const float2 kNavWingL = float2(0.375, 0.969);
static const float2 kNavWingR = float2(0.969, 0.375);
bool nav_any(float2 p) {
    return in_tri(p, kNavTip, kNavWingL, kNavNotch) || in_tri(p, kNavTip, kNavNotch, kNavWingR);
}

float4 ps_main(VSOut input) : SV_Target {
    // Elliptical distance to the crosshair region center, in eye-U units
    // (y corrected by the eye aspect via uv_scale-free heuristic: region is
    // defined as a circle in eye-width fractions on both axes).
    float2 d = input.uv - region_center;
    float region_dist = length(d);

    if (layer == 4) {
        // Geometry-depth cursor: sample the scene depth under the tip and shift
        // the WHOLE arrow rigidly to that depth (per-eye SIGNED via hud_k_px),
        // reusing the HUD depth formula. region_center is the mono tip position;
        // sky / no surface -> screen plane. Clamped so a very near hit still fuses.
        if (cursor_depth != 0) {
            float dev = scene_depth.SampleLevel(samp, float2(region_center.x * hud_depth_uscale, region_center.y), 0).r;
            float invz = (dev > 1e-7) ? (dev / max(hud_nearz_uu, 1e-6)) : hud_inv_conv_uu;
            float s_px = hud_k_px * (invz - hud_inv_conv_uu) - hud_bias_px;
            d.x -= clamp(s_px / max(eye_width_px, 1.0), -0.06, 0.06);
        }

        // Stereo cursor: folded 3D navigation arrow. dot_radius_px = arrow height.
        float inv_size = 1.0 / max(dot_radius_px, 1.0);
        float pxu = inv_size; // one output pixel in arrow units
        float2 p = float2(d.x * eye_width_px, d.y * max(eye_height_px, 1.0)) * inv_size;

        // Solid silhouette + outline from the UNWARPED shape — identical in both
        // eyes, so the arrow fuses as one flat cutout at GUI depth (stable hotspot,
        // no squeezing). 4x rotated-grid supersample for smooth edges.
        const float2 J[4] = { float2(0.125, 0.375), float2(0.375, -0.125),
                              float2(-0.125, -0.375), float2(-0.375, 0.125) };
        float fill = 0.0, edge = 0.0;
        [unroll]
        for (int i = 0; i < 4; ++i) {
            float2 s = p + J[i] * (pxu * 2.0);
            if (nav_any(s)) fill += 0.25;
            if (nav_any(s * (1.0 / 1.32))) edge += 0.25; // enlarged silhouette = fill+outline
        }
        float outline = saturate(edge - fill);

        // Two-facet shading + a crease highlight: a flat folded LOOK, drawn
        // identically in both eyes (no stereo disparity).
        float side  = dot(p, kPerp);   // signed distance across the crease
        float axial = dot(p, kAxis);   // 0 at tip -> ~0.95 at wings
        float crease = saturate(1.0 - abs(side) / 0.42)
                     * smoothstep(0.0, 0.18, axial) * (1.0 - smoothstep(0.62, 0.95, axial));

        float t  = saturate(abs(side) * 2.4);  // 0 at crease -> 1 at wing
        float vv = saturate(axial / 0.95);     // 0 at tip -> 1 at base
        float shade = ((side > 0.0) ? 0.98 : 0.80) - 0.10 * t - 0.12 * vv;
        shade = saturate(shade + smoothstep(0.06, 0.0, abs(side)) * crease * 0.12); // crease highlight

        float3 fill_col = ui_to_eye_space(float3(shade, shade, shade));
        float3 dark = ui_to_eye_space(float3(0.03, 0.03, 0.03));

        // Premultiplied over: outline (behind) then the facet fill.
        float3 col = dark * outline;
        float a = outline;
        col = fill_col * fill + col * (1.0 - fill);
        a = fill + a * (1.0 - fill);
        return float4(col, a);
    }

    if (layer == 2) {
        // Procedural anti-aliased laser dot with a thin dark outline for
        // visibility on bright scenes.
        float radius_uv = dot_radius_px / max(eye_width_px, 1.0);
        float aa = 1.0 / max(eye_width_px, 1.0);
        float alpha = 1.0 - smoothstep(radius_uv - aa, radius_uv + aa, region_dist);
        float outline = (1.0 - smoothstep(radius_uv * 1.4 - aa, radius_uv * 1.4 + aa, region_dist)) - alpha;

        float3 color = ui_to_eye_space(tint.rgb) * alpha; // premultiplied
        float a = saturate(alpha * tint.a + outline * 0.6 * tint.a);
        return float4(color, a);
    }

    // Layers 0/1 sample the redirected game UI texture; layer 3 samples the
    // UEVR menu texture (both premultiplied-alpha sRGB).
    float2 in_uv = input.uv;

    // HUD depth modes: the FLAT transform (fit-scale included) is applied by
    // uv_scale/uv_offset as usual; the shader adds only the per-pixel DELTA
    // from the flat shift. Static/fallback pixels have delta 0 and follow
    // the flat path exactly (no edge overshoot).
    if (layer == 0 && HudMode() != 0) {
        in_uv.x -= ComputeHudShiftUV(input.uv) - hud_flat_shift_uv;
    }

    float2 src_uv = in_uv * uv_scale + uv_offset;

    if (any(src_uv < 0.0) || any(src_uv > 1.0)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    float4 c = (layer == 3) ? menu_tex.Sample(samp, src_uv) : ui_tex.Sample(samp, src_uv);

    // UI_InvertAlpha: flip the game UI's alpha toward its complement
    // (lerp(a, 1-a, amount)), matching the VR-path alpha-invert. Some titles
    // (e.g. FF7 Rebirth) store the HUD/UI alpha inverted, so it reads bright and
    // transparent until corrected. Not applied to the UEVR ImGui menu (layer 3).
    if ((layer == 0 || layer == 1) && ui_invert_alpha > 0.0) {
        // UI_InvertAlpha, matching the VR path exactly: lerp(a, 1-a, amount).
        // This is correct ONLY because the UI target's EMPTY regions are
        // pre-cleared to alpha = ui_invert_alpha (see the clear in the Flat3D
        // submit): the game draws real content (HUD, loading-screen fills) with
        // a=0, empty stays at the clear value, and 1-a then makes drawn content
        // opaque and empty transparent. Without that clear this just blacks the
        // whole frame (empty a=0 -> opaque).
        c.a = lerp(c.a, 1.0 - c.a, ui_invert_alpha);
    }

    // Classification debug view (hud_mode 3): red = world-anchored, green =
    // static, tinted over the UI plus a faint full-screen wash so empty
    // tiles are visible too.
    if (layer == 0 && HudMode() == 3) {
        float m = hud_mask.SampleLevel(samp, src_uv, 0).r;
        float3 cls_color = float3(m, 1.0 - m, 0.0);
        c.rgb = c.rgb * 0.35 + cls_color * (0.5 * max(c.a, 0.2));
        c.a = max(c.a, 0.2);
    }

    if (layer == 0 && region_radius_uv > 0.0) {
        // Cut the crosshair region out of the flat UI pass (drawn separately
        // at aim depth as layer 1). Soft edge to avoid a hard seam.
        float aa = 2.0 / max(eye_width_px, 1.0);
        c *= smoothstep(region_radius_uv - aa, region_radius_uv + aa, region_dist);
    } else if (layer == 1) {
        float aa = 2.0 / max(eye_width_px, 1.0);
        c *= 1.0 - smoothstep(region_radius_uv - aa, region_radius_uv + aa, region_dist);
    }

    // The UI target is premultiplied-alpha sRGB (drawn onto the backbuffer
    // with SpriteBatch's premultiplied blend in HMD mode). Convert color to
    // the eye texture's space; alpha stays.
    c.rgb = ui_to_eye_space(c.rgb);
    return c;
}
)";

// HUD world/static classification pass: one texel per UI tile. Compares the
// current UI content against last frame's at (a) zero offset and (b) the
// camera-predicted world flow (both signs — the sign convention is
// self-correcting). Content that tracks the camera flow is world-anchored
// (mask -> 1, gets scene depth); content that matches at zero offset is
// screen-anchored (mask -> 0, stays flat). Temporal EMA smooths flicker;
// with no rotation this frame there is no signal and the mask holds.
static const char* const g_flat3d_hudclass_hlsl = R"(
cbuffer ClassifyParams : register(b0) {
    float2 flow_uv;
    float  blend_alpha;
    int    flow_valid;
    float2 inv_mask_size;
    int    translating;
    float  trans_d0_gate;
    float  rot_move_gate;
    float  occ_gate;
    float  halo_tiles;
    int    excl_count;
    float  occ_safe_hw;    // central marker safe-zone (occupancy off inside)
    float  occ_safe_hh;
    float  fill_radius;    // #5: areal-fill reject block half-extent (tiles)
    float  fill_gate;      // #5: coverage above which the tile is a big fill (panel/backdrop)
    float4 excl[4];        // cx, cy, half_w, half_h in UV
    float  ui_invert_alpha; // UI_InvertAlpha: undo inverted game-UI alpha in sil()
};

Texture2D cur_ui   : register(t0);
Texture2D prev_ui  : register(t1);
Texture2D old_mask : register(t2); // r = class, b = occupancy (g/a unused)
SamplerState samp  : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

// Silhouette signal for classification. Premultiplied UI: alpha is the coverage
// mask — the thing that TRANSLATES across the screen for a world-anchored
// element, while an in-place animation (a spinning minimap, a side video) keeps
// its alpha silhouette parked and only churns its interior content. So we key
// on alpha, with a small content term so a uniform-alpha glyph that merely
// recolors doesn't read as empty. Alpha is a CONTINUOUS weight here, never
// thresholded to "opaque", so feathered / semi-transparent UI still works.
float sil(float4 c) {
    // UI_InvertAlpha: some titles (e.g. FF7 Rebirth) store the HUD alpha inverted
    // (empty screen = 1, drawn UI = 0). Undo it to match the composited opacity —
    // otherwise the empty screen reads as the silhouette and the HUD reads as void.
    float a = lerp(c.a, 1.0 - c.a, ui_invert_alpha);
    return a + dot(c.rgb, float3(0.299, 0.587, 0.114)) * 0.15;
}

float4 ps_main(VSOut input) : SV_Target {
    float4 om   = old_mask.SampleLevel(samp, input.uv, 0);
    float  old  = om.r;
    float  occ  = om.b; // long-term occupancy (presence)

    // #4 Exclusion rects: user-marked screen regions that are ALWAYS flat HUD
    // (minimap, gauges, action bar). Force static; leave occupancy inert so a
    // later toggle-off recovers cleanly.
    [loop]
    for (int e = 0; e < excl_count; ++e) {
        float2 d = abs(input.uv - excl[e].xy);
        if (d.x <= excl[e].z && d.y <= excl[e].w) {
            return float4(0.0, 0.0, occ, 0.0);
        }
    }

    // Peak / best-tap metrics (NOT sums): a tiny marker (a waypoint diamond is
    // ~1/3 of a tile) lands on only a few taps, so its motion evidence must be
    // the strongest single tap, never averaged into the surrounding void.
    float peak      = 0.0;  // strongest current coverage (presence)
    float peak_d0   = 0.0;  // strongest zero-offset change (any tap)
    float peak_move = -1.0; // best (d0 - dw): tap tracked the rotation flow
    float peak_stat = -1.0; // best (dw - d0): tap best at zero offset (screen)

    [unroll]
    for (int y = -2; y <= 2; ++y) {
        [unroll]
        for (int x = -2; x <= 2; ++x) {
            float2 p = input.uv + float2((float)x, (float)y) * inv_mask_size * 0.2;
            // Skip taps that leave the UI: clamp-sampling folds real content
            // back in and misclassifies markers parked at the screen edge.
            if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) {
                continue;
            }

            float c = sil(cur_ui.SampleLevel(samp, p, 0));
            peak = max(peak, c);

            float d0 = abs(c - sil(prev_ui.SampleLevel(samp, p, 0)));
            peak_d0 = max(peak_d0, d0);

            if (flow_valid != 0) {
                float dp = abs(c - sil(prev_ui.SampleLevel(samp, p - flow_uv, 0)));
                float dm = abs(c - sil(prev_ui.SampleLevel(samp, p + flow_uv, 0)));
                float dw = min(dp, dm);
                peak_move = max(peak_move, d0 - dw); // >0: this tap tracks the flow
                peak_stat = max(peak_stat, dw - d0); // >0: this tap best at zero
            }
        }
    }

    bool present = peak > 0.06;

    // #2 Occupancy: slow rise, fast decay. Only a tile covered EVERY frame (a
    // permanent panel) saturates; a marker that ever moves or briefly clears
    // resets and never reaches the gate. Separates "always-there fixture" from
    // "lingering marker" far better than a symmetric EMA.
    occ = present ? min(occ + 0.008, 1.0) : max(occ - 0.04, 0.0);

    // #5 Areal-fill reject: a real world-marker is a small isolated island; a
    // menu backdrop / scene-blur scrim is a LARGE contiguous fill. Sample the
    // current UI's coverage over a +/- fill_radius tile block: if most of it is
    // covered, this tile is interior to a panel/backdrop and can never be a
    // marker -> ease flat. Purely spatial, so it catches a menu the frame it
    // opens (no occupancy build-up) and works dead-center, independent of the
    // occupancy safe-zone that otherwise protects a central backdrop. Runs
    // before the hold/motion tests so it pulls down an already-latched tile AND
    // blocks a fresh latch outright.
    if (present && fill_gate < 1.0 && fill_radius > 0.0) {
        float cov = 0.0;
        float n   = 0.0;
        [unroll]
        for (int fy = -2; fy <= 2; ++fy) {
            [unroll]
            for (int fx = -2; fx <= 2; ++fx) {
                float2 fp = input.uv + float2((float)fx, (float)fy)
                          * (fill_radius * 0.5) * inv_mask_size;
                if (fp.x < 0.0 || fp.x > 1.0 || fp.y < 0.0 || fp.y > 1.0) {
                    continue;
                }
                n += 1.0;
                if (sil(cur_ui.SampleLevel(samp, fp, 0)) > 0.06) {
                    cov += 1.0;
                }
            }
        }
        if (n > 0.0 && cov / n >= fill_gate) {
            return float4(lerp(old, 0.0, blend_alpha), 0.0, occ, 0.0);
        }
    }

    // Presence gate on the PEAK tap: if nothing is there anymore, decay toward
    // flat — EVEN WITH A STILL CAMERA. A world element that slides across the
    // screen latches these tiles; when it moves on or vanishes the alpha is gone,
    // and the tile must not keep its stale depth or it leaves a "red trail". This
    // runs BEFORE the still-camera hold so an emptied tile always releases (the
    // hold is only meant to preserve a marker that is STILL present). A
    // tiny/semi-transparent marker covers only a few taps, so gate on the peak.
    if (!present) {
        return float4(lerp(old, 0.0, blend_alpha), 0.0, occ, 0.0); // empty -> flat
    }

    // Still present but no usable camera motion this frame -> no fresh world/
    // static evidence; hold the classification so a stationary marker keeps its
    // depth while the camera is still (occupancy still updated above).
    if (flow_valid == 0 && translating == 0) {
        return float4(old, 0.0, occ, 0.0);
    }

    // #2 Permanent-panel halo: dilate saturated occupancy by halo_tiles so a
    // marginal-coverage rim (minimap edge, icons protruding past the disc) is
    // absorbed into the panel and forced flat too.
    float halo = occ;
    if (halo_tiles > 0.0) {
        int R = (int)halo_tiles;
        [loop]
        for (int hy = -R; hy <= R; ++hy) {
            [loop]
            for (int hx = -R; hx <= R; ++hx) {
                float2 np = saturate(input.uv + float2((float)hx, (float)hy) * inv_mask_size);
                halo = max(halo, old_mask.SampleLevel(samp, np, 0).b);
            }
        }
    }
    // #2 Central marker safe-zone: permanent panels live at the edges/corners,
    // world markers live centrally. Only suppress occupancy OUTSIDE the safe box
    // so a marker that lingers long enough to saturate occupancy is never grabbed.
    bool in_safe = (abs(input.uv.x - 0.5) < occ_safe_hw) && (abs(input.uv.y - 0.5) < occ_safe_hh);
    bool permanent = !in_safe && (halo > occ_gate);

    // World evidence: rot_world is direction-verified (the silhouette tracked the
    // predicted rotation flow, stronger); trans_world is the weak "changed while
    // walking" proxy. Permanent-panel suppression (occupancy) handles the animated
    // fixtures these would otherwise mis-fire on.
    bool rot_world   = (flow_valid != 0) && (peak_move > rot_move_gate);
    bool trans_world = (translating != 0) && (peak_d0 > trans_d0_gate) &&
                       (flow_valid == 0 || peak_move > -0.02);

    // Asymmetric latch: world evidence is sporadic, so grab it fast and let it
    // decay slowly; the static call has to be decisive to pull a tile down.
    float target = old;
    float a = blend_alpha;

    if (permanent) {
        target = 0.0; // #2 permanent fixture / halo -> ease to flat
        a = blend_alpha;
    } else if (rot_world || trans_world) {
        target = 1.0; // world-anchored -> gets scene depth
        a = min(blend_alpha * 3.0, 1.0);
    } else if (flow_valid != 0 && peak_stat > 0.05) {
        target = 0.0; // clearly best at zero offset -> screen-anchored
        a = blend_alpha * 0.4;
    }

    return float4(lerp(old, target, a), 0.0, occ, 0.0);
}
)";

// HUD per-tile depth pre-pass (mode 1). Pass 0 resolves ONE nearest-surface
// inv-z per world tile (a small radial scene-depth search, done once per tile
// instead of per screen-pixel). Passes >=1 min-z-flood that value across
// CONTIGUOUS world tiles (8-neighbourhood, gated by the classification mask) so
// every tile of one icon converges to the icon's single nearest depth. The
// flood stops at non-world tiles, so separate icons never bleed together — the
// contiguous-component behaviour falls out for free, no labeling needed.
static const char* const g_flat3d_huddepth_hlsl = R"(
cbuffer DepthParams : register(b0) {
    int    pass_idx; // 'pass' is a reserved HLSL keyword
    float  mask_thr;
    float2 inv_mask_size;
    float  hud_depth_uscale;
    float  hud_nearz_uu;
    float  aspect_xy;
    float  pad_;
};

Texture2D hud_mask      : register(t0); // classification mask (world = high)
Texture2D scene_depth   : register(t1); // reversed-Z device depth
Texture2D prev_tiledep  : register(t2); // last iteration's tile-depth (diffuse)
SamplerState samp       : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

// Nearest non-sky scene inv-z (1/z_uu) under a tile; 0 = no surface / sky only.
float resolve_invz(float2 uv) {
    float best = 0.0; // larger inv-z = nearer

    float dev = scene_depth.SampleLevel(samp, float2(uv.x * hud_depth_uscale, uv.y), 0).r;
    if (dev > 1e-9) {
        float z = hud_nearz_uu / dev;
        if (z < 1.0e6) best = 1.0 / z;
    }

    [loop]
    for (int ring = 1; ring <= 8 && best <= 0.0; ++ring) {
        float r = (float)ring * 0.03;
        [unroll]
        for (int a = 0; a < 8; ++a) {
            float ang = (float)a * 0.785398163; // 2*pi/8
            float2 duv = saturate(uv + float2(cos(ang) * r * aspect_xy, sin(ang) * r));
            float d = scene_depth.SampleLevel(samp, float2(duv.x * hud_depth_uscale, duv.y), 0).r;
            if (d > 1e-9) {
                float z = hud_nearz_uu / d;
                if (z < 1.0e6) best = max(best, 1.0 / z);
            }
        }
    }
    return best;
}

float ps_main(VSOut input) : SV_Target {
    // Non-world tiles carry no depth (0 => the composite keeps them flat). This
    // also bounds the flood: it never crosses a non-world gap between icons.
    if (hud_mask.SampleLevel(samp, input.uv, 0).r < mask_thr) {
        return 0.0;
    }

    if (pass_idx == 0) {
        return resolve_invz(input.uv);
    }

    // Diffuse: take the max inv-z (= nearest) over self + world neighbours.
    float best = prev_tiledep.SampleLevel(samp, input.uv, 0).r;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float2 nuv = input.uv + float2((float)x, (float)y) * inv_mask_size;
            if (nuv.x < 0.0 || nuv.x > 1.0 || nuv.y < 0.0 || nuv.y > 1.0) {
                continue;
            }
            if (hud_mask.SampleLevel(samp, nuv, 0).r >= mask_thr) {
                best = max(best, prev_tiledep.SampleLevel(samp, nuv, 0).r);
            }
        }
    }
    return best;
}
)";

// Full-screen-GUI coverage reduction: averages the redirected UI's screen
// COVERAGE (fraction of a coarse grid whose alpha clears a low bar) to a single
// value in [0,1], read back with a frame or two of latency to drive the
// cursor-independent full-screen-menu detector. Partial opacity counts — a dim
// full-screen overlay still reads as covered; it is never a hard "opaque" test.
static const char* const g_flat3d_coverage_hlsl = R"(
cbuffer CovParams : register(b0) {
    float ui_invert_alpha; // UI_InvertAlpha: undo inverted game-UI alpha before the count
};
Texture2D ui_tex  : register(t0);
SamplerState samp : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

float ps_main(VSOut input) : SV_Target {
    const int NX = 40;
    const int NY = 23; // ~16:9, 920 taps — plenty for a menu/HUD decision
    float cov = 0.0;

    [loop]
    for (int y = 0; y < NY; ++y) {
        [loop]
        for (int x = 0; x < NX; ++x) {
            float2 uv = (float2((float)x, (float)y) + 0.5) / float2((float)NX, (float)NY);
            float a = ui_tex.SampleLevel(samp, uv, 0).a;
            a = lerp(a, 1.0 - a, ui_invert_alpha); // undo inverted game-UI alpha
            cov += a > 0.15 ? 1.0 : 0.0; // partial opacity still counts
        }
    }

    return cov / (float)(NX * NY);
}
)";

} // namespace vrmod::flat3d
