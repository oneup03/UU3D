#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <d3d11.h>
#include <wrl.h>

#include "Flat3DShaders.hpp"

// LeiaSR SR SDK is optional: present only when the SDK's include dir is on
// the include path (add it + simulatedreality libs via cmake.toml to enable).
#if defined(__has_include)
#if __has_include("sr/weaver/dx11weaver.h")
#define UEVR_FLAT3D_HAS_LEIASR 1
#endif
#endif

#ifdef UEVR_FLAT3D_HAS_LEIASR
namespace SR {
class SRContext;
class IDX11Weaver1;
}
#endif

namespace vrmod::flat3d {

// Weaves the engine's stereo pair into the game's REAL backbuffer in the
// selected 3D output format (the game then presents normally). Keeps two
// persistent per-eye textures so AFR frames (only one fresh eye) still
// composite a full pair; the game UI layer and crosshair are drawn into the
// per-eye textures (with per-eye parallax) before the repack.
class Flat3DCompositorD3D11 {
public:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    // (Re)creates eye textures + pipelines. Safe to call every frame; no-ops
    // when nothing changed. backbuffer_format drives HDR colorspace handling;
    // hwnd is used by the (optional) LeiaSR weaver.
    // backbuffer_pq: the game explicitly set a PQ color space on the
    // swapchain (10-bit formats are otherwise treated as SDR/G22).
    bool setup(ID3D11Device* device, uint32_t eye_w, uint32_t eye_h, DXGI_FORMAT eye_format,
               DXGI_FORMAT backbuffer_format, bool backbuffer_pq, HWND hwnd);

    // Full frame: refresh eye cache from the double-wide, draw UI/crosshair
    // overlays into each eye, then repack into the backbuffer RTV (or weave
    // via LeiaSR). ui_srv may be null (no redirected UI this frame).
    // right_eye_src: scene-capture render target holding the RIGHT eye under
    // native-stereo-fix titles (null otherwise). menu_tex: the Framework's
    // IMGUI render target (UEVR menu), composited into the eyes at menu
    // depth (null = draw it flat on the backbuffer as before).
    bool composite(ID3D11DeviceContext* context,
                   ID3D11Texture2D* double_wide,
                   ID3D11Texture2D* right_eye_src,
                   ID3D11ShaderResourceView* ui_srv,
                   ID3D11Texture2D* menu_tex,
                   ID3D11Texture2D* scene_depth,
                   ID3D11RenderTargetView* backbuffer_rtv,
                   uint32_t out_w, uint32_t out_h,
                   const Flat3DFrameParams& params,
                   float* out_ui_coverage = nullptr);

    // Reads center (aim) + nearest scene depth from SceneDepthZ without
    // stalling (staging ring; results are ~ring-depth frames old). Outputs
    // are in UE units, < 0 when no valid sample. EMA-smoothed internally.
    void sample_depth(ID3D11DeviceContext* context, ID3D11Texture2D* scene_depth,
                      float nearz_uu, float* out_center_uu, float* out_nearest_uu);

    void reset();

    bool ready() const { return m_ready; }
    uint32_t eye_width() const { return m_eye_w; }
    uint32_t eye_height() const { return m_eye_h; }
    DXGI_FORMAT eye_format() const { return m_eye_format; }
    Flat3DColorSpace colorspace() const { return m_colorspace; }
    bool leiasr_available() const;

    // Composited per-eye textures (index 0 = left), valid after composite().
    // Used by the Katanga handoff to build the shared SbS frame.
    ID3D11Texture2D* eye_texture(int i) const { return (i >= 0 && i < 2) ? m_eye_tex[i].Get() : nullptr; }

    // Saves a 3D screenshot of the last composited frame: renders the two
    // composited eye textures into an SbS image via the repack shader (so the
    // configured SDR color correction is applied and HDR eye formats are
    // normalized), then encodes PNGs — a parallel-view (left|right) and a
    // cross-view (right|left, for cross-eyed free-viewing). Either path may be
    // empty to skip that variant. Call after composite() on the immediate
    // context. Returns true only if every requested variant was written.
    bool save_screenshot(ID3D11DeviceContext* context, const Flat3DFrameParams& params,
                         const std::wstring& parallel_path, const std::wstring& crossview_path);

