#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler") // runtime shader compilation (D3DCompile)

#include <wincodec.h>
#include <ScreenGrab.h>

#include <spdlog/spdlog.h>

#include "Flat3DCompositorD3D11.hpp"

#ifdef UEVR_FLAT3D_HAS_LEIASR
#include <sr/management/srcontext.h>
#include <sr/weaver/dx11weaver.h>
#endif

namespace vrmod::flat3d {

namespace {
// Compact scoped backup of every piece of pipeline state the compositor
// touches (same spirit as D3D11Component.cpp's DX11StateBackup, which is
// file-local there).
class ScopedD3D11State {
public:
    explicit ScopedD3D11State(ID3D11DeviceContext* ctx) : m_ctx{ctx} {
        m_ctx->IAGetInputLayout(&m_layout);
        m_ctx->IAGetPrimitiveTopology(&m_topology);
        m_ctx->IAGetVertexBuffers(0, 1, &m_vb, &m_vb_stride, &m_vb_offset);
        m_ctx->IAGetIndexBuffer(&m_ib, &m_ib_format, &m_ib_offset);
        m_ctx->VSGetShader(&m_vs, nullptr, nullptr);
        m_ctx->VSGetConstantBuffers(0, 1, &m_vs_cb);
        m_ctx->PSGetShader(&m_ps, nullptr, nullptr);
        m_ctx->PSGetConstantBuffers(0, 1, &m_ps_cb);
        ID3D11ShaderResourceView* srvs[2]{};
        m_ctx->PSGetShaderResources(0, 2, srvs);
        for (size_t i = 0; i < 2; ++i) {
            m_ps_srvs[i].Attach(srvs[i]);
        }
        m_ctx->PSGetSamplers(0, 1, &m_ps_sampler);
        m_ctx->GSGetShader(&m_gs, nullptr, nullptr);
        m_ctx->HSGetShader(&m_hs, nullptr, nullptr);
        m_ctx->DSGetShader(&m_ds, nullptr, nullptr);
        m_ctx->RSGetState(&m_rs);
        m_num_viewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        m_ctx->RSGetViewports(&m_num_viewports, m_viewports);
        m_num_scissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        m_ctx->RSGetScissorRects(&m_num_scissors, m_scissors);
        m_ctx->OMGetBlendState(&m_blend, m_blend_factor, &m_sample_mask);
        m_ctx->OMGetDepthStencilState(&m_depth, &m_stencil_ref);
        ID3D11RenderTargetView* raw_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        m_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, raw_rtvs, &m_dsv);
        for (size_t i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            m_rtvs[i].Attach(raw_rtvs[i]);
        }
    }

    ~ScopedD3D11State() {
        m_ctx->IASetInputLayout(m_layout.Get());
        m_ctx->IASetPrimitiveTopology(m_topology);
        m_ctx->IASetVertexBuffers(0, 1, m_vb.GetAddressOf(), &m_vb_stride, &m_vb_offset);
        m_ctx->IASetIndexBuffer(m_ib.Get(), m_ib_format, m_ib_offset);
        m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
        m_ctx->VSSetConstantBuffers(0, 1, m_vs_cb.GetAddressOf());
        m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
        m_ctx->PSSetConstantBuffers(0, 1, m_ps_cb.GetAddressOf());
        ID3D11ShaderResourceView* srvs[2] = {m_ps_srvs[0].Get(), m_ps_srvs[1].Get()};
        m_ctx->PSSetShaderResources(0, 2, srvs);
        m_ctx->PSSetSamplers(0, 1, m_ps_sampler.GetAddressOf());
        m_ctx->GSSetShader(m_gs.Get(), nullptr, 0);
        m_ctx->HSSetShader(m_hs.Get(), nullptr, 0);
        m_ctx->DSSetShader(m_ds.Get(), nullptr, 0);
        m_ctx->RSSetState(m_rs.Get());
        if (m_num_viewports > 0) {
            m_ctx->RSSetViewports(m_num_viewports, m_viewports);
        }
        if (m_num_scissors > 0) {
            m_ctx->RSSetScissorRects(m_num_scissors, m_scissors);
        }
        m_ctx->OMSetBlendState(m_blend.Get(), m_blend_factor, m_sample_mask);
        m_ctx->OMSetDepthStencilState(m_depth.Get(), m_stencil_ref);
        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        for (size_t i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            rtvs[i] = m_rtvs[i].Get();
        }
        m_ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, m_dsv.Get());
    }

private:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ID3D11DeviceContext* m_ctx{};
    ComPtr<ID3D11InputLayout> m_layout{};
    D3D11_PRIMITIVE_TOPOLOGY m_topology{};
    ComPtr<ID3D11Buffer> m_vb{};
    UINT m_vb_stride{}, m_vb_offset{};
    ComPtr<ID3D11Buffer> m_ib{};
    DXGI_FORMAT m_ib_format{};
    UINT m_ib_offset{};
    ComPtr<ID3D11VertexShader> m_vs{};
    ComPtr<ID3D11Buffer> m_vs_cb{};
    ComPtr<ID3D11PixelShader> m_ps{};
    ComPtr<ID3D11Buffer> m_ps_cb{};
    ComPtr<ID3D11ShaderResourceView> m_ps_srvs[2]{};
    ComPtr<ID3D11SamplerState> m_ps_sampler{};
    ComPtr<ID3D11GeometryShader> m_gs{};
    ComPtr<ID3D11HullShader> m_hs{};
    ComPtr<ID3D11DomainShader> m_ds{};
    ComPtr<ID3D11RasterizerState> m_rs{};
    UINT m_num_viewports{};
    D3D11_VIEWPORT m_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT m_num_scissors{};
    D3D11_RECT m_scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ComPtr<ID3D11BlendState> m_blend{};
    float m_blend_factor[4]{};
    UINT m_sample_mask{};
    ComPtr<ID3D11DepthStencilState> m_depth{};
    UINT m_stencil_ref{};
    ComPtr<ID3D11RenderTargetView> m_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ComPtr<ID3D11DepthStencilView> m_dsv{};
};

// View format for typeless (or already-typed) color formats.
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
} // namespace

bool Flat3DCompositorD3D11::setup(ID3D11Device* device, uint32_t eye_w, uint32_t eye_h, DXGI_FORMAT eye_format,
                                  DXGI_FORMAT backbuffer_format, bool backbuffer_pq, HWND hwnd) {
    if (m_ready && eye_w == m_eye_w && eye_h == m_eye_h && eye_format == m_eye_format &&
        backbuffer_format == m_backbuffer_format && backbuffer_pq == m_backbuffer_pq) {
        m_hwnd = hwnd;
        return true;
    }

    reset();

    if (device == nullptr || eye_w == 0 || eye_h == 0) {
        return false;
    }

    m_hwnd = hwnd;
    m_backbuffer_format = backbuffer_format;
    m_backbuffer_pq = backbuffer_pq;

    // HDR colorspace from the swapchain format. R16F is unambiguously scRGB.
    // 10-bit is NOT inherently HDR: SDR games commonly run R10G10B10A2
    // swapchains with plain G22 gamma — PQ-encoding those washes the image
    // out. Only treat as HDR10 when the game explicitly set a PQ color space.
    switch (backbuffer_format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        m_colorspace = Flat3DColorSpace::SCRGB;
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        m_colorspace = backbuffer_pq ? Flat3DColorSpace::HDR10_PQ : Flat3DColorSpace::SDR;
        break;
    default:
        m_colorspace = Flat3DColorSpace::SDR;
        break;
    }

    // 8-bit engine target under an HDR swapchain needs an sRGB->HDR encode
    // in the repack/overlay passes.
    const bool eye_is_8bit =
        eye_format == DXGI_FORMAT_B8G8R8A8_UNORM || eye_format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
        eye_format == DXGI_FORMAT_R8G8B8A8_UNORM || eye_format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        eye_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || eye_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    m_src_srgb = eye_is_8bit && m_colorspace != Flat3DColorSpace::SDR;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = eye_w;
    desc.Height = eye_h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = eye_format; // same family as the double-wide -> CopySubresourceRegion-compatible
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    const auto view_fmt = view_format_for(eye_format);

    // Eye size/format changed: drop the synced-pair stash so it is lazily
    // recreated at the new dimensions.
    m_pair_pending.Reset();
    m_pair_pending_valid = false;

    for (int i = 0; i < 2; ++i) {
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_eye_tex[i]))) {
            spdlog::error("[Flat3D] Failed to create eye texture {}", i);
            reset();
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = view_fmt;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;

        if (FAILED(device->CreateShaderResourceView(m_eye_tex[i].Get(), &srv_desc, &m_eye_srv[i]))) {
            spdlog::error("[Flat3D] Failed to create eye SRV {}", i);
            reset();
            return false;
        }

        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = view_fmt;
        rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

        if (FAILED(device->CreateRenderTargetView(m_eye_tex[i].Get(), &rtv_desc, &m_eye_rtv[i]))) {
            spdlog::error("[Flat3D] Failed to create eye RTV {}", i);
            reset();
            return false;
        }
    }

    // Full-screen-GUI coverage: 1x1 R32_FLOAT target + a small staging ring.
    // All optional — failure just leaves the coverage signal off (cursor +
    // pause detection still work).
    {
        D3D11_TEXTURE2D_DESC cov_desc{};
        cov_desc.Width = 1;
        cov_desc.Height = 1;
        cov_desc.MipLevels = 1;
        cov_desc.ArraySize = 1;
        cov_desc.Format = DXGI_FORMAT_R32_FLOAT;
        cov_desc.SampleDesc.Count = 1;
        cov_desc.Usage = D3D11_USAGE_DEFAULT;
        cov_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

        if (SUCCEEDED(device->CreateTexture2D(&cov_desc, nullptr, &m_coverage_rt)) &&
            SUCCEEDED(device->CreateRenderTargetView(m_coverage_rt.Get(), nullptr, &m_coverage_rtv))) {
            D3D11_TEXTURE2D_DESC st_desc = cov_desc;
            st_desc.Usage = D3D11_USAGE_STAGING;
            st_desc.BindFlags = 0;
            st_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            for (auto& tex : m_coverage_staging) {
                if (FAILED(device->CreateTexture2D(&st_desc, nullptr, &tex))) {
                    for (auto& t : m_coverage_staging) t.Reset();
                    m_coverage_rt.Reset();
                    m_coverage_rtv.Reset();
                    break;
                }
            }
        } else {
            m_coverage_rt.Reset();
            m_coverage_rtv.Reset();
        }
    }

    if (!create_pipeline(device)) {
        reset();
        return false;
    }

    m_eye_w = eye_w;
    m_eye_h = eye_h;
    m_eye_format = eye_format;
    m_ready = true;

    spdlog::info("[Flat3D] Compositor ready: {}x{} per eye (format {})", eye_w, eye_h, (uint32_t)eye_format);

    return true;
}

