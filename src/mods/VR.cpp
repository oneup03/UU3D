#define NOMINMAX

#include <fstream>

#include <windows.h>
#include <dbt.h>

#include <imgui.h>
#include <utility/Module.hpp>
#include <utility/Registry.hpp>
#include <utility/ScopeGuard.hpp>

#include <sdk/Globals.hpp>
#include <sdk/CVar.hpp>
#include <sdk/threading/GameThreadWorker.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <sdk/APlayerController.hpp>

#include <tracy/Tracy.hpp>

#include "Framework.hpp"
#include "frameworkConfig.hpp"

#include "utility/Logging.hpp"

#include "VR.hpp"
#include <safetyhook.hpp>

NVSDK_NGX_Result hk_NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Feature InFeatureID, NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Handle** OutHandle) {
    spdlog::info("hk_NVSDK_NGX_D3D12_CreateFeature FeatureID {}", (int)InFeatureID);
    auto result = NVSDK_NGX_D3D12_CreateFeature_Hook.call<NVSDK_NGX_Result>(InCmdList, InFeatureID, InParameters, OutHandle);
    const auto& vr = VR::get();
    int flag;
    InParameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flag);
    spdlog::info("hk_NVSDK_NGX_D3D12_CreateFeature 0x{0:x} flag:0x{0:x}", (INT64)result, (INT64)flag);
    if ((InFeatureID != NVSDK_NGX_Feature_SuperSampling && InFeatureID != NVSDK_NGX_Feature_RayReconstruction)) {
        vr->vrNoneDLSSHandleMap[*OutHandle] = InFeatureID;
    }
    return result;
}

NVSDK_NGX_Result hk_NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* InHandle) {
    spdlog::info("hk_NVSDK_NGX_D3D12_ReleaseFeature Starts");
    auto result = NVSDK_NGX_D3D12_ReleaseFeature_Hook.call<NVSDK_NGX_Result>(InHandle);
    spdlog::info("hk_NVSDK_NGX_D3D12_ReleaseFeature 0x{0:x}", (INT64)result);
    const auto& vr = VR::get();
    if (vr->vrNoneDLSSHandleMap.contains(InHandle))
        vr->vrNoneDLSSHandleMap.erase(InHandle);
    return result;
}

static std::thread::id RHIThreadID = {};
NVSDK_NGX_Result hk_NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList* InCmdList, const NVSDK_NGX_Handle* InFeatureHandle, NVSDK_NGX_Parameter* InParameters, void* InCallback) {
    const auto& vr = VR::get();
    if (!vr->vrNoneDLSSHandleMap.contains((NVSDK_NGX_Handle*)InFeatureHandle)) {
        ID3D12Resource* color;
        ID3D12Resource* depth;
        ID3D12Resource* motionVectors;
        ID3D12Resource* output;
        float mvScale[2] = {1.0, 1.0};
        InParameters->Get(NVSDK_NGX_Parameter_Color, &color);
        InParameters->Get(NVSDK_NGX_Parameter_Depth, &depth);
        InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &motionVectors);
        InParameters->Get(NVSDK_NGX_Parameter_Output, &output);
        InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScale[0]);
        InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScale[1]);
        InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &vr->jitterOffset[0]);
        InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &vr->jitterOffset[1]);
        if (vr->rawDepthTex != depth) {
            SAFE_RELEASE(vr->rawDepthTex);
            vr->rawDepthTex = depth;
            vr->rawDepthTex->AddRef();
        }
        if (vr->rawMotionVectorsTex != motionVectors) {
            SAFE_RELEASE(vr->rawMotionVectorsTex);
            vr->rawMotionVectorsTex = motionVectors;
            vr->rawMotionVectorsTex->AddRef();
        }
        if (output && motionVectors) {
            auto mvDesc = motionVectors->GetDesc();
            auto outputDesc = output->GetDesc();
            vr->mvScale[0] = mvScale[0] * outputDesc.Width / mvDesc.Width;
            vr->mvScale[1] = mvScale[1] * outputDesc.Height / mvDesc.Height;
        }
        if (depth) {
            auto depthDesc = depth->GetDesc();
            if (vr->renderSize[0] != depthDesc.Width || vr->renderSize[1] != depthDesc.Height) {
                vr->renderSize[0] = depthDesc.Width;
                vr->renderSize[1] = depthDesc.Height;
                vr->afw_resolution_change_skip_frames = 90;
            }
        }
        RHIThreadID = std::this_thread::get_id();
        auto render_frame_count = vr->get_render_frame_count();
        // Same eye-parity expression as run_flat3d_framewarp / update_camera_data /
        // the compositors — a hardcoded ==0 here swaps the harvest's eye slots
        // whenever m_left_eye_interval is 1.
        EyeIndex nEye = (render_frame_count % 2 == vr->get_left_eye_interval()) ? EyeLeft : EyeRight;
        EyeIndex nEyeOther = (render_frame_count % 2 == vr->get_left_eye_interval()) ? EyeRight : EyeLeft;
        if (render_frame_count - vr->last_dlss_frame_count > 2)
            vr->dlss_continue_frame_count = 0;
        vr->last_dlss_frame_count = render_frame_count;
        vr->dlss_continue_frame_count++;
        static int lastPausedFrame = render_frame_count;

        // Flat3D "DLSS Depth" source: snapshot the DLSS input depth into our OWN
        // per-eye copy on the game's command list. Plugin-free (uses the game device
        // + CopyResource), and independent of the rendering method — so it works in
        // Native / Synced / AFR, not just AFW.
        if (vr->flat3d_wants_dlss_depth() && depth) {
            vr->capture_dlss_depth_copy(InCmdList, depth, (int)nEye);
        }

        // --- AFW (plugin) depth + motion-vector harvest -----------------------
        // depthDesc/motionVectorsDesc are allocated by the AFW warp block and copied
        // here via the plugin's D3D12 renderer; requires the real PDAFWPlugin.
        bool bufferValid = vr->is_hmd_active() && motionVectors && vr->motionVectorsDesc[nEye].pTexture && vr->depthDesc[nEye].pTexture;
        if (!bufferValid)
            lastPausedFrame = render_frame_count;
        if (lastPausedFrame > render_frame_count)
            lastPausedFrame = render_frame_count;
        if (vr->is_using_afw() && vr->d3d12Renderer != nullptr && vr->afw_resolution_change_skip_frames <= 0 &&
            (render_frame_count - lastPausedFrame > 30) && bufferValid) {
            TextureDesc src;
            src.pTexture = depth;
            src.initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            vr->d3d12Renderer->Copy(InCmdList, vr->depthDesc[nEye], src);
            if (motionVectors && vr->rawMVDesc[nEye].pTexture != motionVectors) {
                vr->rawMVDesc[nEye].pTexture = motionVectors;
                vr->rawMVDesc[nEye].initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                vr->d3d12Renderer->SetupTextureDesc(vr->rawMVDesc[nEye]);
            }
            if (vr->is_ghosting_fix_enabled() && vr->is_fix_object_motion_vector() &&
                vr->rawVelocityDesc[nEye].pTexture && vr->rawVelocityDesc[nEyeOther].pTexture) {
                if (vr->rawMVDesc[nEye].pTexture && vr->motionVectorsDesc[nEye].pTexture) {
                    vr->update_camera_data(render_frame_count);
                    auto inMVDesc = vr->rawVelocityDesc[nEye].pTexture->GetDesc();
                    auto outMVDesc = vr->rawMVDesc[nEye].pTexture->GetDesc();
                    CorrectMotionVectorsParams mvParams;
                    mvParams.InMotionVectors = &vr->rawVelocityDesc[nEye];
                    mvParams.InDepth = &vr->depthDesc[nEye];
                    mvParams.CameraData = &vr->cameraDataForMV[nEye];
                    mvParams.InMotionScale[0] = mvScale[0];
                    mvParams.InMotionScale[1] = mvScale[1];
                    mvParams.CorrectMVType = FixUEObjectMotion;
                    mvParams.ObjectMotionScale = 2.0f;
                    mvParams.FixUEObjMotionRange = vr->get_fix_object_motion_range();
                    mvParams.IgnoreMotionThreshold = vr->get_ignore_motion_threshold();
                    mvParams.InUEVelocityPrev = &vr->rawVelocityDesc[nEyeOther];
                    mvParams.InDepthPrev = &vr->depthDesc[nEyeOther];
                    vr->d3d12Renderer->CorrectMotionVectors(InCmdList, vr->rawMVDesc[nEye], mvParams);
                    vr->d3d12Renderer->Copy(InCmdList, vr->motionVectorsDesc[nEye], vr->rawMVDesc[nEye]);
                }
            } else {
                src.pTexture = motionVectors;
                src.initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                vr->d3d12Renderer->Copy(InCmdList, vr->motionVectorsDesc[nEye], src);
            }
        }
        if (vr->is_renderdoc && vr->d3d12Renderer != nullptr) {
            static TextureDesc colorDesc[2];
            static TextureDesc outputDesc[2];
            if (color && colorDesc[nEye].pTexture != color) {
                colorDesc[nEye].pTexture = color;
                colorDesc[nEye].initialState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                vr->d3d12Renderer->SetupTextureDesc(colorDesc[nEye]);
            }
            if (output && outputDesc[nEye].pTexture != output) {
                outputDesc[nEye].pTexture = output;
                outputDesc[nEye].initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                vr->d3d12Renderer->SetupTextureDesc(outputDesc[nEye]);
            }

            vr->d3d12Renderer->Blit(InCmdList, outputDesc[nEye], colorDesc[nEye], {}, NoBlend, true);
            return NVSDK_NGX_Result_Success;
        }
    }
    if (!InFeatureHandle)
        return NVSDK_NGX_Result_Success;
    auto result = NVSDK_NGX_D3D12_EvaluateFeature_Hook.call<NVSDK_NGX_Result>(InCmdList, InFeatureHandle, InParameters, InCallback);
    return result;
}

