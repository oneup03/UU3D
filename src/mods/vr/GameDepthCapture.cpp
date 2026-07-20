#include <windows.h>

#include <cstdlib>

#include <spdlog/spdlog.h>

#include "GameDepthCapture.hpp"

// ---------------------------------------------------------------------------
// Original function pointers.
//
// The static thunks below forward to the game's original vtable entries. We
// stash the originals in file-static atomics (set once at install time) so the
// draw hot path forwards with a single relaxed load — no singleton/unique_ptr
// dereference per draw.
// ---------------------------------------------------------------------------
namespace {
using PFN_CreateDSV12 = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*,
    const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateCommittedResource12 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,
    const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreatePlacedResource12 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,
    ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreateReservedResource12 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_OMSetRTs12 = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
    const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using PFN_BeginRenderPass12 = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC*, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,
    D3D12_RENDER_PASS_FLAGS);
using PFN_ClearDSV12 = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE,
    D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
using PFN_DrawInstanced12 = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using PFN_DrawIndexedInstanced12 = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);

using PFN_OMSetRTs11 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using PFN_ClearDSV11 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using PFN_DrawIndexed11 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw11 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexedInstanced11 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced11 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);

std::atomic<void*> g_orig_create_dsv12{nullptr};
std::atomic<void*> g_orig_create_committed12{nullptr};
std::atomic<void*> g_orig_create_placed12{nullptr};
std::atomic<void*> g_orig_create_reserved12{nullptr};
std::atomic<void*> g_orig_om_set_rts12{nullptr};
std::atomic<void*> g_orig_begin_render_pass12{nullptr};
std::atomic<void*> g_orig_clear_dsv12{nullptr};
std::atomic<void*> g_orig_draw12{nullptr};
std::atomic<void*> g_orig_draw_indexed12{nullptr};

std::atomic<void*> g_orig_om_set_rts11{nullptr};
std::atomic<void*> g_orig_clear_dsv11{nullptr};
std::atomic<void*> g_orig_draw_indexed11{nullptr};
std::atomic<void*> g_orig_draw11{nullptr};
std::atomic<void*> g_orig_draw_indexed_instanced11{nullptr};
std::atomic<void*> g_orig_draw_instanced11{nullptr};

// Diagnostics (D3D12): running totals of OMSetRenderTargets calls that carried a
// non-null DSV handle, and how many of those resolved to a registered depth
// entry. If binds climb but resolved stays 0, the game's depth DSV was created
// before our hook (or via a path we don't observe) — its handle isn't in the map.
std::atomic<uint64_t> g_d12_om_binds{0};
std::atomic<uint64_t> g_d12_om_resolved{0};

// D3D12 GraphicsCommandList / Device vtable indices (base interface — stable).
constexpr size_t k12_DrawInstanced        = 12;
constexpr size_t k12_DrawIndexedInstanced = 13;
constexpr size_t k12_OMSetRenderTargets   = 46;
constexpr size_t k12_ClearDepthStencilView = 47;
constexpr size_t k12_BeginRenderPass      = 68; // ID3D12GraphicsCommandList4
constexpr size_t k12dev_CreateDepthStencilView   = 21;
constexpr size_t k12dev_CreateCommittedResource  = 27;
constexpr size_t k12dev_CreatePlacedResource     = 29;
constexpr size_t k12dev_CreateReservedResource   = 30;

// D3D11 DeviceContext vtable indices (stable).
constexpr size_t k11_DrawIndexed          = 12;
constexpr size_t k11_Draw                 = 13;
constexpr size_t k11_DrawIndexedInstanced = 20;
constexpr size_t k11_DrawInstanced        = 21;
constexpr size_t k11_OMSetRenderTargets   = 33;
constexpr size_t k11_ClearDepthStencilView = 53;

inline size_t ptr_hash(void* p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return static_cast<size_t>((v >> 4) ^ (v >> 12) ^ (v >> 20));
}
} // namespace

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
GameDepthCapture& GameDepthCapture::get() {
    static GameDepthCapture instance;
    return instance;
}

