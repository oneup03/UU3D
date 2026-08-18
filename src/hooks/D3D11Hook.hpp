#pragma once

#include <functional>
#include <atomic>
#include <chrono>
#include <optional>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl.h>

#include "utility/PointerHook.hpp"

class D3D11Hook {
public:
    typedef std::function<void(D3D11Hook&)> OnPresentFn;
    typedef std::function<void(D3D11Hook&, uint32_t w, uint32_t h)> OnResizeBuffersFn;

    D3D11Hook() = default;
    virtual ~D3D11Hook();

	bool is_hooked() {
		return m_hooked;
	}

    bool is_inside_present() const {
        return m_inside_present;
    }

    void ignore_next_present() {
        m_ignore_next_present = true;
    }

    void set_next_present_interval(uint32_t interval) {
        m_next_present_interval = interval;
    }

    // One-shot: with interval 0, strip DXGI_PRESENT_ALLOW_TEARING instead of
    // adding it (flip-model swapchains never tear without the flag; see
    // D3D12Hook::set_next_present_no_tearing).
    void set_next_present_no_tearing() {
        m_next_present_no_tearing = true;
    }

    bool hook();
    bool unhook();

    void on_present(OnPresentFn fn) { m_on_present = fn; }
    void on_post_present(OnPresentFn fn) { m_on_post_present = fn; }
    void on_resize_buffers(OnResizeBuffersFn fn) { m_on_resize_buffers = fn; }

    // Naruto/UE4.16 draws the scene viewport as a Slate element while Slate is
    // redirected to the dedicated UI target. Limit suppression to that draw.
    static void begin_naruto_slate_ui_capture(
        ID3D11Resource* ui_target,
        ID3D11Resource* scene_target,
        ID3D11Resource* original_target);
    static void end_naruto_slate_ui_capture();

    // 3D Display mode: rewrite sub-native ResizeBuffers requests to this size
    // (0 disables). See D3D12Hook::set_forced_resize.
    void set_forced_resize(uint32_t w, uint32_t h) {
        m_forced_resize_w = w;
        m_forced_resize_h = h;
    }

    // The native output size the swapchain is being held at (0 = no hold).
    uint32_t get_forced_resize_width() const { return m_forced_resize_w.load(); }
    uint32_t get_forced_resize_height() const { return m_forced_resize_h.load(); }

    // The size the game last REQUESTED before a forced rewrite (0 = none).
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

    ID3D11Device* get_device() { return m_device; }
    IDXGISwapChain* get_swap_chain() { return m_swap_chain; } // The "active" swap chain.
    auto get_swapchain_0() { return m_swapchain_0; }
    auto get_swapchain_1() { return m_swapchain_1; }
    auto& get_last_depthstencil_used() { return m_last_depthstencil_used; }

protected:
    template<typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ID3D11Device* m_device{ nullptr };
    IDXGISwapChain* m_swap_chain{ nullptr };
    IDXGISwapChain* m_swapchain_0{};
    IDXGISwapChain* m_swapchain_1{};
    bool m_hooked{ false };
    bool m_inside_present{false};
    bool m_ignore_next_present{false};

    std::optional<uint32_t> m_next_present_interval{};
    bool m_next_present_no_tearing{false};

    // Forced minimum swapchain size (0 = off).
    std::atomic<uint32_t> m_forced_resize_w{ 0 };
    std::atomic<uint32_t> m_forced_resize_h{ 0 };
    std::atomic<uint32_t> m_game_requested_w{ 0 };
    std::atomic<uint32_t> m_game_requested_h{ 0 };
    std::atomic<uint32_t> m_engine_believed_w{ 0 };
    std::atomic<uint32_t> m_engine_believed_h{ 0 };
    std::atomic<int64_t> m_suppress_capture_until_ms{ 0 };

    std::unique_ptr<PointerHook> m_present_hook{};
    std::unique_ptr<PointerHook> m_resize_buffers_hook{};
    std::unique_ptr<PointerHook> m_set_render_targets_hook{};
    OnPresentFn m_on_present{ nullptr };
    OnPresentFn m_on_post_present{ nullptr };
    OnResizeBuffersFn m_on_resize_buffers{ nullptr };
    ComPtr<ID3D11Texture2D> m_last_depthstencil_used{};

    static HRESULT WINAPI present(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags);
    static HRESULT WINAPI resize_buffers(IDXGISwapChain* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags);
    static void WINAPI set_render_targets(
        ID3D11DeviceContext* context, UINT num_views, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
};