// AFW/NeverDLSS raw harvest. Inline-hooked on the command list's function body
// (not a second patch of vtable slot 26): a vtable patch here and D3D12Hook's
// PointerHook on the same slot each captured the other as "original" after a
// re-hook, recursing every ResourceBarrier to a stack overflow (P3R crash).
// An inline hook patches the function itself, so it composes instead.
static SafetyHookInline ResourceBarrier_Hook{};
void WINAPI hk_ID3D12GraphicsCommandList_ResourceBarrier(ID3D12GraphicsCommandList* This, UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {
    ResourceBarrier_Hook.call(This, NumBarriers, pBarriers);
    const auto& vr = VR::get();

    // Only track barriers submitted in RHISubmissionThread
    // Unless there's no RHISubmissionThread
    auto threadID = std::this_thread::get_id();
    bool isRHIThread = RHIThreadID == threadID;
    static bool skip = false;
    if (!vr->is_using_afw() || skip)
        return;
    static int lastRHIThreadFoundFrame = 0;
    static int lastRHISubmissionThreadFoundFrame = 0;

    ID3D12Resource* velocityCandidate = nullptr;
    ID3D12Resource* motionVectorsCandidate = nullptr;
    auto render_frame_count = vr->get_render_frame_count();
    EyeIndex nEye = (render_frame_count % 2 == vr->get_left_eye_interval()) ? EyeLeft : EyeRight;
    bool isNeverDLSS = vr->is_never_dlss();
    for (int i = 0; i < NumBarriers; i++) {
        auto& barrier = pBarriers[i];
        if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !barrier.Transition.pResource || 
            vr->rawVelocityDesc[nEye].pTexture == barrier.Transition.pResource ||
            vr->rawMVDesc[nEye].pTexture == barrier.Transition.pResource)
            continue;
        auto desc = barrier.Transition.pResource->GetDesc();
        if (desc.Format == DXGI_FORMAT_R16G16B16A16_UNORM) {
            if ((barrier.Transition.StateAfter & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
                (barrier.Transition.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET || barrier.Transition.StateBefore == D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)) {
                if ((desc.Width == vr->renderSize[0] || vr->renderSize[0] == 0) &&
                    (desc.Height == vr->renderSize[1] || vr->renderSize[1] == 0)) {
                    velocityCandidate = barrier.Transition.pResource;
                }
            }
        } else if (isNeverDLSS && desc.Format == DXGI_FORMAT_R16G16_FLOAT) {
            if (barrier.Transition.StateAfter == D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE &&
                barrier.Transition.StateBefore == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                if ((desc.Width == vr->renderSize[0] || vr->renderSize[0] == 0) &&
                    (desc.Height == vr->renderSize[1] || vr->renderSize[1] == 0)) {
                    motionVectorsCandidate = barrier.Transition.pResource;
                    vr->mvScale[0] = 1.0f * vr->finalSize[0];
                    vr->mvScale[1] = 1.0f * vr->finalSize[1];
                }
            }
        }
    }
    if (velocityCandidate || motionVectorsCandidate) {
        if (isRHIThread)
            lastRHIThreadFoundFrame = render_frame_count;
        else
            lastRHISubmissionThreadFoundFrame = render_frame_count;

        bool isRHIThreadFoundRecently = render_frame_count - lastRHIThreadFoundFrame <= 100;
        bool isRHISubmissionThreadFoundRecently = render_frame_count - lastRHISubmissionThreadFoundFrame <= 100;
        bool RHIThreadPass = isRHIThread && !isRHISubmissionThreadFoundRecently;
        bool RHISubmissionThreadPass = !isRHIThread;
        if (RHIThreadPass || RHISubmissionThreadPass) {
            if (velocityCandidate && vr->is_ghosting_fix_enabled() && vr->is_fix_object_motion_vector() &&
                (render_frame_count - vr->last_dlss_frame_count) <= 1) {
                auto desc = velocityCandidate->GetDesc();
                if (vr->rawVelocityDesc[nEye].pTexture == NULL || vr->rawVelocityDesc[nEye].pTexture->GetDesc().Width != desc.Width ||
                    vr->rawVelocityDesc[nEye].pTexture->GetDesc().Height != desc.Height) {
                    vr->d3d12Renderer->CreateTexture(
                        desc.Width, desc.Height, desc.Format, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, vr->rawVelocityDesc[nEye], true);
                }
                static std::map<ID3D12Resource*, TextureDesc> rawVelocityDescMap;
                if (!rawVelocityDescMap.contains(velocityCandidate)) {
                    rawVelocityDescMap[velocityCandidate].pTexture = velocityCandidate;
                    rawVelocityDescMap[velocityCandidate].initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    vr->d3d12Renderer->SetupTextureDesc(rawVelocityDescMap[velocityCandidate]);
                    // velocityCandidate->SetName(L"VelocityBuffer");
                }
                skip = true;
                vr->d3d12Renderer->Copy(This, vr->rawVelocityDesc[nEye], rawVelocityDescMap[velocityCandidate]);
                skip = false;
            }
            if (motionVectorsCandidate) {
                auto desc = motionVectorsCandidate->GetDesc();
                if (vr->rawMotionVectorsTex != motionVectorsCandidate) {
                    SAFE_RELEASE(vr->rawMotionVectorsTex);
                    vr->rawMotionVectorsTex = motionVectorsCandidate;
                    vr->rawMotionVectorsTex->AddRef();
                }
                if (vr->motionVectorsDesc[nEye].pTexture) {
                    TextureDesc src;
                    src.pTexture = motionVectorsCandidate;
                    src.initialState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                    skip = true;
                    vr->d3d12Renderer->Copy(This, vr->motionVectorsDesc[nEye], src);
                    skip = false;
                }
            }
        }
    }
}

static std::map<SIZE_T, ID3D12Resource*> DSVMap = {};
void WINAPI hk_ID3D12Device_CreateDepthStencilView(
    ID3D12Device* This, ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    DSVMap[DestDescriptor.ptr] = pResource;
}

static SafetyHookInline ClearDepthStencilView_Hook{};
void WINAPI hk_ID3D12GraphicsCommandList_ClearDepthStencilView(ID3D12GraphicsCommandList* This,
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) {

    ClearDepthStencilView_Hook.call(This, DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);

    const auto& vr = VR::get();

    if (ClearFlags != D3D12_CLEAR_FLAG_STENCIL || !vr->is_hmd_active())
        return;

    auto render_frame_count = vr->get_render_frame_count();
    bool isNeverDLSS = vr->is_never_dlss();
    EyeIndex nEye = (render_frame_count % 2 == vr->get_left_eye_interval()) ? EyeLeft : EyeRight;
    if (isNeverDLSS && DSVMap.contains(DepthStencilView.ptr)) {
        auto depth = DSVMap[DepthStencilView.ptr];
        auto desc = depth->GetDesc();
        float aspectRatioX = float(desc.Width) / vr->finalSize[0];
        float aspectRatioY = float(desc.Height) / vr->finalSize[1];
        if (abs(aspectRatioX - aspectRatioY) < 0.01 && 
            (abs(aspectRatioX - 0.333) < 0.01 || abs(aspectRatioX - 0.5) < 0.01 ||
            abs(aspectRatioX - 0.58) < 0.01 || abs(aspectRatioX - 0.666) < 0.01) ||
            abs(aspectRatioX - 0.777) < 0.01 || abs(aspectRatioX - 1.0) < 0.01) {
            RHIThreadID = std::this_thread::get_id();
            if (vr->rawDepthTex != depth) {
                SAFE_RELEASE(vr->rawDepthTex);
                vr->rawDepthTex = depth;
                vr->rawDepthTex->AddRef();
            }
            if (depth) {
                auto depthDesc = depth->GetDesc();
                vr->renderSize[0] = depthDesc.Width;
                vr->renderSize[1] = depthDesc.Height;
            }
            if (vr->depthDesc[nEye].pTexture) {
                TextureDesc src;
                src.pTexture = depth;
                src.initialState = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                vr->d3d12Renderer->Copy(This, vr->depthDesc[nEye], src);
            }
        }
    }
}

std::shared_ptr<VR>& VR::get() {
    //static std::shared_ptr<VR> instance = std::make_shared<VR>();
    return g_framework->vr();
}

// Called when the mod is initialized
void VR::init_framewarp_module() {
    if (!g_framework->is_dx12()) {
        return;
    }
    if (m_framewarp_device_initialized && m_ngx_hooks_installed) {
        return; // fully initialized
    }

    auto& hook = g_framework->get_d3d12_hook();
    if (hook == nullptr || hook->get_device() == nullptr || hook->get_command_queue() == nullptr) {
        return; // D3D12 not ready yet; retried next frame
    }

    // --- Plugin device + raw D3D12 harvest hooks (once) --------------------
    if (!m_framewarp_device_initialized) {
        if (GetModuleHandleW(L"PDAFWPlugin.dll") == nullptr) {
            const auto current_path = utility::get_module_directoryw(GetModuleHandleW(L"UEVRBackend.dll"));
            if (current_path) {
                auto fspath = std::filesystem::path{*current_path} / L"PDAFWPlugin.dll";
                if (LoadLibraryW(fspath.c_str()) == nullptr) {
                    spdlog::info("[VR][AFW] Could not load PDAFWPlugin.dll (AFW will be unavailable)");
                }
            }
        }

        is_renderdoc = GetModuleHandleW(L"renderdoc.dll") != nullptr;

        pd::DeviceParams params{};
        params.d3d12Device = hook->get_device();
        params.d3d12Queue = hook->get_command_queue();
        d3d12Renderer = InitDevice(params);

        if (d3d12Renderer == nullptr) {
            // No real plugin (the shipped no-op dummy returns null, or the DLL is
            // absent). AFW itself and the NeverDLSS raw hooks (which record on the
            // plugin's command list) stay disabled — but the DLSS/NGX depth harvest
            // does NOT need the plugin, so we still fall through and install it so
            // the Flat3D "DLSS Depth" source works via our own copy.
            spdlog::warn("[VR][AFW] PDAFWPlugin InitDevice returned null; AFW disabled "
                         "(drop the real PDAFWPlugin.dll beside UEVRBackend.dll for AFW). "
                         "DLSS Depth capture still works via our own copy.");
        } else {
            // CreateDepthStencilView stays a D3D12Hook post-original callback.
            // Upstream still patches device vtable slot 21 directly, which goes
            // mutually recursive with D3D12Hook's own PointerHook on that slot
            // after a re-hook (stack overflow in P3R).
            D3D12Hook::s_on_raw_create_depth_stencil_view.store(&hk_ID3D12Device_CreateDepthStencilView, std::memory_order_release);

            // ResourceBarrier / ClearDepthStencilView are inline-hooked on the
            // command list's function bodies rather than its vtable slots, so
            // they compose with D3D12Hook's PointerHooks instead of recursing.
            // This is upstream's mechanism (fixes the harvest not firing in Halo
            // Campaign Evolved, where the vtable patch never saw the barriers).
            if (auto* cmd_list = d3d12Renderer->BeginCommandList(0); cmd_list != nullptr) {
                auto* const vtable = *(uintptr_t**)cmd_list;

                if (auto rb = safetyhook::InlineHook::create(
                        (LPVOID)vtable[26], reinterpret_cast<void*>(hk_ID3D12GraphicsCommandList_ResourceBarrier))) {
                    ResourceBarrier_Hook = std::move(rb.value());
                } else {
                    spdlog::error("[VR][AFW] Hook ID3D12GraphicsCommandList::ResourceBarrier failed: {}",
                                  (INT)rb.error().type);
                }

                if (auto cd = safetyhook::InlineHook::create(
                        (LPVOID)vtable[47], reinterpret_cast<void*>(hk_ID3D12GraphicsCommandList_ClearDepthStencilView))) {
                    ClearDepthStencilView_Hook = std::move(cd.value());
                } else {
                    spdlog::error("[VR][AFW] Hook ID3D12GraphicsCommandList::ClearDepthStencilView failed: {}",
                                  (INT)cd.error().type);
                }

                d3d12Renderer->EndCommandList(0);
            } else {
                spdlog::error("[VR][AFW] Could not obtain a plugin command list; raw depth harvest disabled");
            }

            spdlog::info("[VR][AFW] Frame Warp device initialized");
        }

        m_framewarp_device_initialized = true;
    }

    // --- DLSS / NGX harvest hooks (retry until nvngx.dll is loaded) --------
    // nvngx loads lazily when the game first initializes DLSS, which can be well
    // after this mod initializes — so this is retried each frame from
    // on_pre_engine_tick until it succeeds.
    if (!m_ngx_hooks_installed) {
        auto dllNGX = GetModuleHandle("_nvngx.dll");
        if (!dllNGX) {
            dllNGX = GetModuleHandle("nvngx.dll");
        }
        // OptiScaler ships as a local dxgi.dll or winmm.dll exporting the NGX
        // entry points; prefer it over nvngx.dll when present (upstream parity).
        for (const char* opti_name : {"dxgi.dll", "winmm.dll"}) {
            const auto dllOpti = GetModuleHandle(opti_name);
            if (dllOpti != nullptr && GetProcAddress(dllOpti, "NVSDK_NGX_D3D12_CreateFeature") != nullptr) {
                dllNGX = dllOpti;
                spdlog::info("[VR][AFW] OptiScaler detected ({}), hooking it instead of nvngx.dll", opti_name);
                break;
            }
        }
        if (!dllNGX) {
            return; // DLSS not initialized by the game yet
        }

        auto result = safetyhook::InlineHook::create(
            GetProcAddress(dllNGX, "NVSDK_NGX_D3D12_CreateFeature"), reinterpret_cast<void*>(hk_NVSDK_NGX_D3D12_CreateFeature));
        if (!result) {
            spdlog::error("[VR][AFW] Hook NVSDK_NGX_D3D12_CreateFeature failed: {}", (INT)result.error().type);
            return;
        }
        NVSDK_NGX_D3D12_CreateFeature_Hook = std::move(result.value());

        result = safetyhook::InlineHook::create(
            GetProcAddress(dllNGX, "NVSDK_NGX_D3D12_ReleaseFeature"), reinterpret_cast<void*>(hk_NVSDK_NGX_D3D12_ReleaseFeature));
        if (!result) {
            spdlog::error("[VR][AFW] Hook NVSDK_NGX_D3D12_ReleaseFeature failed: {}", (INT)result.error().type);
            return;
        }
        NVSDK_NGX_D3D12_ReleaseFeature_Hook = std::move(result.value());

        result = safetyhook::InlineHook::create(
            GetProcAddress(dllNGX, "NVSDK_NGX_D3D12_EvaluateFeature"), reinterpret_cast<void*>(hk_NVSDK_NGX_D3D12_EvaluateFeature));
        if (!result) {
            spdlog::error("[VR][AFW] Hook NVSDK_NGX_D3D12_EvaluateFeature failed: {}", (INT)result.error().type);
            return;
        }
        NVSDK_NGX_D3D12_EvaluateFeature_Hook = std::move(result.value());

        m_ngx_hooks_installed = true;
        spdlog::info("[VR][AFW] DLSS/NGX harvest hooks installed (nvngx.dll found)");
    }
}

void VR::capture_dlss_depth_copy(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* depth, int eye) {
    if (cmd_list == nullptr || depth == nullptr || eye < 0 || eye > 1) {
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();
    if (hook == nullptr) {
        return;
    }
    auto* device = hook->get_device();
    if (device == nullptr) {
        return;
    }

    const auto src_desc = depth->GetDesc();
    // The DLSS input depth is always a plain 2D, single-sample, shader-readable
    // texture. Bail on anything unexpected rather than issue a bad copy/barrier.
    if (src_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || src_desc.SampleDesc.Count != 1 ||
        src_desc.Width == 0 || src_desc.Height == 0 ||
        (src_desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
        return;
    }

    std::scoped_lock lock(m_dlss_depth_mutex);

    // (Re)allocate the owned copy to match the source (a plain shader-readable copy
    // — strip DSV/RTV/UAV/deny flags but keep the format/extent so CopyResource is
    // an exact whole-resource copy).
    bool need_alloc = m_dlss_depth[eye] == nullptr;
    if (!need_alloc) {
        const auto cur = m_dlss_depth[eye]->GetDesc();
        need_alloc = cur.Width != src_desc.Width || cur.Height != src_desc.Height ||
                     cur.Format != src_desc.Format || cur.DepthOrArraySize != src_desc.DepthOrArraySize ||
                     cur.MipLevels != src_desc.MipLevels;
    }
    if (need_alloc) {
        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        auto dst_desc = src_desc;
        dst_desc.Flags &= ~(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
        dst_desc.Alignment = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &dst_desc,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&tex)))) {
            spdlog::warn("[VR][DLSS depth] Could not allocate the owned depth copy");
            return;
        }
        tex->SetName(L"Flat3D DLSS Depth Copy");
        m_dlss_depth[eye] = std::move(tex);
    }

    // The DLSS input depth is passed in NON_PIXEL_SHADER_RESOURCE (DLSS samples it).
    // Round-trip through COPY_SOURCE and restore it before the copy list is closed,
    // leaving our owned copy shader-readable for the Flat3D compositor.
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (auto& b : barriers) {
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    barriers[0].Transition.pResource = depth;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.pResource = m_dlss_depth[eye].Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cmd_list->ResourceBarrier(2, barriers);

    cmd_list->CopyResource(m_dlss_depth[eye].Get(), depth);

    std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
    std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
    cmd_list->ResourceBarrier(2, barriers);
}

Microsoft::WRL::ComPtr<ID3D12Resource> VR::get_dlss_depth_copy(int eye) {
    if (eye < 0 || eye > 1) {
        return nullptr;
    }
    std::scoped_lock lock(m_dlss_depth_mutex);
    return m_dlss_depth[eye];
}

std::optional<std::string> VR::clean_initialize() try {
    ZoneScopedN(__FUNCTION__);

    // Flat 3D display mode takes priority over the HMD runtimes when the
    // frontend requests it. No VR API/DLL is required for it.
    if (m_requested_runtime_name->value() == "flat3d") {
        auto flat3d_error = initialize_flat3d();

        if (!flat3d_error && m_flat3d->loaded) {
            m_openvr->is_hmd_active = false;
            m_openvr->was_hmd_active = false;
            m_openvr->needs_pose_update = false;
            m_openxr->needs_pose_update = false;

            // AFW works under Flat3D too: install the plugin device + DLSS/NGX +
            // raw D3D12 harvest hooks here, since this path returns before the
            // normal Frame Warp init below. Retryable, so a late-loading nvngx.dll
            // (DLSS) still gets hooked from the per-frame retry in on_present.
            init_framewarp_module();

            m_init_finished = true;
            return Mod::on_initialize();
        }

        if (flat3d_error) {
            spdlog::error("Flat3D failed to initialize: {}", *flat3d_error);
        }
        // fall through to the HMD runtimes
    }

    auto openvr_error = initialize_openvr();

    if (openvr_error || !m_openvr->loaded) {
        if (m_openvr->error) {
            spdlog::info("OpenVR failed to load: {}", *m_openvr->error);
        }

        m_openvr->is_hmd_active = false;
        m_openvr->was_hmd_active = false;
        m_openvr->needs_pose_update = false;

        // Attempt to load OpenXR instead
        auto openxr_error = initialize_openxr();

        if (openxr_error || !m_openxr->loaded) {
            m_openxr->needs_pose_update = false;
        }
    } else {
        m_openxr->error = "OpenVR loaded first.";
    }

    if (!get_runtime()->loaded) {
        // this is okay. we're not going to fail the whole thing entirely
        // so we're just going to return OK, but
        // when the VR mod draws its menu, it'll say "VR is not available"
        return Mod::on_initialize();
    }

    // Check whether the user has Hardware accelerated GPU scheduling enabled
    const auto hw_schedule_value = utility::get_registry_dword(
        HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
        "HwSchMode");

    if (hw_schedule_value) {
        m_has_hw_scheduling = *hw_schedule_value == 2;
    }

    m_init_finished = true;

    // AFW (Async Frame Warp): plugin device + DLSS/NGX + raw D3D12 harvest hooks.
    // Retryable and dummy-plugin-safe; also runs for Flat3D (see the flat3d branch
    // above). Defined in init_framewarp_module().
    init_framewarp_module();

    // all OK
    return Mod::on_initialize();
} catch(...) {
    spdlog::error("Exception occurred in VR::on_initialize()");

    m_runtime->error = "Exception occurred in VR::on_initialize()";
    m_openxr->dll_missing = false;
    m_openvr->dll_missing = false;
    m_openxr->error = "Exception occurred in VR::on_initialize()";
    m_openvr->error = "Exception occurred in VR::on_initialize()";
    m_openvr->loaded = false;
    m_openvr->is_hmd_active = false;
    m_openxr->loaded = false;
    m_init_finished = false;

    return Mod::on_initialize();
}

std::optional<std::string> VR::initialize_openvr() {
    ZoneScopedN(__FUNCTION__);

    spdlog::info("Attempting to load OpenVR");

    m_openvr = std::make_shared<runtimes::OpenVR>();
    m_openvr->loaded = false;

    const auto wants_openxr = m_requested_runtime_name->value() == "openxr_loader.dll";

    SPDLOG_INFO("[VR] Requested runtime: {}", m_requested_runtime_name->value());

    if (wants_openxr && GetModuleHandleW(L"openxr_loader.dll") != nullptr) {
        // pre-injected
        m_openvr->dll_missing = true;
        m_openvr->error = "OpenXR already loaded";
        return Mod::on_initialize();
    }

    if (GetModuleHandleW(L"openvr_api.dll") == nullptr) {
        // pre-injected
        if (GetModuleHandleW(L"openxr_loader.dll") != nullptr) {
            m_openvr->dll_missing = true;
            m_openvr->error = "OpenXR already loaded";
            return Mod::on_initialize();
        }


        if (utility::load_module_from_current_directory(L"openvr_api.dll") == nullptr) {
            spdlog::info("[VR] Could not load openvr_api.dll");

            m_openvr->dll_missing = true;
            m_openvr->error = "Could not load openvr_api.dll";
            return Mod::on_initialize();
        }
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    m_openvr->needs_pose_update = true;
    m_openvr->got_first_poses = false;
    m_openvr->is_hmd_active = true;
    m_openvr->was_hmd_active = true;

    spdlog::info("Attempting to call vr::VR_Init");

    auto error = vr::VRInitError_None;
	m_openvr->hmd = vr::VR_Init(&error, vr::VRApplication_Scene);

    // check if error
    if (error != vr::VRInitError_None) {
        m_openvr->error = "VR_Init failed: " + std::string{vr::VR_GetVRInitErrorAsEnglishDescription(error)};
        return Mod::on_initialize();
    }

    if (m_openvr->hmd == nullptr) {
        m_openvr->error = "VR_Init failed: HMD is null";
        return Mod::on_initialize();
    }

    // get render target size
    m_openvr->update_render_target_size();

    if (vr::VRCompositor() == nullptr) {
        m_openvr->error = "VRCompositor failed to initialize.";
        return Mod::on_initialize();
    }

    auto input_error = initialize_openvr_input();

    if (input_error) {
        m_openvr->error = *input_error;
        return Mod::on_initialize();
    }

    auto overlay_error = m_overlay_component.on_initialize_openvr();

    if (overlay_error) {
        m_openvr->error = *overlay_error;
        return Mod::on_initialize();
    }
    
    m_openvr->loaded = true;
    m_openvr->error = std::nullopt;
    m_runtime = m_openvr;

    return Mod::on_initialize();
}

std::optional<std::string> VR::initialize_openvr_input() {
    ZoneScopedN(__FUNCTION__);

    const auto module_directory = Framework::get_persistent_dir();

    // write default actions and bindings with the static strings we have
    for (auto& it : m_binding_files) {
        spdlog::info("Writing default binding file {}", it.first);

        std::ofstream file{ module_directory / it.first };
        file << it.second;
    }

    const auto actions_path = module_directory / "actions.json";
    auto input_error = vr::VRInput()->SetActionManifestPath(actions_path.string().c_str());

    if (input_error != vr::VRInputError_None) {
        return "VRInput failed to set action manifest path: " + std::to_string((uint32_t)input_error);
    }

    // get action set
    auto action_set_error = vr::VRInput()->GetActionSetHandle("/actions/default", &m_action_set);

    if (action_set_error != vr::VRInputError_None) {
        return "VRInput failed to get action set: " + std::to_string((uint32_t)action_set_error);
    }

    if (m_action_set == vr::k_ulInvalidActionSetHandle) {
        return "VRInput failed to get action set handle.";
    }

    for (auto& it : m_action_handles) {
        auto error = vr::VRInput()->GetActionHandle(it.first.c_str(), &it.second.get());

        if (error != vr::VRInputError_None) {
            return "VRInput failed to get action handle: (" + it.first + "): " + std::to_string((uint32_t)error);
        }

        if (it.second == vr::k_ulInvalidActionHandle) {
            return "VRInput failed to get action handle: (" + it.first + ")";
        }
    }

    m_active_action_set.ulActionSet = m_action_set;
    m_active_action_set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    m_active_action_set.nPriority = 0;

    m_openvr->pose_action = m_action_pose;
    m_openvr->grip_pose_action = m_action_grip_pose;

    detect_controllers();

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr() {
    ZoneScopedN(__FUNCTION__);

    m_openxr.reset();
    m_openxr = std::make_shared<runtimes::OpenXR>();

    spdlog::info("[VR] Initializing OpenXR");

    if (GetModuleHandleW(L"openxr_loader.dll") == nullptr) {
        if (utility::load_module_from_current_directory(L"openxr_loader.dll") == nullptr) {
            spdlog::info("[VR] Could not load openxr_loader.dll");

            m_openxr->loaded = false;
            m_openxr->error = "Could not load openxr_loader.dll";

            return std::nullopt;
        }
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    m_openxr->needs_pose_update = true;
    m_openxr->got_first_poses = false;

    // Step 1: Create an instance
    spdlog::info("[VR] Creating OpenXR instance");

    XrResult result{XR_SUCCESS};

    // We may just be restarting OpenXR, so try to find an existing instance first
    if (m_openxr->instance == XR_NULL_HANDLE) {
        std::vector<const char*> extensions{};

        if (g_framework->is_dx12()) {
            extensions.push_back(XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
        } else {
            extensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        }

        // Enumerate available extensions and enable depth extension if available
        uint32_t extension_count{};
        result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr);

        std::vector<XrExtensionProperties> extension_properties(extension_count, {XR_TYPE_EXTENSION_PROPERTIES});

        if (!XR_FAILED(result)) try {
            result = xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count, extension_properties.data());

            if (!XR_FAILED(result)) {
                for (const auto& extension_property : extension_properties) {
                    spdlog::info("[VR] Found OpenXR extension: {}", extension_property.extensionName);
                }

                const std::unordered_set<std::string> wanted_extensions{
                    XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME,
                    XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME
                    // To be seen if we need more!
                };

                for (const auto& extension_property : extension_properties) {
                    if (wanted_extensions.contains(extension_property.extensionName)) {
                        spdlog::info("[VR] Enabling {} extension", extension_property.extensionName);
                        m_openxr->enabled_extensions.insert(extension_property.extensionName);
                        extensions.push_back(extension_property.extensionName);
                    }
                }
            }
        } catch(...) {
            spdlog::error("[VR] Unknown error while enumerating OpenXR extensions");
        }

        XrInstanceCreateInfo instance_create_info{XR_TYPE_INSTANCE_CREATE_INFO};
        instance_create_info.next = nullptr;
        instance_create_info.enabledExtensionCount = (uint32_t)extensions.size();
        instance_create_info.enabledExtensionNames = extensions.data();

        std::string application_name{"UEVR"};

        // Append the current executable name to the application base name
        {
            const auto exe = utility::get_executable();
            const auto full_path = utility::get_module_pathw(exe);

            if (full_path) {
                const auto fs_path = std::filesystem::path(*full_path);
                const auto filename = fs_path.stem().string();

                application_name += "_" + filename;

                // Trim the name to 127 characters
                if (application_name.length() >= XR_MAX_APPLICATION_NAME_SIZE) {
                    application_name = application_name.substr(0, XR_MAX_APPLICATION_NAME_SIZE - 1);
                }
            }
        }

        spdlog::info("[VR] Application name: {}", application_name);

        strcpy(instance_create_info.applicationInfo.applicationName, application_name.c_str());
        instance_create_info.applicationInfo.applicationName[XR_MAX_APPLICATION_NAME_SIZE - 1] = '\0';
        instance_create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        
        result = xrCreateInstance(&instance_create_info, &m_openxr->instance);

        // we can't convert the result to a string here
        // because the function requires the instance to be valid
        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr instance: " + std::to_string((int32_t)result);
            if (result == XR_ERROR_LIMIT_REACHED) {
                m_openxr->error = "Could not create openxr instance: XR_ERROR_LIMIT_REACHED\n"
                    "Ensure that the OpenXR plugin has been renamed or deleted from the game's binaries folder.";
            }
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    } else {
        spdlog::info("[VR] Found existing openxr instance");
    }
    
    // Step 2: Create a system
    spdlog::info("[VR] Creating OpenXR system");

    // We may just be restarting OpenXR, so try to find an existing system first
    if (m_openxr->system == XR_NULL_SYSTEM_ID) {
        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = m_openxr->form_factor;

        result = xrGetSystem(m_openxr->instance, &system_info, &m_openxr->system);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr system: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    } else {
        spdlog::info("[VR] Found existing openxr system");
    }

    // Step 3: Create a session
    spdlog::info("[VR] Initializing graphics info");

    XrSessionCreateInfo session_create_info{XR_TYPE_SESSION_CREATE_INFO};

    if (g_framework->is_dx12()) {
        m_d3d12.openxr().initialize(session_create_info);
    } else {
        m_d3d11.openxr().initialize(session_create_info);
    }

    spdlog::info("[VR] Creating OpenXR session");
    session_create_info.systemId = m_openxr->system;
    result = xrCreateSession(m_openxr->instance, &session_create_info, &m_openxr->session);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not create openxr session: " + m_openxr->get_result_string(result);
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    // Step 4: Create a space
    spdlog::info("[VR] Creating OpenXR space");

    // We may just be restarting OpenXR, so try to find an existing space first

    if (m_openxr->stage_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        space_create_info.poseInReferenceSpace = {};
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->stage_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr stage space: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    }

    if (m_openxr->view_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        space_create_info.poseInReferenceSpace = {};
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->view_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr view space: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    }

    // Step 5: Get the system properties
    spdlog::info("[VR] Getting OpenXR system properties");

    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    result = xrGetSystemProperties(m_openxr->instance, m_openxr->system, &system_properties);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not get system properties: " + m_openxr->get_result_string(result);
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    m_openxr->on_system_properties_acquired(system_properties);

    // Step 6: Get the view configuration properties
    m_openxr->update_render_target_size();

    // Step 7: Create a view
    if (!m_openxr->view_configs.empty()){
        m_openxr->views.resize(m_openxr->view_configs.size(), {XR_TYPE_VIEW});
        m_openxr->stage_views.resize(m_openxr->view_configs.size(), {XR_TYPE_VIEW});
    }

    if (m_openxr->view_configs.empty()) {
        m_openxr->error = "No view configurations found";
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    m_openxr->loaded = true;
    m_runtime = m_openxr;

    if (auto err = initialize_openxr_input()) {
        m_openxr->error = err.value();
        m_openxr->loaded = false;
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    detect_controllers();

    if (m_init_finished) {
        // This is usually done in on_config_load
        // but the runtime can be reinitialized, so we do it here instead
        initialize_openxr_swapchains();
    }

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_input() {
    ZoneScopedN(__FUNCTION__);

    if (auto err = m_openxr->initialize_actions(VR::actions_json)) {
        m_openxr->error = err.value();
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }
    
    for (auto& it : m_action_handles) {
        auto openxr_action_name = m_openxr->translate_openvr_action_name(it.first);

        if (m_openxr->action_set.action_map.contains(openxr_action_name)) {
            it.second.get() = (decltype(it.second)::type)m_openxr->action_set.action_map[openxr_action_name];
            spdlog::info("[VR] Successfully mapped action {} to {}", it.first, openxr_action_name);
        }
    }

    m_left_joystick = (decltype(m_left_joystick))VRRuntime::Hand::LEFT;
    m_right_joystick = (decltype(m_right_joystick))VRRuntime::Hand::RIGHT;

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_swapchains() {
    ZoneScopedN(__FUNCTION__);

    // This depends on the config being loaded.
    if (!m_init_finished) {
        return std::nullopt;
    }

    spdlog::info("[VR] Creating OpenXR swapchain");

    const auto supported_swapchain_formats = m_openxr->get_supported_swapchain_formats();

    // Log
    for (auto f : supported_swapchain_formats) {
        spdlog::info("[VR] Supported swapchain format: {}", (uint32_t)f);
    }

    if (g_framework->is_dx12()) {
        auto err = m_d3d12.openxr().create_swapchains();

        if (err) {
            m_openxr->error = err.value();
            m_openxr->loaded = false;
            spdlog::error("[VR] {}", m_openxr->error.value());

            return m_openxr->error;
        }
    } else {
        auto err = m_d3d11.openxr().create_swapchains();

        if (err) {
            m_openxr->error = err.value();
            m_openxr->loaded = false;
            spdlog::error("[VR] {}", m_openxr->error.value());
            return m_openxr->error;
        }
    }

    return std::nullopt;
}

bool VR::detect_controllers() {
    ZoneScopedN(__FUNCTION__);

    // already detected
    if (!m_controllers.empty()) {
        return true;
    }

    if (get_runtime()->is_openvr()) {
        auto left_joystick_origin_error = vr::EVRInputError::VRInputError_None;
        auto right_joystick_origin_error = vr::EVRInputError::VRInputError_None;

        vr::InputOriginInfo_t left_joystick_origin_info{};
        vr::InputOriginInfo_t right_joystick_origin_info{};

        // Get input origin info for the joysticks
        // get the source input device handles for the joysticks
        auto left_joystick_error = vr::VRInput()->GetInputSourceHandle("/user/hand/left", &m_left_joystick);

        if (left_joystick_error != vr::VRInputError_None) {
            return false;
        }

        auto right_joystick_error = vr::VRInput()->GetInputSourceHandle("/user/hand/right", &m_right_joystick);

        if (right_joystick_error != vr::VRInputError_None) {
            return false;
        }

        m_openvr->left_controller_handle = m_left_joystick;
        m_openvr->right_controller_handle = m_right_joystick;

        left_joystick_origin_info = {};
        right_joystick_origin_info = {};

        left_joystick_origin_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_left_joystick, &left_joystick_origin_info, sizeof(left_joystick_origin_info));
        right_joystick_origin_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_right_joystick, &right_joystick_origin_info, sizeof(right_joystick_origin_info));
        if (left_joystick_origin_error != vr::EVRInputError::VRInputError_None || right_joystick_origin_error != vr::EVRInputError::VRInputError_None) {
            return false;
        }

        // Instead of manually going through the devices,
        // We do this. The order of the devices isn't always guaranteed to be
        // Left, and then right. Using the input state handles will always
        // Get us the correct device indices.
        m_controllers.push_back(left_joystick_origin_info.trackedDeviceIndex);
        m_controllers.push_back(right_joystick_origin_info.trackedDeviceIndex);
        m_controllers_set.insert(left_joystick_origin_info.trackedDeviceIndex);
        m_controllers_set.insert(right_joystick_origin_info.trackedDeviceIndex);

        spdlog::info("Left Hand: {}", left_joystick_origin_info.trackedDeviceIndex);
        spdlog::info("Right Hand: {}", right_joystick_origin_info.trackedDeviceIndex);

        m_openvr->left_controller_index = left_joystick_origin_info.trackedDeviceIndex;
        m_openvr->right_controller_index = right_joystick_origin_info.trackedDeviceIndex;
    } else if (get_runtime()->is_openxr()) {
        // ezpz
        m_controllers.push_back(1);
        m_controllers.push_back(2);
        m_controllers_set.insert(1);
        m_controllers_set.insert(2);

        spdlog::info("Left Hand: {}", 1);
        spdlog::info("Right Hand: {}", 2);
    }


    return true;
}

bool VR::is_any_action_down() {
    ZoneScopedN(__FUNCTION__);

    if (!m_runtime->ready()) {
        return false;
    }

    const auto left_axis = get_left_stick_axis();
    const auto right_axis = get_right_stick_axis();

    if (glm::length(left_axis) >= m_joystick_deadzone->value()) {
        return true;
    }

    if (glm::length(right_axis) >= m_joystick_deadzone->value()) {
        return true;
    }

    const auto left_joystick = get_left_joystick();
    const auto right_joystick = get_right_joystick();

    for (auto& it : m_action_handles) {
        // These are too easy to trigger
        if (it.second == m_action_thumbrest_touch_left || it.second == m_action_thumbrest_touch_right) {
            continue;
        }

        if (it.second == m_action_a_button_touch_left || it.second == m_action_a_button_touch_right) {
            continue;
        }

        if (it.second == m_action_b_button_touch_left || it.second == m_action_b_button_touch_right) {
            continue;
        }

        if (is_action_active(it.second, left_joystick) || is_action_active(it.second, right_joystick)) {
            return true;
        }
    }

    return false;
}

bool VR::on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) {
    ZoneScopedN(__FUNCTION__);

    if (message == WM_DEVICECHANGE && !m_spoofed_gamepad_connection) {
        spdlog::info("[VR] Received WM_DEVICECHANGE");
        m_last_xinput_spoof_sent = std::chrono::steady_clock::now();
    }

    // 3D Display stereo cursor: keep the game from arming a hardware cursor —
    // the OS composites it flat on top of the stereo output (one copy at
    // screen depth). The compositor draws the per-eye cursor instead.
    if (message == WM_SETCURSOR && is_using_flat3d() && m_flat3d_cursor_mode->value() != 0) {
        SetCursor(nullptr);
        return false;
    }

    return true;
}

void VR::on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) {
    ZoneScopedN(__FUNCTION__);

    if (std::chrono::steady_clock::now() - m_last_engine_tick > std::chrono::seconds(1)) {
        SPDLOG_INFO_EVERY_N_SEC(1, "[VR] XInputGetState called, but engine tick hasn't been called in over a second. Is the game loading?");
        update_action_states();
    }

    if (*retval == ERROR_SUCCESS) {
        // Once here for normal gamepads, and once for the spoofed gamepad at the end
        update_imgui_state_from_xinput_state(*state, false);
        gamepad_snapturn(*state);
    }

    const auto now = std::chrono::steady_clock::now();

    if (now - m_last_xinput_update > std::chrono::seconds(2)) {
        m_lowest_xinput_user_index = user_index;
    }

    if (user_index < m_lowest_xinput_user_index) {
        m_lowest_xinput_user_index = user_index;
        spdlog::info("[VR] Changed lowest XInput user index to {}", user_index);
    }

    if (user_index != m_lowest_xinput_user_index) {
        if (!m_spoofed_gamepad_connection && is_using_controllers()) {
            spdlog::info("[VR] XInputGetState called, but user index is {}", user_index);
        }

        return;
    }

    if (!m_spoofed_gamepad_connection) {
        spdlog::info("[VR] Successfully spoofed gamepad connection @ {}", user_index);
    }
    
    m_last_xinput_update = now;
    m_spoofed_gamepad_connection = true;

    auto runtime = get_runtime();

    auto do_pause_select = [&]() {
        if (!runtime->ready()) {
            return;
        }

        if (runtime->handle_pause) {
            // Spoof the start button being pressed
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_START;
            *retval = ERROR_SUCCESS;
            runtime->handle_pause = false;
            runtime->handle_select_button = false;
        }

        if (runtime->handle_select_button) {
            // Spoof the back button being pressed
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_BACK;
            *retval = ERROR_SUCCESS;
            runtime->handle_select_button = false;
            runtime->handle_pause = false;
        }
    };

    do_pause_select();

    if (is_using_controllers_within(std::chrono::minutes(5))) {
        *retval = ERROR_SUCCESS;
    }

    if (!is_using_controllers()) {
        return;
    }

    // Clear button state for VR controllers
    if (is_using_controllers_within(std::chrono::seconds(5))) {
        state->Gamepad.wButtons = 0;
        state->Gamepad.bLeftTrigger = 0;
        state->Gamepad.bRightTrigger = 0;
        state->Gamepad.sThumbLX = 0;
        state->Gamepad.sThumbLY = 0;
        state->Gamepad.sThumbRX = 0;
        state->Gamepad.sThumbRY = 0;
    }

    const auto left_joystick = get_left_joystick();
    const auto right_joystick = get_right_joystick();
    const auto wants_swap = m_swap_controllers->value();

    runtime->handle_pause_select(is_action_active_any_joystick(m_action_system_button));
    do_pause_select();

    const auto& a_button_left = !wants_swap ? m_action_a_button_left : m_action_a_button_right;
    const auto& a_button_right = !wants_swap ? m_action_a_button_right : m_action_a_button_left;

    const auto is_right_a_button_down = is_action_active_any_joystick(a_button_right);
    const auto is_left_a_button_down = is_action_active_any_joystick(a_button_left);

    if (is_right_a_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_A;
    }

    if (is_left_a_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_B;
    }

    const auto& b_button_left = !wants_swap ? m_action_b_button_left : m_action_b_button_right;
    const auto& b_button_right = !wants_swap ? m_action_b_button_right : m_action_b_button_left;

    const auto is_right_b_button_down = is_action_active_any_joystick(b_button_right);
    const auto is_left_b_button_down = is_action_active_any_joystick(b_button_left);

    if (is_right_b_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_X;
    }

    if (is_left_b_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_Y;
    }

    const auto is_left_joystick_click_down = is_action_active(m_action_joystick_click, left_joystick);
    const auto is_right_joystick_click_down = is_action_active(m_action_joystick_click, right_joystick);

    if (is_left_joystick_click_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    }

    if (is_right_joystick_click_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    }

    const auto is_left_trigger_down = is_action_active(m_action_trigger, left_joystick);
    const auto is_right_trigger_down = is_action_active(m_action_trigger, right_joystick);

    if (is_left_trigger_down) {
        state->Gamepad.bLeftTrigger = 255;
    }

    if (is_right_trigger_down) {
        state->Gamepad.bRightTrigger = 255;
    }

    const auto is_right_grip_down = is_action_active(m_action_grip, right_joystick);
    const auto is_left_grip_down = is_action_active(m_action_grip, left_joystick);

    if (is_right_grip_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    }

    if (is_left_grip_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    }

    const auto is_dpad_up_down = is_action_active_any_joystick(m_action_dpad_up);

    if (is_dpad_up_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    }

    const auto is_dpad_right_down = is_action_active_any_joystick(m_action_dpad_right);

    if (is_dpad_right_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    }

    const auto is_dpad_down_down = is_action_active_any_joystick(m_action_dpad_down);

    if (is_dpad_down_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    }

    const auto is_dpad_left_down = is_action_active_any_joystick(m_action_dpad_left);

    if (is_dpad_left_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    }

    const auto left_joystick_axis = get_joystick_axis(left_joystick);
    const auto right_joystick_axis = get_joystick_axis(right_joystick);

    const auto true_left_joystick_axis = get_joystick_axis(m_left_joystick);
    const auto true_right_joystick_axis = get_joystick_axis(m_right_joystick);

    state->Gamepad.sThumbLX = (int16_t)std::clamp<float>(((float)state->Gamepad.sThumbLX + left_joystick_axis.x * 32767.0f), -32767.0f, 32767.0f);
    state->Gamepad.sThumbLY = (int16_t)std::clamp<float>(((float)state->Gamepad.sThumbLY + left_joystick_axis.y * 32767.0f), -32767.0f, 32767.0f);

    state->Gamepad.sThumbRX = (int16_t)std::clamp<float>(((float)state->Gamepad.sThumbRX + right_joystick_axis.x * 32767.0f), -32767.0f, 32767.0f);
    state->Gamepad.sThumbRY = (int16_t)std::clamp<float>(((float)state->Gamepad.sThumbRY + right_joystick_axis.y * 32767.0f), -32767.0f, 32767.0f);

    bool already_dpad_shifted{false};

    if (m_dpad_gesture_state.direction != DPadGestureState::Direction::NONE) {
        already_dpad_shifted = true;

        if ((m_dpad_gesture_state.direction & DPadGestureState::Direction::UP) != 0) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        }

        if ((m_dpad_gesture_state.direction & DPadGestureState::Direction::RIGHT) != 0) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        }

        if ((m_dpad_gesture_state.direction & DPadGestureState::Direction::DOWN) != 0) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        }

        if ((m_dpad_gesture_state.direction & DPadGestureState::Direction::LEFT) != 0) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        }

        // Zero out the thumbstick values
        state->Gamepad.sThumbLX = 0;
        state->Gamepad.sThumbLY = 0;

        std::scoped_lock _{m_dpad_gesture_state.mtx};
        m_dpad_gesture_state.direction = DPadGestureState::Direction::NONE;
    }

    // Touching the thumbrest allows us to use the thumbstick as a dpad.  Additional options are for controllers without capacitives/games that rely solely on DPad
    if (!already_dpad_shifted && m_dpad_shifting->value()) {
        bool button_touch_inactive{true};
        bool thumbrest_check{false};

        DPadMethod dpad_method = get_dpad_method();
        if (dpad_method == DPadMethod::RIGHT_TOUCH) {
            thumbrest_check = is_action_active_any_joystick(m_action_thumbrest_touch_right);
            button_touch_inactive = !is_action_active_any_joystick(m_action_a_button_touch_right) && !is_action_active_any_joystick(m_action_b_button_touch_right);
        }
        if (dpad_method == DPadMethod::LEFT_TOUCH) {
            thumbrest_check = is_action_active_any_joystick(m_action_thumbrest_touch_left);
            button_touch_inactive = !is_action_active_any_joystick(m_action_a_button_touch_left) && !is_action_active_any_joystick(m_action_b_button_touch_left);
        }

        const auto dpad_active = (button_touch_inactive && thumbrest_check) || dpad_method == DPadMethod::LEFT_JOYSTICK || dpad_method == DPadMethod::RIGHT_JOYSTICK;

        if (dpad_active) {
            float ty{0.0f};
            float tx{0.0f};
            //SHORT ThumbY{0};
            //SHORT ThumbX{0};
            // If someone is accidentally touching both thumbrests while also moving a joystick, this will default to left joystick.
            if (dpad_method == DPadMethod::RIGHT_TOUCH || dpad_method == DPadMethod::LEFT_JOYSTICK) {
                //ThumbY = state->Gamepad.sThumbLY;
                //ThumbX = state->Gamepad.sThumbLX;
                ty = true_left_joystick_axis.y;
                tx = true_left_joystick_axis.x;
            }
            else if (dpad_method == DPadMethod::LEFT_TOUCH || dpad_method == DPadMethod::RIGHT_JOYSTICK) {
                //ThumbY = state->Gamepad.sThumbRY;
                //ThumbX = state->Gamepad.sThumbRX;
                ty = true_right_joystick_axis.y;
                tx = true_right_joystick_axis.x;
            }
            
            if (ty >= 0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
            }

            if (ty <= -0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
            }

            if (tx >= 0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            }

            if (tx <= -0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
            }

            if (dpad_method == DPadMethod::RIGHT_TOUCH || dpad_method == DPadMethod::LEFT_JOYSTICK) {
                if (!wants_swap) {
                    state->Gamepad.sThumbLY = 0;
                    state->Gamepad.sThumbLX = 0;
                } else {
                    state->Gamepad.sThumbRY = 0;
                    state->Gamepad.sThumbRX = 0;
                }
            }
            else if (dpad_method == DPadMethod::LEFT_TOUCH || dpad_method == DPadMethod::RIGHT_JOYSTICK) {
                if (!wants_swap) {
                    state->Gamepad.sThumbRY = 0;
                    state->Gamepad.sThumbRX = 0;
                } else {
                    state->Gamepad.sThumbLY = 0;
                    state->Gamepad.sThumbLX = 0;
                }
            }
        }
    }

    // Determine if snapturn should be run on frame
    if (m_snapturn->value()) {
        DPadMethod dpad_method = get_dpad_method();
        const auto snapturn_deadzone = get_snapturn_js_deadzone();
        float stick_axis{};

        if (!m_was_snapturn_run_on_input) {
            if (dpad_method == RIGHT_JOYSTICK) {
                stick_axis = true_left_joystick_axis.x;
                if (glm::abs(stick_axis) >= snapturn_deadzone) {
                    if (stick_axis < 0) {
                        m_snapturn_left = true;
                    }
                    m_snapturn_on_frame = true;
                    m_was_snapturn_run_on_input = true;
                }
            }
            else {
                stick_axis = right_joystick_axis.x;
                const auto& thumbrest_touch_left = !wants_swap ? m_action_thumbrest_touch_left : m_action_thumbrest_touch_right;
                if (glm::abs(stick_axis) >= snapturn_deadzone && !(dpad_method == DPadMethod::LEFT_TOUCH && is_action_active_any_joystick(thumbrest_touch_left))) {
                    if (stick_axis < 0) {
                        m_snapturn_left = true;
                    }
                    m_snapturn_on_frame = true;
                    m_was_snapturn_run_on_input = true;
                }
            }
        }
        else {
            if (dpad_method == RIGHT_JOYSTICK) {
                if (glm::abs(true_left_joystick_axis.x) < snapturn_deadzone) {
                    m_was_snapturn_run_on_input = false;
                } else {
                    state->Gamepad.sThumbLY = 0;
                    state->Gamepad.sThumbLX = 0;
                }
            }
            else {
                if (glm::abs(right_joystick_axis.x) < snapturn_deadzone) {
                    m_was_snapturn_run_on_input = false;
                } else {
                    state->Gamepad.sThumbRY = 0;
                    state->Gamepad.sThumbRX = 0;
                }
            }
        }
    }
    
    // Do it again after all the VR buttons have been spoofed
    update_imgui_state_from_xinput_state(*state, true);
}

void VR::on_xinput_set_state(uint32_t* retval, uint32_t user_index, XINPUT_VIBRATION* vibration) {
    ZoneScopedN(__FUNCTION__);

    if (user_index != m_lowest_xinput_user_index) {
        return;
    }

    if (!is_using_controllers()) {
        return;
    }

    const auto left_amplitude = ((float)vibration->wLeftMotorSpeed / 65535.0f) * 5.0f;
    const auto right_amplitude = ((float)vibration->wRightMotorSpeed / 65535.0f) * 5.0f;

    if (left_amplitude > 0.0f) {
        trigger_haptic_vibration(0.0f, 0.1f, 1.0f, left_amplitude, get_left_joystick());
    }

    if (right_amplitude > 0.0f) {
        trigger_haptic_vibration(0.0f, 0.1f, 1.0f, right_amplitude, get_right_joystick());
    }
}

