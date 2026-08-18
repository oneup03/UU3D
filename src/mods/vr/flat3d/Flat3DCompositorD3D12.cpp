// Before any header that reaches windows.h: its min/max macros break the
// std::min/std::max calls below. This used to come in by accident, via the SR
// SDK's display.h; SR-lib's facade doesn't include it, so say it explicitly.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler")

#include <wincodec.h>
#include <../../directxtk12-src/Inc/ScreenGrab.h>

#include <spdlog/spdlog.h>

#include "Flat3DCompositorD3D12.hpp"

#ifdef UEVR_FLAT3D_HAS_LEIASR_DX12
// No SR SDK headers here: everything goes through SR-lib's facade, which keeps
// SRContext's transitive OpenCV includes out of this TU entirely.
#include <SR.hpp>

#include "Flat3DLeiaSRRuntime.hpp"
#endif

namespace vrmod::flat3d {

namespace {
// Engine resource states between our copies, matching the assumptions the
// HMD submit path makes in D3D12Component.cpp: the double-wide render target
// sits in RENDER_TARGET (copy_left/right calls pass it explicitly), depth in
// ENGINE_SRC_DEPTH.
constexpr auto kEngineSrcColor = D3D12_RESOURCE_STATE_RENDER_TARGET;
constexpr auto kEngineSrcDepth = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) {
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &b);
}

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

bool is_srgb_format(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

// The 8-bit BGRA render-target format the 3D screenshot uses: mirrors the
// backbuffer's sRGB-ness (so the repack applies the same encode the display
// does) but is always 8-bit. Rendering into the raw backbuffer format instead
// would force ScreenGrab through a >8-bit WIC conversion (e.g. R10G10B10A2 ->
// 32bppBGRA), which washes the gamma out.
DXGI_FORMAT screenshot_8bit_format(DXGI_FORMAT backbuffer_format) {
    return is_srgb_format(backbuffer_format) ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                                             : DXGI_FORMAT_B8G8R8A8_UNORM;
}
} // namespace

bool Flat3DCompositorD3D12::setup(ID3D12Device* device, uint32_t eye_w, uint32_t eye_h, DXGI_FORMAT eye_format,
                                  DXGI_FORMAT backbuffer_format, bool backbuffer_pq) {
    if (m_ready && eye_w == m_eye_w && eye_h == m_eye_h && eye_format == m_eye_format &&
        backbuffer_format == m_backbuffer_format && backbuffer_pq == m_backbuffer_pq && device == m_device) {
        return true;
    }

    reset();

    if (device == nullptr || eye_w == 0 || eye_h == 0) {
        return false;
    }

    m_device = device;
    m_backbuffer_format = backbuffer_format;
    m_backbuffer_pq = backbuffer_pq;

    switch (backbuffer_format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        m_colorspace = Flat3DColorSpace::SCRGB;
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        // 10-bit is NOT inherently HDR: SDR games commonly run R10G10B10A2
        // swapchains with plain G22 gamma. PQ-encoding those washes the
        // image out — only treat as HDR10 when the game explicitly set a PQ
        // color space (SetColorSpace1, tracked by the swapchain hook).
        m_colorspace = backbuffer_pq ? Flat3DColorSpace::HDR10_PQ : Flat3DColorSpace::SDR;
        break;
    default:
        m_colorspace = Flat3DColorSpace::SDR;
        break;
    }

    const bool eye_is_8bit =
        eye_format == DXGI_FORMAT_B8G8R8A8_UNORM || eye_format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
        eye_format == DXGI_FORMAT_R8G8B8A8_UNORM || eye_format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        eye_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || eye_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    m_src_srgb = eye_is_8bit && m_colorspace != Flat3DColorSpace::SDR;

    // Assigned before create_pipelines — the overlay PSO keys on m_eye_format.
    m_eye_w = eye_w;
    m_eye_h = eye_h;
    m_eye_format = eye_format;

    // --- per-eye textures ----------------------------------------------------
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = eye_w;
    tex_desc.Height = eye_h;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = eye_format;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // Eye size/format changed: drop the synced-pair stash so it is lazily
    // recreated at the new dimensions.
    m_pair_pending.Reset();
    m_pair_pending_valid = false;

    for (int i = 0; i < 2; ++i) {
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                   IID_PPV_ARGS(&m_eye_tex[i])))) {
            spdlog::error("[Flat3D][D3D12] Failed to create eye texture {}", i);
            reset();
            return false;
        }
    }

    // --- descriptor heaps ----------------------------------------------------
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.NumDescriptors = 24; // eye0,1, ui, menu, depth, mask, hud t4, tiledep A/B blocks (7-12), cls (21-23)
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&m_srv_heap)))) {
        spdlog::error("[Flat3D][D3D12] Failed to create SRV heap");
        reset();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 4; // eye0, eye1, backbuffer (rewritten per frame), coverage(1x1)

    if (FAILED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&m_rtv_heap)))) {
        spdlog::error("[Flat3D][D3D12] Failed to create RTV heap");
        reset();
        return false;
    }

    m_srv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    const auto view_fmt = view_format_for(eye_format);

    for (int i = 0; i < 2; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = view_fmt;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;

        auto srv_handle = m_srv_heap->GetCPUDescriptorHandleForHeapStart();
        srv_handle.ptr += (size_t)i * m_srv_stride;
        device->CreateShaderResourceView(m_eye_tex[i].Get(), &srv_desc, srv_handle);

        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = view_fmt;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        auto rtv_handle = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv_handle.ptr += (size_t)i * m_rtv_stride;
        device->CreateRenderTargetView(m_eye_tex[i].Get(), &rtv_desc, rtv_handle);
    }

    // --- full-screen-GUI coverage: 1x1 R32_FLOAT target (RTV slot 3) + one
    // small readback buffer per ring slot. All optional — any failure just
    // leaves the coverage signal off (cursor + pause detection still work).
    {
        D3D12_RESOURCE_DESC cov_desc{};
        cov_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        cov_desc.Width = 1;
        cov_desc.Height = 1;
        cov_desc.DepthOrArraySize = 1;
        cov_desc.MipLevels = 1;
        cov_desc.Format = DXGI_FORMAT_R32_FLOAT;
        cov_desc.SampleDesc.Count = 1;
        cov_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        if (SUCCEEDED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &cov_desc,
                                                      D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                                      IID_PPV_ARGS(&m_coverage_rt)))) {
            D3D12_RENDER_TARGET_VIEW_DESC cov_rtv_desc{};
            cov_rtv_desc.Format = DXGI_FORMAT_R32_FLOAT;
            cov_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            auto cov_rtv = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
            cov_rtv.ptr += (size_t)3 * m_rtv_stride;
            device->CreateRenderTargetView(m_coverage_rt.Get(), &cov_rtv_desc, cov_rtv);

            D3D12_HEAP_PROPERTIES rb_heap{};
            rb_heap.Type = D3D12_HEAP_TYPE_READBACK;

            D3D12_RESOURCE_DESC rb_desc{};
            rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rb_desc.Width = 256; // >= one R32 texel, footprint-aligned
            rb_desc.Height = 1;
            rb_desc.DepthOrArraySize = 1;
            rb_desc.MipLevels = 1;
            rb_desc.SampleDesc.Count = 1;
            rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            for (uint32_t i = 0; i < kRing; ++i) {
                if (FAILED(device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                           IID_PPV_ARGS(&m_coverage_readback[i])))) {
                    m_coverage_rt.Reset();
                    break;
                }
                m_coverage_copied[i] = false;
            }
        }
    }

    if (!create_pipelines(device)) {
        reset();
        return false;
    }

    for (uint32_t i = 0; i < kRing; ++i) {
        if (!m_cmds[i].setup(L"Flat3D compositor commands")) {
            spdlog::error("[Flat3D][D3D12] Failed to set up command ring");
            reset();
            return false;
        }
        m_depth_copied[i] = false;
    }

    m_ready = true;

    spdlog::info("[Flat3D][D3D12] Compositor ready: {}x{} per eye (format {}, colorspace {})",
                 eye_w, eye_h, (uint32_t)eye_format, (int)m_colorspace);

    return true;
}

