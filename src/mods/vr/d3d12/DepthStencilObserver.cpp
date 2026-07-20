// DepthStencilCaptureObserver — API-level scene-depth capture for Flat3D's
// "DSV Observer" depth source. Extracted verbatim from the former DIBR
// depth-capture path (the DIBR synthesis half was dropped). Watches DSV
// creation and resource barriers via the D3D12Hook observer and makes a
// self-contained shader-readable copy of the matching scene depth.

#include "DepthStencilObserver.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <spdlog/spdlog.h>

namespace {
uint32_t dsv_array_slice(const D3D12_DEPTH_STENCIL_VIEW_DESC* desc) {
    if (desc == nullptr) {
        return 0;
    }

    switch (desc->ViewDimension) {
    case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
        return desc->Texture2DArray.FirstArraySlice;
    case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
        return desc->Texture2DMSArray.FirstArraySlice;
    default:
        return 0;
    }
}

constexpr auto CAPTURED_DEPTH_SHADER_READ =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// UE commonly finishes depth in more than one write pass (prepass + base pass).
// Each opening-window capture is a full depth copy, so bound the refreshes
// between consumer re-arms; the last one before Present wins.
constexpr uint32_t CAPTURE_OPENING_BUDGET_PER_FRAME = 4;
} // namespace

namespace d3d12 {
DepthStencilCaptureObserver::~DepthStencilCaptureObserver() {
    reset();
}

void DepthStencilCaptureObserver::reset() {
    {
        std::scoped_lock _{m_capture_mutex};
        // reset() only runs after the owning component has drained any consumer
        // (or at destruction), so the retired copies can be freed here.
        m_captured_depth.Reset();
        m_retired_captured_depths.clear();
        m_captured_depth_width = 0;
        m_captured_depth_height = 0;
        m_captured_depth_format = DXGI_FORMAT_UNKNOWN;
        m_captured_depth_generation = 0;
        m_capture_success_logged = false;
        m_capture_failure_logged = false;
        m_capture_failure_reason.clear();
    }
    m_capture_opening_budget.store(0, std::memory_order_release);
    m_opening_capture_seen.store(false, std::memory_order_release);
    {
        std::scoped_lock _{m_trace_mutex};
        m_traced_depth_resources.clear();
        m_depth_trace_summary = "waiting for a DSV candidate";
        m_depth_trace_expected_width = 0;
        m_depth_trace_expected_height = 0;
        m_barrier_discovery_logged = false;
    }
    m_depth_trace_candidate.store(0, std::memory_order_release);
    m_depth_trace_candidate_array_slice.store(0, std::memory_order_release);
    m_depth_trace_last_state.store(D3D12_RESOURCE_STATE_COMMON, std::memory_order_release);
    m_ue5_rdg_depth_capture_enabled.store(false, std::memory_order_release);
    m_ue5_rdg_depth_capture_requested.store(false, std::memory_order_release);
}

std::string DepthStencilCaptureObserver::depth_trace_summary() const {
    std::scoped_lock _{m_trace_mutex};
    return m_depth_trace_summary;
}

void DepthStencilCaptureObserver::set_depth_trace_expected_extent(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }

    bool changed{};
    {
        std::scoped_lock _{m_trace_mutex};
        if (m_depth_trace_expected_width == width && m_depth_trace_expected_height == height) {
            return;
        }

        m_depth_trace_expected_width = width;
        m_depth_trace_expected_height = height;
        m_barrier_discovery_logged = false;
        refresh_depth_trace_candidate_locked();
        changed = true;
    }

