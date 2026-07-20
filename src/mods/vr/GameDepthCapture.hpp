#pragma once

// Name/engine-independent scene-depth interceptor at the graphics-API level
// (ReShade "generic depth" style).
//
// This REPLACES the older by-name SceneDepthCapture, which walked the UE RDG
// pointer graph looking for an FRDGTexture literally named "SceneDepthZ". That
// approach fails on modern titles because RDG resource debug names are stripped
// or renamed in shipping builds.
//
// Instead we watch the game's own D3D11/D3D12 rendering at the API vtable level
// and identify the main scene depth-stencil buffer by TRUE PER-DRAW COUNTING:
// the depth-format target that receives the most draw calls at ~render
// resolution each frame is the scene depth. We publish that native resource so
// the flat3d / VR compositors can read it (identical downstream handling to the
// old pool/name path — it is the game's own ID3D12Resource / ID3D11Texture2D).
//
// Mechanism:
//  - Hook the SHARED vtable slots of ID3D12GraphicsCommandList / ID3D12Device
//    (D3D12) and ID3D11DeviceContext (D3D11) via the project's PointerHook. All
//    objects of a type created by one device share one vtable, so a single
//    patch covers every command list / context.
//  - D3D12 OMSetRenderTargets only hands us an opaque descriptor handle, so we
//    also hook CreateDepthStencilView and maintain a handle.ptr -> resource(entry)
//    map to recover the resource at bind time.
//  - Draw thunks increment a per-depth-resource counter for the depth currently
//    bound on THAT command list / context. The draw hot path is fully lock-free
//    (atomics only); registry mutation (rare) is guarded by a light mutex.
//  - end_frame_* picks the winner (max draws among depth targets at ~render res),
//    publishes it, and resets the per-frame counters.

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl.h>

#include "utility/PointerHook.hpp"

class GameDepthCapture {
public:
    static GameDepthCapture& get();

    // Idempotent: installs the API vtable hooks once. Safe to call every frame.
    void ensure_installed_d3d12(ID3D12Device* device);
    void ensure_installed_d3d11(ID3D11Device* device);

    // Call once per frame (at present/composite time, AFTER the game's draws for
    // the frame have been recorded). Selects this frame's scene-depth resource
    // (max draws among depth-format targets at ~render resolution), publishes it,
    // and resets per-frame counters. render_w/h may be 0 (then pick global max).
    void end_frame_d3d12(uint32_t render_w, uint32_t render_h);
    void end_frame_d3d11(uint32_t render_w, uint32_t render_h);

    // The most recently published native scene-depth resource (ID3D12Resource /
    // ID3D11Texture2D), or nullptr. Thread-safe.
    template<typename T>
    Microsoft::WRL::ComPtr<T> get_native() {
        std::scoped_lock _{m_publish_mutex};
        if (m_published == nullptr) return nullptr;
        Microsoft::WRL::ComPtr<T> out{};
        m_published.As(&out);
        return out;
    }

    // True when the most recently published resource was selected by real
    // per-draw attribution this session (a genuine, verified scene depth) —
    // false when it came from the Pass-3 resource-registry size fallback, which
    // fires on frames with NO attributed depth draws (e.g. a loading screen).
    // A fallback resource's live GPU state is unknown, so consumers that issue
    // resource-state barriers against it (the compositor's CPU depth readback)
    // must not trust it: a wrong barrier StateBefore poisons their command list.
    bool last_publish_via_draws() const {
        return m_published_via_draws.load(std::memory_order_relaxed);
    }

    void reset(); // drop the published resource (called when no depth consumer is active)

private:
    GameDepthCapture() = default;

    // ---- shared capacities -------------------------------------------------
    static constexpr size_t kMaxDepthEntries = 64;   // distinct depth resources tracked
    static constexpr size_t kListSlots       = 512;  // command-list / context slots (power of two)