bool Flat3DCompositorD3D12::create_pipelines(ID3D12Device* device) {
    // Root signature: [0] 16 root constants (b0), [1] SRV table (t0..t2),
    // static linear-clamp sampler (s0).
    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 5; // widest consumer: overlay pass t0..t4 (t4 = hud tile-depth)
    srv_range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 34; // max(RepackConstants=24, OverlayConstants=28, HudClassifyConstants=34)
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srv_range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // b1: HUD anchors
    params[2].Descriptor.ShaderRegister = 1;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 3;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 1;
    rs_desc.pStaticSamplers = &sampler;

    Microsoft::WRL::ComPtr<ID3DBlob> rs_blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> rs_err{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err))) {
        spdlog::error("[Flat3D][D3D12] Root signature serialize failed: {}",
                      rs_err != nullptr ? (const char*)rs_err->GetBufferPointer() : "?");
        return false;
    }

    if (FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                           IID_PPV_ARGS(&m_root_sig)))) {
        spdlog::error("[Flat3D][D3D12] CreateRootSignature failed");
        return false;
    }

    // Shaders (shared HLSL, fxc at runtime like the D3D11 side).
    auto compile = [&](const char* src, const char* entry, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& out) {
        Microsoft::WRL::ComPtr<ID3DBlob> err{};
        if (FAILED(D3DCompile(src, strlen(src), "flat3d12", nullptr, nullptr, entry, target, 0, 0, &out, &err))) {
            spdlog::error("[Flat3D][D3D12] Shader compile failed ({}): {}", entry,
                          err != nullptr ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        return true;
    };

    Microsoft::WRL::ComPtr<ID3DBlob> repack_vs{}, repack_ps{}, overlay_vs{}, overlay_ps{}, classify_vs{}, classify_ps{},
        huddepth_vs{}, huddepth_ps{};

    if (!compile(g_flat3d_repack_hlsl, "vs_main", "vs_5_0", repack_vs) ||
        !compile(g_flat3d_repack_hlsl, "ps_main", "ps_5_0", repack_ps) ||
        !compile(g_flat3d_overlay_hlsl, "vs_main", "vs_5_0", overlay_vs) ||
        !compile(g_flat3d_overlay_hlsl, "ps_main", "ps_5_0", overlay_ps) ||
        !compile(g_flat3d_hudclass_hlsl, "vs_main", "vs_5_0", classify_vs) ||
        !compile(g_flat3d_hudclass_hlsl, "ps_main", "ps_5_0", classify_ps) ||
        !compile(g_flat3d_huddepth_hlsl, "vs_main", "vs_5_0", huddepth_vs) ||
        !compile(g_flat3d_huddepth_hlsl, "ps_main", "ps_5_0", huddepth_ps)) {
        return false;
    }

    auto make_pso = [&](ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv_format, bool premult_blend,
                        Microsoft::WRL::ComPtr<ID3D12PipelineState>& out) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_root_sig.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (premult_blend) {
            auto& rt = pso.BlendState.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_ONE;
            rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = view_format_for(rtv_format);
        pso.SampleDesc.Count = 1;

        if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out)))) {
            spdlog::error("[Flat3D][D3D12] CreateGraphicsPipelineState failed (rtv format {})", (uint32_t)rtv_format);
            return false;
        }
        return true;
    };

    if (!make_pso(repack_vs.Get(), repack_ps.Get(), m_backbuffer_format, false, m_repack_pso)) {
        return false;
    }

    // SbS-build PSO (LeiaSR weaver input): the repack shader targeting the
    // eye format — applies eye swap + SDR color correction to the weave.
    if (!make_pso(repack_vs.Get(), repack_ps.Get(), m_eye_format, false, m_sbs_pso)) {
        return false;
    }

    // Screenshot PSO: repack shader into an 8-bit RTV mirroring the backbuffer's
    // sRGB-ness (see screenshot_8bit_format) — the display's exact color, but
    // 8-bit so ScreenGrab doesn't wash it through a >8-bit WIC conversion.
    if (!make_pso(repack_vs.Get(), repack_ps.Get(), screenshot_8bit_format(m_backbuffer_format), false,
                  m_screenshot_pso)) {
        return false;
    }

    // Overlay PSO renders INTO the eye textures.
    if (!make_pso(overlay_vs.Get(), overlay_ps.Get(), m_eye_format, true, m_overlay_pso)) {
        return false;
    }

    // HUD world/static classification (tiny RGBA8 mask: r=class, g=anim, b=occ).
    if (!make_pso(classify_vs.Get(), classify_ps.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, false, m_classify_pso)) {
        return false;
    }

    // HUD per-tile depth pre-pass (mode 1): resolve + min-z-flood into R32F.
    if (!make_pso(huddepth_vs.Get(), huddepth_ps.Get(), DXGI_FORMAT_R32_FLOAT, false, m_huddepth_pso)) {
        return false;
    }

    // Full-screen-GUI coverage reduction (1x1 R32_FLOAT). Optional — on failure
    // the coverage signal is simply absent (cursor + pause detection remain).
    Microsoft::WRL::ComPtr<ID3DBlob> coverage_vs{}, coverage_ps{};
    if (compile(g_flat3d_coverage_hlsl, "vs_main", "vs_5_0", coverage_vs) &&
        compile(g_flat3d_coverage_hlsl, "ps_main", "ps_5_0", coverage_ps)) {
        make_pso(coverage_vs.Get(), coverage_ps.Get(), DXGI_FORMAT_R32_FLOAT, false, m_coverage_pso);
    }

    return true;
}

void Flat3DCompositorD3D12::record_overlays(ID3D12GraphicsCommandList* cmd, bool have_ui, bool have_menu,
                                            const Flat3DFrameParams& params, uint32_t eye_refresh_mask) {
    // A 3D-screenshot capture hides only the UEVR menu (layer 3) so it doesn't
    // land in the saved pair; the game HUD, crosshair and stereo cursor stay.
    // See begin_screenshot().
    const bool want_ui = params.ui_enabled && have_ui;
    const bool want_game_crosshair = params.crosshair_mode == 1 && have_ui;
    const bool want_laser = params.crosshair_mode == 2;
    const bool want_menu = !m_ss_active && have_menu;
    const bool want_cursor = params.cursor_enabled;

    // Effective mode computed in composite (depth SRV + classification state).
    const int32_t hud_mode = m_hud_mode_effective;

    if (!want_ui && !want_game_crosshair && !want_laser && !want_menu && !want_cursor) {
        return;
    }

    cmd->SetPipelineState(m_overlay_pso.Get());

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)m_eye_w;
    viewport.Height = (float)m_eye_h;
    viewport.MaxDepth = 1.0f;
    cmd->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{0, 0, (LONG)m_eye_w, (LONG)m_eye_h};
    cmd->RSSetScissorRects(1, &scissor);

    // t0 = UI (heap slot 2).
    auto ui_table = m_srv_heap->GetGPUDescriptorHandleForHeapStart();
    ui_table.ptr += (size_t)2 * m_srv_stride;
    cmd->SetGraphicsRootDescriptorTable(1, ui_table);

    const int32_t eye_space = m_src_srgb || m_colorspace == Flat3DColorSpace::SDR ? 0 : (int32_t)m_colorspace;

    const auto argb = params.crosshair_color_argb;
    const float laser[4] = {
        ((argb >> 16) & 0xFF) / 255.0f,
        ((argb >> 8) & 0xFF) / 255.0f,
        (argb & 0xFF) / 255.0f,
        ((argb >> 24) & 0xFF) / 255.0f,
    };

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

        auto rtv = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (size_t)eye * m_rtv_stride;
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        const float ui_shift_uv = dir * params.ui_shift_px / (float)m_eye_w;
        const float ch_shift_uv = dir * params.crosshair_shift_px / (float)m_eye_w;
        const float menu_shift_uv = dir * params.menu_shift_px / (float)m_eye_w;

        // Geometry-depth cursor: only when selected AND scene depth is bound; the
        // shader then samples depth under the tip and shifts the arrow itself.
        const int32_t cursor_geo = (params.cursor_depth_mode == 1 && m_have_depth_srv) ? 1 : 0;

        auto draw_layer = [&](int32_t layer, float shift_uv, float scale, float center_x, float center_y) {
            OverlayConstants oc{};
            // Central aspect crop of the GAME-UI texture only (layers 0/1): the
            // game constrains its HUD to a central slice matching the eye aspect,
            // so sample that slice instead of the whole (wider/taller) target.
            // Other layers (menu, cursor) are authored at eye aspect => no crop.
            // Native Render + Upscale: the game HUD (0/1) AND the UEVR menu (3)
            // draw at the perceived half width into the TOP-LEFT of a full-width
            // target — sample that top-left region (anchor 0), not the centre.
            const bool topleft = params.ui_topleft_crop && (layer == 0 || layer == 1 || layer == 3);
            const bool crop_layer = (layer == 0 || layer == 1) || topleft;
            const float crop_x = crop_layer ? params.ui_crop_x : 1.0f;
            const float crop_y = crop_layer ? params.ui_crop_y : 1.0f;
            // The UI's center within the target: central (0.5) normally, or the
            // centre of the TOP-LEFT crop region (crop/2) under Native Render +
            // Upscale. Everything else — the fit-squish `scale`, the vertical
            // scene_scale, and the per-eye depth shift — is applied about that
            // centre, so the GUI still stays on-screen and squishes at depth.
            // (For central, crop*0.5 collapses to 0.5 => identical to before.)
            const float ui_cx = topleft ? crop_x * 0.5f : 0.5f;
            const float ui_cy = topleft ? crop_y * 0.5f : 0.5f;
            oc.uv_scale[0] = crop_x / scale;
            oc.uv_scale[1] = crop_y * params.scene_scale;
            oc.uv_offset[0] = ui_cx - crop_x * (0.5f + shift_uv) / scale;
            oc.uv_offset[1] = ui_cy - 0.5f * crop_y * params.scene_scale;
            oc.color[0] = laser[0];
            oc.color[1] = laser[1];
            oc.color[2] = laser[2];
            oc.color[3] = laser[3];
            oc.layer = layer;
            oc.ui_invert_alpha = params.ui_invert_alpha; // shader applies it only to game-UI layers (0/1)
            oc.ui_color_gate = params.ui_color_gate;     // zero alpha on colourless pixels (invert-0.5 tint fix)
            oc.colorspace = eye_space;
            oc.paper_white = params.paper_white_nits;
            oc.region_radius_uv = (layer == 2 || layer == 4) ? 0.0f
                                : (want_game_crosshair ? params.crosshair_region_radius : 0.0f);
            oc.region_center[0] = center_x;
            oc.region_center[1] = center_y;
            oc.dot_radius_px = (layer == 4) ? params.cursor_size_px : params.crosshair_size_px;
            oc.eye_width_px = (float)m_eye_w;
            oc.eye_height_px = (float)m_eye_h;
            // Low 4 bits = mode; bits 4..7 = x-dilation tiles, bits 8..11 = y.
            // Fraction-of-screen-WIDTH radius -> MASK TILES per axis (grid 64x36),
            // with the central UI crop folded in so the neighbourhood keeps a
            // fixed on-screen shape: x needs *crop_x, y needs *crop_y.
            const int32_t base_mode = (layer == 0) ? ((hud_mode == 1 && params.hud_debug) ? 3 : hud_mode) : 0;
            const int32_t tiles_x = std::clamp(
                (int32_t)std::lround(params.hud_icon_radius * 64.0f * params.ui_crop_x), 1, 8);
            const int32_t tiles_y = std::clamp(
                (int32_t)std::lround(params.hud_icon_radius * 36.0f * params.ui_crop_y), 1, 15);
            oc.hud_mode = base_mode | (tiles_x << 4) | (tiles_y << 8);
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

            static_assert(sizeof(OverlayConstants) == 28 * sizeof(uint32_t), "overlay root constant size");
            cmd->SetGraphicsRoot32BitConstants(0, 28, &oc, 0);
            cmd->DrawInstanced(3, 1, 0, 0);
        };

        if (want_ui) {
            // Depth modes ride the same flat transform (fit-scale included);
            // the shader adds only the per-pixel DELTA from the flat shift.
            draw_layer(0, ui_shift_uv, params.ui_scale, 0.5f + ch_shift_uv, params.crosshair_region_center_y);
        }
        if (want_game_crosshair) {
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
}

