#include <cstring>

#include <d3dcompiler.h>
#include <dxgi.h>
#pragma comment(lib, "d3dcompiler") // runtime shader compilation (D3DCompile)

#include <spdlog/spdlog.h>

#include "Flat3DKatanga.hpp"

namespace vrmod::flat3d {

namespace {

// The Katanga named-object rendezvous. Both are CREATED by the producer.
constexpr wchar_t kKatangaMmfName[] = L"Local\\KatangaMappedFile";
constexpr wchar_t kKatangaMutexName[] = L"KatangaSetupMutex";

// View format for typeless (or already-typed) eye formats, so a typeless eye
// texture can be sampled. Mirrors Flat3DCompositorD3D11.cpp's view_format_for.
DXGI_FORMAT view_format_for(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return fmt;
    }
}

// Fullscreen triangle from SV_VertexID (no VB/IA layout). uv.y = 0 at the top,
// matching the compositor's eye textures so the published image is upright.
// The PS forces alpha = 1 because the source may be a BGRX / HDR format with
// undefined alpha, and Katanga consumers expect an opaque image.
const char g_flat3d_katanga_hlsl[] = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

Texture2D    src  : register(t0);
SamplerState samp : register(s0);

float4 ps_main(VSOut i) : SV_Target {
    return float4(src.Sample(samp, i.uv).rgb, 1.0);
}
)";

} // namespace

bool Flat3DKatanga::setup_ipc() {
    // 8-byte MMF (NOT 4): VRScreenCap maps a `usize` (8 bytes on x64); a 4-byte
    // object makes its MapViewOfFile(8) fail and silently breaks it. Katanga.exe
    // reads only the low 4 bytes and is happy either way. Init the slot to 0 so
    // an already-polling consumer sees "not ready" instead of stale data.
    HANDLE mmf = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    0, sizeof(uint64_t), kKatangaMmfName);
    if (mmf == nullptr) {
        spdlog::error("[Flat3D][Katanga] CreateFileMappingW failed (0x{:x})", GetLastError());
        return false;
    }

    void* view = MapViewOfFile(mmf, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(uint64_t));
    if (view == nullptr) {
        spdlog::error("[Flat3D][Katanga] MapViewOfFile failed (0x{:x})", GetLastError());
        CloseHandle(mmf);
        return false;
    }
    *(volatile uint64_t*)view = 0;

    // bInitialOwner=FALSE so launch order doesn't matter: whoever runs first
    // creates the kernel object, the other side attaches to it.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kKatangaMutexName);
    if (mutex == nullptr) {
        spdlog::error("[Flat3D][Katanga] CreateMutexW failed (0x{:x})", GetLastError());
        UnmapViewOfFile(view);
        CloseHandle(mmf);
        return false;
    }

    m_mmf = mmf;
    m_mmf_view = view;
    m_setup_mutex = mutex;
    m_ipc_ready = true;
    spdlog::info("[Flat3D][Katanga] IPC ready (Local\\KatangaMappedFile + KatangaSetupMutex)");
    return true;
}

bool Flat3DKatanga::ensure_pipeline(ID3D11Device* device) {
    if (m_vs != nullptr && m_ps != nullptr && m_sampler != nullptr && m_rasterizer != nullptr) {
        return true;
    }

    ComPtr<ID3DBlob> vs_blob{};
    ComPtr<ID3DBlob> ps_blob{};
    ComPtr<ID3DBlob> error_blob{};

    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        error_blob.Reset();
        const auto hr = D3DCompile(g_flat3d_katanga_hlsl, strlen(g_flat3d_katanga_hlsl), "flat3d_katanga",
                                   nullptr, nullptr, entry, target, 0, 0, &out, &error_blob);
        if (FAILED(hr)) {
            spdlog::error("[Flat3D][Katanga] shader compile failed ({}): {}", entry,
                          error_blob != nullptr ? (const char*)error_blob->GetBufferPointer() : "unknown error");
            return false;
        }
        return true;
    };

    if (!compile("vs_main", "vs_5_0", vs_blob) || !compile("ps_main", "ps_5_0", ps_blob)) {
        return false;
    }

    if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vs)) ||
        FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_ps))) {
        spdlog::error("[Flat3D][Katanga] failed to create shaders");
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &m_sampler))) {
        spdlog::error("[Flat3D][Katanga] failed to create sampler");
        return false;
    }

    // CULL_NONE — the fullscreen triangle would be culled by the default
    // CULL_BACK in render-target Y-down space.
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, &m_rasterizer))) {
        spdlog::error("[Flat3D][Katanga] failed to create rasterizer state");
        return false;
    }

    return true;
}

