#include <d3dcompiler.h>

#include <openvr.h>
#include <utility/String.hpp>
#include <utility/ScopeGuard.hpp>
#include <utility/Logging.hpp>

#include "Framework.hpp"
#include "../VR.hpp"

#include <../../directxtk12-src/Inc/ResourceUploadBatch.h>
#include <../../directxtk12-src/Inc/RenderTargetState.h>

#include "shaders/Compiled/alpha_luminance_sprite_ps_SpritePixelShader.inc"
#include "shaders/Compiled/alpha_luminance_sprite_ps_SpriteVertexShader.inc"

#include "d3d12/DirectXTK.hpp"

#include "D3D12Component.hpp"

//#define AFR_DEPTH_TEMP_DISABLED

constexpr auto ENGINE_SRC_DEPTH = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr auto ENGINE_SRC_COLOR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

namespace vrmod {
namespace {
constexpr auto FRAME_TIMING_LOG_INTERVAL = std::chrono::seconds(5);
constexpr bool SHF_AUTO_MONO_CINEMATIC = true;
constexpr bool SHF_AUTO_2D_SCREEN_FROM_MONO_CINEMATIC = true;

enum SwapchainRecreateReason : uint32_t {
    SWAPCHAIN_RECREATE_NONE = 0,
    SWAPCHAIN_RECREATE_HMD_RESOLUTION = 1 << 0,
    SWAPCHAIN_RECREATE_EMPTY = 1 << 1,
    SWAPCHAIN_RECREATE_UI_EXTENT = 1 << 2,
    SWAPCHAIN_RECREATE_AFR_STATE = 1 << 3,
    SWAPCHAIN_RECREATE_DEPTH_EXTENT = 1 << 4,
    SWAPCHAIN_RECREATE_DEPTH_NULL_DEFAULTS = 1 << 5,
    SWAPCHAIN_RECREATE_SCENE_TARGET_READY = 1 << 6,
};

std::string format_swapchain_recreate_reasons(uint32_t reasons) {
    if (reasons == SWAPCHAIN_RECREATE_NONE) {
        return "none";
    }

    std::string out{};
    const auto append = [&](uint32_t flag, const char* name) {
        if ((reasons & flag) == 0) {
            return;
        }

        if (!out.empty()) {
            out += "|";
        }

        out += name;
    };

    append(SWAPCHAIN_RECREATE_HMD_RESOLUTION, "hmd_resolution");
    append(SWAPCHAIN_RECREATE_EMPTY, "empty_swapchains");
    append(SWAPCHAIN_RECREATE_UI_EXTENT, "ui_extent");
    append(SWAPCHAIN_RECREATE_AFR_STATE, "afr_state");
    append(SWAPCHAIN_RECREATE_DEPTH_EXTENT, "depth_extent");
    append(SWAPCHAIN_RECREATE_DEPTH_NULL_DEFAULTS, "depth_null_defaults");
    append(SWAPCHAIN_RECREATE_SCENE_TARGET_READY, "scene_target_ready");
    return out;
}

uint8_t depth_format_family(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        return 1;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return 2;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        return 3;
    default:
        return 0;
    }
}

bool copy_resource_depth_descriptors_compatible(
    const D3D12_RESOURCE_DESC& src,
    const D3D12_RESOURCE_DESC& dst)
{
    const auto src_family = depth_format_family(src.Format);
    const auto dst_family = depth_format_family(dst.Format);

    return src.Dimension == dst.Dimension &&
        src.Width == dst.Width &&
        src.Height == dst.Height &&
        src.DepthOrArraySize == dst.DepthOrArraySize &&
        src.MipLevels == dst.MipLevels &&
        src.SampleDesc.Count == dst.SampleDesc.Count &&
        src.SampleDesc.Quality == dst.SampleDesc.Quality &&
        src_family != 0 &&
        src_family == dst_family;
}
bool is_ue58_runtime_cached() {
    static const bool is_ue58 = []() {
        const auto file_version = sdk::get_file_version_info();

        if (file_version.dwFileVersionMS == 0x00050008) {
            return true;
        }

        const auto embedded_version =
            utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));
        return embedded_version.starts_with("5.8");
    }();

    return is_ue58;
}

void prepare_openxr_swapchain_recreate(VR* vr, uint32_t reasons) {
    const auto cadence_sensitive_recreate =
        (reasons & (SWAPCHAIN_RECREATE_AFR_STATE |
                    SWAPCHAIN_RECREATE_DEPTH_EXTENT |
                    SWAPCHAIN_RECREATE_DEPTH_NULL_DEFAULTS |
                    SWAPCHAIN_RECREATE_SCENE_TARGET_READY)) != 0;

    if (!cadence_sensitive_recreate) {
        return;
    }

    if (vr == nullptr || vr->get_runtime() == nullptr || !vr->get_runtime()->is_openxr()) {
        return;
    }

    const auto openxr = vr->get_openxr_runtime();

    if (openxr == nullptr) {
        return;
    }

    const auto reason_text = "d3d12_swapchain_recreate:" + format_swapchain_recreate_reasons(reasons);
    openxr->prepare_resolution_scale_reconfigure(reason_text.c_str());
}

std::pair<uint32_t, uint32_t> get_ui_extent() {
    const auto fallback = std::pair<uint32_t, uint32_t>{
        (uint32_t)g_framework->get_d3d12_rt_size().x,
        (uint32_t)g_framework->get_d3d12_rt_size().y
    };

    const auto vr = VR::get();

    if (vr == nullptr) {
        return fallback;
    }

    const auto& fake_stereo_hook = vr->get_fake_stereo_hook();

    if (fake_stereo_hook == nullptr) {
        return fallback;
    }

    const auto rtm = fake_stereo_hook->get_render_target_manager();

    if (rtm == nullptr) {
        return fallback;
    }

    if (const auto requested_width = rtm->get_dedicated_ui_width();
        requested_width > 0 && rtm->get_dedicated_ui_height() > 0)
    {
        return {requested_width, rtm->get_dedicated_ui_height()};
    }

    const auto ui_target = rtm->get_ui_target();

    if (ui_target == nullptr || !g_framework->is_dx12()) {
        return fallback;
    }

    const auto native = (ID3D12Resource*)ui_target->get_native_resource();

    if (native == nullptr) {
        return fallback;
    }

    const auto desc = native->GetDesc();

    if (desc.Width == 0 || desc.Height == 0) {
        return fallback;
    }

    return {(uint32_t)desc.Width, (uint32_t)desc.Height};
}

bool is_shf_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"SHf-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool is_deadzone_rogue_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"DeadzoneSteam-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool is_dead_island_2_ue425_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());

        if (!exe_path) {
            return false;
        }

        const auto lowered = uevr::games::lowercase_path(*exe_path);
        const bool matching_executable =
            lowered.ends_with(L"\\deadisland-win64-shipping.exe") ||
            lowered.ends_with(L"/deadisland-win64-shipping.exe") ||
            lowered == L"deadisland-win64-shipping.exe";

        if (!matching_executable) {
            return false;
        }

        const auto version = sdk::get_file_version_info();
        return HIWORD(version.dwFileVersionMS) == 4 &&
            LOWORD(version.dwFileVersionMS) == 25;
    }();

    return result;
}

bool should_disable_dead_island_2_afr_depth(VR* vr) {
    return vr != nullptr &&
        is_dead_island_2_ue425_current_game() &&
        vr->is_using_afr();
}

bool is_everspace2_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && uevr::games::is_everspace2_executable_path(*exe_path);
    }();

    return result;
}

Microsoft::WRL::ComPtr<ID3D12Resource> acquire_scene_target_resource(
    VR* vr,
    const char* consumer,
    bool* from_everspace2_snapshot = nullptr)
{
    if (from_everspace2_snapshot != nullptr) {
        *from_everspace2_snapshot = false;
    }

    if (vr == nullptr) {
        return nullptr;
    }

    const auto& fake_stereo_hook = vr->get_fake_stereo_hook();
    if (fake_stereo_hook == nullptr) {
        return nullptr;
    }

    const auto rtm = fake_stereo_hook->get_render_target_manager();
    if (rtm == nullptr) {
        return nullptr;
    }

    if (is_everspace2_current_game() && g_framework->is_dx12()) {
        const auto snapshot = rtm->get_everspace2_scene_target_snapshot();
        if (snapshot == nullptr || snapshot->resource == nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(
                1,
                "[Everspace2][SceneTargetSnapshot] {} waiting for a valid native scene target",
                consumer != nullptr ? consumer : "<unknown>");
            return nullptr;
        }

        if (from_everspace2_snapshot != nullptr) {
            *from_everspace2_snapshot = true;
        }

        SPDLOG_INFO_EVERY_N_SEC(
            5,
            "[Everspace2][SceneTargetSnapshot] {} consuming generation={} frhi={:x} native={:x} size={}x{}",
            consumer != nullptr ? consumer : "<unknown>",
            snapshot->generation,
            snapshot->source_texture,
            (uintptr_t)snapshot->resource.Get(),
            snapshot->desc.Width,
            snapshot->desc.Height);
        return snapshot->resource;
    }

    const auto ue4_texture = rtm->get_render_target();
    if (ue4_texture == nullptr) {
        return nullptr;
    }

    return (ID3D12Resource*)ue4_texture->get_native_resource();
}

bool is_stalker2_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"Stalker2-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool is_avowed_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && uevr::games::is_avowed_executable_path(*exe_path);
    }();

    return result;
}

bool is_dune_awakening_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && uevr::games::is_dune_awakening_executable_path(*exe_path);
    }();

    return result;
}

std::array<uintptr_t, 2> g_dune_null_residency_reference{};
std::atomic_uint64_t g_dune_null_residency_reference_count{};
safetyhook::MidHook g_dune_descriptor_cache_null_guard{};

void dune_descriptor_cache_null_guard(safetyhook::Context& ctx) {
    if (ctx.rdx != 0) {
        return;
    }

    // Dune added residency-reference tracking after OMSetRenderTargets, but
    // unlike stock UE5.2 it dereferences optional null RTV/DSV references.
    // Preserve the null semantics by supplying readable zero storage only for
    // that tracking check; the actual render-target binding already happened.
    ctx.rdx = reinterpret_cast<uintptr_t>(g_dune_null_residency_reference.data());

    const auto count = g_dune_null_residency_reference_count.fetch_add(1) + 1;
    if (count == 1) {
        SPDLOG_WARN("[Dune][D3D12] Guarded the first null SetRenderTargets residency reference");
    } else {
        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[Dune][D3D12] Guarded null SetRenderTargets residency references count={}",
            count);
    }
}

void apply_dune_descriptor_cache_guard() {
    if (!is_dune_awakening_current_game()) {
        return;
    }

    // Dune's custom UE5.2 residency-reference loop lacks the null check present
    // in the surrounding render-target logic. Guard only that dereference,
    // leaving residency tracking enabled for every valid offscreen target.
    constexpr uintptr_t DUNE_DESCRIPTOR_TRACKING_LOAD_RVA = 0x5516c47;
    constexpr uintptr_t DUNE_NULL_REFERENCE_DEREFERENCE_RVA = 0x5516c5d;
    constexpr uintptr_t DUNE_DESCRIPTOR_TRACKING_GUARD_RVA = 0xb4fc7b4;
    constexpr std::array<uint8_t, 29> EXPECTED_DESCRIPTOR_TRACKING_BYTES{
        0x0f, 0xb6, 0x0d, 0x66, 0x5b, 0xfe, 0x05,
        0x48, 0x89, 0x7c, 0x24, 0x20,
        0x48, 0x8b, 0x13,
        0x48, 0x8b, 0xf8,
        0x84, 0xc9,
        0x74, 0x1e,
        0x48, 0x83, 0x7a, 0x08, 0x00,
        0x74, 0x17,
    };

    static bool s_attempted = false;
    if (s_attempted) {
        return;
    }
    s_attempted = true;

    const auto module = utility::get_executable();
    const auto module_base = reinterpret_cast<uintptr_t>(module);
    const auto module_size = utility::get_module_size(module).value_or(0);

    if (module_base == 0 || module_size <= DUNE_DESCRIPTOR_TRACKING_GUARD_RVA) {
        SPDLOG_WARN(
            "[Dune][D3D12] Descriptor-cache guard skipped because executable image is smaller than expected base={:x} size=0x{:x}",
            module_base,
            module_size);
        return;
    }

    const auto signature_address = reinterpret_cast<const uint8_t*>(module_base + DUNE_DESCRIPTOR_TRACKING_LOAD_RVA);
    if (std::memcmp(signature_address, EXPECTED_DESCRIPTOR_TRACKING_BYTES.data(), EXPECTED_DESCRIPTOR_TRACKING_BYTES.size()) != 0) {
        SPDLOG_WARN(
            "[Dune][D3D12] Descriptor-cache null guard signature mismatch at {:x}; leaving Dune D3D12 code untouched",
            module_base + DUNE_DESCRIPTOR_TRACKING_LOAD_RVA);
        return;
    }

    auto* const guard_byte = reinterpret_cast<uint8_t*>(module_base + DUNE_DESCRIPTOR_TRACKING_GUARD_RVA);
    const auto hook_address = module_base + DUNE_NULL_REFERENCE_DEREFERENCE_RVA;
    auto hook_result = safetyhook::create_mid(
        reinterpret_cast<void*>(hook_address),
        &dune_descriptor_cache_null_guard);
    if (!hook_result) {
        SPDLOG_ERROR(
            "[Dune][D3D12] Failed to install narrow SetRenderTargets null guard at {:x}; retaining disabled residency tracking fallback",
            hook_address);

        DWORD old_protect{};
        if (VirtualProtect(guard_byte, sizeof(*guard_byte), PAGE_READWRITE, &old_protect)) {
            *guard_byte = 0;
            DWORD ignored{};
            VirtualProtect(guard_byte, sizeof(*guard_byte), old_protect, &ignored);
        }
        return;
    }

    g_dune_descriptor_cache_null_guard = std::move(hook_result);

    DWORD old_protect{};
    if (!VirtualProtect(guard_byte, sizeof(*guard_byte), PAGE_READWRITE, &old_protect)) {
        SPDLOG_ERROR(
            "[Dune][D3D12] Narrow null guard installed, but descriptor tracking could not be restored at {:x}; last_error={}",
            reinterpret_cast<uintptr_t>(guard_byte),
            GetLastError());
        return;
    }

    *guard_byte = 1;

    DWORD ignored{};
    VirtualProtect(guard_byte, sizeof(*guard_byte), old_protect, &ignored);

    SPDLOG_WARN(
        "[Dune][D3D12] Installed narrow SetRenderTargets null guard at {:x}; restored valid descriptor residency tracking byte {:x}",
        hook_address,
        reinterpret_cast<uintptr_t>(guard_byte));
}