    // A tracked depth-format target. `resource` is a raw IDENTITY KEY (fast
    // compare). `hold` is set only for entries discovered via resource creation
    // (CreateCommittedResource/Placed/Reserved) — it AddRefs the resource so it
    // stays alive even when no DSV bind/draw is ever observed for it, which is
    // what makes the size-heuristic fallback safe to publish. Entries created
    // only from a DSV/draw path leave `hold` null (the draw-attributed winner is
    // inherently live — it was just rendered into this frame).
    struct D3D12DepthEntry {
        ID3D12Resource* resource{nullptr};
        Microsoft::WRL::ComPtr<ID3D12Resource> hold{};
        uint32_t w{0};
        uint32_t h{0};
        DXGI_FORMAT fmt{DXGI_FORMAT_UNKNOWN};
        std::atomic<uint64_t> draw_count{0};
    };
    struct D3D11DepthEntry {
        ID3D11Texture2D* resource{nullptr};
        uint32_t w{0};
        uint32_t h{0};
        DXGI_FORMAT fmt{DXGI_FORMAT_UNKNOWN};
        std::atomic<uint64_t> draw_count{0};
    };

    // Open-addressing slot: object pointer (list/context) -> current DepthEntry*.
    struct Slot {
        std::atomic<void*> key{nullptr};
        std::atomic<void*> val{nullptr};
    };

    // ---- lock-free list/context -> current-depth table ---------------------
    static void  slot_set(std::array<Slot, kListSlots>& table, void* key, void* val);
    static void* slot_get(std::array<Slot, kListSlots>& table, void* key);

    static bool is_depth_format(DXGI_FORMAT fmt);

    // ---- per-API handlers (called from the static thunks) ------------------
    void handle_create_dsv_d3d12(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE dest);
    // Called after any resource creation succeeds; registers depth-capable
    // TEXTURE2Ds (ALLOW_DEPTH_STENCIL) by identity so a scene depth whose DSV we
    // never observe (created before our hook, or bound via render passes) is
    // still a publishable candidate. Holds a ref to keep it alive.
    void handle_create_resource_d3d12(const D3D12_RESOURCE_DESC* desc, ID3D12Resource* resource);
    // Shared by the three resource-creation thunks: recovers the created resource
    // and registers it if it's a depth-capable 2D texture.
    static void register_if_depth(const D3D12_RESOURCE_DESC* desc, HRESULT hr, void** ppvResource);
    void handle_om_set_rts_d3d12(ID3D12GraphicsCommandList* list, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv);
    void handle_begin_render_pass_d3d12(ID3D12GraphicsCommandList* list, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds);
    void handle_clear_dsv_d3d12(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE dsv);
    void handle_draw_d3d12(ID3D12GraphicsCommandList* list);

    void handle_om_set_rts_d3d11(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv);
    void handle_clear_dsv_d3d11(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv);
    void handle_draw_d3d11(ID3D11DeviceContext* ctx);

    D3D11DepthEntry* d3d11_entry_for(ID3D11DepthStencilView* dsv); // under m_d11_mutex