bool GameDepthCapture::is_depth_format(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:      // 16-bit depth (e.g. shadow atlases) viewed as D16_UNORM
    case DXGI_FORMAT_R16_UNORM:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Lock-free open-addressing table: object pointer -> current DepthEntry*
// ---------------------------------------------------------------------------
void GameDepthCapture::slot_set(std::array<Slot, kListSlots>& table, void* key, void* val) {
    const size_t start = ptr_hash(key) & (kListSlots - 1);
    for (size_t i = 0; i < kListSlots; ++i) {
        const size_t idx = (start + i) & (kListSlots - 1);
        void* k = table[idx].key.load(std::memory_order_acquire);
        if (k == key) {
            table[idx].val.store(val, std::memory_order_release);
            return;
        }
        if (k == nullptr) {
            void* expected = nullptr;
            if (table[idx].key.compare_exchange_strong(expected, key,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                table[idx].val.store(val, std::memory_order_release);
                return;
            }
            // Lost the race; if the winner claimed OUR key, reuse the slot.
            if (expected == key) {
                table[idx].val.store(val, std::memory_order_release);
                return;
            }
        }
    }
    // Table saturated (>512 distinct lists/contexts): drop silently.
}

void* GameDepthCapture::slot_get(std::array<Slot, kListSlots>& table, void* key) {
    const size_t start = ptr_hash(key) & (kListSlots - 1);
    for (size_t i = 0; i < kListSlots; ++i) {
        const size_t idx = (start + i) & (kListSlots - 1);
        void* k = table[idx].key.load(std::memory_order_acquire);
        if (k == key) {
            return table[idx].val.load(std::memory_order_acquire);
        }
        if (k == nullptr) {
            return nullptr;
        }
    }
    return nullptr;
}

// ===========================================================================
// D3D12
// ===========================================================================
void GameDepthCapture::ensure_installed_d3d12(ID3D12Device* device) {
    if (m_installed_d3d12 || device == nullptr) {
        return;
    }
    m_installed_d3d12 = true; // one attempt; do not spin every frame on failure

    // Throwaway allocator + command list to read the shared
    // ID3D12GraphicsCommandList vtable. The objects can be released afterwards
    // — the vtable pointer stays valid for the process lifetime.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc{};
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) {
        SPDLOG_ERROR("[GameDepthCapture] D3D12: CreateCommandAllocator failed - depth capture not installed");
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list{};
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) {
        SPDLOG_ERROR("[GameDepthCapture] D3D12: CreateCommandList failed - depth capture not installed");
        return;
    }

    void** list_vtbl = *reinterpret_cast<void***>(list.Get());
    void** dev_vtbl = *reinterpret_cast<void***>(device);
    if (list_vtbl == nullptr || dev_vtbl == nullptr) {
        SPDLOG_ERROR("[GameDepthCapture] D3D12: null vtable - depth capture not installed");
        return;
    }

    g_orig_om_set_rts12.store(list_vtbl[k12_OMSetRenderTargets], std::memory_order_relaxed);
    g_orig_clear_dsv12.store(list_vtbl[k12_ClearDepthStencilView], std::memory_order_relaxed);
    g_orig_draw12.store(list_vtbl[k12_DrawInstanced], std::memory_order_relaxed);
    g_orig_draw_indexed12.store(list_vtbl[k12_DrawIndexedInstanced], std::memory_order_relaxed);
    g_orig_create_dsv12.store(dev_vtbl[k12dev_CreateDepthStencilView], std::memory_order_relaxed);
    g_orig_create_committed12.store(dev_vtbl[k12dev_CreateCommittedResource], std::memory_order_relaxed);
    g_orig_create_placed12.store(dev_vtbl[k12dev_CreatePlacedResource], std::memory_order_relaxed);
    g_orig_create_reserved12.store(dev_vtbl[k12dev_CreateReservedResource], std::memory_order_relaxed);

    m_h12_om_set_rts    = std::make_unique<PointerHook>(&list_vtbl[k12_OMSetRenderTargets],   reinterpret_cast<void*>(&thunk_om_set_rts_d3d12));
    m_h12_clear_dsv     = std::make_unique<PointerHook>(&list_vtbl[k12_ClearDepthStencilView], reinterpret_cast<void*>(&thunk_clear_dsv_d3d12));
    m_h12_draw          = std::make_unique<PointerHook>(&list_vtbl[k12_DrawInstanced],        reinterpret_cast<void*>(&thunk_draw_instanced_d3d12));
    m_h12_draw_indexed  = std::make_unique<PointerHook>(&list_vtbl[k12_DrawIndexedInstanced], reinterpret_cast<void*>(&thunk_draw_indexed_instanced_d3d12));
    m_h12_create_dsv    = std::make_unique<PointerHook>(&dev_vtbl[k12dev_CreateDepthStencilView], reinterpret_cast<void*>(&thunk_create_dsv_d3d12));

    // Resource-creation hooks: register depth-capable TEXTURE2Ds by identity so a
    // scene depth whose DSV we never observe (created before our hook, or bound
    // via a render pass) is still a publishable candidate. This is the primary
    // capture path for titles like SMT5V that allocate depth once at load.
    m_h12_create_committed = std::make_unique<PointerHook>(&dev_vtbl[k12dev_CreateCommittedResource], reinterpret_cast<void*>(&thunk_create_committed_resource_d3d12));
    m_h12_create_placed    = std::make_unique<PointerHook>(&dev_vtbl[k12dev_CreatePlacedResource],    reinterpret_cast<void*>(&thunk_create_placed_resource_d3d12));
    m_h12_create_reserved  = std::make_unique<PointerHook>(&dev_vtbl[k12dev_CreateReservedResource],  reinterpret_cast<void*>(&thunk_create_reserved_resource_d3d12));

    // BeginRenderPass carries the depth target for engines that bind scene/shadow
    // depth via render passes instead of OMSetRenderTargets (common on UE5 D3D12).
    // Only present on ID3D12GraphicsCommandList4+; gate on QI so we don't patch a
    // slot past the end of an older runtime's vtable.
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> list4{};
    if (SUCCEEDED(list.As(&list4)) && list4 != nullptr) {
        g_orig_begin_render_pass12.store(list_vtbl[k12_BeginRenderPass], std::memory_order_relaxed);
        m_h12_begin_render_pass = std::make_unique<PointerHook>(&list_vtbl[k12_BeginRenderPass], reinterpret_cast<void*>(&thunk_begin_render_pass_d3d12));
    }

    SPDLOG_INFO("[GameDepthCapture] D3D12 vtable hooks installed (per-draw + resource-registry scene-depth capture active; render-pass hook={})",
                m_h12_begin_render_pass != nullptr);
}

void GameDepthCapture::handle_create_dsv_d3d12(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    // DSV heap slots get recycled; latest write wins. Map to the resolved entry
    // (or nullptr when the resource is not a depth candidate, which clears any
    // stale mapping for that recycled slot).
    D3D12DepthEntry* entry = nullptr;

    if (resource != nullptr) {
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        // DIAG: dump the first several DSV resource descs so we can see exactly
        // what the game creates depth-stencil views over (dim / DXGI format /
        // size / MSAA), and whether our classifier accepts it.
        static std::atomic<int> s_dsv_log{0};
        if (s_dsv_log.fetch_add(1, std::memory_order_relaxed) < 24) {
            SPDLOG_INFO("[GameDepthCapture] D3D12 CreateDSV: dim={} fmt={} {}x{} samples={} depth={}",
                        (int)desc.Dimension, (int)desc.Format, (uint32_t)desc.Width, (uint32_t)desc.Height,
                        desc.SampleDesc.Count,
                        (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && is_depth_format(desc.Format)) ? 1 : 0);
        }
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && is_depth_format(desc.Format)) {
            std::scoped_lock lk(m_d12_mutex);
            // Find existing entry for this resource (many DSVs may alias it).
            for (int i = 0; i < m_d12_entry_count; ++i) {
                if (m_d12_entries[i].resource == resource) {
                    entry = &m_d12_entries[i];
                    break;
                }
            }
            if (entry == nullptr && m_d12_entry_count < (int)kMaxDepthEntries) {
                entry = &m_d12_entries[m_d12_entry_count++];
                entry->resource = resource;
                entry->w = (uint32_t)desc.Width;
                entry->h = (uint32_t)desc.Height;
                entry->fmt = desc.Format;
                entry->draw_count.store(0, std::memory_order_relaxed);
            }
            m_d12_handle_map[dest.ptr] = entry;
            return;
        }
    }

    // Non-depth / null resource: clear any stale mapping for this slot.
    std::scoped_lock lk(m_d12_mutex);
    m_d12_handle_map[dest.ptr] = nullptr;
}