bool is_ue_5_1_dx12_backend() {
    if (g_framework == nullptr || !g_framework->is_dx12()) {
        return false;
    }

    static const bool result = []() {
        const auto found_version = sdk::search_for_version(utility::get_executable());

        if (found_version) {
            const auto version = utility::narrow(*found_version);
            return version == "5.1" || version.starts_with("5.1.");
        }

        const auto disk_version = sdk::get_file_version_info();
        return disk_version.dwFileVersionMS == 0x00050001;
    }();

    return result;
}

bool texture_context_has_views(const d3d12::TextureContext& context) {
    return context.texture.Get() != nullptr &&
        context.rtv_heap != nullptr &&
        context.rtv_heap->Heap() != nullptr &&
        context.srv_heap != nullptr &&
        context.srv_heap->Heap() != nullptr;
}

void log_shf_texture_reference_rebuild(
    ID3D12Resource* backbuffer,
    ID3D12Resource* real_backbuffer,
    ID3D12Resource* current_game_texture,
    uint64_t frame_count)
{
    if (!is_shf_current_game() || backbuffer == nullptr) {
        return;
    }

    const auto backbuffer_desc = backbuffer->GetDesc();
    const auto real_desc = real_backbuffer != nullptr ? std::optional<D3D12_RESOURCE_DESC>{real_backbuffer->GetDesc()} : std::nullopt;
    static std::mutex log_mutex{};
    static std::unordered_set<uintptr_t> logged_backbuffers{};
    static uint64_t rebuild_count{};
    static uint64_t duplicate_suppressed{};

    bool log_unique = false;
    uint64_t seen = 0;
    uint64_t unique = 0;
    uint64_t suppressed = 0;

    {
        std::scoped_lock _{log_mutex};
        ++rebuild_count;
        seen = rebuild_count;

        const auto key = (uintptr_t)backbuffer;

        if (!logged_backbuffers.contains(key)) {
            logged_backbuffers.insert(key);
            log_unique = logged_backbuffers.size() <= 64;
        } else {
            ++duplicate_suppressed;
        }

        unique = logged_backbuffers.size();
        suppressed = duplicate_suppressed;
    }

    if (log_unique && real_desc) {
        SPDLOG_WARN("[SHf][D3D12] Game Texture reference rebuild #{} frame={} unique_backbuffers={} backbuffer={:x} real_backbuffer={:x} current_game_texture={:x} bb=[{}x{} fmt={} flags=0x{:x}] real=[{}x{} fmt={} flags=0x{:x}]",
            seen, frame_count, unique, (uintptr_t)backbuffer, (uintptr_t)real_backbuffer, (uintptr_t)current_game_texture,
            backbuffer_desc.Width, backbuffer_desc.Height, (uint32_t)backbuffer_desc.Format, (uint32_t)backbuffer_desc.Flags,
            real_desc->Width, real_desc->Height, (uint32_t)real_desc->Format, (uint32_t)real_desc->Flags);
    } else if (log_unique) {
        SPDLOG_WARN("[SHf][D3D12] Game Texture reference rebuild #{} frame={} unique_backbuffers={} backbuffer={:x} real_backbuffer=<null> current_game_texture={:x} bb=[{}x{} fmt={} flags=0x{:x}]",
            seen, frame_count, unique, (uintptr_t)backbuffer, (uintptr_t)current_game_texture,
            backbuffer_desc.Width, backbuffer_desc.Height, (uint32_t)backbuffer_desc.Format, (uint32_t)backbuffer_desc.Flags);
    } else if (real_desc) {
        SPDLOG_INFO_EVERY_N_SEC(2,
            "[SHf][D3D12] Game Texture reference rebuild summary seen={} unique_backbuffers={} duplicate_suppressed={} frame={} backbuffer={:x} real_backbuffer={:x} current_game_texture={:x} bb=[{}x{} fmt={} flags=0x{:x}] real=[{}x{} fmt={} flags=0x{:x}]",
            seen, unique, suppressed, frame_count, (uintptr_t)backbuffer, (uintptr_t)real_backbuffer, (uintptr_t)current_game_texture,
            backbuffer_desc.Width, backbuffer_desc.Height, (uint32_t)backbuffer_desc.Format, (uint32_t)backbuffer_desc.Flags,
            real_desc->Width, real_desc->Height, (uint32_t)real_desc->Format, (uint32_t)real_desc->Flags);
    } else {
        SPDLOG_INFO_EVERY_N_SEC(2,
            "[SHf][D3D12] Game Texture reference rebuild summary seen={} unique_backbuffers={} duplicate_suppressed={} frame={} backbuffer={:x} real_backbuffer=<null> current_game_texture={:x} bb=[{}x{} fmt={} flags=0x{:x}]",
            seen, unique, suppressed, frame_count, (uintptr_t)backbuffer, (uintptr_t)current_game_texture,
            backbuffer_desc.Width, backbuffer_desc.Height, (uint32_t)backbuffer_desc.Format, (uint32_t)backbuffer_desc.Flags);
    }
}

bool shf_texture_desc_matches(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
    return a.Dimension == b.Dimension &&
           a.Alignment == b.Alignment &&
           a.Width == b.Width &&
           a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize &&
           a.MipLevels == b.MipLevels &&
           a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality;
}

std::optional<DXGI_FORMAT> dune_view_format_for_resource(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return std::nullopt;
    }
}

std::optional<DXGI_FORMAT> concrete_color_view_format_for_resource(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return format;
    default:
        return std::nullopt;
    }
}

bool can_copy_to_openxr_ui_swapchain(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return true;
    default:
        return false;
    }
}

}

const char* D3D12Component::shf_scene_mode_name(ShfSceneMode mode) {
    switch (mode) {
    case ShfSceneMode::Stereo3D:
        return "Stereo3D";
    case ShfSceneMode::Mono2D:
        return "Mono2D";
    default:
        return "Unknown";
    }
}

bool D3D12Component::ensure_2d_screen_textures(ID3D12Device* device, const D3D12_RESOURCE_DESC& base_desc) {
    if (device == nullptr) {
        return false;
    }

    auto screen_desc = base_desc;
    screen_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    screen_desc.Alignment = 0;
    screen_desc.Width = (uint32_t)g_framework->get_d3d12_rt_size().x;
    screen_desc.Height = (uint32_t)g_framework->get_d3d12_rt_size().y;
    screen_desc.DepthOrArraySize = 1;
    screen_desc.MipLevels = 1;
    screen_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    screen_desc.SampleDesc.Count = 1;
    screen_desc.SampleDesc.Quality = 0;
    screen_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    screen_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    screen_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    if (screen_desc.Width == 0 || screen_desc.Height == 0) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Refusing to create zero-sized 2D screen textures (D3D12).");
        return false;
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    bool all_ready = true;

    for (auto& context : m_2d_screen_tex) {
        bool needs_create = context.texture.Get() == nullptr;

        if (!needs_create) {
            const auto existing_desc = context.texture->GetDesc();
            needs_create =
                existing_desc.Width != screen_desc.Width ||
                existing_desc.Height != screen_desc.Height ||
                existing_desc.Format != screen_desc.Format ||
                existing_desc.SampleDesc.Count != screen_desc.SampleDesc.Count ||
                existing_desc.SampleDesc.Quality != screen_desc.SampleDesc.Quality;
        }

        if (!needs_create) {
            continue;
        }

        context.reset();

        ComPtr<ID3D12Resource> screen_tex{};
        if (FAILED(device->CreateCommittedResource(
                &heap_props,
                D3D12_HEAP_FLAG_NONE,
                &screen_desc,
                ENGINE_SRC_COLOR,
                nullptr,
                IID_PPV_ARGS(&screen_tex)))) {
            spdlog::error("[VR] Failed to create 2D screen texture.");
            all_ready = false;
            continue;
        }

        screen_tex->SetName(L"2D Screen Texture");

        if (!context.setup(device, screen_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"2D Screen")) {
            spdlog::error("[VR] Failed to setup 2D screen context.");
            context.reset();
            all_ready = false;
            continue;
        }

        SPDLOG_INFO("[VR] Created D3D12 2D screen texture [{}x{} fmt={}]", screen_desc.Width, screen_desc.Height, (uint32_t)screen_desc.Format);
    }

    return all_ready;
}

bool D3D12Component::ensure_halo_electra_quad_source_texture(ID3D12Device* device, uint64_t width, uint32_t height) {
    if (device == nullptr || width == 0 || height == 0 ||
        width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
    {
        SPDLOG_ERROR_EVERY_N_SEC(
            1,
            "[Halo][D3D12] Refusing invalid cinematic staging extent [{}x{}]",
            width,
            height);
        return false;
    }

    D3D12_RESOURCE_DESC staging_desc{};
    staging_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    staging_desc.Width = width;
    staging_desc.Height = height;
    staging_desc.DepthOrArraySize = 1;
    staging_desc.MipLevels = 1;
    // Keep the resource typeless so the copy exactly matches Halo's Electra
    // target while exposing typed BGRA views to SpriteBatch.
    staging_desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    staging_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    if (m_halo_electra_quad_source_tex.texture != nullptr) {
        const auto existing_desc = m_halo_electra_quad_source_tex.texture->GetDesc();
        if (existing_desc.Width == staging_desc.Width &&
            existing_desc.Height == staging_desc.Height &&
            existing_desc.Format == staging_desc.Format &&
            existing_desc.SampleDesc.Count == 1 &&
            m_halo_electra_quad_source_tex.rtv_heap != nullptr &&
            m_halo_electra_quad_source_tex.srv_heap != nullptr)
        {
            return true;
        }
    }

    if (m_halo_electra_quad_source_tex.commands.ready()) {
        m_halo_electra_quad_source_tex.commands.wait(INFINITE);
    }
    m_halo_electra_quad_source_tex.reset();

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> staging_texture{};
    const auto create_result = device->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_NONE,
        &staging_desc,
        ENGINE_SRC_COLOR,
        nullptr,
        IID_PPV_ARGS(&staging_texture));
    if (FAILED(create_result)) {
        SPDLOG_ERROR(
            "[Halo][D3D12] Failed to create owned cinematic staging texture hr=0x{:08x} [{}x{} fmt={}]",
            (uint32_t)create_result,
            staging_desc.Width,
            staging_desc.Height,
            (uint32_t)staging_desc.Format);
        return false;
    }

    if (!m_halo_electra_quad_source_tex.setup(
            device,
            staging_texture.Get(),
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            L"Halo Electra Quad Source"))
    {
        SPDLOG_ERROR("[Halo][D3D12] Failed to create typed views for the owned cinematic staging texture");
        m_halo_electra_quad_source_tex.reset();
        return false;
    }

    SPDLOG_INFO(
        "[Halo][D3D12] Created owned cinematic staging texture [{}x{} resource_fmt={} view_fmt={}]",
        staging_desc.Width,
        staging_desc.Height,
        (uint32_t)staging_desc.Format,
        (uint32_t)DXGI_FORMAT_B8G8R8A8_UNORM);
    return true;
}

bool D3D12Component::ensure_ui_invert_tex(ID3D12Device* device, const D3D12_RESOURCE_DESC& base_desc) {
    if (device == nullptr || base_desc.Width == 0 || base_desc.Height == 0) {
        return false;
    }

    auto desc = base_desc;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // Match how the UI is VIEWED (m_game_ui_tex is set up as B8G8R8A8_UNORM)
    // and how the alpha-invert PSO is built. The underlying native UI resource
    // may be a different/typeless format; using its raw format here would both
    // break RTV creation and mismatch the PSO's render-target format (which is
    // what removed the device on FF7 Rebirth's 10-bit backbuffer).
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    if (m_ui_invert_tex.texture.Get() != nullptr) {
        const auto existing = m_ui_invert_tex.texture->GetDesc();
        if (existing.Width == desc.Width && existing.Height == desc.Height && existing.Format == desc.Format) {
            return true; // already matches
        }
        m_ui_invert_tex.reset();
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    ComPtr<ID3D12Resource> tex{};
    if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
            ENGINE_SRC_COLOR, nullptr, IID_PPV_ARGS(&tex)))) {
        spdlog::error("[VR] Failed to create UI alpha-invert texture.");
        return false;
    }

    tex->SetName(L"UI Alpha Invert Texture");

    if (!m_ui_invert_tex.setup(device, tex.Get(), desc.Format, desc.Format, L"UI Alpha Invert")) {
        spdlog::error("[VR] Failed to setup UI alpha-invert context.");
        m_ui_invert_tex.reset();
        return false;
    }

    SPDLOG_INFO("[VR] Created D3D12 UI alpha-invert texture [{}x{} fmt={}]", (uint32_t)desc.Width, desc.Height, (uint32_t)desc.Format);
    return true;
}

bool D3D12Component::ensure_ue58_spectator_texture(ID3D12Device* device, ID3D12Resource* source) {
    if (device == nullptr || source == nullptr) {
        return false;
    }

    const auto source_desc = source->GetDesc();
    if (source_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        source_desc.Width < 2 ||
        source_desc.Height == 0 ||
        source_desc.SampleDesc.Count != 1)
    {
        SPDLOG_WARNING_EVERY_N_SEC(
            2,
            "[UE5.8][spectator] Cannot stage unsupported scene resource dim={} extent={}x{} samples={}",
            (uint32_t)source_desc.Dimension,
            source_desc.Width,
            source_desc.Height,
            source_desc.SampleDesc.Count);
        return false;
    }

    auto spectator_desc = source_desc;
    spectator_desc.Width /= 2;
    spectator_desc.DepthOrArraySize = 1;
    spectator_desc.MipLevels = 1;
    spectator_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    spectator_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    if (m_ue58_spectator_tex.texture != nullptr &&
        shf_texture_desc_matches(m_ue58_spectator_tex.texture->GetDesc(), spectator_desc) &&
        texture_context_has_views(m_ue58_spectator_tex) &&
        m_ue58_spectator_tex.commands.ready())
    {
        return true;
    }

    if (m_ue58_spectator_tex.commands.ready()) {
        m_ue58_spectator_tex.commands.wait(INFINITE);
    }
    m_ue58_spectator_tex.reset();

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> spectator_texture{};
    const auto create_result = device->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_NONE,
        &spectator_desc,
        ENGINE_SRC_COLOR,
        nullptr,
        IID_PPV_ARGS(&spectator_texture));
    if (FAILED(create_result)) {
        SPDLOG_ERROR(
            "[UE5.8][spectator] Failed to create owned eye texture hr=0x{:08x} [{}x{} fmt={} flags=0x{:x}]",
            (uint32_t)create_result,
            spectator_desc.Width,
            spectator_desc.Height,
            (uint32_t)spectator_desc.Format,
            (uint32_t)spectator_desc.Flags);
        return false;
    }

    auto view_format = dune_view_format_for_resource(spectator_desc.Format);
    if (!view_format && spectator_desc.Format != DXGI_FORMAT_UNKNOWN) {
        view_format = spectator_desc.Format;
    }

    if (!m_ue58_spectator_tex.setup(
            device,
            spectator_texture.Get(),
            view_format,
            view_format,
            L"UE5.8 Spectator Eye Copy"))
    {
        SPDLOG_ERROR("[UE5.8][spectator] Failed to set up owned eye texture views");
        m_ue58_spectator_tex.reset();
        return false;
    }

    SPDLOG_INFO(
        "[UE5.8][spectator] Created owned shader-readable eye texture [{}x{} resource_fmt={} view_fmt={}]",
        spectator_desc.Width,
        spectator_desc.Height,
        (uint32_t)spectator_desc.Format,
        view_format ? (uint32_t)*view_format : (uint32_t)DXGI_FORMAT_UNKNOWN);
    return true;
}

