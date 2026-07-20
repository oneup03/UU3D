#pragma once

#include <cstdint>
#include <unordered_map>

#include <d3d11on12.h>
#include <d3d12.h>
#include <wrl.h>

#include "Flat3DKatanga.hpp"

// DX12 Katanga publisher. The Katanga shared texture is a D3D11 (KMT) shared
// resource, so we bridge via D3D11On12: an interop D3D11 device wraps the
// composited D3D12 eye textures as D3D11 resources, and the embedded D3D11
// Flat3DKatanga renders the shared side-by-side texture + publishes the handle
// on that device. The interop context is isolated from the game's, so the
// shader pass has no game-state concern.
namespace vrmod::flat3d {

class Flat3DKatangaD3D12 {
public:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool ensure(ID3D12Device* device, ID3D12CommandQueue* queue,
                uint32_t eye_w, uint32_t eye_h, DXGI_FORMAT eye_format);

    // eye_left/eye_right are the composited D3D12 per-eye textures (in
    // PIXEL_SHADER_RESOURCE state). Wraps them as D3D11, hands them to the
    // shared D3D11 Katanga helper, then releases the wrap.
    void present(ID3D12Resource* eye_left, ID3D12Resource* eye_right);

    void shutdown();
    bool available() const { return m_katanga.available(); }

private:
    ID3D11Texture2D* wrap(ID3D12Resource* d3d12_res); // cached per resource

    ComPtr<ID3D11Device> m_d11{};
    ComPtr<ID3D11DeviceContext> m_ctx{};
    ComPtr<ID3D11On12Device> m_on12{};

    struct Wrapped {
        ComPtr<ID3D11Texture2D> tex11{};
    };
    std::unordered_map<ID3D12Resource*, Wrapped> m_wrapped{};

    Flat3DKatanga m_katanga{}; // reused D3D11 publisher path
    bool m_ready{false};
};

} // namespace vrmod::flat3d
