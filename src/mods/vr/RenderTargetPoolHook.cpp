#include <windows.h>

#include <spdlog/spdlog.h>

#include <utility/Scan.hpp>
#include <utility/String.hpp>

#include <sdk/FRenderTargetPool.hpp>
#include <sdk/EngineModule.hpp>
#include <sdk/Utility.hpp>
#include <sdk/threading/RHIThreadWorker.hpp>

#include "../VR.hpp"
#include "../../utility/Logging.hpp"
#include "RenderTargetPoolHook.hpp"

RenderTargetPoolHook* g_hook{nullptr};

namespace {
bool is_ue_5_1() {
    static const bool result = []() {
        const auto found_version = sdk::search_for_version(GetModuleHandleW(nullptr));

        if (found_version) {
            const auto version = utility::narrow(*found_version);
            return version == "5.1" || version.starts_with("5.1.");
        }

        const auto disk_version = sdk::get_file_version_info();
        return disk_version.dwFileVersionMS == 0x00050001;
    }();

    return result;
}
}

RenderTargetPoolHook::RenderTargetPoolHook() {
    g_hook = this;
}

void RenderTargetPoolHook::on_pre_engine_tick(sdk::UGameEngine* engine, float delta) {
    // Flat3D defaults to the API-level GameDepthCapture for scene depth, but can
    // opt into this engine pool hook (Flat3D_UseEngineDepth) for titles whose
    // depth is allocated at load and thus invisible to the API path (e.g. SMT5V).
    const bool flat3d_engine_depth = VR::get()->is_using_flat3d() && VR::get()->flat3d_use_engine_depth();

    if (!m_attempted_hook && (VR::get()->is_depth_enabled() || flat3d_engine_depth)) {
        m_wants_activate = true;
    }

    // Installing the inline hook mid-game on FindFreeElement crashes some titles
    // (e.g. Jedi: Survivor), so in Flat3D it is gated behind the opt-in toggle;
    // the VR depth-submission path installs it whenever VR depth is enabled.
    if (!m_attempted_hook && m_wants_activate && (!VR::get()->is_using_flat3d() || flat3d_engine_depth)) {
        m_attempted_hook = true;
        m_hooked = hook();
    }
}

bool RenderTargetPoolHook::hook() {
    SPDLOG_INFO("Attempting to hook RenderTargetPool::FindFreeElement");

    const auto is_ue5 = VR::get()->get_fake_stereo_hook()->has_double_precision();
    const auto find_free_element = sdk::FRenderTargetPool::get_find_free_element_fn(is_ue5);

    if (is_ue5 && is_ue_5_1()) {
        SPDLOG_WARN("[UE5.1] Skipping RenderTargetPool::FindFreeElement hook; UE 5.1 has multiple FindFreeElement overloads and depth pass-through can crash during level load");
        return false;
    }

    if (!find_free_element) {
        SPDLOG_ERROR("Failed to find FRenderTargetPool::FindFreeElement, cannot hook");
        return false;
    }

    /*if (VR::get()->get_fake_stereo_hook()->has_double_precision()) {
        spdlog::error("Render target pool hook is temporarily disabled on UE5, sorry :(");
        return false;
    }*/

    SPDLOG_INFO("Performing hook...");

    if (is_ue5) {
        m_find_free_element_hook = safetyhook::create_inline((void*)*find_free_element, find_free_element_hook_ue5);
    } else {
        m_find_free_element_hook = safetyhook::create_inline((void*)*find_free_element, find_free_element_hook);
    }

    if (m_find_free_element_hook) {
        SPDLOG_INFO("Successfully hooked RenderTargetPool::FindFreeElement");
    } else {
        SPDLOG_ERROR("Failed to hook RenderTargetPool::FindFreeElement");
    }

    return true;
}

