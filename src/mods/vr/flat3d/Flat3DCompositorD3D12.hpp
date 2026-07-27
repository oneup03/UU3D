#pragma once

#include <chrono>
#include <cstdint>

#include <d3d12.h>
#include <wrl.h>

#include "../d3d12/CommandContext.hpp"

#include "Flat3DShaders.hpp"

// LeiaSR SR SDK is optional (same detection as the D3D11 side, DX12 weaver).
#if defined(__has_include)
#if __has_include("sr/weaver/dx12weaver.h")
#define UEVR_FLAT3D_HAS_LEIASR_DX12 1
#endif
#endif

#ifdef UEVR_FLAT3D_HAS_LEIASR_DX12
namespace SR {
class SRContext;
class IDX12Weaver1;
}
#endif

namespace vrmod::flat3d {

// D3D12 counterpart of Flat3DCompositorD3D11: weaves the engine's stereo
// pair into the game's REAL backbuffer in the selected 3D output format.
// Same architecture — persistent per-eye cache (AFR-capable), UI/crosshair
// overlay passes into the eyes, repack pass into the backbuffer — built on
// a small self-contained root signature (root constants + one SRV table)
// and a 3-deep command ring. LeiaSR weaving uses the SR SDK's DX12 weaver.
class Flat3DCompositorD3D12 {
public:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    // backbuffer_pq: the game explicitly set a PQ color space on the
    // swapchain (10-bit formats are otherwise treated as SDR/G22).
    bool setup(ID3D12Device* device, uint32_t eye_w, uint32_t eye_h, DXGI_FORMAT eye_format,
               DXGI_FORMAT backbuffer_format, bool backbuffer_pq);