    // 3D screenshot capture control. begin_screenshot() starts a short capture
    // during which composite() hides the UEVR menu — keeping the game HUD,
    // crosshair and stereo cursor — and waits for BOTH eyes to be refreshed
    // that way (AFR-correct). Once screenshot_capture_complete() returns true
    // the caller saves via save_screenshot (the eye cache now holds the
    // menu-free pair) and calls end_screenshot().
    void begin_screenshot() { m_ss_active = true; m_ss_captured_mask = 0; m_ss_frames = 0; }
    bool screenshot_active() const { return m_ss_active; }
    bool screenshot_capture_complete() const {
        return m_ss_active && (m_ss_captured_mask == 0b11u || m_ss_frames >= kScreenshotMaxFrames);
    }
    void end_screenshot() { m_ss_active = false; }

private:
    bool create_pipeline(ID3D11Device* device);
    // Renders the two composited eye textures into m_sbs_tex (created/resized on
    // demand) as a display-res side-by-side image via the repack shader in SBS
    // mode — honoring params.eye_swap and applying the SDR color correction.
    // Shared by the LeiaSR weaver input and the screenshot path. Returns false
    // only if the SbS texture could not be (re)created.
    bool build_sbs(ID3D11DeviceContext* context, const Flat3DFrameParams& params,
                   uint32_t out_w, uint32_t out_h);
    // eye_refresh_mask: bit N set = eye N's scene content was refreshed this
    // present; overlays bake only into refreshed eyes (persistent caches —
    // re-baking translucent UI onto an unrefreshed eye ratchets its opacity).
    void draw_overlays(ID3D11DeviceContext* context, ID3D11ShaderResourceView* ui_srv,
                       ID3D11ShaderResourceView* menu_srv, const Flat3DFrameParams& params,
                       uint32_t eye_refresh_mask);
    bool weave_leiasr(ID3D11DeviceContext* context, ID3D11RenderTargetView* backbuffer_rtv,
                      uint32_t out_w, uint32_t out_h, const Flat3DFrameParams& params);
    void destroy_leiasr();

    bool m_ready{false};

    // 3D screenshot capture state (see begin_screenshot). While active, composite
    // hides the UEVR menu and accumulates which eyes have been refreshed that
    // way; the capture completes once both have (or after the frame budget — a
    // backstop for a stuck pair-lock that never republishes).
    static constexpr int kScreenshotMaxFrames = 16;
    bool m_ss_active{false};
    uint32_t m_ss_captured_mask{0};
    int m_ss_frames{0};

    uint32_t m_eye_w{0};
    uint32_t m_eye_h{0};
    DXGI_FORMAT m_eye_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT m_backbuffer_format{DXGI_FORMAT_UNKNOWN};
    bool m_backbuffer_pq{false};
    Flat3DColorSpace m_colorspace{Flat3DColorSpace::SDR};
    bool m_src_srgb{false};
    HWND m_hwnd{nullptr};

    // Persistent per-eye cache (index 0 = left).
    ComPtr<ID3D11Texture2D> m_eye_tex[2]{};
    ComPtr<ID3D11ShaderResourceView> m_eye_srv[2]{};
    ComPtr<ID3D11RenderTargetView> m_eye_rtv[2]{};

    // Synced-sequential pair lock: mid-pair stash of the first eye so only
    // matched (same game-state) stereo pairs are ever displayed.
    ComPtr<ID3D11Texture2D> m_pair_pending{};
    bool m_pair_pending_valid{false};
    int m_pair_pending_eye{0}; // which eye slot the stash belongs to

    // Cached SRV over the Framework's IMGUI RT (recreated when the texture
    // pointer changes, e.g. after a device reset).
    ComPtr<ID3D11ShaderResourceView> m_menu_srv{};
    ID3D11Texture2D* m_menu_src{nullptr}; // not owned; identity key only

    // Depth-adaptive HUD: cached SRV over SceneDepthZ + anchor constants (b1).
    ComPtr<ID3D11ShaderResourceView> m_hud_depth_srv{};
    ID3D11Texture2D* m_hud_depth_src{nullptr}; // not owned; identity key only
    float m_hud_depth_uscale{1.0f};
    ComPtr<ID3D11Buffer> m_anchor_cb{};
    int32_t m_hud_mode_effective{0};

    // HUD world/static classification: tiny R8 mask ping-pong + a copy of
    // last frame's UI texture (see g_flat3d_hudclass_hlsl).
    ComPtr<ID3D11VertexShader> m_classify_vs{};
    ComPtr<ID3D11PixelShader> m_classify_ps{};
    ComPtr<ID3D11Buffer> m_classify_cb{};
    ComPtr<ID3D11Texture2D> m_hud_mask_tex[2]{};
    ComPtr<ID3D11RenderTargetView> m_hud_mask_rtv[2]{};
    ComPtr<ID3D11ShaderResourceView> m_hud_mask_srv[2]{};
    ComPtr<ID3D11Texture2D> m_hud_prev_ui{};
    ComPtr<ID3D11ShaderResourceView> m_hud_prev_srv{};
    uint32_t m_hud_prev_w{0};
    uint32_t m_hud_prev_h{0};
    DXGI_FORMAT m_hud_prev_fmt{DXGI_FORMAT_UNKNOWN};
    int m_mask_idx{0};
    bool m_hud_prev_valid{false};

