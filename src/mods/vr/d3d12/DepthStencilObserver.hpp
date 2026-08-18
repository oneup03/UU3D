#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <wrl.h>

#include <hooks/D3D12Hook.hpp>

namespace d3d12 {
// API-level scene-depth capture that rides the D3D12Hook depth-stencil observer.
// It watches DSV creation and resource barriers, adopts the depth resource that
// matches the presentation extent, and makes a self-contained shader-readable
// copy inside the game's own command list (state restored before the original
// barrier runs). No engine hook, so it sees depth allocated at any time
// (including at load) and is safe on titles where the engine render-target pool
// path is unsafe. Feeds Flat3D's "DSV Observer" depth source.
//
// Extracted from the former DIBR depth-capture path; the synthesis/reprojection
// half was dropped along with DIBR.
class DepthStencilCaptureObserver : public D3D12DepthStencilObserver {
public:
    DepthStencilCaptureObserver() = default;
    ~DepthStencilCaptureObserver() override;

    DepthStencilCaptureObserver(const DepthStencilCaptureObserver&) = delete;
    DepthStencilCaptureObserver& operator=(const DepthStencilCaptureObserver&) = delete;

    void reset();

    std::string depth_trace_summary() const;
    Microsoft::WRL::ComPtr<ID3D12Resource> captured_depth_snapshot() const;
    bool has_captured_depth() const;

    // Match a DSV/RDG candidate against the presentation source extent.
    void set_depth_trace_expected_extent(uint32_t width, uint32_t height);
    void set_ue5_rdg_depth_capture_enabled(bool enabled);
    // A producer can present several matching depth transitions inside one
    // frame. Capture only the next verified window; the consumer re-arms this
    // after it has used the snapshot for a frame.
    void request_ue5_rdg_depth_capture();

    void on_depth_stencil_view_created(
        ID3D12Resource* resource,
        const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override;
    void on_resource_barriers(
        ID3D12GraphicsCommandList* command_list,
        UINT count,
        const D3D12_RESOURCE_BARRIER* barriers) override;

private:
    using ComPtr = Microsoft::WRL::ComPtr<ID3D12Resource>;

    static DXGI_FORMAT depth_srv_format(DXGI_FORMAT format);

    bool ensure_captured_depth_locked(ID3D12Device* device, const D3D12_RESOURCE_DESC& source_desc, std::string& reason);
    void retire_captured_depth_locked();
    bool is_trace_candidate_compatible_locked(uintptr_t resource) const;
    bool try_adopt_depth_candidate_from_barrier(
        ID3D12Resource* resource,
        UINT transition_subresource,
        uint32_t& selected_array_slice);
    bool capture_depth_before_restore_locked(
        ID3D12GraphicsCommandList* command_list,
        ID3D12Resource* source,
        D3D12_RESOURCE_STATES source_state,
        uint32_t source_array_slice,
        UINT source_transition_subresource);
    void refresh_depth_trace_candidate_locked();

    struct DepthTraceCandidate {
        uintptr_t resource{};
        uint64_t width{};
        uint32_t height{};
        DXGI_FORMAT resource_format{DXGI_FORMAT_UNKNOWN};
        D3D12_RESOURCE_FLAGS flags{D3D12_RESOURCE_FLAG_NONE};
        uint32_t sample_count{};
        DXGI_FORMAT view_format{DXGI_FORMAT_UNKNOWN};
        uint16_t array_size{};
        uint32_t array_slice{};
    };

    // Guards the traced-resource map, expected extent, and the trace summary.
    mutable std::mutex m_trace_mutex{};
    std::string m_capture_failure_reason{};
    std::unordered_map<uintptr_t, DepthTraceCandidate> m_traced_depth_resources{};
    std::string m_depth_trace_summary{"waiting for a DSV candidate"};
    uint64_t m_depth_trace_expected_width{};
    uint32_t m_depth_trace_expected_height{};
    bool m_barrier_discovery_logged{};
    std::atomic<uintptr_t> m_depth_trace_candidate{};
    std::atomic<uint32_t> m_depth_trace_candidate_array_slice{};
    std::atomic<uint32_t> m_depth_trace_last_state{D3D12_RESOURCE_STATE_COMMON};
    std::atomic<bool> m_ue5_rdg_depth_capture_enabled{false};
    std::atomic<bool> m_ue5_rdg_depth_capture_requested{false};
    // Opening windows (DEPTH_WRITE -> shader-read) snapshot the depth the engine
    // just wrote THIS frame; the closing window (shader-read -> DEPTH_WRITE) only
    // sees the previous frame's depth right before it is overwritten, which lags
    // the color by one frame. Prefer opening windows when the title has them,
    // refreshing up to a small budget per consumer re-arm.
    std::atomic<uint32_t> m_capture_opening_budget{};
    std::atomic<bool> m_opening_capture_seen{false};
    mutable std::mutex m_capture_mutex{};
    ComPtr m_captured_depth{};
    // A consumer may still have work in flight sampling the owned copy when a
    // resize or capture toggle replaces it. Retain replaced copies until reset()
    // (called only after the consumer has drained) rather than freeing mid-use.
    std::vector<ComPtr> m_retired_captured_depths{};
    uint64_t m_captured_depth_width{};
    uint32_t m_captured_depth_height{};
    DXGI_FORMAT m_captured_depth_format{DXGI_FORMAT_UNKNOWN};
    uint64_t m_captured_depth_generation{};
    bool m_capture_success_logged{};
    bool m_capture_failure_logged{};
};
} // namespace d3d12