bool D3D12Component::ensure_ue58_slate_ui_consumer_fence(ID3D12Device* device) {
    if (m_ue58_converted_ui_consumer_fence != nullptr) {
        return true;
    }

    if (device == nullptr || FAILED(device->CreateFence(
        0,
        D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_ue58_converted_ui_consumer_fence))))
    {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[UE5.8][SlateUI] failed to create converted UI consumer fence");
        m_ue58_converted_ui_consumer_fence.Reset();
        return false;
    }

    m_ue58_converted_ui_consumer_fence->SetName(L"UE5.8 Slate UI Consumer Fence");
    return true;
}

void D3D12Component::wait_for_ue58_slate_ui_consumers() {
    if (m_ue58_converted_ui_consumer_fence == nullptr ||
        m_ue58_converted_ui_consumer_fence_value == 0 ||
        m_ue58_converted_ui_consumer_fence->GetCompletedValue() >= m_ue58_converted_ui_consumer_fence_value)
    {
        return;
    }

    const auto event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    if (event == nullptr) {
        SPDLOG_ERROR("[UE5.8][SlateUI] failed to create a consumer-fence wait event during reset");
        return;
    }

    const auto result = m_ue58_converted_ui_consumer_fence->SetEventOnCompletion(
        m_ue58_converted_ui_consumer_fence_value,
        event);

    if (FAILED(result)) {
        SPDLOG_ERROR("[UE5.8][SlateUI] failed to arm consumer-fence wait during reset");
        CloseHandle(event);
        return;
    }

    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);
}

void D3D12Component::release_ue58_converted_ui_source_slot(uint32_t converted_slot_index) {
    if (converted_slot_index >= UE58_CONVERTED_UI_SLOT_COUNT) {
        return;
    }

    auto& source_slot_index = m_ue58_converted_ui_source_slots[converted_slot_index];

    if (source_slot_index < 0 || source_slot_index >= (int32_t)UE58_SLATE_UI_SOURCE_SLOT_COUNT) {
        source_slot_index = -1;
        return;
    }

    auto& source_slot = m_ue58_ui_source_slots[source_slot_index];

    if (source_slot.conversion_references > 0) {
        --source_slot.conversion_references;
    } else {
        SPDLOG_WARN("[UE5.8][SlateUI] converted UI source-slot reference underflow");
    }

    source_slot_index = -1;
}

bool D3D12Component::is_ue58_converted_ui_slot_reusable(uint32_t slot_index) {
    if (slot_index >= UE58_CONVERTED_UI_SLOT_COUNT) {
        return false;
    }

    auto& slot = m_ue58_converted_ui_tex[slot_index];

    // The source SRV remains valid until the conversion command list retires.
    if (slot.commands.ready() && !slot.commands.try_wait()) {
        return false;
    }

    const auto consumer_fence_value = m_ue58_converted_ui_consumer_fence_values[slot_index];

    // The converted output also stays immutable until the OpenXR UI copy that
    // reads it has retired on the same graphics queue.
    if (consumer_fence_value != 0 &&
        (m_ue58_converted_ui_consumer_fence == nullptr ||
         m_ue58_converted_ui_consumer_fence->GetCompletedValue() < consumer_fence_value))
    {
        return false;
    }

    release_ue58_converted_ui_source_slot(slot_index);
    m_ue58_converted_ui_consumer_fence_values[slot_index] = 0;
    return true;
}

d3d12::TextureContext* D3D12Component::acquire_ue58_slate_ui_source_slot(
    ID3D12Device* device,
    ID3D12Resource* resource,
    DXGI_FORMAT view_format)
{
    if (device == nullptr || resource == nullptr) {
        return nullptr;
    }

    for (auto& source_slot : m_ue58_ui_source_slots) {
        if (source_slot.texture.texture.Get() == resource && source_slot.texture.srv_heap != nullptr) {
            return &source_slot.texture;
        }
    }

    for (auto& source_slot : m_ue58_ui_source_slots) {
        if (source_slot.conversion_references != 0) {
            continue;
        }

        if (!source_slot.texture.setup(
                device,
                resource,
                view_format,
                view_format,
                L"UE5.8 Slate UI Source Texture"))
        {
            source_slot.texture.reset();
            return nullptr;
        }

        return &source_slot.texture;
    }

    SPDLOG_WARNING_EVERY_N_SEC(
        1,
        "[UE5.8][SlateUI] all source descriptor slots are still referenced; preserving the last completed converted UI frame");
    return nullptr;
}

void D3D12Component::mark_ue58_converted_ui_slot_consumed(uint32_t slot_index) {
    if (slot_index >= UE58_CONVERTED_UI_SLOT_COUNT ||
        m_ue58_converted_ui_consumer_fence == nullptr ||
        g_framework == nullptr)
    {
        return;
    }

    const auto& d3d12_hook = g_framework->get_d3d12_hook();

    if (d3d12_hook == nullptr) {
        return;
    }

    const auto command_queue = d3d12_hook->get_command_queue();

    if (command_queue == nullptr) {
        return;
    }

    const auto fence_value = ++m_ue58_converted_ui_consumer_fence_value;

    if (FAILED(command_queue->Signal(m_ue58_converted_ui_consumer_fence.Get(), fence_value))) {
        --m_ue58_converted_ui_consumer_fence_value;
        SPDLOG_ERROR_EVERY_N_SEC(1, "[UE5.8][SlateUI] failed to signal converted UI consumer fence");
        return;
    }

    m_ue58_converted_ui_consumer_fence_values[slot_index] = fence_value;
}

void D3D12Component::reset_ue58_converted_ui_textures(bool reset_sources) {
    // Reset is exceptional (resize/device reset). Wait here rather than on the
    // frame path so an OpenXR UI copy never observes a released ring resource.
    wait_for_ue58_slate_ui_consumers();

    for (uint32_t slot_index = 0; slot_index < UE58_CONVERTED_UI_SLOT_COUNT; ++slot_index) {
        auto& converted_ui = m_ue58_converted_ui_tex[slot_index];

        if (converted_ui.commands.ready()) {
            converted_ui.commands.wait(INFINITE);
        }

        release_ue58_converted_ui_source_slot(slot_index);
        converted_ui.reset();
        m_ue58_converted_ui_consumer_fence_values[slot_index] = 0;
    }

    m_ue58_active_converted_ui_tex = nullptr;
    m_ue58_active_converted_ui_slot = UE58_CONVERTED_UI_SLOT_COUNT;
    m_ue58_converted_ui_slot_cursor = 0;
    m_ue58_converted_ui_consumer_fence.Reset();
    m_ue58_converted_ui_consumer_fence_value = 0;

    if (reset_sources) {
        for (auto& source_slot : m_ue58_ui_source_slots) {
            source_slot.texture.reset();
            source_slot.conversion_references = 0;
        }
    }
}

D3D12Component::ShfSceneMode D3D12Component::classify_shf_scene_mode(
    const D3D12_RESOURCE_DESC& source_desc,
    const D3D12_RESOURCE_DESC& real_desc) const
{
    const auto source_width = (uint64_t)source_desc.Width;
    const auto source_height = (uint32_t)source_desc.Height;
    const auto real_width = (uint64_t)real_desc.Width;
    const auto real_height = (uint32_t)real_desc.Height;

    if (real_width > 0 && real_height > 0 && source_width == real_width * 2 && source_height == real_height) {
        return ShfSceneMode::Mono2D;
    }

    if (m_backbuffer_size[0] != 0 && m_backbuffer_size[1] != 0 &&
        source_width == m_backbuffer_size[0] && source_height == m_backbuffer_size[1]) {
        return ShfSceneMode::Stereo3D;
    }

    if (source_width > real_width * 2 || source_height > real_height) {
        return ShfSceneMode::Stereo3D;
    }

    return ShfSceneMode::Unknown;
}

void D3D12Component::log_shf_scene_mode_if_needed(
    ShfSceneMode mode,
    const D3D12_RESOURCE_DESC& source_desc,
    const D3D12_RESOURCE_DESC& real_desc,
    uint64_t frame_count,
    bool using_mono_expansion)
{
    if (!is_shf_current_game()) {
        return;
    }

    if (m_shf_scene_mode != mode) {
        SPDLOG_WARN(
            "[SHf][D3D12] Scene mode changed {} -> {} frame={} src=[{}x{} fmt={} flags=0x{:x}] real=[{}x{} fmt={} flags=0x{:x}] normal_dw={}x{} mono_expanded={}",
            shf_scene_mode_name(m_shf_scene_mode),
            shf_scene_mode_name(mode),
            frame_count,
            source_desc.Width,
            source_desc.Height,
            (uint32_t)source_desc.Format,
            (uint32_t)source_desc.Flags,
            real_desc.Width,
            real_desc.Height,
            (uint32_t)real_desc.Format,
            (uint32_t)real_desc.Flags,
            m_backbuffer_size[0],
            m_backbuffer_size[1],
            using_mono_expansion);
        m_shf_scene_mode = mode;
        return;
    }

    SPDLOG_INFO_EVERY_N_SEC(
        5,
        "[SHf][D3D12] Scene mode summary mode={} frame={} src=[{}x{} fmt={} flags=0x{:x}] real=[{}x{} fmt={} flags=0x{:x}] normal_dw={}x{} mono_expanded={}",
        shf_scene_mode_name(mode),
        frame_count,
        source_desc.Width,
        source_desc.Height,
        (uint32_t)source_desc.Format,
        (uint32_t)source_desc.Flags,
        real_desc.Width,
        real_desc.Height,
        (uint32_t)real_desc.Format,
        (uint32_t)real_desc.Flags,
        m_backbuffer_size[0],
        m_backbuffer_size[1],
        using_mono_expansion);
}

bool D3D12Component::ensure_shf_mono_scene_texture(ID3D12Device* device, const D3D12_RESOURCE_DESC& source_desc) {
    if (device == nullptr || m_backbuffer_size[0] == 0 || m_backbuffer_size[1] == 0) {
        return false;
    }

    auto mono_desc = source_desc;
    mono_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    mono_desc.Alignment = 0;
    mono_desc.Width = m_backbuffer_size[0];
    mono_desc.Height = m_backbuffer_size[1];
    mono_desc.DepthOrArraySize = 1;
    mono_desc.MipLevels = 1;
    mono_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    mono_desc.SampleDesc.Count = 1;
    mono_desc.SampleDesc.Quality = 0;
    mono_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    mono_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    mono_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    const auto needs_create =
        m_shf_mono_scene_tex.texture.Get() == nullptr ||
        m_shf_mono_scene_width != mono_desc.Width ||
        m_shf_mono_scene_height != mono_desc.Height ||
        m_shf_mono_scene_format != mono_desc.Format;

    if (!needs_create) {
        return m_shf_mono_scene_tex.srv_heap != nullptr && m_shf_mono_scene_tex.rtv_heap != nullptr;
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    m_shf_mono_scene_tex.reset();

    ComPtr<ID3D12Resource> mono_tex{};
    if (FAILED(device->CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &mono_desc,
            ENGINE_SRC_COLOR,
            nullptr,
            IID_PPV_ARGS(&mono_tex)))) {
        SPDLOG_ERROR_EVERY_N_SEC(
            1,
            "[SHf][D3D12] Failed to create mono cutscene expansion texture [{}x{} fmt={} flags=0x{:x}]",
            mono_desc.Width,
            mono_desc.Height,
            (uint32_t)mono_desc.Format,
            (uint32_t)mono_desc.Flags);
        return false;
    }

    mono_tex->SetName(L"SHf Mono Cutscene Expansion");

    if (!m_shf_mono_scene_tex.setup(device, mono_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"SHf Mono Cutscene Expansion")) {
        spdlog::error("[SHf][D3D12] Failed to setup mono cutscene expansion texture.");
        m_shf_mono_scene_tex.reset();
        m_shf_mono_scene_width = 0;
        m_shf_mono_scene_height = 0;
        m_shf_mono_scene_format = DXGI_FORMAT_UNKNOWN;
        return false;
    }

    m_shf_mono_scene_width = mono_desc.Width;
    m_shf_mono_scene_height = mono_desc.Height;
    m_shf_mono_scene_format = mono_desc.Format;

    if (!m_shf_mono_scene_commands.ready()) {
        m_shf_mono_scene_commands.setup(L"SHf Mono Cutscene Expansion Commands");
    }

    SPDLOG_WARN(
        "[SHf][D3D12] Created mono cutscene expansion texture [{}x{}] from source [{}x{}]",
        mono_desc.Width,
        mono_desc.Height,
        source_desc.Width,
        source_desc.Height);

    return true;
}

d3d12::TextureContext* D3D12Component::render_shf_mono_scene_texture(ID3D12Device* device) {
    if (!SHF_AUTO_MONO_CINEMATIC ||
        m_game_batch == nullptr ||
        m_game_tex.texture.Get() == nullptr ||
        m_game_tex.srv_heap == nullptr ||
        m_game_tex.srv_heap->Heap() == nullptr) {
        return nullptr;
    }

    const auto source_desc = m_game_tex.texture->GetDesc();

    if (!ensure_shf_mono_scene_texture(device, source_desc) ||
        m_shf_mono_scene_tex.texture.Get() == nullptr ||
        m_shf_mono_scene_tex.rtv_heap == nullptr) {
        return nullptr;
    }

    auto& command_ctx = m_shf_mono_scene_commands;

    if (!command_ctx.ready()) {
        command_ctx.setup(L"SHf Mono Cutscene Expansion Commands");
    }

    if (!command_ctx.ready()) {
        return nullptr;
    }

    command_ctx.wait(INFINITE);

    const float clear_color[] = {0.0f, 0.0f, 0.0f, 0.0f};
    command_ctx.clear_rtv(m_shf_mono_scene_tex, clear_color, ENGINE_SRC_COLOR);

    const auto half_width = (LONG)(m_backbuffer_size[0] / 2);
    const auto full_width = (LONG)m_backbuffer_size[0];
    const auto full_height = (LONG)m_backbuffer_size[1];
    const auto source_half_width = (LONG)(source_desc.Width / 2);
    const auto source_height = (LONG)source_desc.Height;

    const RECT left_src{0, 0, source_half_width, source_height};
    const RECT right_src{source_half_width, 0, (LONG)source_desc.Width, source_height};

    auto fit_eye_rect = [&](LONG eye_left, LONG eye_right) {
        RECT dest{eye_left, 0, eye_right, full_height};
        const auto eye_width = (float)(eye_right - eye_left);
        const auto eye_height = (float)full_height;
        const auto source_aspect = source_half_width > 0 && source_height > 0 ? (float)source_half_width / (float)source_height : 1.0f;
        const auto eye_aspect = eye_height > 0.0f ? eye_width / eye_height : source_aspect;

        if (source_aspect > eye_aspect) {
            const auto fitted_height = (LONG)(eye_width / source_aspect);
            const auto y = (full_height - fitted_height) / 2;
            dest.top = y;
            dest.bottom = y + fitted_height;
        } else {
            const auto fitted_width = (LONG)(eye_height * source_aspect);
            const auto x = eye_left + ((LONG)eye_width - fitted_width) / 2;
            dest.left = x;
            dest.right = x + fitted_width;
        }

        return dest;
    };

    const auto left_dest = fit_eye_rect(0, half_width);
    const auto right_dest = fit_eye_rect(half_width, full_width);

    d3d12::render_srv_to_rtv(
        m_game_batch.get(),
        command_ctx.cmd_list.Get(),
        m_game_tex,
        m_shf_mono_scene_tex,
        left_src,
        left_dest,
        ENGINE_SRC_COLOR,
        ENGINE_SRC_COLOR);

    d3d12::render_srv_to_rtv(
        m_game_batch.get(),
        command_ctx.cmd_list.Get(),
        m_game_tex,
        m_shf_mono_scene_tex,
        right_src,
        right_dest,
        ENGINE_SRC_COLOR,
        ENGINE_SRC_COLOR);

    command_ctx.execute();

    SPDLOG_INFO_EVERY_N_SEC(
        2,
        "[SHf][D3D12] Expanded low-res cutscene source [{}x{}] into stereo-safe double-wide [{}x{}]",
        source_desc.Width,
        source_desc.Height,
        m_backbuffer_size[0],
        m_backbuffer_size[1]);

    return &m_shf_mono_scene_tex;
}