bool Flat3DCompositorD3D11::create_pipeline(ID3D11Device* device) {
    Microsoft::WRL::ComPtr<ID3DBlob> vs_blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> ps_blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> error_blob{};

    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        const auto hr = D3DCompile(g_flat3d_repack_hlsl, strlen(g_flat3d_repack_hlsl), "flat3d_repack",
                                   nullptr, nullptr, entry, target, 0, 0, &out, &error_blob);
        if (FAILED(hr)) {
            spdlog::error("[Flat3D] Shader compile failed ({}): {}", entry,
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
        spdlog::error("[Flat3D] Failed to create shaders");
        return false;
    }

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = sizeof(RepackConstants);
    cb_desc.Usage = D3D11_USAGE_DEFAULT;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    if (FAILED(device->CreateBuffer(&cb_desc, nullptr, &m_cb))) {
        spdlog::error("[Flat3D] Failed to create constant buffer");
        return false;
    }

    D3D11_SAMPLER_DESC samp_desc{};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.MaxLOD = D3D11_FLOAT32_MAX;

    if (FAILED(device->CreateSamplerState(&samp_desc, &m_sampler))) {
        spdlog::error("[Flat3D] Failed to create sampler");
        return false;
    }

    D3D11_RASTERIZER_DESC rs_desc{};
    rs_desc.FillMode = D3D11_FILL_SOLID;
    rs_desc.CullMode = D3D11_CULL_NONE;
    rs_desc.DepthClipEnable = TRUE;

    if (FAILED(device->CreateRasterizerState(&rs_desc, &m_rasterizer))) {
        spdlog::error("[Flat3D] Failed to create rasterizer state");
        return false;
    }

    D3D11_BLEND_DESC blend_desc{}; // opaque
    blend_desc.RenderTarget[0].BlendEnable = FALSE;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (FAILED(device->CreateBlendState(&blend_desc, &m_blend))) {
        spdlog::error("[Flat3D] Failed to create blend state");
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC ds_desc{}; // depth off
    ds_desc.DepthEnable = FALSE;
    ds_desc.StencilEnable = FALSE;

    if (FAILED(device->CreateDepthStencilState(&ds_desc, &m_depth))) {
        spdlog::error("[Flat3D] Failed to create depth-stencil state");
        return false;
    }

    // ---- overlay pipeline (UI / crosshair region / laser dot) --------------
    Microsoft::WRL::ComPtr<ID3DBlob> ovs_blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> ops_blob{};

    auto compile_overlay = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        Microsoft::WRL::ComPtr<ID3DBlob> err{};
        const auto hr = D3DCompile(g_flat3d_overlay_hlsl, strlen(g_flat3d_overlay_hlsl), "flat3d_overlay",
                                   nullptr, nullptr, entry, target, 0, 0, &out, &err);
        if (FAILED(hr)) {
            spdlog::error("[Flat3D] Overlay shader compile failed ({}): {}", entry,
                          err != nullptr ? (const char*)err->GetBufferPointer() : "unknown error");
            return false;
        }
        return true;
    };

    if (!compile_overlay("vs_main", "vs_5_0", ovs_blob) || !compile_overlay("ps_main", "ps_5_0", ops_blob)) {
        return false;
    }

    if (FAILED(device->CreateVertexShader(ovs_blob->GetBufferPointer(), ovs_blob->GetBufferSize(), nullptr, &m_overlay_vs)) ||
        FAILED(device->CreatePixelShader(ops_blob->GetBufferPointer(), ops_blob->GetBufferSize(), nullptr, &m_overlay_ps))) {
        spdlog::error("[Flat3D] Failed to create overlay shaders");
        return false;
    }

    D3D11_BUFFER_DESC ocb_desc{};
    ocb_desc.ByteWidth = sizeof(OverlayConstants);
    ocb_desc.Usage = D3D11_USAGE_DEFAULT;
    ocb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    if (FAILED(device->CreateBuffer(&ocb_desc, nullptr, &m_overlay_cb))) {
        spdlog::error("[Flat3D] Failed to create overlay constant buffer");
        return false;
    }

    D3D11_BUFFER_DESC acb_desc{};
    acb_desc.ByteWidth = sizeof(HudAnchorConstants);
    acb_desc.Usage = D3D11_USAGE_DEFAULT;
    acb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    if (FAILED(device->CreateBuffer(&acb_desc, nullptr, &m_anchor_cb))) {
        spdlog::error("[Flat3D] Failed to create HUD anchor constant buffer");
        return false;
    }

    // HUD world/static classification pass.
    {
        Microsoft::WRL::ComPtr<ID3DBlob> cvs_blob{};
        Microsoft::WRL::ComPtr<ID3DBlob> cps_blob{};

        auto compile_classify = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
            Microsoft::WRL::ComPtr<ID3DBlob> err{};
            const auto hr = D3DCompile(g_flat3d_hudclass_hlsl, strlen(g_flat3d_hudclass_hlsl), "flat3d_hudclass",
                                       nullptr, nullptr, entry, target, 0, 0, &out, &err);
            if (FAILED(hr)) {
                spdlog::error("[Flat3D] HUD classify shader compile failed ({}): {}", entry,
                              err != nullptr ? (const char*)err->GetBufferPointer() : "unknown error");
                return false;
            }
            return true;
        };

        if (!compile_classify("vs_main", "vs_5_0", cvs_blob) || !compile_classify("ps_main", "ps_5_0", cps_blob)) {
            return false;
        }

        if (FAILED(device->CreateVertexShader(cvs_blob->GetBufferPointer(), cvs_blob->GetBufferSize(), nullptr, &m_classify_vs)) ||
            FAILED(device->CreatePixelShader(cps_blob->GetBufferPointer(), cps_blob->GetBufferSize(), nullptr, &m_classify_ps))) {
            spdlog::error("[Flat3D] Failed to create HUD classify shaders");
            return false;
        }

        D3D11_BUFFER_DESC ccb_desc{};
        ccb_desc.ByteWidth = sizeof(HudClassifyConstants);
        ccb_desc.Usage = D3D11_USAGE_DEFAULT;
        ccb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

        if (FAILED(device->CreateBuffer(&ccb_desc, nullptr, &m_classify_cb))) {
            spdlog::error("[Flat3D] Failed to create HUD classify constant buffer");
            return false;
        }
    }

    // Per-tile HUD depth pre-pass (mode 1). Non-fatal: leave the PS null on
    // failure so the pass is simply skipped (like the classify pass gates on
    // m_classify_ps).
    {
        Microsoft::WRL::ComPtr<ID3DBlob> dvs_blob{};
        Microsoft::WRL::ComPtr<ID3DBlob> dps_blob{};

        auto compile_huddepth = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
            Microsoft::WRL::ComPtr<ID3DBlob> err{};
            const auto hr = D3DCompile(g_flat3d_huddepth_hlsl, strlen(g_flat3d_huddepth_hlsl), "flat3d_huddepth",
                                       nullptr, nullptr, entry, target, 0, 0, &out, &err);
            if (FAILED(hr)) {
                spdlog::error("[Flat3D] HUD depth shader compile failed ({}): {}", entry,
                              err != nullptr ? (const char*)err->GetBufferPointer() : "unknown error");
                return false;
            }
            return true;
        };

        if (compile_huddepth("vs_main", "vs_5_0", dvs_blob) && compile_huddepth("ps_main", "ps_5_0", dps_blob)) {
            if (SUCCEEDED(device->CreateVertexShader(dvs_blob->GetBufferPointer(), dvs_blob->GetBufferSize(), nullptr, &m_huddepth_vs)) &&
                SUCCEEDED(device->CreatePixelShader(dps_blob->GetBufferPointer(), dps_blob->GetBufferSize(), nullptr, &m_huddepth_ps))) {
                D3D11_BUFFER_DESC dcb_desc{};
                dcb_desc.ByteWidth = sizeof(HudDepthConstants);
                dcb_desc.Usage = D3D11_USAGE_DEFAULT;
                dcb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

                if (FAILED(device->CreateBuffer(&dcb_desc, nullptr, &m_huddepth_cb))) {
                    spdlog::error("[Flat3D] Failed to create HUD depth constant buffer");
                    m_huddepth_vs.Reset();
                    m_huddepth_ps.Reset();
                }
            } else {
                spdlog::error("[Flat3D] Failed to create HUD depth shaders");
                m_huddepth_vs.Reset();
                m_huddepth_ps.Reset();
            }
        }
    }

    // Premultiplied alpha (matches the UI target's blend model / SpriteBatch).
    D3D11_BLEND_DESC oblend_desc{};
    oblend_desc.RenderTarget[0].BlendEnable = TRUE;
    oblend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    oblend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    oblend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    oblend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    oblend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    oblend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    oblend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (FAILED(device->CreateBlendState(&oblend_desc, &m_overlay_blend))) {
        spdlog::error("[Flat3D] Failed to create overlay blend state");
        return false;
    }

    // Full-screen-GUI coverage reduction (optional — on failure the coverage
    // signal is simply absent; cursor + pause detection remain).
    {
        Microsoft::WRL::ComPtr<ID3DBlob> vvs_blob{};
        Microsoft::WRL::ComPtr<ID3DBlob> vps_blob{};

        auto compile_coverage = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
            Microsoft::WRL::ComPtr<ID3DBlob> err{};
            const auto hr = D3DCompile(g_flat3d_coverage_hlsl, strlen(g_flat3d_coverage_hlsl), "flat3d_coverage",
                                       nullptr, nullptr, entry, target, 0, 0, &out, &err);
            if (FAILED(hr)) {
                spdlog::error("[Flat3D] Coverage shader compile failed ({}): {}", entry,
                              err != nullptr ? (const char*)err->GetBufferPointer() : "unknown error");
                return false;
            }
            return true;
        };

        if (compile_coverage("vs_main", "vs_5_0", vvs_blob) && compile_coverage("ps_main", "ps_5_0", vps_blob)) {
            device->CreateVertexShader(vvs_blob->GetBufferPointer(), vvs_blob->GetBufferSize(), nullptr, &m_coverage_vs);
            device->CreatePixelShader(vps_blob->GetBufferPointer(), vps_blob->GetBufferSize(), nullptr, &m_coverage_ps);

            D3D11_BUFFER_DESC covcb_desc{};
            covcb_desc.ByteWidth = 16; // one float, 16-byte aligned
            covcb_desc.Usage = D3D11_USAGE_DEFAULT;
            covcb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            device->CreateBuffer(&covcb_desc, nullptr, &m_coverage_cb);
        }
    }

    return true;
}