void Flat3DKatanga::release_shared() {
    m_shared_rtv.Reset();
    m_shared_tex.Reset();
    m_shared_handle = nullptr;
    // Zero the published slot so a reconnecting consumer doesn't read a stale
    // handle before we republish.
    if (m_mmf_view != nullptr) {
        *(volatile uint64_t*)m_mmf_view = 0;
    }
}

bool Flat3DKatanga::recreate_shared(ID3D11Device* device, uint32_t width, uint32_t height) {
    if (m_setup_mutex == nullptr) {
        return false;
    }

    // Gate (re)creation so the consumer can't sample mid-recreate.
    const DWORD wait = WaitForSingleObject(m_setup_mutex, 1000);
    if (wait != WAIT_OBJECT_0) {
        spdlog::warn("[Flat3D][Katanga] setup mutex wait 0x{:x} — consumer may be gone", (uint32_t)wait);
        return false;
    }

    release_shared();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // FIXED — every known consumer's allowlist
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // legacy KMT handle (32-bit)

    if (FAILED(device->CreateTexture2D(&td, nullptr, &m_shared_tex)) || m_shared_tex == nullptr) {
        spdlog::error("[Flat3D][Katanga] CreateTexture2D {}x{} failed", width, height);
        release_shared();
        ReleaseMutex(m_setup_mutex);
        return false;
    }

    if (FAILED(device->CreateRenderTargetView(m_shared_tex.Get(), nullptr, &m_shared_rtv)) || m_shared_rtv == nullptr) {
        spdlog::error("[Flat3D][Katanga] CreateRenderTargetView failed");
        release_shared();
        ReleaseMutex(m_setup_mutex);
        return false;
    }

    ComPtr<IDXGIResource> dxgi{};
    if (FAILED(m_shared_tex.As(&dxgi)) || dxgi == nullptr ||
        FAILED(dxgi->GetSharedHandle(&m_shared_handle)) || m_shared_handle == nullptr) {
        spdlog::error("[Flat3D][Katanga] GetSharedHandle failed");
        release_shared();
        ReleaseMutex(m_setup_mutex);
        return false;
    }

    // Stamp the handle into the 8-byte slot: low 32 bits = KMT handle, high = 0.
    if (m_mmf_view != nullptr) {
        *(volatile uint64_t*)m_mmf_view = (uint64_t)(uintptr_t)m_shared_handle;
    }

    spdlog::info("[Flat3D][Katanga] shared texture published {}x{} BGRA8 handle=0x{:x}",
                 width, height, (uint32_t)(uintptr_t)m_shared_handle);

    ReleaseMutex(m_setup_mutex);
    return true;
}