// Allows imgui navigation to work with the controllers
void VR::update_imgui_state_from_xinput_state(XINPUT_STATE& state, bool is_vr_controller) {
    ZoneScopedN(__FUNCTION__);

    bool is_using_this_controller = true;

    const auto is_using_vr_controller_recently = is_using_controllers_within(std::chrono::seconds(1));
    const auto is_gamepad = !is_vr_controller;

    if (is_vr_controller && !is_using_vr_controller_recently) {
        is_using_this_controller = false;
    } else if (is_gamepad && is_using_vr_controller_recently) { // dont allow gamepad navigation if using vr controllers
        is_using_this_controller = false;
    }

    // L3 + R3 to open the menu
    if ((state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0 && (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0) {
        if (!FrameworkConfig::get()->is_enable_l3_r3_toggle()) {
            return;
        }

        bool should_open = true;

        const auto now = std::chrono::steady_clock::now();

        if (FrameworkConfig::get()->is_l3_r3_long_press() && !g_framework->is_drawing_ui()) {
            if (!m_xinput_context.menu_longpress_begin_held) {
                m_xinput_context.menu_longpress_begin = now;
            }

            m_xinput_context.menu_longpress_begin_held = true;
            should_open = (now - m_xinput_context.menu_longpress_begin) >= std::chrono::seconds(1);
        } else {
            m_xinput_context.menu_longpress_begin_held = false;
        }

        if (should_open && now - m_last_xinput_l3_r3_menu_open >= std::chrono::seconds(1)) {
            m_last_xinput_l3_r3_menu_open = std::chrono::steady_clock::now();
            g_framework->set_draw_ui(!g_framework->is_drawing_ui());

            state.Gamepad.wButtons &= ~(XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB); // so input doesn't go through to the game
        }
    } else if (is_using_this_controller) {
        m_xinput_context.headlocked_begin_held = false;
        m_xinput_context.menu_longpress_begin_held = false;
    }

    // We need to adjust the stick values based on the selected movement orientation value if the user wants to do this
    // It will either need to be adjusted by the HMD rotation or one of the controllers.
    if (is_using_this_controller && m_movement_orientation->value() != VR::AimMethod::GAME && m_movement_orientation->value() != m_aim_method->value()) {
        const auto left_stick_og = glm::vec2((float)state.Gamepad.sThumbLX, (float)state.Gamepad.sThumbLY );
        const auto left_stick_magnitude = glm::clamp(glm::length(left_stick_og), -32767.0f, 32767.0f);
        const auto left_stick = glm::normalize(left_stick_og);
        const auto left_stick_angle = glm::atan2(left_stick.y, left_stick.x);

        if (this->is_controller_movement_enabled() && is_vr_controller) {
            const auto controller_index = this->get_movement_orientation() == VR::AimMethod::LEFT_CONTROLLER ? get_left_controller_index() : get_right_controller_index();
            const auto controller_rotation = utility::math::flatten(m_rotation_offset * glm::quat{get_rotation(controller_index)});
            const auto controller_forward = controller_rotation * glm::vec3(0.0f, 0.0f, 1.0f);
            const auto controller_angle = glm::atan2(controller_forward.x, controller_forward.z);

            // Normalize angles to [0, 2π]
            const auto normalized_left_stick_angle = left_stick_angle < 0 ? left_stick_angle + 2 * glm::pi<float>() : left_stick_angle;
            const auto normalized_controller_angle = controller_angle < 0 ? controller_angle + 2 * glm::pi<float>() : controller_angle;

            // Add the angles together
            const auto new_left_stick_angle = utility::math::fix_angle(normalized_left_stick_angle + normalized_controller_angle);
            const auto new_left_stick = glm::vec2(glm::cos(new_left_stick_angle), glm::sin(new_left_stick_angle)) * left_stick_magnitude;

            state.Gamepad.sThumbLX = (int16_t)new_left_stick.x;
            state.Gamepad.sThumbLY = (int16_t)new_left_stick.y;
        } else { // Fallback to head aim
            // Rotate the left stick by the HMD rotation
            const auto hmd_rotation = utility::math::flatten(m_rotation_offset * glm::quat{get_rotation(0)});
            const auto hmd_forward = hmd_rotation * glm::vec3(0.0f, 0.0f, 1.0f);
            const auto hmd_angle = glm::atan2(hmd_forward.x, hmd_forward.z);

            // Normalize angles to [0, 2π]
            const auto normalized_left_stick_angle = left_stick_angle < 0 ? left_stick_angle + 2 * glm::pi<float>() : left_stick_angle;
            const auto normalized_hmd_angle = hmd_angle < 0 ? hmd_angle + 2 * glm::pi<float>() : hmd_angle;

            // Add the angles together
            const auto new_left_stick_angle = utility::math::fix_angle(normalized_left_stick_angle + normalized_hmd_angle);
            const auto new_left_stick = glm::vec2{glm::cos(new_left_stick_angle), glm::sin(new_left_stick_angle)} * left_stick_magnitude;

            state.Gamepad.sThumbLX = (int16_t)new_left_stick.x;
            state.Gamepad.sThumbLY = (int16_t)new_left_stick.y;
        }
    }

    if (!g_framework->is_drawing_ui()) {
        m_rt_modifier.draw = false;
        return;
    }

    if (!is_using_this_controller) {
        return;
    }

    // Gamepad navigation when the menu is open
    m_xinput_context.enqueue(is_vr_controller, state, [this](const XINPUT_STATE& state, bool is_vr_controller){
        static auto last_time = std::chrono::high_resolution_clock::now();

        const auto delta = std::chrono::duration<float>((std::chrono::high_resolution_clock::now() - last_time)).count();
        last_time = std::chrono::high_resolution_clock::now();

        auto& io = ImGui::GetIO();
        auto& gamepad = state.Gamepad;

        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

        // Headlocked aim toggle
        if (!FrameworkConfig::get()->is_l3_r3_long_press()) {
            if ((state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0 && (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0) {
                if (!m_xinput_context.headlocked_begin_held) {
                    m_xinput_context.headlocked_begin = std::chrono::steady_clock::now();
                    m_xinput_context.headlocked_begin_held = true;
                }
            } else {
                m_xinput_context.headlocked_begin_held = false;
            }
        }

        // Now that we're drawing the UI, check for special button combos the user can use as shortcuts
        // like recenter view, set standing origin, camera offset modification, etc.
        m_rt_modifier.draw = gamepad.bRightTrigger >= 128;

        if (!m_rt_modifier.draw) {
            m_rt_modifier.page = 0;
            m_rt_modifier.was_moving_left = false;
            m_rt_modifier.was_moving_right = false;
        }

        // If user holding down RT with menu open...
        if (m_rt_modifier.draw) {
            // Camera offset modification
            const auto right_ratio = (float)gamepad.sThumbLX / 32767.0f;
            const auto forward_ratio = (float)gamepad.sThumbLY / 32767.0f;
            const auto up_ratio = (float)gamepad.sThumbRY / 32767.0f;

            if (right_ratio <= -0.25f || right_ratio >= 0.25f) {
                const auto right_offset = right_ratio * delta * 150.0f;
                m_camera_right_offset->value() += right_offset;
            }

            if (forward_ratio <= -0.25f || forward_ratio >= 0.25f) {
                const auto forward_offset = forward_ratio * delta * 150.0f;
                m_camera_forward_offset->value() += forward_offset;
            }

            if (up_ratio <= -0.25f || up_ratio >= 0.25f) {
                const auto up_offset = up_ratio * delta * 150.0f;
                m_camera_up_offset->value() += up_offset;
            }

            if (gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) {
                if (!m_rt_modifier.was_moving_left) {
                    if (m_rt_modifier.page > 0) {
                        m_rt_modifier.page--;
                    } else {
                        m_rt_modifier.page = m_rt_modifier.num_pages - 1;
                    }

                    m_rt_modifier.was_moving_left = true;
                }
            } else {
                m_rt_modifier.was_moving_left = false;
            }

            if (gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) {
                if (!m_rt_modifier.was_moving_right) {
                    if (m_rt_modifier.page < m_rt_modifier.num_pages - 1) {
                        m_rt_modifier.page++;
                    } else {
                        m_rt_modifier.page = 0;
                    }

                    m_rt_modifier.was_moving_right = true;
                }
            } else {
                m_rt_modifier.was_moving_right = false;
            }

            // Reset camera offset
            switch (m_rt_modifier.page) {
            case 2:
                if (gamepad.wButtons & XINPUT_GAMEPAD_B) {
                    save_camera(2);
                }

                // Recenter
                if (gamepad.wButtons & XINPUT_GAMEPAD_Y) {
                    save_camera(1);
                }

                // Reset standing origin
                if (gamepad.wButtons & XINPUT_GAMEPAD_X) {
                    save_camera(0);
                }
                break;
            
            case 1:
                if (gamepad.wButtons & XINPUT_GAMEPAD_B) {
                    load_camera(2);
                }

                // Recenter
                if (gamepad.wButtons & XINPUT_GAMEPAD_Y) {
                    load_camera(1);
                }

                // Reset standing origin
                if (gamepad.wButtons & XINPUT_GAMEPAD_X) {
                    load_camera(0);
                }

                break; 
            case 0:
            default:
                if (gamepad.wButtons & XINPUT_GAMEPAD_B) {
                    m_camera_right_offset->value() = 0.0f;
                    m_camera_forward_offset->value() = 0.0f;
                    m_camera_up_offset->value() = 0.0f;
                }

                // Recenter
                if (gamepad.wButtons & XINPUT_GAMEPAD_Y) {
                    this->recenter_view();
                }

                // Reset standing origin
                if (gamepad.wButtons & XINPUT_GAMEPAD_X) {
                    this->set_standing_origin(this->get_position(0));
                }
                
                break;
            }

            // ignore everything else
            return;
        }

        // From imgui_impl_win32.cpp
        #define IM_SATURATE(V)                      (V < 0.0f ? 0.0f : V > 1.0f ? 1.0f : V)
        #define MAP_BUTTON(KEY_NO, BUTTON_ENUM)     { io.AddKeyEvent(KEY_NO, (gamepad.wButtons & BUTTON_ENUM) != 0); }
        #define MAP_ANALOG(KEY_NO, VALUE, V0, V1)   { float vn = (float)(VALUE - V0) / (float)(V1 - V0); io.AddKeyAnalogEvent(KEY_NO, vn > 0.10f, IM_SATURATE(vn)); }

        MAP_BUTTON(ImGuiKey_GamepadStart,           XINPUT_GAMEPAD_START);
        MAP_BUTTON(ImGuiKey_GamepadBack,            XINPUT_GAMEPAD_BACK);
        MAP_BUTTON(ImGuiKey_GamepadFaceLeft,        XINPUT_GAMEPAD_X);
        MAP_BUTTON(ImGuiKey_GamepadFaceRight,       XINPUT_GAMEPAD_B);
        MAP_BUTTON(ImGuiKey_GamepadFaceUp,          XINPUT_GAMEPAD_Y);
        MAP_BUTTON(ImGuiKey_GamepadFaceDown,        XINPUT_GAMEPAD_A);
        MAP_BUTTON(ImGuiKey_GamepadDpadLeft,        XINPUT_GAMEPAD_DPAD_LEFT);
        MAP_BUTTON(ImGuiKey_GamepadDpadRight,       XINPUT_GAMEPAD_DPAD_RIGHT);
        MAP_BUTTON(ImGuiKey_GamepadDpadUp,          XINPUT_GAMEPAD_DPAD_UP);
        MAP_BUTTON(ImGuiKey_GamepadDpadDown,        XINPUT_GAMEPAD_DPAD_DOWN);
        MAP_ANALOG(ImGuiKey_GamepadL2,              gamepad.bLeftTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
        MAP_ANALOG(ImGuiKey_GamepadR2,              gamepad.bRightTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
        MAP_BUTTON(ImGuiKey_GamepadL3,              XINPUT_GAMEPAD_LEFT_THUMB);
        MAP_BUTTON(ImGuiKey_GamepadR3,              XINPUT_GAMEPAD_RIGHT_THUMB);

        if (!is_vr_controller) {
            MAP_ANALOG(ImGuiKey_GamepadLStickLeft,      gamepad.sThumbLX, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
            MAP_ANALOG(ImGuiKey_GamepadLStickRight,     gamepad.sThumbLX, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
            MAP_ANALOG(ImGuiKey_GamepadLStickUp,        gamepad.sThumbLY, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
            MAP_ANALOG(ImGuiKey_GamepadLStickDown,      gamepad.sThumbLY, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
            MAP_BUTTON(ImGuiKey_GamepadL1,              XINPUT_GAMEPAD_LEFT_SHOULDER);
            MAP_BUTTON(ImGuiKey_GamepadR1,              XINPUT_GAMEPAD_RIGHT_SHOULDER);
        } else {
            // Map it to the dpad
            const auto left_stick_left = gamepad.sThumbLX < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * 2;
            if (m_xinput_context.vr.left_stick_left.was_pressed(left_stick_left)) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, true);
            } else if (!left_stick_left) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, false);
            }

            const auto left_stick_right = gamepad.sThumbLX > +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * 2;
            if (m_xinput_context.vr.left_stick_right.was_pressed(left_stick_right)) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadRight, true);
            } else if (!left_stick_right) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadRight, false);
            }

            const auto left_stick_up = gamepad.sThumbLY > +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * 2;
            if (m_xinput_context.vr.left_stick_up.was_pressed(left_stick_up)) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadUp, true);
            } else if (!left_stick_up) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadUp, false);
            }

            const auto left_stick_down = gamepad.sThumbLY < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * 2;
            if (m_xinput_context.vr.left_stick_down.was_pressed(left_stick_down)) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadDown, true);
            } else if (!left_stick_down) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadDown, false);
            }
        }

        MAP_ANALOG(ImGuiKey_GamepadRStickLeft,      gamepad.sThumbRX, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
        MAP_ANALOG(ImGuiKey_GamepadRStickRight,     gamepad.sThumbRX, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
        MAP_ANALOG(ImGuiKey_GamepadRStickUp,        gamepad.sThumbRY, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
        MAP_ANALOG(ImGuiKey_GamepadRStickDown,      gamepad.sThumbRY, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
    });

    // Zero out the state so we don't send input to the game.
    ZeroMemory(&state.Gamepad, sizeof(XINPUT_GAMEPAD));
}

void VR::on_pre_engine_tick(sdk::UGameEngine* engine, float delta) {
    ZoneScopedN(__FUNCTION__);

    // AFW init retry: no-op once fully installed. Re-attempts the DLSS/NGX hooks
    // until the game has loaded nvngx.dll (it loads lazily on first DLSS use).
    init_framewarp_module();

    const auto now = std::chrono::steady_clock::now();
    const auto previous_engine_tick = m_last_engine_tick;
    const bool hitch_diagnostics_enabled = m_enable_hitch_diagnostics->value();

    m_cvar_manager->on_pre_engine_tick(engine, delta);
    m_last_engine_tick = std::chrono::steady_clock::now();

    if (!get_runtime()->loaded || !is_hmd_active()) {
        return;
    }

    SPDLOG_INFO_ONCE("VR: Pre-engine tick");

    m_render_target_pool_hook->on_pre_engine_tick(engine, delta);

    update_statistics_overlay(engine);

    // Dont update action states on AFR frames
    // TODO: fix this for actual AFR, but we dont really care about pure AFR since synced beats it most of the time
    if (m_fake_stereo_hook != nullptr && !m_fake_stereo_hook->is_ignoring_next_viewport_draw()) {
        update_action_states();
    }
}

void VR::update_imgui_state_from_vr_controller_fallback() {
    // A few games only delay-load XInput when a physical gamepad is first
    // queried. Their normal VR controller path therefore never reaches
    // on_xinput_get_state. Feed UEVR's ImGui navigation directly from the
    // already-synchronized OpenXR actions until a real XInput callback arrives.
    if (m_has_observed_xinput.load(std::memory_order_relaxed) ||
        g_framework == nullptr ||
        !is_using_controllers())
    {
        return;
    }

    XINPUT_STATE state{};
    const auto left_joystick = get_left_joystick();
    const auto right_joystick = get_right_joystick();
    const auto wants_swap = m_swap_controllers->value();

    const auto& a_button_left = !wants_swap ? m_action_a_button_left : m_action_a_button_right;
    const auto& a_button_right = !wants_swap ? m_action_a_button_right : m_action_a_button_left;
    const auto& b_button_left = !wants_swap ? m_action_b_button_left : m_action_b_button_right;
    const auto& b_button_right = !wants_swap ? m_action_b_button_right : m_action_b_button_left;

    if (is_action_active_any_joystick(a_button_right)) {
        state.Gamepad.wButtons |= XINPUT_GAMEPAD_A;
    }

    if (is_action_active_any_joystick(a_button_left)) {
        state.Gamepad.wButtons |= XINPUT_GAMEPAD_B;
    }

    if (is_action_active_any_joystick(b_button_right)) {
        state.Gamepad.wButtons |= XINPUT_GAMEPAD_X;
    }

    if (is_action_active_any_joystick(b_button_left)) {
        state.Gamepad.wButtons |= XINPUT_GAMEPAD_Y;
    }

    if (is_action_active(m_action_joystick_click, left_joystick)) {
        state.Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    }

    if (is_action_active(m_action_joystick_click, right_joystick)) {
        state.Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    }

    if (is_action_active(m_action_trigger, left_joystick)) {
        state.Gamepad.bLeftTrigger = 255;
    }

    if (is_action_active(m_action_trigger, right_joystick)) {
        state.Gamepad.bRightTrigger = 255;
    }

    if (is_action_active(m_action_grip, left_joystick)) {
        state.Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    }

    if (is_action_active(m_action_grip, right_joystick)) {
        state.Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    }

    if (is_action_active_any_joystick(m_action_dpad_up)) {
        state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    }

    if (is_action_active_any_joystick(m_action_dpad_right)) {
        state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    }

    if (is_action_active_any_joystick(m_action_dpad_down)) {
        state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    }

    if (is_action_active_any_joystick(m_action_dpad_left)) {
        state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    }

    const auto left_axis = get_joystick_axis(left_joystick);
    const auto right_axis = get_joystick_axis(right_joystick);
    state.Gamepad.sThumbLX = (int16_t)std::clamp(left_axis.x * 32767.0f, -32767.0f, 32767.0f);
    state.Gamepad.sThumbLY = (int16_t)std::clamp(left_axis.y * 32767.0f, -32767.0f, 32767.0f);
    state.Gamepad.sThumbRX = (int16_t)std::clamp(right_axis.x * 32767.0f, -32767.0f, 32767.0f);
    state.Gamepad.sThumbRY = (int16_t)std::clamp(right_axis.y * 32767.0f, -32767.0f, 32767.0f);

    update_imgui_state_from_xinput_state(state, true);
}

void VR::update_subnautica2_save_thumbnail_guard(sdk::UGameEngine* engine) {
    if (!is_subnautica2_executable() || m_subnautica2_save_thumbnail_guard_done) {
        return;
    }

    if (!m_subnautica2_save_thumbnail_fallback_logged) {
        m_subnautica2_save_thumbnail_fallback_logged = true;
        SPDLOG_INFO("[Subnautica2][SaveThumbnailGuard] Skipping save-thumbnail byte patch path and forcing UObject thumbnail-disable fallback");
    }

    if (engine == nullptr) {
        return;
    }

    const auto object_array = sdk::FUObjectArray::get();
    if (object_array == nullptr || IsBadReadPtr(object_array, sizeof(void*))) {
        return;
    }

    const auto object_count = object_array->get_object_count();
    if (object_count <= 0) {
        return;
    }

    if (m_subnautica2_save_thumbnail_guard_cursor < 0 || m_subnautica2_save_thumbnail_guard_cursor >= object_count) {
        m_subnautica2_save_thumbnail_guard_cursor = 0;
    }

    // Subnautica 2 save thumbnails use a UWE screenshot readback path that can
    // overrun its thumbnail crop allocation in VR. Scan incrementally and only
    // touch the narrow UWE settings shape to avoid a startup hitch.
    constexpr int32_t SCAN_BUDGET_PER_TICK = 2048;
    constexpr uint32_t MAX_FULL_SWEEPS = 3;

    int32_t scanned = 0;
    while (scanned < SCAN_BUDGET_PER_TICK && m_subnautica2_save_thumbnail_guard_full_sweeps < MAX_FULL_SWEEPS) {
        auto* item = object_array->get_object(m_subnautica2_save_thumbnail_guard_cursor);
        ++scanned;

        m_subnautica2_save_thumbnail_guard_cursor++;
        if (m_subnautica2_save_thumbnail_guard_cursor >= object_count) {
            m_subnautica2_save_thumbnail_guard_cursor = 0;
            ++m_subnautica2_save_thumbnail_guard_full_sweeps;
        }

        if (item == nullptr || IsBadReadPtr(item, sizeof(void*))) {
            continue;
        }

        auto* object = (sdk::UObject*)item->get_object();
        if (object == nullptr || IsBadReadPtr(object, sizeof(void*))) {
            continue;
        }

        sdk::UClass* klass = nullptr;
        try {
            klass = object->get_class();
        } catch (...) {
            klass = nullptr;
        }

        if (klass == nullptr || IsBadReadPtr(klass, sizeof(void*))) {
            continue;
        }

        const auto key = (uintptr_t)klass;
        auto it = m_subnautica2_save_thumbnail_guard_class_cache.find(key);
        const bool candidate = it != m_subnautica2_save_thumbnail_guard_class_cache.end()
            ? it->second
            : is_subnautica2_save_thumbnail_settings_class(klass);

        if (it == m_subnautica2_save_thumbnail_guard_class_cache.end()) {
            m_subnautica2_save_thumbnail_guard_class_cache.emplace(key, candidate);
        }

        if (!candidate) {
            continue;
        }

        bool patched = false;
        if (disable_subnautica2_save_thumbnails_on_object(object)) {
            ++m_subnautica2_save_thumbnail_guard_patched_objects;
            patched = true;
        }

        if (auto* cdo = klass->get_class_default_object(); cdo != nullptr && cdo != object && !IsBadReadPtr(cdo, sizeof(void*))) {
            if (disable_subnautica2_save_thumbnails_on_object(cdo)) {
                ++m_subnautica2_save_thumbnail_guard_patched_objects;
                patched = true;
            }
        }

        if (patched) {
            m_subnautica2_save_thumbnail_guard_done = true;
            m_subnautica2_save_thumbnail_guard_found_candidate = true;
            SPDLOG_INFO(
                "[Subnautica2][SaveThumbnailGuard] Applied after scanning {} objects over {} full sweeps; patched_objects={}",
                scanned,
                m_subnautica2_save_thumbnail_guard_full_sweeps,
                m_subnautica2_save_thumbnail_guard_patched_objects);
            return;
        }
    }

    if (m_subnautica2_save_thumbnail_guard_full_sweeps >= MAX_FULL_SWEEPS && !m_subnautica2_save_thumbnail_guard_warned_exhausted) {
        m_subnautica2_save_thumbnail_guard_warned_exhausted = true;
        m_subnautica2_save_thumbnail_guard_done = true;
        SPDLOG_WARN(
            "[Subnautica2][SaveThumbnailGuard] Did not find a UWE save-thumbnail settings object after {} FUObjectArray sweeps; save-thumbnail readback remains enabled",
            m_subnautica2_save_thumbnail_guard_full_sweeps);
    }
}

void VR::restore_subnautica2_native_water_cvars() {
    if (!m_subnautica2_native_water_cvars_applied || m_subnautica2_native_water_previous_ints.empty()) {
        m_subnautica2_native_water_cvars_applied = false;
        m_subnautica2_native_water_cvars_logged = false;
        m_subnautica2_native_water_cvar_attempts = 0;
        m_subnautica2_native_water_last_mode = -1;
        m_subnautica2_native_water_next_apply = {};
        return;
    }

    const auto console_manager = sdk::FConsoleManager::get();
    if (console_manager == nullptr) {
        return;
    }

    uint32_t restored{};
    uint32_t missing{};
    uint32_t failed{};

    for (const auto& [name, value] : m_subnautica2_native_water_previous_ints) {
        auto* object = console_manager->find(name);
        if (object == nullptr || object->AsCommand() != nullptr) {
            ++missing;
            continue;
        }

        auto* variable = (sdk::IConsoleVariable*)object;
        bool ok{};

        try {
            ok = variable->Set(std::to_wstring(value).c_str());
        } catch (...) {
            ok = false;
        }

        if (ok) {
            ++restored;
        } else {
            ++failed;
        }
    }

    SPDLOG_INFO(
        "[Subnautica2][NativeWaterCompat] Restored water cvars restored={} missing={} failed={}",
        restored,
        missing,
        failed);

    m_subnautica2_native_water_previous_ints.clear();
    m_subnautica2_native_water_cvars_applied = false;
    m_subnautica2_native_water_cvars_logged = false;
    m_subnautica2_native_water_cvar_attempts = 0;
    m_subnautica2_native_water_last_mode = -1;
    m_subnautica2_native_water_next_apply = {};
}

void VR::update_subnautica2_native_water_compatibility(sdk::UGameEngine* engine) {
    (void)engine;

    const bool active =
        is_subnautica2_executable() &&
        g_framework != nullptr &&
        g_framework->is_dx12() &&
        is_hmd_active() &&
        m_compatibility_subnautica2_native_water->value() &&
        m_rendering_method->value() == RenderingMethod::NATIVE_STEREO &&
        !m_native_stereo_fix->value();

    if (!active) {
        restore_subnautica2_native_water_cvars();
        return;
    }

    auto selected_mode = static_cast<int32_t>(m_subnautica2_native_water_mode->value());
    if (selected_mode < SUBNAUTICA2_NATIVE_WATER_SAFE_REFLECTIONS ||
        selected_mode > SUBNAUTICA2_NATIVE_WATER_DISABLE_SINGLE_LAYER) {
        selected_mode = SUBNAUTICA2_NATIVE_WATER_SAFE_REFLECTIONS;
    }

    const bool mode_changed = selected_mode != m_subnautica2_native_water_last_mode;
    if (mode_changed && m_subnautica2_native_water_cvars_applied) {
        // Switching modes must not retain disables from the previous mode.
        restore_subnautica2_native_water_cvars();
    }

    if (mode_changed) {
        m_subnautica2_native_water_cvars_logged = false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_subnautica2_native_water_next_apply != std::chrono::steady_clock::time_point{} &&
        now < m_subnautica2_native_water_next_apply &&
        !mode_changed) {
        return;
    }

    // This is not a per-frame hammer path. It only corrects drift occasionally,
    // with immediate reapply when the user changes modes.
    m_subnautica2_native_water_next_apply = now + std::chrono::seconds(5);
    ++m_subnautica2_native_water_cvar_attempts;

    const auto console_manager = sdk::FConsoleManager::get();
    if (console_manager == nullptr) {
        if (m_subnautica2_native_water_cvar_attempts == 1 || (m_subnautica2_native_water_cvar_attempts % 120) == 0) {
            SPDLOG_WARN("[Subnautica2][NativeWaterCompat] FConsoleManager unavailable; cannot apply water cvars yet");
        }
        return;
    }

    struct ForcedCVar {
        const wchar_t* name;
        int value;
    };

    // UE5.6 SingleLayerWater uses per-view scene-color/reflection inputs that can
    // diverge in Subnautica 2 native stereo. Keep the water system enabled and
    // disable the native-stereo-sensitive subpaths first; fall back to disabling
    // SingleLayerWater only when the user explicitly chooses that mode.
    static constexpr std::array<ForcedCVar, 9> safe_reflections_cvars{{
        ForcedCVar{L"r.Water.Enabled", 1},
        ForcedCVar{L"r.Water.WaterMesh.Enabled", 1},
        ForcedCVar{L"r.Water.SingleLayer", 1},
        ForcedCVar{L"r.ParallelSingleLayerWaterPass", 0},
        ForcedCVar{L"r.Water.SingleLayer.TiledSceneColorCopy", 0},
        ForcedCVar{L"r.Water.SingleLayer.TiledComposite", 0},
        ForcedCVar{L"r.Water.SingleLayer.Reflection", 2},
        ForcedCVar{L"r.Water.SingleLayer.SSRTAA", 0},
        ForcedCVar{L"r.NGX.DLSS.WaterReflections.TemporalAA", 0},
    }};

    static constexpr std::array<ForcedCVar, 9> no_reflections_cvars{{
        ForcedCVar{L"r.Water.Enabled", 1},
        ForcedCVar{L"r.Water.WaterMesh.Enabled", 1},
        ForcedCVar{L"r.Water.SingleLayer", 1},
        ForcedCVar{L"r.ParallelSingleLayerWaterPass", 0},
        ForcedCVar{L"r.Water.SingleLayer.TiledSceneColorCopy", 0},
        ForcedCVar{L"r.Water.SingleLayer.TiledComposite", 0},
        ForcedCVar{L"r.Water.SingleLayer.Reflection", 0},
        ForcedCVar{L"r.Water.SingleLayer.SSRTAA", 0},
        ForcedCVar{L"r.NGX.DLSS.WaterReflections.TemporalAA", 0},
    }};

    static constexpr std::array<ForcedCVar, 9> disable_single_layer_cvars{{
        ForcedCVar{L"r.Water.Enabled", 1},
        ForcedCVar{L"r.Water.WaterMesh.Enabled", 1},
        ForcedCVar{L"r.Water.SingleLayer", 0},
        ForcedCVar{L"r.ParallelSingleLayerWaterPass", 0},
        ForcedCVar{L"r.Water.SingleLayer.TiledSceneColorCopy", 0},
        ForcedCVar{L"r.Water.SingleLayer.TiledComposite", 0},
        ForcedCVar{L"r.Water.SingleLayer.Reflection", 0},
        ForcedCVar{L"r.Water.SingleLayer.SSRTAA", 0},
        ForcedCVar{L"r.NGX.DLSS.WaterReflections.TemporalAA", 0},
    }};

    const char* mode_name = "Native Water Safe Reflections";
    const ForcedCVar* forced_cvars = safe_reflections_cvars.data();
    size_t forced_cvar_count = safe_reflections_cvars.size();

    switch (selected_mode) {
    case SUBNAUTICA2_NATIVE_WATER_NO_REFLECTIONS:
        mode_name = "Native Water No Reflections";
        forced_cvars = no_reflections_cvars.data();
        forced_cvar_count = no_reflections_cvars.size();
        break;
    case SUBNAUTICA2_NATIVE_WATER_DISABLE_SINGLE_LAYER:
        mode_name = "Disable SingleLayerWater Fallback";
        forced_cvars = disable_single_layer_cvars.data();
        forced_cvar_count = disable_single_layer_cvars.size();
        break;
    default:
        break;
    };

    uint32_t found{};
    uint32_t set_ok{};
    uint32_t set_failed{};
    uint32_t already_ok{};
    uint32_t missing{};

    for (size_t i = 0; i < forced_cvar_count; ++i) {
        const auto& forced = forced_cvars[i];
        const std::wstring cvar_name{forced.name};
        auto* object = console_manager->find(cvar_name);
        if (object == nullptr || object->AsCommand() != nullptr) {
            ++missing;
            continue;
        }

        ++found;
        auto* variable = (sdk::IConsoleVariable*)object;
        int before{};
        int after{};
        bool ok{};

        try {
            before = variable->GetInt();
            if (!m_subnautica2_native_water_previous_ints.contains(cvar_name)) {
                m_subnautica2_native_water_previous_ints.emplace(cvar_name, before);
            }

            if (before == forced.value) {
                ok = true;
                ++already_ok;
            } else {
                ok = variable->Set(std::to_wstring(forced.value).c_str());
            }

            after = variable->GetInt();
        } catch (...) {
            ok = false;
        }

        if (!ok) {
            ++set_failed;
        } else if (before != forced.value) {
            ++set_ok;
        }

        if (!m_subnautica2_native_water_cvars_logged) {
            SPDLOG_INFO(
                "[Subnautica2][NativeWaterCompat] forced {}: before={} requested={} after={} ok={}",
                utility::narrow(forced.name),
                before,
                forced.value,
                after,
                ok);
        }
    }

    if (found == 0) {
        if (!m_subnautica2_native_water_cvars_logged || (m_subnautica2_native_water_cvar_attempts % 120) == 0) {
            SPDLOG_WARN(
                "[Subnautica2][NativeWaterCompat] No SingleLayerWater cvars found attempt={} missing={}",
                m_subnautica2_native_water_cvar_attempts,
                missing);
        }
        return;
    }

    if (!m_subnautica2_native_water_cvars_logged) {
        SPDLOG_INFO(
            "[Subnautica2][NativeWaterCompat] Applied native water cvar guard mode=\"{}\" found={} missing={} already_ok={} set_ok={} set_failed={}",
            mode_name,
            found,
            missing,
            already_ok,
            set_ok,
            set_failed);
        m_subnautica2_native_water_cvars_logged = true;
    }

    m_subnautica2_native_water_cvars_applied = true;
    m_subnautica2_native_water_last_mode = selected_mode;
}

void VR::restore_1666amsterdam_native_postprocess_cvars() {
    if (!m_1666amsterdam_native_postprocess_cvars_applied ||
        m_1666amsterdam_native_postprocess_previous_ints.empty()) {
        m_1666amsterdam_native_postprocess_cvars_applied = false;
        m_1666amsterdam_native_postprocess_cvars_logged = false;
        m_1666amsterdam_native_postprocess_cvar_attempts = 0;
        m_1666amsterdam_native_postprocess_next_apply = {};
        return;
    }

    const auto console_manager = sdk::FConsoleManager::get();
    if (console_manager == nullptr) {
        return;
    }

    uint32_t restored{};
    uint32_t missing{};
    uint32_t failed{};

    for (const auto& [name, value] : m_1666amsterdam_native_postprocess_previous_ints) {
        auto* object = console_manager->find(name);
        if (object == nullptr || object->AsCommand() != nullptr) {
            ++missing;
            continue;
        }

        auto* variable = (sdk::IConsoleVariable*)object;
        bool ok{};

        try {
            ok = variable->Set(std::to_wstring(value).c_str());
        } catch (...) {
            ok = false;
        }

        if (ok) {
            ++restored;
        } else {
            ++failed;
        }
    }

    SPDLOG_INFO(
        "[1666Amsterdam][NativePostProcess] Restored cvars restored={} missing={} failed={}",
        restored,
        missing,
        failed);

    m_1666amsterdam_native_postprocess_previous_ints.clear();
    m_1666amsterdam_native_postprocess_cvars_applied = false;
    m_1666amsterdam_native_postprocess_cvars_logged = false;
    m_1666amsterdam_native_postprocess_cvar_attempts = 0;
    m_1666amsterdam_native_postprocess_next_apply = {};
}

void VR::update_1666amsterdam_native_postprocess_compatibility(sdk::UGameEngine* engine) {
    (void)engine;

    const bool active =
        is_1666amsterdam_executable() &&
        g_framework != nullptr &&
        g_framework->is_dx12() &&
        is_hmd_active() &&
        m_compatibility_1666amsterdam_native_postprocess->value() &&
        m_rendering_method->value() == RenderingMethod::NATIVE_STEREO &&
        !m_native_stereo_fix->value();

    if (!active) {
        restore_1666amsterdam_native_postprocess_cvars();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_1666amsterdam_native_postprocess_next_apply != std::chrono::steady_clock::time_point{} &&
        now < m_1666amsterdam_native_postprocess_next_apply) {
        return;
    }

    // Amsterdam's full temporal path can replace the secondary-eye scene with
    // black while its geometry, HUD, and gamma-only path remain valid.
    m_1666amsterdam_native_postprocess_next_apply = now + std::chrono::seconds(5);
    ++m_1666amsterdam_native_postprocess_cvar_attempts;

    const auto console_manager = sdk::FConsoleManager::get();
    if (console_manager == nullptr) {
        if (m_1666amsterdam_native_postprocess_cvar_attempts == 1 ||
            (m_1666amsterdam_native_postprocess_cvar_attempts % 120) == 0) {
            SPDLOG_WARN("[1666Amsterdam][NativePostProcess] FConsoleManager unavailable");
        }
        return;
    }

    struct ForcedCVar {
        const wchar_t* name;
        int value;
    };

    // Preserve the full tonemap/exposure chain, but bypass temporal history and
    // temporal upscaling. FXAA is spatial and therefore cannot consume a stale
    // or primary-eye-only history texture.
    static constexpr std::array<ForcedCVar, 3> forced_cvars{{
        ForcedCVar{L"r.AntiAliasingMethod", 1},
        ForcedCVar{L"r.TemporalAA.Upsampling", 0},
        ForcedCVar{L"r.TemporalAA.Upscaler", 0},
    }};

    uint32_t found{};
    uint32_t already_ok{};
    uint32_t set_ok{};
    uint32_t set_failed{};
    uint32_t missing{};

    for (const auto& forced : forced_cvars) {
        const std::wstring cvar_name{forced.name};
        auto* object = console_manager->find(cvar_name);
        if (object == nullptr || object->AsCommand() != nullptr) {
            ++missing;
            continue;
        }

        ++found;
        auto* variable = (sdk::IConsoleVariable*)object;
        int before{};
        int after{};
        bool ok{};

        try {
            before = variable->GetInt();
            if (!m_1666amsterdam_native_postprocess_previous_ints.contains(cvar_name)) {
                m_1666amsterdam_native_postprocess_previous_ints.emplace(cvar_name, before);
            }

            if (before == forced.value) {
                ok = true;
                ++already_ok;
            } else {
                ok = variable->Set(std::to_wstring(forced.value).c_str());
            }

            after = variable->GetInt();
        } catch (...) {
            ok = false;
        }

        if (!ok || after != forced.value) {
            ++set_failed;
        } else if (before != forced.value) {
            ++set_ok;
        }

        if (!m_1666amsterdam_native_postprocess_cvars_logged) {
            SPDLOG_INFO(
                "[1666Amsterdam][NativePostProcess] forced {}: before={} requested={} after={} ok={}",
                utility::narrow(forced.name),
                before,
                forced.value,
                after,
                ok && after == forced.value);
        }
    }

    if (found == 0) {
        if (!m_1666amsterdam_native_postprocess_cvars_logged ||
            (m_1666amsterdam_native_postprocess_cvar_attempts % 120) == 0) {
            SPDLOG_WARN(
                "[1666Amsterdam][NativePostProcess] No target cvars found attempt={} missing={}",
                m_1666amsterdam_native_postprocess_cvar_attempts,
                missing);
        }
        return;
    }

    if (!m_1666amsterdam_native_postprocess_cvars_logged) {
        SPDLOG_INFO(
            "[1666Amsterdam][NativePostProcess] Applied FXAA temporal bypass found={} missing={} already_ok={} set_ok={} set_failed={}",
            found,
            missing,
            already_ok,
            set_ok,
            set_failed);
        m_1666amsterdam_native_postprocess_cvars_logged = true;
    }

    m_1666amsterdam_native_postprocess_cvars_applied = true;
}

void VR::restore_daysgone_gbuffer_cvar() {
    if (!m_daysgone_gbuffer_cvar_applied) {
        m_daysgone_gbuffer_cvar_logged = false;
        m_daysgone_gbuffer_previous_valid = false;
        m_daysgone_gbuffer_previous_value = 1;
        m_daysgone_gbuffer_cvar_attempts = 0;
        m_daysgone_gbuffer_next_apply = {};
        return;
    }

    const auto console_manager = sdk::FConsoleManager::get();
    if (console_manager == nullptr) {
        return;
    }

    auto* object = console_manager->find(L"r.GBuffer");
    bool restored{};
    bool failed{};

    if (object != nullptr && object->AsCommand() == nullptr && m_daysgone_gbuffer_previous_valid) {
        auto* variable = (sdk::IConsoleVariable*)object;

        try {
            restored = variable->Set(std::to_wstring(m_daysgone_gbuffer_previous_value).c_str());
        } catch (...) {
            restored = false;
        }

        failed = !restored;
    }

    SPDLOG_INFO(
        "[DaysGone][GBufferSafeMode] Restored r.GBuffer previous={} restored={} failed={} had_previous={}",
        m_daysgone_gbuffer_previous_value,
        restored,
        failed,
        m_daysgone_gbuffer_previous_valid);

    m_daysgone_gbuffer_cvar_applied = false;
    m_daysgone_gbuffer_cvar_logged = false;
    m_daysgone_gbuffer_previous_valid = false;
    m_daysgone_gbuffer_previous_value = 1;
    m_daysgone_gbuffer_cvar_attempts = 0;
    m_daysgone_gbuffer_next_apply = {};
}

void VR::update_daysgone_gbuffer_compatibility(sdk::UGameEngine* engine) {
    (void)engine;

    const bool active =
        is_daysgone_executable() &&
        g_framework != nullptr &&
        g_framework->is_dx11() &&
        is_hmd_active() &&
        m_compatibility_daysgone_gbuffer_safe_mode->value();

    if (!active) {
        restore_daysgone_gbuffer_cvar();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_daysgone_gbuffer_next_apply != std::chrono::steady_clock::time_point{} &&
        now < m_daysgone_gbuffer_next_apply) {
        return;
    }

    // Days Gone's black road/terrain patches were narrowed to the deferred
    // GBuffer path. Reapply sparingly to correct drift without touching a hot path.
    m_daysgone_gbuffer_next_apply = now + std::chrono::seconds(5);
    ++m_daysgone_gbuffer_cvar_attempts;

    const auto console_manager = sdk::FConsoleManager::get();
    if (console_manager == nullptr) {
        if (m_daysgone_gbuffer_cvar_attempts == 1 || (m_daysgone_gbuffer_cvar_attempts % 120) == 0) {
            SPDLOG_WARN("[DaysGone][GBufferSafeMode] FConsoleManager unavailable; cannot apply r.GBuffer yet");
        }
        return;
    }

    auto* object = console_manager->find(L"r.GBuffer");
    if (object == nullptr || object->AsCommand() != nullptr) {
        if (!m_daysgone_gbuffer_cvar_logged || (m_daysgone_gbuffer_cvar_attempts % 120) == 0) {
            SPDLOG_WARN("[DaysGone][GBufferSafeMode] r.GBuffer cvar not found");
        }
        return;
    }

    auto* variable = (sdk::IConsoleVariable*)object;
    int before{};
    int after{};
    bool ok{};

    try {
        before = variable->GetInt();

        if (!m_daysgone_gbuffer_previous_valid) {
            m_daysgone_gbuffer_previous_valid = true;
            m_daysgone_gbuffer_previous_value = before;
        }

        if (before == 0) {
            ok = true;
        } else {
            ok = variable->Set(L"0");
            CVarManager::record_global_change(L"r.GBuffer", L"0", "daysgone_gbuffer_safe_mode");
        }

        after = variable->GetInt();
    } catch (...) {
        ok = false;
    }

    if (!m_daysgone_gbuffer_cvar_logged) {
        SPDLOG_INFO(
            "[DaysGone][GBufferSafeMode] Applied r.GBuffer=0 before={} after={} ok={} previous={}",
            before,
            after,
            ok,
            m_daysgone_gbuffer_previous_value);
        m_daysgone_gbuffer_cvar_logged = true;
    } else if (!ok && (m_daysgone_gbuffer_cvar_attempts % 120) == 0) {
        SPDLOG_WARN(
            "[DaysGone][GBufferSafeMode] Failed to maintain r.GBuffer=0 attempt={} before={} after={}",
            m_daysgone_gbuffer_cvar_attempts,
            before,
            after);
    }

    if (ok) {
        m_daysgone_gbuffer_cvar_applied = true;
    }
}

void VR::update_everspace2_cinematic_bars(sdk::UGameEngine* engine) {
    (void)engine;

    auto& state = m_everspace2_cinematic_bars;
    const bool enabled =
        is_everspace2_executable_cached() &&
        m_compatibility_everspace2_remove_cinematic_bars->value();

    if (!enabled) {
        if (state.was_enabled) {
            state = {};
        }
        return;
    }

    if (!state.was_enabled) {
        state = {};
        state.was_enabled = true;
        SPDLOG_INFO("[Everspace2][CinematicBars] Compatibility enabled; waiting for WG_Ingame_HUD");
    }

    if (state.processed_hud != nullptr &&
        is_live_uobject_identity(
            (sdk::UObject*)state.processed_hud,
            state.processed_index,
            state.processed_serial)) {
        return;
    }

    state.processed_hud = nullptr;
    state.processed_index = -1;
    state.processed_serial = 0;

    const auto now = std::chrono::steady_clock::now();
    if (state.hud_class == nullptr) {
        if (state.next_class_lookup != std::chrono::steady_clock::time_point{} &&
            now < state.next_class_lookup) {
            return;
        }

        state.next_class_lookup = now + std::chrono::seconds(2);
        state.hud_class = sdk::find_uobject<sdk::UClass>(
            L"WidgetBlueprintGeneratedClass /Game/Blueprints/UI/HUD/WG_Ingame_HUD.WG_Ingame_HUD_C",
            true);
        if (state.hud_class == nullptr) {
            return;
        }

        state.scan_cursor = -1;
    }

    const auto hud_class = (sdk::UClass*)state.hud_class;
    const auto objects = sdk::FUObjectArray::get();
    if (objects == nullptr) {
        return;
    }

    if (state.next_scan != std::chrono::steady_clock::time_point{} && now < state.next_scan) {
        return;
    }

    const auto object_count = objects->get_object_count();
    const auto try_remove_from_hud = [&](sdk::UObject* object, int32_t index, int32_t serial) {
        if (object == nullptr || object == hud_class->get_class_default_object() ||
            !is_live_uobject_identity(object, index, serial) ||
            object->get_class() != hud_class) {
            return false;
        }

        if (!remove_everspace2_cinematic_bars(object)) {
            if (!state.invalid_layout_logged) {
                SPDLOG_WARN(
                    "[Everspace2][CinematicBars] Exact WG_Ingame_HUD bar layout/function validation failed; leaving UI unchanged");
                state.invalid_layout_logged = true;
            }
            return false;
        }

        state.processed_hud = object;
        state.processed_index = index;
        state.processed_serial = serial;
        ++state.removed_instances;
        SPDLOG_INFO(
            "[Everspace2][CinematicBars] Removed BarImageTop/BarImageBottom from {} instance={} index={} serial={}",
            get_log_object_name(object),
            state.removed_instances,
            state.processed_index,
            state.processed_serial);
        return true;
    };

    // UObjectHook already maintains this exact class set in normal ES2 runs.
    // Use it first so enabling the checkbox in-game reacts immediately.
    if (const auto object_hook = UObjectHook::get();
        object_hook != nullptr && object_hook->is_fully_hooked() && !object_hook->is_disabled()) {
        for (const auto object_base : object_hook->get_objects_by_class(hud_class)) {
            auto object = (sdk::UObject*)object_base;
            if (object == nullptr || !object_hook->exists(object)) {
                continue;
            }

            const auto index = (int32_t)object->get_internal_index();
            const auto item = index >= 0 && index < object_count ? objects->get_object(index) : nullptr;
            if (item != nullptr && try_remove_from_hud(object, index, item->get_serial_number())) {
                return;
            }
        }
    }

    // Fail-safe fallback searches newest objects first. Runtime HUD instances
    // live near the end of GUObjectArray, so a live toggle should not wait for
    // a full 600k-object ascending sweep.
    constexpr int32_t OBJECTS_PER_TICK = 4096;
    if (state.scan_cursor <= 0 || state.scan_cursor > object_count) {
        state.scan_cursor = object_count;
    }
    const auto scan_start = std::max(0, state.scan_cursor - OBJECTS_PER_TICK);

    for (auto index = state.scan_cursor - 1; index >= scan_start; --index) {
        const auto item = objects->get_object(index);
        if (item == nullptr) {
            continue;
        }

        auto object = (sdk::UObject*)item->get_object();
        if (try_remove_from_hud(object, index, item->get_serial_number())) {
            return;
        }
    }

    state.scan_cursor = scan_start;
    if (state.scan_cursor == 0) {
        state.scan_cursor = object_count;
        state.next_scan = now + std::chrono::milliseconds(500);
    }
}

void VR::on_post_engine_tick(sdk::UGameEngine* engine, float delta) {
    ZoneScopedN(__FUNCTION__);

    if (!get_runtime()->loaded || !is_hmd_active()) {
        return;
    }

    // Some Supermassive camera paths rewrite crop/aspect state during engine
    // tick. Reapply the opt-in compatibility after game tick, but keep it to
    // the active camera only; broad object sweeps caused cadence/flicker issues.
    update_fullscreen_16x9_camera_compatibility(engine);
}

void VR::update_shf_auto_2d_mode(sdk::UGameEngine* engine) {
    if (!is_shf_executable()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_shf_auto_2d_last_sample.time_since_epoch().count() != 0 &&
        now - m_shf_auto_2d_last_sample < std::chrono::milliseconds(250)) {
        return;
    }

    m_shf_auto_2d_last_sample = now;

    const auto decision = evaluate_shf_auto_2d(engine);

    if (decision.should_force) {
        if (!m_shf_auto_2d_active) {
            m_shf_auto_2d_previous_mode = m_2d_screen_mode->value();
            m_shf_auto_2d_active = true;
            spdlog::info(
                "[SHf][Auto2D] active=true previous={} cutscene={} fov={:.3f} url={} target={}",
                m_shf_auto_2d_previous_mode,
                utility::narrow(decision.cutscene),
                decision.fov.value_or(0.0f),
                utility::narrow(decision.url),
                decision.target.empty() ? "unresolved" : utility::narrow(decision.target));
        }

        m_2d_screen_mode->value() = true;
        return;
    }

    if (m_shf_auto_2d_active) {
        m_2d_screen_mode->value() = m_shf_auto_2d_previous_mode;
        m_shf_auto_2d_active = false;
        spdlog::info(
            "[SHf][Auto2D] active=false restored={} cutscene={} fov={} url={} target={}",
            m_shf_auto_2d_previous_mode,
            utility::narrow(decision.cutscene),
            decision.fov.has_value() ? std::format("{:.3f}", *decision.fov) : "unresolved",
            decision.url.empty() ? "unresolved" : utility::narrow(decision.url),
            decision.target.empty() ? "unresolved" : utility::narrow(decision.target));
    }
}

void VR::update_dispatch_auto_2d_mode(sdk::UGameEngine* engine) {
    if (!is_dispatch_executable()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_dispatch_auto_2d_last_sample.time_since_epoch().count() != 0 &&
        now - m_dispatch_auto_2d_last_sample < std::chrono::milliseconds(250)) {
        return;
    }

    m_dispatch_auto_2d_last_sample = now;

    const auto decision = evaluate_dispatch_auto_2d(engine);

    if (decision.should_force) {
        if (!m_dispatch_auto_2d_active) {
            m_dispatch_auto_2d_previous_mode = m_2d_screen_mode->value();
            m_dispatch_auto_2d_active = true;
            spdlog::info(
                "[Dispatch][Auto2D] active=true previous={} reason={} subsystem={} source={} player={} texture={} playing={} preparing={} buffering={} ready={}",
                m_dispatch_auto_2d_previous_mode,
                decision.reason,
                decision.subsystem.empty() ? "unresolved" : decision.subsystem,
                decision.source.empty() ? "unresolved" : decision.source,
                decision.player.empty() ? "unresolved" : decision.player,
                decision.texture.empty() ? "unresolved" : decision.texture,
                decision.playing.has_value() ? (*decision.playing ? "true" : "false") : "unresolved",
                decision.preparing.has_value() ? (*decision.preparing ? "true" : "false") : "unresolved",
                decision.buffering.has_value() ? (*decision.buffering ? "true" : "false") : "unresolved",
                decision.ready.has_value() ? (*decision.ready ? "true" : "false") : "unresolved");
        }

        m_2d_screen_mode->value() = true;
        return;
    }

    if (m_dispatch_auto_2d_active) {
        m_2d_screen_mode->value() = m_dispatch_auto_2d_previous_mode;
        m_dispatch_auto_2d_active = false;
        spdlog::info("[Dispatch][Auto2D] active=false restored={}", m_dispatch_auto_2d_previous_mode);
    }
}

void VR::update_mixtape_auto_2d_mode(sdk::UGameEngine* engine) {
    if (!is_mixtape_executable()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_mixtape_auto_2d_last_sample.time_since_epoch().count() != 0 &&
        now - m_mixtape_auto_2d_last_sample < std::chrono::milliseconds(250)) {
        return;
    }

    m_mixtape_auto_2d_last_sample = now;

    const auto decision = evaluate_mixtape_auto_2d(engine);

    if (decision.should_force) {
        if (!m_mixtape_auto_2d_active.load(std::memory_order_relaxed)) {
            m_mixtape_auto_2d_previous_mode = m_2d_screen_mode->value();
            m_mixtape_auto_2d_active.store(true, std::memory_order_relaxed);
            spdlog::info(
                "[Mixtape][Auto2D] active=true previous={} reason={} player={} url={} playing={} preparing={} buffering={} ready={}",
                m_mixtape_auto_2d_previous_mode,
                decision.reason,
                decision.player.empty() ? "unresolved" : decision.player,
                decision.url.empty() ? "unresolved" : decision.url,
                decision.playing.has_value() ? (*decision.playing ? "true" : "false") : "unresolved",
                decision.preparing.has_value() ? (*decision.preparing ? "true" : "false") : "unresolved",
                decision.buffering.has_value() ? (*decision.buffering ? "true" : "false") : "unresolved",
                decision.ready.has_value() ? (*decision.ready ? "true" : "false") : "unresolved");
        }

        m_2d_screen_mode->value() = true;
        return;
    }

    if (m_mixtape_auto_2d_active.exchange(false, std::memory_order_relaxed)) {
        m_2d_screen_mode->value() = m_mixtape_auto_2d_previous_mode;
        spdlog::info("[Mixtape][Auto2D] active=false restored={}", m_mixtape_auto_2d_previous_mode);
    }
}

void VR::update_halo_electra_cinematic_state(sdk::UGameEngine* engine) {
    (void)engine;

    if (!is_halo_campaign_evolved_executable() ||
        g_framework == nullptr ||
        !g_framework->is_dx12())
    {
        return;
    }

    if (!ensure_halo_electra_native_hooks()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto active_player = g_halo_electra_active_player.load(std::memory_order_acquire);

    if (active_player != 0) {
        m_halo_electra_restore_after = {};

        if (!m_halo_electra_cinematic_active.exchange(true, std::memory_order_relaxed)) {
            spdlog::info(
                "[Halo][Electra] renderer-quad=true reason=classified-prerender player={:x} frames={} flushes={}",
                active_player,
                g_halo_electra_present_count.load(std::memory_order_relaxed),
                g_halo_electra_flush_count.load(std::memory_order_relaxed));
        }
        return;
    }

    if (!m_halo_electra_cinematic_active.load(std::memory_order_relaxed)) {
        return;
    }

    constexpr auto RESTORE_DEBOUNCE = std::chrono::milliseconds(750);
    if (m_halo_electra_restore_after.time_since_epoch().count() == 0) {
        m_halo_electra_restore_after = now + RESTORE_DEBOUNCE;
    }

    if (now < m_halo_electra_restore_after) {
        return;
    }

    if (m_halo_electra_cinematic_active.exchange(false, std::memory_order_relaxed)) {
        m_halo_electra_restore_after = {};
        spdlog::info(
            "[Halo][Electra] renderer-quad=false frames={} flushes={}",
            g_halo_electra_present_count.load(std::memory_order_relaxed),
            g_halo_electra_flush_count.load(std::memory_order_relaxed));
    }
}

void VR::set_windrose_meta_ui_2d_state_active(
    std::string_view state_name,
    uintptr_t state_id,
    std::string_view source,
    bool force_2d,
    bool active)
{
    if (m_rendering_method->value() != RenderingMethod::NATIVE_STEREO) {
        return;
    }

    std::scoped_lock _{m_windrose_meta_ui_auto_2d_mtx};

    const std::string key{state_name};
    const std::string source_key{source};

    if (active && force_2d) {
        m_windrose_meta_ui_auto_2d_tokens[state_id] = WindroseMetaUiToken{
            key,
            source_key,
            std::chrono::steady_clock::now()};
        m_windrose_meta_ui_auto_2d_last_state = key;
        m_windrose_meta_ui_auto_2d_last_source = source_key;
        m_windrose_meta_ui_auto_2d_restore_after = {};

        if (!m_windrose_meta_ui_auto_2d_active) {
            m_windrose_meta_ui_auto_2d_previous_mode = m_2d_screen_mode->value();
            m_windrose_meta_ui_auto_2d_active = true;
            spdlog::info(
                "[Windrose][MetaUI2D] active=true previous={} state={} source={} tokens={}",
                m_windrose_meta_ui_auto_2d_previous_mode,
                key,
                source_key,
                m_windrose_meta_ui_auto_2d_tokens.size());
        }

        if (!m_2d_screen_mode->value()) {
            m_2d_screen_mode->value() = true;
        }
        return;
    }

    if (active && !force_2d) {
        // Windrose sends Adventure/NPC/cutscene transitions through the same HFSM path
        // as fullscreen menus. Treat them as a hard boundary so stale menu tokens cannot
        // keep the whole scene forced into 2D after dialogue or cutscene playback.
        if (m_windrose_meta_ui_auto_2d_active || !m_windrose_meta_ui_auto_2d_tokens.empty()) {
            const auto cleared = m_windrose_meta_ui_auto_2d_tokens.size();
            m_windrose_meta_ui_auto_2d_tokens.clear();
            m_windrose_meta_ui_auto_2d_last_state = key;
            m_windrose_meta_ui_auto_2d_last_source = source_key;
            m_windrose_meta_ui_auto_2d_restore_after = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
            ++m_windrose_meta_ui_auto_2d_stale_clears;
            spdlog::info(
                "[Windrose][MetaUI2D] stale clear state={} source={} cleared={} pending_restore_ms=150 stale_clears={}",
                key,
                source_key,
                cleared,
                m_windrose_meta_ui_auto_2d_stale_clears);
        }
        return;
    }

    if (!force_2d) {
        return;
    }

    auto erased = m_windrose_meta_ui_auto_2d_tokens.erase(state_id);
    if (erased == 0 && !key.empty()) {
        for (auto it = m_windrose_meta_ui_auto_2d_tokens.begin(); it != m_windrose_meta_ui_auto_2d_tokens.end(); ++it) {
            if (it->second.name == key && it->second.source == source_key) {
                m_windrose_meta_ui_auto_2d_tokens.erase(it);
                erased = 1;
                break;
            }
        }
    }

    if (m_windrose_meta_ui_auto_2d_tokens.empty()) {
        // Tab switches can emit Exit then Enter in the same tick; defer restore a touch
        // so we do not flap 2D mode while R5 swaps HFSM states.
        m_windrose_meta_ui_auto_2d_restore_after = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
        m_windrose_meta_ui_auto_2d_last_state = key;
        m_windrose_meta_ui_auto_2d_last_source = source_key;
        spdlog::info(
            "[Windrose][MetaUI2D] pending restore state={} source={} erased={}",
            key,
            source_key,
            erased);
    }
}

void VR::update_windrose_meta_ui_auto_2d_mode() {
    std::scoped_lock _{m_windrose_meta_ui_auto_2d_mtx};

    if (!m_windrose_meta_ui_auto_2d_active) {
        return;
    }

    if (!m_windrose_meta_ui_auto_2d_tokens.empty()) {
        if (!m_2d_screen_mode->value()) {
            m_2d_screen_mode->value() = true;
        }
        return;
    }

    if (m_windrose_meta_ui_auto_2d_restore_after.time_since_epoch().count() == 0 ||
        std::chrono::steady_clock::now() < m_windrose_meta_ui_auto_2d_restore_after)
    {
        if (!m_2d_screen_mode->value()) {
            m_2d_screen_mode->value() = true;
        }
        return;
    }

    m_2d_screen_mode->value() = m_windrose_meta_ui_auto_2d_previous_mode;
    spdlog::info(
        "[Windrose][MetaUI2D] active=false restored={} last_state={} last_source={} stale_clears={}",
        m_windrose_meta_ui_auto_2d_previous_mode,
        m_windrose_meta_ui_auto_2d_last_state,
        m_windrose_meta_ui_auto_2d_last_source,
        m_windrose_meta_ui_auto_2d_stale_clears);

    m_windrose_meta_ui_auto_2d_active = false;
    m_windrose_meta_ui_auto_2d_previous_mode = false;
    m_windrose_meta_ui_auto_2d_restore_after = {};
    m_windrose_meta_ui_auto_2d_last_state.clear();
    m_windrose_meta_ui_auto_2d_last_source.clear();
}

void VR::clear_windrose_meta_ui_2d_state(std::string_view reason) {
    std::scoped_lock _{m_windrose_meta_ui_auto_2d_mtx};

    if (!m_windrose_meta_ui_auto_2d_active && m_windrose_meta_ui_auto_2d_tokens.empty()) {
        return;
    }

    const auto cleared = m_windrose_meta_ui_auto_2d_tokens.size();
    m_windrose_meta_ui_auto_2d_tokens.clear();
    m_windrose_meta_ui_auto_2d_restore_after = {};
    m_2d_screen_mode->value() = m_windrose_meta_ui_auto_2d_previous_mode;
    spdlog::info(
        "[Windrose][MetaUI2D] manual clear reason={} restored={} cleared={}",
        std::string{reason},
        m_windrose_meta_ui_auto_2d_previous_mode,
        cleared);

    m_windrose_meta_ui_auto_2d_active = false;
    m_windrose_meta_ui_auto_2d_previous_mode = false;
    m_windrose_meta_ui_auto_2d_last_state.clear();
    m_windrose_meta_ui_auto_2d_last_source.clear();
}

std::string VR::get_windrose_meta_ui_2d_status_text() const {
    std::scoped_lock _{m_windrose_meta_ui_auto_2d_mtx};

    std::ostringstream out;
    out << (m_windrose_meta_ui_auto_2d_active ? "active" : "inactive")
        << " tokens=" << m_windrose_meta_ui_auto_2d_tokens.size();

    if (m_windrose_meta_ui_auto_2d_restore_after.time_since_epoch().count() != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now < m_windrose_meta_ui_auto_2d_restore_after) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                m_windrose_meta_ui_auto_2d_restore_after - now).count();
            out << " restore_in_ms=" << remaining;
        }
    }

    if (!m_windrose_meta_ui_auto_2d_last_state.empty()) {
        out << " last=" << m_windrose_meta_ui_auto_2d_last_state;
    }

    if (!m_windrose_meta_ui_auto_2d_last_source.empty()) {
        out << " source=" << m_windrose_meta_ui_auto_2d_last_source;
    }

    out << " stale_clears=" << m_windrose_meta_ui_auto_2d_stale_clears;
    return out.str();
}