bool D3D12Component::ensure_dune_hmd_mono_scene_texture(ID3D12Device* device, const D3D12_RESOURCE_DESC& source_desc) {
    auto vr = VR::get();

    if (device == nullptr || vr == nullptr || vr->get_hmd_width() == 0 || vr->get_hmd_height() == 0) {
        return false;
    }

    const auto width_multiplier = vr->is_using_afr() ? 1u : 2u;
    const auto target_width = (uint64_t)vr->get_hmd_width() * width_multiplier;
    const auto target_height = vr->get_hmd_height();

    auto mono_desc = source_desc;
    mono_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    mono_desc.Alignment = 0;
    mono_desc.Width = target_width;
    mono_desc.Height = target_height;
    mono_desc.DepthOrArraySize = 1;
    mono_desc.MipLevels = 1;
    mono_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    mono_desc.SampleDesc.Count = 1;
    mono_desc.SampleDesc.Quality = 0;
    mono_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    mono_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    mono_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    const auto needs_create =
        m_dune_hmd_mono_scene_tex.texture.Get() == nullptr ||
        m_dune_hmd_mono_scene_width != mono_desc.Width ||
        m_dune_hmd_mono_scene_height != mono_desc.Height ||
        m_dune_hmd_mono_scene_format != mono_desc.Format;

    if (!needs_create) {
        return m_dune_hmd_mono_scene_tex.srv_heap != nullptr && m_dune_hmd_mono_scene_tex.rtv_heap != nullptr;
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    m_dune_hmd_mono_scene_tex.reset();

    ComPtr<ID3D12Resource> mono_tex{};
    if (FAILED(device->CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &mono_desc,
            ENGINE_SRC_COLOR,
            nullptr,
            IID_PPV_ARGS(&mono_tex)))) {
        SPDLOG_ERROR_EVERY_N_SEC(
            1,
            "[Dune][D3D12] Failed to create HMD mono scene texture [{}x{} fmt={} flags=0x{:x}]",
            mono_desc.Width,
            mono_desc.Height,
            (uint32_t)mono_desc.Format,
            (uint32_t)mono_desc.Flags);
        return false;
    }

    mono_tex->SetName(L"Dune HMD Mono Scene");

    if (!m_dune_hmd_mono_scene_tex.setup(device, mono_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Dune HMD Mono Scene")) {
        spdlog::error("[Dune][D3D12] Failed to setup HMD mono scene texture.");
        m_dune_hmd_mono_scene_tex.reset();
        m_dune_hmd_mono_scene_width = 0;
        m_dune_hmd_mono_scene_height = 0;
        m_dune_hmd_mono_scene_format = DXGI_FORMAT_UNKNOWN;
        return false;
    }

    m_dune_hmd_mono_scene_width = mono_desc.Width;
    m_dune_hmd_mono_scene_height = mono_desc.Height;
    m_dune_hmd_mono_scene_format = mono_desc.Format;

    if (!m_dune_hmd_mono_scene_commands.ready()) {
        m_dune_hmd_mono_scene_commands.setup(L"Dune HMD Mono Scene Commands");
    }

    SPDLOG_WARN(
        "[Dune][D3D12] Created HMD mono scene texture [{}x{}] from desktop source [{}x{}] afr={}",
        mono_desc.Width,
        mono_desc.Height,
        source_desc.Width,
        source_desc.Height,
        vr->is_using_afr());

    return true;
}

d3d12::TextureContext* D3D12Component::render_dune_hmd_mono_scene_texture(
    ID3D12Device* device,
    D3D12_RESOURCE_STATES source_state)
{
    if (!is_dune_awakening_current_game() ||
        m_game_batch == nullptr ||
        m_game_tex.texture.Get() == nullptr ||
        m_game_tex.srv_heap == nullptr ||
        m_game_tex.srv_heap->Heap() == nullptr) {
        return nullptr;
    }

    auto vr = VR::get();
    const auto source_desc = m_game_tex.texture->GetDesc();

    if (vr == nullptr ||
        !ensure_dune_hmd_mono_scene_texture(device, source_desc) ||
        m_dune_hmd_mono_scene_tex.texture.Get() == nullptr ||
        m_dune_hmd_mono_scene_tex.rtv_heap == nullptr) {
        return nullptr;
    }

    auto& command_ctx = m_dune_hmd_mono_scene_commands;

    if (!command_ctx.ready()) {
        command_ctx.setup(L"Dune HMD Mono Scene Commands");
    }

    if (!command_ctx.ready()) {
        return nullptr;
    }

    command_ctx.wait(INFINITE);

    const auto transition_source = source_state != ENGINE_SRC_COLOR;
    D3D12_RESOURCE_BARRIER source_to_srv{};
    if (transition_source) {
        source_to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        source_to_srv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        source_to_srv.Transition.pResource = m_game_tex.texture.Get();
        source_to_srv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        source_to_srv.Transition.StateBefore = source_state;
        source_to_srv.Transition.StateAfter = ENGINE_SRC_COLOR;
        command_ctx.cmd_list->ResourceBarrier(1, &source_to_srv);
    }

    const float clear_color[] = {0.0f, 0.0f, 0.0f, 0.0f};
    command_ctx.clear_rtv(m_dune_hmd_mono_scene_tex, clear_color, ENGINE_SRC_COLOR);

    const auto target_desc = m_dune_hmd_mono_scene_tex.texture->GetDesc();
    const auto target_width = (LONG)target_desc.Width;
    const auto target_height = (LONG)target_desc.Height;
    const RECT source_rect{0, 0, (LONG)source_desc.Width, (LONG)source_desc.Height};

    if (vr->is_using_afr()) {
        const RECT dest_rect{0, 0, target_width, target_height};
        d3d12::render_srv_to_rtv(
            m_game_batch.get(),
            command_ctx.cmd_list.Get(),
            m_game_tex,
            m_dune_hmd_mono_scene_tex,
            source_rect,
            dest_rect,
            ENGINE_SRC_COLOR,
            ENGINE_SRC_COLOR);
    } else {
        const auto half_width = target_width / 2;
        const RECT left_dest{0, 0, half_width, target_height};
        const RECT right_dest{half_width, 0, target_width, target_height};

        d3d12::render_srv_to_rtv(
            m_game_batch.get(),
            command_ctx.cmd_list.Get(),
            m_game_tex,
            m_dune_hmd_mono_scene_tex,
            source_rect,
            left_dest,
            ENGINE_SRC_COLOR,
            ENGINE_SRC_COLOR);

        d3d12::render_srv_to_rtv(
            m_game_batch.get(),
            command_ctx.cmd_list.Get(),
            m_game_tex,
            m_dune_hmd_mono_scene_tex,
            source_rect,
            right_dest,
            ENGINE_SRC_COLOR,
            ENGINE_SRC_COLOR);
    }

    if (transition_source) {
        source_to_srv.Transition.StateBefore = ENGINE_SRC_COLOR;
        source_to_srv.Transition.StateAfter = source_state;
        command_ctx.cmd_list->ResourceBarrier(1, &source_to_srv);
    }

    command_ctx.execute();

    SPDLOG_INFO_EVERY_N_SEC(
        2,
        "[Dune][D3D12] Expanded desktop scene [{}x{}] into HMD mono scene [{}x{}] afr={} source_state=0x{:x}",
        source_desc.Width,
        source_desc.Height,
        target_desc.Width,
        target_desc.Height,
        vr->is_using_afr(),
        (uint32_t)source_state);

    return &m_dune_hmd_mono_scene_tex;
}

vr::EVRCompositorError D3D12Component::on_frame(VR* vr) {
    if (vr != nullptr && vr->get_runtime()->is_flat3d()) {
        return on_frame_flat3d(vr); // flat 3D monitor mode: composite to the real backbuffer, no VR submit
    }

    const bool collect_frame_timing = vr != nullptr && vr->is_hitch_diagnostics_enabled();
    d3d12::set_fence_profiler_enabled(collect_frame_timing);

    if (collect_frame_timing != m_frame_timing_collection_active) {
        reset_frame_timing_stats();
        m_frame_timing_collection_active = collect_frame_timing;
    }

    const auto on_frame_start = collect_frame_timing
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    utility::ScopeGuard frame_timing_guard{[&]() {
        if (collect_frame_timing) {
            m_perf_on_frame.add(std::chrono::steady_clock::now() - on_frame_start);
            log_frame_timing_stats_if_needed(vr);
        }
    }};

    m_last_on_frame = std::chrono::steady_clock::now();
    apply_dune_descriptor_cache_guard();
    bool defer_stalker2_transition_openxr = false;

    auto close_openxr_setup_failure_frame = [&]() {
        if (vr->m_openxr == nullptr || !vr->get_runtime()->is_openxr()) {
            return;
        }

        if (vr->m_openxr->close_synced_frame_without_layers("d3d12_setup_failed")) {
            SPDLOG_WARNING_EVERY_N_SEC(
                1,
                "[D3D12 VR] Closed pending OpenXR frame after D3D12 setup failure so the runtime can keep advancing");
        }
    };

    if (!is_dead_island_2_ue425_current_game() || !vr->is_using_strict_synchronized_afr()) {
        m_dead_island_2_synced_eye_rebase_pending = false;
    }
    if (m_force_reset || m_last_afr_state != vr->is_using_afr()) {
        if (!setup()) {
            SPDLOG_ERROR_EVERY_N_SEC(1, "[D3D12 VR] Could not set up, trying again next frame");
            m_force_reset = true;
            return vr::VRCompositorError_None;
        }

        m_last_afr_state = vr->is_using_afr();
    }

    auto& hook = g_framework->get_d3d12_hook();

    hook->set_next_present_interval(0); // disable vsync for vr
    
    // get device
    auto device = hook->get_device();

    // get command queue
    auto command_queue = hook->get_command_queue();

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    // get back buffer
    ComPtr<ID3D12Resource> backbuffer{};
    ComPtr<ID3D12Resource> real_backbuffer{};
    auto ue4_texture = VR::get()->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

    if (ue4_texture != nullptr) {
        backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
    }

    if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get real back buffer.");
        return vr::VRCompositorError_None;
    }

    if (vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to get back buffer.");
        return vr::VRCompositorError_None;
    }

    const auto ui_should_invert_alpha = vr->get_overlay_component().should_invert_ui_alpha();

    // Update the UI overlay.
    auto runtime = vr->get_runtime();

    const auto is_same_frame = m_last_rendered_frame > 0 && m_last_rendered_frame == vr->m_render_frame_count;
    m_last_rendered_frame = vr->m_render_frame_count;

    const auto is_actually_afr = vr->is_using_afr();
    const auto is_afr = !is_same_frame && vr->is_using_afr();
    const auto is_left_eye_frame = is_afr && vr->m_render_frame_count % 2 == vr->m_left_eye_interval;
    const auto is_right_eye_frame = !is_afr || vr->m_render_frame_count % 2 == vr->m_right_eye_interval;

    // Sometimes this can happen if pipeline execution does not go exactly as planned
    // so we need to resynchronized or begin the frame again.
    if (runtime->ready()) {
        runtime->fix_frame();
    }

    const auto& ffsr = VR::get()->m_fake_stereo_hook;
    const auto ui_target = ffsr->get_render_target_manager()->get_ui_target();

    const auto frame_count = vr->m_render_frame_count;

    if (m_game_tex.texture.Get() == nullptr && backbuffer.Get() == real_backbuffer.Get()) {
        spdlog::info("[VR] Setting up game texture as copy of backbuffer");
        
        ComPtr<ID3D12Resource> backbuffer_copy{};
        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        auto desc = backbuffer->GetDesc();
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        m_backbuffer_copy.reset();

        ComPtr<ID3D12Resource> backbuffer_copy2{};

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&backbuffer_copy2)))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return vr::VRCompositorError_None;
        }

        if (!m_backbuffer_copy.setup(device, backbuffer_copy2.Get(), std::nullopt, std::nullopt, L"Backbuffer Copy")) {
            spdlog::error("[VR] Failed to fully setup backbuffer copy.");
            m_backbuffer_copy.reset();
        }

        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // UE backbuffer is not VR compatible, so we need to copy it to a new texture with this one.

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&backbuffer_copy)))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return vr::VRCompositorError_None;
        }

        if (!m_game_tex.setup(device, backbuffer_copy.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Game Texture")) {
            spdlog::error("[VR] Failed to fully setup game texture.");
            m_game_tex.reset();
        } else {
            for (auto& commands : m_game_tex_commands) {
                commands.setup(L"Game Texture Commands");
            }
        }
    } else if (backbuffer.Get() != real_backbuffer.Get() && m_game_tex.texture.Get() != backbuffer.Get()) {
        spdlog::info("[VR] Setting up game texture as reference to original");

        if (!m_game_tex.setup(device, backbuffer.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Game Texture")) {
            spdlog::error("[VR] Failed to fully setup game texture.");
            m_game_tex.reset();
        }
    }

    if (vr->is_native_stereo_fix_enabled()) {
        const auto scene_capture = ffsr->get_render_target_manager()->get_scene_capture_render_target();
        const auto scene_capture_rt = scene_capture != nullptr ? (ID3D12Resource*)scene_capture->get_native_resource() : nullptr;

        if (scene_capture_rt != nullptr && m_scene_capture_tex.texture.Get() != scene_capture_rt) {
            spdlog::info("[VR] Setting up scene capture texture as reference to original");

            if (!m_scene_capture_tex.setup(device, scene_capture_rt, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Scene Capture Texture")) {
                spdlog::error("[VR] Failed to fully setup scene capture texture.");
                m_scene_capture_tex.reset();
            }
        }

        if (scene_capture_rt == nullptr && m_scene_capture_tex.texture.Get() != nullptr) {
            spdlog::info("[VR] Resetting scene capture texture");

            m_scene_capture_tex.reset();
        }
    } else {
        m_scene_capture_tex.reset();
    }

    // We need to render the scene capture texture to the right side of the double wide texture
    auto pre_render = [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
        if (render_target == nullptr) {
            return;
        }

        // Also the same for right, even though it's not a double wide texture
        D3D12_BOX left_src_box{
            .left = 0,
            .top = 0,
            .front = 0,
            .right = m_backbuffer_size[0] / 2,
            .bottom = m_backbuffer_size[1],
            .back = 1
        };

        commands.copy_region_stereo(
            m_game_tex.texture.Get(), m_scene_capture_tex.texture.Get(), render_target,
            &left_src_box, &left_src_box,
            0, 0, 0, m_backbuffer_size[0] / 2, 0, 0,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET
        );
    };

    // For copying the real backbuffer if we need to
    if (m_game_tex.texture.Get() != nullptr && backbuffer == real_backbuffer) {
        const auto idx = swapchain->GetCurrentBackBufferIndex() % m_game_tex_commands.size();
        auto& command_ctx = m_game_tex_commands[idx];
        if (command_ctx.cmd_list != nullptr) {
            command_ctx.wait(INFINITE);
            float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
            command_ctx.clear_rtv(m_game_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            command_ctx.copy(real_backbuffer.Get(), m_backbuffer_copy.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            //m_game_tex_commands[idx].copy(backbuffer.Get(), m_game_tex.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, ENGINE_SRC_COLOR);
            d3d12::render_srv_to_rtv(
                m_game_batch.get(),
                command_ctx.cmd_list.Get(),
                m_backbuffer_copy,
                m_game_tex,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RENDER_TARGET
            );
            command_ctx.execute();
        }

        backbuffer = m_game_tex.texture;
    }

    if (ui_target != nullptr) {
        if (m_game_ui_tex.texture.Get() != ui_target->get_native_resource()) {
            if (!m_game_ui_tex.setup(device, 
                (ID3D12Resource*)ui_target->get_native_resource(), 
                DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
                L"Game UI Texture"))
            {
                spdlog::error("[VR] Failed to fully setup game UI texture.");
                m_game_ui_tex.reset();
            }
        }

        // Recreate UI texture if needed
        if (!vr->is_extreme_compatibility_mode_enabled()) {
            const auto native = (ID3D12Resource*)ui_target->get_native_resource();
            const auto is_same_native = native == m_last_checked_native;
            m_last_checked_native = native;

            if (native != nullptr && !is_same_native) {
                const auto desc = native->GetDesc();

                if (runtime->is_openxr()) {
                    if (auto it = vr->m_openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::UI);
                        it != vr->m_openxr->swapchains.end()) 
                    {
                        const auto& uisc = it->second;
                        if (desc.Width != uisc.width ||
                            desc.Height != uisc.height)
                        {
                            SPDLOG_INFO_EVERY_N_SEC(1, "[OpenXR] UI size changed, recreating [{}x{}]->[{}x{}]", desc.Width, desc.Height, uisc.width, uisc.height);
                            ffsr->set_should_recreate_textures(true);
                        }
                    }
                } else if (m_game_ui_tex.texture != nullptr) {
                    const auto ui_desc = m_game_ui_tex.texture->GetDesc();

                    if (desc.Width != ui_desc.Width || desc.Height != ui_desc.Height) {
                        SPDLOG_INFO_EVERY_N_SEC(1, "[OpenVR] UI size changed, recreating texture [{}x{}]->[{}x{}]", desc.Width, desc.Height, ui_desc.Width, ui_desc.Height);
                        ffsr->set_should_recreate_textures(true);
                    }
                }
            } else if (native == nullptr) {
                spdlog::error("[VR] Recreating UI texture because native resource is null");
                ffsr->set_should_recreate_textures(true);
            }
        }
    } else {
        m_game_ui_tex.reset(); // Probably fixes non-resident errors.
    }

    const float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const auto is_2d_screen = vr->is_using_2d_screen();

    auto draw_2d_view = [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
        if (ui_should_invert_alpha && m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
            d3d12::render_srv_to_rtv(m_ui_batch_alpha_invert.get(), commands.cmd_list.Get(), m_game_ui_tex, m_game_ui_tex, std::nullopt, ENGINE_SRC_COLOR, ENGINE_SRC_COLOR);
        }

        draw_spectator_view(commands.cmd_list.Get(), is_right_eye_frame);

        if (is_2d_screen && m_game_tex.texture.Get() != nullptr && m_game_tex.srv_heap != nullptr) {
            // Clear previous frame
            for (auto& screen : m_2d_screen_tex) {
                commands.clear_rtv(screen, clear_color, ENGINE_SRC_COLOR);
            }

            // Render left side to left screen tex
            d3d12::render_srv_to_rtv(
                m_game_batch.get(),
                commands.cmd_list.Get(),
                m_game_tex,
                m_2d_screen_tex[0],
                RECT{0, 0, (LONG)((float)m_backbuffer_size[0] / 2.0f), (LONG)m_backbuffer_size[1]},
                ENGINE_SRC_COLOR,
                ENGINE_SRC_COLOR
            );

            if (m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
                d3d12::render_srv_to_rtv(
                    m_game_batch.get(),
                    commands.cmd_list.Get(),
                    m_game_ui_tex,
                    m_2d_screen_tex[0],
                    ENGINE_SRC_COLOR,
                    ENGINE_SRC_COLOR
                );
            }

            if (!is_afr) {
                // Render right side to right screen tex
                if (m_scene_capture_tex.texture.Get() != nullptr) {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_scene_capture_tex,
                        m_2d_screen_tex[1],
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                } else {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_game_tex,
                        m_2d_screen_tex[1],
                        RECT{(LONG)((float)m_backbuffer_size[0] / 2.0f), 0, (LONG)((float)m_backbuffer_size[0]), (LONG)m_backbuffer_size[1]},
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                }

                if (m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_game_ui_tex,
                        m_2d_screen_tex[1],
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                }
            }

            // Clear the RT so the entire background is black when submitting to the compositor
            commands.clear_rtv(m_game_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);

            if (m_scene_capture_tex.texture.Get() != nullptr) {
                commands.clear_rtv(m_scene_capture_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
    };

    // Draws the spectator view
    auto clear_rt = [&](d3d12::CommandContext& commands) {
        if (m_game_ui_tex.texture.Get() == nullptr) {
            return;
        }

        const float ui_clear_color[] = { 0.0f, 0.0f, 0.0f, ui_should_invert_alpha ? 1.0f : 0.0f };
        commands.clear_rtv(m_game_ui_tex, (float*)&ui_clear_color, ENGINE_SRC_COLOR);
    };

    if (runtime->is_openvr() && m_openvr.ui_tex.texture.Get() != nullptr) {
        m_openvr.ui_tex.commands.wait(INFINITE);

        draw_2d_view(m_openvr.ui_tex.commands, nullptr);

        if (is_right_eye_frame) {
            if (is_2d_screen) {
                m_openvr.ui_tex.commands.copy(m_2d_screen_tex[0].texture.Get(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
            } else if (ui_target != nullptr) {
                m_openvr.ui_tex.commands.copy((ID3D12Resource*)ui_target->get_native_resource(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
            }
        } else if (is_2d_screen) {
            m_openvr.ui_tex.commands.copy(m_2d_screen_tex[0].texture.Get(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
        }

        clear_rt(m_openvr.ui_tex.commands);
        m_openvr.ui_tex.commands.execute();
    } else if (runtime->is_openxr() && runtime->ready() && vr->m_openxr->frame_began) {
        if (is_right_eye_frame) {
            if (is_2d_screen) {
                if (is_afr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, m_2d_screen_tex[0].texture.Get(), draw_2d_view, clear_rt, ENGINE_SRC_COLOR);
                } else {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_2d_screen_tex[0].texture.Get(), draw_2d_view, std::nullopt, ENGINE_SRC_COLOR);
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, m_2d_screen_tex[1].texture.Get(), std::nullopt, clear_rt, ENGINE_SRC_COLOR);
                }
            } else if (ui_target != nullptr) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, (ID3D12Resource*)ui_target->get_native_resource(), draw_2d_view, clear_rt, ENGINE_SRC_COLOR);
            }

            auto fw_rt = g_framework->get_rendertarget_d3d12();

            if (fw_rt && g_framework->is_drawing_anything()) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, g_framework->get_rendertarget_d3d12().Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        } else if (is_2d_screen) {
            m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_2d_screen_tex[0].texture.Get(), draw_2d_view, clear_rt, ENGINE_SRC_COLOR);
        } else if (m_game_ui_tex.commands.ready()) {
            m_game_ui_tex.commands.wait(INFINITE);
            draw_2d_view(m_game_ui_tex.commands, nullptr);
            clear_rt(m_game_ui_tex.commands);
            m_game_ui_tex.commands.execute();
        }
    }

    /*else if (m_game_tex.texture.Get() != nullptr) {
        m_game_tex.commands.wait(INFINITE);
        draw_spectator_view(m_game_tex.commands.cmd_list.Get(), is_right_eye_frame);
        m_game_tex.commands.execute();
    }*/

    ComPtr<ID3D12Resource> scene_depth_tex{};

    if (vr->is_depth_enabled() && runtime->is_depth_allowed()) {
        auto& rt_pool = vr->get_render_target_pool_hook();
        scene_depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");

        if (scene_depth_tex != nullptr) {
            const auto desc = scene_depth_tex->GetDesc();

            if (runtime->is_openxr()) {
                if (vr->m_openxr->needs_depth_resize(desc.Width, desc.Height) || m_openxr.made_depth_with_null_defaults) {
                    spdlog::info("[OpenXR] Depth size changed, recreating swapchains [{}x{}]", desc.Width, desc.Height);
                    m_openxr.create_swapchains(); // recreate swapchains to match the new depth size
                }
            }
        }

    #ifdef AFR_DEPTH_TEMP_DISABLED
        if (is_actually_afr) {
            scene_depth_tex.Reset();
        }
    #endif
    }

    // #############################
    // #Frame Warp Module Start
    // #############################

    bool is_using_afw = vr->is_using_afw();

    const auto bb_desc = backbuffer->GetDesc();
    const auto eye_width = static_cast<uint32_t>(bb_desc.Width / 2);
    const auto eye_height = static_cast<uint32_t>(bb_desc.Height);

    vr->finalSize[0] = eye_width;
    vr->finalSize[1] = eye_height;

    if (!vr->rawDepthTex) {
        auto& rt_pool = vr->get_render_target_pool_hook();
        scene_depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");
        if (scene_depth_tex)
            vr->rawDepthTex = scene_depth_tex.Get();
    }

    auto backbuffer_index = swapchain->GetCurrentBackBufferIndex();

    EyeIndex nEye = (frame_count % 2 == vr->m_left_eye_interval) ? EyeLeft : EyeRight;
    EyeIndex nEyeOther = (frame_count % 2 == vr->m_left_eye_interval) ? EyeRight : EyeLeft;
    auto eyeFrameBuffer = m_eyeFrameBuffers.eyeFrameBuffers[nEye];
    auto otherEyeFrameBuffer = m_eyeFrameBuffers.eyeFrameBuffers[nEyeOther];
    FrameWarpEvaluateParams params;
    if ((is_using_afw) && (!eyeFrameBuffer.color.pTexture || !otherEyeFrameBuffer.color.pTexture))
        force_reset();

    auto colorDesc = eyeFrameBuffer.color.pTexture->GetDesc();
    static TextureDesc backbufferDesc[6];
    if (backbufferDesc[backbuffer_index].pTexture != backbuffer.Get()) {
        backbufferDesc[backbuffer_index].pTexture = backbuffer.Get();
        backbufferDesc[backbuffer_index].initialState = D3D12_RESOURCE_STATE_PRESENT;
        vr->d3d12Renderer->SetupTextureDesc(backbufferDesc[backbuffer_index]);
    }
    static TextureDesc realBackbufferDesc[6];
    if (realBackbufferDesc[backbuffer_index].pTexture != real_backbuffer.Get()) {
        realBackbufferDesc[backbuffer_index].pTexture = real_backbuffer.Get();
        realBackbufferDesc[backbuffer_index].initialState = D3D12_RESOURCE_STATE_PRESENT;
        vr->d3d12Renderer->SetupTextureDesc(realBackbufferDesc[backbuffer_index]);
    }

    if (vr->rawDepthTex) {
        auto desc = vr->rawDepthTex->GetDesc();
        for (int i = 0; i < 2; i++) {
            if (vr->depthDesc[i].pTexture == NULL || vr->depthDesc[i].pTexture->GetDesc().Width != desc.Width ||
                vr->depthDesc[i].pTexture->GetDesc().Height != desc.Height) {
                vr->d3d12Renderer->CreateTexture(
                    desc.Width, desc.Height, desc.Format, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, vr->depthDesc[i], true);
            }
        }
        if (!vr->rawMotionVectorsTex) {
            for (int i = 0; i < 2; i++) {
                if (vr->motionVectorsDesc[i].pTexture == NULL || vr->motionVectorsDesc[i].pTexture->GetDesc().Width != desc.Width ||
                    vr->motionVectorsDesc[i].pTexture->GetDesc().Height != desc.Height) {
                    vr->d3d12Renderer->CreateTexture(desc.Width, desc.Height, DXGI_FORMAT_R16G16_FLOAT,
                        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, vr->motionVectorsDesc[i], true);
                }
            }
        }
    }
    if (vr->rawMotionVectorsTex) {
        auto desc = vr->rawMotionVectorsTex->GetDesc();
        for (int i = 0; i < 2; i++) {
            if (vr->motionVectorsDesc[i].pTexture == NULL || vr->motionVectorsDesc[i].pTexture->GetDesc().Width != desc.Width ||
                vr->motionVectorsDesc[i].pTexture->GetDesc().Height != desc.Height) {
                vr->d3d12Renderer->CreateTexture(desc.Width, desc.Height, DXGI_FORMAT_R16G16_FLOAT,
                    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, vr->motionVectorsDesc[i], true);
            }
        }
    }

    auto cmdList = vr->d3d12Renderer->BeginCommandList(backbuffer_index);

    if (is_using_afw && eyeFrameBuffer.color.pTexture && vr->depthDesc[nEye].pTexture) {
        static FrameBufferDesc s_CurrentEyeFrameBuffer{};

        D3D12_BOX src_box{.left = 0,
            .top = 0,
            .front = 0,
            .right = vr->is_extreme_compatibility_mode_enabled() ? m_backbuffer_size[0] : m_backbuffer_size[0] / 2,
            .bottom = m_backbuffer_size[1],
            .back = 1};

        // Sharpening
        if (vr->is_enable_sharpening() && vr->get_sharpness() > 0) {
            vr->d3d12Renderer->Crop(cmdList, otherEyeFrameBuffer.color, backbufferDesc[backbuffer_index], src_box);
            vr->d3d12Renderer->Sharpen(cmdList, eyeFrameBuffer.color, otherEyeFrameBuffer.color, vr->get_sharpness());
        } else {
            vr->d3d12Renderer->Crop(cmdList, eyeFrameBuffer.color, backbufferDesc[backbuffer_index], src_box);
        }

        s_CurrentEyeFrameBuffer.color = eyeFrameBuffer.color;
        s_CurrentEyeFrameBuffer.depth = vr->depthDesc[nEye];
        s_CurrentEyeFrameBuffer.motionVectors = vr->motionVectorsDesc[nEye];

        params.InCmdList = cmdList;
        params.InEyeFrameBuffer = &s_CurrentEyeFrameBuffer;
        params.InUIColorAlpha = NULL;
        params.IsHudlessColor = true;
        params.MotionVectorsType = vr->is_ghosting_fix_enabled() ? Normal : FromOtherEye;
        params.InMotionScale[0] = vr->mvScale[0];
        params.InMotionScale[1] = vr->mvScale[1];
        params.Mode = (FrameWarpMode)vr->m_framewarp_mode->value();
        params.EyeIndex = nEye;
        params.ClearBeforeWarping = vr->m_clear_before_framewarp->value();
        params.CameraData = &vr->cameraData[nEye];
        params.IgnoreMotionThreshold = vr->m_ignore_motion_threshold->value();
        params.Debug = vr->m_framewarp_debug->value();
        if (vr->is_ghosting_fix_enabled() && vr->is_fix_object_motion_vector() && vr->is_fix_moving_object_brightness_flickering())
            params.InUEVelocityBuffer = &vr->rawVelocityDesc[nEye];
        params.UseUINT64 = vr->is_use_uint64();
        params.ShadingRate = vr->get_framewarp_shading_rate();
        EvaluateFrameWarp(params);
    }

    if (vr->mDebug3 && vr->is_fix_object_motion_vector() && vr->rawVelocityDesc[nEye].pTexture) {
        if (vr->rawVelocityDesc[nEye].shaderResourceViewHandle.ptr == 0) {
            vr->rawVelocityDesc[nEye].initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            vr->d3d12Renderer->SetupTextureDesc(vr->rawVelocityDesc[nEye]);
        }
        D3D12_VIEWPORT vp{
            .TopLeftX = 0, 
            .TopLeftY = 0, 
            .Width = (float)(vr->is_extreme_compatibility_mode_enabled() ? m_backbuffer_size[0] : m_backbuffer_size[0] / 2),
            .Height = (float)m_backbuffer_size[1],
            .MinDepth = 0, 
            .MaxDepth = 1
        };
        vr->d3d12Renderer->Blit(cmdList, backbufferDesc[backbuffer_index], vr->rawVelocityDesc[nEye], vp);
    }

    vr->d3d12Renderer->EndCommandList(backbuffer_index);

    // #############################
    // #Frame Warp Module End
    // #############################

    // If m_frame_count is even, we're rendering the left eye.
    if (is_left_eye_frame) {
        m_submitted_left_eye = true;

        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            D3D12_BOX src_box{};
            src_box.left = 0;
            src_box.top = 0;
            src_box.bottom = m_backbuffer_size[1];
            src_box.front = 0;
            src_box.back = 1;

            if (vr->is_extreme_compatibility_mode_enabled()) {
                src_box.right = m_backbuffer_size[0];
            } else {
                src_box.right = m_backbuffer_size[0] / 2;
            }

            m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &src_box);

            if (scene_depth_tex != nullptr) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
            }

            if (is_using_afw) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, otherEyeFrameBuffer.color.pTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, &src_box);
                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, otherEyeFrameBuffer.depth.pTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr);
                }
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left eye texture
        if (runtime->is_openvr()) {
            m_openvr.copy_left(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

            auto openvr = vr->get_runtime<runtimes::OpenVR>();
            const auto submit_pose = openvr->get_pose_for_submit();

            vr::D3D12TextureData_t left {
                m_openvr.get_left().texture.Get(),
                command_queue,
                0
            };
            
            vr::VRTextureWithPose_t left_eye{
                (void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                submit_pose
            };
            const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                           runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
            auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                return e;
            }
            if (is_using_afw) {
                m_openvr.copy_left_to_right(otherEyeFrameBuffer.color.pTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

                vr::D3D12TextureData_t right {
                    m_openvr.get_right().texture.Get(),
                    command_queue,
                    0
                };

                vr::VRTextureWithPose_t right_eye{
                    (void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                    submit_pose
                };
                const auto right_bounds = vr::VRTextureBounds_t{runtime->view_bounds[1][0], runtime->view_bounds[1][2],
                                                                runtime->view_bounds[1][1], runtime->view_bounds[1][3]};
                auto e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &right_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);
                runtime->frame_synced = false;

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                    return e;
                } else {
                    vr->m_submitted = true;
                }

                ++m_openvr.texture_counter;
            }
        }
    } else {
        utility::ScopeGuard __{[&]() {
            m_submitted_left_eye = false;
        }};

        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (is_actually_afr && !is_afr && !m_submitted_left_eye) {
                D3D12_BOX src_box{};
                src_box.left = 0;
                src_box.top = 0;
                src_box.bottom = m_backbuffer_size[1];
                src_box.front = 0;
                src_box.back = 1;

                if (vr->is_extreme_compatibility_mode_enabled()) {
                    src_box.right = m_backbuffer_size[0];
                } else {
                    src_box.right = m_backbuffer_size[0] / 2;
                }

                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &src_box);

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }
            }

            if (is_actually_afr) {
                D3D12_BOX src_box{};

                if (!vr->is_extreme_compatibility_mode_enabled()) {
                    if (!is_afr) {
                        src_box.left = m_backbuffer_size[0] / 2;
                        src_box.right = m_backbuffer_size[0];
                        src_box.top = 0;
                        src_box.bottom = m_backbuffer_size[1];
                        src_box.front = 0;
                        src_box.back = 1;
                    } else { // Copy the left eye on AFR
                        src_box.left = 0;
                        src_box.right = m_backbuffer_size[0] / 2;
                        src_box.top = 0;
                        src_box.bottom = m_backbuffer_size[1];
                        src_box.front = 0;
                        src_box.back = 1;
                    }   
                } else {
                    src_box.left = 0;
                    src_box.right = m_backbuffer_size[0];
                    src_box.top = 0;
                    src_box.bottom = m_backbuffer_size[1];
                    src_box.front = 0;
                    src_box.back = 1;
                }

                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &src_box);

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }

                if (is_using_afw) {
                    D3D12_BOX src_box2{.left = 0,
                        .top = 0,
                        .front = 0,
                        .right = vr->is_extreme_compatibility_mode_enabled() ? m_backbuffer_size[0] : m_backbuffer_size[0] / 2,
                        .bottom = m_backbuffer_size[1],
                        .back = 1};
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, otherEyeFrameBuffer.color.pTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, &src_box2);
                    if (scene_depth_tex != nullptr) {
                        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, otherEyeFrameBuffer.depth.pTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr);
                    }
                }
            } else {
                // Copy over the entire double wide instead
                if (m_scene_capture_tex.texture.Get() == nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
                } else {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, nullptr, pre_render, std::nullopt, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
                }

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left and right eye textures.
        if (runtime->is_openvr()) {
            auto openvr = vr->get_runtime<runtimes::OpenVR>();
            const auto submit_pose = openvr->get_pose_for_submit();

            if (!is_afr || is_using_afw) {
                if (is_using_afw)
                    m_openvr.copy_left(otherEyeFrameBuffer.color.pTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                else
                    m_openvr.copy_left(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

                vr::D3D12TextureData_t left {
                    m_openvr.get_left().texture.Get(),
                    command_queue,
                    0
                };

                vr::VRTextureWithPose_t left_eye{
                    (void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                    submit_pose
                };
                const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                               runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
                auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                    //return e; // dont return because it will just completely stop us from even getting to the right eye which could be catastrophic
                }
            }
            if (!is_afr) {
                if (m_scene_capture_tex.texture.Get() == nullptr) {
                    m_openvr.copy_right(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                } else {
                    m_openvr.copy_left_to_right(m_scene_capture_tex.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            } else {
                m_openvr.copy_left_to_right(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            }

            vr::D3D12TextureData_t right {
                m_openvr.get_right().texture.Get(),
                command_queue,
                0
            };

            vr::VRTextureWithPose_t right_eye{
                (void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                submit_pose
            };
            const auto right_bounds = vr::VRTextureBounds_t{runtime->view_bounds[1][0], runtime->view_bounds[1][2],
                                                            runtime->view_bounds[1][1], runtime->view_bounds[1][3]};
            auto e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &right_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);
            runtime->frame_synced = false;

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                return e;
            } else {
                vr->m_submitted = true;
            }

            ++m_openvr.texture_counter;
        }
    }

    if (is_right_eye_frame || is_using_afw) {
        if ((runtime->ready() && vr->get_synchronize_stage() == VR::SynchronizeStage::VERY_LATE) || !runtime->got_first_sync) {
            //vr->update_hmd_state();
        }
    }

    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (is_right_eye_frame || is_using_afw) {
        ////////////////////////////////////////////////////////////////////////////////
        // OpenXR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (!vr->m_openxr->frame_began) {
                vr->m_openxr->begin_frame();
            }

            thread_local std::vector<XrCompositionLayerBaseHeader*> quad_layers{}; quad_layers.clear();

            auto& openxr_overlay = vr->get_overlay_component().get_openxr();

            if (vr->m_2d_screen_mode->value()) {
                const auto left_layer = openxr_overlay.generate_slate_layer(runtimes::OpenXR::SwapchainIndex::UI, XrEyeVisibility::XR_EYE_VISIBILITY_LEFT);
                const auto right_layer = openxr_overlay.generate_slate_layer(runtimes::OpenXR::SwapchainIndex::UI_RIGHT, XrEyeVisibility::XR_EYE_VISIBILITY_RIGHT);

                if (left_layer && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI)) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&left_layer->get());
                }

                if (right_layer && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT)) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&right_layer->get());
                }
            } else if (m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI)) {
                const auto slate_layer = openxr_overlay.generate_slate_layer();

                if (slate_layer) {
                    quad_layers.push_back(&slate_layer->get());
                }   
            }
            
            if (m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI)) {
                const auto framework_quad = openxr_overlay.generate_framework_ui_quad();
                if (framework_quad) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&framework_quad->get());
                }
            }

            auto result = vr->m_openxr->end_frame(quad_layers, scene_depth_tex.Get() != nullptr);

            if (result == XR_ERROR_LAYER_INVALID) {
                spdlog::info("[VR] Attempting to correct invalid layer");

                m_openxr.wait_for_all_copies();

                spdlog::info("[VR] Calling xrEndFrame again");
                result = vr->m_openxr->end_frame(quad_layers);
            }

            vr->m_openxr->needs_pose_update = true;
            vr->m_submitted = result == XR_SUCCESS;
        }

        ////////////////////////////////////////////////////////////////////////////////
        // OpenVR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openvr()) {
            if (runtime->needs_pose_update) {
                vr->m_submitted = false;
                spdlog::info("[VR] Runtime needed pose update inside present (frame {})", vr->m_frame_count);
                return vr::VRCompositorError_None;
            }

            //++m_openvr.texture_counter;
        }

        // Allows the desktop window to be recorded.
        /*if (vr->m_desktop_fix->value()) {
            if (runtime->ready() && m_prev_backbuffer != backbuffer && m_prev_backbuffer != nullptr) {
                m_generic_commands[frame_count % 3].wait(INFINITE);
                m_generic_commands[frame_count % 3].copy(m_prev_backbuffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                m_generic_commands[frame_count % 3].execute();
            }
        }*/
    }

    m_prev_backbuffer = backbuffer;

    return e;
}