void GameDepthCapture::handle_create_resource_d3d12(const D3D12_RESOURCE_DESC* desc, ID3D12Resource* resource) {
    if (desc == nullptr || resource == nullptr) {
        return;
    }
    // Depth candidacy keys off the DEPTH_STENCIL flag, not the format: the
    // resource-level format is frequently typeless (R24G8/R32/R16_TYPELESS).
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
        return;
    }

    // DIAG: dump the first several depth-capable resources so we can see whether
    // the game's scene depth is created AFTER our hook (registered here) or not
    // at all (created at load, before injection — needs an even earlier hook).
    static std::atomic<int> s_res_log{0};
    if (s_res_log.fetch_add(1, std::memory_order_relaxed) < 24) {
        SPDLOG_INFO("[GameDepthCapture] D3D12 depth resource created: fmt={} {}x{} samples={} flags={:x}",
                    (int)desc->Format, (uint32_t)desc->Width, (uint32_t)desc->Height,
                    desc->SampleDesc.Count, (uint32_t)desc->Flags);
    }

    std::scoped_lock lk(m_d12_mutex);
    for (int i = 0; i < m_d12_entry_count; ++i) {
        if (m_d12_entries[i].resource == resource) {
            if (m_d12_entries[i].hold == nullptr) {
                m_d12_entries[i].hold = resource; // upgrade a DSV-only entry to held
            }
            return;
        }
    }
    if (m_d12_entry_count < (int)kMaxDepthEntries) {
        auto& e = m_d12_entries[m_d12_entry_count++];
        e.resource = resource;
        e.hold = resource;              // AddRef: keep alive so the fallback publish is crash-safe
        e.w = (uint32_t)desc->Width;
        e.h = desc->Height;
        e.fmt = desc->Format;
        e.draw_count.store(0, std::memory_order_relaxed);
    }
}

void GameDepthCapture::handle_om_set_rts_d3d12(ID3D12GraphicsCommandList* list, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    D3D12DepthEntry* entry = nullptr;
    if (dsv != nullptr) {
        g_d12_om_binds.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lk(m_d12_mutex);
        auto it = m_d12_handle_map.find(dsv->ptr);
        if (it != m_d12_handle_map.end()) {
            entry = it->second;
        }
    }
    if (entry != nullptr) {
        g_d12_om_resolved.fetch_add(1, std::memory_order_relaxed);
    }
    slot_set(m_d12_lists, list, entry);
}

