#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi")

#include <d3d12.h>
#include <dxgi1_4.h>

#include "utility/PointerHook.hpp"
#include "utility/VtableHook.hpp"

// Consumers may observe DSV creation and resource barriers without taking
// ownership. The barrier callback runs immediately before the original barrier
// so an opt-in consumer can make a short, self-contained copy while the game
// still owns the command-list state. Used by Flat3D's DSV Observer depth source.
class D3D12DepthStencilObserver {
public:
    virtual ~D3D12DepthStencilObserver() = default;

    virtual void on_depth_stencil_view_created(
        ID3D12Resource* resource,
        const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor) = 0;

    virtual void on_resource_barriers(
        ID3D12GraphicsCommandList* command_list,
        UINT count,
        const D3D12_RESOURCE_BARRIER* barriers) = 0;
};

class D3D12Hook
{
public:
	typedef std::function<void(D3D12Hook&)> OnPresentFn;
	typedef std::function<void(D3D12Hook&, uint32_t w, uint32_t h)> OnResizeBuffersFn;
    typedef std::function<void(D3D12Hook&, uint32_t w, uint32_t h)> OnResizeTargetFn;
    typedef std::function<void(D3D12Hook&)> OnCreateSwapChainFn;

	D3D12Hook() = default;
	virtual ~D3D12Hook();

	bool hook();
	bool unhook();