ID3D11ShaderResourceView* Flat3DKatanga::srv_for(ID3D11Device* device, ID3D11Texture2D* eye) {
    if (auto it = m_srv_cache.find(eye); it != m_srv_cache.end()) {
        return it->second.Get();
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = view_format_for(m_eye_format);
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv{};
    if (FAILED(device->CreateShaderResourceView(eye, &srv_desc, &srv)) || srv == nullptr) {
        spdlog::error("[Flat3D][Katanga] CreateShaderResourceView failed");
        return nullptr;
    }

    auto* raw = srv.Get();
    m_srv_cache.emplace(eye, std::move(srv));
    return raw;
}

bool Flat3DKatanga::ensure(ID3D11Device* device, uint32_t eye_w, uint32_t eye_h, DXGI_FORMAT eye_format) {
    if (device == nullptr || eye_w == 0 || eye_h == 0) {
        return false;
    }

    if (!m_ipc_ready && !setup_ipc()) {
        return false;
    }

    if (!ensure_pipeline(device)) {
        return false;
    }

    const bool dims_changed = (eye_w != m_eye_w || eye_h != m_eye_h);
    const bool format_changed = (eye_format != m_eye_format);

    // The compositor recreated its eye textures — the cached SRVs (and their
    // view format) are stale.
    if (dims_changed || format_changed) {
        m_srv_cache.clear();
    }

    // The shared texture is always BGRA8, so only its size matters.
    if (m_shared_tex == nullptr || dims_changed) {
        if (!recreate_shared(device, eye_w * 2, eye_h)) {
            return false;
        }
    }

    m_eye_w = eye_w;
    m_eye_h = eye_h;
    m_eye_format = eye_format;
    return true;
}

void Flat3DKatanga::present(ID3D11DeviceContext* context, ID3D11Texture2D* eye_left, ID3D11Texture2D* eye_right) {
    if (!m_ipc_ready || context == nullptr || eye_left == nullptr || eye_right == nullptr || m_shared_rtv == nullptr) {
        return;
    }

    ComPtr<ID3D11Device> device{};
    context->GetDevice(device.GetAddressOf());
    if (device == nullptr) {
        return;
    }

    auto* srv_left = srv_for(device.Get(), eye_left);
    auto* srv_right = srv_for(device.Get(), eye_right);
    if (srv_left == nullptr || srv_right == nullptr) {
        return;
    }

    context->OMSetRenderTargets(1, m_shared_rtv.GetAddressOf(), nullptr);
    context->RSSetState(m_rasterizer.Get());
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    D3D11_VIEWPORT vp{};
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.Width = (float)m_eye_w;
    vp.Height = (float)m_eye_h;

    // Katanga convention: R image on the LEFT half, L image on the RIGHT half.
    // Left half <- right eye.
    vp.TopLeftX = 0.0f;
    context->RSSetViewports(1, &vp);
    context->PSSetShaderResources(0, 1, &srv_right);
    context->Draw(3, 0);

    // Right half <- left eye.
    vp.TopLeftX = (float)m_eye_w;
    context->RSSetViewports(1, &vp);
    context->PSSetShaderResources(0, 1, &srv_left);
    context->Draw(3, 0);

    // Unbind the SRV + RTV so the compositor/game can rebind these textures
    // next frame. No full state save/restore needed — the compositor already
    // clobbers the immediate context and the game rebinds its own state.
    ID3D11ShaderResourceView* null_srv = nullptr;
    context->PSSetShaderResources(0, 1, &null_srv);
    ID3D11RenderTargetView* null_rtv = nullptr;
    context->OMSetRenderTargets(1, &null_rtv, nullptr);

    // Commit the draws: there is no swapchain Present in the headless Katanga
    // path to flush the queue, so the consumer would otherwise see a stale
    // texture until the driver flushes on its own.
    context->Flush();
}

void Flat3DKatanga::shutdown() {
    // Grab the mutex one last time so a still-running consumer doesn't sample
    // during release. Best-effort — skip the wait if it's already gone.
    if (m_ipc_ready && m_setup_mutex != nullptr) {
        if (WaitForSingleObject(m_setup_mutex, 250) == WAIT_OBJECT_0) {
            release_shared();
            ReleaseMutex(m_setup_mutex);
        } else {
            release_shared();
        }
    } else {
        release_shared();
    }

    m_srv_cache.clear();
    m_vs.Reset();
    m_ps.Reset();
    m_sampler.Reset();
    m_rasterizer.Reset();

    if (m_setup_mutex != nullptr) {
        CloseHandle(m_setup_mutex);
        m_setup_mutex = nullptr;
    }
    if (m_mmf_view != nullptr) {
        UnmapViewOfFile(m_mmf_view);
        m_mmf_view = nullptr;
    }
    if (m_mmf != nullptr) {
        CloseHandle(m_mmf);
        m_mmf = nullptr;
    }
    m_ipc_ready = false;
    m_eye_w = m_eye_h = 0;
    m_eye_format = DXGI_FORMAT_UNKNOWN;
}

} // namespace vrmod::flat3d