std::unique_ptr<DirectX::DX12::SpriteBatch> D3D12Component::setup_sprite_batch_pso(
    DXGI_FORMAT output_format, 
    std::span<const uint8_t> ps, 
    std::span<const uint8_t> vs, 
    std::optional<DirectX::SpriteBatchPipelineStateDescription> pd) 
{
    spdlog::info("[D3D12] Setting up sprite batch PSO");

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    DirectX::ResourceUploadBatch upload{ device };
    upload.Begin();

    if (!pd) {
        pd = DirectX::SpriteBatchPipelineStateDescription{DirectX::RenderTargetState{output_format, DXGI_FORMAT_UNKNOWN}};
    }

    if (ps.size() > 0) {
        pd->customPixelShader = D3D12_SHADER_BYTECODE{ps.data(), ps.size()};
    }

    if (vs.size() > 0) {
        pd->customVertexShader = D3D12_SHADER_BYTECODE{vs.data(), vs.size()};
    }

    auto batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, *pd);

    auto result = upload.End(command_queue);
    result.wait();

    spdlog::info("[D3D12] Sprite batch PSO setup complete");

    return batch;
}

void D3D12Component::draw_spectator_view(ID3D12GraphicsCommandList* command_list, bool is_right_eye_frame) {
    if (command_list == nullptr || m_game_ui_tex.texture == nullptr) {
        return;
    }

    if (m_game_ui_tex.srv_heap == nullptr || m_game_ui_tex.srv_heap->Heap() == nullptr) {
        return;
    }

    if (m_game_tex.texture == nullptr || m_game_tex.srv_heap == nullptr || m_game_tex.srv_heap->Heap() == nullptr) {
        return;
    }

    const auto& vr = VR::get();

    if (!vr->is_hmd_active() || !vr->m_desktop_fix->value()) {
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};
    const auto index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) {
        return;
    }

    if (index >= m_backbuffer_textures.size()) {
        m_backbuffer_textures.resize(index + 1);
        spdlog::info("[VR] Resized backbuffer textures to {}", index + 1);

        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx_ptr = m_backbuffer_textures[index];
    
    if (backbuffer_ctx_ptr == nullptr) {
        // if this has happened, assume the rest of the textures are also null
        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx = *backbuffer_ctx_ptr;

    const auto desc = backbuffer->GetDesc();

    if (backbuffer_ctx.texture.Get() != backbuffer.Get()) {
        if (!backbuffer_ctx.setup(device, backbuffer.Get(), std::nullopt, std::nullopt, L"Backbuffer")) {
            spdlog::error("[VR] Failed to setup backbuffer RTV (D3D12)");
            return;
        }

        spdlog::info("[VR] Created backbuffer RTV (D3D12)");
    }

    if (backbuffer_ctx.rtv_heap == nullptr || backbuffer_ctx.rtv_heap->Heap() == nullptr) {
        spdlog::error("[VR] Backbuffer RTV heap is null (D3D12)");
        return;
    }

    // Copy the previous right eye frame to the left eye frame
    const auto prev_index = (index + m_backbuffer_textures.size() - 1) % m_backbuffer_textures.size();
    if ((vr->is_using_afr()) && !is_right_eye_frame && m_backbuffer_textures[prev_index]->texture != nullptr) {
        const auto& last_right_eye_buffer = m_backbuffer_textures[prev_index]->texture;

        if (backbuffer.Get() != last_right_eye_buffer.Get()) {
            m_generic_commands[index % 3].wait(INFINITE);
            m_generic_commands[index % 3].copy(last_right_eye_buffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
            m_generic_commands[index % 3].execute();

            return;
        }
    }

    auto& batch = m_backbuffer_batch;

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)desc.Width;
    viewport.Height = (float)desc.Height;
    viewport.MaxDepth = 1.0f;
    
    batch->SetViewport(viewport);

    D3D12_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = (LONG)desc.Width;
    scissor_rect.bottom = (LONG)desc.Height;

    // Transition backbuffer to D3D12_RESOURCE_STATE_RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1, &barrier);

    // Set RTV to backbuffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_heaps[] = { backbuffer_ctx.get_rtv() };
    command_list->OMSetRenderTargets(1, rtv_heaps, FALSE, nullptr);

    // Clear backbuffer
    const float bb_clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    command_list->ClearRenderTargetView(backbuffer_ctx.get_rtv(), bb_clear_color, 0, nullptr);

    // Setup viewport and scissor rects
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor_rect);

    batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);

    RECT dest_rect{ 0, 0, (LONG)desc.Width, (LONG)desc.Height };

    const auto aspect_ratio = (float)desc.Width / (float)desc.Height;

    const auto eye_width = ((float)m_backbuffer_size[0] / 2.0f);
    const auto eye_height = (float)m_backbuffer_size[1];
    const auto eye_aspect_ratio = eye_width / eye_height;

    const auto original_centerw = (float)eye_width / 2.0f;
    const auto original_centerh = (float)eye_height / 2.0f;

    ///////////////
    // Eye (game) texture
    ///////////////
    // only show one half of the double wide texture (right side)
    RECT source_rect{};

    // Show left side when using AFR or native stereo fix
    if (vr->is_using_afr() || vr->is_native_stereo_fix_enabled()) {
        source_rect.left = 0;
        source_rect.top = 0;
        source_rect.right = m_backbuffer_size[0] / 2;
        source_rect.bottom = m_backbuffer_size[1];
    } else {
        source_rect.left = (LONG)m_backbuffer_size[0] / 2;
        source_rect.top = 0;
        source_rect.right = m_backbuffer_size[0];
        source_rect.bottom = m_backbuffer_size[1];
    }

    // Correct left/top/right/bottom to match the aspect ratio of the game
    if (eye_aspect_ratio > aspect_ratio) {
        const auto new_width = eye_height * aspect_ratio;
        const auto new_centerw = new_width / 2.0f;
        source_rect.left = (LONG)(original_centerw - new_centerw);
        source_rect.right = (LONG)(original_centerw + new_centerw);
    } else {
        const auto new_height = eye_width / aspect_ratio;
        const auto new_centerh = new_height / 2.0f;
        source_rect.top = (LONG)(original_centerh - new_centerh);
        source_rect.bottom = (LONG)(original_centerh + new_centerh);
    }

    // Set descriptor heaps
    ID3D12DescriptorHeap* game_heaps[] = { m_game_tex.srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, game_heaps);

    batch->Draw(m_game_tex.get_srv_gpu(), 
        DirectX::XMUINT2{ (uint32_t)m_backbuffer_size[0], (uint32_t)m_backbuffer_size[1] },
        dest_rect,
        &source_rect, 
        DirectX::Colors::White);

    //////
    // UI
    //////
    // Set descriptor heaps
    ID3D12DescriptorHeap* ui_heaps[] = { m_game_ui_tex.srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, ui_heaps);

    batch->Draw(m_game_ui_tex.get_srv_gpu(), 
        DirectX::XMUINT2{ (uint32_t)desc.Width, (uint32_t)desc.Height },
        dest_rect, 
        DirectX::Colors::White);

    batch->End();

    // Transition backbuffer to D3D12_RESOURCE_STATE_PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    command_list->ResourceBarrier(1, &barrier);
}