void GameDepthCapture::handle_begin_render_pass_d3d12(ID3D12GraphicsCommandList* list, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds) {
    if (ds == nullptr) {
        return; // depthless render pass: leave the list's current attribution intact
    }
    g_d12_om_binds.fetch_add(1, std::memory_order_relaxed);
    D3D12DepthEntry* entry = nullptr;
    {
        std::scoped_lock lk(m_d12_mutex);
        auto it = m_d12_handle_map.find(ds->cpuDescriptor.ptr);
        if (it != m_d12_handle_map.end()) {
            entry = it->second;
        }
    }
    // Only (re)bind when we recognize the depth. An unknown handle here means its
    // DSV predates our hook — the resource-registry fallback handles that case,
    // so don't wipe a good attribution by slotting null.
    if (entry != nullptr) {
        g_d12_om_resolved.fetch_add(1, std::memory_order_relaxed);
        slot_set(m_d12_lists, list, entry);
    }
}

void GameDepthCapture::handle_clear_dsv_d3d12(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE dsv) {
    // A depth clear also (re)establishes which depth this list is targeting —
    // engines that clear-then-draw are attributed correctly even if the bind
    // came through a path we didn't observe. OMSetRenderTargets overrides.
    D3D12DepthEntry* entry = nullptr;
    {
        std::scoped_lock lk(m_d12_mutex);
        auto it = m_d12_handle_map.find(dsv.ptr);
        if (it != m_d12_handle_map.end()) {
            entry = it->second;
        }
    }
    if (entry != nullptr) {
        slot_set(m_d12_lists, list, entry);
    }
}

void GameDepthCapture::handle_draw_d3d12(ID3D12GraphicsCommandList* list) {
    // Lock-free hot path: a couple of atomic loads + one relaxed fetch_add.
    void* v = slot_get(m_d12_lists, list);
    if (v != nullptr) {
        static_cast<D3D12DepthEntry*>(v)->draw_count.fetch_add(1, std::memory_order_relaxed);
    }
}