bool Flat3DCompositorD3D12::composite(ID3D12Resource* double_wide,
                                      ID3D12Resource* right_eye_src,
                                      ID3D12Resource* ui_tex,
                                      ID3D12Resource* menu_tex,
                                      ID3D12Resource* backbuffer,
                                      uint32_t out_w, uint32_t out_h,
                                      const Flat3DFrameParams& params,
                                      ID3D12Resource* scene_depth, float nearz_uu,
                                      float* out_center_uu, float* out_nearest_uu,
                                      float* out_ui_coverage,
                                      HWND hwnd,
                                      D3D12_RESOURCE_STATES scene_depth_state) {
    if (out_center_uu != nullptr) *out_center_uu = m_center_ema_uu;
    if (out_nearest_uu != nullptr) *out_nearest_uu = -1.0f;
    if (out_ui_coverage != nullptr) *out_ui_coverage = m_coverage_ema;

    if (!m_ready || double_wide == nullptr || backbuffer == nullptr) {
        return false;
    }

    const auto slot = (uint32_t)(m_frame % kRing);
    auto& ctx = m_cmds[slot];

    // Waiting the fence here guarantees this slot's depth readback buffer is
    // idle — harvest it BEFORE re-recording.
    ctx.wait(INFINITE);

    if (m_depth_copied[slot]) {
        read_depth_slot(slot, nearz_uu, out_center_uu, out_nearest_uu);
        m_depth_copied[slot] = false;
    }

    // Full-screen-GUI coverage: harvest this slot's readback (a couple frames
    // old — the fence wait above guarantees it's idle) and EMA-smooth.
    if (m_coverage_copied[slot] && m_coverage_readback[slot] != nullptr) {
        void* data = nullptr;
        const D3D12_RANGE read_range{0, sizeof(float)};
        if (SUCCEEDED(m_coverage_readback[slot]->Map(0, &read_range, &data)) && data != nullptr) {
            const float cov = *(const float*)data;
            const D3D12_RANGE no_write{0, 0};
            m_coverage_readback[slot]->Unmap(0, &no_write);
            if (cov >= 0.0f && cov <= 1.0f) {
                m_coverage_ema += (cov - m_coverage_ema) * 0.25f;
            }
        }
        m_coverage_copied[slot] = false;
    }
    if (out_ui_coverage != nullptr) {
        *out_ui_coverage = m_coverage_ema;
    }

    auto* cmd = ctx.cmd_list.Get();

    // --- 1. Refresh eye cache from the double-wide --------------------------
    // Under extreme compat the "double-wide" IS the real backbuffer (PRESENT
    // state at this point); otherwise the engine RT sits in RENDER_TARGET.
    const auto src_state = params.extreme_backbuffer_src ? D3D12_RESOURCE_STATE_PRESENT : kEngineSrcColor;
    // The AFW-warped second eye arrives in ALL_SHADER_RESOURCE (the plugin leaves it
    // there after EvaluateFrameWarp); the native-stereo-fix scene-capture is in
    // RENDER_TARGET (kEngineSrcColor).
    const auto second_src_state = params.warp_frame
        ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
        : kEngineSrcColor;

    barrier(cmd, double_wide, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);

    if (right_eye_src != nullptr) {
        barrier(cmd, right_eye_src, second_src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }

    const auto copy_half = [&](int eye) {
        barrier(cmd, m_eye_tex[eye].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

        // Native-stereo-fix titles: only the LEFT half of the double-wide is
        // written by the engine; the RIGHT eye is rendered into the dedicated
        // scene-capture target (same consumption as the HMD submit path).
        // AFW: the engine renders ONE eye into the LEFT half; the OTHER eye is the
        // discrete warped texture (right_eye_src), a full-frame copy.
        const int fresh_eye = params.afr_left_eye ? 0 : 1;
        const bool use_capture =
            right_eye_src != nullptr &&
            ((eye == 1 && params.native_stereo_layout) ||
             (params.warp_frame && eye != fresh_eye));

        // AFR / synced sequential / AFW: the engine renders ONE view per frame and
        // it always lands in the LEFT half of the double-wide (same source
        // box the HMD AFR submit paths use) — regardless of which eye it is.
        const bool left_half_src = params.native_stereo_layout || params.afr_frame || params.warp_frame;

        D3D12_BOX box{};
        box.left = (eye == 0 || left_half_src) ? 0 : m_eye_w;
        box.right = (eye == 0 || left_half_src) ? m_eye_w : m_eye_w * 2;
        box.top = 0;
        box.bottom = m_eye_h;
        box.front = 0;
        box.back = 1;

        if (use_capture) {
            const auto sc_desc = right_eye_src->GetDesc();
            box.left = 0;
            box.right = std::min<UINT>(m_eye_w, (UINT)sc_desc.Width);
            box.bottom = std::min<UINT>(m_eye_h, sc_desc.Height);
        }

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = use_capture ? right_eye_src : double_wide;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_eye_tex[eye].Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

        // Eyes that get overlay draws go to RENDER_TARGET; others back to PSR.
        barrier(cmd, m_eye_tex[eye].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    };

    // Both eyes go through RENDER_TARGET so overlay recording is uniform;
    // AFR frames still refresh only the fresh eye's contents. The mask records
    // which eyes actually got new scene content this present — overlays bake
    // only into those (see record_overlays).
    uint32_t eye_refresh_mask = 0b11;

    if (params.afr_frame) {
        const int fresh = params.afr_left_eye ? 0 : 1;
        const int other = 1 - fresh;
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
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                auto pd = m_eye_tex[0]->GetDesc();
                pd.Flags = D3D12_RESOURCE_FLAG_NONE;
                if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &pd,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_pair_pending)))) {
                    m_pair_pending.Reset();
                }
            }

            if (m_pair_pending != nullptr) {
                // AFR sources always render into the LEFT half (see copy_half).
                D3D12_BOX box{};
                box.right = m_eye_w;
                box.bottom = m_eye_h;
                box.back = 1;

                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = double_wide;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = m_pair_pending.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

                cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                m_pair_pending_valid = true;
                m_pair_pending_eye = fresh;
                eye_refresh_mask = 0; // held pair: both eyes keep their composited image

                // Neither eye cache was touched; both still need RENDER_TARGET
                // for the (uniform) overlay pass.
                barrier(cmd, m_eye_tex[0].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                barrier(cmd, m_eye_tex[1].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            } else {
                copy_half(fresh); // allocation failed: fall back to plain AFR
                eye_refresh_mask = 1u << fresh;
                barrier(cmd, m_eye_tex[other].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        } else {
            // Publish the stashed first half into ITS eye slot (recorded at
            // stash time — the pair boundary is not a stable parity, so never
            // assume the stash is simply the complement of the current eye).
            if (m_pair_pending_valid && m_pair_pending != nullptr && m_pair_pending_eye != fresh) {
                barrier(cmd, m_eye_tex[m_pair_pending_eye].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                barrier(cmd, m_pair_pending.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmd->CopyResource(m_eye_tex[m_pair_pending_eye].Get(), m_pair_pending.Get());
                barrier(cmd, m_pair_pending.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                barrier(cmd, m_eye_tex[m_pair_pending_eye].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
                m_pair_pending_valid = false;
                copy_half(fresh);
                eye_refresh_mask = 0b11; // both halves republished together
            } else {
                m_pair_pending_valid = false;
                copy_half(fresh);
                eye_refresh_mask = 1u << fresh;
                barrier(cmd, m_eye_tex[other].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
    } else {
        m_pair_pending_valid = false; // native/warp frame: any held stash is stale
        copy_half(0);
        copy_half(1);
    }

    barrier(cmd, double_wide, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state);

    if (right_eye_src != nullptr) {
        barrier(cmd, right_eye_src, D3D12_RESOURCE_STATE_COPY_SOURCE, second_src_state);
    }

    // NOTE on overlays under AFR: the stale eye keeps last frame's UI baked
    // in (it holds a fully-composited image). This matches how the stale eye
    // works for the scene itself.

    // --- 2. UI SRV (heap slot 2, rewritten per frame) + overlays -------------
    ID3D12DescriptorHeap* heaps[] = {m_srv_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_root_sig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    bool have_ui = false;

    if (ui_tex != nullptr) {
        const auto ui_desc = ui_tex->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = view_format_for(ui_desc.Format);
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;

        auto handle = m_srv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (size_t)2 * m_srv_stride;
        m_device->CreateShaderResourceView(ui_tex, &srv_desc, handle);
        have_ui = true;
        // Engine leaves the UI target in a shader-readable state; sampled as-is.
    }

    // --- HUD depth-mode resources -------------------------------------------
    m_have_depth_srv = false;
    m_hud_depth_uscale = 1.0f;

    if ((params.hud_depth_mode == 1 || params.cursor_depth_mode == 1) && scene_depth != nullptr) {
        const auto sd_desc = scene_depth->GetDesc();
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

        if (srv_fmt != DXGI_FORMAT_UNKNOWN) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd_srv{};
            sd_srv.Format = srv_fmt;
            sd_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd_srv.Texture2D.MipLevels = 1;

            auto handle = m_srv_heap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += (size_t)4 * m_srv_stride;
            m_device->CreateShaderResourceView(scene_depth, &sd_srv, handle);
            m_have_depth_srv = true;
            // Double-wide depth: sample the LEFT view's half. kEngineSrcDepth
            // already includes PIXEL_SHADER_RESOURCE — no barrier needed.
            m_hud_depth_uscale = sd_desc.Width >= (uint64_t)m_eye_w * 2 ? 0.5f : 1.0f;
        }
    }

    // Anchor constants (b1) — bound every frame (empty when the mode is off)
    // since the overlay shader declares the buffer unconditionally.
    if (m_anchor_cb[slot] == nullptr) {
        D3D12_HEAP_PROPERTIES up_heap{};
        up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC buf_desc{};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = (sizeof(HudAnchorConstants) + 255) & ~255ull;
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (SUCCEEDED(m_device->CreateCommittedResource(&up_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&m_anchor_cb[slot])))) {
            void* mapped = nullptr;
            const D3D12_RANGE no_read{0, 0};
            if (SUCCEEDED(m_anchor_cb[slot]->Map(0, &no_read, &mapped))) {
                m_anchor_cb_ptr[slot] = (uint8_t*)mapped;
            }
        }
    }

    if (m_anchor_cb_ptr[slot] != nullptr) {
        HudAnchorConstants ac{};
        ac.count = (int32_t)params.anchor_count;
        ac.radius_uv = params.hud_marker_radius;
        ac.feather_uv = params.hud_marker_radius * 0.35f;

        for (uint32_t i = 0; i < params.anchor_count && i < Flat3DFrameParams::kMaxHudAnchors; ++i) {
            ac.anchors[i][0] = params.anchors[i][0];
            ac.anchors[i][1] = params.anchors[i][1];
            ac.anchors[i][2] = params.anchors[i][2];
        }

        memcpy(m_anchor_cb_ptr[slot], &ac, sizeof(ac));
        cmd->SetGraphicsRootConstantBufferView(2, m_anchor_cb[slot]->GetGPUVirtualAddress());
    }

    bool have_menu = false;

    if (menu_tex != nullptr) {
        const auto menu_desc = menu_tex->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = view_format_for(menu_desc.Format);
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;

        auto handle = m_srv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (size_t)3 * m_srv_stride;
        m_device->CreateShaderResourceView(menu_tex, &srv_desc, handle);
        have_menu = true;
        // The Framework leaves its IMGUI RT in PIXEL_SHADER_RESOURCE between
        // frames (this frame's menu is drawn AFTER the composite — the layer
        // holds last frame's menu, same one-frame latency the HMD overlay
        // path has).
    }

    // --- HUD world/static classification (mode 1) -----------------------------
    // Tiny mask ping-pong: classify against last frame's UI copy, then
    // snapshot the current UI as next frame's reference (BEFORE the
    // post-composite UI clear below).
    bool classification_ok = false;

    if (params.hud_depth_mode == 1 && m_have_depth_srv && ui_tex != nullptr && m_classify_pso != nullptr) {
        if (m_hud_mask[0] == nullptr) {
            D3D12_HEAP_PROPERTIES props{};
            props.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC mask_desc{};
            mask_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            mask_desc.Width = kHudMaskW;
            mask_desc.Height = kHudMaskH;
            mask_desc.DepthOrArraySize = 1;
            mask_desc.MipLevels = 1;
            mask_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            mask_desc.SampleDesc.Count = 1;
            mask_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_CLEAR_VALUE clear_value{};
            clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

            bool ok = true;
            for (int i = 0; i < 2 && ok; ++i) {
                ok = SUCCEEDED(m_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &mask_desc,
                                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
                                                                 IID_PPV_ARGS(&m_hud_mask[i])));
            }

            if (ok && m_hud_mask_rtv_heap == nullptr) {
                D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
                heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                heap_desc.NumDescriptors = 2;
                ok = SUCCEEDED(m_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_hud_mask_rtv_heap)));
            }

            if (ok) {
                for (int i = 0; i < 2; ++i) {
                    auto rtv = m_hud_mask_rtv_heap->GetCPUDescriptorHandleForHeapStart();
                    rtv.ptr += (size_t)i * m_rtv_stride;
                    m_device->CreateRenderTargetView(m_hud_mask[i].Get(), nullptr, rtv);
                    const float zero_clear[4]{};
                    cmd->ClearRenderTargetView(rtv, zero_clear, 0, nullptr);
                    barrier(cmd, m_hud_mask[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                }
            } else {
                m_hud_mask[0].Reset();
                m_hud_mask[1].Reset();
            }
        }

        const auto full_ui_desc = ui_tex->GetDesc();

        if (m_hud_mask[0] != nullptr &&
            (m_hud_prev_ui == nullptr || m_hud_prev_w != full_ui_desc.Width || m_hud_prev_h != full_ui_desc.Height ||
             m_hud_prev_fmt != full_ui_desc.Format)) {
            m_hud_prev_ui.Reset();
            m_hud_prev_valid = false;

            D3D12_HEAP_PROPERTIES props{};
            props.Type = D3D12_HEAP_TYPE_DEFAULT;

            auto prev_desc = full_ui_desc;
            prev_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

            if (SUCCEEDED(m_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &prev_desc,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&m_hud_prev_ui)))) {
                m_hud_prev_w = full_ui_desc.Width;
                m_hud_prev_h = (uint32_t)full_ui_desc.Height;
                m_hud_prev_fmt = full_ui_desc.Format;
            }
        }

        if (m_hud_mask[0] != nullptr && m_hud_prev_ui != nullptr) {
            const int old_idx = m_mask_idx;
            const int new_idx = 1 - m_mask_idx;

            // SRVs: slot 5 = mask consumed by the overlay pass this frame;
            // slots 21..23 feed the classify pass (t0 cur, t1 prev, t2 old).
            D3D12_SHADER_RESOURCE_VIEW_DESC mask_srv{};
            mask_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            mask_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            mask_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            mask_srv.Texture2D.MipLevels = 1;

            D3D12_SHADER_RESOURCE_VIEW_DESC ui_srv_desc{};
            ui_srv_desc.Format = view_format_for(full_ui_desc.Format);
            ui_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            ui_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            ui_srv_desc.Texture2D.MipLevels = 1;

            const auto heap_start = m_srv_heap->GetCPUDescriptorHandleForHeapStart();
            const auto at = [&](size_t i) {
                auto h = heap_start;
                h.ptr += i * m_srv_stride;
                return h;
            };

            m_device->CreateShaderResourceView(m_hud_mask[new_idx].Get(), &mask_srv, at(5));
            m_device->CreateShaderResourceView(ui_tex, &ui_srv_desc, at(21));
            m_device->CreateShaderResourceView(m_hud_prev_ui.Get(), &ui_srv_desc, at(22));
            m_device->CreateShaderResourceView(m_hud_mask[old_idx].Get(), &mask_srv, at(23));

            // Classify into mask[new].
            barrier(cmd, m_hud_mask[new_idx].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);

            auto mask_rtv = m_hud_mask_rtv_heap->GetCPUDescriptorHandleForHeapStart();
            mask_rtv.ptr += (size_t)new_idx * m_rtv_stride;
            cmd->OMSetRenderTargets(1, &mask_rtv, FALSE, nullptr);

            D3D12_VIEWPORT vp{};
            vp.Width = (float)kHudMaskW;
            vp.Height = (float)kHudMaskH;
            vp.MaxDepth = 1.0f;
            cmd->RSSetViewports(1, &vp);
            D3D12_RECT sc{0, 0, (LONG)kHudMaskW, (LONG)kHudMaskH};
            cmd->RSSetScissorRects(1, &sc);

            cmd->SetPipelineState(m_classify_pso.Get());

            // The classify pass runs in UI-TARGET uv, but flow and the user's
            // region settings are authored in SCREEN (eye) uv. The overlay reads
            // a central crop of the UI, so map screen->UI: extent *= crop; an
            // off-centre position re-centres about 0.5.
            const float cropx = params.ui_crop_x;
            const float cropy = params.ui_crop_y;

            HudClassifyConstants cc{};
            cc.flow_uv[0] = params.hud_flow_du * cropx;
            cc.flow_uv[1] = params.hud_flow_dv * cropy;
            cc.blend_alpha = 0.15f;
            cc.flow_valid = (params.hud_flow_valid && m_hud_prev_valid) ? 1 : 0;
            cc.translating = (params.hud_translating && m_hud_prev_valid) ? 1 : 0;
            cc.inv_mask_size[0] = 1.0f / (float)kHudMaskW;
            cc.inv_mask_size[1] = 1.0f / (float)kHudMaskH;
            cc.trans_d0_gate = params.hud_trans_gate;
            cc.rot_move_gate = params.hud_rot_gate;
            cc.occ_gate = params.hud_occ_gate;
            cc.halo_tiles = (float)params.hud_halo_tiles;
            cc.occ_safe_hw = params.hud_occ_safe_hw * cropx;
            cc.occ_safe_hh = params.hud_occ_safe_hh * cropy;
            cc.fill_radius = params.hud_fill_radius;
            cc.fill_gate = params.hud_fill_gate;
            cc.excl_count = params.hud_excl_count;
            for (int e = 0; e < 4; ++e) {
                cc.excl[e][0] = 0.5f + (params.hud_excl[e][0] - 0.5f) * cropx;
                cc.excl[e][1] = 0.5f + (params.hud_excl[e][1] - 0.5f) * cropy;
                cc.excl[e][2] = params.hud_excl[e][2] * cropx;
                cc.excl[e][3] = params.hud_excl[e][3] * cropy;
            }

            cc.ui_invert_alpha = params.ui_invert_alpha;
            cc.ui_color_gate = params.ui_color_gate;

            static_assert(sizeof(HudClassifyConstants) == 36 * sizeof(uint32_t), "classify constant size");
            cmd->SetGraphicsRoot32BitConstants(0, 34, &cc, 0); // 34 meaningful values (incl. ui_invert_alpha + ui_color_gate)

            auto cls_table = m_srv_heap->GetGPUDescriptorHandleForHeapStart();
            cls_table.ptr += (size_t)21 * m_srv_stride;
            cmd->SetGraphicsRootDescriptorTable(1, cls_table);

            cmd->DrawInstanced(3, 1, 0, 0);

            barrier(cmd, m_hud_mask[new_idx].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            // Snapshot cur UI as next frame's reference (pre-clear).
            constexpr auto kUiState =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier(cmd, ui_tex, kUiState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            barrier(cmd, m_hud_prev_ui.Get(),
                    m_hud_prev_valid ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(m_hud_prev_ui.Get(), ui_tex);
            barrier(cmd, m_hud_prev_ui.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            barrier(cmd, ui_tex, D3D12_RESOURCE_STATE_COPY_SOURCE, kUiState);

            m_hud_prev_valid = true;
            m_mask_idx = new_idx;
            classification_ok = true;
        }
    }

    // --- HUD per-tile depth pre-pass (mode 1) --------------------------------
    // Resolve + min-z-flood one nearest-surface inv-z per world tile into a
    // 64x36 R32_FLOAT texture (ping-pong), sampled by the overlay PS at t4.
    if (params.hud_depth_mode == 1 && m_have_depth_srv && classification_ok &&
        m_huddepth_pso != nullptr && scene_depth != nullptr) {
        // Lazy-create the two tile-depth targets + their RTV heap (mirrors the
        // mask-texture lazy-create above).
        if (m_huddepth_tex[0] == nullptr) {
            D3D12_HEAP_PROPERTIES props{};
            props.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC td_desc{};
            td_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td_desc.Width = kHudMaskW;
            td_desc.Height = kHudMaskH;
            td_desc.DepthOrArraySize = 1;
            td_desc.MipLevels = 1;
            td_desc.Format = DXGI_FORMAT_R32_FLOAT;
            td_desc.SampleDesc.Count = 1;
            td_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_CLEAR_VALUE clear_value{};
            clear_value.Format = DXGI_FORMAT_R32_FLOAT;

            bool ok = true;
            for (int i = 0; i < 2 && ok; ++i) {
                ok = SUCCEEDED(m_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &td_desc,
                                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
                                                                 IID_PPV_ARGS(&m_huddepth_tex[i])));
            }

            if (ok && m_huddepth_rtv_heap == nullptr) {
                D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
                heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                heap_desc.NumDescriptors = 2;
                ok = SUCCEEDED(m_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_huddepth_rtv_heap)));
            }

            if (ok) {
                for (int i = 0; i < 2; ++i) {
                    auto rtv = m_huddepth_rtv_heap->GetCPUDescriptorHandleForHeapStart();
                    rtv.ptr += (size_t)i * m_rtv_stride;
                    m_device->CreateRenderTargetView(m_huddepth_tex[i].Get(), nullptr, rtv);
                    const float zero_clear[4]{};
                    cmd->ClearRenderTargetView(rtv, zero_clear, 0, nullptr);
                    barrier(cmd, m_huddepth_tex[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                }
            } else {
                m_huddepth_tex[0].Reset();
                m_huddepth_tex[1].Reset();
            }
        }

        if (m_huddepth_tex[0] != nullptr) {
            const auto heap_start = m_srv_heap->GetCPUDescriptorHandleForHeapStart();
            const auto at = [&](size_t i) {
                auto h = heap_start;
                h.ptr += i * m_srv_stride;
                return h;
            };

            // scene_depth SRV format: same derivation as the slot-4 depth SRV.
            const auto sd_desc = scene_depth->GetDesc();
            DXGI_FORMAT sd_fmt = DXGI_FORMAT_R32_FLOAT;
            switch (sd_desc.Format) {
            case DXGI_FORMAT_R32G8X24_TYPELESS:
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                sd_fmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
                break;
            case DXGI_FORMAT_R24G8_TYPELESS:
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
                sd_fmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                break;
            default:
                sd_fmt = DXGI_FORMAT_R32_FLOAT;
                break;
            }

            D3D12_SHADER_RESOURCE_VIEW_DESC mask_srv{};
            mask_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            mask_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            mask_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            mask_srv.Texture2D.MipLevels = 1;

            D3D12_SHADER_RESOURCE_VIEW_DESC sd_srv{};
            sd_srv.Format = sd_fmt;
            sd_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd_srv.Texture2D.MipLevels = 1;

            D3D12_SHADER_RESOURCE_VIEW_DESC td_srv{};
            td_srv.Format = DXGI_FORMAT_R32_FLOAT;
            td_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            td_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            td_srv.Texture2D.MipLevels = 1;

            // Persistent tile-depth SRVs (block A[2] = slot 9 = td0,
            // block B[2] = slot 12 = td1) — the textures never change.
            if (!m_huddepth_srv_made) {
                m_device->CreateShaderResourceView(m_huddepth_tex[0].Get(), &td_srv, at(9));
                m_device->CreateShaderResourceView(m_huddepth_tex[1].Get(), &td_srv, at(12));
                m_huddepth_srv_made = true;
            }

            // Per-frame block inputs: block A/B = [mask, scene_depth] at 7,8 / 10,11.
            m_device->CreateShaderResourceView(m_hud_mask[m_mask_idx].Get(), &mask_srv, at(7));
            m_device->CreateShaderResourceView(scene_depth, &sd_srv, at(8));
            m_device->CreateShaderResourceView(m_hud_mask[m_mask_idx].Get(), &mask_srv, at(10));
            m_device->CreateShaderResourceView(scene_depth, &sd_srv, at(11));

            cmd->SetPipelineState(m_huddepth_pso.Get());

            D3D12_VIEWPORT vp{};
            vp.Width = (float)kHudMaskW;
            vp.Height = (float)kHudMaskH;
            vp.MaxDepth = 1.0f;
            cmd->RSSetViewports(1, &vp);
            D3D12_RECT sc{0, 0, (LONG)kHudMaskW, (LONG)kHudMaskH};
            cmd->RSSetScissorRects(1, &sc);

            HudDepthConstants dc{};
            dc.mask_thr = 0.2f;
            dc.inv_mask_size[0] = 1.0f / (float)kHudMaskW;
            dc.inv_mask_size[1] = 1.0f / (float)kHudMaskH;
            dc.hud_depth_uscale = m_hud_depth_uscale;
            dc.hud_nearz_uu = params.hud_nearz_uu;
            dc.aspect_xy = (float)m_eye_h / (float)m_eye_w; // eye_h/eye_w
            dc.ui_crop_x = params.ui_crop_x;
            dc.ui_crop_y = params.ui_crop_y;

            const auto gpu_start = m_srv_heap->GetGPUDescriptorHandleForHeapStart();

            // Resolve (k=0) then kHudDepthDiffusePasses min-z floods, ping-pong.
            for (uint32_t k = 0; k <= kHudDepthDiffusePasses; ++k) {
                const int write_idx = (k % 2 == 0) ? 0 : 1;
                const int read_src = (k == 0) ? 0 : (int)((k - 1) % 2); // previous write
                const size_t base_slot = (read_src == 0) ? 7 : 10;      // block A / block B

                dc.pass_idx = (int32_t)k;
                cmd->SetGraphicsRoot32BitConstants(0, 9, &dc, 0); // through ui_crop_y (dword 8)

                barrier(cmd, m_huddepth_tex[write_idx].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);

                auto rtv = m_huddepth_rtv_heap->GetCPUDescriptorHandleForHeapStart();
                rtv.ptr += (size_t)write_idx * m_rtv_stride;
                cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

                auto table = gpu_start;
                table.ptr += base_slot * m_srv_stride;
                cmd->SetGraphicsRootDescriptorTable(1, table);

                cmd->DrawInstanced(3, 1, 0, 0);

                barrier(cmd, m_huddepth_tex[write_idx].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }

            // Overlay t4 = final tile-depth (td0; kHudDepthDiffusePasses is even),
            // published at slot 6 for record_overlays' t0..t4 table.
            m_device->CreateShaderResourceView(m_huddepth_tex[0].Get(), &td_srv, at(6));
        }
    }

    m_hud_mode_effective = params.hud_depth_mode;
    if (m_hud_mode_effective == 1 && (!m_have_depth_srv || !classification_ok)) {
        m_hud_mode_effective = 0;
    }

    // 3D-screenshot capture: record which eyes are refreshed this present
    // (record_overlays below hides the UEVR menu while m_ss_active).
    if (m_ss_active) {
        m_ss_captured_mask |= eye_refresh_mask;
        ++m_ss_frames;
    }

    record_overlays(cmd, have_ui, have_menu, params, eye_refresh_mask);

    // --- Full-screen-GUI coverage reduction (independent of HUD mode) --------
    // Average the UI's alpha coverage to the 1x1 target and copy it into this
    // slot's readback (harvested at the top of a later composite). The UI SRV
    // (heap slot 2) is still valid here, before the post-composite UI clear.
    if (!m_coverage_disabled && m_coverage_pso != nullptr && m_coverage_rt != nullptr &&
        m_coverage_readback[slot] != nullptr && ui_tex != nullptr) {
        barrier(cmd, m_coverage_rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        auto cov_rtv = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        cov_rtv.ptr += (size_t)3 * m_rtv_stride;
        cmd->OMSetRenderTargets(1, &cov_rtv, FALSE, nullptr);

        D3D12_VIEWPORT cov_vp{};
        cov_vp.Width = 1.0f;
        cov_vp.Height = 1.0f;
        cov_vp.MaxDepth = 1.0f;
        cmd->RSSetViewports(1, &cov_vp);
        D3D12_RECT cov_sc{0, 0, 1, 1};
        cmd->RSSetScissorRects(1, &cov_sc);

        cmd->SetPipelineState(m_coverage_pso.Get());
        const float cov_consts[2]{params.ui_invert_alpha, params.ui_color_gate}; // undo inverted alpha + colour gate
        cmd->SetGraphicsRoot32BitConstants(0, 2, cov_consts, 0);
        auto ui_table = m_srv_heap->GetGPUDescriptorHandleForHeapStart(); // t0 = UI (slot 2)
        ui_table.ptr += (size_t)2 * m_srv_stride;
        cmd->SetGraphicsRootDescriptorTable(1, ui_table);
        cmd->DrawInstanced(3, 1, 0, 0);

        barrier(cmd, m_coverage_rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION cov_src{};
        cov_src.pResource = m_coverage_rt.Get();
        cov_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cov_src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION cov_dst{};
        cov_dst.pResource = m_coverage_readback[slot].Get();
        cov_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        cov_dst.PlacedFootprint.Offset = 0;
        cov_dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
        cov_dst.PlacedFootprint.Footprint.Width = 1;
        cov_dst.PlacedFootprint.Footprint.Height = 1;
        cov_dst.PlacedFootprint.Footprint.Depth = 1;
        cov_dst.PlacedFootprint.Footprint.RowPitch = 256; // D3D12 min copy alignment

        cmd->CopyTextureRegion(&cov_dst, 0, 0, 0, &cov_src, nullptr);
        m_coverage_copied[slot] = true;
    }


    // Clear the redirected UI target after consuming it — Slate only draws
    // deltas on top, so without this the HUD accumulates ghost trails and a
    // fresh target shows uninitialized VRAM (the HMD paths do the same
    // post-submit clear). The engine keeps it in shader-readable state.
    if (ui_tex != nullptr) {
        constexpr auto kEngineUiState =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        const auto ui_desc = ui_tex->GetDesc();
        D3D12_RENDER_TARGET_VIEW_DESC ui_rtv_desc{};
        ui_rtv_desc.Format = view_format_for(ui_desc.Format);
        ui_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        // Scratch RTV slot 2 — the repack pass rewrites it with the backbuffer below.
        auto ui_rtv = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        ui_rtv.ptr += (size_t)2 * m_rtv_stride;
        m_device->CreateRenderTargetView(ui_tex, &ui_rtv_desc, ui_rtv);

        barrier(cmd, ui_tex, kEngineUiState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        // Clear empty UI regions to alpha = ui_invert_alpha (NOT 0). Slate only
        // draws real content on top (with a=0 for titles like FF7 Rebirth), so
        // this is what lets the UI_InvertAlpha shader (1-a) tell drawn content
        // (a=0 -> opaque) from untouched empty screen (a=ui_invert_alpha ->
        // transparent). Matches the VR path's clear_rt. Alpha 0 here made every
        // empty pixel opaque black under invert, blacking out the geometry.
        const float ui_clear[4]{0.0f, 0.0f, 0.0f, params.ui_invert_alpha};
        cmd->ClearRenderTargetView(ui_rtv, ui_clear, 0, nullptr);
        barrier(cmd, ui_tex, D3D12_RESOURCE_STATE_RENDER_TARGET, kEngineUiState);
    }

    // Eyes back to PSR for the repack / SbS-build sampling.
    for (int eye = 0; eye < 2; ++eye) {
        barrier(cmd, m_eye_tex[eye].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // --- 3a. LeiaSR: build SbS from the eyes and weave into the backbuffer ---
    bool leia_done = false;
    if (params.mode == (int32_t)Flat3DOutputMode::LEIA_SR) {
        build_sbs(cmd, params, out_w, out_h);
        leia_done = weave_leiasr(cmd, backbuffer, out_w, out_h, hwnd);
    }

    // The lens preference tracks whether we actually wove, not the requested
    // mode: a LeiaSR frame that fell back to SbS must not leave the panel
    // lensed either. Idempotent, so this costs nothing on unchanged frames.
    set_leiasr_lens(leia_done);

    // --- 3b. Repack into the real backbuffer --------------------------------
    if (!leia_done) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = view_format_for(m_backbuffer_format);
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        auto bb_rtv = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        bb_rtv.ptr += (size_t)2 * m_rtv_stride;
        m_device->CreateRenderTargetView(backbuffer, &rtv_desc, bb_rtv);

        barrier(cmd, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        cmd->SetPipelineState(m_repack_pso.Get());
        cmd->OMSetRenderTargets(1, &bb_rtv, FALSE, nullptr);

        D3D12_VIEWPORT viewport{};
        viewport.Width = (float)out_w;
        viewport.Height = (float)out_h;
        viewport.MaxDepth = 1.0f;
        cmd->RSSetViewports(1, &viewport);

        D3D12_RECT scissor{0, 0, (LONG)out_w, (LONG)out_h};
        cmd->RSSetScissorRects(1, &scissor);

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

        static_assert(sizeof(RepackConstants) == 24 * sizeof(uint32_t), "repack root constant size");
        cmd->SetGraphicsRoot32BitConstants(0, 24, &constants, 0);

        auto eye_table = m_srv_heap->GetGPUDescriptorHandleForHeapStart(); // t0 = eye0, t1 = eye1
        cmd->SetGraphicsRootDescriptorTable(1, eye_table);

        cmd->DrawInstanced(3, 1, 0, 0);

        barrier(cmd, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }

    // --- 4. Optional SceneDepthZ stripe readback -----------------------------
    if (params.want_depth && !m_depth_disabled && scene_depth != nullptr && nearz_uu > 0.0f) {
        const auto sd_desc = scene_depth->GetDesc();

        const bool is_r32 = sd_desc.Format == DXGI_FORMAT_R32_TYPELESS || sd_desc.Format == DXGI_FORMAT_R32_FLOAT ||
                            sd_desc.Format == DXGI_FORMAT_D32_FLOAT;
        const bool is_r24g8 = sd_desc.Format == DXGI_FORMAT_R24G8_TYPELESS || sd_desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
        // D32 + S8 planar family (e.g. Gotham Knights): depth plane is a
        // 32-bit float in an 8-byte texel (R32_FLOAT_X8X24 footprint).
        const bool is_d32s8 = sd_desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
                              sd_desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

        if (is_r32 || is_r24g8 || is_d32s8) {
            const uint32_t d_eye_w = sd_desc.Width >= (uint64_t)m_eye_w * 2 ? (uint32_t)(sd_desc.Width / 2) : (uint32_t)sd_desc.Width;
            const uint32_t d_eye_h = sd_desc.Height;
            const uint32_t roi_x0 = (uint32_t)(d_eye_w * 0.08f);
            const uint32_t roi_w = (uint32_t)(d_eye_w * 0.92f) - roi_x0;
            const uint32_t roi_y0 = (uint32_t)(d_eye_h * 0.05f);
            const uint32_t roi_y1 = (uint32_t)(d_eye_h * 0.95f);

            // All supported layouts read 4 bytes per texel: planar depth
            // (D32S8) copies plane 0 only, which is tightly packed 32-bit.
            const uint32_t row_pitch = (roi_w * 4 + 255) & ~255u;

            if (m_depth_readback[slot] == nullptr || m_depth_roi_w != roi_w || m_depth_format != sd_desc.Format) {
                for (uint32_t i = 0; i < kRing; ++i) {
                    m_depth_readback[i].Reset();
                    m_depth_copied[i] = false;
                }

                D3D12_HEAP_PROPERTIES rb_heap{};
                rb_heap.Type = D3D12_HEAP_TYPE_READBACK;

                D3D12_RESOURCE_DESC rb_desc{};
                rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rb_desc.Width = (uint64_t)row_pitch * kDepthStripes * kStripeRows;
                rb_desc.Height = 1;
                rb_desc.DepthOrArraySize = 1;
                rb_desc.MipLevels = 1;
                rb_desc.SampleDesc.Count = 1;
                rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                bool ok = true;
                for (uint32_t i = 0; i < kRing; ++i) {
                    if (FAILED(m_device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                 IID_PPV_ARGS(&m_depth_readback[i])))) {
                        ok = false;
                        break;
                    }
                }

                if (ok) {
                    m_depth_roi_w = roi_w;
                    m_depth_row_pitch = row_pitch;
                    m_depth_format = sd_desc.Format;
                } else {
                    spdlog::error("[Flat3D][D3D12] Failed to create depth readback buffers");
                    for (uint32_t i = 0; i < kRing; ++i) {
                        m_depth_readback[i].Reset();
                    }
                }
            }

            if (m_depth_readback[slot] != nullptr) {
                barrier(cmd, scene_depth, scene_depth_state, D3D12_RESOURCE_STATE_COPY_SOURCE);

                for (uint32_t s = 0; s < kDepthStripes; ++s) {
                    const float frac = ((float)s + 0.5f) / (float)kDepthStripes;
                    const uint32_t y = roi_y0 + (uint32_t)((roi_y1 - roi_y0 - kStripeRows) * frac);

                    D3D12_BOX box{};
                    box.left = roi_x0;
                    box.right = roi_x0 + roi_w;
                    box.top = y;
                    box.bottom = y + kStripeRows;
                    box.front = 0;
                    box.back = 1;

                    D3D12_TEXTURE_COPY_LOCATION src{};
                    src.pResource = scene_depth;
                    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    src.SubresourceIndex = 0; // depth plane

                    D3D12_TEXTURE_COPY_LOCATION dst{};
                    dst.pResource = m_depth_readback[slot].Get();
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint.Offset = (uint64_t)s * kStripeRows * row_pitch;
                    // Planar D32S8: the depth PLANE's buffer footprint is
                    // R32_TYPELESS (the interleaved X8X24 formats are invalid
                    // as buffer footprints and poison the command list).
                    dst.PlacedFootprint.Footprint.Format = is_d32s8 ? DXGI_FORMAT_R32_TYPELESS
                                                         : is_r32  ? DXGI_FORMAT_R32_FLOAT
                                                                   : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                    dst.PlacedFootprint.Footprint.Width = roi_w;
                    dst.PlacedFootprint.Footprint.Height = kStripeRows;
                    dst.PlacedFootprint.Footprint.Depth = 1;
                    dst.PlacedFootprint.Footprint.RowPitch = row_pitch;

                    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                }

                barrier(cmd, scene_depth, D3D12_RESOURCE_STATE_COPY_SOURCE, scene_depth_state);
                m_depth_copied[slot] = true;
            }
        } else if (!m_depth_format_warned) {
            m_depth_format_warned = true;
            spdlog::warn("[Flat3D][D3D12] SceneDepthZ format {} unsupported for depth sampling", (uint32_t)sd_desc.Format);
        }
    }

    ctx.has_commands = true;
    ctx.execute();

    // Resilience: if a frame containing the depth readback failed to close,
    // permanently disable depth sampling for the session (auto-convergence /
    // adaptive crosshair degrade) rather than losing the composite entirely.
    if (ctx.last_close_failed && m_depth_copied[slot]) {
        m_depth_disabled = true;
        m_depth_copied[slot] = false;
        spdlog::error("[Flat3D][D3D12] depth readback invalidated the command list — disabling depth sampling "
                      "(auto-convergence / adaptive crosshair will use fallbacks)");
    }

    if (ctx.last_close_failed && m_coverage_copied[slot]) {
        m_coverage_disabled = true;
        m_coverage_copied[slot] = false;
        spdlog::error("[Flat3D][D3D12] coverage readback invalidated the command list — disabling "
                      "full-screen-GUI coverage detection");
    }

    ++m_frame;

    return true;
}

void Flat3DCompositorD3D12::read_depth_slot(uint32_t slot, float nearz_uu,
                                            float* out_center_uu, float* out_nearest_uu) {
    auto* buffer = m_depth_readback[slot].Get();

    if (buffer == nullptr || m_depth_roi_w == 0 || nearz_uu <= 0.0f) {
        return;
    }

    // Planar D32S8 reads as tightly packed 32-bit floats (plane 0 only).
    const bool is_r32 = m_depth_format == DXGI_FORMAT_R32G8X24_TYPELESS ||
                        m_depth_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                        m_depth_format == DXGI_FORMAT_R32_TYPELESS ||
                        m_depth_format == DXGI_FORMAT_R32_FLOAT || m_depth_format == DXGI_FORMAT_D32_FLOAT;

    void* data = nullptr;
    const D3D12_RANGE read_range{0, (SIZE_T)m_depth_row_pitch * kDepthStripes * kStripeRows};

    if (FAILED(buffer->Map(0, &read_range, &data))) {
        return;
    }

    const auto device_to_z = [&](float d) {
        if (d <= 1e-9f) {
            return 1e9f;
        }
        return std::min(nearz_uu / d, 1e9f);
    };

    // Samples essentially AT the near plane are not scene geometry — they are
    // camera-clipped polys or full-screen overlay/backdrop quads drawn at the
    // near plane (Expedition 33 dialogs). Feeding them into the nearest
    // percentile permanently saturates auto-convergence at its floor, so they
    // are excluded from the nearest estimate; the convergence floor already
    // covers the genuine camera-clip case.
    const float z_glue_uu = nearz_uu * 1.5f;

    std::vector<float> z_samples;
    z_samples.reserve((m_depth_roi_w / 4 + 1) * kDepthStripes);
    std::vector<float> center_samples;       // tiny box, crosshair depth
    std::vector<float> center_region_samples; // mid-screen, auto-convergence bias

    uint32_t total = 0;
    uint32_t far_rejected = 0;
    uint32_t glued = 0;
    float d_min = 1.0f;
    float d_max = 0.0f;

    const uint32_t center_stripe = kDepthStripes / 2;
    const uint32_t center_x = m_depth_roi_w / 2;
    // Aim-window half-width. Wide enough to span the stereo parallax band: we
    // read the LEFT-eye half, but a near object under the FUSED reticle sits at
    // eye-center only at the convergence depth — nearer geometry has crossed
    // disparity and shifts sideways in the left eye, so a center-only sample
    // reads the background behind it (the reticle only "pops" onto it when the
    // player aims off-center by the parallax). Sampling out to ~6% of the ROI
    // and taking the nearest catches the object wherever its disparity puts it.
    const uint32_t kAimHalfW = std::max<uint32_t>(12u, m_depth_roi_w / 16);
    // Central sub-region (mid ~40% of the ROI both axes): the aim target sits
    // here even though it is rarely the frame's global nearest object.
    const uint32_t cregion_x0 = (uint32_t)(m_depth_roi_w * 0.30f);
    const uint32_t cregion_x1 = (uint32_t)(m_depth_roi_w * 0.70f);

    for (uint32_t row = 0; row < kDepthStripes * kStripeRows; ++row) {
        const uint8_t* row_data = (const uint8_t*)data + (size_t)row * m_depth_row_pitch;
        const uint32_t stripe = row / kStripeRows;
        const bool is_center_region_row = stripe >= 3 && stripe <= 5; // mid ~third vertically

        for (uint32_t x = 0; x < m_depth_roi_w; x += 4) {
            const uint8_t* texel = row_data + (size_t)x * 4u;
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
        const uint32_t ax1 = std::min(center_x + kAimHalfW, m_depth_roi_w - 1);
        for (uint32_t r = 0; r < kStripeRows; ++r) {
            const uint32_t row = center_stripe * kStripeRows + r;
            const uint8_t* row_data = (const uint8_t*)data + (size_t)row * m_depth_row_pitch;
            for (uint32_t x = ax0; x <= ax1; ++x) {
                const uint8_t* texel = row_data + (size_t)x * 4u;
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

    const D3D12_RANGE no_write{0, 0};
    buffer->Unmap(0, &no_write);

    // Sample-data diagnostics: the raw picture behind the auto-convergence /
    // crosshair decisions. glued≈total means a full-screen near overlay (or a
    // wrong buffer); far≈total means a cleared/unrendered target.
    // Aim/crosshair depth: the NEAREST surface in the center window; the whole
    // reticle region then renders at that single depth. A tiny near percentile
    // (~5th) instead of the raw minimum rejects a lone near speck but still
    // pins the crosshair to the closest thing under the aim point rather than
    // the background behind a small target.
    float center_z_raw = -1.0f;
    if (!center_samples.empty()) {
        // 3rd-nearest of the dense aim window: rejects a lone 1-2px speck / edge
        // texel but lets a genuine small target win (a percentile that scales
        // with the sample count would need MORE coverage as density rises,
        // defeating the point — a small object covers only a few dense texels).
        const size_t ci = std::min<size_t>(2, center_samples.size() - 1);
        std::nth_element(center_samples.begin(), center_samples.begin() + ci, center_samples.end());
        center_z_raw = center_samples[ci];

        if (m_center_ema_uu <= 0.0f) {
            m_center_ema_uu = center_z_raw;
        } else if (std::fabs(center_z_raw - m_center_ema_uu) / m_center_ema_uu > 0.01f) {
            m_center_ema_uu += (center_z_raw - m_center_ema_uu) * 0.25f;
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

    // Sample-data diagnostics: the crosshair center depth and the
    // auto-convergence nearest alongside the ROI spread. glued≈total means a
    // full-screen near overlay (or a wrong buffer); far≈total means a
    // cleared/unrendered target; center ≫ p2 means the crosshair is grabbing
    // background behind a small aim target.
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

        spdlog::info("[Flat3D][depth-sample] nearz={:.3f}uu center(znear={:.1f} ema={:.1f}) nearest(p2)={:.1f} "
                     "ROI z(p2/p50/p98)={:.1f}/{:.1f}/{:.1f}uu samples={} far={} glued(z<{:.3f})={} kept={} "
                     "d=[{:.6f}..{:.6f}]",
                     nearz_uu, center_z_raw, m_center_ema_uu, nearest_raw, p2, p50, p98,
                     total, far_rejected, z_glue_uu, glued, z_samples.size(), d_min, d_max);
    }
}

void Flat3DCompositorD3D12::build_sbs(ID3D12GraphicsCommandList* cmd, const Flat3DFrameParams& params,
                                      uint32_t out_w, uint32_t out_h) {
    // The weaver input is built at DISPLAY resolution (upscale-then-weave):
    // the SR lenticular pattern is display-pixel-exact only when its input
    // matches the panel, regardless of the game's render resolution.
    if (m_sbs_tex == nullptr || m_sbs_w != out_w || m_sbs_h != out_h) {
        m_sbs_tex.Reset();

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = (uint64_t)out_w * 2;
        desc.Height = out_h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        // Concrete (non-typeless) format: the weaver already binds this via
        // view_format_for, and a typeless resource can't be read back by
        // ScreenGrab for the screenshot (fails with ERROR_NOT_SUPPORTED).
        desc.Format = view_format_for(m_eye_format);
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&m_sbs_tex)))) {
            spdlog::error("[Flat3D][D3D12] Failed to create SbS texture");
            m_sbs_tex.Reset();
            return;
        }

        m_sbs_w = out_w;
        m_sbs_h = out_h;
#ifdef UEVR_FLAT3D_HAS_LEIASR_DX12
        m_sr_input_bound = false; // weaver must rebind the recreated input
#endif

        if (m_sbs_rtv_heap == nullptr) {
            D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
            heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heap_desc.NumDescriptors = 1;

            if (FAILED(m_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_sbs_rtv_heap)))) {
                spdlog::error("[Flat3D][D3D12] Failed to create SbS RTV heap");
                m_sbs_tex.Reset();
                return;
            }
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = view_format_for(m_eye_format);
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_device->CreateRenderTargetView(m_sbs_tex.Get(), &rtv_desc, m_sbs_rtv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    if (m_sbs_pso == nullptr) {
        return;
    }

    // Render the pair with the repack shader in SBS mode instead of copying:
    // honors eye swap and applies the SDR color correction to the weaver
    // input (the weaver bypasses the backbuffer repack pass entirely).
    barrier(cmd, m_sbs_tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    cmd->SetPipelineState(m_sbs_pso.Get());
    auto rtv = m_sbs_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)(m_sbs_w * 2);
    viewport.Height = (float)m_sbs_h;
    viewport.MaxDepth = 1.0f;
    cmd->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{0, 0, (LONG)(m_sbs_w * 2), (LONG)m_sbs_h};
    cmd->RSSetScissorRects(1, &scissor);

    RepackConstants constants{};
    constants.out_size[0] = (int32_t)(m_sbs_w * 2);
    constants.out_size[1] = (int32_t)m_sbs_h;
    constants.mode = (int32_t)Flat3DOutputMode::SBS;
    constants.eye_swap = params.eye_swap ? 1 : 0;
    constants.colorspace = 0; // the weaver consumes SDR sRGB
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

    cmd->SetGraphicsRoot32BitConstants(0, 24, &constants, 0);
    cmd->SetGraphicsRootDescriptorTable(1, m_srv_heap->GetGPUDescriptorHandleForHeapStart()); // t0/t1 = eyes

    cmd->DrawInstanced(3, 1, 0, 0);

    barrier(cmd, m_sbs_tex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

bool Flat3DCompositorD3D12::save_screenshot(ID3D12CommandQueue* queue, const Flat3DFrameParams& params,
                                            const std::wstring& parallel_path, const std::wstring& crossview_path) {
    if (!m_ready || queue == nullptr || m_screenshot_pso == nullptr ||
        m_eye_tex[0] == nullptr || m_eye_tex[1] == nullptr || m_eye_w == 0 || m_eye_h == 0) {
        return false;
    }

    if (!m_screenshot_ctx.ready() && !m_screenshot_ctx.setup(L"Flat3D screenshot")) {
        spdlog::error("[Flat3D][D3D12] Screenshot command context setup failed");
        return false;
    }

    // The SbS is rendered with the DISPLAY repack shader (same colorspace /
    // src_srgb / correction the screen uses) into an 8-bit RTV that mirrors the
    // backbuffer's sRGB-ness. Rendering into the raw backbuffer format instead
    // washes 10-bit (R10G10B10A2) titles out in ScreenGrab's >8-bit WIC
    // conversion, while _SRGB backbuffers still get their sRGB encode here.
    const uint32_t ss_w = m_eye_w * 2;
    const uint32_t ss_h = m_eye_h;
    const DXGI_FORMAT ss_fmt = screenshot_8bit_format(m_backbuffer_format);

    if (m_screenshot_tex == nullptr || m_screenshot_w != ss_w || m_screenshot_h != ss_h ||
        m_screenshot_fmt != ss_fmt) {
        m_screenshot_tex.Reset();

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = ss_w;
        desc.Height = ss_h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = ss_fmt;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                     IID_PPV_ARGS(&m_screenshot_tex)))) {
            spdlog::error("[Flat3D][D3D12] Failed to create screenshot texture");
            m_screenshot_tex.Reset();
            return false;
        }

        if (m_screenshot_rtv_heap == nullptr) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = 1;
            if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_screenshot_rtv_heap)))) {
                spdlog::error("[Flat3D][D3D12] Failed to create screenshot RTV heap");
                m_screenshot_tex.Reset();
                return false;
            }
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = ss_fmt;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_device->CreateRenderTargetView(m_screenshot_tex.Get(), &rtv_desc,
                                         m_screenshot_rtv_heap->GetCPUDescriptorHandleForHeapStart());
        m_screenshot_w = ss_w;
        m_screenshot_h = ss_h;
        m_screenshot_fmt = ss_fmt;
    }

    // cross=false is the geometric left|right pair; cross=true swaps the halves
    // for cross-eyed viewing. Kept in RENDER_TARGET throughout (ScreenGrab
    // transitions to COPY_SOURCE and back).
    const auto build_and_save = [&](bool cross, const std::wstring& path) -> bool {
        if (path.empty()) {
            return true;
        }

        m_screenshot_ctx.wait(INFINITE); // resets the list into the recording state
        auto* cmd = m_screenshot_ctx.cmd_list.Get();

        ID3D12DescriptorHeap* heaps[] = {m_srv_heap.Get()};
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootSignature(m_root_sig.Get());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        cmd->SetPipelineState(m_screenshot_pso.Get());
        auto rtv = m_screenshot_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        D3D12_VIEWPORT vp{};
        vp.Width = (float)m_screenshot_w;
        vp.Height = (float)m_screenshot_h;
        vp.MaxDepth = 1.0f;
        cmd->RSSetViewports(1, &vp);
        D3D12_RECT sc{0, 0, (LONG)m_screenshot_w, (LONG)m_screenshot_h};
        cmd->RSSetScissorRects(1, &sc);

        // Mirror the display repack exactly (same PSO, colorspace, src_srgb,
        // correction) — only the mode is forced to SbS and eye_swap, taken
        // geometrically (eye0 = left), selects parallel/cross.
        RepackConstants constants{};
        constants.out_size[0] = (int32_t)m_screenshot_w;
        constants.out_size[1] = (int32_t)m_screenshot_h;
        constants.mode = (int32_t)Flat3DOutputMode::SBS;
        constants.eye_swap = cross ? 1 : 0;
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

        cmd->SetGraphicsRoot32BitConstants(0, 24, &constants, 0);
        cmd->SetGraphicsRootDescriptorTable(1, m_srv_heap->GetGPUDescriptorHandleForHeapStart()); // t0/t1 = eyes

        cmd->DrawInstanced(3, 1, 0, 0);

        m_screenshot_ctx.has_commands = true;
        m_screenshot_ctx.execute();
        m_screenshot_ctx.wait(INFINITE); // block until the render is on the GPU

        // 24bpp BGR (no alpha): the eye textures carry the game's backbuffer
        // alpha, which is meaningless and often ~0 (FF7 Rebirth's UI-alpha path
        // leaves it fully transparent) — an RGBA PNG then reads as all-black in
        // alpha-respecting viewers. forceSRGB=true tags the PNG sRGB WITHOUT
        // touching pixels; the 8-bit UNORM target otherwise gets a gAMA=1.0
        // (linear) stamp that makes color-managed viewers wash the shot out.
        const HRESULT hr = DirectX::SaveWICTextureToFile(
            queue, m_screenshot_tex.Get(), GUID_ContainerFormatPng, path.c_str(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
            &GUID_WICPixelFormat24bppBGR, nullptr, /*forceSRGB=*/true);
        if (FAILED(hr)) {
            spdlog::error("[Flat3D][D3D12] Screenshot encode failed (hr=0x{:x})", (uint32_t)hr);
            return false;
        }
        return true;
    };

    bool ok = build_and_save(false, parallel_path);
    ok = build_and_save(true, crossview_path) && ok;
    return ok;
}

bool Flat3DCompositorD3D12::weave_leiasr(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backbuffer,
                                         uint32_t out_w, uint32_t out_h, HWND hwnd) {
#ifdef UEVR_FLAT3D_HAS_LEIASR_DX12
    if (m_sbs_tex == nullptr) {
        return false;
    }

    // Resolve SR's window/monitor queries to physical pixels for the whole
    // create+weave interaction, so the weave maps to the full native panel
    // instead of a DPI-virtualized sub-region (top-left) under a non-per-
    // monitor-aware host game.
    leiasr::ScopedPerMonitorDpi dpi_guard{};

    if (m_sr == nullptr && !m_sr_attempted) {
        m_sr_attempted = true;

        // One call replaces context creation, weaver creation and
        // initialize(): SR-lib performs them in the required order (the
        // initialize MUST follow weaver creation or eye tracking silently never
        // starts), probes the delay-loaded SR DLLs before touching any SDK
        // entry point, and converts the SDK's exceptions — notably
        // ServerNotAvailableException when the service isn't running — into an
        // HRESULT. It also applies the weaver defaults (latency 1 frame, late
        // latching on).
        const HRESULT hr = SimulatedReality::CreateSRInterfaceDX12(m_device, hwnd, &m_sr);

        if (FAILED(hr) || m_sr == nullptr) {
            spdlog::warn("[Flat3D][D3D12] LeiaSR: CreateSRInterfaceDX12 failed (hr {:#x}) — falling back to SbS",
                         (uint32_t)hr);
            m_sr = nullptr;
            return false;
        }

        // Input is sRGB 8-bit from the engine; the weaver converts on read and
        // re-encodes on write.
        m_sr->SetShaderSRGBConversion(true, true);

        spdlog::info("[Flat3D][D3D12] LeiaSR weaver ready");
    }

    if (m_sr == nullptr) {
        return false;
    }

    if (!m_sr_input_bound) {
        // The FULL combined-SbS texture (2W x H): the weaver samples exactly
        // width x height texels and expects L in the left half, R in the right.
        // SR-lib reads the dimensions and format straight off the resource, so
        // it cannot be told a size the texture doesn't have.
        m_sr->SetInputTexture(m_sbs_tex.Get());
        m_sr->SetOutputFormat(view_format_for(m_backbuffer_format));
        m_sr_input_bound = true;
    }

    // The weaver records into cmd and draws to the currently bound RTV. The
    // effective viewport is the command list's rasterizer state — the eye/SbS
    // passes above left it at eye size, so reset it to swap-chain dims here
    // (the weaver's own setViewport alone is not sufficient).
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = view_format_for(m_backbuffer_format);
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    auto bb_rtv = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    bb_rtv.ptr += (size_t)2 * m_rtv_stride;
    m_device->CreateRenderTargetView(backbuffer, &rtv_desc, bb_rtv);

    barrier(cmd, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->OMSetRenderTargets(1, &bb_rtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)out_w;
    viewport.Height = (float)out_h;
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, (LONG)out_w, (LONG)out_h};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    try {
        // Weave() does SetCommandList + SetViewport + SetScissorRect itself.
        // The RSSetViewports above is still required and is NOT the same thing:
        // D3D12 rasterizes against the command list's own viewport state.
        m_sr->Weave(cmd, viewport, scissor);
    } catch (...) {
        // SR service crash / display unplug mid-session — disable and fall
        // back to SbS instead of taking the game down.
        spdlog::warn("[Flat3D][D3D12] LeiaSR: weave threw — disabling weaver");
        // Hand the lens back BEFORE Delete() destroys the context that owns it.
        set_leiasr_lens(false);
        m_sr->Delete(); // weaver then context, in that order
        m_sr = nullptr;
        m_sr_input_bound = false;
        barrier(cmd, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        return false;
    }

    barrier(cmd, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    return true;
#else
    (void)cmd; (void)backbuffer; (void)out_w; (void)out_h; (void)hwnd;
    return false;
#endif
}

void Flat3DCompositorD3D12::set_leiasr_lens(bool enabled) {
#ifdef UEVR_FLAT3D_HAS_LEIASR_DX12
    if (m_sr == nullptr) {
        return;
    }

    // Context-scoped, so it works off the context SR-lib created for our
    // interface — nothing to wire up beyond having an interface alive.
    const HRESULT hr = enabled ? SimulatedReality::SREnableLensHint()
                               : SimulatedReality::SRDisableLensHint();

    // S_FALSE = already in that state (the common per-frame case). Only a real
    // transition is worth a line; E_NOINTERFACE just means a fixed-lens panel.
    if (hr == S_OK) {
        spdlog::info("[Flat3D] LeiaSR: switchable lens {}", enabled ? "enabled" : "released");
    }
#else
    (void)enabled;
#endif
}

void Flat3DCompositorD3D12::destroy_leiasr() {
    // Release the lens while the interface — and so the SRContext that owns the
    // hint — is still alive. A game exit or device reset must not leave the
    // panel lensed for whatever runs next.
    set_leiasr_lens(false);

#ifdef UEVR_FLAT3D_HAS_LEIASR_DX12
    if (m_sr != nullptr) {
        // Delete() tears down the weaver and then the context, in that order,
        // and pairs SRContext::create with deleteSRContext (the SDK's matching
        // free — plain delete would free it on the wrong heap, and leaking it
        // leaves the SR service holding a session that degrades across restarts).
        m_sr->Delete();
        m_sr = nullptr;
    }
    m_sr_attempted = false;
    m_sr_input_bound = false;
#endif
    m_sbs_tex.Reset();
    m_sbs_rtv_heap.Reset();
}

void Flat3DCompositorD3D12::reset() {
    for (auto& ctx : m_cmds) {
        ctx.reset();
    }

    for (int i = 0; i < 2; ++i) {
        m_eye_tex[i].Reset();
    }

    m_pair_pending.Reset();
    m_pair_pending_valid = false;

    for (uint32_t i = 0; i < kRing; ++i) {
        m_depth_readback[i].Reset();
        m_depth_copied[i] = false;
        m_coverage_readback[i].Reset();
        m_coverage_copied[i] = false;
        m_anchor_cb[i].Reset(); // implicit unmap on release
        m_anchor_cb_ptr[i] = nullptr;
    }

    m_have_depth_srv = false;
    m_hud_depth_uscale = 1.0f;
    m_hud_mode_effective = 0;

    m_classify_pso.Reset();
    m_hud_mask[0].Reset();
    m_hud_mask[1].Reset();
    m_hud_mask_rtv_heap.Reset();
    m_hud_prev_ui.Reset();
    m_hud_prev_w = 0;
    m_hud_prev_h = 0;
    m_hud_prev_fmt = DXGI_FORMAT_UNKNOWN;
    m_mask_idx = 0;
    m_hud_prev_valid = false;

    m_huddepth_pso.Reset();
    m_huddepth_tex[0].Reset();
    m_huddepth_tex[1].Reset();
    m_huddepth_rtv_heap.Reset();
    m_huddepth_srv_made = false;

    destroy_leiasr();

    m_screenshot_ctx.reset();
    m_screenshot_tex.Reset();
    m_screenshot_rtv_heap.Reset();
    m_screenshot_pso.Reset();
    m_screenshot_w = 0;
    m_screenshot_h = 0;
    m_screenshot_fmt = DXGI_FORMAT_UNKNOWN;

    m_srv_heap.Reset();
    m_rtv_heap.Reset();
    m_root_sig.Reset();
    m_repack_pso.Reset();
    m_sbs_pso.Reset();
    m_overlay_pso.Reset();

    m_coverage_pso.Reset();
    m_coverage_rt.Reset();
    m_coverage_ema = 0.0f;
    m_coverage_disabled = false;

    m_depth_format = DXGI_FORMAT_UNKNOWN;
    m_depth_disabled = false;
    m_depth_roi_w = 0;
    m_depth_row_pitch = 0;
    m_frame = 0;
    m_center_ema_uu = -1.0f;
    m_depth_format_warned = false;

    m_device = nullptr;
    m_eye_w = 0;
    m_eye_h = 0;
    m_eye_format = DXGI_FORMAT_UNKNOWN;
    m_backbuffer_format = DXGI_FORMAT_UNKNOWN;
    m_colorspace = Flat3DColorSpace::SDR;
    m_src_srgb = false;
    m_ready = false;
}

} // namespace vrmod::flat3d