// The ACTIVE camera component's own FoV-axis override, if it has one.
//
// A CameraComponent can set bOverrideAspectRatioAxisConstraint and carry its own
// AspectRatioAxisConstraint, so a cutscene camera can use a different axis than
// the local player without ever touching the local player's value. This is
// checked first; nullopt means "no override in play", and the caller falls back
// to the global ULocalPlayer constraint.
//
// Lives here (not in VR_Flat3D.cpp) because the object/function helpers it needs
// are internal to this TU — the same pair the 16:9 camera compat path uses to
// reach the live camera component.
std::optional<bool> VR::camera_component_fov_is_vertical() try {
    const auto engine = sdk::UEngine::get();
    if (engine == nullptr) {
        return std::nullopt;
    }

    const auto world = engine->get_world();
    const auto gameplay = sdk::UGameplayStatics::get();
    if (world == nullptr || gameplay == nullptr) {
        return std::nullopt;
    }

    const auto pc = gameplay->get_player_controller(world, 0);
    if (pc == nullptr) {
        return std::nullopt;
    }

    const auto pcm = pc->get_player_camera_manager();
    if (pcm == nullptr) {
        return std::nullopt;
    }

    // Not a stock UE function — plenty of titles won't have it, in which case
    // there is no per-camera override to find and the global value stands.
    const auto camera = call_object_object_function((sdk::UObject*)pcm, L"GetCurrentCamera");
    if (!camera.has_value()) {
        return std::nullopt;
    }

    const auto component = read_object_property(*camera, L"CameraComponent");
    if (!component.has_value()) {
        return std::nullopt;
    }

    auto* comp = *component;
    const auto klass = comp->get_class();
    if (klass == nullptr) {
        return std::nullopt;
    }

    static sdk::UClass* cached_class = nullptr;
    static sdk::FProperty* override_prop = nullptr;
    static sdk::FProperty* constraint_prop = nullptr;

    if (klass != cached_class) {
        cached_class = klass;
        override_prop = klass->find_property(L"bOverrideAspectRatioAxisConstraint");
        constraint_prop = klass->find_property(L"AspectRatioAxisConstraint");
    }

    if (override_prop == nullptr || constraint_prop == nullptr ||
        override_prop->get_class() == nullptr ||
        override_prop->get_class()->get_name().to_string() != L"BoolProperty")
    {
        return std::nullopt;
    }

    if (!((sdk::FBoolProperty*)override_prop)->get_value_from_object(comp)) {
        return std::nullopt; // component defers to the local player
    }

    // TEnumAsByte<EAspectRatioAxisConstraint>: 0 MaintainYFOV = vertical.
    return *constraint_prop->get_data<uint8_t>(comp) == 0;
} catch (...) {
    return std::nullopt;
}

void VR::update_fullscreen_16x9_camera_compatibility(sdk::UGameEngine* engine) {
    if (!m_compatibility_fullscreen_16x9_cameras->value()) {
        m_fullscreen_16x9_camera_compat = {};
        return;
    }

    constexpr auto camera_poll_interval = std::chrono::milliseconds(100);
    constexpr auto transition_burst_duration = std::chrono::milliseconds(500);
    constexpr auto keepalive_interval = std::chrono::milliseconds(1000);
    const auto now = std::chrono::steady_clock::now();

    auto world = engine != nullptr ? engine->get_world() : nullptr;
    auto gameplay = sdk::UGameplayStatics::get();

    if (world == nullptr || gameplay == nullptr) {
        return;
    }

    auto pc = gameplay->get_player_controller(world, 0);
    if (pc == nullptr) {
        return;
    }

    auto pcm = pc->get_player_camera_manager();
    if (pcm == nullptr) {
        return;
    }

    auto aspect_ratio = m_compatibility_fullscreen_16x9_camera_aspect->value();
    if (!std::isfinite(aspect_ratio) || aspect_ratio <= 0.1f) {
        const auto runtime = get_runtime();
        if (runtime != nullptr && runtime->get_height() > 0) {
            aspect_ratio = (float)runtime->get_width() / (float)runtime->get_height();
        } else {
            aspect_ratio = 16.0f / 9.0f;
        }
    }

    aspect_ratio = std::clamp(aspect_ratio, 0.5f, 4.0f);

    auto& state = m_fullscreen_16x9_camera_compat;
    const bool just_enabled = !state.was_enabled;
    const bool pcm_changed = state.last_pcm != pcm;
    const bool aspect_changed = std::abs(state.last_aspect - aspect_ratio) > 0.001f;
    const bool should_poll_camera =
        just_enabled ||
        pcm_changed ||
        aspect_changed ||
        state.last_camera_poll.time_since_epoch().count() == 0 ||
        now - state.last_camera_poll >= camera_poll_interval ||
        now < state.burst_until;

    sdk::UObject* current_camera = (sdk::UObject*)state.last_camera;
    sdk::UObject* camera_component = (sdk::UObject*)state.last_camera_component;

    if (should_poll_camera) {
        state.last_camera_poll = now;

        if (auto camera = call_object_object_function((sdk::UObject*)pcm, L"GetCurrentCamera"); camera.has_value()) {
            current_camera = *camera;
        } else {
            current_camera = nullptr;
        }

        if (current_camera != nullptr) {
            if (auto component = read_object_property(current_camera, L"CameraComponent"); component.has_value()) {
                camera_component = *component;
            } else {
                camera_component = nullptr;
            }
        } else {
            camera_component = nullptr;
        }
    }

    const bool camera_changed = state.last_camera != current_camera;
    const bool component_changed = state.last_camera_component != camera_component;
    const bool keepalive_due =
        state.last_apply.time_since_epoch().count() == 0 ||
        now - state.last_apply >= keepalive_interval;
    const bool in_transition_burst = now < state.burst_until;

    if (just_enabled || pcm_changed || camera_changed || component_changed || aspect_changed) {
        state.burst_until = now + transition_burst_duration;
    }

    const bool should_apply =
        just_enabled ||
        pcm_changed ||
        camera_changed ||
        component_changed ||
        aspect_changed ||
        in_transition_burst ||
        keepalive_due;

    state.was_enabled = true;
    state.last_pcm = pcm;
    state.last_camera = current_camera;
    state.last_camera_component = camera_component;
    state.last_aspect = aspect_ratio;

    if (!should_apply) {
        return;
    }

    bool wrote_any = false;
    wrote_any |= write_object_bool_property((sdk::UObject*)pcm, L"bUse16_9CamerasAsFullscreen", true);
    wrote_any |= write_object_bool_property((sdk::UObject*)pcm, L"bForceOutputToConstraintXFov", false);
    wrote_any |= write_game_camera_aspect_constraints(pcm, aspect_ratio);

    if (current_camera != nullptr) {
        wrote_any |= write_object_bool_property(current_camera, L"bEnableCameraViewportRemapPPMI", false);

        if (camera_component != nullptr) {
            wrote_any |= write_camera_component_fullscreen_aspect(camera_component, aspect_ratio);
        }
    }

    state.last_apply = now;

    if (is_directive8020_executable_cached()) {
        if (wrote_any &&
            (state.last_log.time_since_epoch().count() == 0 || now - state.last_log >= std::chrono::seconds(5))) {
            state.last_log = now;
            SPDLOG_INFO(
                "[Directive8020][AspectCompat] aspect={:.3f} reason={}{}{}{}{}{} current_camera={} component={}",
                aspect_ratio,
                just_enabled ? "enabled " : "",
                pcm_changed ? "pcm " : "",
                camera_changed ? "camera " : "",
                component_changed ? "component " : "",
                aspect_changed ? "aspect " : "",
                keepalive_due ? "keepalive" : (in_transition_burst ? "burst" : "apply"),
                current_camera != nullptr,
                camera_component != nullptr);
        }
    }

    if (wrote_any) {
        SPDLOG_INFO_ONCE("[Compatibility] Fullscreen 16:9 Cameras active; aspect={:.3f}, camera constraints/remap disabled where available", aspect_ratio);
    } else {
        SPDLOG_WARN_ONCE("[Compatibility] Fullscreen 16:9 Cameras is enabled, but no supported camera/aspect fields were found");
    }
}