    // Per-tile HUD depth pre-pass (mode 1): resolve + min-z-flood one nearest
    // surface depth per world tile into a tiny 64x36 R32F ping-pong (see
    // g_flat3d_huddepth_hlsl). Sampled by the overlay shader at t4.
    ComPtr<ID3D11VertexShader> m_huddepth_vs{};
    ComPtr<ID3D11PixelShader> m_huddepth_ps{};
    ComPtr<ID3D11Buffer> m_huddepth_cb{};
    ComPtr<ID3D11Texture2D> m_huddepth_tex[2]{};
    ComPtr<ID3D11RenderTargetView> m_huddepth_rtv[2]{};
    ComPtr<ID3D11ShaderResourceView> m_huddepth_srv[2]{};
    int m_huddepth_idx{0};

    // Repack pipeline.
    ComPtr<ID3D11VertexShader> m_vs{};
    ComPtr<ID3D11PixelShader> m_ps{};
    ComPtr<ID3D11Buffer> m_cb{};
    ComPtr<ID3D11SamplerState> m_sampler{};
    ComPtr<ID3D11RasterizerState> m_rasterizer{};
    ComPtr<ID3D11BlendState> m_blend{};
    ComPtr<ID3D11DepthStencilState> m_depth{};

    // Overlay pipeline (UI / crosshair region / laser dot).
    ComPtr<ID3D11VertexShader> m_overlay_vs{};
    ComPtr<ID3D11PixelShader> m_overlay_ps{};
    ComPtr<ID3D11Buffer> m_overlay_cb{};
    ComPtr<ID3D11BlendState> m_overlay_blend{}; // premultiplied alpha

    // Depth readback (staging ring, non-blocking).
    static constexpr uint32_t kDepthRing = 3;
    static constexpr uint32_t kDepthStripes = 9;
    static constexpr uint32_t kStripeRows = 2;
    ComPtr<ID3D11Texture2D> m_depth_staging[kDepthRing]{};
    DXGI_FORMAT m_depth_format{DXGI_FORMAT_UNKNOWN};
    uint32_t m_depth_roi_w{0};
    uint64_t m_depth_frame{0};
    float m_center_ema_uu{-1.0f};
    bool m_depth_format_warned{false};
    std::chrono::steady_clock::time_point m_last_depth_stats{};

    // Full-screen-GUI coverage: a 1x1 alpha reduction of the UI + a small
    // staging ring for non-blocking readback. EMA-smoothed and handed to the
    // runtime for the cursor-independent menu detector.
    ComPtr<ID3D11VertexShader> m_coverage_vs{};
    ComPtr<ID3D11PixelShader> m_coverage_ps{};
    ComPtr<ID3D11Buffer> m_coverage_cb{};                 // b0: ui_invert_alpha
    ComPtr<ID3D11Texture2D> m_coverage_rt{};              // 1x1 R32_FLOAT
    ComPtr<ID3D11RenderTargetView> m_coverage_rtv{};
    ComPtr<ID3D11Texture2D> m_coverage_staging[kDepthRing]{};
    uint64_t m_coverage_frame{0};
    float m_coverage_ema{0.0f};

    // Screenshot SbS target: an 8-bit RTV mirroring the backbuffer's sRGB-ness
    // (see screenshot_8bit_format), rendered with the display repack config so
    // the saved pair matches on-screen color but stays 8-bit for a clean PNG —
    // separate from the LeiaSR eye-format m_sbs_tex.
    ComPtr<ID3D11Texture2D> m_screenshot_tex{};
    ComPtr<ID3D11RenderTargetView> m_screenshot_rtv{};
    uint32_t m_screenshot_w{0};
    uint32_t m_screenshot_h{0};
    DXGI_FORMAT m_screenshot_fmt{DXGI_FORMAT_UNKNOWN};

    // LeiaSR weaver (optional). Own SbS texture because the weaver takes one
    // side-by-side input.
    ComPtr<ID3D11Texture2D> m_sbs_tex{};
    ComPtr<ID3D11ShaderResourceView> m_sbs_srv{};
    ComPtr<ID3D11RenderTargetView> m_sbs_rtv{}; // repack-shader SbS build (eye swap + color correction)
    uint32_t m_sbs_w{0}; // display-res SbS (per-eye) dimensions
    uint32_t m_sbs_h{0};
#ifdef UEVR_FLAT3D_HAS_LEIASR
    SR::SRContext* m_sr_context{nullptr};
    SR::IDX11Weaver1* m_sr_weaver{nullptr};
    bool m_sr_attempted{false};
    bool m_sr_input_bound{false};
#endif
};

} // namespace vrmod::flat3d