// IPooledRenderTarget is refcounted (IRefCountedObject: AddRef / Release /
// GetRefCount). The pool deletes an element once only the pool itself holds a
// reference, and that happens on every resolution change: the scene targets
// reallocate, FreeUnusedResources runs, and a raw pointer cached here dangles
// while its memory still reads as a perfectly sane texture. RAIN CODE: the
// depth readback created a view over the freed SceneDepthZ and the NVIDIA
// driver's worker thread died 6 ms later on a nulled internal field. Holding
// our own reference keeps the element alive until the next allocation of that
// name replaces it, so the worst case is a few frames of stale depth.
//
// SEH-guarded with plain arguments only: this hook can be reached with shifted
// arguments (Jedi Survivor), and a virtual call through a sentinel must bail
// where a readability check would, not take the game down.
__declspec(noinline) static bool rtpool_addref_guarded(IPooledRenderTarget* rt) {
    __try {
        if (rt == nullptr || IsBadReadPtr(rt, sizeof(void*)) ||
            IsBadReadPtr(*reinterpret_cast<void**>(rt), 3 * sizeof(void*))) {
            return false;
        }
        rt->AddRef();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) static void rtpool_release_guarded(IPooledRenderTarget* rt) {
    __try {
        if (rt != nullptr && !IsBadReadPtr(rt, sizeof(void*)) &&
            !IsBadReadPtr(*reinterpret_cast<void**>(rt), 3 * sizeof(void*))) {
            rt->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void RenderTargetPoolHook::release_all_locked() {
    for (auto& [name, rt] : m_render_targets) {
        rtpool_release_guarded(rt);
    }
    m_render_targets.clear();
}

void RenderTargetPoolHook::on_post_find_free_element(
    sdk::FRenderTargetPool* pool, 
    sdk::FPooledRenderTargetDesc* desc, 
    TRefCountPtr<IPooledRenderTarget>* out, 
    const wchar_t* name)
{
    // Right now we are only using this for depth
    // and on some games it will crash if we mess with anything
    // so, TODO: fix the games that crash with depth enabled
    if (!m_wants_activate) {
        std::scoped_lock _{g_hook->m_mutex};
        release_all_locked();
        return;
    }

    // InDebugName is NOT guaranteed to be a valid string pointer. Shipping UE
    // builds strip RDG debug names and can pass a non-null sentinel here (Jedi
    // Survivor passes 0x1). A plain null check lets that through, and using it
    // as a std::wstring key below dereferences it (wcslen) -> AV reading 0x1.
    // Require an actually-readable pointer.
    if (name != nullptr && !IsBadReadPtr(name, sizeof(wchar_t))) {
        //SPDLOG_INFO("FRenderTargetPool::FindFreeElement called with name {}", utility::narrow(name));

        // `out` (the TRefCountPtr the engine fills in) can ALSO arrive as a
        // shifted-arg sentinel. Jedi Survivor's FindFreeElement is UE5-style
        // (no leading FRHICommandList), but has_double_precision() selected the
        // UE4 hook, so we read the args one slot over and `out` lands on the
        // stripped RDG InDebugName == 0x1 — out->reference below then AVs reading
        // 0x1. Bail on a non-readable out rather than fall through to erase
        // (name is suspect too when the signature doesn't match).
        if (out != nullptr && IsBadReadPtr(out, sizeof(void*))) {
            return;
        }

        std::scoped_lock _{g_hook->m_mutex};

        if (out != nullptr) {
            // Take our reference on the NEW element first, then drop the one
            // on whatever this name held before. The pool handing the same
            // element back (reuse) is the common case and changes nothing.
            IPooledRenderTarget* fresh = out->reference;
            auto& slot = g_hook->m_render_targets[name];
            if (slot != fresh) {
                if (fresh != nullptr && !rtpool_addref_guarded(fresh)) {
                    fresh = nullptr; // unreadable / sentinel: never cache it
                }
                rtpool_release_guarded(slot);
                slot = fresh;
            }
            if (slot == nullptr) {
                g_hook->m_render_targets.erase(name);
            }
        } else if (auto it = g_hook->m_render_targets.find(name); it != g_hook->m_render_targets.end()) {
            rtpool_release_guarded(it->second);
            g_hook->m_render_targets.erase(it);
        }

        if (!g_hook->m_seen_names.contains(name)) {
            g_hook->m_seen_names.insert(name);
            SPDLOG_INFO("FRenderTargetPool::FindFreeElement called with name {}", utility::narrow(name));
        }
    }
}

bool RenderTargetPoolHook::find_free_element_hook(
    sdk::FRenderTargetPool* pool, sdk::FRHICommandListBase* cmd_list,
    sdk::FPooledRenderTargetDesc* desc, TRefCountPtr<IPooledRenderTarget>* out,
    const wchar_t* name, 
    uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10)
{
    SPDLOG_INFO_ONCE("FRenderTargetPool::FindFreeElement (UE4) called for the first time!");

    const auto result = g_hook->m_find_free_element_hook.call<bool>(pool, cmd_list, desc, out, name, a6, a7, a8, a9, a10);

    SPDLOG_INFO_ONCE("Finished calling FRenderTargetPool::FindFreeElement!");

    g_hook->on_post_find_free_element(pool, desc, out, name);

    return result;
}

bool RenderTargetPoolHook::find_free_element_hook_ue5(
    sdk::FRenderTargetPool* pool,
    sdk::FPooledRenderTargetDesc* desc,
    TRefCountPtr<IPooledRenderTarget>* out,
    const wchar_t* name,
    // these arent uintptrs, just defending against future changes to the size of the params
    uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10)
{
    SPDLOG_INFO_ONCE("FRenderTargetPool::FindFreeElement (UE5) called for the first time!");

    const auto result = g_hook->m_find_free_element_hook.call<bool>(pool, desc, out, name, a5, a6, a7, a8, a9, a10);

    SPDLOG_INFO_ONCE("Finished calling FRenderTargetPool::FindFreeElement! (UE5)");

    g_hook->on_post_find_free_element(pool, desc, out, name);

    return result;
}