    if (changed) {
        // A resize invalidates both the selected DSV shape and the persistent
        // copy. Do not reuse a previous map's depth after a resize.
        std::scoped_lock _{m_capture_mutex};
        retire_captured_depth_locked();
        m_captured_depth_width = 0;
        m_captured_depth_height = 0;
        m_captured_depth_format = DXGI_FORMAT_UNKNOWN;
        m_captured_depth_generation = 0;
        m_capture_success_logged = false;
        m_capture_failure_logged = false;
        m_capture_failure_reason.clear();
        m_ue5_rdg_depth_capture_requested.store(
            m_ue5_rdg_depth_capture_enabled.load(std::memory_order_acquire),
            std::memory_order_release);
        // A new map/mode can change the depth transition shape; re-learn
        // whether same-frame opening windows exist for this source.
        m_capture_opening_budget.store(0, std::memory_order_release);
        m_opening_capture_seen.store(false, std::memory_order_release);
        SPDLOG_INFO("[DepthObserver][RDG trace] presentation source is {}x{}; accepting matching full-size or per-eye DSV depth", width, height);
    }
}

void DepthStencilCaptureObserver::set_ue5_rdg_depth_capture_enabled(bool enabled) {
    const auto previous = m_ue5_rdg_depth_capture_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (previous == enabled) {
        return;
    }

    if (!enabled) {
        m_ue5_rdg_depth_capture_requested.store(false, std::memory_order_release);
        m_capture_opening_budget.store(0, std::memory_order_release);
        m_opening_capture_seen.store(false, std::memory_order_release);
        std::scoped_lock _{m_capture_mutex};
        retire_captured_depth_locked();
        m_captured_depth_width = 0;
        m_captured_depth_height = 0;
        m_captured_depth_format = DXGI_FORMAT_UNKNOWN;
        m_captured_depth_generation = 0;
        m_capture_success_logged = false;
        m_capture_failure_logged = false;
        m_capture_failure_reason.clear();
        SPDLOG_INFO("[DepthObserver][DSV/RDG capture] disabled; released the owned depth copy");
        return;
    }

    m_ue5_rdg_depth_capture_requested.store(true, std::memory_order_release);
    m_capture_opening_budget.store(CAPTURE_OPENING_BUDGET_PER_FRAME, std::memory_order_release);
    SPDLOG_INFO("[DepthObserver][DSV/RDG capture] enabled; pacing capture to one verified depth copy per re-arm");
}

void DepthStencilCaptureObserver::request_ue5_rdg_depth_capture() {
    if (m_ue5_rdg_depth_capture_enabled.load(std::memory_order_acquire)) {
        m_ue5_rdg_depth_capture_requested.store(true, std::memory_order_release);
        m_capture_opening_budget.store(CAPTURE_OPENING_BUDGET_PER_FRAME, std::memory_order_release);
    }
}

void DepthStencilCaptureObserver::retire_captured_depth_locked() {
    if (m_captured_depth != nullptr) {
        m_retired_captured_depths.emplace_back(std::move(m_captured_depth));
    }
}

Microsoft::WRL::ComPtr<ID3D12Resource> DepthStencilCaptureObserver::captured_depth_snapshot() const {
    std::scoped_lock _{m_capture_mutex};
    return m_captured_depth;
}

bool DepthStencilCaptureObserver::has_captured_depth() const {
    std::scoped_lock _{m_capture_mutex};
    return m_captured_depth != nullptr;
}