void VR::update_game_fov() {
    const auto update_prospi_telephoto_perf_override = [&](bool should_apply) {
        const auto restore = [&]() {
            if (!m_prospi_telephoto_perf_override_applied) {
                m_match_game_fov_prospi_telephoto_perf_active.store(false, std::memory_order_relaxed);
                return;
            }

            set_runtime_cvar_float(L"r.ViewDistanceScale", m_prospi_telephoto_perf_baseline_view_distance_scale);
            set_runtime_cvar_float(L"r.StaticMeshLODDistanceScale", m_prospi_telephoto_perf_baseline_static_mesh_lod_distance_scale);
            set_runtime_cvar_int(L"r.SkeletalMeshLODBias", m_prospi_telephoto_perf_baseline_skeletal_mesh_lod_bias);
            m_prospi_telephoto_perf_override_applied = false;
            m_match_game_fov_prospi_telephoto_perf_active.store(false, std::memory_order_relaxed);
            spdlog::info(
                "[PROSPI_TELEPHOTO_PERF] active=false view_distance={:.2f} static_mesh_lod_scale={:.2f} skeletal_lod_bias={}",
                m_prospi_telephoto_perf_baseline_view_distance_scale,
                m_prospi_telephoto_perf_baseline_static_mesh_lod_distance_scale,
                m_prospi_telephoto_perf_baseline_skeletal_mesh_lod_bias
            );
        };

        if (!is_prospi_executable() || !m_match_game_fov_prospi_telephoto_perf_override->value()) {
            restore();
            return;
        }

        if (!should_apply) {
            restore();
            return;
        }

        if (!m_prospi_telephoto_perf_baselines_valid) {
            m_prospi_telephoto_perf_baseline_view_distance_scale = get_runtime_cvar_float(L"r.ViewDistanceScale").value_or(1.0f);
            m_prospi_telephoto_perf_baseline_static_mesh_lod_distance_scale = get_runtime_cvar_float(L"r.StaticMeshLODDistanceScale").value_or(1.0f);
            m_prospi_telephoto_perf_baseline_skeletal_mesh_lod_bias = get_runtime_cvar_int(L"r.SkeletalMeshLODBias").value_or(0);
            m_prospi_telephoto_perf_baselines_valid = true;
        }

        const auto target_view_distance_scale = std::clamp(m_match_game_fov_prospi_telephoto_perf_view_distance_scale->value(), 0.10f, 2.0f);
        const auto target_static_mesh_lod_distance_scale = std::clamp(m_match_game_fov_prospi_telephoto_perf_static_mesh_lod_distance_scale->value(), 0.10f, 4.0f);
        const auto target_skeletal_mesh_lod_bias = std::clamp((int)std::lround(m_match_game_fov_prospi_telephoto_perf_skeletal_mesh_lod_bias->value()), 0, 4);

        set_runtime_cvar_float(L"r.ViewDistanceScale", target_view_distance_scale);
        set_runtime_cvar_float(L"r.StaticMeshLODDistanceScale", target_static_mesh_lod_distance_scale);
        set_runtime_cvar_int(L"r.SkeletalMeshLODBias", target_skeletal_mesh_lod_bias);

        if (!m_prospi_telephoto_perf_override_applied) {
            spdlog::info(
                "[PROSPI_TELEPHOTO_PERF] active=true view_distance={:.2f} static_mesh_lod_scale={:.2f} skeletal_lod_bias={}",
                target_view_distance_scale,
                target_static_mesh_lod_distance_scale,
                target_skeletal_mesh_lod_bias
            );
        }

        m_prospi_telephoto_perf_override_applied = true;
        m_match_game_fov_prospi_telephoto_perf_active.store(true, std::memory_order_relaxed);
    };

    const auto reset_prospi_state = [&]() {
        update_prospi_telephoto_perf_override(false);
        m_match_game_fov_prospi_preset.store((int32_t)ProSpiCameraPreset::None, std::memory_order_relaxed);
        m_match_game_fov_prospi_actual_min_active.store(0.0f, std::memory_order_relaxed);
        m_match_game_fov_prospi_calibration_applied.store(false, std::memory_order_relaxed);
        m_match_game_fov_prospi_calibration_dolly_distance_active.store(0.0f, std::memory_order_relaxed);
        m_match_game_fov_prospi_calibration_multiplier_active.store(1.0f, std::memory_order_relaxed);
        m_match_game_fov_prospi_calibration_actual_min_active.store(0.0f, std::memory_order_relaxed);
        m_match_game_fov_prospi_tv_override_active.store(false, std::memory_order_relaxed);
        m_match_game_fov_prospi_auto_dolly_distance_active.store(0.0f, std::memory_order_relaxed);
        m_match_game_fov_prospi_telephoto_perf_active.store(false, std::memory_order_relaxed);
        m_match_game_fov_read_only_camera_active.store(false, std::memory_order_relaxed);
        m_match_game_fov_would_write_game_camera.store(false, std::memory_order_relaxed);
        m_match_game_fov_camera_cut_stabilizer_active.store(false, std::memory_order_relaxed);
        m_match_game_fov_camera_cut_stabilizer_remaining_ms.store(0, std::memory_order_relaxed);
        m_match_game_fov_generic_camera_preset_applied.store(false, std::memory_order_relaxed);
        m_match_game_fov_generic_camera_tracking_active.store(false, std::memory_order_relaxed);

        std::scoped_lock _{m_prospi_camera_calibration_mtx};
        m_prospi_current_camera_id.clear();
        m_prospi_sticky_preset_valid = false;
        m_prospi_sticky_preset = (int32_t)ProSpiCameraPreset::None;
        m_prospi_sticky_location = {};
        m_prospi_sticky_rotation = {};
        m_prospi_sticky_raw_fov = 0.0f;
        m_prospi_sticky_calibration_valid = false;
        m_prospi_sticky_camera_id.clear();

        {
            std::scoped_lock generic_lock{m_generic_camera_preset_mtx};
            m_current_game_camera_id.clear();
            m_active_generic_camera_preset = {};
        }

        m_camera_cut_state = {};
    };

    if (!m_match_game_fov->value()) {
        m_game_fov_valid.store(false, std::memory_order_relaxed);
        m_game_fov_raw.store(0.0f, std::memory_order_relaxed);
        m_game_fov_dolly_offset.store(0.0f, std::memory_order_relaxed);
        reset_prospi_state();
        return;
    }

    auto engine = sdk::UEngine::get();
    auto world = engine != nullptr ? engine->get_world() : nullptr;
    auto gameplay = sdk::UGameplayStatics::get();

    if (world == nullptr || gameplay == nullptr) {
        m_game_fov_valid.store(false, std::memory_order_relaxed);
        reset_prospi_state();
        return;
    }

    auto pc = gameplay->get_player_controller(world, 0);
    if (pc == nullptr) {
        m_game_fov_valid.store(false, std::memory_order_relaxed);
        reset_prospi_state();
        return;
    }

    auto pcm = pc->get_player_camera_manager();
    if (pcm == nullptr) {
        m_game_fov_valid.store(false, std::memory_order_relaxed);
        reset_prospi_state();
        return;
    }

    auto fov = read_game_fov(pcm);
    if (!fov.has_value()) {
        m_game_fov_valid.store(false, std::memory_order_relaxed);
        m_game_fov_raw.store(0.0f, std::memory_order_relaxed);
        m_game_fov_dolly_offset.store(0.0f, std::memory_order_relaxed);
        reset_prospi_state();
        return;
    }

    if (!std::isfinite(*fov) || *fov <= 0.01f || *fov >= 179.0f) {
        m_game_fov_valid.store(false, std::memory_order_relaxed);
        m_game_fov_raw.store(0.0f, std::memory_order_relaxed);
        m_game_fov_dolly_offset.store(0.0f, std::memory_order_relaxed);
        reset_prospi_state();
        return;
    }

    const auto raw_fov = *fov;
    m_game_fov_raw.store(raw_fov, std::memory_order_relaxed);

    auto game_fov_for_matching = raw_fov;
    auto active_fov_multiplier = std::clamp(m_match_game_fov_multiplier->value(), 0.1f, 3.0f);
    auto active_dolly_distance = std::clamp(m_match_game_fov_dolly_distance->value(), 10.0f, 50000.0f);
    auto effective_fov = raw_fov * active_fov_multiplier;
    if (!std::isfinite(effective_fov)) {
        m_game_fov_valid.store(false, std::memory_order_relaxed);
        m_game_fov_dolly_offset.store(0.0f, std::memory_order_relaxed);
        reset_prospi_state();
        return;
    }

    auto projection_min_fov = m_match_game_fov_min_enabled->value() ? m_match_game_fov_min->value() : 5.0f;
    const auto prospi_actual_min_fov = std::clamp(m_match_game_fov_prospi_actual_min->value(), 5.0f, 175.0f);
    auto prospi_preset = ProSpiCameraPreset::None;
    auto active_prospi_actual_min_fov = 0.0f;
    auto wrote_prospi_fov = false;
    auto wants_game_fov_write = false;
    auto deferred_game_fov_write = raw_fov;
    auto prospi_calibration_applied = false;
    auto prospi_tv_override_applied = false;
    auto generic_camera_preset_applied = false;
    auto read_only_camera_for_frame = m_match_game_fov_read_only_camera->value();
    const auto camera_cut_stabilizer_enabled = m_match_game_fov_camera_cut_stabilizer->value();
    const auto generic_camera_presets_tracking_enabled =
        m_match_game_fov_dolly->value() &&
        m_match_game_fov_generic_camera_presets->value();
    const auto generic_camera_presets_apply_enabled =
        generic_camera_presets_tracking_enabled &&
        m_match_game_fov_generic_camera_presets_auto_apply->value();
    const auto should_track_generic_camera = camera_cut_stabilizer_enabled || generic_camera_presets_tracking_enabled;
    const char* prospi_dolly_source = "Base";
    std::string prospi_camera_id{};

    const auto is_prospi = is_prospi_executable();
    const auto location = read_game_camera_location(pcm);
    const auto rotation = read_game_camera_rotation(pcm);
    GameCameraSample camera_sample{};
    if (should_track_generic_camera && location.has_value() && rotation.has_value()) {
        camera_sample.valid = true;
        camera_sample.player_camera_manager = reinterpret_cast<uintptr_t>(pcm);
        camera_sample.raw_fov = raw_fov;
        camera_sample.timestamp = std::chrono::steady_clock::now();
        camera_sample.location = *location;
        camera_sample.rotation = *rotation;
        camera_sample.camera_id = build_generic_camera_preset_id(*location, *rotation, raw_fov);
        {
            std::scoped_lock _{m_generic_camera_preset_mtx};
            m_current_game_camera_id = camera_sample.camera_id;
        }

        m_match_game_fov_generic_camera_tracking_active.store(true, std::memory_order_relaxed);
    } else if (m_match_game_fov_generic_camera_tracking_active.exchange(false, std::memory_order_relaxed)) {
        std::scoped_lock _{m_generic_camera_preset_mtx};
        m_current_game_camera_id.clear();
    }

    if (location.has_value() && rotation.has_value()) {
        prospi_camera_id = build_camera_calibration_id(*location, *rotation);
        {
            std::scoped_lock _{m_prospi_camera_calibration_mtx};
            m_prospi_current_camera_id = prospi_camera_id;
        }

        if (m_match_game_fov_prospi_camera_calibration_auto->value()) {
            std::scoped_lock _{m_prospi_camera_calibration_mtx};
            if (const auto it = m_prospi_camera_calibrations.find(prospi_camera_id); it != m_prospi_camera_calibrations.end()) {
                const auto saved_min_fov = std::clamp(it->second.actual_min_fov, 5.0f, 175.0f);
                if (is_prospi) {
                    active_prospi_actual_min_fov = saved_min_fov;
                } else if (m_match_game_fov_min_enabled->value()) {
                    projection_min_fov = saved_min_fov;
                }

                active_fov_multiplier = std::clamp(it->second.projection_multiplier, 0.1f, 3.0f);
                active_dolly_distance = std::clamp(it->second.dolly_distance, 10.0f, 50000.0f);
                prospi_calibration_applied = true;
                prospi_dolly_source = "Calibration";
            }
        }
    } else {
        std::scoped_lock _{m_prospi_camera_calibration_mtx};
        m_prospi_current_camera_id.clear();
    }

    if (is_prospi && location.has_value() && rotation.has_value()) {
        const auto classified_prospi_preset = classify_prospi_camera_preset(*location, *rotation, raw_fov);
        prospi_preset = classified_prospi_preset;

        if (m_prospi_sticky_preset_valid &&
            should_keep_prospi_sticky_preset(
                (ProSpiCameraPreset)m_prospi_sticky_preset,
                m_prospi_sticky_location,
                m_prospi_sticky_rotation,
                m_prospi_sticky_raw_fov,
                classified_prospi_preset,
                *location,
                *rotation,
                raw_fov)) {
            prospi_preset = (ProSpiCameraPreset)m_prospi_sticky_preset;
        }

        if (!prospi_calibration_applied && m_match_game_fov_dolly->value()) {
            switch (prospi_preset) {
            case ProSpiCameraPreset::OpeningAerialTelephoto:
                if (m_match_game_fov_prospi_opening_aerial_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_opening_aerial_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "OpeningAerialTelephoto";
                }
                break;
            case ProSpiCameraPreset::BehindPlateWideTelephoto:
                if (m_match_game_fov_prospi_behind_plate_wide_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_behind_plate_wide_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "BehindPlateWideTelephoto";
                }
                break;
            case ProSpiCameraPreset::HomePlateWaistHighReverse:
                if (m_match_game_fov_prospi_home_plate_waist_high_reverse_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_home_plate_waist_high_reverse_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "HomePlateWaistHighReverse";
                }
                break;
            case ProSpiCameraPreset::TVBroadcast:
                if (m_match_game_fov_prospi_tv_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_tv_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_tv_override_applied = true;
                    prospi_dolly_source = "TVBroadcast";
                }
                break;
            case ProSpiCameraPreset::CenterFieldTelephoto:
                if (m_match_game_fov_prospi_center_field_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_center_field_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "CenterFieldTelephoto";
                }
                break;
            case ProSpiCameraPreset::CenterFieldHighTelephoto:
                if (m_match_game_fov_prospi_center_field_high_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_center_field_high_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "CenterFieldHighTelephoto";
                }
                break;
            case ProSpiCameraPreset::OffsetCenterFieldTelephoto:
                if (m_match_game_fov_prospi_left_field_corner_wide_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_left_field_corner_wide_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "OffsetCenterFieldTelephoto";
                }
                break;
            case ProSpiCameraPreset::DeepOutfieldTelephoto:
                if (m_match_game_fov_prospi_deep_outfield_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_deep_outfield_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "DeepOutfieldTelephoto";
                }
                break;
            case ProSpiCameraPreset::HomePlateSkyAerial:
                if (m_match_game_fov_prospi_home_plate_sky_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_home_plate_sky_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "HomePlateSkyAerial";
                }
                break;
            case ProSpiCameraPreset::PlateHighTelephoto:
                if (m_match_game_fov_prospi_plate_high_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_plate_high_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "PlateHighTelephoto";
                }
                break;
            case ProSpiCameraPreset::HomePlateOverheadTelephoto:
                if (m_match_game_fov_prospi_home_plate_overhead_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_home_plate_overhead_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "HomePlateOverheadTelephoto";
                }
                break;
            case ProSpiCameraPreset::UpperDeckTelephoto:
                if (m_match_game_fov_prospi_upper_deck_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_upper_deck_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "UpperDeckTelephoto";
                }
                break;
            case ProSpiCameraPreset::UpperDeckHomeSkyTelephoto:
                if (m_match_game_fov_prospi_home_sky_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_home_sky_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "UpperDeckHomeSkyTelephoto";
                }
                break;
            case ProSpiCameraPreset::ThirdBaseTelephoto:
                if (m_match_game_fov_prospi_third_base_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_third_base_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "ThirdBaseTelephoto";
                }
                break;
            case ProSpiCameraPreset::ThirdBaseRelayLow:
                if (m_match_game_fov_prospi_third_base_relay_low_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_third_base_relay_low_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "ThirdBaseRelayLow";
                }
                break;
            case ProSpiCameraPreset::ThirdBaseCornerLow:
                if (m_match_game_fov_prospi_third_base_sweep_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_third_base_sweep_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "ThirdBaseCornerLow";
                }
                break;
            case ProSpiCameraPreset::ThirdBaseWideTelephoto:
                if (m_match_game_fov_prospi_third_base_wide_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_third_base_wide_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "ThirdBaseWideTelephoto";
                }
                break;
            case ProSpiCameraPreset::FirstBaseTelephoto:
                if (m_match_game_fov_prospi_first_base_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_first_base_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "FirstBaseTelephoto";
                }
                break;
            case ProSpiCameraPreset::FirstBaseWideTelephoto:
                if (m_match_game_fov_prospi_first_base_wide_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_first_base_wide_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "FirstBaseWideTelephoto";
                }
                break;
            case ProSpiCameraPreset::FirstBaseCornerLow:
                if (m_match_game_fov_prospi_first_base_corner_low_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_first_base_corner_low_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "FirstBaseCornerLow";
                }
                break;
            case ProSpiCameraPreset::LowInfieldSideCloseUp:
                if (m_match_game_fov_prospi_low_plate_corner_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_low_plate_corner_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "LowInfieldSideCloseUp";
                }
                break;
            case ProSpiCameraPreset::BackstopHighTelephoto:
                if (m_match_game_fov_prospi_backstop_high_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_backstop_high_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "BackstopHighTelephoto";
                }
                break;
            case ProSpiCameraPreset::RightFieldCornerTelephoto:
                if (m_match_game_fov_prospi_right_field_corner_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_right_field_corner_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "RightFieldCornerTelephoto";
                }
                break;
            case ProSpiCameraPreset::RightCenterFieldTelephoto:
                if (m_match_game_fov_prospi_right_center_field_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_right_center_field_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "RightCenterFieldTelephoto";
                }
                break;
            default:
                break;
            }
        }

        if (is_specific_prospi_preset(prospi_preset)) {
            m_prospi_sticky_preset_valid = true;
            m_prospi_sticky_preset = (int32_t)prospi_preset;
            m_prospi_sticky_location = *location;
            m_prospi_sticky_rotation = *rotation;
            m_prospi_sticky_raw_fov = raw_fov;
            m_prospi_sticky_camera_id = prospi_camera_id;
        }
    }

    if (m_match_game_fov_dolly->value() &&
        m_match_game_fov_prospi_actual_clamp->value() &&
        is_prospi) {
        const auto resolve_prospi_actual_min_fov = [&](ProSpiCameraPreset preset) {
            switch (preset) {
            case ProSpiCameraPreset::CenterFieldTelephoto:
            case ProSpiCameraPreset::CenterFieldHighTelephoto:
            case ProSpiCameraPreset::OffsetCenterFieldTelephoto:
                return std::clamp(m_match_game_fov_prospi_center_field_actual_min->value(), 5.0f, 175.0f);
            case ProSpiCameraPreset::UpperDeckTelephoto:
            case ProSpiCameraPreset::UpperDeckHomeSkyTelephoto:
                return std::clamp(m_match_game_fov_prospi_upper_deck_actual_min->value(), 5.0f, 175.0f);
            case ProSpiCameraPreset::PlateHighTelephoto:
            case ProSpiCameraPreset::HomePlateOverheadTelephoto:
                return std::clamp(m_match_game_fov_prospi_plate_high_actual_min->value(), 5.0f, 175.0f);
            case ProSpiCameraPreset::DeepOutfieldTelephoto:
                return std::clamp(m_match_game_fov_prospi_deep_outfield_actual_min->value(), 5.0f, 175.0f);
            case ProSpiCameraPreset::ThirdBaseTelephoto:
            case ProSpiCameraPreset::ThirdBaseRelayLow:
            case ProSpiCameraPreset::ThirdBaseWideTelephoto:
            case ProSpiCameraPreset::FirstBaseTelephoto:
            case ProSpiCameraPreset::FirstBaseWideTelephoto:
            case ProSpiCameraPreset::GenericTelephoto:
            case ProSpiCameraPreset::None:
            default:
                return prospi_actual_min_fov;
            }
        };

        if (!prospi_calibration_applied) {
            active_prospi_actual_min_fov = resolve_prospi_actual_min_fov(prospi_preset);
        }

        const auto generic_trigger_min_fov = prospi_calibration_applied ? active_prospi_actual_min_fov : prospi_actual_min_fov;

        if (prospi_preset != ProSpiCameraPreset::None || raw_fov < generic_trigger_min_fov) {
            if (prospi_preset == ProSpiCameraPreset::None) {
                prospi_preset = ProSpiCameraPreset::GenericTelephoto;
                if (!prospi_calibration_applied) {
                    active_prospi_actual_min_fov = resolve_prospi_actual_min_fov(prospi_preset);
                }
            }

            game_fov_for_matching = std::clamp(raw_fov, active_prospi_actual_min_fov, 175.0f);
            if (std::abs(game_fov_for_matching - raw_fov) > 0.01f) {
                wants_game_fov_write = true;
                deferred_game_fov_write = game_fov_for_matching;
            }
        }
    }

    if (generic_camera_presets_apply_enabled && camera_sample.valid) {
        std::optional<GenericCameraPreset> preset{};
        {
            std::scoped_lock _{m_generic_camera_preset_mtx};
            if (const auto it = m_generic_camera_presets.find(camera_sample.camera_id); it != m_generic_camera_presets.end()) {
                preset = it->second;
                m_active_generic_camera_preset = it->second;
            } else {
                m_active_generic_camera_preset = {};
            }
        }

        if (preset.has_value()) {
            generic_camera_preset_applied = true;
            active_fov_multiplier = std::clamp(preset->projection_multiplier, 0.1f, 3.0f);
            active_dolly_distance = std::clamp(preset->dolly_distance, 10.0f, 50000.0f);
            projection_min_fov = std::max(projection_min_fov, std::clamp(preset->min_fov, 5.0f, 175.0f));
            game_fov_for_matching = std::clamp(game_fov_for_matching, projection_min_fov, 175.0f);
            read_only_camera_for_frame = read_only_camera_for_frame || preset->read_only_camera;
        }
    } else {
        std::scoped_lock _{m_generic_camera_preset_mtx};
        m_active_generic_camera_preset = {};
    }

    effective_fov = game_fov_for_matching * active_fov_multiplier;
    effective_fov = std::clamp(effective_fov, projection_min_fov, 175.0f);

    bool camera_cut_stabilizer_blocked_write = false;
    if (!camera_cut_stabilizer_enabled) {
        if (m_camera_cut_state.stabilizing) {
            spdlog::info("[CAMERA_STABILIZER] active=false reason=disabled");
        }

        m_camera_cut_state = {};
        m_match_game_fov_camera_cut_stabilizer_active.store(false, std::memory_order_relaxed);
        m_match_game_fov_camera_cut_stabilizer_remaining_ms.store(0, std::memory_order_relaxed);
    } else if (camera_sample.valid) {
        auto output = GameCameraProjectionState{
            .valid = true,
            .game_fov_for_matching = game_fov_for_matching,
            .effective_fov = effective_fov,
            .active_dolly_distance = active_dolly_distance,
            .active_fov_multiplier = active_fov_multiplier
        };

        const auto now = camera_sample.timestamp;
        const auto duration_ms = std::clamp(m_match_game_fov_camera_cut_stabilizer_duration_ms->value(), 100.0f, 1500.0f);
        const auto fov_threshold = std::clamp(m_match_game_fov_camera_cut_stabilizer_fov_delta->value(), 1.0f, 45.0f);
        const auto rotation_threshold = std::clamp(m_match_game_fov_camera_cut_stabilizer_rotation_delta->value(), 1.0f, 90.0f);
        const auto location_threshold = std::clamp(m_match_game_fov_camera_cut_stabilizer_location_delta->value(), 25.0f, 10000.0f);

        bool detected_cut = false;
        float location_delta = 0.0f;
        float rotation_delta = 0.0f;
        float fov_delta = 0.0f;
        bool camera_id_changed = false;
        bool pcm_changed = false;

        if (m_camera_cut_state.has_previous_sample) {
            const auto& previous = m_camera_cut_state.previous_sample;
            location_delta = glm::distance(camera_sample.location, previous.location);
            const auto pitch_delta = normalize_angle_delta(camera_sample.rotation.x, previous.rotation.x);
            const auto yaw_delta = normalize_angle_delta(camera_sample.rotation.y, previous.rotation.y);
            const auto roll_delta = normalize_angle_delta(camera_sample.rotation.z, previous.rotation.z);
            rotation_delta = (std::max)(pitch_delta, (std::max)(yaw_delta, roll_delta));
            fov_delta = std::abs(camera_sample.raw_fov - previous.raw_fov);
            camera_id_changed = camera_sample.camera_id != previous.camera_id;
            pcm_changed = camera_sample.player_camera_manager != previous.player_camera_manager;

            detected_cut =
                camera_id_changed ||
                pcm_changed ||
                fov_delta >= fov_threshold ||
                rotation_delta >= rotation_threshold ||
                location_delta >= location_threshold;
        }

        if (detected_cut && m_camera_cut_state.has_last_output) {
            m_camera_cut_state.stabilizing = true;
            m_camera_cut_state.cut_time = now;
            m_camera_cut_state.stabilize_until = now + std::chrono::milliseconds((int64_t)std::lround(duration_ms));
            m_camera_cut_state.blend_from = m_camera_cut_state.last_output;
            m_camera_cut_state.blend_to = output;
            m_camera_cut_state.last_cut_from = m_camera_cut_state.previous_sample;
            m_camera_cut_state.last_cut_to = camera_sample;

            spdlog::info(
                "[CAMERA_CUT] from={} to={} id_changed={} pcm_changed={} loc_delta={:.1f} rot_delta={:.1f} fov_delta={:.1f} stabilize_ms={:.0f}",
                m_camera_cut_state.last_cut_from.camera_id.empty() ? "None" : m_camera_cut_state.last_cut_from.camera_id,
                camera_sample.camera_id.empty() ? "None" : camera_sample.camera_id,
                camera_id_changed,
                pcm_changed,
                location_delta,
                rotation_delta,
                fov_delta,
                duration_ms);
        }

        if (m_camera_cut_state.stabilizing) {
            m_camera_cut_state.blend_to = output;

            if (now < m_camera_cut_state.stabilize_until) {
                const auto elapsed_ms = (float)std::chrono::duration_cast<std::chrono::milliseconds>(now - m_camera_cut_state.cut_time).count();
                const auto freeze_ms = duration_ms * 0.4f;
                const auto blend_ms = (std::max)(1.0f, duration_ms - freeze_ms);
                const auto t = elapsed_ms <= freeze_ms ? 0.0f : smoothstep01((elapsed_ms - freeze_ms) / blend_ms);
                const auto& from = m_camera_cut_state.blend_from;
                const auto& to = m_camera_cut_state.blend_to;

                game_fov_for_matching = lerp_float(from.game_fov_for_matching, to.game_fov_for_matching, t);
                effective_fov = std::clamp(lerp_float(from.effective_fov, to.effective_fov, t), projection_min_fov, 175.0f);
                active_dolly_distance = lerp_float(from.active_dolly_distance, to.active_dolly_distance, t);
                active_fov_multiplier = lerp_float(from.active_fov_multiplier, to.active_fov_multiplier, t);
                camera_cut_stabilizer_blocked_write = true;

                const auto remaining_ms =
                    (int32_t)std::chrono::duration_cast<std::chrono::milliseconds>(m_camera_cut_state.stabilize_until - now).count();
                m_match_game_fov_camera_cut_stabilizer_active.store(true, std::memory_order_relaxed);
                m_match_game_fov_camera_cut_stabilizer_remaining_ms.store((std::max)(0, remaining_ms), std::memory_order_relaxed);
            } else {
                m_camera_cut_state.stabilizing = false;
                m_match_game_fov_camera_cut_stabilizer_active.store(false, std::memory_order_relaxed);
                m_match_game_fov_camera_cut_stabilizer_remaining_ms.store(0, std::memory_order_relaxed);
                spdlog::info("[CAMERA_STABILIZER] active=false reason=complete");
            }
        } else {
            m_match_game_fov_camera_cut_stabilizer_active.store(false, std::memory_order_relaxed);
            m_match_game_fov_camera_cut_stabilizer_remaining_ms.store(0, std::memory_order_relaxed);
        }

        m_camera_cut_state.previous_sample = camera_sample;
        m_camera_cut_state.has_previous_sample = true;
        m_camera_cut_state.last_output = GameCameraProjectionState{
            .valid = true,
            .game_fov_for_matching = game_fov_for_matching,
            .effective_fov = effective_fov,
            .active_dolly_distance = active_dolly_distance,
            .active_fov_multiplier = active_fov_multiplier
        };
        m_camera_cut_state.has_last_output = true;
    } else {
        m_camera_cut_state = {};
        m_match_game_fov_camera_cut_stabilizer_active.store(false, std::memory_order_relaxed);
        m_match_game_fov_camera_cut_stabilizer_remaining_ms.store(0, std::memory_order_relaxed);
    }

    const auto block_game_fov_write = read_only_camera_for_frame || camera_cut_stabilizer_blocked_write;
    if (wants_game_fov_write) {
        if (block_game_fov_write) {
            m_match_game_fov_would_write_game_camera.store(true, std::memory_order_relaxed);
        } else {
            wrote_prospi_fov = write_game_fov(pcm, deferred_game_fov_write);
            m_match_game_fov_would_write_game_camera.store(!wrote_prospi_fov, std::memory_order_relaxed);
        }
    } else {
        m_match_game_fov_would_write_game_camera.store(false, std::memory_order_relaxed);
    }

    m_match_game_fov_read_only_camera_active.store(block_game_fov_write, std::memory_order_relaxed);
    m_match_game_fov_generic_camera_preset_applied.store(generic_camera_preset_applied, std::memory_order_relaxed);

    const auto telephoto_perf_trigger_fov = std::clamp(m_match_game_fov_prospi_telephoto_perf_trigger_fov->value(), 10.0f, 40.0f);
    const auto telephoto_perf_should_apply =
        is_prospi &&
        m_match_game_fov_prospi_telephoto_perf_override->value() &&
        !is_prospi_nontelephoto_preset(prospi_preset) &&
        (raw_fov <= telephoto_perf_trigger_fov || game_fov_for_matching <= telephoto_perf_trigger_fov);
    update_prospi_telephoto_perf_override(telephoto_perf_should_apply);

    m_match_game_fov_prospi_preset.store((int32_t)prospi_preset, std::memory_order_relaxed);
    m_match_game_fov_prospi_actual_min_active.store(active_prospi_actual_min_fov, std::memory_order_relaxed);
    m_match_game_fov_prospi_calibration_applied.store(prospi_calibration_applied, std::memory_order_relaxed);
    m_match_game_fov_prospi_calibration_dolly_distance_active.store(active_dolly_distance, std::memory_order_relaxed);
    m_match_game_fov_prospi_calibration_multiplier_active.store(active_fov_multiplier, std::memory_order_relaxed);
    m_match_game_fov_prospi_calibration_actual_min_active.store(prospi_calibration_applied ? active_prospi_actual_min_fov : 0.0f, std::memory_order_relaxed);
    m_match_game_fov_prospi_tv_override_active.store(prospi_tv_override_applied, std::memory_order_relaxed);
    m_match_game_fov_prospi_auto_dolly_distance_active.store(prospi_dolly_source == std::string_view{"Base"} ? 0.0f : active_dolly_distance, std::memory_order_relaxed);
    m_game_fov.store(effective_fov, std::memory_order_relaxed);
    m_game_fov_valid.store(true, std::memory_order_relaxed);

    if (is_prospi_executable() && m_match_game_fov_prospi_actual_clamp->value()) {
        static auto last_logged_preset = ProSpiCameraPreset::None;
        static auto last_logged_raw_fov = 0.0f;
        static auto last_logged_min_fov = 0.0f;
        static auto last_logged_written_fov = 0.0f;
        static auto last_logged_effective_fov = 0.0f;
        static auto last_logged_write_state = false;
        static auto last_logged_calibration_state = false;
        static auto last_logged_tv_override_state = false;
        static auto last_logged_telephoto_perf_state = false;
        static auto last_logged_multiplier = 1.0f;
        static auto last_logged_dolly_distance = 0.0f;
        static std::string last_logged_dolly_source{"Base"};
        static std::string last_logged_camera_id{};

        if (prospi_preset != last_logged_preset ||
            wrote_prospi_fov != last_logged_write_state ||
            prospi_calibration_applied != last_logged_calibration_state ||
            prospi_tv_override_applied != last_logged_tv_override_state ||
            telephoto_perf_should_apply != last_logged_telephoto_perf_state ||
            prospi_dolly_source != last_logged_dolly_source ||
            prospi_camera_id != last_logged_camera_id ||
            std::abs(raw_fov - last_logged_raw_fov) > 0.25f ||
            std::abs(active_prospi_actual_min_fov - last_logged_min_fov) > 0.01f ||
            std::abs(active_fov_multiplier - last_logged_multiplier) > 0.01f ||
            std::abs(active_dolly_distance - last_logged_dolly_distance) > 0.25f ||
            std::abs(game_fov_for_matching - last_logged_written_fov) > 0.25f ||
            std::abs(effective_fov - last_logged_effective_fov) > 0.25f) {
            spdlog::info(
                "[PROSPI_FOV] preset={} camera={} calibrated={} tv_override={} telephoto_perf={} dolly_source={} raw={:.2f} min={:.2f} mult={:.2f} dolly={:.2f} written={:.2f} effective={:.2f} wrote={}",
                get_prospi_camera_preset_name(prospi_preset),
                prospi_camera_id.empty() ? "None" : prospi_camera_id,
                prospi_calibration_applied,
                prospi_tv_override_applied,
                telephoto_perf_should_apply,
                prospi_dolly_source,
                raw_fov,
                active_prospi_actual_min_fov,
                active_fov_multiplier,
                active_dolly_distance,
                game_fov_for_matching,
                effective_fov,
                wrote_prospi_fov
            );

            last_logged_preset = prospi_preset;
            last_logged_raw_fov = raw_fov;
            last_logged_min_fov = active_prospi_actual_min_fov;
            last_logged_multiplier = active_fov_multiplier;
            last_logged_dolly_distance = active_dolly_distance;
            last_logged_written_fov = game_fov_for_matching;
            last_logged_effective_fov = effective_fov;
            last_logged_write_state = wrote_prospi_fov;
            last_logged_calibration_state = prospi_calibration_applied;
            last_logged_tv_override_state = prospi_tv_override_applied;
            last_logged_telephoto_perf_state = telephoto_perf_should_apply;
            last_logged_dolly_source = prospi_dolly_source;
            last_logged_camera_id = prospi_camera_id;
        }
    }

    if (m_match_game_fov_dolly->value()) {
        auto base_fov = read_default_fov(pcm).value_or(m_game_fov_base.load(std::memory_order_relaxed));
        if (!std::isfinite(base_fov) || base_fov <= 1.0f || base_fov >= 179.0f) {
            base_fov = game_fov_for_matching;
        }

        m_game_fov_base.store(base_fov, std::memory_order_relaxed);
        base_fov = std::clamp(base_fov, 5.0f, 175.0f);

        const auto current_half = glm::radians(effective_fov) * 0.5f;
        const auto base_half = glm::radians(base_fov) * 0.5f;
        const auto base_tan = std::tan(base_half);
        const auto current_tan = std::tan(current_half);

        if (base_tan <= 0.0f || current_tan <= 0.0f) {
            m_game_fov_dolly_offset.store(0.0f, std::memory_order_relaxed);
            return;
        }

        const auto scale = current_tan / base_tan;
        const auto focus_distance = active_dolly_distance;
        auto dolly_offset = focus_distance * (1.0f - scale);
        const auto max_offset = focus_distance * 2.0f;
        dolly_offset = std::clamp(dolly_offset, -max_offset, max_offset);

        if (!std::isfinite(dolly_offset)) {
            m_game_fov_dolly_offset.store(0.0f, std::memory_order_relaxed);
            return;
        }

        m_game_fov_dolly_offset.store(dolly_offset, std::memory_order_relaxed);
    } else {
        m_game_fov_dolly_offset.store(0.0f, std::memory_order_relaxed);
    }
}