void GameDepthCapture::end_frame_d3d12(uint32_t render_w, uint32_t render_h) {
    D3D12DepthEntry* winner = nullptr;
    uint64_t winner_draws = 0;
    uint32_t win_w = 0, win_h = 0;
    DXGI_FORMAT win_fmt = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource* win_res = nullptr;

    // Diagnostic snapshot (logged throttled below).
    int diag_entries = 0;
    int diag_held = 0;
    size_t diag_map = 0;
    uint64_t diag_total_draws = 0, diag_best_draws = 0;
    uint32_t diag_bw = 0, diag_bh = 0;
    bool used_fallback = false;

    {
        std::scoped_lock lk(m_d12_mutex);

        uint64_t max_area = 0;
        for (int i = 0; i < m_d12_entry_count; ++i) {
            const uint64_t area = (uint64_t)m_d12_entries[i].w * m_d12_entries[i].h;
            if (area > max_area) max_area = area;
        }

        // Pass 1: entries whose dimensions match render resolution.
        if (render_w != 0 && render_h != 0) {
            const uint64_t target = (uint64_t)render_w * render_h;
            for (int i = 0; i < m_d12_entry_count; ++i) {
                auto& e = m_d12_entries[i];
                const uint64_t draws = e.draw_count.load(std::memory_order_relaxed);
                if (draws == 0) continue;
                const uint64_t area = (uint64_t)e.w * e.h;
                const bool res_match =
                    (e.h == render_h) || (e.w == render_w) ||
                    (e.w == render_w * 2) ||
                    ((uint64_t)llabs((long long)area - (long long)target) <= target * 15 / 100);
                if (res_match && draws > winner_draws) {
                    winner = &e; winner_draws = draws;
                }
            }
        }

        // Pass 2: fall back to global max draws, excluding tiny targets
        // (shadow maps etc.) via a 25%-of-largest area floor.
        if (winner == nullptr) {
            const uint64_t area_floor = max_area / 4;
            for (int i = 0; i < m_d12_entry_count; ++i) {
                auto& e = m_d12_entries[i];
                const uint64_t draws = e.draw_count.load(std::memory_order_relaxed);
                if (draws == 0) continue;
                const uint64_t area = (uint64_t)e.w * e.h;
                if (area < area_floor) continue;
                if (draws > winner_draws) {
                    winner = &e; winner_draws = draws;
                }
            }
        }

        // Pass 3: resource-registry fallback. NO depth received attributed draws
        // this frame — the game binds/clears depth through a path we can't
        // observe (e.g. a render pass whose DSV predates our hook). Publish the
        // registered depth-capable resource whose size best matches render res.
        // Restricted to held (resource-registered) entries so the publish AddRef
        // is crash-safe, and to known render res so we never pick a shadow atlas.
        if (winner == nullptr && render_w != 0 && render_h != 0) {
            const uint64_t target = (uint64_t)render_w * render_h;
            uint64_t best_area = 0;
            for (int i = 0; i < m_d12_entry_count; ++i) {
                auto& e = m_d12_entries[i];
                if (e.hold == nullptr) continue;
                const uint64_t area = (uint64_t)e.w * e.h;
                const bool res_match =
                    (e.h == render_h) || (e.w == render_w) ||
                    (e.w == render_w * 2) ||
                    ((uint64_t)llabs((long long)area - (long long)target) <= target * 15 / 100);
                if (res_match && area > best_area) {
                    winner = &e; best_area = area; used_fallback = true;
                }
            }
        }

        if (winner != nullptr) {
            win_w = winner->w; win_h = winner->h; win_fmt = winner->fmt;
            win_res = winner->resource;
        }

        // Snapshot registry state for the throttled diagnostic (pre-reset).
        diag_entries = m_d12_entry_count;
        diag_map = m_d12_handle_map.size();
        for (int i = 0; i < m_d12_entry_count; ++i) {
            if (m_d12_entries[i].hold != nullptr) ++diag_held;
            const uint64_t d = m_d12_entries[i].draw_count.load(std::memory_order_relaxed);
            diag_total_draws += d;
            if (d > diag_best_draws) {
                diag_best_draws = d;
                diag_bw = m_d12_entries[i].w;
                diag_bh = m_d12_entries[i].h;
            }
        }

        // Reset all per-frame counters for the next frame.
        for (int i = 0; i < m_d12_entry_count; ++i) {
            m_d12_entries[i].draw_count.store(0, std::memory_order_relaxed);
        }
    }

    // Throttled visibility (~every 2s @ 60fps). If this line NEVER appears,
    // end_frame_d3d12 is not being called (depth feature gate upstream). If
    // entries=0, CreateDepthStencilView never saw a depth-format TEXTURE2D. If
    // entries>0 but total_draws=0, binds/draws aren't being attributed.
    static uint32_t s_diag12 = 0;
    if ((s_diag12++ % 120u) == 0) {
        SPDLOG_INFO("[GameDepthCapture] D3D12 end_frame: entries={} held={} map={} total_draws={} "
                    "best={}x{}(draws={}) om_binds={} om_resolved={} render={}x{} published={} via={}",
                    diag_entries, diag_held, (int)diag_map, diag_total_draws, diag_bw, diag_bh, diag_best_draws,
                    g_d12_om_binds.load(std::memory_order_relaxed),
                    g_d12_om_resolved.load(std::memory_order_relaxed),
                    render_w, render_h, win_res != nullptr ? 1 : 0,
                    win_res == nullptr ? "none" : (used_fallback ? "resource-fallback" : "draws"));
    }

    if (winner == nullptr || win_res == nullptr) {
        return; // keep the previously published resource across an empty frame
    }

    {
        std::scoped_lock lk(m_publish_mutex);
        m_published = win_res; // ComPtr<IUnknown> assignment AddRefs
        m_published_via_draws.store(!used_fallback, std::memory_order_relaxed);
    }

    if (win_res != m_d12_last_logged) {
        m_d12_last_logged = win_res;
        SPDLOG_INFO("[GameDepthCapture] D3D12 scene-depth published: {}x{} fmt={} draws={}",
                    win_w, win_h, (int)win_fmt, winner_draws);
    }
}

// ---- D3D12 static thunks ---------------------------------------------------
void STDMETHODCALLTYPE GameDepthCapture::thunk_create_dsv_d3d12(
    ID3D12Device* self, ID3D12Resource* pResource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    auto orig = reinterpret_cast<PFN_CreateDSV12>(g_orig_create_dsv12.load(std::memory_order_relaxed));
    if (orig != nullptr) {
        orig(self, pResource, pDesc, DestDescriptor); // create the DSV first
    }
    get().handle_create_dsv_d3d12(pResource, DestDescriptor);
}