    // Raw AFW/NeverDLSS harvest callbacks (registered by VR's frame-warp init).
    // Invoked AFTER the original call from D3D12Hook's own vtable hooks. The AFW
    // base used to patch these same vtable slots with a second, untracked hook
    // layer (hookVtable) — when either layer re-installed, each one's "original"
    // pointed at the other and every barrier call recursed to a stack overflow.
    // Registering here keeps exactly ONE patcher per slot.
    using RawResourceBarrierFn = void(WINAPI*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
    using RawClearDepthStencilViewFn = void(WINAPI*)(
        ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
    using RawCreateDepthStencilViewFn = void(WINAPI*)(
        ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    static inline std::atomic<RawResourceBarrierFn> s_on_raw_resource_barrier{nullptr};
    static inline std::atomic<RawClearDepthStencilViewFn> s_on_raw_clear_depth_stencil_view{nullptr};
    static inline std::atomic<RawCreateDepthStencilViewFn> s_on_raw_create_depth_stencil_view{nullptr};

    bool is_hooked() {
        return m_hooked;
    }

    void on_present(OnPresentFn fn) {
        m_on_present = fn;
    }

    void on_post_present(OnPresentFn fn) {
        m_on_post_present = fn;
    }

    void on_resize_buffers(OnResizeBuffersFn fn) {
        m_on_resize_buffers = fn;
    }

    void on_resize_target(OnResizeTargetFn fn) {
        m_on_resize_target = fn;
    }

    // 3D Display mode: rewrite sub-native ResizeBuffers/ResizeTarget requests
    // to this size (0 disables). Keeps pixel-exact output modes alive when the
    // game selects a smaller resolution.
    void set_forced_resize(uint32_t w, uint32_t h) {
        m_forced_resize_w = w;
        m_forced_resize_h = h;
    }

    // The native output size the swapchain is being held at (0 = no hold).
    uint32_t get_forced_resize_width() const { return m_forced_resize_w.load(); }
    uint32_t get_forced_resize_height() const { return m_forced_resize_h.load(); }

    // The size the game last REQUESTED before a forced rewrite (0 = none).
    // Used as the 3D render resolution so the in-game setting still controls
    // render cost while the output stays native.
    uint32_t get_game_requested_width() const { return m_game_requested_w.load(); }
    uint32_t get_game_requested_height() const { return m_game_requested_h.load(); }

    // Seed the preserved size (used when WE initiate the native resize while
    // the game was already running sub-native).
    void set_game_requested(uint32_t w, uint32_t h) {
        m_game_requested_w = w;
        m_game_requested_h = h;
    }

    // The size of the LAST ResizeBuffers request as issued by the engine
    // (pre-rewrite; 0 = no request since the forced-resize was armed). The
    // engine sizes its Slate/UI draw from this belief, so the redirected UI
    // target must match it — not the (forced-native) backbuffer.
    uint32_t get_engine_believed_width() const { return m_engine_believed_w.load(); }
    uint32_t get_engine_believed_height() const { return m_engine_believed_h.load(); }

    // Last color space the game passed to SetColorSpace1 (0xFFFFFFFF = never
    // called). A 10-bit swapchain is NOT inherently HDR — only an explicit
    // PQ/scRGB color space is (SDR games commonly use R10G10B10A2 + G22).
    uint32_t get_swapchain_colorspace() const { return m_swapchain_colorspace.load(); }

    bool is_swapchain_pq() const {
        return m_swapchain_colorspace.load() == (uint32_t)DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    }

    // Suppress render-resolution capture for a time window: engine-initiated
    // resizes WE requested (native nudge / borderless kick) can arrive more
    // than once, seconds apart (Gotham Knights applies twice) — a one-shot
    // flag misses the follow-ups and the render resolution silently jumps to
    // native.
    void suppress_render_res_capture_for(int64_t ms) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count();
        m_suppress_capture_until_ms = now + ms;
    }

    bool is_render_res_capture_suppressed() const {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count();
        return now < m_suppress_capture_until_ms.load();
    }

    /*void on_create_swap_chain(OnCreateSwapChainFn fn) {
        m_on_create_swap_chain = fn;
    }*/

    ID3D12Device4* get_device() const {
        return m_device;
    }

    IDXGISwapChain3* get_swap_chain() const {
        return m_swap_chain;
    }

    auto get_swapchain_0() { return m_swapchain_0; }
    auto get_swapchain_1() { return m_swapchain_1; }

    ID3D12CommandQueue* get_command_queue() const {
        return m_command_queue;
    }

    UINT get_display_width() const {
        return m_display_width;
    }

    UINT get_display_height() const {
        return m_display_height;
    }

    UINT get_render_width() const {
        return m_render_width;
    }

    UINT get_render_height() const {
        return m_render_height;
    }

    bool is_inside_present() const {
        return m_inside_present;
    }

    bool is_proton_swapchain() const {
        return m_using_proton_swapchain;
    }

    bool is_framegen_swapchain() const {
        return m_using_frame_generation_swapchain;
    }

    void ignore_next_present() {
        m_ignore_next_present = true;
    }

    // One-shot: with interval 0, strip DXGI_PRESENT_ALLOW_TEARING instead of
    // adding it. Flip-model swapchains never tear without that flag — the
    // display still flips on vblank while presents run unthrottled, so AFR /
    // Synced Sequential can present both eye frames per refresh tear-free.
    void set_next_present_no_tearing() {
        m_next_present_no_tearing = true;
    }

    void set_next_present_interval(uint32_t interval) {
        m_next_present_interval = interval;
    }

    void set_depth_stencil_observer(D3D12DepthStencilObserver* observer) {
        m_depth_stencil_observer.store(observer, std::memory_order_release);
    }

    // Diagnostic: when set, enable the D3D12 debug layer's InfoQueue on the
    // game's device (if it was created with the debug layer) and pipe its
    // validation messages into the UEVR log. Pushed from the flat3d update so
    // it is inert unless 3D Display mode explicitly asks for it.
    void set_debug_layer_wanted(bool v) {
        m_debug_layer_wanted.store(v);
    }

protected:
    ID3D12Device4* m_device{ nullptr };
    IDXGISwapChain3* m_swap_chain{ nullptr };
    IDXGISwapChain3* m_swapchain_0{};
    IDXGISwapChain3* m_swapchain_1{};
    ID3D12CommandQueue* m_command_queue{ nullptr };
    UINT m_display_width{ NULL };
    UINT m_display_height{ NULL };
    UINT m_render_width{ NULL };
    UINT m_render_height{ NULL };

    // Forced minimum swapchain size (0 = off). See set_forced_resize.
    std::atomic<uint32_t> m_forced_resize_w{ 0 };
    std::atomic<uint32_t> m_forced_resize_h{ 0 };
    std::atomic<uint32_t> m_game_requested_w{ 0 };
    std::atomic<uint32_t> m_game_requested_h{ 0 };
    std::atomic<uint32_t> m_engine_believed_w{ 0 };
    std::atomic<uint32_t> m_engine_believed_h{ 0 };
    std::atomic<int64_t> m_suppress_capture_until_ms{ 0 };
    std::atomic<uint32_t> m_swapchain_colorspace{ 0xFFFFFFFF };

    uint32_t m_command_queue_offset{};
    uint32_t m_proton_swapchain_offset{};

    std::optional<uint32_t> m_next_present_interval{};
    bool m_next_present_no_tearing{false};

    bool m_using_proton_swapchain{ false };
    bool m_using_frame_generation_swapchain{ false };
    bool m_skip_dummy_swapchain_type_info_probe{ false };
    bool m_hooked{ false };
    bool m_is_phase_1{ true };
    bool m_inside_present{false};
    bool m_ignore_next_present{false};
    std::unordered_set<uintptr_t> m_swapchains_requiring_original_present_params{};
    std::unordered_set<uintptr_t> m_original_present_param_skip_logged_swapchains{};

    // --- Diagnostic D3D12 debug-layer capture (see set_debug_layer_wanted) ---
    std::atomic<bool> m_debug_layer_wanted{ false };
    bool m_debug_layer_setup_done{ false };
    bool m_debug_poll{ false }; // fallback when the synchronous callback is unavailable

    void setup_debug_info_queue();
    void drain_debug_messages();

    std::unique_ptr<PointerHook> m_present_hook{};
    std::unique_ptr<PointerHook> m_present1_hook{};
    std::vector<std::unique_ptr<PointerHook>> m_create_graphics_pipeline_state_hooks{};
    std::vector<std::unique_ptr<PointerHook>> m_create_pipeline_state_hooks{};
    std::vector<std::unique_ptr<PointerHook>> m_create_render_target_view_hooks{};
    std::vector<std::unique_ptr<PointerHook>> m_create_depth_stencil_view_hooks{};
    std::vector<std::unique_ptr<PointerHook>> m_set_pipeline_state_hooks{};
    std::vector<std::unique_ptr<PointerHook>> m_resource_barrier_hooks{};
    std::vector<std::unique_ptr<PointerHook>> m_clear_depth_stencil_view_cmd_hooks{};
    std::unordered_map<uintptr_t, PointerHook*> m_create_graphics_pipeline_state_hook_lookup{};
    std::unordered_map<uintptr_t, PointerHook*> m_create_pipeline_state_hook_lookup{};
    std::unordered_map<uintptr_t, PointerHook*> m_create_render_target_view_hook_lookup{};
    std::unordered_map<uintptr_t, PointerHook*> m_create_depth_stencil_view_hook_lookup{};
    std::unordered_map<uintptr_t, PointerHook*> m_set_pipeline_state_hook_lookup{};
    std::atomic<uint64_t> m_set_pipeline_state_hook_generation{1};
    std::unordered_map<uintptr_t, PointerHook*> m_resource_barrier_hook_lookup{};
    std::unordered_map<uintptr_t, PointerHook*> m_clear_depth_stencil_view_cmd_hook_lookup{};
    std::atomic<D3D12DepthStencilObserver*> m_depth_stencil_observer{nullptr};
    std::unique_ptr<VtableHook> m_swapchain_hook{};
    //std::unique_ptr<FunctionHook> m_create_swap_chain_hook{};

    OnPresentFn m_on_present{ nullptr };
    OnPresentFn m_on_post_present{ nullptr };
    OnResizeBuffersFn m_on_resize_buffers{ nullptr };
    OnResizeTargetFn m_on_resize_target{ nullptr };
    //OnCreateSwapChainFn m_on_create_swap_chain{ nullptr };
    
    static HRESULT present_internal(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags, DXGI_PRESENT_PARAMETERS* params, bool present1 = false);

    static HRESULT WINAPI present(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags);
    static HRESULT WINAPI present1(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags, DXGI_PRESENT_PARAMETERS* params);
    static HRESULT WINAPI create_graphics_pipeline_state(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** pipeline_state);
    static HRESULT WINAPI create_pipeline_state(ID3D12Device2* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** pipeline_state);
    static void WINAPI create_render_target_view(ID3D12Device* device, ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
    static void WINAPI create_depth_stencil_view(ID3D12Device* device, ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
    static void WINAPI set_pipeline_state(ID3D12GraphicsCommandList* command_list, ID3D12PipelineState* pipeline_state);
    static void WINAPI resource_barrier(ID3D12GraphicsCommandList* command_list, UINT count, const D3D12_RESOURCE_BARRIER* barriers);
    static void WINAPI clear_depth_stencil_view_cmd(ID3D12GraphicsCommandList* command_list, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
        D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT num_rects, const D3D12_RECT* rects);
    static HRESULT WINAPI resize_buffers(IDXGISwapChain3* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags);
    static HRESULT WINAPI resize_target(IDXGISwapChain3* swap_chain, const DXGI_MODE_DESC* new_target_parameters);
    static HRESULT WINAPI set_color_space1(IDXGISwapChain3* swap_chain, DXGI_COLOR_SPACE_TYPE color_space);
    //static HRESULT WINAPI create_swap_chain(IDXGIFactory4* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* p_fullscreen_desc, IDXGIOutput* p_restrict_to_output, IDXGISwapChain** swap_chain);

    PointerHook* find_create_graphics_pipeline_state_hook(void* slot) const;
    PointerHook* find_create_pipeline_state_hook(void* slot) const;
    PointerHook* find_create_render_target_view_hook(void* slot) const;
    PointerHook* find_create_depth_stencil_view_hook(void* slot) const;
    PointerHook* find_set_pipeline_state_hook(void* slot) const;
    PointerHook* find_resource_barrier_hook(void* slot) const;
    PointerHook* find_clear_depth_stencil_view_cmd_hook(void* slot) const;
};