void DepthStencilCaptureObserver::refresh_depth_trace_candidate_locked() {
    // Once a live barrier has identified the rotating RDG depth pool, its
    // transition is more authoritative than later descriptor creation. Keep
    // the UI summary stable and let the barrier callback follow the active
    // resource internally.
    if (m_barrier_discovery_logged) {
        return;
    }

    uintptr_t selected{};
    const DepthTraceCandidate* selected_candidate{};
    enum class MatchKind : uint8_t {
        None,
        Fallback,
        Eye,
        Full,
    } match_kind{MatchKind::None};
    uint64_t fallback_area{};
    const auto expected_eye_width = m_depth_trace_expected_width / 2;

    for (const auto& [resource, candidate] : m_traced_depth_resources) {
        if (candidate.sample_count != 1 ||
            depth_srv_format(candidate.resource_format) == DXGI_FORMAT_UNKNOWN) {
            continue;
        }

        if (candidate.width == m_depth_trace_expected_width && candidate.height == m_depth_trace_expected_height) {
            selected = resource;
            selected_candidate = &candidate;
            match_kind = MatchKind::Full;
            break;
        }

        // UE5.4+ RDG commonly keeps SceneDepthZ as a two-slice texture array:
        // the scene colour is packed stereo, but each DSV is one eye wide.
        // That is the intended depth source, not an arbitrary fallback.
        if (expected_eye_width != 0 && candidate.width == expected_eye_width &&
            candidate.height == m_depth_trace_expected_height && candidate.array_size >= 1) {
            if (match_kind != MatchKind::Eye || candidate.array_size > selected_candidate->array_size) {
                selected = resource;
                selected_candidate = &candidate;
                match_kind = MatchKind::Eye;
            }
            continue;
        }

        const auto area = candidate.width * candidate.height;
        if (match_kind == MatchKind::None && area > fallback_area) {
            fallback_area = area;
            selected = resource;
            selected_candidate = &candidate;
            match_kind = MatchKind::Fallback;
        }
    }

    const auto previous = m_depth_trace_candidate.exchange(selected, std::memory_order_acq_rel);
    if (selected == 0) {
        m_depth_trace_summary = "no exact DSV match for scene " +
            std::to_string(m_depth_trace_expected_width) + "x" +
            std::to_string(m_depth_trace_expected_height);
        return;
    }

    if (selected != previous && selected_candidate != nullptr) {
        m_depth_trace_candidate_array_slice.store(selected_candidate->array_slice, std::memory_order_release);
        m_ue5_rdg_depth_capture_requested.store(
            m_ue5_rdg_depth_capture_enabled.load(std::memory_order_acquire),
            std::memory_order_release);
        std::ostringstream summary{};
        summary << "selected DSV 0x" << std::hex << std::uppercase << selected << std::dec
                << " for scene " << m_depth_trace_expected_width << "x" << m_depth_trace_expected_height
                << (match_kind == MatchKind::Full ? " (full-size exact)" :
                    match_kind == MatchKind::Eye ? " (per-eye exact)" :
                    " (largest fallback; not adoptable yet)")
                << " candidate=" << selected_candidate->width << "x" << selected_candidate->height
                << " format=" << static_cast<uint32_t>(selected_candidate->resource_format)
                << " view_format=" << static_cast<uint32_t>(selected_candidate->view_format)
                << " array=" << selected_candidate->array_size
                << " slice=" << selected_candidate->array_slice
                << "; waiting for final resource state";
        m_depth_trace_summary = summary.str();
        m_depth_trace_last_state.store(D3D12_RESOURCE_STATE_COMMON, std::memory_order_release);
        SPDLOG_INFO("[DepthObserver][RDG trace] {}", m_depth_trace_summary);
    }
}

bool DepthStencilCaptureObserver::is_trace_candidate_compatible_locked(uintptr_t resource) const {
    const auto it = m_traced_depth_resources.find(resource);
    if (it == m_traced_depth_resources.end() ||
        it->second.sample_count != 1 ||
        depth_srv_format(it->second.resource_format) == DXGI_FORMAT_UNKNOWN ||
        m_depth_trace_expected_width == 0 ||
        m_depth_trace_expected_height == 0) {
        return false;
    }

    const auto expected_eye_width = m_depth_trace_expected_width / 2;
    return it->second.height == m_depth_trace_expected_height &&
        (it->second.width == m_depth_trace_expected_width ||
         (expected_eye_width != 0 && it->second.width == expected_eye_width));
}