bool Flat3DCompositorD3D11::composite(ID3D11DeviceContext* context,
                                      ID3D11Texture2D* double_wide,
                                      ID3D11Texture2D* right_eye_src,
                                      ID3D11ShaderResourceView* ui_srv,
                                      ID3D11Texture2D* menu_tex,
                                      ID3D11Texture2D* scene_depth,
                                      ID3D11RenderTargetView* backbuffer_rtv,
                                      uint32_t out_w, uint32_t out_h,
                                      const Flat3DFrameParams& params,
                                      float* out_ui_coverage) {
    if (out_ui_coverage != nullptr) {
        *out_ui_coverage = m_coverage_ema;
    }

    if (!m_ready || context == nullptr || double_wide == nullptr || backbuffer_rtv == nullptr) {
        return false;
    }

    // Depth-adaptive HUD / geometry cursor: cached SRV over SceneDepthZ
    // (recreated when the pooled texture changes).
    if ((params.hud_depth_mode == 1 || params.cursor_depth_mode == 1) && scene_depth != nullptr) {
        if (m_hud_depth_src != scene_depth || m_hud_depth_srv == nullptr) {
            m_hud_depth_srv.Reset();
            m_hud_depth_src = nullptr;

            D3D11_TEXTURE2D_DESC sd_desc{};
            scene_depth->GetDesc(&sd_desc);

            DXGI_FORMAT srv_fmt = DXGI_FORMAT_UNKNOWN;
            switch (sd_desc.Format) {
            case DXGI_FORMAT_R32G8X24_TYPELESS:
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                srv_fmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
                break;
            case DXGI_FORMAT_R24G8_TYPELESS:
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
                srv_fmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                break;
            case DXGI_FORMAT_R32_TYPELESS:
            case DXGI_FORMAT_R32_FLOAT:
            case DXGI_FORMAT_D32_FLOAT:
                srv_fmt = DXGI_FORMAT_R32_FLOAT;
                break;
            default:
                break;
            }

            if (srv_fmt != DXGI_FORMAT_UNKNOWN && (sd_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0) {
                ComPtr<ID3D11Device> device{};
                scene_depth->GetDevice(&device);

                D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
                srv_desc.Format = srv_fmt;
                srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srv_desc.Texture2D.MipLevels = 1;

                if (device != nullptr && SUCCEEDED(device->CreateShaderResourceView(scene_depth, &srv_desc, &m_hud_depth_srv))) {
                    m_hud_depth_src = scene_depth;
                    m_hud_depth_uscale = sd_desc.Width >= m_eye_w * 2 ? 0.5f : 1.0f;
                }
            }
        }
    }

    // HUD anchor constants (b1) — refreshed every frame (empty when off).
    if (m_anchor_cb != nullptr) {
        HudAnchorConstants ac{};
        ac.count = (int32_t)params.anchor_count;
        ac.radius_uv = params.hud_marker_radius;
        ac.feather_uv = params.hud_marker_radius * 0.35f;

        for (uint32_t i = 0; i < params.anchor_count && i < Flat3DFrameParams::kMaxHudAnchors; ++i) {
            ac.anchors[i][0] = params.anchors[i][0];
            ac.anchors[i][1] = params.anchors[i][1];
            ac.anchors[i][2] = params.anchors[i][2];
        }

        context->UpdateSubresource(m_anchor_cb.Get(), 0, nullptr, &ac, 0, 0);
    }

    // SRV over the Framework's IMGUI RT (UEVR menu), cached until the
    // texture is recreated. It holds last frame's menu — the Framework draws
    // this frame's AFTER the composite (same latency the HMD overlay has).
    ID3D11ShaderResourceView* menu_srv = nullptr;

    if (menu_tex != nullptr) {
        if (m_menu_src != menu_tex || m_menu_srv == nullptr) {
            m_menu_srv.Reset();
            m_menu_src = nullptr;

            ComPtr<ID3D11Device> device{};
            menu_tex->GetDevice(&device);

            if (device != nullptr && SUCCEEDED(device->CreateShaderResourceView(menu_tex, nullptr, &m_menu_srv))) {
                m_menu_src = menu_tex;
            }
        }

        menu_srv = m_menu_srv.Get();
    }

    // --- 1. Refresh the per-eye cache from the double-wide ------------------
    // Full-stereo frame: both halves. AFR frame: only the freshly-rendered
    // eye's half — the other eye keeps last frame's image (same as HMD AFR).
    const auto copy_half = [&](int eye) {
        D3D11_BOX box{};
        box.left = (eye == 0) ? 0 : m_eye_w;
        box.right = (eye == 0) ? m_eye_w : m_eye_w * 2;
        box.top = 0;
        box.bottom = m_eye_h;
        box.front = 0;
        box.back = 1;

        // AFR / synced sequential: the engine renders ONE view per frame and
        // it always lands in the LEFT half of the double-wide (same source
        // box the HMD AFR submit paths use) — regardless of which eye it is.
        if (params.afr_frame) {
            box.left = 0;
            box.right = m_eye_w;
        }

        // Native-stereo-fix titles: only the LEFT half of the double-wide is
        // written by the engine — the RIGHT eye is rendered into a dedicated
        // scene-capture target (same consumption as the HMD submit path,
        // D3D11Component.cpp's copy into the right half at submit time).
        if (params.native_stereo_layout) {
            box.left = 0;
            box.right = m_eye_w;

            if (eye == 1 && right_eye_src != nullptr) {
                D3D11_TEXTURE2D_DESC sc_desc{};
                right_eye_src->GetDesc(&sc_desc);
                box.right = std::min<UINT>(m_eye_w, sc_desc.Width);
                box.bottom = std::min<UINT>(m_eye_h, sc_desc.Height);
                context->CopySubresourceRegion(m_eye_tex[1].Get(), 0, 0, 0, 0, right_eye_src, 0, &box);
                return;
            }
            // No scene capture (yet): fall through and duplicate the left half
            // so the output is at least stable mono.
        }

        context->CopySubresourceRegion(m_eye_tex[eye].Get(), 0, 0, 0, 0, double_wide, 0, &box);
    };

    // Records which eyes actually got new scene content this present —
    // overlays bake only into those (see draw_overlays).
    uint32_t eye_refresh_mask = 0b11;

    if (params.afr_frame) {
        const int fresh = params.afr_left_eye ? 0 : 1;
        // Synced Sequential pair lock: publish only complete pairs. On the
        // pair's FIRST present, stash the fresh eye and keep showing the
        // previous complete pair; on the second (same engine frame — the forced
        // same-state draw) publish both halves together. No fresh@T + stale@T-1
        // mismatch ever reaches the screen (judder + animated-HUD shimmer; an
        // HMD runtime would hide it via reprojection, a monitor shows it raw).
        // Anti-freeze: if a stash is already held, always publish — a missed
        // pair-second signal degrades to plain AFR instead of freezing.
        if (!params.afr_synced_pair) {
            m_pair_pending_valid = false; // left synced mode: a stale stash must never publish
        }
        const bool publish = !params.afr_synced_pair || params.afr_pair_second || m_pair_pending_valid;

        if (!publish) {
            if (m_pair_pending == nullptr && m_eye_tex[0] != nullptr) {
                ComPtr<ID3D11Device> pair_device{};
                context->GetDevice(&pair_device);
                D3D11_TEXTURE2D_DESC pd{};
                m_eye_tex[0]->GetDesc(&pd);
                pd.BindFlags = 0; // copy staging between the halves only
                pd.MiscFlags = 0;
                if (pair_device == nullptr || FAILED(pair_device->CreateTexture2D(&pd, nullptr, &m_pair_pending))) {
                    m_pair_pending.Reset();
                }
            }

            if (m_pair_pending != nullptr) {
                // AFR sources always render into the LEFT half (see copy_half).
                D3D11_BOX box{};
                box.right = m_eye_w;
                box.bottom = m_eye_h;
                box.back = 1;
                context->CopySubresourceRegion(m_pair_pending.Get(), 0, 0, 0, 0, double_wide, 0, &box);
                m_pair_pending_valid = true;
                m_pair_pending_eye = fresh;
                eye_refresh_mask = 0; // held pair: both eyes keep their composited image
            } else {
                copy_half(fresh); // allocation failed: fall back to plain AFR
                eye_refresh_mask = 1u << fresh;
            }
        } else {
            // Publish the stashed first half into ITS eye slot (recorded at
            // stash time — the pair boundary is not a stable parity, so never
            // assume the stash is simply the complement of the current eye).
            if (m_pair_pending_valid && m_pair_pending_eye != fresh) {
                context->CopyResource(m_eye_tex[m_pair_pending_eye].Get(), m_pair_pending.Get());
                eye_refresh_mask = 0b11; // both halves republished together
            } else {
                eye_refresh_mask = 1u << fresh;
            }
            m_pair_pending_valid = false;
            copy_half(fresh);
        }
    } else {
        m_pair_pending_valid = false; // native frame: any held stash is stale
        copy_half(0);
        copy_half(1);
    }

    ScopedD3D11State state_backup{context};

    // --- HUD world/static classification (mode 1) ----------------------------
    bool classification_ok = false;

    if (params.hud_depth_mode == 1 && m_hud_depth_srv != nullptr && ui_srv != nullptr && m_classify_ps != nullptr) {
        ComPtr<ID3D11Device> device{};
        context->GetDevice(&device);

        if (m_hud_mask_tex[0] == nullptr && device != nullptr) {
            D3D11_TEXTURE2D_DESC mask_desc{};
            mask_desc.Width = kHudMaskW;
            mask_desc.Height = kHudMaskH;
            mask_desc.MipLevels = 1;
            mask_desc.ArraySize = 1;
            mask_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // r=class, g=anim, b=occupancy
            mask_desc.SampleDesc.Count = 1;
            mask_desc.Usage = D3D11_USAGE_DEFAULT;
            mask_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            bool ok = true;
            for (int i = 0; i < 2 && ok; ++i) {
                ok = SUCCEEDED(device->CreateTexture2D(&mask_desc, nullptr, &m_hud_mask_tex[i])) &&
                     SUCCEEDED(device->CreateRenderTargetView(m_hud_mask_tex[i].Get(), nullptr, &m_hud_mask_rtv[i])) &&
                     SUCCEEDED(device->CreateShaderResourceView(m_hud_mask_tex[i].Get(), nullptr, &m_hud_mask_srv[i]));
            }

            if (ok) {
                const float zero_clear[4]{};
                context->ClearRenderTargetView(m_hud_mask_rtv[0].Get(), zero_clear);
                context->ClearRenderTargetView(m_hud_mask_rtv[1].Get(), zero_clear);
            } else {
                for (int i = 0; i < 2; ++i) {
                    m_hud_mask_tex[i].Reset();
                    m_hud_mask_rtv[i].Reset();
                    m_hud_mask_srv[i].Reset();
                }
            }
        }

        ComPtr<ID3D11Resource> ui_res{};
        ui_srv->GetResource(&ui_res);
        ComPtr<ID3D11Texture2D> ui_tex2d{};

        if (ui_res != nullptr) {
            ui_res.As(&ui_tex2d);
        }

        if (m_hud_mask_tex[0] != nullptr && ui_tex2d != nullptr && device != nullptr) {
            D3D11_TEXTURE2D_DESC ui_desc{};
            ui_tex2d->GetDesc(&ui_desc);

            if (m_hud_prev_ui == nullptr || m_hud_prev_w != ui_desc.Width || m_hud_prev_h != ui_desc.Height ||
                m_hud_prev_fmt != ui_desc.Format) {
                m_hud_prev_ui.Reset();
                m_hud_prev_srv.Reset();
                m_hud_prev_valid = false;

                auto prev_desc = ui_desc;
                prev_desc.Usage = D3D11_USAGE_DEFAULT;
                prev_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                prev_desc.CPUAccessFlags = 0;
                prev_desc.MiscFlags = 0;

                // SRV needs a typed view over typeless UI formats.
                D3D11_SHADER_RESOURCE_VIEW_DESC prev_srv_desc{};
                prev_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                prev_srv_desc.Texture2D.MipLevels = 1;

                switch (ui_desc.Format) {
                case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                    prev_srv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    break;
                case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                    prev_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    break;
                default:
                    prev_srv_desc.Format = ui_desc.Format;
                    break;
                }

                if (SUCCEEDED(device->CreateTexture2D(&prev_desc, nullptr, &m_hud_prev_ui)) &&
                    SUCCEEDED(device->CreateShaderResourceView(m_hud_prev_ui.Get(), &prev_srv_desc, &m_hud_prev_srv))) {
                    m_hud_prev_w = ui_desc.Width;
                    m_hud_prev_h = ui_desc.Height;
                    m_hud_prev_fmt = ui_desc.Format;
                } else {
                    m_hud_prev_ui.Reset();
                    m_hud_prev_srv.Reset();
                }
            }

            if (m_hud_prev_ui != nullptr) {
                const int old_idx = m_mask_idx;
                const int new_idx = 1 - m_mask_idx;

                HudClassifyConstants cc{};
                cc.flow_uv[0] = params.hud_flow_du;
                cc.flow_uv[1] = params.hud_flow_dv;
                cc.blend_alpha = 0.15f;
                cc.flow_valid = (params.hud_flow_valid && m_hud_prev_valid) ? 1 : 0;
                cc.translating = (params.hud_translating && m_hud_prev_valid) ? 1 : 0;
                cc.inv_mask_size[0] = 1.0f / (float)kHudMaskW;
                cc.inv_mask_size[1] = 1.0f / (float)kHudMaskH;
                cc.trans_d0_gate = params.hud_trans_gate;
                cc.rot_move_gate = params.hud_rot_gate;
                cc.occ_gate = params.hud_occ_gate;
                cc.halo_tiles = (float)params.hud_halo_tiles;
                cc.occ_safe_hw = params.hud_occ_safe_hw;
                cc.occ_safe_hh = params.hud_occ_safe_hh;
                cc.fill_radius = params.hud_fill_radius;
                cc.fill_gate = params.hud_fill_gate;
                cc.excl_count = params.hud_excl_count;
                for (int e = 0; e < 4; ++e) {
                    cc.excl[e][0] = params.hud_excl[e][0];
                    cc.excl[e][1] = params.hud_excl[e][1];
                    cc.excl[e][2] = params.hud_excl[e][2];
                    cc.excl[e][3] = params.hud_excl[e][3];
                }
                cc.ui_invert_alpha = params.ui_invert_alpha;
                cc.ui_color_gate = params.ui_color_gate;
                context->UpdateSubresource(m_classify_cb.Get(), 0, nullptr, &cc, 0, 0);

                context->IASetInputLayout(nullptr);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(m_classify_vs.Get(), nullptr, 0);
                context->PSSetShader(m_classify_ps.Get(), nullptr, 0);
                context->GSSetShader(nullptr, nullptr, 0);
                context->HSSetShader(nullptr, nullptr, 0);
                context->DSSetShader(nullptr, nullptr, 0);
                context->RSSetState(m_rasterizer.Get());
                context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                context->OMSetDepthStencilState(m_depth.Get(), 0);

                ID3D11Buffer* classify_cbs[] = {m_classify_cb.Get()};
                context->PSSetConstantBuffers(0, 1, classify_cbs);

                ID3D11ShaderResourceView* classify_srvs[] = {ui_srv, m_hud_prev_srv.Get(), m_hud_mask_srv[old_idx].Get()};
                context->PSSetShaderResources(0, 3, classify_srvs);

                ID3D11SamplerState* classify_samplers[] = {m_sampler.Get()};
                context->PSSetSamplers(0, 1, classify_samplers);

                D3D11_VIEWPORT vp{};
                vp.Width = (float)kHudMaskW;
                vp.Height = (float)kHudMaskH;
                vp.MaxDepth = 1.0f;
                context->RSSetViewports(1, &vp);

                D3D11_RECT sc{0, 0, (LONG)kHudMaskW, (LONG)kHudMaskH};
                context->RSSetScissorRects(1, &sc);

                ID3D11RenderTargetView* mask_rtvs[] = {m_hud_mask_rtv[new_idx].Get()};
                context->OMSetRenderTargets(1, mask_rtvs, nullptr);
                context->Draw(3, 0);

                ID3D11RenderTargetView* null_rtv[] = {nullptr};
                context->OMSetRenderTargets(1, null_rtv, nullptr);

                ID3D11ShaderResourceView* null_srvs[3] = {};
                context->PSSetShaderResources(0, 3, null_srvs);

                // Snapshot cur UI as next frame's reference (BEFORE the
                // post-composite UI clear the component performs).
                context->CopyResource(m_hud_prev_ui.Get(), ui_tex2d.Get());

                m_hud_prev_valid = true;
                m_mask_idx = new_idx;
                classification_ok = true;
            }
        }
    }

    // --- HUD per-tile depth pre-pass (mode 1) --------------------------------
    // Resolve one nearest-surface depth per world tile, then min-z-flood it
    // across contiguous world tiles into a 64x36 R32F ping-pong so a whole icon
    // shifts at ONE depth. Sampled by the overlay shader at t4. Only runs when
    // the classify pass produced a fresh mask this frame (m_hud_mask_srv[m_mask_idx]).
    if (classification_ok && params.hud_depth_mode == 1 && m_hud_depth_srv != nullptr &&
        m_huddepth_ps != nullptr) {
        ComPtr<ID3D11Device> device{};
        context->GetDevice(&device);

        if (m_huddepth_tex[0] == nullptr && device != nullptr) {
            D3D11_TEXTURE2D_DESC dep_desc{};
            dep_desc.Width = kHudMaskW;
            dep_desc.Height = kHudMaskH;
            dep_desc.MipLevels = 1;
            dep_desc.ArraySize = 1;
            dep_desc.Format = DXGI_FORMAT_R32_FLOAT;
            dep_desc.SampleDesc.Count = 1;
            dep_desc.Usage = D3D11_USAGE_DEFAULT;
            dep_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            bool ok = true;
            for (int i = 0; i < 2 && ok; ++i) {
                ok = SUCCEEDED(device->CreateTexture2D(&dep_desc, nullptr, &m_huddepth_tex[i])) &&
                     SUCCEEDED(device->CreateRenderTargetView(m_huddepth_tex[i].Get(), nullptr, &m_huddepth_rtv[i])) &&
                     SUCCEEDED(device->CreateShaderResourceView(m_huddepth_tex[i].Get(), nullptr, &m_huddepth_srv[i]));
            }

            if (ok) {
                const float zero_clear[4]{};
                context->ClearRenderTargetView(m_huddepth_rtv[0].Get(), zero_clear);
                context->ClearRenderTargetView(m_huddepth_rtv[1].Get(), zero_clear);
            } else {
                for (int i = 0; i < 2; ++i) {
                    m_huddepth_tex[i].Reset();
                    m_huddepth_rtv[i].Reset();
                    m_huddepth_srv[i].Reset();
                }
            }
        }

        if (m_huddepth_tex[0] != nullptr) {
            HudDepthConstants dc{};
            dc.mask_thr = 0.2f;
            dc.inv_mask_size[0] = 1.0f / (float)kHudMaskW;
            dc.inv_mask_size[1] = 1.0f / (float)kHudMaskH;
            dc.hud_depth_uscale = m_hud_depth_uscale;
            dc.hud_nearz_uu = params.hud_nearz_uu;
            dc.aspect_xy = m_eye_w > 0 ? (float)m_eye_h / (float)m_eye_w : 0.5625f;
            dc.pad_ = 0.0f;

            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(m_huddepth_vs.Get(), nullptr, 0);
            context->PSSetShader(m_huddepth_ps.Get(), nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            context->RSSetState(m_rasterizer.Get());
            context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            context->OMSetDepthStencilState(m_depth.Get(), 0);

            ID3D11Buffer* depth_cbs[] = {m_huddepth_cb.Get()};
            context->PSSetConstantBuffers(0, 1, depth_cbs);

            ID3D11SamplerState* depth_samplers[] = {m_sampler.Get()};
            context->PSSetSamplers(0, 1, depth_samplers);

            D3D11_VIEWPORT vp{};
            vp.Width = (float)kHudMaskW;
            vp.Height = (float)kHudMaskH;
            vp.MaxDepth = 1.0f;
            context->RSSetViewports(1, &vp);

            D3D11_RECT sc{0, 0, (LONG)kHudMaskW, (LONG)kHudMaskH};
            context->RSSetScissorRects(1, &sc);

            ID3D11RenderTargetView* null_rtv[] = {nullptr};
            ID3D11ShaderResourceView* null_srvs[3] = {};

            // Iteration 0 (resolve): mask (t0) + scene depth (t1), no prev depth.
            {
                dc.pass_idx = 0;
                context->UpdateSubresource(m_huddepth_cb.Get(), 0, nullptr, &dc, 0, 0);

                ID3D11ShaderResourceView* dsrvs[] = {m_hud_mask_srv[m_mask_idx].Get(), m_hud_depth_srv.Get(), nullptr};
                context->PSSetShaderResources(0, 3, dsrvs);

                ID3D11RenderTargetView* rtvs[] = {m_huddepth_rtv[0].Get()};
                context->OMSetRenderTargets(1, rtvs, nullptr);
                context->Draw(3, 0);

                context->OMSetRenderTargets(1, null_rtv, nullptr);
                context->PSSetShaderResources(0, 3, null_srvs);
            }

            int last = 0;

            // Iterations 1..N (diffuse min-z flood): ping-pong prev depth as t2.
            for (uint32_t i = 1; i <= kHudDepthDiffusePasses; ++i) {
                const int src_idx = last;
                const int dst_idx = 1 - last;

                dc.pass_idx = (int32_t)i;
                context->UpdateSubresource(m_huddepth_cb.Get(), 0, nullptr, &dc, 0, 0);

                ID3D11ShaderResourceView* dsrvs[] = {m_hud_mask_srv[m_mask_idx].Get(), m_hud_depth_srv.Get(),
                                                     m_huddepth_srv[src_idx].Get()};
                context->PSSetShaderResources(0, 3, dsrvs);

                ID3D11RenderTargetView* rtvs[] = {m_huddepth_rtv[dst_idx].Get()};
                context->OMSetRenderTargets(1, rtvs, nullptr);
                context->Draw(3, 0);

                context->OMSetRenderTargets(1, null_rtv, nullptr);
                context->PSSetShaderResources(0, 3, null_srvs);

                last = dst_idx;
            }

            // Last-written index is sampled by the overlay shader at t4.
            m_huddepth_idx = last;
        }
    }

    m_hud_mode_effective = params.hud_depth_mode;
    if (m_hud_mode_effective == 1 && (m_hud_depth_srv == nullptr || !classification_ok)) {
        m_hud_mode_effective = 0;
    }

    // --- 2. Draw the UI layer + crosshair + UEVR menu into each eye ----------
    // 3D-screenshot capture: record which eyes are refreshed this present
    // (draw_overlays below hides the UEVR menu while m_ss_active).
    if (m_ss_active) {
        m_ss_captured_mask |= eye_refresh_mask;
        ++m_ss_frames;
    }

    draw_overlays(context, ui_srv, menu_srv, params, eye_refresh_mask);

    // --- 2b. Full-screen-GUI coverage reduction (independent of HUD mode) ----
    // Average the UI's alpha coverage to the 1x1 target and read back the
    // oldest staging slot without stalling. Runs before the LeiaSR early-out so
    // it works in every output mode. Also unbinds the eye RTVs draw_overlays
    // left set, which the repack below needs unbound to sample them.
    if (m_coverage_ps != nullptr && m_coverage_rtv != nullptr && m_coverage_staging[0] != nullptr &&
        ui_srv != nullptr) {
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(m_coverage_vs.Get(), nullptr, 0);
        context->PSSetShader(m_coverage_ps.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);

        ID3D11ShaderResourceView* cov_srv[] = {ui_srv};
        context->PSSetShaderResources(0, 1, cov_srv);
        ID3D11SamplerState* cov_samp[] = {m_sampler.Get()};
        context->PSSetSamplers(0, 1, cov_samp);

        if (m_coverage_cb != nullptr) {
            const float cov_cb[4]{params.ui_invert_alpha, params.ui_color_gate, 0.0f, 0.0f};
            context->UpdateSubresource(m_coverage_cb.Get(), 0, nullptr, cov_cb, 0, 0);
            ID3D11Buffer* cov_cbs[] = {m_coverage_cb.Get()};
            context->PSSetConstantBuffers(0, 1, cov_cbs);
        }

        context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(m_depth.Get(), 0);
        context->RSSetState(m_rasterizer.Get());

        D3D11_VIEWPORT cov_vp{};
        cov_vp.Width = 1.0f;
        cov_vp.Height = 1.0f;
        cov_vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &cov_vp);
        D3D11_RECT cov_sc{0, 0, 1, 1};
        context->RSSetScissorRects(1, &cov_sc);

        ID3D11RenderTargetView* cov_rtvs[] = {m_coverage_rtv.Get()};
        context->OMSetRenderTargets(1, cov_rtvs, nullptr);
        context->Draw(3, 0);

        ID3D11RenderTargetView* null_rtv[] = {nullptr};
        context->OMSetRenderTargets(1, null_rtv, nullptr);

        const auto write_slot = m_coverage_frame % kDepthRing;
        context->CopyResource(m_coverage_staging[write_slot].Get(), m_coverage_rt.Get());
        ++m_coverage_frame;

        // Read the oldest slot (had kDepthRing-1 frames to complete) without
        // waiting; skip on WAS_STILL_DRAWING.
        if (m_coverage_frame >= kDepthRing) {
            const auto read_slot = m_coverage_frame % kDepthRing;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(m_coverage_staging[read_slot].Get(), 0, D3D11_MAP_READ,
                                       D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)) && mapped.pData != nullptr) {
                const float cov = *(const float*)mapped.pData;
                context->Unmap(m_coverage_staging[read_slot].Get(), 0);
                if (cov >= 0.0f && cov <= 1.0f) {
                    m_coverage_ema += (cov - m_coverage_ema) * 0.25f;
                }
            }
        }

        if (out_ui_coverage != nullptr) {
            *out_ui_coverage = m_coverage_ema;
        }
    }

    // --- 3a. LeiaSR: weave the SbS pair straight into the backbuffer --------
    if (params.mode == (int32_t)Flat3DOutputMode::LEIA_SR && leiasr_available()) {
        return weave_leiasr(context, backbuffer_rtv, out_w, out_h, params);
    }

    // --- 3b. Repack into the real backbuffer --------------------------------
    RepackConstants constants{};
    constants.out_size[0] = (int32_t)out_w;
    constants.out_size[1] = (int32_t)out_h;
    constants.mode = params.mode;
    constants.eye_swap = params.eye_swap ? 1 : 0;
    constants.colorspace = (int32_t)m_colorspace;
    constants.paper_white = params.paper_white_nits;
    constants.src_srgb = m_src_srgb ? 1 : 0;
    constants.correction_enabled = params.correction_enabled ? 1 : 0;
    for (int i = 0; i < 3; ++i) {
        constants.lift[i] = params.lift[i];
        constants.gamma[i] = params.gamma[i];
        constants.gain[i] = params.gain[i];
    }
    constants.curve = params.curve;
    constants.off_low = params.off_low;
    constants.off_high = params.off_high;
    constants.off_both = params.off_both;
    constants.scene_shift_uv = params.scene_shift_px / (float)m_eye_w;
    constants.scene_scale = params.scene_scale;
    context->UpdateSubresource(m_cb.Get(), 0, nullptr, &constants, 0, 0);

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* null_vb = nullptr;
    UINT zero = 0;
    context->IASetVertexBuffers(0, 1, &null_vb, &zero, &zero);
    context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);

    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);

    ID3D11Buffer* cbs[] = {m_cb.Get()};
    context->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[] = {m_eye_srv[0].Get(), m_eye_srv[1].Get()};
    context->PSSetShaderResources(0, 2, srvs);

    ID3D11SamplerState* samplers[] = {m_sampler.Get()};
    context->PSSetSamplers(0, 1, samplers);

    context->RSSetState(m_rasterizer.Get());

    D3D11_VIEWPORT viewport{};
    viewport.Width = (float)out_w;
    viewport.Height = (float)out_h;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);

    D3D11_RECT scissor{0, 0, (LONG)out_w, (LONG)out_h};
    context->RSSetScissorRects(1, &scissor);

    context->OMSetBlendState(m_blend.Get(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(m_depth.Get(), 0);

    ID3D11RenderTargetView* rtvs[] = {backbuffer_rtv};
    context->OMSetRenderTargets(1, rtvs, nullptr);

    context->Draw(3, 0);

    return true;
}

// Draws the game-UI layer (at GUI depth), the extracted crosshair region (at
// aim depth), and/or the procedural laser dot into BOTH eye textures with
// per-eye parallax. dir = +1 for the left eye, -1 for the right.
void Flat3DCompositorD3D11::draw_overlays(ID3D11DeviceContext* context, ID3D11ShaderResourceView* ui_srv,
                                          ID3D11ShaderResourceView* menu_srv, const Flat3DFrameParams& params,
                                          uint32_t eye_refresh_mask) {
    // A 3D-screenshot capture hides only the UEVR menu (layer 3) so it doesn't
    // land in the saved pair; the game HUD, crosshair and stereo cursor stay.
    // See begin_screenshot().
    const bool want_ui = params.ui_enabled && ui_srv != nullptr;
    const bool want_game_crosshair = params.crosshair_mode == 1 && ui_srv != nullptr;
    const bool want_laser = params.crosshair_mode == 2;
    const bool want_menu = !m_ss_active && menu_srv != nullptr;
    const bool want_cursor = params.cursor_enabled;

    // Effective mode computed in composite (depth SRV + classification state).
    const int32_t hud_mode = m_hud_mode_effective;

    if (!want_ui && !want_game_crosshair && !want_laser && !want_menu && !want_cursor) {
        return;
    }

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_overlay_vs.Get(), nullptr, 0);
    context->PSSetShader(m_overlay_ps.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->RSSetState(m_rasterizer.Get());
    context->OMSetBlendState(m_overlay_blend.Get(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(m_depth.Get(), 0);

    ID3D11Buffer* cbs[] = {m_overlay_cb.Get(), m_anchor_cb.Get()};
    context->PSSetConstantBuffers(0, 2, cbs);

    // t4 = per-tile HUD depth (mode 1). When the depth pre-pass didn't run this
    // frame m_huddepth_srv[m_huddepth_idx] is null, which the shader reads as
    // "flat" (safe). It's only sampled when hud_mode == 1 anyway.
    ID3D11ShaderResourceView* srvs[] = {ui_srv, menu_srv, m_hud_depth_srv.Get(), m_hud_mask_srv[m_mask_idx].Get(),
                                        m_huddepth_srv[m_huddepth_idx].Get()};
    context->PSSetShaderResources(0, 5, srvs);

    ID3D11SamplerState* samplers[] = {m_sampler.Get()};
    context->PSSetSamplers(0, 1, samplers);

    D3D11_VIEWPORT viewport{};
    viewport.Width = (float)m_eye_w;
    viewport.Height = (float)m_eye_h;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);

    D3D11_RECT scissor{0, 0, (LONG)m_eye_w, (LONG)m_eye_h};
    context->RSSetScissorRects(1, &scissor);

    // The overlay shader converts UI colors into the EYE texture's space.
    const int32_t eye_space = m_src_srgb || m_colorspace == Flat3DColorSpace::SDR
                            ? 0 : (int32_t)m_colorspace;

    const auto argb = params.crosshair_color_argb;
    const float laser_a = ((argb >> 24) & 0xFF) / 255.0f;
    const float laser_r = ((argb >> 16) & 0xFF) / 255.0f;
    const float laser_g = ((argb >> 8) & 0xFF) / 255.0f;
    const float laser_b = (argb & 0xFF) / 255.0f;

    for (int eye = 0; eye < 2; ++eye) {
        // Bake overlays ONLY into eyes whose scene content was refreshed this
        // present. The eye caches are persistent under AFR/pair-lock; a fresh
        // scene copy washes out the previous bake, but re-drawing the
        // translucent UI onto an eye that was NOT re-copied blends it on top of
        // its own previous bake — opacity ratchets between presents, which
        // reads as HUD flicker/shimmer (AHUD + Synced Sequential).
        if ((eye_refresh_mask & (1u << eye)) == 0) {
            continue;
        }

        const float dir = (eye == 0) ? 1.0f : -1.0f;

        ID3D11RenderTargetView* rtvs[] = {m_eye_rtv[eye].Get()};
        context->OMSetRenderTargets(1, rtvs, nullptr);

        const float ui_shift_uv = dir * params.ui_shift_px / (float)m_eye_w;
        const float ch_shift_uv = dir * params.crosshair_shift_px / (float)m_eye_w;
        const float menu_shift_uv = dir * params.menu_shift_px / (float)m_eye_w;

        // Geometry-depth cursor: only when selected AND scene depth is bound —
        // otherwise it falls back to the static GUI-depth placement below.
        const int32_t cursor_geo = (params.cursor_depth_mode == 1 && m_hud_depth_srv != nullptr) ? 1 : 0;

        auto draw_layer = [&](int32_t layer, float shift_uv, float scale, float center_x, float center_y) {
            OverlayConstants oc{};
            // output = (src - 0.5) * scale + 0.5 + shift  =>  src = out*(1/scale) + (0.5 - (0.5+shift)/scale)
            oc.uv_scale[0] = 1.0f / scale;
            // Vertical: only the symmetric-mode crop-map compensation — the
            // depth shift is horizontal, so a vertical fit-shrink would just
            // letterbox the overlay (black bars on fullscreen UIs).
            oc.uv_scale[1] = params.scene_scale;
            oc.uv_offset[0] = 0.5f - (0.5f + shift_uv) / scale;
            oc.uv_offset[1] = 0.5f * (1.0f - params.scene_scale);
            oc.color[0] = laser_r;
            oc.color[1] = laser_g;
            oc.color[2] = laser_b;
            oc.color[3] = laser_a;
            oc.layer = layer;
            oc.ui_invert_alpha = params.ui_invert_alpha; // shader applies it only to game-UI layers (0/1)
            oc.ui_color_gate = params.ui_color_gate;     // zero alpha on colourless pixels (invert-0.5 tint fix)
            oc.colorspace = eye_space;
            oc.paper_white = params.paper_white_nits;
            // Region math: the crosshair region rides at the CROSSHAIR shift.
            oc.region_radius_uv = (layer == 2 || layer == 4) ? 0.0f
                                : (want_game_crosshair ? params.crosshair_region_radius : 0.0f);
            oc.region_center[0] = center_x;
            oc.region_center[1] = center_y;
            oc.dot_radius_px = (layer == 4) ? params.cursor_size_px : params.crosshair_size_px;
            oc.eye_width_px = (float)m_eye_w;
            oc.eye_height_px = (float)m_eye_h;
            // Low 4 bits = mode; high bits = dilation radius in tiles.
            const int32_t base_mode = (layer == 0) ? ((hud_mode == 1 && params.hud_debug) ? 3 : hud_mode) : 0;
            const int32_t dilate_tiles = std::clamp((int32_t)std::lround(params.hud_icon_radius * 64.0f), 1, 8);
            oc.hud_mode = base_mode | (dilate_tiles << 4);
            // Vertical stem reach (depth-adaptive only): signed extra dilation
            // tiles (>0 down, <0 up), capped so the loop stays bounded.
            oc.hud_stem_reach = (layer == 0 && hud_mode == 1)
                ? std::clamp((int32_t)std::lround(params.hud_stem_reach * 64.0f), -16, 16)
                : 0;
            oc.hud_k_px = dir * params.hud_k_px / params.scene_scale;
            oc.hud_bias_px = dir * params.scene_shift_px / params.scene_scale;
            oc.hud_inv_conv_uu = params.hud_inv_conv_uu;
            oc.hud_nearz_uu = params.hud_nearz_uu;
            oc.hud_depth_uscale = m_hud_depth_uscale;
            oc.hud_flat_shift_uv = dir * params.ui_shift_px / (float)m_eye_w;
            oc.cursor_depth = (layer == 4) ? cursor_geo : 0;
            context->UpdateSubresource(m_overlay_cb.Get(), 0, nullptr, &oc, 0, 0);
            context->Draw(3, 0);
        };

        if (want_ui) {
            // Depth modes ride the same flat transform (fit-scale included);
            // the shader adds only the per-pixel DELTA from the flat shift.
            draw_layer(0, ui_shift_uv, params.ui_scale, 0.5f + ch_shift_uv, params.crosshair_region_center_y);
        }
        if (want_game_crosshair) {
            // Same UI texture, only the center region, at the aim-depth shift.
            // Scale matches the UI scale so the reticle isn't resized.
            draw_layer(1, ch_shift_uv, params.ui_scale, 0.5f + ch_shift_uv, params.crosshair_region_center_y);
        }
        if (want_laser) {
            draw_layer(2, ch_shift_uv, 1.0f, 0.5f + ch_shift_uv, 0.5f);
        }
        if (want_menu) {
            // The UEVR menu at its own depth.
            draw_layer(3, menu_shift_uv, params.menu_scale, 0.5f + ch_shift_uv, 0.5f);
        }
        if (want_cursor) {
            // Topmost: the stereo cursor, riding the UI layer's transform
            // (fit-scale horizontally, crop-map compensation vertically) so
            // it stays over the element it points at. Geometry mode passes the
            // MONO tip (no GUI parallax) — the shader adds the per-eye depth shift.
            const float cur_x = params.ui_scale * (params.cursor_uv[0] - 0.5f) + 0.5f
                              + (cursor_geo ? 0.0f : ui_shift_uv);
            const float cur_y = (params.cursor_uv[1] - 0.5f) / params.scene_scale + 0.5f;
            draw_layer(4, ui_shift_uv, 1.0f, cur_x, cur_y);
        }
    }

    // Unbind the eye RTV before the repack pass samples the eye textures,
    // and drop the depth/mask SRVs so they don't linger past the overlay pass.
    ID3D11RenderTargetView* null_rtv[] = {nullptr};
    context->OMSetRenderTargets(1, null_rtv, nullptr);

    ID3D11ShaderResourceView* null_srvs[5] = {};
    context->PSSetShaderResources(0, 5, null_srvs);
}

// Non-blocking SceneDepthZ readback: copies a few thin ROI stripes of the
// LEFT-eye half into a staging ring, maps the oldest entry with DO_NOT_WAIT,
// and derives the aim-point depth (median around center) and the nearest
// significant depth (~2nd percentile, rejects specks). Values in UE units.
void Flat3DCompositorD3D11::sample_depth(ID3D11DeviceContext* context, ID3D11Texture2D* scene_depth,
                                         float nearz_uu, float* out_center_uu, float* out_nearest_uu) {
    if (out_center_uu != nullptr) *out_center_uu = m_center_ema_uu;
    if (out_nearest_uu != nullptr) *out_nearest_uu = -1.0f;

    if (context == nullptr || scene_depth == nullptr || nearz_uu <= 0.0f) {
        return;
    }

    ComPtr<ID3D11Device> device{};
    context->GetDevice(&device);

    D3D11_TEXTURE2D_DESC sd_desc{};
    scene_depth->GetDesc(&sd_desc);

    // Supported depth layouts.
    const bool is_d32s8 = sd_desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
                          sd_desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    const bool is_r32 = is_d32s8 || sd_desc.Format == DXGI_FORMAT_R32_TYPELESS ||
                        sd_desc.Format == DXGI_FORMAT_R32_FLOAT || sd_desc.Format == DXGI_FORMAT_D32_FLOAT;
    const bool is_r24g8 = sd_desc.Format == DXGI_FORMAT_R24G8_TYPELESS || sd_desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;

    if (!is_r32 && !is_r24g8) {
        if (!m_depth_format_warned) {
            m_depth_format_warned = true;
            spdlog::warn("[Flat3D] SceneDepthZ format {} unsupported for depth sampling", (uint32_t)sd_desc.Format);
        }
        return;
    }

    // SceneDepthZ under stereo is double-wide; use the left-eye half. If it
    // isn't (single-wide), use the whole width.
    const uint32_t eye_w = sd_desc.Width >= m_eye_w * 2 ? sd_desc.Width / 2 : sd_desc.Width;
    const uint32_t eye_h = sd_desc.Height;

    // ROI: central 84% x 90%.
    const uint32_t roi_x0 = (uint32_t)(eye_w * 0.08f);
    const uint32_t roi_x1 = (uint32_t)(eye_w * 0.92f);
    const uint32_t roi_w = roi_x1 - roi_x0;
    const uint32_t roi_y0 = (uint32_t)(eye_h * 0.05f);
    const uint32_t roi_y1 = (uint32_t)(eye_h * 0.95f);

    if (roi_w == 0 || roi_y1 <= roi_y0) {
        return;
    }

    // (Re)create the staging ring.
    if (m_depth_staging[0] == nullptr || m_depth_roi_w != roi_w || m_depth_format != sd_desc.Format) {
        for (auto& tex : m_depth_staging) {
            tex.Reset();
        }

        D3D11_TEXTURE2D_DESC st_desc{};
        st_desc.Width = roi_w;
        st_desc.Height = kDepthStripes * kStripeRows;
        st_desc.MipLevels = 1;
        st_desc.ArraySize = 1;
        st_desc.Format = sd_desc.Format;
        st_desc.SampleDesc.Count = 1;
        st_desc.Usage = D3D11_USAGE_STAGING;
        st_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        for (auto& tex : m_depth_staging) {
            if (FAILED(device->CreateTexture2D(&st_desc, nullptr, &tex))) {
                spdlog::error("[Flat3D] Failed to create depth staging texture");
                for (auto& t : m_depth_staging) t.Reset();
                return;
            }
        }

        m_depth_roi_w = roi_w;
        m_depth_format = sd_desc.Format;
    }

    // Copy this frame's stripes into the current ring slot.
    const auto write_slot = m_depth_frame % kDepthRing;
    auto* dst = m_depth_staging[write_slot].Get();

    for (uint32_t s = 0; s < kDepthStripes; ++s) {
        const float frac = ((float)s + 0.5f) / (float)kDepthStripes; // evenly spread stripe centers
        uint32_t y = roi_y0 + (uint32_t)((roi_y1 - roi_y0 - kStripeRows) * frac);

        D3D11_BOX box{};
        box.left = roi_x0;
        box.right = roi_x1;
        box.top = y;
        box.bottom = y + kStripeRows;
        box.front = 0;
        box.back = 1;

        context->CopySubresourceRegion(dst, 0, 0, s * kStripeRows, 0, scene_depth, 0, &box);
    }

    ++m_depth_frame;

    // Map the OLDEST slot without waiting (it has had kDepthRing-1 frames to
    // complete). If the driver still isn't done, skip this frame.
    if (m_depth_frame < kDepthRing) {
        return;
    }

    const auto read_slot = m_depth_frame % kDepthRing; // slot about to be reused = oldest
    D3D11_MAPPED_SUBRESOURCE mapped{};

    if (FAILED(context->Map(m_depth_staging[read_slot].Get(), 0, D3D11_MAP_READ,
                            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
        return;
    }

    // Reversed-Z with infinite far: z_view = nearz / depth_sample.
    const auto device_to_z = [&](float d) {
        if (d <= 1e-9f) {
            return 1e9f; // far plane / sky
        }
        return std::min(nearz_uu / d, 1e9f);
    };

    // Samples essentially AT the near plane are not scene geometry — they are
    // camera-clipped polys or full-screen overlay/backdrop quads drawn at the
    // near plane. Feeding them into the nearest percentile permanently
    // saturates auto-convergence at its floor, so they are excluded from the
    // nearest estimate; the convergence floor already covers the genuine
    // camera-clip case.
    const float z_glue_uu = nearz_uu * 1.5f;

    std::vector<float> z_samples;
    z_samples.reserve((roi_w / 4 + 1) * kDepthStripes);
    std::vector<float> center_samples;       // tiny box, crosshair depth
    std::vector<float> center_region_samples; // mid-screen, auto-convergence bias

    uint32_t total = 0;
    uint32_t far_rejected = 0;
    uint32_t glued = 0;
    float d_min = 1.0f;
    float d_max = 0.0f;

    const uint32_t center_stripe = kDepthStripes / 2; // frac 0.5 stripe
    const uint32_t center_x = roi_w / 2;
    // Aim-window half-width. Wide enough to span the stereo parallax band: we
    // read the LEFT-eye half, but a near object under the FUSED reticle sits at
    // eye-center only at the convergence depth — nearer geometry has crossed
    // disparity and shifts sideways in the left eye, so a center-only sample
    // reads the background behind it (the reticle only "pops" onto it when the
    // player aims off-center by the parallax). Sampling out to ~6% of the ROI
    // and taking the nearest catches the object wherever its disparity puts it.
    const uint32_t kAimHalfW = std::max<uint32_t>(12u, roi_w / 16);
    // Central sub-region (mid ~40% of the ROI both axes): the aim target sits
    // here even though it is rarely the frame's global nearest object.
    const uint32_t cregion_x0 = (uint32_t)(roi_w * 0.30f);
    const uint32_t cregion_x1 = (uint32_t)(roi_w * 0.70f);

    for (uint32_t row = 0; row < kDepthStripes * kStripeRows; ++row) {
        const uint8_t* row_data = (const uint8_t*)mapped.pData + row * mapped.RowPitch;
        const uint32_t stripe = row / kStripeRows;
        const bool is_center_region_row = stripe >= 3 && stripe <= 5; // mid ~third vertically

        for (uint32_t x = 0; x < roi_w; x += 4) {
            // D32S8 staging texels are 8 bytes; the depth float is the first dword.
            const uint8_t* texel = row_data + (size_t)x * (is_d32s8 ? 8u : 4u);
            float d;
            if (is_r32) {
                d = *(const float*)texel;
            } else {
                const uint32_t v = *(const uint32_t*)texel;
                d = (float)(v & 0xFFFFFF) / 16777215.0f;
            }

            ++total;
            d_min = std::min(d_min, d);
            d_max = std::max(d_max, d);

            const float z = device_to_z(d);
            if (z >= 1e9f) {
                ++far_rejected;
            } else if (z < z_glue_uu) {
                ++glued;
            } else {
                z_samples.push_back(z);
                if (is_center_region_row && x >= cregion_x0 && x <= cregion_x1) {
                    center_region_samples.push_back(z);
                }
            }
        }
    }

    // Dense aim-point sweep: full-resolution (every texel) scan of the reticle's
    // rows over the parallax-band window (see kAimHalfW). Sampling every texel
    // guarantees a thin/small target is hit, and the wide span covers the
    // crossed-disparity offset of a near object in the left-eye half so the
    // reticle stops reading the background behind it. Kept scene depth only
    // (far-sentinel / near-plane "glued" texels excluded), same as the ROI loop.
    {
        const uint32_t ax0 = center_x > kAimHalfW ? center_x - kAimHalfW : 0u;
        const uint32_t ax1 = std::min(center_x + kAimHalfW, roi_w - 1);
        for (uint32_t r = 0; r < kStripeRows; ++r) {
            const uint32_t row = center_stripe * kStripeRows + r;
            const uint8_t* row_data = (const uint8_t*)mapped.pData + row * mapped.RowPitch;
            for (uint32_t x = ax0; x <= ax1; ++x) {
                const uint8_t* texel = row_data + (size_t)x * (is_d32s8 ? 8u : 4u);
                float d;
                if (is_r32) {
                    d = *(const float*)texel;
                } else {
                    const uint32_t v = *(const uint32_t*)texel;
                    d = (float)(v & 0xFFFFFF) / 16777215.0f;
                }
                const float z = device_to_z(d);
                if (z < 1e9f && z >= z_glue_uu) {
                    center_samples.push_back(z);
                }
            }
        }
    }

    context->Unmap(m_depth_staging[read_slot].Get(), 0);

    // Sample-data diagnostics: the raw picture behind the auto-convergence /
    // crosshair decisions. glued≈total means a full-screen near overlay (or a
    // wrong buffer); far≈total means a cleared/unrendered target.
    if (const auto now = std::chrono::steady_clock::now(); now - m_last_depth_stats > std::chrono::seconds(5)) {
        m_last_depth_stats = now;

        float p2 = -1.0f, p50 = -1.0f, p98 = -1.0f;
        if (!z_samples.empty()) {
            auto sorted = z_samples;
            const auto at = [&](float frac) {
                const size_t i = std::min(sorted.size() - 1, (size_t)((sorted.size() - 1) * frac));
                std::nth_element(sorted.begin(), sorted.begin() + i, sorted.end());
                return sorted[i];
            };
            p2 = at(0.02f);
            p50 = at(0.5f);
            p98 = at(0.98f);
        }

        spdlog::info("[Flat3D][depth-sample] nearz={:.3f}uu center(ema={:.1f})uu samples={} far={} "
                     "glued(z<{:.3f}uu)={} kept={} d=[{:.6f}..{:.6f}] z(p2/p50/p98)={:.1f}/{:.1f}/{:.1f}uu",
                     nearz_uu, m_center_ema_uu, total, far_rejected, z_glue_uu, glued, z_samples.size(),
                     d_min, d_max, p2, p50, p98);
    }

    // Aim/crosshair depth: the NEAREST surface in the center window; the whole
    // reticle region then renders at that single depth. A tiny near percentile
    // (~5th) instead of the raw minimum rejects a lone near speck but still
    // pins the crosshair to the closest thing under the aim point.
    if (!center_samples.empty()) {
        // 3rd-nearest of the dense aim window: rejects a lone 1-2px speck / edge
        // texel but lets a genuine small target win (a percentile that scales
        // with the sample count would need MORE coverage as density rises,
        // defeating the point — a small object covers only a few dense texels).
        const size_t ci = std::min<size_t>(2, center_samples.size() - 1);
        std::nth_element(center_samples.begin(), center_samples.begin() + ci, center_samples.end());
        const float center_z = center_samples[ci];

        if (m_center_ema_uu <= 0.0f) {
            m_center_ema_uu = center_z;
        } else if (std::fabs(center_z - m_center_ema_uu) / m_center_ema_uu > 0.01f) {
            m_center_ema_uu += (center_z - m_center_ema_uu) * 0.25f;
        }
    }

    // Nearest significant depth (auto-convergence input): ~2nd percentile of
    // the whole ROI so a few stray near pixels don't dominate.
    float nearest_raw = -1.0f;
    if (z_samples.size() >= 16) {
        const size_t k = std::max<size_t>(3, z_samples.size() / 50);
        std::nth_element(z_samples.begin(), z_samples.begin() + k, z_samples.end());
        nearest_raw = z_samples[k];
    }
    // Center-weighting: the aim target sits mid-screen but is rarely the
    // frame's global nearest object, so plain nearest never reacts to it. Take
    // a low percentile (~10th, robust vs near specks) of the central region and
    // let it win when it is closer — a centered/aimed enemy then drives the
    // pull-in without waiting for it to become the whole frame's nearest thing.
    if (center_region_samples.size() >= 16) {
        const size_t kc = std::max<size_t>(3, center_region_samples.size() / 10);
        std::nth_element(center_region_samples.begin(), center_region_samples.begin() + kc,
                         center_region_samples.end());
        const float center_near = center_region_samples[kc];
        if (nearest_raw <= 0.0f || center_near < nearest_raw) {
            nearest_raw = center_near;
        }
    }
    if (out_nearest_uu != nullptr && nearest_raw > 0.0f) {
        *out_nearest_uu = nearest_raw;
    }

    if (out_center_uu != nullptr) {
        *out_center_uu = m_center_ema_uu;
    }
}

bool Flat3DCompositorD3D11::leiasr_available() const {
#ifdef UEVR_FLAT3D_HAS_LEIASR
    // True while the weaver exists OR creation hasn't been attempted yet —
    // weave_leiasr() creates it lazily on the first call, so gating on the
    // weaver alone would mean it never gets created.
    return m_sr_weaver != nullptr || !m_sr_attempted;
#else
    return false;
#endif
}

#ifdef UEVR_FLAT3D_HAS_LEIASR
#ifndef _DPI_AWARENESS_CONTEXTS_
DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace {
// SEH-guarded SRContext::create — a missing/har-crashing SR runtime must not
// take the game down (VRto3D leiasr_presenter pattern).
SR::SRContext* try_create_sr_context() {
    __try {
        return SR::SRContext::create();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Per-monitor DPI awareness for the weaver's create + weave calls, so the SR
// runtime's window/monitor queries resolve to physical pixels and the weave
// maps to the full native panel instead of a DPI-virtualized sub-region under
// a non-per-monitor-aware host game. Process-wide awareness (DllMain) is the
// primary fix; this backs it up per-thread for when that couldn't take (e.g.
// injected after window creation). No-op when the API is unavailable or the
// thread is already aware.
struct ScopedPerMonitorDpi {
    using SetCtxFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    SetCtxFn set_ctx{nullptr};
    DPI_AWARENESS_CONTEXT prev{nullptr};

    ScopedPerMonitorDpi() {
        if (auto* user32 = GetModuleHandleW(L"user32.dll")) {
            set_ctx = (SetCtxFn)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
        }
        if (set_ctx != nullptr) {
            prev = set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
    ~ScopedPerMonitorDpi() {
        if (set_ctx != nullptr && prev != nullptr) {
            set_ctx(prev);
        }
    }
    ScopedPerMonitorDpi(const ScopedPerMonitorDpi&) = delete;
    ScopedPerMonitorDpi& operator=(const ScopedPerMonitorDpi&) = delete;
};
} // namespace
#endif

bool Flat3DCompositorD3D11::build_sbs(ID3D11DeviceContext* context, const Flat3DFrameParams& params,
                                      uint32_t out_w, uint32_t out_h) {
    if (m_sbs_tex == nullptr || m_sbs_w != out_w || m_sbs_h != out_h) {
        ComPtr<ID3D11Device> device{};
        context->GetDevice(&device);

        m_sbs_tex.Reset();
        m_sbs_srv.Reset();
        m_sbs_rtv.Reset();
        m_sbs_w = out_w;
        m_sbs_h = out_h;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = out_w * 2;
        desc.Height = out_h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = m_eye_format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_sbs_tex))) {
            spdlog::error("[Flat3D] Failed to create SbS texture");
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = m_eye_format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;

        if (FAILED(device->CreateShaderResourceView(m_sbs_tex.Get(), &srv_desc, &m_sbs_srv))) {
            spdlog::error("[Flat3D] Failed to create SbS SRV");
            m_sbs_tex.Reset();
            return false;
        }

        if (FAILED(device->CreateRenderTargetView(m_sbs_tex.Get(), nullptr, &m_sbs_rtv))) {
            spdlog::error("[Flat3D] Failed to create SbS RTV");
            m_sbs_tex.Reset();
            m_sbs_srv.Reset();
            return false;
        }

#ifdef UEVR_FLAT3D_HAS_LEIASR
        m_sr_input_bound = false; // the weaver must rebind the recreated input
#endif
    }

    // Render the pair with the repack shader in SBS mode instead of copying:
    // honors eye swap and applies the SDR color correction (the LeiaSR weaver
    // and the screenshot PNG both consume SDR sRGB).
    RepackConstants constants{};
    constants.out_size[0] = (int32_t)(m_sbs_w * 2);
    constants.out_size[1] = (int32_t)m_sbs_h;
    constants.mode = (int32_t)Flat3DOutputMode::SBS;
    constants.eye_swap = params.eye_swap ? 1 : 0;
    constants.colorspace = 0; // SDR sRGB
    constants.paper_white = params.paper_white_nits;
    constants.src_srgb = 0;
    constants.correction_enabled = params.correction_enabled ? 1 : 0;
    for (int i = 0; i < 3; ++i) {
        constants.lift[i] = params.lift[i];
        constants.gamma[i] = params.gamma[i];
        constants.gain[i] = params.gain[i];
    }
    constants.curve = params.curve;
    constants.off_low = params.off_low;
    constants.off_high = params.off_high;
    constants.off_both = params.off_both;
    constants.scene_shift_uv = params.scene_shift_px / (float)m_eye_w;
    constants.scene_scale = params.scene_scale;
    context->UpdateSubresource(m_cb.Get(), 0, nullptr, &constants, 0, 0);

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);

    ID3D11Buffer* cbs[] = {m_cb.Get()};
    context->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* eye_srvs[] = {m_eye_srv[0].Get(), m_eye_srv[1].Get()};
    context->PSSetShaderResources(0, 2, eye_srvs);

    ID3D11SamplerState* samplers[] = {m_sampler.Get()};
    context->PSSetSamplers(0, 1, samplers);

    context->RSSetState(m_rasterizer.Get());

    D3D11_VIEWPORT sbs_viewport{};
    sbs_viewport.Width = (float)(m_sbs_w * 2);
    sbs_viewport.Height = (float)m_sbs_h;
    sbs_viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &sbs_viewport);

    D3D11_RECT sbs_scissor{0, 0, (LONG)(m_sbs_w * 2), (LONG)m_sbs_h};
    context->RSSetScissorRects(1, &sbs_scissor);

    context->OMSetBlendState(m_blend.Get(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(m_depth.Get(), 0);

    ID3D11RenderTargetView* sbs_rtvs[] = {m_sbs_rtv.Get()};
    context->OMSetRenderTargets(1, sbs_rtvs, nullptr);

    context->Draw(3, 0);

    // Unbind so the caller (weaver / ScreenGrab) can sample m_sbs_tex.
    ID3D11RenderTargetView* null_rtvs[] = {nullptr};
    context->OMSetRenderTargets(1, null_rtvs, nullptr);
    return true;
}

bool Flat3DCompositorD3D11::save_screenshot(ID3D11DeviceContext* context, const Flat3DFrameParams& params,
                                            const std::wstring& parallel_path, const std::wstring& crossview_path) {
    if (!m_ready || context == nullptr || m_eye_tex[0] == nullptr || m_eye_tex[1] == nullptr ||
        m_eye_w == 0 || m_eye_h == 0) {
        return false;
    }

    // Restore the game's pipeline state on return — the SbS build clobbers it.
    ScopedD3D11State _state{context};

    // Renders the composited pair into m_sbs_tex at native per-eye resolution and
    // encodes it. Forced to 8-bit BGRA so HDR eye formats normalize (the SbS
    // build already emits SDR sRGB values). cross=false gives the geometric
    // left|right pair; cross=true swaps the halves for cross-eyed viewing.
    const auto build_and_save = [&](bool cross, const std::wstring& path) -> bool {
        if (path.empty()) {
            return true;
        }
        Flat3DFrameParams p = params;
        p.eye_swap = cross; // eye0 is always the left eye: parallel = no swap
        if (!build_sbs(context, p, m_eye_w, m_eye_h)) {
            return false;
        }
        const HRESULT hr = DirectX::SaveWICTextureToFile(
            context, m_sbs_tex.Get(), GUID_ContainerFormatPng, path.c_str(),
            &GUID_WICPixelFormat32bppBGRA);
        if (FAILED(hr)) {
            spdlog::error("[Flat3D][D3D11] Screenshot encode failed (hr=0x{:x})", (uint32_t)hr);
            return false;
        }
        return true;
    };

    bool ok = build_and_save(false, parallel_path);
    ok = build_and_save(true, crossview_path) && ok;
    return ok;
}

bool Flat3DCompositorD3D11::weave_leiasr(ID3D11DeviceContext* context, ID3D11RenderTargetView* backbuffer_rtv,
                                         uint32_t out_w, uint32_t out_h, const Flat3DFrameParams& params) {
#ifdef UEVR_FLAT3D_HAS_LEIASR
    // Resolve SR's window/monitor queries to physical pixels for the whole
    // create+weave interaction (maps the weave to the full native panel rather
    // than a DPI-virtualized sub-region under a non-per-monitor-aware host).
    ScopedPerMonitorDpi dpi_guard{};

    // Lazy one-shot weaver creation (SRService may block briefly).
    if (m_sr_weaver == nullptr && !m_sr_attempted) {
        m_sr_attempted = true;

        m_sr_context = try_create_sr_context();

        if (m_sr_context == nullptr) {
            spdlog::warn("[Flat3D] LeiaSR: SRContext::create failed (SRService not running?) — falling back to SbS");
            return false;
        }

        try {
            const auto wec = SR::CreateDX11Weaver(m_sr_context, context, m_hwnd, &m_sr_weaver);

            if (wec != WeaverErrorCode::WeaverSuccess || m_sr_weaver == nullptr) {
                spdlog::warn("[Flat3D] LeiaSR: CreateDX11Weaver failed (code {}) — falling back to SbS", (int)wec);
                m_sr_weaver = nullptr;
                return false;
            }

            // Input is sRGB 8-bit from the engine; weaver converts on read/write.
            m_sr_weaver->setShaderSRGBConversion(true, true);
            m_sr_weaver->setLatencyInFrames(1);
            m_sr_weaver->setContext(context);

            // MUST run AFTER weaver creation: starts eye tracking. Without it
            // every call succeeds but the weave never responds to head movement.
            m_sr_context->initialize();
        } catch (...) {
            spdlog::warn("[Flat3D] LeiaSR: weaver init threw — falling back to SbS");
            if (m_sr_weaver != nullptr) {
                m_sr_weaver->destroy();
                m_sr_weaver = nullptr;
            }
            return false;
        }

        spdlog::info("[Flat3D] LeiaSR weaver ready");
    }

    // Build the SbS input the weaver expects from the two eye textures, at
    // DISPLAY resolution (upscale-then-weave): the SR lenticular pattern is
    // display-pixel-exact only when its input matches the panel.
    if (!build_sbs(context, params, out_w, out_h)) {
        return false;
    }

    if (m_sr_weaver != nullptr && !m_sr_input_bound) {
        m_sr_weaver->setInputViewTexture(m_sbs_srv.Get(), (int)m_sbs_w, (int)m_sbs_h, m_eye_format);
        m_sr_input_bound = true;
    }

    if (m_sr_weaver == nullptr) {
        return false;
    }

    D3D11_VIEWPORT viewport{};
    viewport.Width = (float)out_w;
    viewport.Height = (float)out_h;
    viewport.MaxDepth = 1.0f;

    ID3D11RenderTargetView* rtvs[] = {backbuffer_rtv};
    context->OMSetRenderTargets(1, rtvs, nullptr);
    context->RSSetViewports(1, &viewport);

    try {
        m_sr_weaver->weave();
    } catch (...) {
        // SR service crash / display unplug mid-session — disable and fall
        // back to SbS instead of taking the game down.
        spdlog::warn("[Flat3D] LeiaSR: weave threw — disabling weaver");
        m_sr_weaver->destroy();
        m_sr_weaver = nullptr;
        return false;
    }

    ID3D11RenderTargetView* null_rtv[] = {nullptr};
    context->OMSetRenderTargets(1, null_rtv, nullptr);

    return true;
#else
    (void)context; (void)backbuffer_rtv; (void)out_w; (void)out_h; (void)params;
    return false;
#endif
}

void Flat3DCompositorD3D11::destroy_leiasr() {
#ifdef UEVR_FLAT3D_HAS_LEIASR
    if (m_sr_weaver != nullptr) {
        m_sr_weaver->destroy();
        m_sr_weaver = nullptr;
    }
    // SRContext is service-owned; do not delete (VRto3D pattern).
    m_sr_context = nullptr;
    m_sr_attempted = false;
    m_sr_input_bound = false;
#endif
    m_sbs_tex.Reset();
    m_sbs_srv.Reset();
    m_sbs_rtv.Reset();
}

void Flat3DCompositorD3D11::reset() {
    for (int i = 0; i < 2; ++i) {
        m_eye_tex[i].Reset();
        m_eye_srv[i].Reset();
        m_eye_rtv[i].Reset();
    }

    m_pair_pending.Reset();
    m_pair_pending_valid = false;

    m_menu_srv.Reset();
    m_menu_src = nullptr;

    m_hud_depth_srv.Reset();
    m_hud_depth_src = nullptr;
    m_hud_depth_uscale = 1.0f;
    m_anchor_cb.Reset();
    m_hud_mode_effective = 0;

    m_classify_vs.Reset();
    m_classify_ps.Reset();
    m_classify_cb.Reset();
    for (int i = 0; i < 2; ++i) {
        m_hud_mask_tex[i].Reset();
        m_hud_mask_rtv[i].Reset();
        m_hud_mask_srv[i].Reset();
    }
    m_hud_prev_ui.Reset();
    m_hud_prev_srv.Reset();
    m_hud_prev_w = 0;
    m_hud_prev_h = 0;
    m_hud_prev_fmt = DXGI_FORMAT_UNKNOWN;
    m_mask_idx = 0;
    m_hud_prev_valid = false;

    m_huddepth_vs.Reset();
    m_huddepth_ps.Reset();
    m_huddepth_cb.Reset();
    for (int i = 0; i < 2; ++i) {
        m_huddepth_tex[i].Reset();
        m_huddepth_rtv[i].Reset();
        m_huddepth_srv[i].Reset();
    }
    m_huddepth_idx = 0;

    m_vs.Reset();
    m_ps.Reset();
    m_cb.Reset();
    m_sampler.Reset();
    m_rasterizer.Reset();
    m_blend.Reset();
    m_depth.Reset();

    m_overlay_vs.Reset();
    m_overlay_ps.Reset();
    m_overlay_cb.Reset();
    m_overlay_blend.Reset();

    for (auto& tex : m_depth_staging) {
        tex.Reset();
    }
    m_depth_format = DXGI_FORMAT_UNKNOWN;
    m_depth_roi_w = 0;
    m_depth_frame = 0;
    m_center_ema_uu = -1.0f;
    m_depth_format_warned = false;

    m_coverage_vs.Reset();
    m_coverage_ps.Reset();
    m_coverage_rt.Reset();
    m_coverage_rtv.Reset();
    for (auto& tex : m_coverage_staging) {
        tex.Reset();
    }
    m_coverage_frame = 0;
    m_coverage_ema = 0.0f;

    destroy_leiasr();

    m_eye_w = 0;
    m_eye_h = 0;
    m_eye_format = DXGI_FORMAT_UNKNOWN;
    m_backbuffer_format = DXGI_FORMAT_UNKNOWN;
    m_colorspace = Flat3DColorSpace::SDR;
    m_src_srgb = false;
    m_ready = false;
}

} // namespace vrmod::flat3d
