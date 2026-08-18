#pragma once

// Host-side plumbing the LeiaSR weave needs before any SR SDK entry point is
// touched. Deliberately free of SR headers — it is pure Win32 — so both the
// D3D11 and D3D12 compositors can include it regardless of which weaver header
// their __has_include probe found.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vrmod::flat3d::leiasr {

#ifndef _DPI_AWARENESS_CONTEXTS_
DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

// Probe whether the SR runtime DLLs actually resolve, BEFORE calling into the
// SDK. Every SR DLL is delay-loaded (srlib_apply_delayload in cmake.toml), so
// the first call into one that isn't on disk raises an SEH exception from the
// delay-load helper rather than failing cleanly. LoadLibraryW just returns
// null, which is something we can branch on.
//
// The backend weaver DLL has to be probed, not only the core runtime: a machine
// can have the core present while SimulatedRealityDirectX.dll is missing, and a
// core-only probe would wave that through into an SEH inside CreateDX11Weaver /
// CreateDX12Weaver — exactly what this exists to prevent. It is also the
// strongest single check available, since SimulatedRealityDirectX.dll statically
// imports DimencoWeaving, SimulatedRealityCore, SimulatedRealityDisplays,
// SimulatedRealityFaceTrackers and opencv_world343, so a successful load
// resolves the whole chain. Core is probed anyway: that dependency set is an
// implementation detail of one SDK build, and it costs a refcount bump.
//
// No FreeLibrary — we are about to use these, and something else in the process
// may already depend on what we just loaded.
inline bool sr_runtime_available() {
    static const bool available = [] {
        return LoadLibraryW(L"SimulatedRealityDirectX.dll") != nullptr &&
               LoadLibraryW(L"SimulatedRealityCore.dll") != nullptr;
    }();

    return available;
}

// The SR runtime maps the lenticular weave onto PHYSICAL panel pixels via the
// window handle — every SR SDK sample sets PROCESS_PER_MONITOR_DPI_AWARE at
// startup for exactly this reason. When the host game is NOT per-monitor-aware
// (most UE titles), the runtime's window/monitor queries return DPI-virtualized
// coordinates, so on a scaled display the weave is confined to a sub-region of
// the panel (symptom: only the top-left corner weaves). Main.cpp requests
// process-wide awareness at inject time and that is the primary fix; this backs
// it up per-thread around the weaver's create + weave calls for when that
// couldn't take (e.g. injected after the game made its window). No-op when the
// API is unavailable (pre-Win10 1607) or the thread is already aware.
struct ScopedPerMonitorDpi {
    using SetCtxFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    SetCtxFn set_ctx{nullptr};
    DPI_AWARENESS_CONTEXT prev{nullptr};

    ScopedPerMonitorDpi() {
        if (auto* user32 = GetModuleHandleW(L"user32.dll")) {
            set_ctx = (SetCtxFn)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
        }
        if (set_ctx != nullptr) {
            prev = set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
    ~ScopedPerMonitorDpi() {
        if (set_ctx != nullptr && prev != nullptr) {
            set_ctx(prev);
        }
    }
    ScopedPerMonitorDpi(const ScopedPerMonitorDpi&) = delete;
    ScopedPerMonitorDpi& operator=(const ScopedPerMonitorDpi&) = delete;
};

} // namespace vrmod::flat3d::leiasr