float VR::get_game_fov() const {
    return m_game_fov.load(std::memory_order_relaxed);
}

float VR::get_game_fov_scale(float base_half_fov) const {
    if (!m_game_fov_valid.load(std::memory_order_relaxed)) {
        return 1.0f;
    }

    if (m_match_game_fov_dolly->value()) {
        return 1.0f;
    }

    auto game_fov = get_game_fov();

    if (!std::isfinite(game_fov)) {
        return 1.0f;
    }

    game_fov = std::clamp(game_fov, 5.0f, 175.0f);

    const auto desired_half = glm::radians(game_fov) * 0.5f;
    const auto base_tan = std::tan(base_half_fov);
    const auto desired_tan = std::tan(desired_half);

    if (base_tan <= 0.0f || desired_tan <= 0.0f) {
        return 1.0f;
    }

    const auto scale = base_tan / desired_tan;
    if (!std::isfinite(scale) || scale <= 0.01f || scale >= 100.0f) {
        return 1.0f;
    }

    return scale;
}

float VR::get_game_fov_dolly_offset() const {
    return m_game_fov_dolly_offset.load(std::memory_order_relaxed);
}

void VR::on_pre_calculate_stereo_view_offset(void* stereo_device, const int32_t view_index, Rotator<float>* view_rotation, 
                                             const float world_to_meters, Vector3f* view_location, bool is_double)
{
    if (!is_hmd_active()) {
        m_camera_freeze.position_wants_freeze = false;
        m_camera_freeze.rotation_wants_freeze = false;
        return;
    }

    const auto now = std::chrono::high_resolution_clock::now();
    const auto delta = std::chrono::duration<float, std::chrono::seconds::period>(now - m_last_lerp_update).count();

    Rotator<double>* view_rotation_double = (Rotator<double>*)view_rotation;
    Vector3d* view_location_double = (Vector3d*)view_location;

    glm::vec3 target_rotation = is_double ? glm::vec3{*(glm::vec<3, double>*)view_rotation_double} : *(glm::vec<3, float>*)view_rotation;

    const auto should_lerp_pitch = m_lerp_camera_pitch->value();
    const auto should_lerp_yaw = m_lerp_camera_yaw->value();
    const auto should_lerp_roll = m_lerp_camera_roll->value();

    auto lerp_angle = [](auto a, auto b, auto t) {
        const auto diff = b - a;
        if constexpr (std::is_same_v<decltype(a), double>) {
            if (diff > 180.0) {
                b -= 360.0;
            } else if (diff < -180.0) {
                b += 360.0;
            }
        } else {
            if (diff > 180.0f) {
                b -= 360.0f;
            } else if (diff < -180.0f) {
                b += 360.0f;
            }
        }

        return glm::lerp(a, b, t);
    };

    const auto lerp_t = m_lerp_camera_speed->value() * delta;

    if (should_lerp_pitch) {
        if (is_double) {
            view_rotation_double->pitch = lerp_angle((double)m_camera_lerp.last_rotation.x, (double)target_rotation.x, (double)lerp_t);
        } else {
            view_rotation->pitch = lerp_angle(m_camera_lerp.last_rotation.x, target_rotation.x, lerp_t);
        }
    }

    if (should_lerp_yaw) {
        if (is_double) {
            view_rotation_double->yaw = lerp_angle((double)m_camera_lerp.last_rotation.y, (double)target_rotation.y, (double)lerp_t);
        } else {
            view_rotation->yaw = lerp_angle(m_camera_lerp.last_rotation.y, target_rotation.y, lerp_t);
        }
    }

    if (should_lerp_roll) {
        if (is_double) {
            view_rotation_double->roll = lerp_angle((double)m_camera_lerp.last_rotation.z, (double)target_rotation.z, (double)lerp_t);
        } else {
            view_rotation->roll = lerp_angle(m_camera_lerp.last_rotation.z, target_rotation.z, lerp_t);
        }
    }

    if (is_double) {
        m_camera_lerp.last_rotation = glm::vec3{ (float)view_rotation_double->pitch, (float)view_rotation_double->yaw, (float)view_rotation_double->roll };
    } else {
        m_camera_lerp.last_rotation = glm::vec3{ view_rotation->pitch, view_rotation->yaw, view_rotation->roll };
    }

    m_last_lerp_update = std::chrono::high_resolution_clock::now();

    if (m_camera_freeze.position_wants_freeze) {
        if (is_double) {
            m_camera_freeze.position = glm::vec3{ (float)view_location_double->x, (float)view_location_double->y, (float)view_location_double->z };
        } else {
            m_camera_freeze.position = glm::vec3{ view_location->x, view_location->y, view_location->z };
        }

        m_camera_freeze.position_wants_freeze = false;
        m_camera_freeze.position_frozen = true;
    }

    if (m_camera_freeze.rotation_wants_freeze) {
        if (is_double) {
            m_camera_freeze.rotation = glm::vec3{ (float)view_rotation_double->pitch, (float)view_rotation_double->yaw, (float)view_rotation_double->roll };
        } else {
            m_camera_freeze.rotation = glm::vec3{ view_rotation->pitch, view_rotation->yaw, view_rotation->roll };
        }

        m_camera_freeze.rotation_wants_freeze = false;
        m_camera_freeze.rotation_frozen = true;
    }

    if (m_camera_freeze.position_frozen) {
        if (is_double) {
            view_location_double->x = m_camera_freeze.position.x;
            view_location_double->y = m_camera_freeze.position.y;
            view_location_double->z = m_camera_freeze.position.z;
        } else {
            view_location->x = m_camera_freeze.position.x;
            view_location->y = m_camera_freeze.position.y;
            view_location->z = m_camera_freeze.position.z;
        }
    }

    if (m_camera_freeze.rotation_frozen) {
        if (is_double) {
            view_rotation_double->pitch = m_camera_freeze.rotation.x;
            view_rotation_double->yaw = m_camera_freeze.rotation.y;
            view_rotation_double->roll = m_camera_freeze.rotation.z;
        } else {
            view_rotation->pitch = m_camera_freeze.rotation.x;
            view_rotation->yaw = m_camera_freeze.rotation.y;
            view_rotation->roll = m_camera_freeze.rotation.z;
        }
    }
}

void VR::on_pre_viewport_client_draw(void* viewport_client, void* viewport, void* canvas){
    ZoneScopedN(__FUNCTION__);

    if (m_custom_z_near_enabled->value()) {
        SPDLOG_INFO_ONCE("Attempting to set custom z near");
        sdk::globals::get_near_clipping_plane() = m_custom_z_near->value();
    }
}

float VR::flat3d_effective_nearz() {
    // Custom Z Near is an explicit user override written into the SDK's cell by
    // on_pre_viewport_client_draw — honor it exactly, whatever value they pick.
    if (m_custom_z_near_enabled->value()) {
        return sdk::globals::get_near_clipping_plane();
    }

    const float nz = sdk::globals::get_near_clipping_plane();
    // A successful scan yields the game's real plane (UE default ~10 uu). The
    // SDK returns a 1.0 dummy when the scan fails; treat <=1.0 as "no value".
    if (std::isfinite(nz) && nz > 1.0f) {
        return nz;
    }

    // Failed-scan fallback (kept out of the pristine UESDK submodule): use the
    // game's r.SetNearClipPlane cvar if it set one, else UE's own default of 10
    // (a far closer approximation than the SDK's 1.0 dummy). r.SetNearClipPlane
    // is 0 until set, so only adopt a positive value.
    try {
        if (auto data = sdk::find_cvar_data_cached(L"Engine", L"r.SetNearClipPlane"); data) {
            // Only adopt a PLAUSIBLE near plane (UE units; default is 10).
            // FF7 Rebirth's cvar read produced a tiny-positive garbage value —
            // it passed a bare >0 check and poisoned the depth->world
            // conversion (game_nearz ~0 → every sample classified far).
            if (auto* cv = data->get<float>(); cv != nullptr && cv->get() >= 1.0f && cv->get() <= 1000.0f) {
                SPDLOG_INFO_ONCE("[Flat3D] near plane fallback via r.SetNearClipPlane cvar");
                return cv->get();
            }
        }
    } catch (...) {
    }

    return 10.0f;
}

void VR::update_hmd_state(bool from_view_extensions, uint32_t frame_count) {
    ZoneScopedN(__FUNCTION__);

    std::scoped_lock _{m_reinitialize_mtx};

    auto runtime = get_runtime();
    if (m_uncap_framerate->value()) {
        // The 3D Display 2x-refresh cap owns t.MaxFPS while active.
        if (!(is_using_flat3d() && m_flat3d_vsync->value() == 3)) {
            sdk::set_cvar_data_float(L"Engine", L"t.MaxFPS", 500.0f);
        }
    }

    // Allows games running in HDR mode to not have a black UI overlay
    if (m_disable_hdr_compositing->value()) {
        sdk::set_cvar_data_int(L"SlateRHIRenderer", L"r.HDR.UI.CompositeMode", 0);
    }

    if (m_disable_blur_widgets->value()) {
        if (auto val = sdk::get_cvar_int(L"Slate", L"Slate.AllowBackgroundBlurWidgets"); val && *val != 0) {
            sdk::set_cvar_int(L"Slate", L"Slate.AllowBackgroundBlurWidgets", 0);
        }
    }

    if (!is_using_afr()) {
        const auto is_hzbo_frozen_by_cvm = m_cvar_manager != nullptr && m_cvar_manager->is_hzbo_frozen_and_enabled();

        // Forcefully disable r.HZBOcclusion, it doesn't work with native stereo mode (sometimes)
        // Except when the user sets it to 1 with the CVar Manager, we need to respect that
        if (m_disable_hzbocclusion->value() && !is_hzbo_frozen_by_cvm) {
            const auto r_hzb_occlusion_value = sdk::get_cvar_int(L"Renderer", L"r.HZBOcclusion");

            // Only set it once, otherwise we'll be spamming a Set call every frame
            if (r_hzb_occlusion_value && *r_hzb_occlusion_value != 0) {
                sdk::set_cvar_int(L"Renderer", L"r.HZBOcclusion", 0);
            }
        }

        if (m_disable_instance_culling->value()) {
            const auto r_instance_culling_value = sdk::get_cvar_int(L"Renderer", L"r.InstanceCulling.OcclusionCull");

            if (r_instance_culling_value && *r_instance_culling_value != 0) {
                sdk::set_cvar_int(L"Renderer", L"r.InstanceCulling.OcclusionCull", 0);
            }
        }
    }

    if (frame_count != 0 && is_using_afr() && frame_count % 2 == 0) {
        if (runtime->is_openxr()) {
            std::scoped_lock __{ m_openxr->sync_assignment_mtx };

            const auto last_frame = (frame_count - 1) % runtimes::OpenXR::QUEUE_SIZE;
            const auto now_frame = frame_count % runtimes::OpenXR::QUEUE_SIZE;
            m_openxr->pipeline_states[now_frame] = m_openxr->pipeline_states[last_frame];
            m_openxr->pipeline_states[now_frame].frame_count = now_frame;
        } else if (runtime->is_openvr()) {
            // Keep AFW's simpler OpenXR clone (Dead Island 2's synced-token fix is
            // joeyhodge work this branch deliberately does not carry), but the
            // else MUST stay guarded: Flat3D is a third runtime and a bare else
            // would static_cast it to runtimes::OpenVR* and walk pose_queue.
            const auto last_frame = (frame_count - 1) % m_openvr->pose_queue.size();
            const auto now_frame = frame_count % m_openvr->pose_queue.size();
            m_openvr->pose_queue[now_frame] = m_openvr->pose_queue[last_frame];
        }
        // Flat3D: no compositor pose queue to clone.

        // Forcefully disable motion blur because it freaks out with AFR
        sdk::set_cvar_data_int(L"Engine", L"r.DefaultFeature.MotionBlur", 0);
        if (!is_using_afw())
            return;
    }
    
    runtime->update_poses(from_view_extensions, frame_count);

    // Update the poses used for the game
    // If we used the data directly from the WaitGetPoses call, we would have to lock a different mutex and wait a long time
    // This is because the WaitGetPoses call is blocking, and we don't want to block any game logic
    if (runtime->wants_reset_origin && runtime->ready() && runtime->got_first_valid_poses) {
        std::unique_lock _{ runtime->pose_mtx };
        set_rotation_offset(glm::identity<glm::quat>());
        m_standing_origin = get_position_unsafe(vr::k_unTrackedDeviceIndex_Hmd);

        runtime->wants_reset_origin = false;
    }

    runtime->update_matrices(m_nearz, m_farz);

    runtime->got_first_poses = true;
}

void VR::update_action_states() {
    ZoneScopedN(__FUNCTION__);

    std::scoped_lock _{m_actions_mtx};

    auto runtime = get_runtime();

    if (runtime == nullptr || runtime->wants_reinitialize) {
        return;
    }

    static bool once = true;

    if (once) {
        spdlog::info("VR: Updating action states");
        once = false;
    }


    if (runtime->is_openvr()) {
        const auto start_time = std::chrono::high_resolution_clock::now();

        auto error = vr::VRInput()->UpdateActionState(&m_active_action_set, sizeof(m_active_action_set), 1);

        if (error != vr::VRInputError_None) {
            spdlog::error("VRInput failed to update action state: {}", (uint32_t)error);
        }

        const auto end_time = std::chrono::high_resolution_clock::now();
        const auto time_delta = end_time - start_time;

        m_last_input_delay = time_delta;
        m_avg_input_delay = (m_avg_input_delay + time_delta) / 2;

        if ((end_time - start_time) >= std::chrono::milliseconds(30)) {
            spdlog::warn("VRInput update action state took too long: {}ms", std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());

            //reinitialize_openvr();
            runtime->wants_reinitialize = true;
        }   
    } else {
        get_runtime()->update_input();
    }

    bool actively_using_controller = false;

    if (is_any_action_down()) {
        m_last_controller_update = std::chrono::steady_clock::now();
        actively_using_controller = true;
    }

    const auto last_xinput_update_is_late = std::chrono::steady_clock::now() - m_last_xinput_update >= std::chrono::seconds(2);
    const auto should_be_spoofing = (actively_using_controller || get_runtime()->handle_pause);

    if (m_spoofed_gamepad_connection && last_xinput_update_is_late && should_be_spoofing) {
        m_spoofed_gamepad_connection = false;
    }

    if (!m_spoofed_gamepad_connection && last_xinput_update_is_late && should_be_spoofing) {
        spdlog::info("[VR] Attempting to spoof gamepad connection");
        g_framework->post_message(WM_DEVICECHANGE, 0, 0);
        g_framework->activate_window();

        m_last_xinput_spoof_sent = std::chrono::steady_clock::now();
    }

    /*if (m_recenter_view_key->is_key_down_once()) {
        recenter_view();
    }

    if (m_set_standing_key->is_key_down_once()) {
        set_standing_origin(get_position(0));
    }*/

    static bool once2 = true;

    if (once2) {
        spdlog::info("VR: Updated action states");
        once2 = false;
    }

    update_dpad_gestures();
}

void VR::update_dpad_gestures() {
    if (!is_hmd_active()) {
        return;
    }

    const auto dpad_method = get_dpad_method();
    if (dpad_method != DPadMethod::GESTURE_HEAD && dpad_method != DPadMethod::GESTURE_HEAD_RIGHT) {
        return;
    }

    const auto wanted_index = dpad_method == DPadMethod::GESTURE_HEAD ? get_left_controller_index() : get_right_controller_index();

    const auto controller_pos = glm::vec3{get_position(wanted_index)};
    const auto hmd_transform = get_hmd_transform(m_frame_count);

    // Check if controller is near HMD
    const auto dist = glm::length(controller_pos - glm::vec3{hmd_transform[3]});

    if (dist > 0.2f) {
        return;
    }

    const auto dir_to_left = glm::normalize(controller_pos - glm::vec3{hmd_transform[3]});
    const auto hmd_dir = glm::quat{glm::extractMatrixRotation(hmd_transform)} * glm::vec3{0.0f, 0.0f, 1.0f};

    const auto angle = glm::acos(glm::dot(dir_to_left, hmd_dir));

    constexpr float threshold = glm::radians(120.0f);

    if (angle > threshold) {
        return;
    }

    // Make sure the angle is to the left/right of the HMD
    if (dpad_method == DPadMethod::GESTURE_HEAD_RIGHT) {
        if (glm::cross(dir_to_left, hmd_dir).y > 0.0f) {
            return;
        }
    } else if (glm::cross(dir_to_left, hmd_dir).y < 0.0f) {
        return;
    }

    // Send a vibration pulse to the controller
    const auto chosen_joystick = dpad_method == DPadMethod::GESTURE_HEAD ? m_left_joystick : m_right_joystick;
    trigger_haptic_vibration(0.0f, 0.1f, 1.0f, 5.0f, chosen_joystick);

    std::scoped_lock _{m_dpad_gesture_state.mtx};

    const auto left_joystick_axis = get_joystick_axis(chosen_joystick);

    if (left_joystick_axis.x < -0.5f) {
        m_dpad_gesture_state.direction |= DPadGestureState::Direction::LEFT;
    } else if (left_joystick_axis.x > 0.5f) {
        m_dpad_gesture_state.direction |= DPadGestureState::Direction::RIGHT;
    } 
    
    if (left_joystick_axis.y < -0.5f) {
        m_dpad_gesture_state.direction |= DPadGestureState::Direction::DOWN;
    } else if (left_joystick_axis.y > 0.5f) {
        m_dpad_gesture_state.direction |= DPadGestureState::Direction::UP;
    }
}

void VR::on_config_load(const utility::Config& cfg, bool set_defaults) {
    ZoneScopedN(__FUNCTION__);

    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }

    if (get_runtime() != nullptr && get_runtime()->loaded) {
        get_runtime()->on_config_load(cfg, set_defaults);

        // Run the rest of OpenXR initialization code here that depends on config values
        if (m_first_config_load) {
            m_first_config_load = false; // because the frontend can request config reloads

            if (get_runtime()->is_openxr()) {
                spdlog::info("[VR] Finishing up OpenXR initialization");
                initialize_openxr_swapchains();
            }
        }
    }

    if (m_fake_stereo_hook != nullptr) {
        m_fake_stereo_hook->on_config_load(cfg, set_defaults);
    }

    m_overlay_component.on_config_load(cfg, set_defaults);

    if (m_cvar_manager != nullptr) {
        m_cvar_manager->on_config_load(cfg, set_defaults);   
    }

    // Load camera offsets
    load_cameras();
}

void VR::on_config_save(utility::Config& cfg) {
    ZoneScopedN(__FUNCTION__);

    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }

    if (m_fake_stereo_hook != nullptr) {
        m_fake_stereo_hook->on_config_save(cfg);
    }

    if (get_runtime()->loaded) {
        get_runtime()->on_config_save(cfg);
    }

    m_overlay_component.on_config_save(cfg);

    // Save camera offsets
    save_cameras();
}

void VR::load_cameras() try {
    ZoneScopedN(__FUNCTION__);

    const auto cameras_txt = Framework::get_persistent_dir("cameras.txt");

    if (std::filesystem::exists(cameras_txt)) {
        spdlog::info("[VR] Loading camera offsets from {}", cameras_txt.string());

        utility::Config cfg{cameras_txt.string()};

        for (auto i = 0; i < m_camera_datas.size(); i++) {
            auto& data = m_camera_datas[i];

            if (auto offs = cfg.get<float>(std::format("camera_right_offset{}", i))) {
                data.offset.x = *offs;
            }

            if (auto offs = cfg.get<float>(std::format("camera_up_offset{}", i))) {
                data.offset.y = *offs;
            }

            if (auto offs = cfg.get<float>(std::format("camera_forward_offset{}", i))) {
                data.offset.z = *offs;
            }

            if (auto scale = cfg.get<float>(std::format("world_scale{}", i))) {
                data.world_scale = *scale;
            }

            if (auto decoupled_pitch = cfg.get<bool>(std::format("decoupled_pitch{}", i))) {
                data.decoupled_pitch = *decoupled_pitch;
            }

            if (auto decoupled_pitch_ui_adjust = cfg.get<bool>(std::format("decoupled_pitch_ui_adjust{}", i))) {
                data.decoupled_pitch_ui_adjust = *decoupled_pitch_ui_adjust;
            }
        }
    }
} catch(...) {
    spdlog::error("[VR] Failed to load camera offsets");
}

void VR::load_camera(int index) {
    ZoneScopedN(__FUNCTION__);

    if (index < 0 || index >= m_camera_datas.size()) {
        return;
    }

    const auto& data = m_camera_datas[index];

    m_camera_right_offset->value() = data.offset.x;
    m_camera_up_offset->value() = data.offset.y;
    m_camera_forward_offset->value() = data.offset.z;
    m_world_scale->value() = data.world_scale;
    m_decoupled_pitch->value() = data.decoupled_pitch;
    m_decoupled_pitch_ui_adjust->value() = data.decoupled_pitch_ui_adjust;
}

void VR::save_camera(int index) {
    ZoneScopedN(__FUNCTION__);

    if (index < 0 || index >= m_camera_datas.size()) {
        return;
    }

    auto& data = m_camera_datas[index];

    data.offset = {
        m_camera_right_offset->value(),
        m_camera_up_offset->value(),
        m_camera_forward_offset->value()
    };

    data.world_scale = m_world_scale->value();
    data.decoupled_pitch = m_decoupled_pitch->value();
    data.decoupled_pitch_ui_adjust = m_decoupled_pitch_ui_adjust->value();

    save_cameras();
}

void VR::save_cameras() try {
    ZoneScopedN(__FUNCTION__);

    const auto cameras_txt = Framework::get_persistent_dir("cameras.txt");

    spdlog::info("[VR] Saving camera offsets to {}", cameras_txt.string());

    utility::Config cfg{cameras_txt.string()};

    for (auto i = 0; i < m_camera_datas.size(); i++) {
        const auto& data = m_camera_datas[i];
        cfg.set<float>(std::format("camera_right_offset{}", i), data.offset.x);
        cfg.set<float>(std::format("camera_up_offset{}", i), data.offset.y);
        cfg.set<float>(std::format("camera_forward_offset{}", i), data.offset.z);
        cfg.set<float>(std::format("world_scale{}", i), m_camera_datas[i].world_scale);
        cfg.set<bool>(std::format("decoupled_pitch{}", i), m_camera_datas[i].decoupled_pitch);
        cfg.set<bool>(std::format("decoupled_pitch_ui_adjust{}", i), m_camera_datas[i].decoupled_pitch_ui_adjust);
    }

    cfg.save(cameras_txt.string());
} catch(...) {
    spdlog::error("[VR] Failed to save camera offsets");
}


void VR::on_pre_imgui_frame() {
    ZoneScopedN(__FUNCTION__);

    m_xinput_context.update();

    if (!get_runtime()->ready()) {
        return;
    }

    if (!m_disable_overlay) {
        m_overlay_component.on_pre_imgui_frame();
    }
}

void VR::handle_keybinds() {
    ZoneScopedN(__FUNCTION__);

    if (m_keybind_recenter->is_key_down_once()) {
        recenter_view();
    }

    if (m_keybind_recenter_horizon->is_key_down_once()) {
        recenter_horizon();
    }

    if (m_keybind_load_camera_0->is_key_down_once()) {
        load_camera(0);
    }

    if (m_keybind_load_camera_1->is_key_down_once()) {
        load_camera(1);
    }

    if (m_keybind_load_camera_2->is_key_down_once()) {
        load_camera(2);
    }

    if (m_keybind_set_standing_origin->is_key_down_once()) {
        m_standing_origin = get_position(0);
    }

    if (m_keybind_toggle_2d_screen->is_key_down_once()) {
        m_2d_screen_mode->toggle();
    }

    if (m_keybind_disable_vr->is_key_down_once()) {
        m_disable_vr = !m_disable_vr; // definitely should not be persistent
    }

    // The Slate UI
    if (m_keybind_toggle_gui->is_key_down_once()) {
        m_enable_gui->toggle();
    }

    if (is_using_flat3d()) {
        handle_flat3d_keybinds();
    }
}

void VR::on_frame() {
    ZoneScopedN(__FUNCTION__);

    m_cvar_manager->on_frame();
    handle_keybinds();

    if (is_using_flat3d()) {
        update_flat3d_params();
    }

    if (!get_runtime()->ready()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto is_allowed_draw_window = now - m_last_xinput_update < std::chrono::seconds(2);

    if (!is_allowed_draw_window) {
        m_rt_modifier.draw = false;
    }

    if (is_allowed_draw_window && m_xinput_context.headlocked_begin_held && !FrameworkConfig::get()->is_l3_r3_long_press()) {
        const auto rt_size = g_framework->get_rt_size();

        ImGui::Begin("AimMethod Notification", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);

        ImGui::Text("Continue holding down L3 + R3 to toggle aim method");

        if (std::chrono::steady_clock::now() - m_xinput_context.headlocked_begin >= std::chrono::seconds(1)) {
            if (m_aim_method->value() == VR::AimMethod::GAME) {
                m_aim_method->value() = m_previous_aim_method;
            } else {
                m_aim_method->value() = VR::AimMethod::GAME; // turns it off
            }

            m_xinput_context.headlocked_begin_held = false;
        } else {
            if (m_aim_method->value() != VR::AimMethod::GAME) {
                m_previous_aim_method = (VR::AimMethod)m_aim_method->value();
            } else if (m_previous_aim_method == VR::AimMethod::GAME) {
                m_previous_aim_method = VR::AimMethod::HEAD; // so it will at least be something
            }
        }

        const auto window_size = ImGui::GetWindowSize();

        const auto centered_x = (rt_size.x / 2) - (window_size.x / 2);
        const auto centered_y = (rt_size.y / 2) - (window_size.y / 2);
        ImGui::SetWindowPos(ImVec2(centered_x, centered_y), ImGuiCond_Always);

        ImGui::End();
    }

    if (m_rt_modifier.draw) {
        const auto rt_size = g_framework->get_rt_size();

        ImGui::Begin("RT Modifier Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
        
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Left Stick: Camera left/right/forward/back");
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Right Stick: Camera up/down");
        
        ImGui::Text("Page: %d", m_rt_modifier.page + 1);
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "DPad Left: Previous page | DPad Right: Next page");

        switch (m_rt_modifier.page) {
        case 2:
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + B: Save Camera 2");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Y: Save Camera 1");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + X: Save Camera 0");
            break;

        case 1:
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + B: Load Camera 2");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Y: Load Camera 1");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + X: Load Camera 0");
            break;

        case 0:
        default:
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + B: Reset camera offset");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Y: Recenter view");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + X: Reset standing origin");
            m_rt_modifier.page = 0;
            break;
        }

        const auto window_size = ImGui::GetWindowSize();

        const auto centered_x = (rt_size.x / 2) - (window_size.x / 2);
        const auto centered_y = (rt_size.y / 2) - (window_size.y / 2);
        ImGui::SetWindowPos(ImVec2(centered_x, centered_y), ImGuiCond_Always);
        ImGui::End();
    }
}

glm::mat4 to_reverseZ(const glm::mat4& proj) {

    glm::mat4 transformMat = glm::mat4(
        1, 0, 0, 0, 
        0, 1, 0, 0, 
        0, 0, -1, 0, 
        0, 0, 1, 1);

    return transformMat * proj;
}

void VR::update_camera_data(int frame_count) {

    if (last_update_camera_data_frame_count < frame_count || last_update_camera_data_frame_count > (frame_count + 100)) {
        last_update_camera_data_frame_count = frame_count;

        std::shared_lock _{get_runtime()->eyes_mtx};

        EyeIndex nEye = (m_render_frame_count % 2 == m_left_eye_interval) ? EyeLeft : EyeRight;
        EyeIndex nEyeOther = (m_render_frame_count % 2 == m_left_eye_interval) ? EyeRight : EyeLeft;

        cameraDataForMV[nEye].srcWorldToViewMatrixPrev = cameraData[nEye].srcWorldToViewMatrix;
        cameraDataForMV[nEye].srcViewToWorldMatrixPrev = cameraData[nEye].srcViewToWorldMatrix;
        cameraDataForMV[nEye].srcViewToClipMatrixPrev = cameraData[nEye].srcViewToClipMatrix;
        cameraDataForMV[nEye].srcClipToViewMatrixPrev = cameraData[nEye].srcClipToViewMatrix;

        cameraDataForMV[nEye].destWorldToViewMatrix = cameraData[nEyeOther].srcWorldToViewMatrix;
        cameraDataForMV[nEye].destViewToWorldMatrix = cameraData[nEyeOther].srcViewToWorldMatrix;
        cameraDataForMV[nEye].destViewToClipMatrix = cameraData[nEyeOther].srcViewToClipMatrix;
        cameraDataForMV[nEye].destClipToViewMatrix = cameraData[nEyeOther].srcClipToViewMatrix;

        cameraDataForMV[nEyeOther].destWorldToViewMatrixPrev = cameraData[nEye].srcWorldToViewMatrix;
        cameraDataForMV[nEyeOther].destViewToWorldMatrixPrev = cameraData[nEye].srcViewToWorldMatrix;
        cameraDataForMV[nEyeOther].destViewToClipMatrixPrev = cameraData[nEye].srcViewToClipMatrix;
        cameraDataForMV[nEyeOther].destClipToViewMatrixPrev = cameraData[nEye].srcClipToViewMatrix;

        auto offset = last_update_matrix_frame_count[nEye] - last_update_camera_data_frame_count;
        offset = std::clamp(offset, 0, 2) / 2;

        cameraData[nEye].camWorldToViewMatrix = glm::mat4(); // not used
        cameraData[nEye].camViewToWorldMatrix = glm::mat4(); // not used
        cameraData[nEye].destViewToWorldMatrix = render_view_inv_matrix[nEye][offset].other;
        cameraData[nEye].srcViewToWorldMatrix = render_view_inv_matrix[nEye][offset].curr;
        cameraData[nEye].destWorldToViewMatrix = glm::inverse(cameraData[nEye].destViewToWorldMatrix);
        cameraData[nEye].srcWorldToViewMatrix = glm::inverse(cameraData[nEye].srcViewToWorldMatrix);

        //float x = jitterOffset[0] / get_hmd_width();
        //float y = jitterOffset[1] / get_hmd_height();
        //render_projection_matrix[nEye].curr[2][0] += x;
        //render_projection_matrix[nEye].curr[2][1] += y;
        cameraData[nEye].destViewToClipMatrix = to_reverseZ(render_projection_matrix[nEye].other);
        cameraData[nEye].srcViewToClipMatrix = to_reverseZ(render_projection_matrix[nEye].curr);
        cameraData[nEye].destClipToViewMatrix = glm::inverse(cameraData[nEye].destViewToClipMatrix);
        cameraData[nEye].srcClipToViewMatrix = glm::inverse(cameraData[nEye].srcViewToClipMatrix);
        cameraData[nEye].camViewToClipMatrix = glm::mat4(); // not used
        cameraData[nEye].camClipToViewMatrix = glm::mat4(); // not used

        cameraDataForMV[nEye].srcWorldToViewMatrix = cameraData[nEye].srcWorldToViewMatrix;
        cameraDataForMV[nEye].srcViewToWorldMatrix = cameraData[nEye].srcViewToWorldMatrix;
        cameraDataForMV[nEye].srcViewToClipMatrix = cameraData[nEye].srcViewToClipMatrix;
        cameraDataForMV[nEye].srcClipToViewMatrix = cameraData[nEye].srcClipToViewMatrix;
    }
}