// Recover the created resource (QI, not a raw cast: the caller may have asked for
// a derived IID) and register it if it is a depth-capable 2D texture. Filtering
// on the desc first keeps the hot path (buffers, color RTs) at two field reads.
void GameDepthCapture::register_if_depth(const D3D12_RESOURCE_DESC* pDesc, HRESULT hr, void** ppvResource) {
    if (FAILED(hr) || pDesc == nullptr || ppvResource == nullptr || *ppvResource == nullptr) {
        return;
    }
    if (pDesc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> res{};
    if (SUCCEEDED(reinterpret_cast<IUnknown*>(*ppvResource)->QueryInterface(IID_PPV_ARGS(&res))) && res != nullptr) {
        GameDepthCapture::get().handle_create_resource_d3d12(pDesc, res.Get());
    }
}

HRESULT STDMETHODCALLTYPE GameDepthCapture::thunk_create_committed_resource_d3d12(
    ID3D12Device* self, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource) {
    auto orig = reinterpret_cast<PFN_CreateCommittedResource12>(g_orig_create_committed12.load(std::memory_order_relaxed));
    if (orig == nullptr) {
        return E_FAIL;
    }
    const HRESULT hr = orig(self, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                            pOptimizedClearValue, riidResource, ppvResource);
    register_if_depth(pDesc, hr, ppvResource);
    return hr;
}

HRESULT STDMETHODCALLTYPE GameDepthCapture::thunk_create_placed_resource_d3d12(
    ID3D12Device* self, ID3D12Heap* pHeap, UINT64 HeapOffset,
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) {
    auto orig = reinterpret_cast<PFN_CreatePlacedResource12>(g_orig_create_placed12.load(std::memory_order_relaxed));
    if (orig == nullptr) {
        return E_FAIL;
    }
    const HRESULT hr = orig(self, pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);
    register_if_depth(pDesc, hr, ppvResource);
    return hr;
}

HRESULT STDMETHODCALLTYPE GameDepthCapture::thunk_create_reserved_resource_d3d12(
    ID3D12Device* self, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) {
    auto orig = reinterpret_cast<PFN_CreateReservedResource12>(g_orig_create_reserved12.load(std::memory_order_relaxed));
    if (orig == nullptr) {
        return E_FAIL;
    }
    const HRESULT hr = orig(self, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);
    register_if_depth(pDesc, hr, ppvResource);
    return hr;
}

void STDMETHODCALLTYPE GameDepthCapture::thunk_om_set_rts_d3d12(
    ID3D12GraphicsCommandList* self, UINT NumRenderTargetDescriptors,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
    BOOL RTsSingleHandleToDescriptorRange,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) {
    get().handle_om_set_rts_d3d12(self, pDepthStencilDescriptor);
    auto orig = reinterpret_cast<PFN_OMSetRTs12>(g_orig_om_set_rts12.load(std::memory_order_relaxed));
    if (orig != nullptr) {
        orig(self, NumRenderTargetDescriptors, pRenderTargetDescriptors,
             RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
    }
}

void STDMETHODCALLTYPE GameDepthCapture::thunk_begin_render_pass_d3d12(
    ID3D12GraphicsCommandList* self, UINT NumRenderTargets,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pRenderTargets,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDepthStencil, D3D12_RENDER_PASS_FLAGS Flags) {
    get().handle_begin_render_pass_d3d12(self, pDepthStencil);
    auto orig = reinterpret_cast<PFN_BeginRenderPass12>(g_orig_begin_render_pass12.load(std::memory_order_relaxed));
    if (orig != nullptr) {
        orig(self, NumRenderTargets, pRenderTargets, pDepthStencil, Flags);
    }
}

void STDMETHODCALLTYPE GameDepthCapture::thunk_clear_dsv_d3d12(
    ID3D12GraphicsCommandList* self, D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView,
    D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) {
    get().handle_clear_dsv_d3d12(self, DepthStencilView);
    auto orig = reinterpret_cast<PFN_ClearDSV12>(g_orig_clear_dsv12.load(std::memory_order_relaxed));
    if (orig != nullptr) {
        orig(self, DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
    }
}

void STDMETHODCALLTYPE GameDepthCapture::thunk_draw_instanced_d3d12(
    ID3D12GraphicsCommandList* self, UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation) {
    get().handle_draw_d3d12(self);
    auto orig = reinterpret_cast<PFN_DrawInstanced12>(g_orig_draw12.load(std::memory_order_relaxed));
    if (orig != nullptr) {
        orig(self, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    }
}

void STDMETHODCALLTYPE GameDepthCapture::thunk_draw_indexed_instanced_d3d12(
    ID3D12GraphicsCommandList* self, UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) {
    get().handle_draw_d3d12(self);
    auto orig = reinterpret_cast<PFN_DrawIndexedInstanced12>(g_orig_draw_indexed12.load(std::memory_order_relaxed));
    if (orig != nullptr) {
        orig(self, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    }
}

// ===========================================================================
// D3D11
// ===========================================================================
void GameDepthCapture::ensure_installed_d3d11(ID3D11Device* device) {
    if (m_installed_d3d11 || device == nullptr) {
        return;
    }
    m_installed_d3d11 = true;

    // The immediate context gives us the shared ID3D11DeviceContext vtable
    // (deferred contexts share it). Release our temp ref after grabbing it.
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx{};
    device->GetImmediateContext(&ctx);
    if (ctx == nullptr) {
        SPDLOG_ERROR("[GameDepthCapture] D3D11: GetImmediateContext returned null - depth capture not installed");
        return;
    }

    void** vtbl = *reinterpret_cast<void***>(ctx.Get());
    if (vtbl == nullptr) {
        SPDLOG_ERROR("[GameDepthCapture] D3D11: null vtable - depth capture not installed");
        return;
    }

    g_orig_om_set_rts11.store(vtbl[k11_OMSetRenderTargets], std::memory_order_relaxed);
    g_orig_clear_dsv11.store(vtbl[k11_ClearDepthStencilView], std::memory_order_relaxed);
    g_orig_draw_indexed11.store(vtbl[k11_DrawIndexed], std::memory_order_relaxed);
    g_orig_draw11.store(vtbl[k11_Draw], std::memory_order_relaxed);
    g_orig_draw_indexed_instanced11.store(vtbl[k11_DrawIndexedInstanced], std::memory_order_relaxed);
    g_orig_draw_instanced11.store(vtbl[k11_DrawInstanced], std::memory_order_relaxed);

    m_h11_om_set_rts               = std::make_unique<PointerHook>(&vtbl[k11_OMSetRenderTargets],    reinterpret_cast<void*>(&thunk_om_set_rts_d3d11));
    m_h11_clear_dsv                = std::make_unique<PointerHook>(&vtbl[k11_ClearDepthStencilView], reinterpret_cast<void*>(&thunk_clear_dsv_d3d11));
    m_h11_draw_indexed             = std::make_unique<PointerHook>(&vtbl[k11_DrawIndexed],           reinterpret_cast<void*>(&thunk_draw_indexed_d3d11));
    m_h11_draw                     = std::make_unique<PointerHook>(&vtbl[k11_Draw],                  reinterpret_cast<void*>(&thunk_draw_d3d11));
    m_h11_draw_indexed_instanced   = std::make_unique<PointerHook>(&vtbl[k11_DrawIndexedInstanced],  reinterpret_cast<void*>(&thunk_draw_indexed_instanced_d3d11));
    m_h11_draw_instanced           = std::make_unique<PointerHook>(&vtbl[k11_DrawInstanced],         reinterpret_cast<void*>(&thunk_draw_instanced_d3d11));

    SPDLOG_INFO("[GameDepthCapture] D3D11 vtable hooks installed (per-draw scene-depth counting active)");
}

GameDepthCapture::D3D11DepthEntry* GameDepthCapture::d3d11_entry_for(ID3D11DepthStencilView* dsv) {
    // Caller holds m_d11_mutex.
    Microsoft::WRL::ComPtr<ID3D11Resource> res{};
    dsv->GetResource(&res);
    if (res == nullptr) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex{};
    if (FAILED(res.As(&tex)) || tex == nullptr) {
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    if (!is_depth_format(td.Format)) {
        return nullptr;
    }

    ID3D11Texture2D* key = tex.Get(); // identity key only (not held)
    for (int i = 0; i < m_d11_entry_count; ++i) {
        if (m_d11_entries[i].resource == key) {
            return &m_d11_entries[i];
        }
    }
    if (m_d11_entry_count < (int)kMaxDepthEntries) {
        auto* e = &m_d11_entries[m_d11_entry_count++];
        e->resource = key;
        e->w = td.Width;
        e->h = td.Height;
        e->fmt = td.Format;
        e->draw_count.store(0, std::memory_order_relaxed);
        return e;
    }
    return nullptr;
}

void GameDepthCapture::handle_om_set_rts_d3d11(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv) {
    D3D11DepthEntry* entry = nullptr;
    if (dsv != nullptr) {
        std::scoped_lock lk(m_d11_mutex);
        entry = d3d11_entry_for(dsv);
    }
    slot_set(m_d11_ctxs, ctx, entry);
}

void GameDepthCapture::handle_clear_dsv_d3d11(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv) {
    if (dsv == nullptr) {
        return;
    }
    D3D11DepthEntry* entry = nullptr;
    {
        std::scoped_lock lk(m_d11_mutex);
        entry = d3d11_entry_for(dsv);
    }
    if (entry != nullptr) {
        slot_set(m_d11_ctxs, ctx, entry);
    }
}

void GameDepthCapture::handle_draw_d3d11(ID3D11DeviceContext* ctx) {
    void* v = slot_get(m_d11_ctxs, ctx);
    if (v != nullptr) {
        static_cast<D3D11DepthEntry*>(v)->draw_count.fetch_add(1, std::memory_order_relaxed);
    }
}

void GameDepthCapture::end_frame_d3d11(uint32_t render_w, uint32_t render_h) {
    D3D11DepthEntry* winner = nullptr;
    uint64_t winner_draws = 0;
    uint32_t win_w = 0, win_h = 0;
    DXGI_FORMAT win_fmt = DXGI_FORMAT_UNKNOWN;
    ID3D11Texture2D* win_res = nullptr;

    {
        std::scoped_lock lk(m_d11_mutex);

        uint64_t max_area = 0;
        for (int i = 0; i < m_d11_entry_count; ++i) {
            const uint64_t area = (uint64_t)m_d11_entries[i].w * m_d11_entries[i].h;
            if (area > max_area) max_area = area;
        }

        if (render_w != 0 && render_h != 0) {
            const uint64_t target = (uint64_t)render_w * render_h;
            for (int i = 0; i < m_d11_entry_count; ++i) {
                auto& e = m_d11_entries[i];
                const uint64_t draws = e.draw_count.load(std::memory_order_relaxed);
                if (draws == 0) continue;
                const uint64_t area = (uint64_t)e.w * e.h;
                const bool res_match =
                    (e.h == render_h) || (e.w == render_w) ||
                    (e.w == render_w * 2) ||
                    ((uint64_t)llabs((long long)area - (long long)target) <= target * 15 / 100);
                if (res_match && draws > winner_draws) {
                    winner = &e; winner_draws = draws;
                }
            }
        }

        if (winner == nullptr) {
            const uint64_t area_floor = max_area / 4;
            for (int i = 0; i < m_d11_entry_count; ++i) {
                auto& e = m_d11_entries[i];
                const uint64_t draws = e.draw_count.load(std::memory_order_relaxed);
                if (draws == 0) continue;
                const uint64_t area = (uint64_t)e.w * e.h;
                if (area < area_floor) continue;
                if (draws > winner_draws) {
                    winner = &e; winner_draws = draws;
                }
            }
        }

        if (winner != nullptr) {
            win_w = winner->w; win_h = winner->h; win_fmt = winner->fmt;
            win_res = winner->resource;
        }

        for (int i = 0; i < m_d11_entry_count; ++i) {
            m_d11_entries[i].draw_count.store(0, std::memory_order_relaxed);
        }
    }

    if (winner == nullptr || win_res == nullptr) {
        return;
    }

    {
        std::scoped_lock lk(m_publish_mutex);
        m_published = win_res;
        // The D3D11 path has no resource-registry fallback — every publish is a
        // draw-attributed winner.
        m_published_via_draws.store(true, std::memory_order_relaxed);
    }

    if (win_res != m_d11_last_logged) {
        m_d11_last_logged = win_res;
        SPDLOG_INFO("[GameDepthCapture] D3D11 scene-depth published: {}x{} fmt={} draws={}",
                    win_w, win_h, (int)win_fmt, winner_draws);
    }
}

// ---- D3D11 static thunks ---------------------------------------------------
void STDMETHODCALLTYPE GameDepthCapture::thunk_om_set_rts_d3d11(
    ID3D11DeviceContext* self, UINT NumViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView) {
    get().handle_om_set_rts_d3d11(self, pDepthStencilView);
    auto orig = reinterpret_cast<PFN_OMSetRTs11>(g_orig_om_set_rts11.load(std::memory_order_relaxed));
    if (orig != nullptr) {
        orig(self, NumViews, ppRenderTargetViews, pDepthStencilView);
    }
}

void STDMETHODCALLTYPE GameDepthCapture::thunk_clear_dsv_d3d11(
    ID3D11DeviceContext* self, ID3D11DepthStencilView* pDepthStencilView,
    UINT ClearFlags, FLOAT Depth, UINT8 Stencil) {
    get().handle_clear_dsv_d3d11(self, pDepthStencilView);
    auto orig = reinterpret_cast<PFN_ClearDSV11>(g_orig_clear_dsv11.load(std::memory_order_relaxed));
    if (orig != nullptr) {
        orig(self, pDepthStencilView, ClearFlags, Depth, Stencil);
    }
}

void STDMETHODCALLTYPE GameDepthCapture::thunk_draw_indexed_d3d11(
    ID3D11DeviceContext* self, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) {
    get().handle_draw_d3d11(self);
    auto orig = reinterpret_cast<PFN_DrawIndexed11>(g_orig_draw_indexed11.load(std::memory_order_relaxed));
    if (orig != nullptr) {
        orig(self, IndexCount, StartIndexLocation, BaseVertexLocation);
    }
}

void STDMETHODCALLTYPE GameDepthCapture::thunk_draw_d3d11(
    ID3D11DeviceContext* self, UINT VertexCount, UINT StartVertexLocation) {
    get().handle_draw_d3d11(self);
    auto orig = reinterpret_cast<PFN_Draw11>(g_orig_draw11.load(std::memory_order_relaxed));
    if (orig != nullptr) {
        orig(self, VertexCount, StartVertexLocation);
    }
}

void STDMETHODCALLTYPE GameDepthCapture::thunk_draw_indexed_instanced_d3d11(
    ID3D11DeviceContext* self, UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) {
    get().handle_draw_d3d11(self);
    auto orig = reinterpret_cast<PFN_DrawIndexedInstanced11>(g_orig_draw_indexed_instanced11.load(std::memory_order_relaxed));
    if (orig != nullptr) {
        orig(self, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    }
}

void STDMETHODCALLTYPE GameDepthCapture::thunk_draw_instanced_d3d11(
    ID3D11DeviceContext* self, UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation) {
    get().handle_draw_d3d11(self);
    auto orig = reinterpret_cast<PFN_DrawInstanced11>(g_orig_draw_instanced11.load(std::memory_order_relaxed));
    if (orig != nullptr) {
        orig(self, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    }
}

// ===========================================================================
// Shared
// ===========================================================================
void GameDepthCapture::reset() {
    std::scoped_lock lk(m_publish_mutex);
    m_published.Reset();
    m_d12_last_logged = nullptr;
    m_d11_last_logged = nullptr;
}
