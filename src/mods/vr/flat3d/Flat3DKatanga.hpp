#pragma once

#include <cstdint>
#include <unordered_map>

#include <d3d11.h>
#include <wrl.h>

// Katanga shared-texture IPC publisher for Flat3D output.
//
// Publishes the composited stereo pair as a single full side-by-side
// (2W x H) D3D11 shared texture to Katanga.exe / VRScreenCap via the Katanga
// protocol (reverse-engineered in 3DVision4All's Output_Katanga.cpp):
//   - an 8-byte page-file-backed MMF "Local\KatangaMappedFile" carrying the
//     shared texture's KMT handle value, and
//   - a named mutex "KatangaSetupMutex" gating texture (re)creation.
// WE create both objects (consumers only Open them), so launch order is
// irrelevant and the producer keeps publishing if the consumer dies/relaunches.
// The published texture is always DXGI_FORMAT_B8G8R8A8_UNORM and laid out
// R-on-LEFT / L-on-RIGHT (the Katanga ecosystem convention). A fullscreen
// shader pass converts any source eye format (incl. HDR/typeless) to BGRA8 and
// forces alpha = 1 — a CopyResource can't, since the eye format is a different
// typeless family than BGRA8.
namespace vrmod::flat3d {

class Flat3DKatanga {
public:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    // Ensures IPC + shader pipeline are up and the shared texture matches the
    // eye size. Returns false (and no-ops) if setup failed. eye_w/eye_h are
    // per-eye (single-eye) render dims; the shared texture is 2*eye_w x eye_h.
    bool ensure(ID3D11Device* device, uint32_t eye_w, uint32_t eye_h, DXGI_FORMAT eye_format);

    // Renders eye_left|eye_right into the shared texture (R-on-LEFT) and
    // flushes so the cross-process consumer sees committed GPU work. The
    // caller passes the eyes already eye_swap-resolved (l = eye_swap?1:0).
    void present(ID3D11DeviceContext* context, ID3D11Texture2D* eye_left, ID3D11Texture2D* eye_right);

    void shutdown();
    bool available() const { return m_ipc_ready; }

private:
    bool setup_ipc();
    bool ensure_pipeline(ID3D11Device* device);
    bool recreate_shared(ID3D11Device* device, uint32_t width, uint32_t height);
    void release_shared();
    ID3D11ShaderResourceView* srv_for(ID3D11Device* device, ID3D11Texture2D* eye);

    // IPC (Local\KatangaMappedFile + KatangaSetupMutex).
    HANDLE m_mmf{nullptr};
    void* m_mmf_view{nullptr};
    HANDLE m_setup_mutex{nullptr};
    bool m_ipc_ready{false};

    // Additive extension to the Katanga contract: an auto-reset event
    // (Local\KatangaFrameReady) signaled once per published frame, right after
    // the GPU flush, so an extension-aware consumer (WWInjector) can publish
    // frame-accurately instead of polling on a timer. Legacy consumers never
    // open this name and are unaffected; the MMF slot and setup mutex are
    // untouched. Optional — publishing works identically if creation fails.
    HANDLE m_frame_event{nullptr};

    // Single stable shared SbS texture (recreated only on size change — the
    // consumer reads ONE handle from the MMF and samples it in place).
    ComPtr<ID3D11Texture2D> m_shared_tex{};
    ComPtr<ID3D11RenderTargetView> m_shared_rtv{};
    HANDLE m_shared_handle{nullptr};

    // Fullscreen-pass pipeline (sample eye SRV -> BGRA8 RTV, force alpha=1).
    ComPtr<ID3D11VertexShader> m_vs{};
    ComPtr<ID3D11PixelShader> m_ps{};
    ComPtr<ID3D11SamplerState> m_sampler{};
    ComPtr<ID3D11RasterizerState> m_rasterizer{};

    // SRVs over the per-eye textures, keyed by texture pointer (cleared when
    // the eye size/format changes, i.e. the compositor recreated them).
    std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> m_srv_cache{};

    uint32_t m_eye_w{0};
    uint32_t m_eye_h{0};
    DXGI_FORMAT m_eye_format{DXGI_FORMAT_UNKNOWN};
};

} // namespace vrmod::flat3d