void D3D12Component::clear_backbuffer() {
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    if (device == nullptr || swapchain == nullptr) {
        return;
    }

    ComPtr<ID3D12Resource> backbuffer{};
    const auto index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) {
        return;
    }

    if (backbuffer == nullptr) {
        return;
    }

    if (index >= m_backbuffer_textures.size()) {
        m_backbuffer_textures.resize(index + 1);
        spdlog::info("[VR] Resized backbuffer textures to {}", index + 1);

        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx_ptr = m_backbuffer_textures[index];
    
    if (backbuffer_ctx_ptr == nullptr) {
        // if this has happened, assume the rest of the textures are also null
        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx = *backbuffer_ctx_ptr;

    if (backbuffer_ctx.texture.Get() != backbuffer.Get()) {
        if (!backbuffer_ctx.setup(device, backbuffer.Get(), std::nullopt, std::nullopt, L"Backbuffer")) {
            spdlog::error("[VR] Failed to setup backbuffer RTV (D3D12)");
            return;
        }

        spdlog::info("[VR] Created backbuffer RTV (D3D12)");
    }

    // oh well
    if (backbuffer_ctx.rtv_heap == nullptr || backbuffer_ctx.rtv_heap->Heap() == nullptr) {
        return;
    }

    // Clear the backbuffer
    backbuffer_ctx.commands.wait(0);
    const float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    backbuffer_ctx.commands.clear_rtv(backbuffer_ctx.texture.Get(), backbuffer_ctx.get_rtv(), clear_color, D3D12_RESOURCE_STATE_PRESENT);
    backbuffer_ctx.commands.execute();
}

void D3D12Component::on_post_present(VR* vr) {
    if (m_graphics_memory != nullptr) {
        auto& hook = g_framework->get_d3d12_hook();

        auto device = hook->get_device();
        auto command_queue = hook->get_command_queue();

        m_graphics_memory->Commit(command_queue);
    }

    // Clear the (real) backbuffer if VR is enabled. Otherwise it will flicker and all sorts of nasty things.
    // Flat 3D monitor mode: the backbuffer IS the output — never clear it.
    if (vr->is_hmd_active() && !vr->get_runtime()->is_flat3d()) {
        clear_backbuffer();
    }
}

void D3D12Component::on_reset(VR* vr) {
    m_force_reset = true;

    auto runtime = vr->get_runtime();

    for (auto& ctx : m_openvr.left_eye_tex) {
        ctx.reset();
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ctx.reset();
    }

    for (auto& commands : m_generic_commands) {
        commands.reset();
    }

    for (auto& commands : m_game_tex_commands) {
        commands.reset();
    }

    for (auto& backbuffer : m_backbuffer_textures) {
        backbuffer.reset();
    }

    for (auto & screen : m_2d_screen_tex) {
        screen.reset();
    }

    m_openvr.ui_tex.reset();
    m_game_ui_tex.reset();
    m_ui_invert_tex.reset();
    reset_ue58_converted_ui_textures();
    m_game_tex.reset();
    m_scene_capture_tex.reset();
    m_scene_capture_generation = 0;
    m_scene_capture_width = 0;
    m_scene_capture_height = 0;
    m_shf_mono_scene_tex.reset();
    m_halo_electra_quad_source_tex.reset();
    m_shf_mono_scene_commands.reset();
    m_shf_mono_scene_width = 0;
    m_shf_mono_scene_height = 0;
    m_shf_mono_scene_format = DXGI_FORMAT_UNKNOWN;
    m_dune_hmd_mono_scene_tex.reset();
    m_dune_hmd_mono_scene_commands.reset();
    m_dune_hmd_mono_scene_width = 0;
    m_dune_hmd_mono_scene_height = 0;
    m_dune_hmd_mono_scene_format = DXGI_FORMAT_UNKNOWN;
    m_skip_spectator_view_for_volatile_external_rt = false;
    m_shf_scene_mode = ShfSceneMode::Unknown;
    m_flat3d_compositor.reset();
    m_flat3d_katanga12.shutdown();
    m_backbuffer_batch.reset();
    m_game_batch.reset();
    m_ui_batch_alpha_invert.reset();
    m_graphics_memory.reset();

    if (runtime->is_openxr() && runtime->loaded) {
        m_openxr.wait_for_all_copies();

        auto& rt_pool = vr->get_render_target_pool_hook();
        ComPtr<ID3D12Resource> scene_depth_tex{rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ")};

        bool needs_depth_resize = false;

        if (scene_depth_tex != nullptr) {
            const auto desc = scene_depth_tex->GetDesc();
            needs_depth_resize = vr->m_openxr->needs_depth_resize(desc.Width, desc.Height);

            if (needs_depth_resize) {
                spdlog::info("[VR] SceneDepthZ needs resize ({}x{})", desc.Width, desc.Height);
            }
        }


        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height() ||
            vr->m_openxr->swapchains.empty() ||
            g_framework->get_d3d12_rt_size()[0] != vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::UI].width ||
            g_framework->get_d3d12_rt_size()[1] != vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::UI].height ||
            m_last_afr_state != vr->is_using_afr() ||
            needs_depth_resize)
        {
            m_openxr.create_swapchains();
            m_last_afr_state = vr->is_using_afr();
        }

        // end the frame before something terrible happens
        //vr->m_openxr.synchronize_frame();
        //vr->m_openxr.begin_frame();
        //vr->m_openxr.end_frame();
    }

    m_prev_backbuffer.Reset();
    m_openvr.texture_counter = 0;
}