bool DepthStencilCaptureObserver::try_adopt_depth_candidate_from_barrier(
    ID3D12Resource* resource,
    UINT transition_subresource,
    uint32_t& selected_array_slice)
{
    if (resource == nullptr) {
        return false;
    }

    const auto resource_desc = resource->GetDesc();
    if (resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        resource_desc.Width == 0 || resource_desc.Height == 0 ||
        resource_desc.SampleDesc.Count != 1 || resource_desc.MipLevels != 1 ||
        resource_desc.DepthOrArraySize == 0 ||
        depth_srv_format(resource_desc.Format) == DXGI_FORMAT_UNKNOWN ||
        (resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
        return false;
    }

    uint32_t array_slice{};
    if (transition_subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        const auto plane_zero_subresource_count =
            static_cast<UINT>(resource_desc.MipLevels) * resource_desc.DepthOrArraySize;
        if (transition_subresource >= plane_zero_subresource_count) {
            return false;
        }

        array_slice = transition_subresource / resource_desc.MipLevels;
        // The consumer samples the left-eye scene color. Wait for the matching
        // left depth slice instead of silently pairing it with the right eye.
        if (array_slice != 0) {
            return false;
        }
    }

    const auto resource_key = reinterpret_cast<uintptr_t>(resource);
    bool log_discovery{};
    std::string summary{};
    {
        std::scoped_lock _{m_trace_mutex};
        if (m_depth_trace_expected_width == 0 || m_depth_trace_expected_height == 0 ||
            resource_desc.Height != m_depth_trace_expected_height) {
            return false;
        }

        const auto expected_eye_width = m_depth_trace_expected_width / 2;
        const auto full_size = resource_desc.Width == m_depth_trace_expected_width;
        const auto per_eye =
            expected_eye_width != 0 && resource_desc.Width == expected_eye_width;
        if (!full_size && !per_eye) {
            return false;
        }

        auto& candidate = m_traced_depth_resources[resource_key];
        candidate = DepthTraceCandidate{
            .resource = resource_key,
            .width = resource_desc.Width,
            .height = resource_desc.Height,
            .resource_format = resource_desc.Format,
            .flags = resource_desc.Flags,
            .sample_count = resource_desc.SampleDesc.Count,
            .view_format = DXGI_FORMAT_UNKNOWN,
            .array_size = resource_desc.DepthOrArraySize,
            .array_slice = array_slice,
        };

        m_depth_trace_candidate.store(resource_key, std::memory_order_release);
        m_depth_trace_candidate_array_slice.store(array_slice, std::memory_order_release);
        m_depth_trace_last_state.store(
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            std::memory_order_release);

        if (!m_barrier_discovery_logged) {
            std::ostringstream stream{};
            stream << "adopted live barrier depth 0x" << std::hex << std::uppercase
                   << resource_key << std::dec
                   << " for scene " << m_depth_trace_expected_width << "x"
                   << m_depth_trace_expected_height
                   << (full_size ? " (full-size exact)" : " (per-eye exact)")
                   << " candidate=" << resource_desc.Width << "x"
                   << resource_desc.Height
                   << " format=" << static_cast<uint32_t>(resource_desc.Format)
                   << " array=" << resource_desc.DepthOrArraySize
                   << " slice=" << array_slice
                   << "; discovered from NPSR -> DEPTH_WRITE";
            summary = stream.str();
            m_depth_trace_summary =
                "live barrier depth pool active for scene " +
                std::to_string(m_depth_trace_expected_width) + "x" +
                std::to_string(m_depth_trace_expected_height) +
                (full_size ? " (full-size exact)" : " (per-eye exact)") +
                "; following the current RDG resource";
            m_barrier_discovery_logged = true;
            log_discovery = true;
        }
    }

    selected_array_slice = array_slice;
    if (log_discovery) {
        SPDLOG_INFO("[DepthObserver][barrier discovery] {}", summary);
    }
    return true;
}

void DepthStencilCaptureObserver::on_depth_stencil_view_created(
    ID3D12Resource* resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
    if (resource == nullptr || descriptor.ptr == 0) {
        return;
    }

    const auto resource_desc = resource->GetDesc();
    if (resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        resource_desc.Width == 0 || resource_desc.Height == 0) {
        return;
    }

    const auto resource_key = reinterpret_cast<uintptr_t>(resource);
    std::scoped_lock _{m_trace_mutex};
    const auto view_slice = dsv_array_slice(desc);
    if (const auto existing = m_traced_depth_resources.find(resource_key); existing != m_traced_depth_resources.end()) {
        existing->second.array_slice = (std::min)(existing->second.array_slice, view_slice);
        if (m_depth_trace_candidate.load(std::memory_order_acquire) == resource_key) {
            m_depth_trace_candidate_array_slice.store(existing->second.array_slice, std::memory_order_release);
        }
        return;
    }

    m_traced_depth_resources.emplace(
        resource_key,
        DepthTraceCandidate{
            .resource = resource_key,
            .width = resource_desc.Width,
            .height = resource_desc.Height,
            .resource_format = resource_desc.Format,
            .flags = resource_desc.Flags,
            .sample_count = resource_desc.SampleDesc.Count,
            .view_format = desc != nullptr ? desc->Format : DXGI_FORMAT_UNKNOWN,
            .array_size = resource_desc.DepthOrArraySize,
            .array_slice = view_slice,
        });
    refresh_depth_trace_candidate_locked();

    // The DSV hook sees every shadow, reflection and scene-capture target.
    // Only announce candidates that could actually match the presentation
    // source; this keeps a moving camera from turning the diagnostic into a
    // stream of unrelated texture sizes.
    if (!is_trace_candidate_compatible_locked(resource_key)) {
        return;
    }

    std::ostringstream summary{};
    summary << "DSV 0x" << std::hex << std::uppercase << resource_key << std::dec
            << " " << resource_desc.Width << "x" << resource_desc.Height
            << " format=" << static_cast<uint32_t>(resource_desc.Format)
            << " flags=0x" << std::hex << std::uppercase << resource_desc.Flags << std::dec
            << " samples=" << resource_desc.SampleDesc.Count
            << " array=" << resource_desc.DepthOrArraySize;
    if (desc != nullptr) {
        summary << " view_format=" << static_cast<uint32_t>(desc->Format)
                << " view_dimension=" << static_cast<uint32_t>(desc->ViewDimension)
                << " slice=" << view_slice;
    }

    const auto trace = summary.str();
    m_depth_trace_summary = trace;
    SPDLOG_INFO("[DepthObserver][RDG trace] {} handle=0x{:X}", trace, descriptor.ptr);
}

bool DepthStencilCaptureObserver::ensure_captured_depth_locked(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& source_desc,
    std::string& reason)
{
    if (device == nullptr || source_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        source_desc.Width == 0 || source_desc.Height == 0 ||
        source_desc.Width > 32768 || source_desc.Height > 32768 ||
        source_desc.SampleDesc.Count != 1 || source_desc.DepthOrArraySize == 0 ||
        source_desc.MipLevels != 1 || depth_srv_format(source_desc.Format) == DXGI_FORMAT_UNKNOWN) {
        reason = "selected DSV resource is not a single-mip non-MSAA shader-readable depth texture";
        return false;
    }

    if (m_captured_depth != nullptr &&
        m_captured_depth_width == source_desc.Width &&
        m_captured_depth_height == source_desc.Height &&
        m_captured_depth_format == source_desc.Format) {
        return true;
    }

    retire_captured_depth_locked();
    m_captured_depth_width = 0;
    m_captured_depth_height = 0;
    m_captured_depth_format = DXGI_FORMAT_UNKNOWN;
    m_captured_depth_generation = 0;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    // CopyResource only needs a compatible typeless texture. Removing the
    // depth-stencil flag lets the persistent copy expose an SRV later, while
    // the game-owned source keeps its original DSV and state untouched.
    auto destination_desc = source_desc;
    // The source may be a two-eye texture array. The observer needs one selected eye
    // as a normal Texture2D, so its persistent copy always owns one slice.
    destination_desc.DepthOrArraySize = 1;
    destination_desc.Flags &= ~(
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
        D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    destination_desc.Alignment = 0;

    ComPtr captured{};
    const auto create_result = device->CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &destination_desc,
            CAPTURED_DEPTH_SHADER_READ,
            nullptr,
            IID_PPV_ARGS(&captured));
    if (FAILED(create_result)) {
        char text[64]{};
        sprintf_s(text, "CreateCommittedResource failed 0x%08X", static_cast<uint32_t>(create_result));
        reason = text;
        return false;
    }

    captured->SetName(L"Flat3D DSV/RDG Depth Copy");
    m_captured_depth = std::move(captured);
    m_captured_depth_width = source_desc.Width;
    m_captured_depth_height = source_desc.Height;
    m_captured_depth_format = source_desc.Format;
    return true;
}

bool DepthStencilCaptureObserver::capture_depth_before_restore_locked(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_state,
    uint32_t source_array_slice,
    UINT source_transition_subresource)
{
    // Same read-window predicate as on_resource_barriers: UE5 RDG hands us a
    // bare NPSR state while UE4 closes its window from the combined
    // DEPTH_READ | PSR | NPSR read mask. Both transition cleanly through
    // COPY_SOURCE and restore symmetrically.
    constexpr auto capture_shader_read_bits =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto capture_read_only_bits = capture_shader_read_bits |
        D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_COPY_SOURCE;
    // An opening-window capture arrives with the source still in DEPTH_WRITE
    // (the engine's write pass just ended); the same COPY_SOURCE round trip
    // restores it before the original DEPTH_WRITE -> read barrier runs.
    const auto source_is_depth_write = source_state == D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (command_list == nullptr || source == nullptr ||
        (!source_is_depth_write &&
            ((source_state & capture_shader_read_bits) == 0 ||
                (source_state & ~capture_read_only_bits) != 0))) {
        m_capture_failure_reason = "capture callback did not receive the expected shader-read or depth-write command-list state";
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    const auto device_result = source->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(device_result) || device == nullptr) {
        char text[64]{};
        sprintf_s(text, "ID3D12Resource::GetDevice failed 0x%08X", static_cast<uint32_t>(device_result));
        m_capture_failure_reason = text;
        return false;
    }

    const auto source_desc = source->GetDesc();
    if (source_array_slice >= source_desc.DepthOrArraySize) {
        m_capture_failure_reason = "selected DSV array slice is outside the source resource";
        return false;
    }

    // The RDG depth capture only supports plane zero and one mip. Keep the
    // D3D12 subresource calculation explicit so this code does not depend on
    // D3DX helper headers being available in the injected build.
    const auto source_subresource = static_cast<UINT>(source_array_slice * source_desc.MipLevels);
    if (source_transition_subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES &&
        source_transition_subresource != source_subresource)
    {
        m_capture_failure_reason = "selected DSV slice did not match the engine transition subresource";
        return false;
    }

    std::string allocation_reason{};
    if (!ensure_captured_depth_locked(device.Get(), source_desc, allocation_reason)) {
        m_capture_failure_reason = std::move(allocation_reason);
        return false;
    }

    // This is recorded immediately before the engine's NPSR -> DEPTH_WRITE
    // transition. UE5.7 can transition one array slice at a time, so preserve
    // the original subresource exactly instead of changing both eyes' state.
    // The source state is restored before the original barrier runs.
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (auto& barrier : barriers) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    }
    barriers[0].Transition.pResource = source;
    barriers[0].Transition.Subresource = source_transition_subresource;
    barriers[0].Transition.StateBefore = source_state;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.pResource = m_captured_depth.Get();
    barriers[1].Transition.Subresource = 0;
    barriers[1].Transition.StateBefore = CAPTURED_DEPTH_SHADER_READ;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    command_list->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);

    D3D12_TEXTURE_COPY_LOCATION source_location{};
    source_location.pResource = source;
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source_location.SubresourceIndex = source_subresource;
    D3D12_TEXTURE_COPY_LOCATION destination_location{};
    destination_location.pResource = m_captured_depth.Get();
    destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination_location.SubresourceIndex = 0;
    command_list->CopyTextureRegion(&destination_location, 0, 0, 0, &source_location, nullptr);

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter = source_state;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = CAPTURED_DEPTH_SHADER_READ;
    command_list->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);

    ++m_captured_depth_generation;
    return true;
}