    // Records + executes one frame: eye-cache refresh, overlays, repack into
    // the current backbuffer. Also performs the (optional) SceneDepthZ
    // stripe readback in the same command list when scene_depth != null and
    // params.want_depth. Depth results (UE units, < 0 = invalid) are
    // returned from a ring slot ~3 frames old.
    // right_eye_src: scene-capture render target holding the RIGHT eye under
    // native-stereo-fix titles (null otherwise). menu_tex: the Framework's
    // IMGUI render target (UEVR menu), composited into the eyes at menu
    // depth (null = draw it flat on the backbuffer as before).
    // scene_depth_state: resource state scene_depth sits in outside our copies.
    // Defaults to the state UE leaves pooled SceneDepthZ in; the DSV-observer
    // snapshot (a plain copy without DEPTH_READ capability) passes PSR|NPSR.
    bool composite(ID3D12Resource* double_wide,
                   ID3D12Resource* right_eye_src,
                   ID3D12Resource* ui_tex,
                   ID3D12Resource* menu_tex,
                   ID3D12Resource* backbuffer,
                   uint32_t out_w, uint32_t out_h,
                   const Flat3DFrameParams& params,
                   ID3D12Resource* scene_depth, float nearz_uu,
                   float* out_center_uu, float* out_nearest_uu,
                   float* out_ui_coverage,
                   HWND hwnd = nullptr,
                   D3D12_RESOURCE_STATES scene_depth_state =
                       D3D12_RESOURCE_STATE_DEPTH_READ |
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    void reset();

    bool ready() const { return m_ready; }
    Flat3DColorSpace colorspace() const { return m_colorspace; }

    // Composited per-eye textures (index 0 = left), valid after composite().
    // Used by the DX12 Katanga handoff (via D3D11On12).
    ID3D12Resource* eye_texture(int i) const { return (i >= 0 && i < 2) ? m_eye_tex[i].Get() : nullptr; }
    uint32_t eye_width() const { return m_eye_w; }
    uint32_t eye_height() const { return m_eye_h; }
    DXGI_FORMAT eye_format() const { return m_eye_format; }

private:
    bool create_pipelines(ID3D12Device* device);
    // eye_refresh_mask: bit N set = eye N's scene content was refreshed this
    // present; overlays bake only into refreshed eyes (persistent caches —
    // re-baking translucent UI onto an unrefreshed eye ratchets its opacity).
    void record_overlays(ID3D12GraphicsCommandList* cmd, bool have_ui, bool have_menu, const Flat3DFrameParams& params,
                         uint32_t eye_refresh_mask);
    void read_depth_slot(uint32_t slot, float nearz_uu, float* out_center_uu, float* out_nearest_uu);
    // Builds the SbS resource (LeiaSR input) at DISPLAY resolution from the
    // two eye textures via the repack shader (upscale + eye swap + SDR color
    // correction); records into cmd, leaving m_sbs_tex in PSR state.
    void build_sbs(ID3D12GraphicsCommandList* cmd, const Flat3DFrameParams& params,
                   uint32_t out_w, uint32_t out_h);
    bool weave_leiasr(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backbuffer,
                      uint32_t out_w, uint32_t out_h, HWND hwnd);
    void destroy_leiasr();

    bool m_ready{false};
    ID3D12Device* m_device{nullptr}; // not owned
    uint32_t m_eye_w{0};
    uint32_t m_eye_h{0};
    DXGI_FORMAT m_eye_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT m_backbuffer_format{DXGI_FORMAT_UNKNOWN};
    bool m_backbuffer_pq{false};
    Flat3DColorSpace m_colorspace{Flat3DColorSpace::SDR};
    bool m_src_srgb{false};

    // Per-eye cache (index 0 = left), persistent PIXEL_SHADER_RESOURCE state
    // between frames.
    ComPtr<ID3D12Resource> m_eye_tex[2]{};

    // Descriptors: shader-visible SRV heap [eye0, eye1, ui, menu], RTV heap
    // [eye0, eye1, backbuffer(slot rewritten per frame)].
    ComPtr<ID3D12DescriptorHeap> m_srv_heap{};
    ComPtr<ID3D12DescriptorHeap> m_rtv_heap{};
    uint32_t m_srv_stride{0};
    uint32_t m_rtv_stride{0};

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_repack_pso{};
    ComPtr<ID3D12PipelineState> m_sbs_pso{}; // repack shader targeting the eye format (LeiaSR input)
    ComPtr<ID3D12PipelineState> m_overlay_pso{};

    // Command ring: slot = frame % kRing. Waiting on a slot's fence before
    // reuse also guarantees its depth-readback buffer is safe to map.
    static constexpr uint32_t kRing = 3;
    d3d12::CommandContext m_cmds[kRing]{};

    // HUD anchor constants (b1): per-ring-slot upload buffers, persistently
    // mapped. Depth-adaptive HUD state is refreshed per composite.
    ComPtr<ID3D12Resource> m_anchor_cb[3]{};
    uint8_t* m_anchor_cb_ptr[3]{};
    bool m_have_depth_srv{false};
    float m_hud_depth_uscale{1.0f};
    int32_t m_hud_mode_effective{0};

    // HUD world/static classification: tiny R8 mask ping-pong + a copy of
    // last frame's UI texture (see g_flat3d_hudclass_hlsl).
    ComPtr<ID3D12PipelineState> m_classify_pso{};
    ComPtr<ID3D12Resource> m_hud_mask[2]{};
    ComPtr<ID3D12DescriptorHeap> m_hud_mask_rtv_heap{};
    ComPtr<ID3D12Resource> m_hud_prev_ui{};
    uint64_t m_hud_prev_w{0};
    uint32_t m_hud_prev_h{0};
    DXGI_FORMAT m_hud_prev_fmt{DXGI_FORMAT_UNKNOWN};
    int m_mask_idx{0};
    bool m_hud_prev_valid{false};

    // HUD per-tile depth pre-pass (mode 1): resolve + min-z-flood one nearest-
    // surface inv-z per world tile into a 64x36 R32F ping-pong (see
    // g_flat3d_huddepth_hlsl); the final texture feeds the overlay PS at t4.
    ComPtr<ID3D12PipelineState> m_huddepth_pso{};
    ComPtr<ID3D12Resource> m_huddepth_tex[2]{};
    ComPtr<ID3D12DescriptorHeap> m_huddepth_rtv_heap{};
    bool m_huddepth_srv_made{false}; // whether the persistent td SRVs were created

    // Synced-sequential pair lock: mid-pair stash of the first eye so only
    // matched (same game-state) stereo pairs are ever displayed. Kept in
    // COPY_DEST at rest.
    ComPtr<ID3D12Resource> m_pair_pending{};
    bool m_pair_pending_valid{false};
    int m_pair_pending_eye{0}; // which eye slot the stash belongs to

    // Depth readback (one buffer per ring slot).
    static constexpr uint32_t kDepthStripes = 9;
    static constexpr uint32_t kStripeRows = 2;
    ComPtr<ID3D12Resource> m_depth_readback[kRing]{};
    bool m_depth_copied[kRing]{};
    DXGI_FORMAT m_depth_format{DXGI_FORMAT_UNKNOWN};
    uint32_t m_depth_roi_w{0};
    uint32_t m_depth_row_pitch{0};
    uint64_t m_frame{0};
    bool m_depth_disabled{false}; // set when a depth copy invalidates the command list
    float m_center_ema_uu{-1.0f};
    bool m_depth_format_warned{false};
    std::chrono::steady_clock::time_point m_last_depth_stats{};

    // Full-screen-GUI coverage: a 1x1 alpha reduction of the UI, read back one
    // buffer per ring slot (fence-safe, same as depth). EMA-smoothed and handed
    // to the runtime for the cursor-independent menu detector.
    ComPtr<ID3D12PipelineState> m_coverage_pso{};
    ComPtr<ID3D12Resource> m_coverage_rt{};              // 1x1 R32_FLOAT
    ComPtr<ID3D12Resource> m_coverage_readback[kRing]{}; // 256B each (footprint-aligned)
    bool m_coverage_copied[kRing]{};
    float m_coverage_ema{0.0f};
    bool m_coverage_disabled{false}; // a failed close permanently disables it

    // Side-by-side scratch (LeiaSR input, display-res). PSR between uses.
    ComPtr<ID3D12Resource> m_sbs_tex{};
    ComPtr<ID3D12DescriptorHeap> m_sbs_rtv_heap{}; // single RTV for the SbS build
    uint32_t m_sbs_w{0};
    uint32_t m_sbs_h{0};

#ifdef UEVR_FLAT3D_HAS_LEIASR_DX12
    SR::SRContext* m_sr_context{nullptr};
    SR::IDX12Weaver1* m_sr_weaver{nullptr};
    bool m_sr_attempted{false};
    bool m_sr_input_bound{false};
#endif
};

} // namespace vrmod::flat3d