bool D3D12Component::setup() {
    SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Setting up d3d12 textures...");

    auto vr = VR::get();
    on_reset(vr.get());
    
    m_prev_backbuffer.Reset();

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    auto ue4_texture = vr->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

    if (ue4_texture != nullptr) {
        backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
    }

    ComPtr<ID3D12Resource> real_backbuffer{};
    if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get real back buffer (D3D12).");
        return false;
    }

    if (vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to get back buffer (D3D12).");
        return false;
    }

    if (m_graphics_memory == nullptr) {
        m_graphics_memory = std::make_unique<DirectX::DX12::GraphicsMemory>(device);
    }

    const auto real_backbuffer_desc = real_backbuffer->GetDesc();

    auto backbuffer_desc = backbuffer->GetDesc();

    spdlog::info("[VR] D3D12 Real backbuffer width: {}, height: {}, format: {}", real_backbuffer_desc.Width, real_backbuffer_desc.Height, (uint32_t)real_backbuffer_desc.Format);

    backbuffer_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    backbuffer_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    backbuffer_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (!vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer_desc.Width /= 2; // The texture we get from UE is both eyes combined. we will copy the regions later.
    }

    spdlog::info("[VR] D3D12 RT width: {}, height: {}, format: {}", backbuffer_desc.Width, backbuffer_desc.Height, (uint32_t)backbuffer_desc.Format);

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    if (vr->is_using_2d_screen()) {
        auto screen_desc = backbuffer_desc;
        screen_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        screen_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        screen_desc.Width = (uint32_t)g_framework->get_d3d12_rt_size().x;
        screen_desc.Height = (uint32_t)g_framework->get_d3d12_rt_size().y;

        for (auto& context : m_2d_screen_tex) {
            ComPtr<ID3D12Resource> screen_tex{};
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &screen_desc, ENGINE_SRC_COLOR, nullptr,
                    IID_PPV_ARGS(&screen_tex)))) {
                spdlog::error("[VR] Failed to create 2D screen texture.");
                continue;
            }

            screen_tex->SetName(L"2D Screen Texture");

            if (!context.setup(device, screen_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"2D Screen")) {
                spdlog::error("[VR] Failed to setup 2D screen context.");
                continue;
            }
        }
    }

    // #############################
    // #Frame Warp Module Start
    // #############################
    static uint32_t lastSize[2]{0, 0};
    static DXGI_FORMAT lastFormat = DXGI_FORMAT_UNKNOWN;
    if (!vr->is_using_2d_screen() && (lastSize[0] != vr->get_hmd_width() || lastSize[1] != vr->get_hmd_height() || lastFormat != backbuffer_desc.Format)) {
        FrameWarpInitParams params = {vr->get_hmd_width(), vr->get_hmd_height(), backbuffer_desc.Format};
        spdlog::info("[VR] Before InitFrameWarp");
        m_eyeFrameBuffers = InitFrameWarp(params);
        spdlog::info("[VR] After InitFrameWarp");
        spdlog::info("[VR] m_eyeFrameBuffers[0]: {} ", (void*)m_eyeFrameBuffers.eyeFrameBuffers->color.pTexture);
        lastSize[0] = vr->get_hmd_width();
        lastSize[1] = vr->get_hmd_height();
    }
    // #############################
    // #Frame Warp Module End
    // #############################

    if (vr->get_runtime()->is_openvr()) {
        for (auto& ctx : m_openvr.left_eye_tex) {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&ctx.texture)))) {
                spdlog::error("[VR] Failed to create left eye texture.");
                return false;
            }

            ctx.texture->SetName(L"OpenVR Left Eye Texture");
            if (!ctx.commands.setup(L"OpenVR Left Eye")) {
                spdlog::error("[VR] Failed to setup left eye context.");
                return false;
            }
        }

        for (auto& ctx : m_openvr.right_eye_tex) {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&ctx.texture)))) {
                spdlog::error("[VR] Failed to create right eye texture.");
                return false;
            }

            ctx.texture->SetName(L"OpenVR Right Eye Texture");
            if (!ctx.commands.setup(L"OpenVR Right Eye")) {
                spdlog::error("[VR] Failed to setup right eye context.");
                return false;
            }
        }

        // Set up the UI texture. it's the desktop resolution.
        auto ui_desc = backbuffer_desc;
        ui_desc.Width = (uint32_t)g_framework->get_d3d12_rt_size().x;
        ui_desc.Height = (uint32_t)g_framework->get_d3d12_rt_size().y;

        ComPtr<ID3D12Resource> ui_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &ui_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&ui_tex)))) {
            spdlog::error("[VR] Failed to create UI texture.");
            return false;
        }

        ui_tex->SetName(L"OpenVR UI Texture");

        if (!m_openvr.ui_tex.setup(device, ui_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"OpenVR UI")) {
            spdlog::error("[VR] Failed to setup OpenVR UI context.");
            return false;
        }
    }

    for (auto& commands : m_generic_commands) {
        if (!commands.setup(L"Generic commands")) {
            return false;
        }
    }

    if (!vr->is_extreme_compatibility_mode_enabled()) {
        m_backbuffer_size[0] = backbuffer_desc.Width * 2;
    } else {
        m_backbuffer_size[0] = backbuffer_desc.Width;
    }

    m_backbuffer_size[1] = backbuffer_desc.Height;

    m_backbuffer_batch = setup_sprite_batch_pso(real_backbuffer_desc.Format);
    m_game_batch = setup_sprite_batch_pso(backbuffer_desc.Format);

    // Alpha-invert PSO. Renders the UI texture into a SEPARATE off-screen
    // target (m_ui_invert_tex) — never in-place — so an overwrite blend is
    // correct: the pixel shader already outputs the final alpha
    // (lerp(a, 1-a, invertAmount)), and there is no valid destination alpha to
    // blend against. (The old in-place path used a BLEND_FACTOR alpha blend
    // that read the destination — i.e. the source itself — and required the
    // illegal SRV==RTV self-bind that crashed FF7 Rebirth.)
    {
        // The UI is viewed/composited as B8G8R8A8_UNORM (see m_game_ui_tex and
        // m_ui_invert_tex), NOT the backbuffer format — building this PSO for
        // the backbuffer format mismatched the render target on titles whose
        // backbuffer isn't B8G8R8A8 (FF7 Rebirth = R10G10B10A2) and removed the
        // device.
        DirectX::SpriteBatchPipelineStateDescription invert_alpha_in_place_pd{DirectX::RenderTargetState{DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_UNKNOWN}};

        auto& bd = invert_alpha_in_place_pd.blendDesc;
        auto& bdrt = bd.RenderTarget[0];
        bdrt.BlendEnable = TRUE;

        bdrt.SrcBlend = D3D12_BLEND_ONE;
        bdrt.DestBlend = D3D12_BLEND_ZERO;
        bdrt.BlendOp = D3D12_BLEND_OP_ADD;

        bdrt.SrcBlendAlpha = D3D12_BLEND_ONE;
        bdrt.DestBlendAlpha = D3D12_BLEND_ZERO;
        bdrt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bdrt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        m_ui_batch_alpha_invert = setup_sprite_batch_pso(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            alpha_luminance_sprite_ps_SpritePixelShader,
            alpha_luminance_sprite_ps_SpriteVertexShader,
            invert_alpha_in_place_pd
        );
    }

    spdlog::info("[VR] d3d12 textures have been setup");
    m_force_reset = false;

    return true;
}