void VR::on_present() {
    ZoneScopedN(__FUNCTION__);

    static bool btn1 = false;
    if (GetAsyncKeyState(VK_NUMPAD1) < 0 && btn1 == false) {
        btn1 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD1) == 0 && btn1 == true) {
        btn1 = false;
        mDebug1 = !mDebug1;
    }
    static bool btn2 = false;
    if (GetAsyncKeyState(VK_NUMPAD2) < 0 && btn2 == false) {
        btn2 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD2) == 0 && btn2 == true) {
        btn2 = false;
        mDebug2 = !mDebug2;
    }
    static bool btn3 = false;
    if (GetAsyncKeyState(VK_NUMPAD3) < 0 && btn3 == false) {
        btn3 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD3) == 0 && btn3 == true) {
        btn3 = false;
#ifdef _DEBUG
        mDebug3 = !mDebug3;
        mDebug2 = false;
        mDebug1 = false;
#endif
    }
    static bool btn4 = false;
    if (GetAsyncKeyState(VK_NUMPAD4) < 0 && btn4 == false) {
        btn4 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD4) == 0 && btn4 == true) {
        btn4 = false;
        //m_fix_object_motion_vector->toggle();
    }

    m_present_thread_id = GetCurrentThreadId();

    utility::ScopeGuard _guard {[&]() {
        if (!is_using_afr() || is_using_afw() || (m_render_frame_count + 1) % 2 == m_left_eye_interval) {
            SetEvent(m_present_finished_event);
        }

        m_last_frame_count = m_render_frame_count;
    }};

    m_frame_count = get_runtime()->internal_render_frame_count;

    if (!is_using_afr() || is_using_afw() || m_render_frame_count % 2 == m_left_eye_interval) {
        ResetEvent(m_present_finished_event);
    }

    auto runtime = get_runtime();

    if (!runtime->loaded) {
        m_fake_stereo_hook->on_frame(); // Just let all the hooks engage, whatever.
        return;
    }

    runtime->consume_events(nullptr);

    m_fake_stereo_hook->on_frame();

    auto openvr = get_runtime<runtimes::OpenVR>();

    if (runtime->is_openvr()) {
        if (openvr->got_first_poses) {
            const auto hmd_activity = openvr->hmd->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
            auto hmd_active = hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction || hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction_Timeout;

            if (hmd_active) {
                openvr->last_hmd_active_time = std::chrono::system_clock::now();
            }

            const auto now = std::chrono::system_clock::now();

            if (now - openvr->last_hmd_active_time <= std::chrono::seconds(5)) {
                hmd_active = true;
            }

            openvr->is_hmd_active = hmd_active;

            // upon headset re-entry, reinitialize OpenVR
            if (openvr->is_hmd_active && !openvr->was_hmd_active) {
                openvr->wants_reinitialize = true;
            }

            openvr->was_hmd_active = openvr->is_hmd_active;

            if (!is_hmd_active()) {
                return;
            }
        } else {
            openvr->is_hmd_active = true; // We need to force out an initial WaitGetPoses call
            openvr->was_hmd_active = true;
        }
    }

    // attempt to fix crash when reinitializing openvr
    std::scoped_lock _{m_openvr_mtx};
    m_submitted = false;

    static bool btn5 = false;
    if (GetAsyncKeyState(VK_NUMPAD5) < 0 && btn5 == false) {
        btn5 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD5) == 0 && btn5 == true) {
        btn5 = false;
#ifdef _DEBUG
        auto& value = m_framewarp_mode->value();
        if (value == FrameWarpMode::AlternateEyeWarping)
            value = FrameWarpMode::PreviousFrameWarping;
        else if (value == FrameWarpMode::PreviousFrameWarping)
            value = FrameWarpMode::CombinedWarping;
        else if (value == FrameWarpMode::CombinedWarping)
            value = FrameWarpMode::AlternateEyeWarping;
#endif
    }
    static bool btn6 = false;
    if (GetAsyncKeyState(VK_NUMPAD6) < 0 && btn6 == false) {
        btn6 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD6) == 0 && btn6 == true) {
        btn6 = false;
        m_framewarp_debug->toggle();
    }
    static bool btn7 = false;
    if (GetAsyncKeyState(VK_NUMPAD7) < 0 && btn7 == false) {
        btn7 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD7) == 0 && btn7 == true) {
        btn7 = false;
        //m_clear_before_framewarp->toggle();
    }
    static bool btn8 = false;
    if (GetAsyncKeyState(VK_NUMPAD8) < 0 && btn8 == false) {
        btn8 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD8) == 0 && btn8 == true) {
        btn8 = false;
    }
    static bool btn9 = false;
    if (GetAsyncKeyState(VK_NUMPAD9) < 0 && btn9 == false) {
        btn9 = true;
    }
    if (GetAsyncKeyState(VK_NUMPAD9) == 0 && btn9 == true) {
        btn9 = false;
    }
    static bool btnAdd = false;
    if (GetAsyncKeyState(VK_ADD) < 0 && btnAdd == false) {
        btnAdd = true;
    }
    if (GetAsyncKeyState(VK_ADD) == 0 && btnAdd == true) {
        btnAdd = false;
    }
    static bool btnSub = false;
    if (GetAsyncKeyState(VK_SUBTRACT) < 0 && btnSub == false) {
        btnSub = true;
    }
    if (GetAsyncKeyState(VK_SUBTRACT) == 0 && btnSub == true) {
        btnSub = false;
    }
    static bool btnMul = false;
    if (GetAsyncKeyState(VK_MULTIPLY) < 0 && btnMul == false) {
        btnMul = true;
    }
    if (GetAsyncKeyState(VK_MULTIPLY) == 0 && btnMul == true) {
        btnMul = false;
    }

    update_camera_data(m_render_frame_count);

    const auto renderer = g_framework->get_renderer_type();
    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    const auto is_left_eye_frame = is_using_afr() ? (m_render_frame_count % 2 == m_left_eye_interval) : true;

    if ((is_left_eye_frame || is_using_afw()) && get_synchronize_stage() == VR::SynchronizeStage::LATE) {
        const auto had_sync = runtime->got_first_sync;
        runtime->synchronize_frame();

        if (!runtime->got_first_poses || !had_sync) {
            update_hmd_state();
        }
    }

    if (renderer == Framework::RendererType::D3D11) {
        // if we don't do this then D3D11 OpenXR freezes for some reason.
        if (!runtime->got_first_sync) {
            SPDLOG_INFO_EVERY_N_SEC(1, "Attempting to sync!");
            if (get_synchronize_stage() == VR::SynchronizeStage::LATE) {
                runtime->synchronize_frame();
            }

            update_hmd_state();
        }

        m_is_d3d12 = false;
        e = m_d3d11.on_frame(this);
    } else if (renderer == Framework::RendererType::D3D12) {
        m_is_d3d12 = true;
        e = m_d3d12.on_frame(this);
    }

    // force a waitgetposes call to fix this...
    if (e == vr::EVRCompositorError::VRCompositorError_AlreadySubmitted && runtime->is_openvr()) {
        openvr->got_first_poses = false;
        openvr->needs_pose_update = true;
    }

    if (m_submitted) {
        if (m_submitted) {
            if (!m_disable_overlay) {
                m_overlay_component.on_post_compositor_submit();
            }

            if (runtime->is_openvr()) {
                //vr::VRCompositor()->SetExplicitTimingMode(vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
                //vr::VRCompositor()->PostPresentHandoff();
            }
        }

        //runtime->needs_pose_update = true;
        m_submitted = false;

        // On the first ever submit, we need to activate the window and set the mouse to the center
        // so the user doesn't have to click on the window to get input.
        if (m_first_submit) {
            m_first_submit = false;

            // for some reason this doesn't work if called directly from here
            // so we have to do it in a separate thread
            std::thread worker([]() {
                g_framework->activate_window();
                g_framework->set_mouse_to_center();
                spdlog::info("Finished first submit from worker thread!");
            });
            worker.detach();
        }
    }
    if (afw_resolution_change_skip_frames > 0)
        afw_resolution_change_skip_frames--;
    if (afw_switching_skip_frames > 0)
        afw_switching_skip_frames--;
    if (is_afw_last_frame ^ (m_rendering_method->value() == RenderingMethod::ALTERNATE_FRAMEWARP && !is_using_2d_screen())) {
        afw_switching_skip_frames = 90;
    }
    is_afw_last_frame = (m_rendering_method->value() == RenderingMethod::ALTERNATE_FRAMEWARP && !is_using_2d_screen());
    afw_since_inject_frame_count++;
}

void VR::on_post_present() {
    FrameMarkNamed("Present");
    ZoneScopedN(__FUNCTION__);

    const auto is_same_frame = m_render_frame_count > 0 && m_render_frame_count == m_frame_count;

    m_render_frame_count = m_frame_count;

    auto runtime = get_runtime();

    if (!get_runtime()->loaded) {
        return;
    }

    std::scoped_lock _{m_openvr_mtx};

    if (!m_is_d3d12) {
        m_d3d11.on_post_present(this);
    } else {
        m_d3d12.on_post_present(this);
    }

    detect_controllers();

    const auto is_left_eye_frame = is_using_afr() ? (is_same_frame || (m_render_frame_count % 2 == m_left_eye_interval)) : true;

    if ((is_left_eye_frame || is_using_afw())) {
        if (get_synchronize_stage() == VR::SynchronizeStage::VERY_LATE || !runtime->got_first_sync) {
            const auto had_sync = runtime->got_first_sync;
            runtime->synchronize_frame();

            if (!runtime->got_first_poses || !had_sync) {
                update_hmd_state();
            }
        }

        if (runtime->is_openxr() && runtime->ready() && get_synchronize_stage() > VR::SynchronizeStage::EARLY) {
            if (!m_openxr->frame_began) {
                m_openxr->begin_frame();
            }
        }
    }

    if (runtime->wants_reinitialize) {
        std::scoped_lock _{m_reinitialize_mtx};

        if (runtime->is_openvr()) {
            m_openvr->wants_reinitialize = false;
            reinitialize_openvr();
        } else if (runtime->is_openxr()) {
            m_openxr->wants_reinitialize = false;
            reinitialize_openxr();
        }
    }
}

uint32_t VR::get_hmd_width() const {
    if (m_2d_screen_mode->value()) {
        if (get_runtime()->is_openxr()) {
            return g_framework->get_rt_size().x * m_openxr->resolution_scale->value();
        }

        return g_framework->get_rt_size().x;
    }

    if (m_extreme_compat_mode->value()) {
        return g_framework->get_rt_size().x;
    }

    return std::max<uint32_t>(get_runtime()->get_width(), 128);
}

uint32_t VR::get_hmd_height() const {
    if (m_2d_screen_mode->value()) {
        if (get_runtime()->is_openxr()) {
            return g_framework->get_rt_size().y * m_openxr->resolution_scale->value();
        }

        return g_framework->get_rt_size().y;
    }

    if (m_extreme_compat_mode->value()) {
        return g_framework->get_rt_size().y;
    }

    return std::max<uint32_t>(get_runtime()->get_height(), 128);
}

