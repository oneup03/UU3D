#include <spdlog/spdlog.h>

#include "Flat3DKatangaD3D12.hpp"

namespace vrmod::flat3d {

bool Flat3DKatangaD3D12::ensure(ID3D12Device* device, ID3D12CommandQueue* queue,
                                uint32_t eye_w, uint32_t eye_h, DXGI_FORMAT eye_format) {
    if (device == nullptr || queue == nullptr || eye_w == 0 || eye_h == 0) {
        return false;
    }

    // One-shot D3D11On12 interop device on the game's D3D12 queue. BGRA support
    // is required — the Katanga shared texture is BGRA8.
    if (m_d11 == nullptr) {
        IUnknown* queues[] = {queue};
        const auto hr = D3D11On12CreateDevice(device, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                              queues, 1, 0, &m_d11, &m_ctx, nullptr);
        if (FAILED(hr) || m_d11 == nullptr) {
            spdlog::error("[Flat3D][Katanga12] D3D11On12CreateDevice failed (hr=0x{:x})", (uint32_t)hr);
            return false;
        }
        if (FAILED(m_d11.As(&m_on12))) {
            spdlog::error("[Flat3D][Katanga12] QueryInterface ID3D11On12Device failed");
            m_d11.Reset();
            m_ctx.Reset();
            return false;
        }
    }

    // The shared texture / IPC live on the interop D3D11 device.
    if (!m_katanga.ensure(m_d11.Get(), eye_w, eye_h, eye_format)) {
        return false;
    }

    m_ready = true;
    return true;
}

ID3D11Texture2D* Flat3DKatangaD3D12::wrap(ID3D12Resource* d3d12_res) {
    if (d3d12_res == nullptr || m_on12 == nullptr) {
        return nullptr;
    }

    if (auto it = m_wrapped.find(d3d12_res); it != m_wrapped.end()) {
        return it->second.tex11.Get();
    }

    D3D11_RESOURCE_FLAGS flags{};
    flags.BindFlags = D3D11_BIND_SHADER_RESOURCE; // we only sample the eyes

    Wrapped w{};
    // The eye textures sit in PIXEL_SHADER_RESOURCE on the D3D12 side between
    // our frames; the wrap keeps them there on acquire/release.
    const auto hr = m_on12->CreateWrappedResource(
        d3d12_res, &flags,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        IID_PPV_ARGS(&w.tex11));

    if (FAILED(hr) || w.tex11 == nullptr) {
        spdlog::error("[Flat3D][Katanga12] CreateWrappedResource failed (hr=0x{:x})", (uint32_t)hr);
        return nullptr;
    }

    auto* raw = w.tex11.Get();
    m_wrapped.emplace(d3d12_res, std::move(w));
    return raw;
}

void Flat3DKatangaD3D12::present(ID3D12Resource* eye_left, ID3D12Resource* eye_right) {
    if (!m_ready || m_on12 == nullptr || eye_left == nullptr || eye_right == nullptr) {
        return;
    }

    auto* l11 = wrap(eye_left);
    auto* r11 = wrap(eye_right);
    if (l11 == nullptr || r11 == nullptr) {
        return;
    }

    ID3D11Resource* to_acquire[] = {l11, r11};
    m_on12->AcquireWrappedResources(to_acquire, 2);

    // The D3D11 helper renders both eyes into its shared SbS texture and
    // publishes the handle via the Katanga IPC.
    m_katanga.present(m_ctx.Get(), (ID3D11Texture2D*)l11, (ID3D11Texture2D*)r11);

    m_on12->ReleaseWrappedResources(to_acquire, 2);
    m_ctx->Flush();
}

void Flat3DKatangaD3D12::shutdown() {
    m_katanga.shutdown();
    m_wrapped.clear();
    m_on12.Reset();
    m_ctx.Reset();
    m_d11.Reset();
    m_ready = false;
}

} // namespace vrmod::flat3d