void D3D12Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    std::scoped_lock _{this->mtx};

	auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();

    this->binding.device = device;
    this->binding.queue = command_queue;

    spdlog::info("[VR] Searching for xrGetD3D12GraphicsRequirementsKHR...");
    PFN_xrGetD3D12GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    XrGraphicsRequirementsD3D12KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    gr.adapterLuid = device->GetAdapterLuid();
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    spdlog::info("[VR] Calling xrGetD3D12GraphicsRequirementsKHR");
    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D12Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D12");

    this->destroy_swapchains();
    
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    auto vr = VR::get();
    bool has_actual_vr_backbuffer = false;

    if (vr != nullptr && vr->m_fake_stereo_hook != nullptr) {
        auto ue4_texture = vr->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

        if (ue4_texture != nullptr) {
            backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
            has_actual_vr_backbuffer = backbuffer != nullptr;
        }
    }
    
    // Get the existing backbuffer
    // so we can get the format and stuff.
    if (backbuffer == nullptr && FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    auto backbuffer_desc = backbuffer->GetDesc();
    auto& openxr = vr->m_openxr;

    this->contexts.clear();

    auto create_swapchain = [&](uint32_t i, const XrSwapchainCreateInfo& swapchain_create_info, const D3D12_RESOURCE_DESC& desc) -> std::optional<std::string> {
        // Create the swapchain.
        runtimes::OpenXR::Swapchain swapchain{};
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;

        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            spdlog::error("[VR] D3D12: Failed to create swapchain.");
            return "Failed to create swapchain.";
        }

        vr->m_openxr->swapchains[i] = swapchain;

        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images.");
            return "Failed to enumerate swapchain images.";
        }

        SPDLOG_INFO("[VR] Runtime wants {} images for swapchain {}", image_count, i);

        auto& ctx = this->contexts[i];

        ctx.textures.clear();
        ctx.textures.resize(image_count);
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR commands "} + std::to_wstring(i) + L" " + std::to_wstring(j)).c_str());
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);
        
        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images after texture creation.");
            return "Failed to enumerate swapchain images after texture creation.";
        }

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j].texture->AddRef();
            const auto ref_count = ctx.textures[j].texture->Release();

            spdlog::info("[VR] AFTER Swapchain texture {} {} ref count: {}", i, j, ref_count);
        }

        if (swapchain_create_info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) {
            for (uint32_t j = 0; j < image_count; ++j) {
                XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait_info.timeout = XR_INFINITE_DURATION;
                XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

                uint32_t index{};
                xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &index);
                xrWaitSwapchainImage(swapchain.handle, &wait_info);

                auto& texture_ctx = ctx.texture_contexts[index];
                texture_ctx->texture = ctx.textures[index].texture;

                // Depth stencil textures don't need an RTV.
                if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
                    if (ctx.texture_contexts[index]->create_rtv(device, (DXGI_FORMAT)swapchain_create_info.format)) {
                        const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        texture_ctx->commands.clear_rtv(ctx.textures[index].texture, texture_ctx->get_rtv(), clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
                        texture_ctx->commands.execute();
                        texture_ctx->commands.wait(100);
                    } else {
                        spdlog::error("[VR] Failed to create RTV for swapchain image {}.", index);
                    }
                }

                texture_ctx->texture.Reset();
                texture_ctx->rtv_heap.reset();

                xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }
        }

        return std::nullopt;
    };

    const auto double_wide_multiple = vr->is_using_afr() ? 1 : 2;

    XrSwapchainCreateInfo standard_swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    standard_swapchain_create_info.arraySize = 1;
    standard_swapchain_create_info.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    standard_swapchain_create_info.width = vr->get_hmd_width() * double_wide_multiple;
    standard_swapchain_create_info.height = vr->get_hmd_height();
    standard_swapchain_create_info.mipCount = 1;
    standard_swapchain_create_info.faceCount = 1;
    standard_swapchain_create_info.sampleCount = backbuffer_desc.SampleDesc.Count;
    standard_swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

    auto hmd_desc = backbuffer_desc;
    hmd_desc.Width = vr->get_hmd_width() * double_wide_multiple;
    hmd_desc.Height = vr->get_hmd_height();
    hmd_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    hmd_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    hmd_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    // Above is outdated, we will just use a double wide texture
    if (!vr->is_using_afr()) {
        spdlog::info("[VR] Creating double wide swapchain for eyes");
        spdlog::info("[VR] Width: {}", vr->get_hmd_width() * 2);
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    } else {
        spdlog::info("[VR] Creating AFR swapchain for eyes");
        spdlog::info("[VR] Width: {}", vr->get_hmd_width());
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        spdlog::info("[VR] Creating AFR left eye swapchain");
        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }

        spdlog::info("[VR] Creating AFR right eye swapchain");
        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    }

    auto virtual_desktop_dummy_desc = backbuffer_desc;
    auto virtual_desktop_dummy_swapchain_create_info = standard_swapchain_create_info;

    virtual_desktop_dummy_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    virtual_desktop_dummy_desc.Width = 4;
    virtual_desktop_dummy_desc.Height = 4;
    virtual_desktop_dummy_swapchain_create_info.width = 4;
    virtual_desktop_dummy_swapchain_create_info.height = 4;
    virtual_desktop_dummy_swapchain_create_info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT; // so we dont need to acquire/release/wait

    // The virtual desktop dummy texture
    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DUMMY_VIRTUAL_DESKTOP, virtual_desktop_dummy_swapchain_create_info, virtual_desktop_dummy_desc)) {
        return err;
    }

    auto desktop_rt_swapchain_create_info = standard_swapchain_create_info;
    desktop_rt_swapchain_create_info.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desktop_rt_swapchain_create_info.width = g_framework->get_d3d12_rt_size().x;
    desktop_rt_swapchain_create_info.height = g_framework->get_d3d12_rt_size().y;

    auto desktop_rt_desc = backbuffer_desc;
    desktop_rt_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desktop_rt_desc.Width = g_framework->get_d3d12_rt_size().x;
    desktop_rt_desc.Height = g_framework->get_d3d12_rt_size().y;

    desktop_rt_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    desktop_rt_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    // The UI texture
    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    // Depth textures
    if (vr->get_openxr_runtime()->is_depth_allowed()) {
        // Even when using AFR, the depth tex is always the size of a double wide.
        // That's kind of unfortunate in terms of how many copies we have to do but whatever.
        auto depth_swapchain_create_info = standard_swapchain_create_info;
        depth_swapchain_create_info.format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        depth_swapchain_create_info.createFlags = 0;
        depth_swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        depth_swapchain_create_info.width = vr->get_hmd_width() * 2;
        depth_swapchain_create_info.height = vr->get_hmd_height();

        auto depth_desc = backbuffer_desc;
        depth_desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        //depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth_desc.DepthOrArraySize = 1;

        depth_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        depth_desc.Width = vr->get_hmd_width() * 2;
        depth_desc.Height = vr->get_hmd_height();

        auto& rt_pool = vr->get_render_target_pool_hook();
        auto depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");

        if (depth_tex != nullptr) {
            this->made_depth_with_null_defaults = false;
            depth_desc = depth_tex->GetDesc();

            if (depth_desc.Format == DXGI_FORMAT_R24G8_TYPELESS) {
                depth_swapchain_create_info.format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            }

            spdlog::info("[VR] Depth texture size: {}x{}", depth_desc.Width, depth_desc.Height);
            spdlog::info("[VR] Depth texture format: {}", (uint32_t)depth_desc.Format);
            spdlog::info("[VR] Depth texture flags: {}", (uint32_t)depth_desc.Flags);

            if (depth_desc.Width > hmd_desc.Width || depth_desc.Height > hmd_desc.Height) {
                spdlog::info("[VR] Depth texture is larger than the HMD");
                //depth_desc.Width = hmd_desc.Width;
                //depth_desc.Height = hmd_desc.Height;
            }

            depth_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            depth_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

            depth_swapchain_create_info.width = depth_desc.Width;
            depth_swapchain_create_info.height = depth_desc.Height;
        } else {
            this->made_depth_with_null_defaults = true;
            spdlog::error("[VR] Depth texture is null! Using default values");
            depth_desc.Width = vr->get_hmd_width() * 2;
            depth_desc.Height = vr->get_hmd_height();
        }

        if (!vr->is_using_afr()) {
            spdlog::info("[VR] Creating double wide depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH, depth_swapchain_create_info, depth_desc)) {
                return err;
            }
        } else {
            spdlog::info("[VR] Creating AFR depth swapchain");
            spdlog::info("[VR] Creating AFR left eye depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, depth_swapchain_create_info, depth_desc)) {
                return err;
            }

            spdlog::info("[VR] Creating AFR right eye depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, depth_swapchain_create_info, depth_desc)) {
                return err;
            }
        }
    }

    this->last_resolution = {vr->get_hmd_width(), vr->get_hmd_height()};

    return std::nullopt;
}

void D3D12Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};

    if (this->contexts.empty()) {
        return;
    }
    
    auto& vr = VR::get();
    std::scoped_lock __{vr->m_openxr->swapchain_mtx};

    spdlog::info("[VR] Destroying swapchains.");

    this->wait_for_all_copies();

    for (auto& it : this->contexts) {
        auto& ctx = it.second;
        const auto i = it.first;

        //ctx.texture_contexts.clear();
        for (auto& texture_context : ctx.texture_contexts) {
            if (texture_context != nullptr) {
                texture_context->reset();
            }
        }

        ctx.texture_contexts.clear();

        std::vector<ID3D12Resource*> needs_release{};

        for (auto& tex : ctx.textures) {
            if (tex.texture != nullptr) {
                tex.texture->AddRef();
                needs_release.push_back(tex.texture);
            }
        }

        if (vr->m_openxr->swapchains.contains(i)) {
            const auto result = xrDestroySwapchain(vr->m_openxr->swapchains[i].handle);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to destroy swapchain {}.", i);
            } else {
                spdlog::info("[VR] Destroyed swapchain {}.", i);
            }
        } else {
            spdlog::error("[VR] Swapchain {} does not exist.", i);
        }

        for (auto& tex : needs_release) {
            if (const auto ref_count = tex->Release(); ref_count != 0) {
                spdlog::info("[VR] Memory leak detected in swapchain texture {} ({} refs)", i, ref_count);
            } else {
                spdlog::info("[VR] Swapchain texture {} released.", i);
            }
        }
        
        ctx.textures.clear();
    }

    this->contexts.clear();
    vr->m_openxr->swapchains.clear();
}

void D3D12Component::OpenXR::copy(
    uint32_t swapchain_idx, 
    ID3D12Resource* resource, 
    std::optional<std::function<void(d3d12::CommandContext&, ID3D12Resource*)>> pre_commands, 
    std::optional<std::function<void(d3d12::CommandContext&)>> additional_commands, 
    D3D12_RESOURCE_STATES src_state, 
    D3D12_BOX* src_box) 
{
    std::scoped_lock _{this->mtx};

    auto vr = VR::get();

    if (vr->m_openxr->frame_state.shouldRender != XR_TRUE) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        if (vr->get_synchronize_stage() != VR::SynchronizeStage::VERY_LATE) {
            spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
            return;
        }
    }

    if (!this->contexts.contains(swapchain_idx)) {
        spdlog::error("[VR] OpenXR: Trying to copy to swapchain {} but it doesn't exist.", swapchain_idx);
        return;
    }

    if (!vr->m_openxr->swapchains.contains(swapchain_idx)) {
        spdlog::error("[VR] OpenXR: Trying to copy to swapchain {} but it doesn't exist.", swapchain_idx);
        return;
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx);
    }

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result == XR_ERROR_RUNTIME_FAILURE) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        spdlog::info("[VR] Attempting to correct...");

        for (auto& texture_ctx : ctx.texture_contexts) {
            texture_ctx->commands.reset();
        }

        texture_index = 0;
        result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);
    }


    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        } else {
            auto& texture_ctx = ctx.texture_contexts[texture_index];
            texture_ctx->commands.wait(INFINITE);

            if (pre_commands) {
                (*pre_commands)(texture_ctx->commands, ctx.textures[texture_index].texture);
            }

            // We may simply just want to render to the render target directly
            // hence, a null resource is allowed.
            if (resource != nullptr) {
                if (src_box == nullptr) {
                    const auto is_depth = swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH || 
                                        swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE || 
                                        swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE;
                    const auto dst_state = is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;

                    texture_ctx->commands.copy(
                        resource, 
                        ctx.textures[texture_index].texture, 
                        src_state, 
                        dst_state);
                } else {
                    texture_ctx->commands.copy_region(
                        resource, 
                        ctx.textures[texture_index].texture, src_box,
                        src_state, 
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            }

            if (additional_commands) {
                (*additional_commands)(texture_ctx->commands);
            }

            texture_ctx->commands.execute();

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            // SteamVR shenanigans.
            if (result == XR_ERROR_RUNTIME_FAILURE) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                spdlog::info("[VR] Attempting to correct...");

                result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

                if (result != XR_SUCCESS) {
                    spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                }

                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }

                result = xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                return;
            }

            ctx.num_textures_acquired--;
            ctx.ever_acquired = true;
        }
    }
}
} // namespace vrmod