    // ---- static vtable thunks (D3D12) --------------------------------------
    static void STDMETHODCALLTYPE thunk_create_dsv_d3d12(
        ID3D12Device* self, ID3D12Resource* pResource,
        const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
    static HRESULT STDMETHODCALLTYPE thunk_create_committed_resource_d3d12(
        ID3D12Device* self, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource);
    static HRESULT STDMETHODCALLTYPE thunk_create_placed_resource_d3d12(
        ID3D12Device* self, ID3D12Heap* pHeap, UINT64 HeapOffset,
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource);
    static HRESULT STDMETHODCALLTYPE thunk_create_reserved_resource_d3d12(
        ID3D12Device* self, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource);
    static void STDMETHODCALLTYPE thunk_om_set_rts_d3d12(
        ID3D12GraphicsCommandList* self, UINT NumRenderTargetDescriptors,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
        BOOL RTsSingleHandleToDescriptorRange,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor);
    static void STDMETHODCALLTYPE thunk_begin_render_pass_d3d12(
        ID3D12GraphicsCommandList* self, UINT NumRenderTargets,
        const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pRenderTargets,
        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDepthStencil, D3D12_RENDER_PASS_FLAGS Flags);
    static void STDMETHODCALLTYPE thunk_clear_dsv_d3d12(
        ID3D12GraphicsCommandList* self, D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView,
        D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects);
    static void STDMETHODCALLTYPE thunk_draw_instanced_d3d12(
        ID3D12GraphicsCommandList* self, UINT VertexCountPerInstance, UINT InstanceCount,
        UINT StartVertexLocation, UINT StartInstanceLocation);
    static void STDMETHODCALLTYPE thunk_draw_indexed_instanced_d3d12(
        ID3D12GraphicsCommandList* self, UINT IndexCountPerInstance, UINT InstanceCount,
        UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation);

    // ---- static vtable thunks (D3D11) --------------------------------------
    static void STDMETHODCALLTYPE thunk_om_set_rts_d3d11(
        ID3D11DeviceContext* self, UINT NumViews,
        ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView);
    static void STDMETHODCALLTYPE thunk_clear_dsv_d3d11(
        ID3D11DeviceContext* self, ID3D11DepthStencilView* pDepthStencilView,
        UINT ClearFlags, FLOAT Depth, UINT8 Stencil);
    static void STDMETHODCALLTYPE thunk_draw_indexed_d3d11(
        ID3D11DeviceContext* self, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation);
    static void STDMETHODCALLTYPE thunk_draw_d3d11(
        ID3D11DeviceContext* self, UINT VertexCount, UINT StartVertexLocation);
    static void STDMETHODCALLTYPE thunk_draw_indexed_instanced_d3d11(
        ID3D11DeviceContext* self, UINT IndexCountPerInstance, UINT InstanceCount,
        UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation);
    static void STDMETHODCALLTYPE thunk_draw_instanced_d3d11(
        ID3D11DeviceContext* self, UINT VertexCountPerInstance, UINT InstanceCount,
        UINT StartVertexLocation, UINT StartInstanceLocation);

    // ---- published resource (shared) ---------------------------------------
    std::mutex m_publish_mutex{};
    Microsoft::WRL::ComPtr<IUnknown> m_published{};
    // Whether m_published was chosen by draw attribution (true) or the size
    // fallback (false). Atomic so lock-free consumers can read it. See
    // last_publish_via_draws().
    std::atomic<bool> m_published_via_draws{false};

    // ---- D3D12 state -------------------------------------------------------
    bool m_installed_d3d12{false};
    std::mutex m_d12_mutex{}; // guards entries/count and the handle map
    std::array<D3D12DepthEntry, kMaxDepthEntries> m_d12_entries{};
    int m_d12_entry_count{0};
    std::unordered_map<SIZE_T, D3D12DepthEntry*> m_d12_handle_map{}; // DSV handle.ptr -> entry (may be null)
    std::array<Slot, kListSlots> m_d12_lists{};                      // command list -> current DepthEntry*
    void* m_d12_last_logged{nullptr};

    std::unique_ptr<PointerHook> m_h12_create_dsv{};
    std::unique_ptr<PointerHook> m_h12_create_committed{};
    std::unique_ptr<PointerHook> m_h12_create_placed{};
    std::unique_ptr<PointerHook> m_h12_create_reserved{};
    std::unique_ptr<PointerHook> m_h12_om_set_rts{};
    std::unique_ptr<PointerHook> m_h12_begin_render_pass{};
    std::unique_ptr<PointerHook> m_h12_clear_dsv{};
    std::unique_ptr<PointerHook> m_h12_draw{};
    std::unique_ptr<PointerHook> m_h12_draw_indexed{};

    // ---- D3D11 state -------------------------------------------------------
    bool m_installed_d3d11{false};
    std::mutex m_d11_mutex{}; // guards entries/count
    std::array<D3D11DepthEntry, kMaxDepthEntries> m_d11_entries{};
    int m_d11_entry_count{0};
    std::array<Slot, kListSlots> m_d11_ctxs{}; // context -> current DepthEntry*
    void* m_d11_last_logged{nullptr};

    std::unique_ptr<PointerHook> m_h11_om_set_rts{};
    std::unique_ptr<PointerHook> m_h11_clear_dsv{};
    std::unique_ptr<PointerHook> m_h11_draw_indexed{};
    std::unique_ptr<PointerHook> m_h11_draw{};
    std::unique_ptr<PointerHook> m_h11_draw_indexed_instanced{};
    std::unique_ptr<PointerHook> m_h11_draw_instanced{};
};