void VR::on_draw_sidebar_entry(std::string_view name) {
    const auto hash = utility::hash(name.data());

    // Draw the ui thats always drawn first.
    on_draw_ui();

    /*const auto made_child = ImGui::BeginChild("VRChild", ImVec2(0, 0), true, ImGuiWindowFlags_::ImGuiWindowFlags_NavFlattened);

    utility::ScopeGuard sg([made_child]() {
        if (made_child) {
            ImGui::EndChild();
        }
    });*/

    enum SelectedPage {
        PAGE_RUNTIME,
        PAGE_UNREAL,
        PAGE_INPUT,
        PAGE_CAMERA,
        PAGE_KEYBINDS,
        PAGE_MONITOR3D,
        PAGE_CONSOLE,
        PAGE_COMPATIBILITY,
        PAGE_DEBUG,
    };

    SelectedPage selected_page = PAGE_RUNTIME;

    /*ImGui::BeginTable("VRTable", 2, ImGuiTableFlags_::ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_::ImGuiTableFlags_BordersOuterV | ImGuiTableFlags_::ImGuiTableFlags_SizingFixedFit);
    ImGui::TableSetupColumn("LeftPane", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableSetupColumn("RightPane", ImGuiTableColumnFlags_WidthStretch);

    // Draw left pane
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); // Set to the first column

        ImGui::BeginGroup();

        auto dcs = [&](const char* label, SelectedPage page_value) -> bool {
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
            utility::ScopeGuard sg3([]() {
                ImGui::PopStyleVar();
            });
            if (ImGui::Selectable(label, selected_page == page_value)) {
                selected_page = page_value;
                return true;
            }
            return false;
        };

        dcs("Runtime", PAGE_RUNTIME);
        dcs("Unreal", PAGE_UNREAL);
        dcs("Input", PAGE_INPUT);
        dcs("Camera", PAGE_CAMERA);
        dcs("Console/CVars", PAGE_CONSOLE);
        dcs("Compatibility", PAGE_COMPATIBILITY);
        dcs("Debug", PAGE_DEBUG);

        ImGui::EndGroup();
    }

    ImGui::TableNextColumn(); // Move to the next column (right)
    ImGui::BeginGroup();*/

    switch (hash) {
    case "Runtime"_fnv:
        selected_page = PAGE_RUNTIME;
        break;
    case "Unreal"_fnv:
        selected_page = PAGE_UNREAL;
        break;
    case "Input"_fnv:
        selected_page = PAGE_INPUT;
        break;
    case "Camera"_fnv:
        selected_page = PAGE_CAMERA;
        break;
    case "Keybinds"_fnv:
        selected_page = PAGE_KEYBINDS;
        break;
    case "3D Display"_fnv:
        selected_page = PAGE_MONITOR3D;
        break;
    case "Console/CVars"_fnv:
        selected_page = PAGE_CONSOLE;
        break;
    case "Compatibility"_fnv:
        selected_page = PAGE_COMPATIBILITY;
        break;
    case "Debug"_fnv:
        selected_page = PAGE_DEBUG;
        break;
    default:
        ImGui::Text("Unknown page selected");
        break;
    }

    if (selected_page == PAGE_RUNTIME) {
        if (m_has_hw_scheduling) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
            ImGui::TextWrapped("WARNING: Hardware-accelerated GPU scheduling is enabled. This may cause the game to run slower.");
            ImGui::TextWrapped("Go into your Windows Graphics settings and disable \"Hardware-accelerated GPU scheduling\"");
            ImGui::PopStyleColor();
            ImGui::TextWrapped("Note: This is only necessary if you are experiencing performance issues.");
        }

        if (GetModuleHandleW(L"nvngx_dlssg.dll") != nullptr) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
            ImGui::TextWrapped("WARNING: DLSS Frame Generation has been detected. Make sure it is disabled within in-game settings.");
            ImGui::PopStyleColor();
        }

        ImGui::Text((std::string{"Runtime Information ("} + get_runtime()->name().data() + ")").c_str());

        // No-ops under 3D Display mode: the flat3d compositor owns the real
        // backbuffer (no desktop mirror) and 2D-screen is mutually exclusive.
        if (!is_using_flat3d()) {
            m_desktop_fix->draw("Desktop Spectator View");

            if (m_desktop_fix->value()) {
                m_desktop_mirror_mode->draw("Desktop Spectator View Mode");
            }

            m_2d_screen_mode->draw("2D Screen Mode");
        }

        ImGui::TextWrapped("Render Resolution (per-eye): %d x %d", get_runtime()->get_width(), get_runtime()->get_height());
        ImGui::TextWrapped("Total Render Resolution: %d x %d", get_runtime()->get_width() * 2, get_runtime()->get_height());

        if (get_runtime()->is_openvr()) {
            ImGui::TextWrapped("Resolution can be changed in SteamVR");
        }

        get_runtime()->on_draw_ui();

        if (!is_using_flat3d()) {
            // VR-compositor overlay options — inert without a VR compositor.
            m_overlay_component.on_draw_ui();
        }

        ImGui::TreePop();
    }

    if (selected_page == PAGE_UNREAL) {
        m_rendering_method->draw("Rendering Method");

        if (is_using_synchronized_afr()) {
            ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
            if (ImGui::TreeNode("Synced Sequential")) {
                m_synced_afr_method->draw("Synced Sequential Method");
                ImGui::TreePop();
            }
        }

        if (is_using_afw_without_api_check()) {
            if (g_framework->is_dx12()) {
                ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
                if (ImGui::TreeNode("Alternate Frame Warping")) {
                    m_framewarp_mode->draw("Framewarp Mode");
                    if (is_no_dlss()) {
                        ImGui::TextWrapped("No DLSS instance detected, are you sure you have turned on DLSS in-game?");
                    }
                    if (m_framewarp_mode->value() == CombinedWarping) {
                        m_framewarp_shading_rate->draw("Framewarp Shading Rate");
                    }
                    ImGui::Spacing();
                    //m_use_uint64->draw("Use UINT64");
                    m_clear_before_framewarp->draw("Clear Before Framewarp");
                    m_framewarp_debug->draw("Debug Framewarp");
                    ImGui::Spacing();
                    m_enable_sharpening->draw("Enable Sharpening");
                    m_sharpness->draw("Sharpness");
                    ImGui::Spacing();
                    if (is_ghosting_fix_enabled()) {
                        m_fix_object_motion_vector->draw("Fix Object Motion Vector");
                        m_fix_object_motion_range->draw("Fix Object Motion Rnage");
                        if (is_fix_object_motion_vector() && !rawVelocityDesc[0].pTexture) {
                            ImGui::TextWrapped("No UE Velocity Buffer found, can't use the object motion vector fix.");
                        } else {
                            m_fix_moving_object_brightness_flickering->draw("Fix Moving Object Brightness Flickering");
                        }
                        ImGui::Spacing();
                    } else {
                        ImGui::BeginDisabled(!is_ghosting_fix_enabled());
                        m_fix_object_motion_vector->draw("Fix Object Motion Vector");
                        ImGui::EndDisabled();
                        ImGui::TextWrapped("Object Motion fix is only needed when ghosting fix is enabled.");
                    }
                    m_ignore_motion_threshold->draw("Ignore Motion Threshold");
                    m_ultra_responsive->draw("Ultra Responsive");
                    ImGui::TextWrapped("This basically just disables lerping in UObjectHook Config tab.");
                    ImGui::TreePop();
                }
            } else {
                ImGui::TextWrapped("Using DX11, AFW only supports DX12, fallback to AFR.");
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        m_world_scale->draw("World Scale");
        m_depth_scale->draw("Depth Scale");

        m_disable_hzbocclusion->draw("Disable HZBOcclusion");
        m_disable_instance_culling->draw("Disable Instance Culling");
        m_disable_hdr_compositing->draw("Disable HDR Composition");
        m_disable_blur_widgets->draw("Disable Blur Widgets");
        m_uncap_framerate->draw("Uncap Framerate");
        m_enable_gui->draw("Enable GUI");

        if (!is_using_flat3d()) {
            // Depth submission to the VR compositor — no compositor here.
            m_enable_depth->draw("Enable Depth-based Latency Reduction");
        }
        m_load_blueprint_code->draw("Load Blueprint Code");
        m_ghosting_fix->draw("Ghosting Fix");

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Native Stereo Fix")) {
            m_native_stereo_fix->draw("Enabled");
            m_native_stereo_fix_same_pass->draw("Use Same Stereo Pass");
            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Near Clip Plane")) {
            m_custom_z_near_enabled->draw("Enable");

            if (m_custom_z_near_enabled->value()) {
                m_custom_z_near->draw("Value");

                if (m_custom_z_near->value() <= 0.0f) {
                    m_custom_z_near->value() = 0.01f;
                }
            }

            ImGui::TreePop();
        }
    }

    if (selected_page == PAGE_INPUT && is_using_flat3d()) {
        ImGui::TextWrapped("Motion-controller input options are not applicable in 3D Display mode\n"
                           "(no VR controllers). Keyboard/mouse hotkeys are on the Keybinds and\n"
                           "3D Display pages.");
    } else if (selected_page == PAGE_INPUT) {
        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Controller")) {
            m_joystick_deadzone->draw("VR Joystick Deadzone");
            m_controller_pitch_offset->draw("Controller Pitch Offset");

            m_dpad_shifting->draw("DPad Shifting");
            ImGui::SameLine();
            m_swap_controllers->draw("Left-handed Controller Inputs");
            m_dpad_shifting_method->draw("DPad Shifting Method");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Aim Method")) {
            ImGui::TextWrapped("Some games may not work with this enabled.");
            if (m_aim_method->draw("Type")) {
                m_previous_aim_method = (AimMethod)m_aim_method->value();
            }

            m_aim_speed->draw("Speed");
            m_aim_interp->draw("Smoothing");

            m_aim_modify_player_control_rotation->draw("Modify Player Control Rotation");
            ImGui::SameLine();
            m_aim_use_pawn_control_rotation->draw("Use Pawn Control Rotation");

            m_aim_multiplayer_support->draw("Multiplayer Support");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Snap Turn")) {
            m_snapturn->draw("Enabled");
            m_snapturn_angle->draw("Angle");
            m_snapturn_joystick_deadzone->draw("Deadzone");
        
            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Movement Orientation")) {
            m_movement_orientation->draw("Type");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Roomscale Movement")) {
            m_roomscale_movement->draw("Enabled");

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, headset movement will affect the movement of the player character.");
            }

            ImGui::SameLine();
            m_roomscale_sweep->draw("Sweep Movement");
            // Draw description of option
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, roomscale movement will use a sweep to prevent the player from moving through walls.\nThis also allows physics objects to interact with the player, like doors.");
            }

            ImGui::TreePop();
        }
    }

    if (selected_page == PAGE_CAMERA) {
        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Camera Freeze")) {
            float camera_offset[] = {m_camera_forward_offset->value(), m_camera_right_offset->value(), m_camera_up_offset->value()};
            if (ImGui::SliderFloat3("Camera Offset", camera_offset, -4000.0f, 4000.0f)) {
                m_camera_forward_offset->value() = camera_offset[0];
                m_camera_right_offset->value() = camera_offset[1];
                m_camera_up_offset->value() = camera_offset[2];
            }

            for (auto i = 0; i < m_camera_datas.size(); ++i) {
                auto& data = m_camera_datas[i];

                if (ImGui::Button(std::format("Save Camera {}", i).data())) {
                    save_camera(i);
                }

                ImGui::SameLine();

                if (ImGui::Button(std::format("Load Camera {}", i).data())) {
                    load_camera(i);
                }
            }

            bool pos_freeze = m_camera_freeze.position_frozen || m_camera_freeze.position_wants_freeze;
            if (ImGui::Checkbox("Freeze Position", &pos_freeze)) {
                if (pos_freeze) {
                    m_camera_freeze.position_wants_freeze = true;
                } else {
                    m_camera_freeze.position_frozen = false;
                }
            }

            ImGui::SameLine();
            bool rot_freeze = m_camera_freeze.rotation_frozen || m_camera_freeze.rotation_wants_freeze;
            if (ImGui::Checkbox("Freeze Rotation", &rot_freeze)) {
                if (rot_freeze) {
                    m_camera_freeze.rotation_wants_freeze = true;
                } else {
                    m_camera_freeze.rotation_frozen = false;
                }
            }

            ImGui::TreePop();
        }

        // No-op under 3D Display mode: the flat3d projection is rebuilt from the
        // game's own live FoV every frame (see the 3D Display page's Camera FoV
        // Axis / FoV Multiplier), so there is no HMD FoV to match it to.
        const bool show_game_fov = !is_using_flat3d();

        if (show_game_fov) {
            ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        }

        if (show_game_fov && ImGui::TreeNode("Game FOV")) {
            m_match_game_fov->draw("Match Game FOV");

            if (m_match_game_fov->value()) {
                if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen)) {
                    m_match_game_fov_dolly->draw("Use Dolly Instead of FOV");
                    m_match_game_fov_multiplier->draw("FOV Multiplier");
                    m_match_game_fov_min_enabled->draw("Clamp Minimum FOV");
                    if (m_match_game_fov_min_enabled->value()) {
                        m_match_game_fov_min->draw("Minimum FOV");
                    }
                    m_match_game_fov_read_only_camera->draw("Read Game Camera Only (No FOV Writes)");

                    if (m_match_game_fov_dolly->value()) {
                        m_match_game_fov_dolly_distance->draw_drag("Dolly Focus Distance", 10.0f, "%.0f");
                        ImGui::Text("Dolly Offset: %.2f", get_game_fov_dolly_offset());
                    }
                }

                if (ImGui::CollapsingHeader("Camera Cut Stabilizer")) {
                    m_match_game_fov_camera_cut_stabilizer->draw("Enable Camera Cut Stabilizer");
                    if (m_match_game_fov_camera_cut_stabilizer->value()) {
                        m_match_game_fov_camera_cut_stabilizer_duration_ms->draw("Stabilizer Duration (ms)");
                        m_match_game_fov_camera_cut_stabilizer_fov_delta->draw("Cut FOV Delta Threshold");
                        m_match_game_fov_camera_cut_stabilizer_rotation_delta->draw("Cut Rotation Delta Threshold");
                        m_match_game_fov_camera_cut_stabilizer_location_delta->draw("Cut Location Delta Threshold");
                    }
                }

                if (m_match_game_fov_dolly->value() &&
                    ImGui::CollapsingHeader("Generic Camera Presets")) {
                    m_match_game_fov_generic_camera_presets->draw("Enable Generic Camera Presets");
                    if (m_match_game_fov_generic_camera_presets->value()) {
                        m_match_game_fov_generic_camera_presets_auto_apply->draw("Auto Apply Saved Camera Presets");

                        if (ImGui::Button("Save Current Generic Camera Preset")) {
                            save_current_generic_camera_preset();
                        }

                        ImGui::SameLine();

                        if (ImGui::Button("Clear Current Generic Camera Preset")) {
                            clear_current_generic_camera_preset();
                        }
                    }
                }

                if (is_prospi_executable() && m_match_game_fov_dolly->value() &&
                    ImGui::CollapsingHeader("ProSpi Actual FOV Clamp", ImGuiTreeNodeFlags_DefaultOpen)) {
                    m_match_game_fov_prospi_actual_clamp->draw("Clamp Actual Game FOV (ProSpi)");
                    if (m_match_game_fov_prospi_actual_clamp->value()) {
                        m_match_game_fov_prospi_actual_min->draw("Default ProSpi Actual Minimum FOV");
                        m_match_game_fov_prospi_center_field_actual_min->draw("Center Field Minimum FOV");
                        m_match_game_fov_prospi_upper_deck_actual_min->draw("Upper Deck Minimum FOV");
                        m_match_game_fov_prospi_plate_high_actual_min->draw("High Plate Minimum FOV");
                        m_match_game_fov_prospi_deep_outfield_actual_min->draw("Deep Outfield Minimum FOV");
                    }
                }

                if (is_prospi_executable() &&
                    ImGui::CollapsingHeader("ProSpi Telephoto Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
                    m_match_game_fov_prospi_telephoto_perf_override->draw("Enable Telephoto Performance Override");
                    if (m_match_game_fov_prospi_telephoto_perf_override->value()) {
                        m_match_game_fov_prospi_telephoto_perf_trigger_fov->draw("Telephoto Trigger FOV");
                        m_match_game_fov_prospi_telephoto_perf_view_distance_scale->draw("Telephoto View Distance Scale");
                        m_match_game_fov_prospi_telephoto_perf_static_mesh_lod_distance_scale->draw("Telephoto Static Mesh LOD Distance Scale");
                        m_match_game_fov_prospi_telephoto_perf_skeletal_mesh_lod_bias->draw("Telephoto Skeletal Mesh LOD Bias");
                    }
                }

                if (is_prospi_executable() && m_match_game_fov_dolly->value() &&
                    ImGui::CollapsingHeader("ProSpi Dolly Overrides", ImGuiTreeNodeFlags_DefaultOpen)) {
                    m_match_game_fov_prospi_tv_dolly_override->draw("Auto Override TV View Dolly");
                    if (m_match_game_fov_prospi_tv_dolly_override->value()) {
                        m_match_game_fov_prospi_tv_dolly_distance->draw_drag("TV View Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_opening_aerial_dolly_override->draw("Auto Override Opening Aerial Dolly");
                    if (m_match_game_fov_prospi_opening_aerial_dolly_override->value()) {
                        m_match_game_fov_prospi_opening_aerial_dolly_distance->draw_drag("Opening Aerial Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_behind_plate_wide_dolly_override->draw("Auto Override Behind Plate Wide Dolly");
                    if (m_match_game_fov_prospi_behind_plate_wide_dolly_override->value()) {
                        m_match_game_fov_prospi_behind_plate_wide_dolly_distance->draw_drag("Behind Plate Wide Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_home_plate_waist_high_reverse_dolly_override->draw("Auto Override Waist High Reverse Dolly");
                    if (m_match_game_fov_prospi_home_plate_waist_high_reverse_dolly_override->value()) {
                        m_match_game_fov_prospi_home_plate_waist_high_reverse_dolly_distance->draw_drag("Waist High Reverse Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_low_plate_corner_dolly_override->draw("Auto Override Low Infield Close-Up Dolly");
                    if (m_match_game_fov_prospi_low_plate_corner_dolly_override->value()) {
                        m_match_game_fov_prospi_low_plate_corner_dolly_distance->draw_drag("Low Infield Close-Up Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_center_field_dolly_override->draw("Auto Override Center Field Dolly");
                    if (m_match_game_fov_prospi_center_field_dolly_override->value()) {
                        m_match_game_fov_prospi_center_field_dolly_distance->draw_drag("Center Field Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_center_field_high_dolly_override->draw("Auto Override Center Field High Dolly");
                    if (m_match_game_fov_prospi_center_field_high_dolly_override->value()) {
                        m_match_game_fov_prospi_center_field_high_dolly_distance->draw_drag("Center Field High Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_left_field_corner_wide_dolly_override->draw("Auto Override Offset Center Field Dolly");
                    if (m_match_game_fov_prospi_left_field_corner_wide_dolly_override->value()) {
                        m_match_game_fov_prospi_left_field_corner_wide_dolly_distance->draw_drag("Offset Center Field Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_deep_outfield_dolly_override->draw("Auto Override Deep Outfield Dolly");
                    if (m_match_game_fov_prospi_deep_outfield_dolly_override->value()) {
                        m_match_game_fov_prospi_deep_outfield_dolly_distance->draw_drag("Deep Outfield Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_home_plate_sky_dolly_override->draw("Auto Override Home Plate Sky Dolly");
                    if (m_match_game_fov_prospi_home_plate_sky_dolly_override->value()) {
                        m_match_game_fov_prospi_home_plate_sky_dolly_distance->draw_drag("Home Plate Sky Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_upper_deck_dolly_override->draw("Auto Override Upper Deck 3B Dolly");
                    if (m_match_game_fov_prospi_upper_deck_dolly_override->value()) {
                        m_match_game_fov_prospi_upper_deck_dolly_distance->draw_drag("Upper Deck 3B Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_home_sky_dolly_override->draw("Auto Override Home Sky Dolly");
                    if (m_match_game_fov_prospi_home_sky_dolly_override->value()) {
                        m_match_game_fov_prospi_home_sky_dolly_distance->draw_drag("Home Sky Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_third_base_dolly_override->draw("Auto Override Third Base Line Dolly");
                    if (m_match_game_fov_prospi_third_base_dolly_override->value()) {
                        m_match_game_fov_prospi_third_base_dolly_distance->draw_drag("Third Base Line Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_third_base_relay_low_dolly_override->draw("Auto Override Third Base Relay Low Dolly");
                    if (m_match_game_fov_prospi_third_base_relay_low_dolly_override->value()) {
                        m_match_game_fov_prospi_third_base_relay_low_dolly_distance->draw_drag("Third Base Relay Low Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_third_base_sweep_dolly_override->draw("Auto Override Third Base Corner Low Dolly");
                    if (m_match_game_fov_prospi_third_base_sweep_dolly_override->value()) {
                        m_match_game_fov_prospi_third_base_sweep_dolly_distance->draw_drag("Third Base Corner Low Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_third_base_wide_dolly_override->draw("Auto Override Third Base Wide Dolly");
                    if (m_match_game_fov_prospi_third_base_wide_dolly_override->value()) {
                        m_match_game_fov_prospi_third_base_wide_dolly_distance->draw_drag("Third Base Wide Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_first_base_dolly_override->draw("Auto Override First Base Line Dolly");
                    if (m_match_game_fov_prospi_first_base_dolly_override->value()) {
                        m_match_game_fov_prospi_first_base_dolly_distance->draw_drag("First Base Line Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_first_base_wide_dolly_override->draw("Auto Override First Base Wide Dolly");
                    if (m_match_game_fov_prospi_first_base_wide_dolly_override->value()) {
                        m_match_game_fov_prospi_first_base_wide_dolly_distance->draw_drag("First Base Wide Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_first_base_corner_low_dolly_override->draw("Auto Override First Base Corner Low Dolly");
                    if (m_match_game_fov_prospi_first_base_corner_low_dolly_override->value()) {
                        m_match_game_fov_prospi_first_base_corner_low_dolly_distance->draw_drag("First Base Corner Low Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_backstop_high_dolly_override->draw("Auto Override Backstop High Dolly");
                    if (m_match_game_fov_prospi_backstop_high_dolly_override->value()) {
                        m_match_game_fov_prospi_backstop_high_dolly_distance->draw_drag("Backstop High Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_right_field_corner_dolly_override->draw("Auto Override Right Field Corner Dolly");
                    if (m_match_game_fov_prospi_right_field_corner_dolly_override->value()) {
                        m_match_game_fov_prospi_right_field_corner_dolly_distance->draw_drag("Right Field Corner Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_right_center_field_dolly_override->draw("Auto Override Right Center Field Dolly");
                    if (m_match_game_fov_prospi_right_center_field_dolly_override->value()) {
                        m_match_game_fov_prospi_right_center_field_dolly_distance->draw_drag("Right Center Field Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_plate_high_dolly_override->draw("Auto Override High Plate Dolly");
                    if (m_match_game_fov_prospi_plate_high_dolly_override->value()) {
                        m_match_game_fov_prospi_plate_high_dolly_distance->draw_drag("High Plate Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_home_plate_overhead_dolly_override->draw("Auto Override Home Plate Overhead Dolly");
                    if (m_match_game_fov_prospi_home_plate_overhead_dolly_override->value()) {
                        m_match_game_fov_prospi_home_plate_overhead_dolly_distance->draw_drag("Home Plate Overhead Dolly Distance", 10.0f, "%.0f");
                    }
                }

                if (m_match_game_fov_dolly->value() &&
                    ImGui::CollapsingHeader("Camera Calibration", ImGuiTreeNodeFlags_DefaultOpen)) {
                    m_match_game_fov_prospi_camera_calibration_auto->draw("Auto Apply Camera Calibration");
                    if (ImGui::Button("Save Current Camera Calibration")) {
                        save_current_prospi_camera_calibration();
                    }

                    ImGui::SameLine();

                    if (ImGui::Button("Clear Current Camera Calibration")) {
                        clear_current_prospi_camera_calibration();
                    }

                    if (is_prospi_executable()) {
                        ImGui::SameLine();

                        if (ImGui::Button("Clear Current Preset Calibrations")) {
                            clear_current_prospi_preset_calibrations();
                        }
                    }
                }

                if (ImGui::CollapsingHeader("Live Status", ImGuiTreeNodeFlags_DefaultOpen)) {
                    const auto fov = get_game_fov();
                    const auto raw_fov = m_game_fov_raw.load(std::memory_order_relaxed);
                    const bool fov_valid = m_game_fov_valid.load(std::memory_order_relaxed);
                    const auto current_camera_id = get_current_game_camera_id();
                    const auto calibration_applied = m_match_game_fov_prospi_calibration_applied.load(std::memory_order_relaxed);
                    const auto calibration_multiplier = m_match_game_fov_prospi_calibration_multiplier_active.load(std::memory_order_relaxed);
                    const auto calibration_dolly_distance = m_match_game_fov_prospi_calibration_dolly_distance_active.load(std::memory_order_relaxed);
                    const auto read_only_active = m_match_game_fov_read_only_camera_active.load(std::memory_order_relaxed);
                    const auto would_write = m_match_game_fov_would_write_game_camera.load(std::memory_order_relaxed);
                    const auto stabilizer_active = m_match_game_fov_camera_cut_stabilizer_active.load(std::memory_order_relaxed);
                    const auto stabilizer_remaining_ms = m_match_game_fov_camera_cut_stabilizer_remaining_ms.load(std::memory_order_relaxed);
                    const auto generic_preset_applied = m_match_game_fov_generic_camera_preset_applied.load(std::memory_order_relaxed);
                    ImGui::Text("Current Game FOV: %.2f (%s)", fov, fov_valid ? "valid" : "invalid");
                    ImGui::Text("Raw Game FOV: %.2f", raw_fov);
                    ImGui::Text("Current Camera ID: %s", current_camera_id.empty() ? "None" : current_camera_id.c_str());
                    ImGui::Text("Read-Only Camera Active: %s", read_only_active ? "yes" : "no");
                    ImGui::Text("Blocked Game FOV Write Pending: %s", would_write ? "yes" : "no");
                    ImGui::Text("Camera Cut Stabilizer: %s (%dms)", stabilizer_active ? "active" : "inactive", stabilizer_remaining_ms);
                    ImGui::Text("Generic Camera Preset Applied: %s", generic_preset_applied ? "yes" : "no");
                    ImGui::Text("Camera Calibration Applied: %s", calibration_applied ? "yes" : "no");
                    if (calibration_applied) {
                        ImGui::Text("Calibration Multiplier: %.2f", calibration_multiplier);
                        ImGui::Text("Calibration Dolly Distance: %.2f", calibration_dolly_distance);
                    }
                }

                if (is_prospi_executable() && m_match_game_fov_dolly->value() &&
                    ImGui::CollapsingHeader("ProSpi Live Status", ImGuiTreeNodeFlags_DefaultOpen)) {
                    const auto preset = (ProSpiCameraPreset)m_match_game_fov_prospi_preset.load(std::memory_order_relaxed);
                    const auto active_min = m_match_game_fov_prospi_actual_min_active.load(std::memory_order_relaxed);
                    const auto calibration_applied = m_match_game_fov_prospi_calibration_applied.load(std::memory_order_relaxed);
                    const auto calibration_min = m_match_game_fov_prospi_calibration_actual_min_active.load(std::memory_order_relaxed);
                    const auto calibration_multiplier = m_match_game_fov_prospi_calibration_multiplier_active.load(std::memory_order_relaxed);
                    const auto calibration_dolly_distance = m_match_game_fov_prospi_calibration_dolly_distance_active.load(std::memory_order_relaxed);
                    const auto tv_override_applied = m_match_game_fov_prospi_tv_override_active.load(std::memory_order_relaxed);
                    const auto auto_dolly_distance = m_match_game_fov_prospi_auto_dolly_distance_active.load(std::memory_order_relaxed);
                    const auto telephoto_perf_active = m_match_game_fov_prospi_telephoto_perf_active.load(std::memory_order_relaxed);
                    const auto current_camera_id = get_current_prospi_camera_id();
                    ImGui::Text("Active ProSpi Preset: %s", get_prospi_camera_preset_name(preset));
                    if (m_match_game_fov_prospi_actual_clamp->value()) {
                        ImGui::Text("Active ProSpi Minimum FOV: %.2f", active_min);
                    }
                    ImGui::Text("Current ProSpi Camera ID: %s", current_camera_id.empty() ? "None" : current_camera_id.c_str());
                    ImGui::Text("Calibration Applied: %s", calibration_applied ? "yes" : "no");
                    ImGui::Text("TV Override Active: %s", tv_override_applied ? "yes" : "no");
                    ImGui::Text("Telephoto Performance Override: %s", telephoto_perf_active ? "yes" : "no");
                    ImGui::Text("Auto Dolly Override Distance: %.2f", auto_dolly_distance);
                    if (calibration_applied) {
                        ImGui::Text("Calibration Minimum FOV: %.2f", calibration_min);
                        ImGui::Text("Calibration Multiplier: %.2f", calibration_multiplier);
                        ImGui::Text("Calibration Dolly Distance: %.2f", calibration_dolly_distance);
                    }
                }
            }

            ImGui::TreePop();
        }

        // No-op under 3D Display mode: lerping smooths the HMD pose against the
        // game camera, and flat3d has no headset pose to smooth.
        const bool show_camera_lerp = !is_using_flat3d();

        if (show_camera_lerp) {
            ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        }

        if (show_camera_lerp && ImGui::TreeNode("Camera Lerp")) {
            m_lerp_camera_pitch->draw("Lerp Pitch");
            ImGui::SameLine();
            m_lerp_camera_yaw->draw("Lerp Yaw");
            ImGui::SameLine();
            m_lerp_camera_roll->draw("Lerp Roll");
            m_lerp_camera_speed->draw("Lerp Speed");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Decoupled Pitch")) {
            m_decoupled_pitch->draw("Enabled");
            m_decoupled_pitch_ui_adjust->draw("Auto Adjust UI");

            ImGui::TreePop();
        }
    }

    if (selected_page == PAGE_KEYBINDS) {
        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Playspace Keys")) {
            m_keybind_recenter->draw("Recenter View Key");
            m_keybind_recenter_horizon->draw("Recenter Horizon Key");
            m_keybind_set_standing_origin->draw("Set Standing Origin Key");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Camera Keys")) {
            m_keybind_load_camera_0->draw("Load Camera 0 Key");
            m_keybind_load_camera_1->draw("Load Camera 1 Key");
            m_keybind_load_camera_2->draw("Load Camera 2 Key");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Overlay/Runtime Keys")) {
            m_keybind_toggle_2d_screen->draw("Toggle 2D Screen Mode Key");
            m_keybind_toggle_gui->draw("Toggle In-Game UI Key");
            m_keybind_disable_vr->draw("Disable VR Key");

            ImGui::TreePop();
        }
    }

    if (selected_page == PAGE_MONITOR3D) {
        on_draw_sidebar_flat3d();
    }

    if (selected_page == PAGE_CONSOLE) {
        m_cvar_manager->on_draw_ui();
    }

    if (selected_page == PAGE_COMPATIBILITY) {
        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Compatibility Options")) {
            m_compatibility_ahud->draw("AHUD UI Compatibility");
            m_overlay_component.draw_ui_invert_alpha("UI Invert Alpha");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Inverts/blends the game UI's alpha so it composites correctly\n"
                                  "(0 = off). Needed by some titles for HUD/UI visibility.");
            }
            m_flat3d_ui_color_gate->draw("UI Color Gate");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Zeroes the game UI's alpha where it has (almost) no color. Fixes the\n"
                                  "dark film over the whole scene when using UI Invert Alpha 0.5 (that\n"
                                  "collapses every pixel's alpha to a flat 0.5, including the empty\n"
                                  "screen): drawn UI has color, empty screen doesn't. Also keeps the\n"
                                  "full-screen-menu detector and the adaptive-HUD classifier working\n"
                                  "under a partial invert. Try 0.03-0.08; 0 = off. Side effect: pure-\n"
                                  "black opaque UI (dark panels) turns transparent - keep the gate low.");
            }
            m_compatibility_skip_uobjectarray_init->draw("Skip UObjectArray Init");
            m_compatibility_skip_pip->draw("Skip PostInitProperties");
            m_sceneview_compatibility_mode->draw("SceneView Compatibility Mode");
            m_compatibility_single_view_render_target->draw("Single-View Render Target (3D Display, AFR)");
            if (m_compatibility_single_view_render_target->value()) {
                ImGui::TextWrapped(
                    "Advertises a single-eye render target instead of the double-wide. Fixes AFR modes that "
                    "draw only the left half of the frame (Fantasy Life i).");
            }
            m_extreme_compat_mode->draw("Extreme Compatibility Mode");

            // changes to any of these options should trigger a regeneration of the eye projection matrices
            const auto horizontal_projection_changed = m_horizontal_projection_override->draw("Horizontal Projection");
            const auto vertical_projection_changed = m_vertical_projection_override->draw("Vertical Projection");
            const auto scale_render = m_grow_rectangle_for_projection_cropping->draw("Scale Render Target");
            const auto scale_render_changed = get_runtime()->is_modifying_eye_texture_scale != scale_render;
            get_runtime()->is_modifying_eye_texture_scale = scale_render;
            get_runtime()->should_recalculate_eye_projections = horizontal_projection_changed || vertical_projection_changed || scale_render_changed;

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Splitscreen Compatibility")) {
            m_splitscreen_compatibility_mode->draw("Enabled");
            m_splitscreen_view_index->draw("Index");
            ImGui::TreePop();
        }
    }
    
    if (selected_page == PAGE_DEBUG) {
        if (m_fake_stereo_hook != nullptr) {
            m_fake_stereo_hook->on_draw_ui();
        }

        //ImGui::Combo("Sync Mode", (int*)&get_runtime()->custom_stage, "Early\0Late\0Very Late\0");
        m_sync_mode->draw("Sync Mode");
        ImGui::DragFloat4("Right Bounds", (float*)&m_right_bounds, 0.005f, -2.0f, 2.0f);
        ImGui::DragFloat4("Left Bounds", (float*)&m_left_bounds, 0.005f, -2.0f, 2.0f);
        ImGui::Checkbox("Disable Projection Matrix Override", &m_disable_projection_matrix_override);
        ImGui::Checkbox("Disable View Matrix Override", &m_disable_view_matrix_override);
        ImGui::Checkbox("Disable Backbuffer Size Override", &m_disable_backbuffer_size_override);
        ImGui::Checkbox("Disable VR Overlay", &m_disable_overlay);
        ImGui::Checkbox("Disable VR Entirely", &m_disable_vr);
        ImGui::Checkbox("Stereo Emulation Mode", &m_stereo_emulation_mode);
        ImGui::Checkbox("Wait for Present", &m_wait_for_present);
        m_controllers_allowed->draw("Controllers allowed");
        ImGui::Checkbox("Controller test mode", &m_controller_test_mode);
        m_show_fps->draw("Show FPS");
        m_show_statistics->draw("Show Engine Statistics");

        const double min_ = 0.0;
        const double max_ = 25.0;
        ImGui::SliderScalar("Prediction Scale", ImGuiDataType_Double, &m_openxr->prediction_scale, &min_, &max_);

        ImGui::DragFloat4("Raw Left", (float*)&m_raw_projections[0], 0.01f, -100.0f, 100.0f);
        ImGui::DragFloat4("Raw Right", (float*)&m_raw_projections[1], 0.01f, -100.0f, 100.0f);

        const auto left_stick_axis = get_left_stick_axis();
        const auto right_stick_axis = get_right_stick_axis();

        ImGui::DragFloat2("Left Stick", (float*)&left_stick_axis, 0.01f, -1.0f, 1.0f);
        ImGui::DragFloat2("Right Stick", (float*)&right_stick_axis, 0.01f, -1.0f, 1.0f);

        ImGui::TextWrapped("Hardware scheduling: %s", m_has_hw_scheduling ? "Enabled" : "Disabled");
    }

    ImGui::EndGroup();
    //ImGui::EndTable();
}

void VR::on_draw_ui() {
    ZoneScopedN(__FUNCTION__);

    // create VR tree entry in menu (imgui)
    ImGui::PushID("VR");
    ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
    if (!m_fake_stereo_hook->has_attempted_to_hook_engine() || !m_fake_stereo_hook->has_attempted_to_hook_slate()) {
        std::string adjusted_name = get_name().data();
        adjusted_name += " (Loading...)";

        /*if (!ImGui::CollapsingHeader(adjusted_name.data())) {
            ImGui::PopID();
            return;
        }*/

        ImGui::TextWrapped("Loading...");
    } else {
        /*if (!ImGui::CollapsingHeader(get_name().data())) {
            ImGui::PopID();
            return;
        }*/
    }
    ImGui::PopID();

    auto display_error = [](auto& runtime, std::string dll_name) {
        if (runtime == nullptr || !runtime->error && runtime->loaded) {
            return;
        }

        if (runtime->error && runtime->dll_missing) {
            ImGui::TextWrapped("%s not loaded: %s not found", runtime->name().data(), dll_name.data());
            ImGui::TextWrapped("Please select %s from the loader if you want to use %s", runtime->name().data(), runtime->name().data());
        } else if (runtime->error) {
            ImGui::TextWrapped("%s not loaded: %s", runtime->name().data(), runtime->error->c_str());
        } else {
            ImGui::TextWrapped("%s not loaded: Unknown error", runtime->name().data());
        }

        ImGui::Separator();
    };

    if (!get_runtime()->loaded || get_runtime()->error) {
        display_error(m_openxr, "openxr_loader.dll");
        display_error(m_openvr, "openvr_api.dll");
    }

    if (!get_runtime()->loaded) {
        ImGui::TextWrapped("No runtime loaded.");

        if (ImGui::Button("Attempt to reinitialize")) {
            clean_initialize();
        }

        return;
    }

    // These are HMD-runtime controls (tracked standing origin / recenter /
    // reinitialize) — meaningless in Flat3D (3D Display) mode, so hide the row.
    if (!is_using_flat3d()) {
        if (ImGui::Button("Set Standing Height")) {
            m_standing_origin.y = get_position(0).y;
        }

        ImGui::SameLine();

        if (ImGui::Button("Set Standing Origin")) {
            m_standing_origin = get_position(0);
        }

        ImGui::SameLine();

        if (ImGui::Button("Recenter View")) {
            recenter_view();
        }

        ImGui::SameLine();

        if (ImGui::Button("Recenter Horizon")) {
            recenter_horizon();
        }

        if (ImGui::Button("Reinitialize Runtime")) {
            get_runtime()->wants_reinitialize = true;
        }
    }
}

Vector4f VR::get_position(uint32_t index, bool grip) const {
    return get_transform(index, grip)[3];
}

Vector4f VR::get_velocity(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };

    return get_velocity_unsafe(index);
}

Vector4f VR::get_angular_velocity(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };

    return get_angular_velocity_unsafe(index);
}

Vector4f VR::get_position_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        if (index == vr::k_unTrackedDeviceIndex_Hmd) {
            const auto pose = m_openvr->get_current_hmd_pose();
            auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose };
            auto result = glm::rowMajor4(matrix)[3];
            result.w = 1.0f;

            return result;
        }

        if (index == get_left_controller_index()) {
            return m_openvr->grip_matrices[VRRuntime::Hand::LEFT][3];
        }

        if (index == get_right_controller_index()) {
            return m_openvr->grip_matrices[VRRuntime::Hand::RIGHT][3];
        }

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        auto result = glm::rowMajor4(matrix)[3];
        result.w = 1.0f;

        return result;
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // HMD position
        if (index == 0 && !m_openxr->stage_views.empty()) {
            const auto vspl = m_openxr->get_current_view_space_location();
            return Vector4f{ *(Vector3f*)&vspl.pose.position, 1.0f };
        } else if (index > 0) {
            if (index == get_left_controller_index()) {
                return m_openxr->grip_matrices[VRRuntime::Hand::LEFT][3];
            } else if (index == get_right_controller_index()) {
                return m_openxr->grip_matrices[VRRuntime::Hand::RIGHT][3];
            }
        }

        return Vector4f{};
    } 

    return Vector4f{};
}

Vector4f VR::get_velocity_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& pose = get_openvr_poses()[index];
        const auto& velocity = pose.vVelocity;

        return Vector4f{ velocity.v[0], velocity.v[1], velocity.v[2], 0.0f };
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // todo: implement HMD velocity
        if (index == 0) {
            return Vector4f{};
        }

        return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].grip_velocity.linearVelocity, 0.0f };
    }

    return Vector4f{};
}

Vector4f VR::get_angular_velocity_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& pose = get_openvr_poses()[index];
        const auto& angular_velocity = pose.vAngularVelocity;

        return Vector4f{ angular_velocity.v[0], angular_velocity.v[1], angular_velocity.v[2], 0.0f };
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // todo: implement HMD velocity
        if (index == 0) {
            return Vector4f{};
        }
    
        return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].grip_velocity.angularVelocity, 0.0f };
    }

    return Vector4f{};
}

Matrix4x4f VR::get_hmd_rotation(uint32_t frame_count) const {
    return glm::extractMatrixRotation(get_hmd_transform(frame_count));
}

Matrix4x4f VR::get_hmd_transform(uint32_t frame_count) const {
    ZoneScopedN(__FUNCTION__);

    if (get_runtime()->is_openvr()) {
        std::shared_lock _{ get_runtime()->pose_mtx };

        const auto pose = m_openvr->get_hmd_pose(frame_count);
        const auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose };
        return glm::rowMajor4(matrix);
    } else if (get_runtime()->is_openxr()) {
        std::shared_lock __{ get_runtime()->eyes_mtx };

        const auto vspl = m_openxr->get_view_space_location(frame_count);
        auto mat = Matrix4x4f{runtimes::OpenXR::to_glm(vspl.pose.orientation)};
        mat[3] = Vector4f{*(Vector3f*)&vspl.pose.position, 1.0f};

        return mat;
    }

    return glm::identity<Matrix4x4f>();
}

Matrix4x4f VR::get_rotation(uint32_t index, bool grip) const {
    return glm::extractMatrixRotation(get_transform(index, grip));
}

Matrix4x4f VR::get_transform(uint32_t index, bool grip) const {
    ZoneScopedN(__FUNCTION__);

    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return glm::identity<Matrix4x4f>();
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        if (index == vr::k_unTrackedDeviceIndex_Hmd) {
            const auto pose = m_openvr->get_current_hmd_pose();
            const auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose };
            return glm::rowMajor4(matrix);
        }

        if (index == get_left_controller_index()) {
            return grip ? m_openvr->grip_matrices[VRRuntime::Hand::LEFT] : m_openvr->aim_matrices[VRRuntime::Hand::LEFT];
        } else if (index == get_right_controller_index()) {
            return grip ? m_openvr->grip_matrices[VRRuntime::Hand::RIGHT] : m_openvr->aim_matrices[VRRuntime::Hand::RIGHT];
        }

        const auto& pose = get_openvr_poses()[index];
        const auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        return glm::rowMajor4(matrix);
    } else if (get_runtime()->is_openxr()) {
        // HMD rotation
        if (index == 0 && !m_openxr->stage_views.empty()) {
            const auto vspl = m_openxr->get_current_view_space_location();
            auto mat = Matrix4x4f{runtimes::OpenXR::to_glm(vspl.pose.orientation)};
            mat[3] = Vector4f{*(Vector3f*)&vspl.pose.position, 1.0f};
            return mat;
        } else if (index > 0) {
            if (index == get_left_controller_index()) {
                return grip ? m_openxr->grip_matrices[VRRuntime::Hand::LEFT] : m_openxr->aim_matrices[VRRuntime::Hand::LEFT];
            } else if (index == get_right_controller_index()) {
                return grip ? m_openxr->grip_matrices[VRRuntime::Hand::RIGHT] : m_openxr->aim_matrices[VRRuntime::Hand::RIGHT];
            }
        }
    }

    return glm::identity<Matrix4x4f>();
}

Matrix4x4f VR::get_grip_transform(uint32_t index) const {
    return get_transform(index);
}

Matrix4x4f VR::get_aim_transform(uint32_t index) const {
    return get_transform(index, false);
}

vr::HmdMatrix34_t VR::get_raw_transform(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return vr::HmdMatrix34_t{};
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        if (index == vr::k_unTrackedDeviceIndex_Hmd) {
            return m_openvr->get_current_hmd_pose();
        }

        auto& pose = get_openvr_poses()[index];
        return pose.mDeviceToAbsoluteTracking;
    } else {
        spdlog::error("VR: get_raw_transform: not implemented for {}", get_runtime()->name());
        return vr::HmdMatrix34_t{};
    }
}

Vector4f VR::get_eye_offset(VRRuntime::Eye eye) const {
    ZoneScopedN(__FUNCTION__);

    if (!is_hmd_active()) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->eyes_mtx };

    if (eye == VRRuntime::Eye::LEFT) {
        return get_runtime()->eyes[vr::Eye_Left][3];
    }
    
    return get_runtime()->eyes[vr::Eye_Right][3];
}

Vector4f VR::get_current_offset() {
    if (!is_hmd_active()) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->eyes_mtx };

    if (m_frame_count % 2 == m_left_eye_interval) {
        //return Vector4f{m_eye_distance * -1.0f, 0.0f, 0.0f, 0.0f};
        return get_runtime()->eyes[vr::Eye_Left][3];
    }
    
    return get_runtime()->eyes[vr::Eye_Right][3];
    //return Vector4f{m_eye_distance, 0.0f, 0.0f, 0.0f};
}

Matrix4x4f VR::get_eye_transform(uint32_t index) {
    ZoneScopedN(__FUNCTION__);

    if (!is_hmd_active() || index > 2) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    return get_runtime()->eyes[index];
}

Matrix4x4f VR::get_current_eye_transform(bool flip) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    auto mod_count = flip ? m_right_eye_interval : m_left_eye_interval;

    if (m_frame_count % 2 == mod_count) {
        return get_runtime()->eyes[vr::Eye_Left];
    }

    return get_runtime()->eyes[vr::Eye_Right];
}

Matrix4x4f VR::get_projection_matrix(VRRuntime::Eye eye, bool flip) {
    ZoneScopedN(__FUNCTION__);

    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    if ((eye == VRRuntime::Eye::LEFT && !flip) || (eye == VRRuntime::Eye::RIGHT && flip)) {
        return get_runtime()->projections[(uint32_t)VRRuntime::Eye::LEFT];
    }

    return get_runtime()->projections[(uint32_t)VRRuntime::Eye::RIGHT];
}

Matrix4x4f VR::get_current_projection_matrix(bool flip) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    auto mod_count = flip ? m_right_eye_interval : m_left_eye_interval;

    if (m_frame_count % 2 == mod_count) {
        return get_runtime()->projections[(uint32_t)VRRuntime::Eye::LEFT];
    }

    return get_runtime()->projections[(uint32_t)VRRuntime::Eye::RIGHT];
}

bool VR::is_action_active(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source) const {
    ZoneScopedN(__FUNCTION__);

    if (!get_runtime()->loaded) {
        return false;
    }

    if (action == vr::k_ulInvalidActionHandle) {
        return false;
    }
    
    bool active = false;

    if (get_runtime()->is_openvr()) {
        vr::InputDigitalActionData_t data{};
        vr::VRInput()->GetDigitalActionData(action, &data, sizeof(data), source);

        active = data.bActive && data.bState;
    } else if (get_runtime()->is_openxr()) {
        active = m_openxr->is_action_active((XrAction)action, (VRRuntime::Hand)source);
    }

    return active;
}

Vector2f VR::get_joystick_axis(vr::VRInputValueHandle_t handle) const {
    ZoneScopedN(__FUNCTION__);

    if (!get_runtime()->loaded) {
        return Vector2f{};
    }

    if (get_runtime()->is_openvr()) {
        vr::InputAnalogActionData_t data{};
        vr::VRInput()->GetAnalogActionData(m_action_joystick, &data, sizeof(data), handle);

        const auto deadzone = m_joystick_deadzone->value();
        auto out = Vector2f{ data.x, data.y };

        //return glm::length(out) > deadzone ? out : Vector2f{};
        if (glm::abs(out.x) < deadzone) {
            out.x = 0.0f;
        }

        if (glm::abs(out.y) < deadzone) {
            out.y = 0.0f;
        }

        return out;
    } else if (get_runtime()->is_openxr()) {
        // Not using get_left/right_joystick here because it flips the controllers
        if (handle == m_left_joystick) {
            auto out = m_openxr->get_left_stick_axis();
            //return glm::length(out) > m_joystick_deadzone->value() ? out : Vector2f{};
            // okay.. instead of that actually clamp x/y to the proper deadzone
            if (glm::abs(out.x) < m_joystick_deadzone->value()) {
                out.x = 0.0f;
            }

            if (glm::abs(out.y) < m_joystick_deadzone->value()) {
                out.y = 0.0f;
            }

            return out;
        } else if (handle == m_right_joystick) {
            auto out = m_openxr->get_right_stick_axis();
            //return glm::length(out) > m_joystick_deadzone->value() ? out : Vector2f{};

            if (glm::abs(out.x) < m_joystick_deadzone->value()) {
                out.x = 0.0f;
            }

            if (glm::abs(out.y) < m_joystick_deadzone->value()) {
                out.y = 0.0f;
            }

            return out;
        }
    }

    return Vector2f{};
}

Vector2f VR::get_left_stick_axis() const {
    return get_joystick_axis(get_left_joystick());
}

Vector2f VR::get_right_stick_axis() const {
    return get_joystick_axis(get_right_joystick());
}

void VR::trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, vr::VRInputValueHandle_t source) {
    ZoneScopedN(__FUNCTION__);

    if (!get_runtime()->loaded || !is_using_controllers()) {
        return;
    }

    if (get_runtime()->is_openvr()) {
        vr::VRInput()->TriggerHapticVibrationAction(m_action_haptic, seconds_from_now, duration, frequency, amplitude, source);
    } else if (get_runtime()->is_openxr()) {
        m_openxr->trigger_haptic_vibration(duration, frequency, amplitude, (VRRuntime::Hand)source);
    }
}

float VR::get_standing_height() {
    ZoneScopedN(__FUNCTION__);

    std::shared_lock _{ get_runtime()->pose_mtx };

    return m_standing_origin.y;
}

Vector4f VR::get_standing_origin() {
    ZoneScopedN(__FUNCTION__);

    std::shared_lock _{ get_runtime()->pose_mtx };

    return m_standing_origin;
}

void VR::set_standing_origin(const Vector4f& origin) {
    ZoneScopedN(__FUNCTION__);

    std::unique_lock _{ get_runtime()->pose_mtx };
    
    m_standing_origin = origin;
}

glm::quat VR::get_rotation_offset() {
    ZoneScopedN(__FUNCTION__);

    std::shared_lock _{ m_rotation_mtx };

    return m_rotation_offset;
}

void VR::set_rotation_offset(const glm::quat& offset) {
    ZoneScopedN(__FUNCTION__);

    std::unique_lock _{ m_rotation_mtx };

    m_rotation_offset = offset;
}

void VR::recenter_view() {
    ZoneScopedN(__FUNCTION__);

    const auto new_rotation_offset = glm::normalize(glm::inverse(utility::math::flatten(glm::quat{get_rotation(0)})));

    set_rotation_offset(new_rotation_offset);
}

void VR::recenter_horizon() {
    ZoneScopedN(__FUNCTION__);

    const auto new_rotation_offset = glm::normalize(glm::inverse(glm::quat{get_rotation(0)}));

    set_rotation_offset(new_rotation_offset);
}

void VR::gamepad_snapturn(XINPUT_STATE& state) {
    if (!m_snapturn->value()) {
        return;
    }

    if (!is_hmd_active()) {
        return;
    }

    const auto stick_axis = (float)state.Gamepad.sThumbRX / (float)std::numeric_limits<SHORT>::max();

    if (!m_was_snapturn_run_on_input) {
        if (glm::abs(stick_axis) > m_snapturn_joystick_deadzone->value()) {
            m_snapturn_left = stick_axis < 0.0f;
            m_snapturn_on_frame = true;
            m_was_snapturn_run_on_input = true;
            state.Gamepad.sThumbRX = 0;
        }
    } else {
        if (glm::abs(stick_axis) < m_snapturn_joystick_deadzone->value()) {
            m_was_snapturn_run_on_input = false;
        } else {
            state.Gamepad.sThumbRX = 0;
        }
    }
}

void VR::process_snapturn() {
    if (!m_snapturn_on_frame) {
        return;
    }

    const auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        return;
    }

    const auto world = engine->get_world();

    if (world == nullptr) {
        return;
    }

    if (const auto controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0); controller != nullptr) {
        auto controller_rot = controller->get_control_rotation();
        auto turn_degrees = get_snapturn_angle();
        
        if (m_snapturn_left) {
            turn_degrees = -turn_degrees;
            m_snapturn_left = false;
        }

        controller_rot.y += turn_degrees;
        controller->set_control_rotation(controller_rot);
    }
        
    m_snapturn_on_frame = false;
}

void VR::update_statistics_overlay(sdk::UGameEngine* engine) {
    if (engine == nullptr) {
        return;
    }
    
    if (m_show_fps_state != m_show_fps->value()) {
        engine->exec(L"stat fps");
        m_show_fps_state = m_show_fps->value();
    }
    
    if (m_show_statistics_state != m_show_statistics->value()) {
        engine->exec(L"stat unit");
        m_show_statistics_state = m_show_statistics->value();
    }
}