void DepthStencilCaptureObserver::on_resource_barriers(
    ID3D12GraphicsCommandList* command_list,
    UINT count,
    const D3D12_RESOURCE_BARRIER* barriers)
{
    if (barriers == nullptr) {
        return;
    }

    // ResourceBarrier is a renderer hot path. The ordinary path remains an
    // atomic pointer comparison. If injection happened after SceneDepthZ's DSV
    // was created, inspect only the shader-read -> DEPTH_WRITE window requested
    // requested and recover the already-live depth resource from that barrier.
    auto candidate = m_depth_trace_candidate.load(std::memory_order_acquire);
    const auto capture_enabled = m_ue5_rdg_depth_capture_enabled.load(std::memory_order_acquire);
    if (candidate == 0 && !capture_enabled) {
        return;
    }

    auto candidate_slice = m_depth_trace_candidate_array_slice.load(std::memory_order_acquire);

    for (UINT i = 0; i < count; ++i) {
        const auto& barrier = barriers[i];
        if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            continue;
        }

        const auto before = barrier.Transition.StateBefore;
        const auto after = static_cast<uint32_t>(barrier.Transition.StateAfter);
        // UE5's RDG closes the depth read window with a bare NPSR ->
        // DEPTH_WRITE transition, but UE4's RHI transitions the combined
        // read mask (DEPTH_READ | PSR | NPSR) back to DEPTH_WRITE instead.
        // Accept any read-only mask that includes a shader-resource bit so
        // load-time-allocated UE4 depth (whose DSV predates this observer)
        // can still be adopted and captured from its live barriers.
        constexpr auto capture_window_shader_read_bits =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        constexpr auto capture_window_read_only_bits = capture_window_shader_read_bits |
            D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_COPY_SOURCE;
        const auto closing_capture_window =
            (before & capture_window_shader_read_bits) != 0 &&
            (before & ~capture_window_read_only_bits) == 0 &&
            barrier.Transition.StateAfter == D3D12_RESOURCE_STATE_DEPTH_WRITE;
        // DEPTH_WRITE -> shader-read opens the frame's read window: the depth
        // the engine JUST wrote. The closing window above only sees the
        // previous frame's depth (about to be overwritten), which lags the
        // color by one frame and ghosts near geometry under camera motion.
        // Once an opening capture has succeeded, the closing window is
        // skipped — it would only re-copy the same completed depth a frame
        // late.
        const auto opening_capture_window =
            before == D3D12_RESOURCE_STATE_DEPTH_WRITE &&
            (barrier.Transition.StateAfter & capture_window_shader_read_bits) != 0 &&
            (static_cast<uint32_t>(barrier.Transition.StateAfter) &
                ~static_cast<uint32_t>(capture_window_read_only_bits)) == 0;
        const auto capture_window =
            opening_capture_window ||
            (closing_capture_window && !m_opening_capture_seen.load(std::memory_order_acquire));

        const auto resource_key = reinterpret_cast<uintptr_t>(barrier.Transition.pResource);
        if (resource_key != candidate) {
            if (!capture_enabled || !capture_window ||
                !m_ue5_rdg_depth_capture_requested.load(std::memory_order_acquire) ||
                !try_adopt_depth_candidate_from_barrier(
                    barrier.Transition.pResource,
                    barrier.Transition.Subresource,
                    candidate_slice)) {
                continue;
            }

            candidate = resource_key;
        } else if (capture_enabled && capture_window) {
            // Fallback DSVs are retained only for diagnostics. Revalidate the
            // selected resource under the map lock before any game-state copy.
            std::scoped_lock _{m_trace_mutex};
            if (!is_trace_candidate_compatible_locked(resource_key)) {
                continue;
            }
        }

        const auto previous = m_depth_trace_last_state.exchange(after, std::memory_order_acq_rel);
        if (capture_enabled && capture_window) {
            const auto source_desc = barrier.Transition.pResource->GetDesc();
            if (candidate_slice >= source_desc.DepthOrArraySize) {
                continue;
            }

            const auto selected_subresource = static_cast<UINT>(candidate_slice * source_desc.MipLevels);
            if (barrier.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES &&
                barrier.Transition.Subresource != selected_subresource)
            {
                // The selected eye was not the one the engine is about to
                // overwrite. Leave its state entirely to the game.
                continue;
            }

            if (opening_capture_window) {
                // Same-frame refreshes are bounded per consumer re-arm; the
                // last depth-write pass before Present wins.
                auto budget = m_capture_opening_budget.load(std::memory_order_acquire);
                bool acquired = false;
                while (budget != 0 &&
                    !(acquired = m_capture_opening_budget.compare_exchange_weak(
                        budget, budget - 1, std::memory_order_acq_rel))) {
                }
                if (!acquired) {
                    continue;
                }
            } else if (!m_ue5_rdg_depth_capture_requested.exchange(false, std::memory_order_acq_rel)) {
                // UE5.7 can cycle the same RDG depth resource through several
                // shader-read/write windows before Present. Each capture is a
                // full depth copy plus four barriers, so keep only the next
                // window the consumer explicitly requested.
                continue;
            }

            bool captured{};
            uint64_t generation{};
            bool log_success{};
            bool log_failure{};
            std::string capture_failure{};
            {
                std::scoped_lock _{m_capture_mutex};
                captured = capture_depth_before_restore_locked(
                    command_list,
                    barrier.Transition.pResource,
                    before,
                    candidate_slice,
                    barrier.Transition.Subresource);
                generation = m_captured_depth_generation;
                if (captured && !m_capture_success_logged) {
                    m_capture_success_logged = true;
                    log_success = true;
                } else if (!captured && !m_capture_failure_logged) {
                    m_capture_failure_logged = true;
                    log_failure = true;
                    capture_failure = m_capture_failure_reason;
                }
            }

            if (captured) {
                if (opening_capture_window && !m_opening_capture_seen.exchange(true, std::memory_order_acq_rel)) {
                    SPDLOG_INFO(
                        "[DepthObserver][DSV/RDG capture] opening windows available; capturing same-frame depth after each write pass");
                }

                std::ostringstream summary{};
                summary << "captured verified RDG depth 0x" << std::hex << std::uppercase << candidate << std::dec
                        << " generation=" << generation
                        << (opening_capture_window
                            ? " during DEPTH_WRITE -> shader-read (same-frame)"
                            : " during shader-read -> DEPTH_WRITE (previous frame)")
                        << "; source state restored before the original barrier";
                if (log_success) {
                    {
                        std::scoped_lock _{m_trace_mutex};
                        m_depth_trace_summary = summary.str();
                    }
                    SPDLOG_INFO("[DepthObserver][DSV/RDG capture] {}", summary.str());
                }
            } else if (log_failure) {
                SPDLOG_WARN(
                    "[DepthObserver][DSV/RDG capture] verified depth window could not be copied; reason={}; falling back to the normal scene path",
                    capture_failure);
            }

            // Do not permanently lose the first capture because a resource
            // resize or transient command-list failure made this window
            // unsuitable. A later verified transition may still be valid.
            if (!captured && !opening_capture_window) {
                m_ue5_rdg_depth_capture_requested.store(true, std::memory_order_release);
            }
            continue;
        }

        if (previous == after || capture_enabled) {
            continue;
        }

        const auto shader_readable =
            (barrier.Transition.StateAfter & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0 ||
            (barrier.Transition.StateAfter & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
        std::ostringstream summary{};
        summary << "selected DSV 0x" << std::hex << std::uppercase << candidate << std::dec
                << " transition 0x" << std::hex << std::uppercase << barrier.Transition.StateBefore
                << " -> 0x" << barrier.Transition.StateAfter << std::dec
                << (shader_readable ? " (shader-readable; not yet adopted)" : " (not shader-readable)");
        {
            std::scoped_lock _{m_trace_mutex};
            m_depth_trace_summary = summary.str();
        }
        SPDLOG_INFO("[DepthObserver][RDG trace] {}", summary.str());
    }
}

DXGI_FORMAT DepthStencilCaptureObserver::depth_srv_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    // UE4's pooled SceneDepthZ is commonly 32F depth + 8 stencil; sample the
    // depth plane the same way the flat3d compositor does.
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
} // namespace d3d12
