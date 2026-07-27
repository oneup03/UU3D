// Flat 3D monitor mode ("FLAT3D" runtime) — initialization, per-frame
// parameter derivation, settings page, and keybinds. Kept in its own TU so
// the upstream VR.cpp diff stays minimal (rebase-friendliness).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>
#include <thread>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32")

#include <imgui.h>
#include <safetyhook.hpp>
#include <spdlog/spdlog.h>
#include <utility/Module.hpp>
#include <utility/ScopeGuard.hpp>
#include <utility/String.hpp>

#include <sdk/CVar.hpp>
#include <sdk/FBoolProperty.hpp>
#include <sdk/UFunction.hpp>
#include <sdk/UEngine.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <sdk/UObjectArray.hpp>
#include <sdk/threading/GameThreadWorker.hpp>
#include <sdk/APlayerController.hpp>
#include <sdk/APlayerCameraManager.hpp>
#include <sdk/UClass.hpp>

#include "Framework.hpp"
#include "PluginLoader.hpp"

#include "vr/GameDepthCapture.hpp"
#include "vr/flat3d/Flat3DAutoConvergence.hpp"
#include "vr/flat3d/Flat3DDisplayTiming.hpp"

#include "VR.hpp"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace {
// --- DPI spoof (Native Output) ----------------------------------------------
// Main.cpp makes the PROCESS per-monitor-DPI-aware at inject time — pixel-exact
// 3D output (interlaced, LeiaSR) needs the window and swapchain in PHYSICAL
// pixels. On a scaled display that flips UE's high-DPI path ON: the engine
// multiplies requested resolutions / window sizes by the monitor scale while
// our hooks pin the swapchain and window at native — a periodic tug-of-war
// (window re-kicks + swapchain churn + framework re-inits every round, engine
// fatals on some titles; seen on SMT5V, 4K @ 150%). Rather than compensate
// request-by-request, lie to the game about the DPI VALUE: every effective-DPI
// query returns 96, so the engine's scale is 1.0 and logical == physical ==
// native. Installed only when flat3d native output engages; our own code reads
// the real DPI through the hook originals.
std::atomic<bool> g_flat3d_dpi_spoof_active{false};
SafetyHookInline g_dpi_hook_for_window{};
SafetyHookInline g_dpi_hook_for_system{};
SafetyHookInline g_dpi_hook_for_monitor{};

UINT WINAPI flat3d_spoofed_get_dpi_for_window(HWND wnd) {
    if (g_flat3d_dpi_spoof_active.load(std::memory_order_relaxed)) {
        return 96;
    }

    return g_dpi_hook_for_window.unsafe_call<UINT>(wnd);
}

UINT WINAPI flat3d_spoofed_get_dpi_for_system() {
    if (g_flat3d_dpi_spoof_active.load(std::memory_order_relaxed)) {
        return 96;
    }

    return g_dpi_hook_for_system.unsafe_call<UINT>();
}

HRESULT WINAPI flat3d_spoofed_get_dpi_for_monitor(HMONITOR monitor, int type, UINT* dpi_x, UINT* dpi_y) {
    // MDT_EFFECTIVE_DPI == 0. RAW/ANGULAR queries (physical panel properties,
    // e.g. for the SR weaver) stay truthful.
    if (type == 0 && g_flat3d_dpi_spoof_active.load(std::memory_order_relaxed)) {
        if (dpi_x != nullptr) {
            *dpi_x = 96;
        }
        if (dpi_y != nullptr) {
            *dpi_y = 96;
        }

        return S_OK;
    }

    return g_dpi_hook_for_monitor.unsafe_call<HRESULT>(monitor, type, dpi_x, dpi_y);
}

// The real (unspoofed) DPI for our own logic — echo detection must keep seeing
// the true monitor scale.
UINT flat3d_real_dpi_for_window(HWND wnd) {
    if (g_dpi_hook_for_window) {
        return g_dpi_hook_for_window.unsafe_call<UINT>(wnd);
    }

    if (auto* user32 = GetModuleHandleW(L"user32.dll")) {
        using Fn = UINT(WINAPI*)(HWND);
        if (auto fn = (Fn)GetProcAddress(user32, "GetDpiForWindow")) {
            return fn(wnd);
        }
    }

    return 96;
}

bool flat3d_install_dpi_spoof() {
    static bool attempted = false;
    static bool installed = false;

    if (attempted) {
        return installed;
    }
    attempted = true;

    auto* user32 = GetModuleHandleW(L"user32.dll");
    auto* shcore = GetModuleHandleW(L"shcore.dll");

    const auto for_window = user32 != nullptr ? GetProcAddress(user32, "GetDpiForWindow") : nullptr;
    const auto for_system = user32 != nullptr ? GetProcAddress(user32, "GetDpiForSystem") : nullptr;
    const auto for_monitor = shcore != nullptr ? GetProcAddress(shcore, "GetDpiForMonitor") : nullptr;

    if (for_window != nullptr) {
        g_dpi_hook_for_window = safetyhook::create_inline((void*)for_window, (void*)&flat3d_spoofed_get_dpi_for_window);
    }
    if (for_system != nullptr) {
        g_dpi_hook_for_system = safetyhook::create_inline((void*)for_system, (void*)&flat3d_spoofed_get_dpi_for_system);
    }
    if (for_monitor != nullptr) {
        g_dpi_hook_for_monitor = safetyhook::create_inline((void*)for_monitor, (void*)&flat3d_spoofed_get_dpi_for_monitor);
    }

    // UE's per-monitor high-DPI path reads GetDpiForMonitor; per-window caches
    // and game-side code use GetDpiForWindow. Either is enough to hold the
    // engine at scale 1.0.
    installed = (bool)g_dpi_hook_for_monitor || (bool)g_dpi_hook_for_window;

    spdlog::info("[Flat3D] DPI spoof hooks: GetDpiForMonitor={} GetDpiForWindow={} GetDpiForSystem={}",
                 (bool)g_dpi_hook_for_monitor, (bool)g_dpi_hook_for_window, (bool)g_dpi_hook_for_system);

    return installed;
}

// UE only refreshes its cached per-window DPI scale from WM_DPICHANGED — the
// spoofed queries alone never clear a scale cached at window creation. Sent
// from the game thread (the window's owner) so it processes synchronously.
void flat3d_flush_engine_dpi_cache(HWND wnd, int x, int y, LONG w, LONG h) {
    GameThreadWorker::get().enqueue([wnd, x, y, w, h]() {
        RECT suggested{x, y, x + w, y + h};
        SendMessageW(wnd, WM_DPICHANGED, MAKEWPARAM(96, 96), (LPARAM)&suggested);
    });
}

// Current + native (max) resolution of the monitor hosting hwnd. Native is
// cached per display device (EnumDisplaySettings walk).
struct Flat3DDisplayInfo {
    int native_w{0};
    int native_h{0};
    int cur_w{0};
    int cur_h{0};
    int origin_x{0}; // monitor rect origin (for window placement)
    int origin_y{0};
    int refresh_hz{0}; // current display mode refresh rate
};

std::optional<Flat3DDisplayInfo> query_display_info(HWND hwnd) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof(mi);

    if (mon == nullptr || !GetMonitorInfoA(mon, &mi)) {
        return std::nullopt;
    }

    Flat3DDisplayInfo info{};
    info.cur_w = mi.rcMonitor.right - mi.rcMonitor.left;
    info.cur_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    info.origin_x = mi.rcMonitor.left;
    info.origin_y = mi.rcMonitor.top;

    static std::string cached_device{};
    static int cached_native_w = 0;
    static int cached_native_h = 0;

    if (cached_device != mi.szDevice) {
        cached_device = mi.szDevice;
        cached_native_w = 0;
        cached_native_h = 0;

        DEVMODEA dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsA(mi.szDevice, i, &dm); ++i) {
            if ((int)(dm.dmPelsWidth * dm.dmPelsHeight) > cached_native_w * cached_native_h) {
                cached_native_w = (int)dm.dmPelsWidth;
                cached_native_h = (int)dm.dmPelsHeight;
            }
        }
    }

    info.native_w = cached_native_w;
    info.native_h = cached_native_h;

    {
        DEVMODEA dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            info.refresh_hz = (int)dm.dmDisplayFrequency;
        }
    }

    if (info.native_w == 0) {
        return std::nullopt;
    }

    return info;
}

// Drives UGameUserSettings the way the in-game options menu does — games
// with custom resolution pipelines (e.g. Gotham Knights) ignore r.SetRes,
// but their UGameUserSettings subclass override of the Apply path is exactly
// what their settings menu calls, and ProcessEvent dispatches to it.
// full_apply runs ApplySettings (scalability + save) instead of the narrower
// ApplyResolutionSettings, for subclasses that only hook the former.
// Game thread only.
bool apply_gameusersettings_resolution(uint32_t w, uint32_t h, bool full_apply, uint8_t window_mode = 1) {
    const auto c = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.GameUserSettings");
    if (c == nullptr) {
        return false;
    }

    const auto dfo = c->get_class_default_object();
    if (dfo == nullptr) {
        return false;
    }

    const auto get_fn = c->find_function(L"GetGameUserSettings");
    if (get_fn == nullptr) {
        return false;
    }

    struct {
        sdk::UObject* return_value{nullptr};
    } get_params{};

    dfo->process_event(get_fn, &get_params);

    auto* settings = get_params.return_value;
    if (settings == nullptr) {
        return false;
    }

    // The live object is the game's own subclass — resolve the functions
    // from its class so ProcessEvent picks up any overrides.
    const auto settings_class = settings->get_class();
    if (settings_class == nullptr) {
        return false;
    }

    const auto set_res_fn = settings_class->find_function(L"SetScreenResolution");
    const auto get_res_fn = settings_class->find_function(L"GetScreenResolution");
    const auto set_mode_fn = settings_class->find_function(L"SetFullscreenMode");
    const auto apply_fn = settings_class->find_function(full_apply ? L"ApplySettings" : L"ApplyResolutionSettings");

    if (set_res_fn == nullptr || apply_fn == nullptr) {
        return false;
    }

    // Capture the user's current resolution first. We only want the SWAPCHAIN/
    // window at native — the game must keep rendering at its own (sub-native)
    // resolution so the compositor upscales. Applying native here is just the
    // lever that makes the engine call ResizeBuffers (which our hook rewrites to
    // native while suppress_render_res_capture keeps the 3D render resolution
    // sub-native); we restore the stored setting immediately after so native is
    // never left as the game's actual resolution.
    struct { int32_t x; int32_t y; } orig{0, 0};
    if (get_res_fn != nullptr) {
        settings->process_event(get_res_fn, &orig);
    }

    struct {
        int32_t x;
        int32_t y;
    } res_params{(int32_t)w, (int32_t)h};
    settings->process_event(set_res_fn, &res_params);

    if (set_mode_fn != nullptr) {
        struct {
            uint8_t mode;
        } mode_params{window_mode}; // EWindowMode: 0=Fullscreen, 1=WindowedFullscreen (borderless), 2=Windowed
        settings->process_event(set_mode_fn, &mode_params);
    }

    struct {
        bool check_cli;
    } apply_params{false};
    settings->process_event(apply_fn, &apply_params);

    // Put the stored resolution back to the user's value WITHOUT re-applying:
    // the swapchain is already native and the render resolution is preserved, so
    // this only fixes the game's own setting/UI and any later SaveSettings() —
    // leaving native there would make the game boot at native next launch and
    // render there (no upscale, full GPU cost).
    if (get_res_fn != nullptr && orig.x > 0 && orig.y > 0) {
        settings->process_event(set_res_fn, &orig);
    }

    return true;
}

// --- World-marker HUD anchors (HUD depth mode 2) ----------------------------
// Post-hooks on the engine's world->screen projection utilities: every HUD
// widget that tracks a world object calls one of these per frame, giving us
// (screen position, world location) pairs = true depth anchors.
struct MarkerHookTarget {
    sdk::UFunction* func{nullptr};
    int32_t off_world{-1};
    int32_t off_screen{-1};
    sdk::FBoolProperty* ret_prop{nullptr};
};

std::vector<MarkerHookTarget> g_marker_targets{};

bool marker_hook_post(UEVR_UFunctionHandle func_h, UEVR_UObjectHandle, void* params, void*) {
    VR::get()->flat3d_marker_hook_post((void*)func_h, params);
    return true;
}
} // namespace

void VR::flat3d_marker_hook_post(void* ufunction, void* params) {
    if (params == nullptr) {
        return;
    }

    if (m_flat3d_hud_depth_mode->value() != 2 || !is_using_flat3d()) {
        return;
    }

    auto* flat3d = get_flat3d_runtime();
    if (flat3d == nullptr) {
        return;
    }

    // Blueprint-internal calls pass an FFrame& (first member is a vtable
    // pointer into a module) — the arguments aren't materialized as a flat
    // struct there and can't be read generically. ProcessEvent-dispatched
    // calls pass the flat parameter struct; only those are parseable.
    if (utility::get_module_within(*(void**)params).value_or(nullptr) != nullptr) {
        return;
    }

    const MarkerHookTarget* target = nullptr;
    for (const auto& t : g_marker_targets) {
        if ((sdk::UFunction*)ufunction == t.func) {
            target = &t;
            break;
        }
    }

    if (target == nullptr) {
        return;
    }

    // Projection failed (behind camera) — no anchor.
    if (target->ret_prop != nullptr &&
        !target->ret_prop->get_value_from_propbase((void*)((uintptr_t)params + target->ret_prop->get_offset()))) {
        return;
    }

    const bool dp = m_fake_stereo_hook != nullptr && m_fake_stereo_hook->has_double_precision();

    double wx, wy, wz, sx, sy;
    if (dp) { // UE5: FVector/FVector2D are double
        const auto* w = (const double*)((uintptr_t)params + target->off_world);
        const auto* s = (const double*)((uintptr_t)params + target->off_screen);
        wx = w[0]; wy = w[1]; wz = w[2];
        sx = s[0]; sy = s[1];
    } else {
        const auto* w = (const float*)((uintptr_t)params + target->off_world);
        const auto* s = (const float*)((uintptr_t)params + target->off_screen);
        wx = w[0]; wy = w[1]; wz = w[2];
        sx = s[0]; sy = s[1];
    }

    // View depth from the per-frame camera cache.
    const double vx = wx - flat3d->cam_x.load();
    const double vy = wy - flat3d->cam_y.load();
    const double vz = wz - flat3d->cam_z.load();
    const double z = vx * flat3d->cam_fwd_x.load() + vy * flat3d->cam_fwd_y.load() + vz * flat3d->cam_fwd_z.load();

    if (!std::isfinite(z) || z < 10.0) { // ignore closer than ~10 cm
        return;
    }

    // Screen position is in engine-viewport pixels — normalize by the size
    // the engine believes it has (matches the UI texture space).
    uint32_t bw = 0, bh = 0;
    if (g_framework->is_dx11()) {
        if (auto& h = g_framework->get_d3d11_hook(); h != nullptr) {
            bw = h->get_engine_believed_width();
            bh = h->get_engine_believed_height();
        }
    } else {
        if (auto& h = g_framework->get_d3d12_hook(); h != nullptr) {
            bw = h->get_engine_believed_width();
            bh = h->get_engine_believed_height();
        }
    }

    if (bw == 0 || bh == 0) {
        const auto rt = g_framework->get_rt_size();
        bw = (uint32_t)rt.x;
        bh = (uint32_t)rt.y;
    }

    if (bw == 0 || bh == 0) {
        return;
    }

    const float u = (float)(sx / (double)bw);
    const float v = (float)(sy / (double)bh);

    if (u < -0.1f || u > 1.1f || v < -0.1f || v > 1.1f) {
        return;
    }

    std::scoped_lock _{flat3d->anchors_mtx};
    if (flat3d->anchors_collecting.size() < 64) {
        flat3d->anchors_collecting.push_back({std::clamp(u, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f), (float)(1.0 / z)});
    }
}

namespace {
// Auto-convergence state (flat3d is a per-process singleton mode).
vrmod::flat3d::Flat3DAutoConvergence g_autoconv{};

// Frame-pack custom display timing (NVIDIA/AMD/CRU). Applied on switch into a
// FramePacked mode, reverted on switch away / process exit.
vrmod::flat3d::Flat3DDisplayTiming g_display_timing{};
int g_applied_fp_mode = -1;

// OpenTrack UDP receiver. Packet is 6 little-endian doubles: X, Y, Z (cm),
// Yaw, Pitch, Roll (degrees) — the standard OpenTrack "UDP over network"
// output. One receiver thread per flat3d session.
struct OpenTrackReceiver {
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<int> port{4242};
    std::atomic<bool> want_center{false};

    // Smoothed, centered pose (radians / meters, game-camera mapped).
    std::atomic<float> yaw{0.0f}, pitch{0.0f}, roll{0.0f};
    std::atomic<float> x{0.0f}, y{0.0f}, z{0.0f};

    void start(int p) {
        if (running.load()) {
            port.store(p);
            return;
        }
        port.store(p);
        stop.store(false);
        running.store(true);
        thread = std::thread([this] { run(); });
    }

    void stop_and_join() {
        stop.store(true);
        if (thread.joinable()) {
            thread.join();
        }
        running.store(false);
    }

    void run() {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            spdlog::error("[Flat3D][OpenTrack] WSAStartup failed");
            running.store(false);
            return;
        }

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            spdlog::error("[Flat3D][OpenTrack] socket() failed: {}", WSAGetLastError());
            WSACleanup();
            running.store(false);
            return;
        }

        u_long nonblock = 1;
        ioctlsocket(sock, FIONBIO, &nonblock);

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons((u_short)port.load());
        local.sin_addr.s_addr = INADDR_ANY;

        if (bind(sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
            spdlog::error("[Flat3D][OpenTrack] bind({}) failed: {}", port.load(), WSAGetLastError());
            closesocket(sock);
            WSACleanup();
            running.store(false);
            return;
        }

        spdlog::info("[Flat3D][OpenTrack] listening on UDP {}", port.load());

        // Neutral pose captured on center request.
        double c_yaw = 0, c_pitch = 0, c_roll = 0, c_x = 0, c_y = 0, c_z = 0;
        bool have_center = false;

        // Smoothing state (radians / meters).
        float s_yaw = 0, s_pitch = 0, s_roll = 0, s_x = 0, s_y = 0, s_z = 0;
        constexpr float kAlpha = 0.35f;

        double pkt[6]{};

        while (!stop.load()) {
            const int n = recvfrom(sock, (char*)pkt, sizeof(pkt), 0, nullptr, nullptr);

            if (n < (int)sizeof(pkt)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            if (want_center.exchange(false) || !have_center) {
                c_x = pkt[0]; c_y = pkt[1]; c_z = pkt[2];
                c_yaw = pkt[3]; c_pitch = pkt[4]; c_roll = pkt[5];
                have_center = true;
            }

            // Centered deltas -> game-camera mapping (radians / meters).
            const float t_yaw   = glm::radians((float)(pkt[3] - c_yaw));
            const float t_pitch = glm::radians((float)(pkt[4] - c_pitch));
            const float t_roll  = glm::radians((float)(pkt[5] - c_roll));
            const float t_x     = (float)((pkt[0] - c_x) / 100.0); // cm -> m, camera right
            const float t_y     = (float)((pkt[1] - c_y) / 100.0); // up
            const float t_z     = (float)((pkt[2] - c_z) / 100.0); // forward

            s_yaw   += (t_yaw   - s_yaw)   * kAlpha;
            s_pitch += (t_pitch - s_pitch) * kAlpha;
            s_roll  += (t_roll  - s_roll)  * kAlpha;
            s_x     += (t_x     - s_x)     * kAlpha;
            s_y     += (t_y     - s_y)     * kAlpha;
            s_z     += (t_z     - s_z)     * kAlpha;

            yaw.store(s_yaw);   pitch.store(s_pitch); roll.store(s_roll);
            x.store(s_x);       y.store(s_y);         z.store(s_z);
        }

        closesocket(sock);
        WSACleanup();
        spdlog::info("[Flat3D][OpenTrack] receiver stopped");
    }
};

OpenTrackReceiver g_opentrack{};
} // namespace

std::optional<std::string> VR::initialize_flat3d() {
    spdlog::info("[Flat3D] Initializing flat 3D monitor mode");

    // No VR API involved: mark both real runtimes as intentionally unused so
    // the Runtime page reports something sensible.
    m_openvr->error = "Flat3D mode active";
    m_openvr->dll_missing = true;
    m_openxr->error = "Flat3D mode active";
    m_openxr->dll_missing = true;

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    // Install the API-level scene-depth hooks NOW, before the game builds its
    // scene depth-stencil views. GameDepthCapture recovers the depth resource
    // from a DSV descriptor handle via CreateDepthStencilView, so any DSV made
    // before the hook is invisible. Installing lazily (only when a depth feature
    // is toggled on) misses a persistent depth buffer created once at load
    // (e.g. SMT5V) outright, so depth is never published and the whole mode-1
    // HUD path stays gated off. Idempotent; the per-frame compositor call still
    // covers late device availability.
    if (g_framework->is_dx12()) {
        if (auto& h = g_framework->get_d3d12_hook(); h != nullptr) {
            if (auto* dev = h->get_device()) {
                GameDepthCapture::get().ensure_installed_d3d12(dev);
            }
        }
    } else {
        if (auto& h = g_framework->get_d3d11_hook(); h != nullptr) {
            if (auto* dev = h->get_device()) {
                GameDepthCapture::get().ensure_installed_d3d11(dev);
            }
        }
    }

    m_flat3d->loaded = true;
    m_flat3d->error = std::nullopt;
    m_flat3d->update_render_target_size();

    // Seed the live params from config before the first frame.
    m_flat3d->separation_m.store(m_flat3d_depth->value());
    m_flat3d->convergence_m.store(m_flat3d_convergence->value());

    m_runtime = m_flat3d;

    spdlog::info("[Flat3D] Ready ({}x{} per eye)", m_flat3d->get_width(), m_flat3d->get_height());

    return std::nullopt;
}

// Reads the game camera's live FoV via APlayerCameraManager::GetFOVAngle
// (reflects the blended CameraCache POV: ADS zoom, cine cameras, etc.).
// ProcessEvent — must run on the game thread (called from the projection
// hook, same context process_snapturn uses).
float VR::sample_flat3d_game_fov(float fallback_deg) {
    const auto engine = sdk::UEngine::get();
    if (engine == nullptr) {
        return fallback_deg;
    }

    const auto world = engine->get_world();
    if (world == nullptr) {
        return fallback_deg;
    }

    const auto statics = sdk::UGameplayStatics::get();
    if (statics == nullptr) {
        return fallback_deg;
    }

    const auto controller = statics->get_player_controller(world, 0);
    if (controller == nullptr) {
        return fallback_deg;
    }

    const auto camera_manager = controller->get_player_camera_manager();
    if (camera_manager == nullptr) {
        return fallback_deg;
    }

    static sdk::UFunction* func = nullptr;
    if (func == nullptr) {
        if (const auto c = sdk::APlayerCameraManager::static_class(); c != nullptr) {
            func = c->find_function(L"GetFOVAngle");
        }
        if (func == nullptr) {
            return fallback_deg;
        }
    }

    struct {
        float return_value{};
    } params{};

    camera_manager->process_event(func, &params);

    if (!std::isfinite(params.return_value) || params.return_value < 5.0f || params.return_value > 175.0f) {
        return fallback_deg;
    }

    return params.return_value;
}

// Reads APlayerController::bShowMouseCursor — the game's intent to show a
// mouse cursor (menus, inventory). Game thread only (called from the
// projection hook's once-per-frame block, same as the FoV sample).
bool VR::sample_flat3d_show_cursor(bool fallback) {
    const auto engine = sdk::UEngine::get();
    if (engine == nullptr) {
        return fallback;
    }

    const auto world = engine->get_world();
    if (world == nullptr) {
        return fallback;
    }

    const auto statics = sdk::UGameplayStatics::get();
    if (statics == nullptr) {
        return fallback;
    }

    const auto controller = statics->get_player_controller(world, 0);
    if (controller == nullptr) {
        return fallback;
    }

    // Cached per controller class (games subclass APlayerController).
    static sdk::UClass* cached_class = nullptr;
    static sdk::FBoolProperty* prop = nullptr;

    const auto c = controller->get_class();
    if (c == nullptr) {
        return fallback;
    }

    if (c != cached_class) {
        cached_class = c;
        prop = (sdk::FBoolProperty*)c->find_property(L"bShowMouseCursor");
    }

    if (prop == nullptr) {
        return fallback;
    }

    return prop->get_value_from_object(controller);
}

// Reads UGameplayStatics::IsGamePaused — a full-screen-menu signal that does not
// need a visible mouse cursor (controller-driven pause menus don't arm one).
// Game thread only (called from the projection hook's once-per-frame block,
// same as the FoV / cursor samples).
bool VR::sample_flat3d_game_paused(bool fallback) {
    const auto engine = sdk::UEngine::get();
    if (engine == nullptr) {
        return fallback;
    }

    const auto world = engine->get_world();
    if (world == nullptr) {
        return fallback;
    }

    const auto statics = sdk::UGameplayStatics::get();
    if (statics == nullptr) {
        return fallback;
    }

    static sdk::UFunction* func = nullptr;
    if (func == nullptr) {
        if (const auto c = sdk::UGameplayStatics::static_class(); c != nullptr) {
            func = c->find_function(L"IsGamePaused");
        }
        if (func == nullptr) {
            return fallback;
        }
    }

    struct {
        sdk::UObject* world_context{nullptr};
        bool return_value{false};
    } params{(sdk::UObject*)world, false};

    statics->process_event(func, &params);

    return params.return_value;
}

// Installs the world->screen projection hooks feeding HUD depth mode 2.
// Game thread only; idempotent (retries until game data is initialized).
void VR::setup_flat3d_marker_hooks() {
    static bool attempted = false;

    if (attempted || !g_framework->is_game_data_intialized()) {
        return;
    }

    attempted = true;

    struct Spec {
        const wchar_t* class_path;
        const wchar_t* func_name;
        const wchar_t* world_prop;
        const wchar_t* screen_prop;
    };

    const Spec specs[] = {
        {L"Class /Script/Engine.PlayerController", L"ProjectWorldLocationToScreen", L"WorldLocation", L"ScreenLocation"},
        {L"Class /Script/Engine.GameplayStatics", L"ProjectWorldToScreen", L"WorldPosition", L"ScreenPosition"},
    };

    for (const auto& spec : specs) {
        const auto c = sdk::find_uobject<sdk::UClass>(spec.class_path);
        if (c == nullptr) {
            continue;
        }

        const auto f = c->find_function(spec.func_name);
        if (f == nullptr) {
            spdlog::warn("[Flat3D][markers] {} not found", utility::narrow(spec.func_name));
            continue;
        }

        const auto world_prop = f->find_property(spec.world_prop);
        const auto screen_prop = f->find_property(spec.screen_prop);

        if (world_prop == nullptr || screen_prop == nullptr) {
            spdlog::warn("[Flat3D][markers] {} params not found", utility::narrow(spec.func_name));
            continue;
        }

        MarkerHookTarget target{};
        target.func = f;
        target.off_world = world_prop->get_offset();
        target.off_screen = screen_prop->get_offset();
        target.ret_prop = (sdk::FBoolProperty*)f->find_property(L"ReturnValue");

        g_marker_targets.push_back(target);

        if (PluginLoader::get()->hook_ufunction_ptr((UEVR_UFunctionHandle)f, nullptr, &marker_hook_post)) {
            spdlog::info("[Flat3D][markers] hooked {}", utility::narrow(spec.func_name));
        } else {
            spdlog::warn("[Flat3D][markers] failed to hook {}", utility::narrow(spec.func_name));
            g_marker_targets.pop_back();
        }
    }
}

// Per-frame (game thread): cache the camera position/forward for anchor
// depth computation and publish this frame's collected anchors.
void VR::sample_flat3d_camera_and_publish_anchors() {
    auto flat3d = get_flat3d_runtime();
    if (flat3d == nullptr) {
        return;
    }

    {
        std::scoped_lock _{flat3d->anchors_mtx};
        flat3d->anchors_published = std::move(flat3d->anchors_collecting);
        flat3d->anchors_collecting.clear();
    }

    const auto hud_mode = m_flat3d_hud_depth_mode->value();

    if (hud_mode == 0) {
        return; // camera sampling only feeds the HUD depth modes
    }

    if (hud_mode == 2) {
        setup_flat3d_marker_hooks();
    }

    const auto engine = sdk::UEngine::get();
    if (engine == nullptr) {
        return;
    }

    const auto world = engine->get_world();
    const auto statics = sdk::UGameplayStatics::get();
    if (world == nullptr || statics == nullptr) {
        return;
    }

    const auto controller = statics->get_player_controller(world, 0);
    if (controller == nullptr) {
        return;
    }

    const auto camera_manager = controller->get_player_camera_manager();
    if (camera_manager == nullptr) {
        return;
    }

    static sdk::UFunction* loc_fn = nullptr;
    static sdk::UFunction* rot_fn = nullptr;

    if (loc_fn == nullptr || rot_fn == nullptr) {
        if (const auto c = sdk::APlayerCameraManager::static_class(); c != nullptr) {
            loc_fn = c->find_function(L"GetCameraLocation");
            rot_fn = c->find_function(L"GetCameraRotation");
        }
        if (loc_fn == nullptr || rot_fn == nullptr) {
            return;
        }
    }

    const bool dp = m_fake_stereo_hook != nullptr && m_fake_stereo_hook->has_double_precision();

    double cx, cy, cz, pitch, yaw;

    if (dp) {
        struct { double x, y, z; } loc{};
        struct { double p, y, r; } rot{};
        camera_manager->process_event(loc_fn, &loc);
        camera_manager->process_event(rot_fn, &rot);
        cx = loc.x; cy = loc.y; cz = loc.z;
        pitch = rot.p; yaw = rot.y;
    } else {
        struct { float x, y, z; } loc{};
        struct { float p, y, r; } rot{};
        camera_manager->process_event(loc_fn, &loc);
        camera_manager->process_event(rot_fn, &rot);
        cx = loc.x; cy = loc.y; cz = loc.z;
        pitch = rot.p; yaw = rot.y;
    }

    if (!std::isfinite(cx) || !std::isfinite(pitch) || !std::isfinite(yaw)) {
        return;
    }

    const double pr = pitch * 3.14159265358979323846 / 180.0;
    const double yr = yaw * 3.14159265358979323846 / 180.0;

    const double fx = std::cos(pr) * std::cos(yr);
    const double fy = std::cos(pr) * std::sin(yr);
    const double fz = std::sin(pr);

    flat3d->cam_x.store((float)cx);
    flat3d->cam_y.store((float)cy);
    flat3d->cam_z.store((float)cz);
    flat3d->cam_fwd_x.store((float)fx);
    flat3d->cam_fwd_y.store((float)fy);
    flat3d->cam_fwd_z.store((float)fz);

    // Rotation deltas for the world/static UI classification (wrap-safe).
    static double prev_yaw = 0.0, prev_pitch = 0.0;
    static double prev_cx = 0.0, prev_cy = 0.0, prev_cz = 0.0;
    static bool have_prev = false;

    const auto wrap_deg = [](double d) {
        while (d > 180.0) d -= 360.0;
        while (d < -180.0) d += 360.0;
        return d;
    };

    if (have_prev) {
        flat3d->cam_dyaw.store((float)(wrap_deg(yaw - prev_yaw) * 3.14159265358979323846 / 180.0));
        flat3d->cam_dpitch.store((float)(wrap_deg(pitch - prev_pitch) * 3.14159265358979323846 / 180.0));

        // Lateral translation (the part perpendicular to the view forward) is
        // what parallax-slides world markers across the screen while walking;
        // forward motion barely moves centered markers, so project it out.
        const double dxp = cx - prev_cx, dyp = cy - prev_cy, dzp = cz - prev_cz;
        const double along = dxp * fx + dyp * fy + dzp * fz;
        const double lx = dxp - along * fx, ly = dyp - along * fy, lz = dzp - along * fz;
        flat3d->cam_dtrans_lat.store((float)std::sqrt(lx * lx + ly * ly + lz * lz));
    }

    prev_yaw = yaw;
    prev_pitch = pitch;
    prev_cx = cx;
    prev_cy = cy;
    prev_cz = cz;
    have_prev = true;
}

// Katanga output runs the game in a NORMAL desktop window instead of the
// borderless-fullscreen hold every other Flat3D mode forces (see
// update_flat3d_params). Releases the native-output resize hold, then — throttled
// — nudges the engine into EWindowMode::Windowed, falling back to a Win32 frame
// restore for games that ignore the engine path. The style changes run on the
// game thread (a cross-thread SetWindowLongPtr can deadlock against the render
// thread we're called from).
void VR::update_flat3d_ensure_windowed() {
    // Let the game own its swapchain/window size again — the Katanga shared
    // texture is sized from the render resolution, not the window.
    if (auto& h12 = g_framework->get_d3d12_hook(); h12 != nullptr) {
        h12->set_forced_resize(0, 0);
    }
    if (auto& h11 = g_framework->get_d3d11_hook(); h11 != nullptr) {
        h11->set_forced_resize(0, 0);
    }

    const auto wnd = g_framework->get_window();
    if (wnd == nullptr) {
        return;
    }

    // A normal framed window has a caption + sizing border + system menu and no
    // WS_POPUP. If it already looks like that we're done.
    constexpr LONG_PTR kFrameStyles = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU;
    const auto style = GetWindowLongPtrW(wnd, GWL_STYLE);
    const bool is_popup = (style & WS_POPUP) != 0;
    const bool framed = (style & kFrameStyles) == kFrameStyles;

    static int attempts = 0;
    if (!is_popup && framed) {
        attempts = 0; // settled
        return;
    }

    static std::chrono::steady_clock::time_point last_kick{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_kick < std::chrono::seconds(3)) {
        return;
    }
    last_kick = now;
    ++attempts;

    const auto rt = g_framework->get_rt_size();
    const auto w = (uint32_t)rt.x;
    const auto h = (uint32_t)rt.y;
    // Try the clean engine path first; only force the Win32 frame after it has
    // clearly been ignored a few times.
    const bool win32_fallback = attempts > 3;

    spdlog::info("[Flat3D] Katanga: forcing windowed (popup={} framed={} attempt={})",
                 is_popup, framed, attempts);

    GameThreadWorker::get().enqueue([wnd, w, h, win32_fallback]() {
        // Engine lever: EWindowMode::Windowed (=2). Keeps the game's own
        // resolution (apply_gameusersettings_resolution restores the stored
        // value after applying).
        if (w != 0 && h != 0) {
            apply_gameusersettings_resolution(w, h, /*full_apply=*/false, /*window_mode=*/2);
        }

        if (!win32_fallback) {
            return;
        }

        // Win32 fallback: the engine ignored the windowed request — restore a
        // normal window frame ourselves (idempotent; only fires if still wrong).
        const auto st = GetWindowLongPtrW(wnd, GWL_STYLE);
        const auto want = (st & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_OVERLAPPEDWINDOW;
        if (want != st) {
            SetWindowLongPtrW(wnd, GWL_STYLE, want);
            SetWindowPos(wnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    });
}

// Called once per game-thread frame (from VR::on_frame) while flat3d is the
// active runtime. Derives the EFFECTIVE separation/convergence from the
// calibrated triple (depth, convergence, reference FoV) + live game FoV.
void VR::update_flat3d_params() {
    auto flat3d = get_flat3d_runtime();

    if (flat3d == nullptr || !flat3d->loaded) {
        return;
    }

    // --- FoV auto-scale (always on) -----------------------------------------
    // Screen disparity is proportional to sep * P00 = sep / tan_half_h, so a
    // zoom (smaller tan_half) would inflate the 3D effect. Scaling separation
    // by tan(gameFov/2)/tan(refFov/2) keeps perceived depth constant through
    // zoom/ADS. EMA smooths hard FoV cuts.
    const auto tan_half_game = flat3d->game_tan_half_h.load();
    const auto tan_half_ref = std::tan(glm::radians(m_flat3d_reference_fov->value()) * 0.5f);

    float fov_scale = (tan_half_ref > 0.0f && tan_half_game > 0.0f) ? (tan_half_game / tan_half_ref) : 1.0f;

    static float fov_scale_ema = 1.0f;
    constexpr float kFovEmaAlpha = 0.2f;
    fov_scale_ema += (fov_scale - fov_scale_ema) * kFovEmaAlpha;

    float separation = m_flat3d_depth->value() * fov_scale_ema;

    // --- Convergence: manual value is the ceiling; auto-convergence pulls
    // it in toward the nearest object's depth budget when enabled. ----------
    const auto w2m = m_world_to_meters != 0.0f ? m_world_to_meters : 100.0f;
    const auto nearz_m = flat3d->game_nearz.load() / w2m;
    // perfect_dark-style near clamp: convergence below ~1.5x near plane makes
    // the shear explode.
    const float conv_floor_m = std::max(nearz_m * 1.5f, 0.001f);

    const float manual_conv = std::max(m_flat3d_convergence->value(), conv_floor_m);

    vrmod::flat3d::Flat3DAutoConvergence::Settings acs{};
    acs.enabled = m_flat3d_autoconv_enabled->value();
    acs.target_disparity = m_flat3d_autoconv_target_disparity->value();
    acs.smoothing = m_flat3d_autoconv_smoothing->value();
    acs.min_convergence_m = m_flat3d_autoconv_min_conv->value();
    acs.logging = m_flat3d_autoconv_logging->value();

    const float p00 = tan_half_game > 0.0f ? 1.0f / tan_half_game : 1.0f;
    const auto ac = g_autoconv.update(flat3d->nearest_depth_uu.load(),
                                      separation * w2m * get_world_scale(), p00,
                                      manual_conv, conv_floor_m, w2m, acs);

    separation *= ac.depth_scale;
    const float convergence = std::max(ac.convergence_m, conv_floor_m);

    flat3d->separation_m.store(separation);
    flat3d->convergence_m.store(convergence);

    // Keep eyes[] in sync with the (possibly rescaled) separation.
    flat3d->update_matrices(m_nearz, m_farz);

    // Diagnostic: mirror the D3D12 debug-layer capture toggle to the hook (only
    // ever pushed from the flat3d path, so it stays inert in VR modes).
    if (auto& h12 = g_framework->get_d3d12_hook(); h12 != nullptr) {
        h12->set_debug_layer_wanted(m_flat3d_d3d12_debug_layer->value());
    }

    const auto mode = (vrmod::flat3d::Flat3DOutputMode)m_flat3d_output_mode->value();

    // Katanga forces the game into a NORMAL WINDOW instead of the
    // borderless-fullscreen hold every other mode uses: the real 3D output
    // goes to the headset via the shared texture, so the game window is only a
    // desktop control surface, and windowed keeps DWM composition happy for the
    // cross-process shared surface. It releases the native-output hold too.
    if (mode == vrmod::flat3d::Flat3DOutputMode::KATANGA) {
        update_flat3d_ensure_windowed();
    } else

    // --- Native output (all other modes: borderless-fullscreen at native) ----
    // The output window/swapchain is held at the display's native size (the
    // virtual-desktop span for dual-display modes); the game's REQUESTED
    // resolution is preserved by the hooks and becomes the 3D render
    // resolution (Flat3D::update_render_target_size). Pixel-exact modes and
    // full-SbS displays require an unscaled output chain.
    {
        // Two DISTINCT coordinate spaces are in play here and must never be
        // mixed:
        //   * swap_w/swap_h — the swapchain backbuffer target, in PHYSICAL
        //     device pixels. DXGI ResizeBuffers is DPI-agnostic and pixel-exact
        //     3D modes need the panel's true native pixels.
        //   * win_w/win_h — the game WINDOW target, in the window's OWN
        //     coordinate space. For a game that isn't per-monitor-DPI-aware
        //     that space is DPI-VIRTUALIZED: GetClientRect / SetWindowPos /
        //     GetMonitorInfo all report and consume virtualized coordinates.
        // On a scaled display the two differ by the DPI factor. The old code
        // fed the physical native size straight into SetWindowPos, so a
        // non-per-monitor-aware game (a handful of them) had its window
        // oversized by the scale factor — and the client<native compare then
        // re-kicked forever. EnumDisplaySettings (native_*) is always physical;
        // GetMonitorInfo (cur_*/origin_*) is in the window's coordinate space,
        // so pairing native with the swapchain and cur with the window keeps
        // each API in the space it expects.
        uint32_t swap_w = 0;
        uint32_t swap_h = 0;
        uint32_t win_w = 0;
        uint32_t win_h = 0;
        int target_x = 0;
        int target_y = 0;

        if (mode == vrmod::flat3d::Flat3DOutputMode::DUAL_DISPLAY ||
            mode == vrmod::flat3d::Flat3DOutputMode::DUAL_DISPLAY_FLIP) {
            // The virtual-desktop span is reported in the window's coordinate
            // space; use it for both (a spanning swapchain has no single
            // physical display mode to force to anyway).
            swap_w = win_w = (uint32_t)GetSystemMetrics(SM_CXVIRTUALSCREEN);
            swap_h = win_h = (uint32_t)GetSystemMetrics(SM_CYVIRTUALSCREEN);
            target_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
            target_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        } else if (const auto di = query_display_info(g_framework->get_window())) {
            swap_w = (uint32_t)di->native_w; // physical (EnumDisplaySettings)
            swap_h = (uint32_t)di->native_h;
            win_w = (uint32_t)di->cur_w;     // window-space (GetMonitorInfo)
            win_h = (uint32_t)di->cur_h;
            target_x = di->origin_x;
            target_y = di->origin_y;
        }

        if (auto& h12 = g_framework->get_d3d12_hook(); h12 != nullptr) {
            h12->set_forced_resize(swap_w, swap_h);
        }
        if (auto& h11 = g_framework->get_d3d11_hook(); h11 != nullptr) {
            h11->set_forced_resize(swap_w, swap_h);
        }

        // Engage the DPI lie before any resolution nudging: with every
        // effective-DPI query answering 96, r.SetRes and the engine's own
        // window sizing apply our native numbers literally instead of
        // multiplying by the monitor scale. One WM_DPICHANGED flush clears
        // the scale UE cached when it created its window (before the hooks
        // existed).
        if (swap_w != 0 && swap_h != 0 && win_w != 0 && win_h != 0) {
            const auto wnd = g_framework->get_window();

            if (wnd != nullptr && !g_flat3d_dpi_spoof_active.load(std::memory_order_acquire) &&
                flat3d_install_dpi_spoof())
            {
                const auto real_dpi = flat3d_real_dpi_for_window(wnd);
                g_flat3d_dpi_spoof_active.store(true, std::memory_order_release);

                spdlog::info("[Flat3D] Native output: DPI spoof engaged (real {} dpi, reporting 96)", real_dpi);

                if (real_dpi != 96) {
                    flat3d_flush_engine_dpi_cache(wnd, target_x, target_y, (LONG)win_w, (LONG)win_h);
                }
            }
        }

        // A swapchain that predates the forced-resize arming (i.e. the one
        // the game booted with) stays sub-native until the game itself calls
        // ResizeBuffers — and nothing does that on startup. Ask the engine to
        // re-apply its resolution AT the native size (r.SetRes): the current
        // believed resolution is seeded as the 3D render resolution first and
        // the hooks skip capturing our own request, so the in-game setting
        // keeps controlling render cost.
        if (swap_w != 0 && swap_h != 0) {
            const auto rt = g_framework->get_rt_size();
            const auto bb_w = (uint32_t)rt.x;
            const auto bb_h = (uint32_t)rt.y;

            static std::chrono::steady_clock::time_point last_nudge{};
            static int nudge_attempts = 0;

            if (bb_w != 0 && (bb_w != swap_w || bb_h != swap_h)) {
                const auto now = std::chrono::steady_clock::now();

                // r.SetRes resizes within a frame or two on the games we target,
                // so retry fast; nudge_attempts resets the moment native is
                // reached, so this never spams once it takes.
                if (now - last_nudge > std::chrono::seconds(4)) {
                    last_nudge = now;

                    if (nudge_attempts < 6) {
                        ++nudge_attempts;

                        const auto seed = [&](auto& hook) {
                            if (hook != nullptr) {
                                if (hook->get_game_requested_width() == 0) {
                                    hook->set_game_requested(bb_w, bb_h);
                                }
                                // Window, not one-shot: some games (Gotham
                                // Knights) re-apply OUR resolution multiple
                                // times, seconds apart.
                                hook->suppress_render_res_capture_for(15000);
                            }
                        };
                        seed(g_framework->get_d3d12_hook());
                        seed(g_framework->get_d3d11_hook());

                        // r.SetRes is the standard UE path and is intercepted by
                        // our ResizeBuffers hook, so it forces the swapchain
                        // native without persisting a resolution change — give it
                        // the bulk of the attempts. Only fall back to
                        // GameUserSettings.ApplyResolutionSettings (still no save,
                        // and it restores the stored resolution afterward) for
                        // games that ignore r.SetRes. We deliberately never call
                        // ApplySettings: its SaveSettings() would write native
                        // into the game's own config, so the game would boot at
                        // native next launch and render there (no upscale).
                        const int method = nudge_attempts <= 4 ? 0 : 1;

                        spdlog::info("[Flat3D] Native output: swapchain {}x{} doesn't match native {}x{} — "
                                     "asking the engine to resize ({}, attempt {})",
                                     bb_w, bb_h, swap_w, swap_h,
                                     method == 0 ? "r.SetRes" : "GameUserSettings.ApplyResolutionSettings",
                                     nudge_attempts);

                        GameThreadWorker::get().enqueue([swap_w, swap_h, method]() {
                            if (method != 0 && apply_gameusersettings_resolution(swap_w, swap_h, /*full_apply=*/false)) {
                                return;
                            }

                            if (auto* engine = sdk::UEngine::get(); engine != nullptr) {
                                const std::wstring cmd =
                                    L"r.SetRes " + std::to_wstring(swap_w) + L"x" + std::to_wstring(swap_h);
                                engine->exec(cmd.c_str());
                            }
                        });
                    } else if (nudge_attempts == 6) {
                        ++nudge_attempts;
                        spdlog::warn("[Flat3D] Native output: engine ignored the resize requests; output "
                                     "stays {}x{} until the game changes resolution", bb_w, bb_h);
                    }
                }
            } else if (nudge_attempts != 0) {
                // Native reached — re-arm for any later sub-native episode
                // (the capture-suppress window expires on its own).
                nudge_attempts = 0;
            }

            // DPI-echo compensation: UE's high-DPI viewport path multiplies a
            // requested resolution by the monitor scale (SMT5V echoes r.SetRes
            // 3840x2160 back as 5760x3240 on a 150% display). The hooks pin the
            // actual swapchain at native, but the engine keeps believing the
            // scaled size and keeps re-sizing its window to it — a periodic
            // tug-of-war with the borderless re-assert below, with swapchain
            // churn each round. Detect the echo (believed == native x window
            // scale while the actual output sits at native) and re-ask at
            // native / scale, so the engine's own multiply lands exactly on
            // native and window, swapchain, and believed size all agree.
            {
                const auto wnd = g_framework->get_window();
                uint32_t believed_w = 0;
                uint32_t believed_h = 0;

                if (auto& h12 = g_framework->get_d3d12_hook(); h12 != nullptr && h12->get_engine_believed_width() != 0) {
                    believed_w = h12->get_engine_believed_width();
                    believed_h = h12->get_engine_believed_height();
                } else if (auto& h11 = g_framework->get_d3d11_hook(); h11 != nullptr) {
                    believed_w = h11->get_engine_believed_width();
                    believed_h = h11->get_engine_believed_height();
                }

                // Real DPI through the hook original — the spoof must not
                // blind our own echo detection.
                const auto dpi = wnd != nullptr ? flat3d_real_dpi_for_window(wnd) : 96u;
                const float scale = (float)dpi / 96.0f;

                static int compensate_attempts = 0;
                static std::chrono::steady_clock::time_point last_compensate{};

                const auto near_eq = [](uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) <= 2; };

                if (scale > 1.01f && compensate_attempts < 2 &&
                    bb_w == swap_w && bb_h == swap_h && believed_w != 0 &&
                    near_eq(believed_w, (uint32_t)std::lround(swap_w * scale)) &&
                    near_eq(believed_h, (uint32_t)std::lround(swap_h * scale)))
                {
                    const auto now = std::chrono::steady_clock::now();

                    if (now - last_compensate > std::chrono::seconds(4)) {
                        last_compensate = now;
                        ++compensate_attempts;

                        if (g_flat3d_dpi_spoof_active.load(std::memory_order_acquire)) {
                            // The engine re-derived a scaled size while the
                            // spoof is live — a per-window scale it cached
                            // before the flush landed. Re-flush instead of
                            // asking for a scaled resolution.
                            spdlog::info("[Flat3D] Native output: engine believes {}x{} (DPI echo x{:.2f}) with spoof "
                                         "active — re-flushing WM_DPICHANGED",
                                         believed_w, believed_h, scale);
                            flat3d_flush_engine_dpi_cache(wnd, target_x, target_y, (LONG)win_w, (LONG)win_h);
                        } else {
                            const auto logical_w = (uint32_t)std::lround(swap_w / scale);
                            const auto logical_h = (uint32_t)std::lround(swap_h / scale);

                            const auto seed = [&](auto& hook) {
                                if (hook != nullptr) {
                                    hook->suppress_render_res_capture_for(15000);
                                }
                            };
                            seed(g_framework->get_d3d12_hook());
                            seed(g_framework->get_d3d11_hook());

                            spdlog::info("[Flat3D] Native output: engine believes {}x{} (DPI echo x{:.2f} of native {}x{}) — "
                                         "re-asking at {}x{} so the engine's own scale lands on native",
                                         believed_w, believed_h, scale, swap_w, swap_h, logical_w, logical_h);

                            GameThreadWorker::get().enqueue([logical_w, logical_h]() {
                                if (auto* engine = sdk::UEngine::get(); engine != nullptr) {
                                    const std::wstring cmd =
                                        L"r.SetRes " + std::to_wstring(logical_w) + L"x" + std::to_wstring(logical_h);
                                    engine->exec(cmd.c_str());
                                }
                            });
                        }
                    }
                } else if (believed_w == swap_w && believed_h == swap_h) {
                    // Engine agreed on native — re-arm for a later episode
                    // (e.g. the game re-applying its own scaled resolution).
                    compensate_attempts = 0;
                }
            }
        }

        // Keep the game WINDOW borderless at the native rect. This matters
        // beyond the swapchain (which the hook rewrites already hold at
        // native): the engine sizes its viewport — and draws its UI — from
        // the window, so a sub-native window means UI cropped into a corner
        // of the native UI target; and a framed (windowed-mode) window can
        // never cover the monitor, so its caption/borders are stripped too.
        // The engine's own WM_SIZE handling follows our resize. Re-checks at
        // most every 3s so a stubborn game gets pushed back.
        if (win_w != 0 && win_h != 0) {
            const auto wnd = g_framework->get_window();
            RECT client{};

            if (wnd != nullptr && GetClientRect(wnd, &client)) {
                const auto client_w = (uint32_t)(client.right - client.left);
                const auto client_h = (uint32_t)(client.bottom - client.top);

                constexpr LONG_PTR kFrameStyles = WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
                const bool framed = (GetWindowLongPtrW(wnd, GWL_STYLE) & kFrameStyles) != 0;

                // client_w/h and win_w/h are both in the window's coordinate
                // space, so this compare is DPI-consistent (no false re-kicks).
                // Both directions: UE's high-DPI path resizes its window to the
                // DPI-multiplied believed resolution (e.g. 5760x3240 on a 150%
                // display) — an OVERSIZED window stretches the native swapchain
                // (only the top-left crop visible) just as a small one crops UI.
                if (client_w != win_w || client_h != win_h || framed) {
                    static std::chrono::steady_clock::time_point last_kick{};
                    const auto now = std::chrono::steady_clock::now();

                    if (now - last_kick > std::chrono::seconds(3)) {
                        last_kick = now;

                        // Preserve the believed resolution as the 3D render
                        // resolution if a resize rewrite hasn't captured one,
                        // and keep the engine's reactive ResizeBuffers (to
                        // the new native window size) from overwriting it.
                        const auto seed = [&](auto& hook) {
                            if (hook != nullptr) {
                                if (hook->get_game_requested_width() == 0) {
                                    hook->set_game_requested(client_w, client_h);
                                }
                                hook->suppress_render_res_capture_for(8000);
                            }
                        };
                        seed(g_framework->get_d3d12_hook());
                        seed(g_framework->get_d3d11_hook());

                        spdlog::info("[Flat3D] Native output: game window {}x{} (framed={}) -> borderless {}x{} at ({},{})",
                                     client_w, client_h, framed, win_w, win_h, target_x, target_y);

                        // Style changes must run on the window's own (game)
                        // thread: a cross-thread SetWindowLongPtr SENDS
                        // WM_STYLECHANGING to the owner and can deadlock
                        // against the render thread we're called from.
                        GameThreadWorker::get().enqueue([wnd, target_x, target_y, win_w, win_h]() {
                            constexpr LONG_PTR kStripStyles =
                                WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
                            const auto style = GetWindowLongPtrW(wnd, GWL_STYLE);
                            const auto new_style = (style & ~kStripStyles) | WS_POPUP;

                            if (new_style != style) {
                                SetWindowLongPtrW(wnd, GWL_STYLE, new_style);
                            }

                            constexpr LONG_PTR kFrameExStyles =
                                WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;
                            const auto exstyle = GetWindowLongPtrW(wnd, GWL_EXSTYLE);
                            const auto new_exstyle = exstyle & ~kFrameExStyles;

                            if (new_exstyle != exstyle) {
                                SetWindowLongPtrW(wnd, GWL_EXSTYLE, new_exstyle);
                            }

                            SetWindowPos(wnd, nullptr, target_x, target_y, (int)win_w, (int)win_h,
                                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                        });
                    }
                }
            }
        }
    }

    // --- Force SDR output ----------------------------------------------------
    // HDR swapchains (PQ/scRGB) wash out the SDR-defined 3D output modes and
    // color correction (the compositor bypasses the game's own HDR
    // composite). Ask the engine to switch HDR output off while the
    // swapchain is an HDR format (10s throttle; the game recreates the
    // swapchain as 8-bit sRGB in response). Katanga's shared texture is 8-bit
    // BGRA and its shader doesn't tone-map, so force SDR whenever Katanga is
    // active regardless of the toggle.
    if (m_flat3d_force_sdr->value() || mode == vrmod::flat3d::Flat3DOutputMode::KATANGA) {
        DXGI_FORMAT bb_fmt = DXGI_FORMAT_UNKNOWN;
        bool bb_pq = false;

        if (g_framework->is_dx11()) {
            if (auto& h11 = g_framework->get_d3d11_hook(); h11 != nullptr && h11->get_swap_chain() != nullptr) {
                DXGI_SWAP_CHAIN_DESC desc{};
                if (SUCCEEDED(h11->get_swap_chain()->GetDesc(&desc))) {
                    bb_fmt = desc.BufferDesc.Format;
                }
            }
        } else {
            if (auto& h12 = g_framework->get_d3d12_hook(); h12 != nullptr && h12->get_swap_chain() != nullptr) {
                DXGI_SWAP_CHAIN_DESC desc{};
                if (SUCCEEDED(h12->get_swap_chain()->GetDesc(&desc))) {
                    bb_fmt = desc.BufferDesc.Format;
                }
                bb_pq = h12->is_swapchain_pq();
            }
        }

        // FP16 is unambiguously HDR (scRGB); 10-bit only counts as HDR when
        // the game explicitly set a PQ color space — SDR titles commonly run
        // R10G10B10A2 swapchains with plain G22 gamma.
        const bool is_hdr = bb_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                            (bb_fmt == DXGI_FORMAT_R10G10B10A2_UNORM && bb_pq);

        if (is_hdr) {
            static std::chrono::steady_clock::time_point last_sdr_nudge{};
            const auto now = std::chrono::steady_clock::now();

            if (now - last_sdr_nudge > std::chrono::seconds(10)) {
                last_sdr_nudge = now;
                spdlog::info("[Flat3D] Force SDR: swapchain is HDR (format {}) — disabling engine HDR output",
                             (uint32_t)bb_fmt);

                GameThreadWorker::get().enqueue([]() {
                    if (auto* engine = sdk::UEngine::get(); engine != nullptr) {
                        engine->exec(L"r.HDR.EnableHDROutput 0");
                    }
                });
            }
        }
    }

    // --- VSync override 2x-refresh framerate cap (AFR/Synced Sequential) ----
    // Force Off + 2x cap: each present is one eye under AFR, so running the
    // game at twice the display refresh gives each eye a full-refresh update
    // rate with sane pacing. Owns t.MaxFPS while active (update_hmd_state
    // skips the UncapFramerate write); restores uncapped on exit.
    {
        static bool cap_was_active = false;
        const bool cap_active = m_flat3d_vsync->value() >= 2; // No-Tear Fast

        if (cap_active) {
            int hz = 0;

            if (const auto di = query_display_info(g_framework->get_window())) {
                hz = di->refresh_hz;
            }

            if (hz <= 0) {
                hz = 60;
            }

            // AFR-family: one eye per present -> 2x refresh gives each eye a
            // full-refresh update. Native stereo: both eyes per present ->
            // 2x would render twice what the display can show; cap at 1x.
            const int mult = is_using_afr() ? 2 : 1;
            const int cap = mult * hz;

            static int s_last_logged_cap = -1;
            if (cap != s_last_logged_cap) {
                s_last_logged_cap = cap;
                spdlog::info("[Flat3D] No-Tear Fast cap: t.MaxFPS={} (display {} Hz x {})", cap, hz, mult);
            }

            sdk::set_cvar_data_float(L"Engine", L"t.MaxFPS", (float)cap);
        } else if (cap_was_active) {
            sdk::set_cvar_data_float(L"Engine", L"t.MaxFPS", 0.0f);
        }

        cap_was_active = cap_active;
    }

    // --- 3D render resolution follows the in-game setting --------------------
    // The hooks capture the game's requested size on every resize, but the
    // runtime only read it once at init — re-read every frame and reallocate
    // the stereo targets when it changes.
    {
        const auto prev_w = flat3d->get_width();
        const auto prev_h = flat3d->get_height();

        flat3d->update_render_target_size();

        if ((flat3d->get_width() != prev_w || flat3d->get_height() != prev_h) && m_fake_stereo_hook != nullptr) {
            spdlog::info("[Flat3D] Render resolution changed {}x{} -> {}x{} — recreating stereo targets",
                         prev_w, prev_h, flat3d->get_width(), flat3d->get_height());
            m_fake_stereo_hook->set_should_recreate_textures(true);
        }
    }

    // --- Frame-pack custom display timing (apply on switch in, revert out) --
    {
        const auto mode = (vrmod::flat3d::Flat3DOutputMode)m_flat3d_output_mode->value();
        const bool is_fp = vrmod::flat3d::IsFramePackedMode(mode);

        if (is_fp && (int)m_flat3d_output_mode->value() != g_applied_fp_mode) {
            if (const auto* spec = vrmod::flat3d::GetFramePackTimingSpec(mode)) {
                g_display_timing.Apply(*spec, 0 /* primary/auto */);
                g_applied_fp_mode = m_flat3d_output_mode->value();
            }
        } else if (!is_fp && g_applied_fp_mode >= 0) {
            g_display_timing.Revert();
            g_applied_fp_mode = -1;
        }
    }

    // --- OpenTrack head tracking lifecycle + pose push ----------------------
    if (m_flat3d_opentrack_enabled->value()) {
        g_opentrack.start(m_flat3d_opentrack_port->value());

        flat3d->head_yaw.store(g_opentrack.yaw.load());
        flat3d->head_pitch.store(g_opentrack.pitch.load());
        flat3d->head_roll.store(g_opentrack.roll.load());
        flat3d->head_x.store(g_opentrack.x.load());
        flat3d->head_y.store(g_opentrack.y.load());
        flat3d->head_z.store(g_opentrack.z.load());
        flat3d->opentrack_active.store(g_opentrack.running.load());
    } else if (g_opentrack.running.load()) {
        g_opentrack.stop_and_join();
        flat3d->opentrack_active.store(false);
        flat3d->head_yaw.store(0.0f); flat3d->head_pitch.store(0.0f); flat3d->head_roll.store(0.0f);
        flat3d->head_x.store(0.0f);   flat3d->head_y.store(0.0f);     flat3d->head_z.store(0.0f);
    }
}

// Builds the per-frame compositor parameter block. Called by the D3D
// components each frame with the current eye dimensions; the component
// fills in the colorspace fields afterward (it knows the swapchain).
vrmod::flat3d::Flat3DFrameParams VR::build_flat3d_frame_params(uint32_t eye_w, uint32_t eye_h) {
    vrmod::flat3d::Flat3DFrameParams p{};

    auto flat3d = get_flat3d_runtime();

    p.mode = m_flat3d_output_mode->value();
    p.vsync_override = m_flat3d_vsync->value();
    p.eye_swap = m_flat3d_eye_swap->value();
    // AFW: the engine renders one eye/frame (rides is_using_afr()); the other eye
    // is reprojected. warp_frame drives the compositor to take both eyes fresh (the
    // rendered half + the warped discrete texture) instead of reusing the stale eye.
    p.warp_frame = is_using_afw();
    p.afr_frame = is_using_afr() && !p.warp_frame;
    // afr_left_eye stays meaningful under warp_frame: which eye the engine rendered.
    p.afr_left_eye = (is_using_afr() || p.warp_frame) && (m_render_frame_count % 2 == m_left_eye_interval);
    // Synced Sequential renders matched same-game-state eye pairs; the pair
    // lock keeps the compositor from displaying the half-updated in-between.
    // Engage it whenever the ENGINE is forcing synced draws — that's
    // is_using_synchronized_afr(), which also covers AFW's warmup /
    // resolution-change windows and extreme-compat Native, not just the
    // explicit Synced Sequential method.
    p.afr_synced_pair = p.afr_frame && is_using_synchronized_afr();

    // Pair-second detection: camera identity ONLY. The view-offset hook pushes
    // one record per eye draw — true when that draw's raw game camera was
    // bit-identical to the previous draw's, which is exactly the forced
    // same-state second draw. Consumed FIFO, one per present, so alignment
    // cannot drift the way a parity guess can (and it self-corrects at motion
    // onset: a static camera matches every draw, which degrades to publish-
    // per-present — invisible without motion). When no record is available,
    // default to TRUE = publish naively (no pair hold): naive shows a matched
    // pair every other present, while a wrongly-guessed hold shows cross-state
    // pairs on EVERY present. Never lock without evidence — this is why the
    // old engine-frame-repeat fallback is gone (it fired on the wrong half in
    // some titles and pinned the lock misaligned).
    bool pair_second = true;
    if (flat3d != nullptr) {
        // Engine frame advanced this present? (Read BEFORE on_post_present
        // syncs the counters.) Two title classes exist:
        //   P3R-class: EVERY draw advances the frame number — a same-frame
        //   present is a draw-less re-present and must not eat a record.
        //   Hogwarts-class: the forced draw KEEPS the frame number — the
        //   same-frame present IS the pair-second and owns a (match=true)
        //   record.
        // Disambiguate by peeking: pop on a same-frame present only when the
        // front record is a camera match. First-half records are never
        // consumed by re-presents, and Hogwarts' pair-seconds consume theirs.
        const bool new_engine_frame = m_frame_count != m_render_frame_count;

        std::scoped_lock _{flat3d->pair_mtx};
        if (p.afr_synced_pair) {
            bool popped = false;

            static int s_prev_pop_eye = -1;
            static uint32_t s_same_eye_pairs = 0;
            static uint32_t s_slot_mismatch = 0;
            static uint32_t s_rec_dropped = 0;

            // Discard records for engine frames that will never present
            // (dropped frames, menu transitions) — matching by frame number
            // makes the alignment exact instead of order-inferred.
            // KEY BY m_render_frame_count, NOT m_frame_count: the fresh latch
            // (m_frame_count) is captured when the render thread BEGINS a
            // frame — one ahead of what this present displays. With eyes
            // alternating per frame, keying on it inverted every slot. The
            // one-present-lagged m_render_frame_count is the engine frame the
            // present actually shows — the same clock plain AFR's slot parity
            // has always used correctly.
            const uint32_t presenting_frame = (uint32_t)m_render_frame_count;
            while (!flat3d->pair_second_fifo.empty() &&
                   (int32_t)(flat3d->pair_second_fifo.front().frame - presenting_frame) < 0) {
                flat3d->pair_second_fifo.pop_front();
                ++s_rec_dropped;
            }

            if (!flat3d->pair_second_fifo.empty() &&
                flat3d->pair_second_fifo.front().frame == presenting_frame) {
                const uint8_t record = flat3d->pair_second_fifo.front().flags;
                pair_second = (record & 1u) != 0;
                const int rec_eye = (record >> 1) & 1;

                // A published pair whose two draws rendered the SAME eye is the
                // depth-squish failure mode — count it directly.
                if (pair_second && s_prev_pop_eye >= 0 && rec_eye == s_prev_pop_eye) {
                    ++s_same_eye_pairs;
                }
                s_prev_pop_eye = rec_eye;

                // Present-side alignment: the compositor copies this present's
                // image into the slot picked by frame parity (afr_left_eye).
                // If that slot differs from the eye the draw actually rendered,
                // every image lands in the OPPOSITE eye — inverted stereo,
                // which reads as squished/broken depth.
                const int fresh_slot = p.afr_left_eye ? 0 : 1;
                if (rec_eye != fresh_slot) {
                    ++s_slot_mismatch;
                }

                // Slot by the MEASURED eye, not the parity model. The frame-
                // counter regime differs per title and can even change within
                // one (Hogwarts: stalled on forced draws in one session,
                // advanced in another once the NSF hook was live — flipping
                // the parity phase and inverting every eye slot = depth
                // squish). The record carries the eye the draw actually
                // rendered; FIFO order keeps it aligned with this present.
                // slot_mismatch above now just tracks the parity model's
                // drift as a diagnostic.
                p.afr_left_eye = (rec_eye == 0);

                flat3d->pair_second_fifo.pop_front();
                flat3d->pair_pop_count++;
                popped = true;

                // NOTE: no eye flip here. m_render_frame_count lags one
                // present, so its parity across a Hogwarts pair runs
                // A,B,B,A,A,B... — it already flips exactly once per pair and
                // mirrors the per-draw eye alternation (L,R,R,L,L,R...) on the
                // game thread, whose g_frame_count is one frame stale for the
                // same reason. An extra flip on the pair-second present slots
                // both halves into the SAME eye again (depth squish).
            } else {
                if (new_engine_frame) {
                    flat3d->pair_empty_count++;
                }

                // No record for this present (menus / non-stereo scenes push
                // nothing): keep ALTERNATING from the last measured eye
                // rather than falling back to frame parity — the parity phase
                // is arbitrary and differs between counter regimes, which
                // surfaced as the eyes swapping between the main menu and
                // gameplay (user had to toggle Eye Swap per context).
                if (s_prev_pop_eye >= 0) {
                    const int guessed = s_prev_pop_eye ^ 1;
                    s_prev_pop_eye = guessed;
                    p.afr_left_eye = (guessed == 0);
                }
            }

            // Visible world-update pacing: the screen only changes on presents
            // that publish (pair_second true with a fresh engine frame). Their
            // rate is the effective world framerate the user perceives —
            // HALF the draw rate under synced sequential — and their max gap
            // exposes pacing hitches the averages hide.
            static uint32_t s_publish_count = 0;
            static double s_max_gap_ms = 0.0;
            static auto s_last_publish = std::chrono::steady_clock::time_point{};
            const auto now = std::chrono::steady_clock::now();

            if (pair_second && (popped || new_engine_frame)) {
                if (s_last_publish.time_since_epoch().count() != 0) {
                    const auto gap_ms = std::chrono::duration<double, std::milli>(now - s_last_publish).count();
                    s_max_gap_ms = std::max(s_max_gap_ms, gap_ms);
                }
                s_last_publish = now;
                s_publish_count++;
            }

            // Compact 5s diagnostic so a log tells us what the detector sees:
            // pushes (eye draws), match rate (should be ~50% while the camera
            // moves, ~100% static), pops vs empty (draw->present correspondence),
            // world-updates/s + worst visible gap (perceived smoothness).
            static auto s_last_diag = std::chrono::steady_clock::now();
            if (now - s_last_diag >= std::chrono::seconds(5)) {
                const auto window_s = std::chrono::duration<double>(now - s_last_diag).count();
                s_last_diag = now;
                spdlog::info("[Flat3D][sync-pair] pushes={} matches={} pops={} empty={} fifo={} world_ups={:.1f}/s max_gap={:.0f}ms same_eye_pairs={} stalls={} slot_mismatch={} drops={}",
                    flat3d->pair_push_count, flat3d->pair_match_count,
                    flat3d->pair_pop_count, flat3d->pair_empty_count,
                    flat3d->pair_second_fifo.size(),
                    window_s > 0.0 ? (double)s_publish_count / window_s : 0.0,
                    s_max_gap_ms,
                    s_same_eye_pairs,
                    flat3d->pair_stall_count.exchange(0, std::memory_order_relaxed),
                    s_slot_mismatch,
                    s_rec_dropped);
                s_same_eye_pairs = 0;
                s_slot_mismatch = 0;
                s_rec_dropped = 0;
                flat3d->pair_push_count = 0;
                flat3d->pair_match_count = 0;
                flat3d->pair_pop_count = 0;
                flat3d->pair_empty_count = 0;
                s_publish_count = 0;
                s_max_gap_ms = 0.0;
            }
        } else if (!flat3d->pair_second_fifo.empty()) {
            flat3d->pair_second_fifo.clear(); // mode switched: drop stale records
        }
    }
    p.afr_pair_second = pair_second;
    p.native_stereo_layout = is_native_stereo_fix_enabled();
    p.paper_white_nits = m_flat3d_hdr_paper_white->value();

    if (flat3d == nullptr || eye_w == 0) {
        return p;
    }

    const float w2m = m_world_to_meters != 0.0f ? m_world_to_meters : 100.0f;
    // Same effective separation the view path applies (w2m x world scale).
    const float sep_uu = flat3d->separation_m.load() * w2m * get_world_scale();
    const float conv_uu = std::max(flat3d->convergence_m.load() * w2m, 1e-3f);
    const float tan_half = flat3d->game_tan_half_h.load();
    const float p00 = tan_half > 0.0f ? 1.0f / tan_half : 1.0f;

    // Per-eye pixel displacement of world content at view depth z (matches
    // the shear+translation pair the world rendering uses, so a UI/crosshair
    // layer shifted by this lands at exactly depth z):
    //   px(z) = dir * (sep_uu * p00 / 2) * (1/z - 1/conv) * eye_w/2
    // with dir = +1 for the LEFT eye (negative for z > conv: the left image
    // moves left -> uncrossed disparity behind the screen).
    const auto shift_px_at = [&](float z_uu) {
        const float inv_z = z_uu > 0.0f ? 1.0f / z_uu : 0.0f; // invalid -> infinity
        return (sep_uu * p00 * 0.5f) * (inv_z - 1.0f / conv_uu) * ((float)eye_w * 0.5f);
    };

    // Symmetric-projection compat (the Compatibility page's Horizontal
    // Projection = Symmetrical — same setting the HMD paths honor): the
    // projection has NO shear, so the whole scene renders converged at
    // infinity; the compositor shifts each eye's image by the
    // shear-equivalent constant (px(inf)) to restore the screen plane at
    // `convergence`, with a mild zoom to cover the shifted edge. Overlays are
    // drawn into the eyes BEFORE that shift, so their own shift must be
    // relative (px(z) - px(inf)).
    const bool symmetric =
        get_horizontal_projection_override() == HORIZONTAL_PROJECTION_OVERRIDE::HORIZONTAL_SYMMETRIC;
    float overlay_shift_bias = 0.0f;

    if (symmetric) {
        p.scene_shift_px = shift_px_at(0.0f); // z -> infinity term only
        p.scene_scale = ((float)eye_w + 2.0f * std::fabs(p.scene_shift_px)) / (float)eye_w;
        overlay_shift_bias = p.scene_shift_px;
    }

    // GUI layer at the single depth slider's distance. Overlays are drawn
    // into the eyes BEFORE the symmetric-mode crop map, which scales offsets
    // by scene_scale and adds scene_shift — invert both for the final shift.
    const auto overlay_shift_at = [&](float z_uu) {
        return (shift_px_at(z_uu) - overlay_shift_bias) / p.scene_scale;
    };

    p.ui_enabled = m_enable_gui->value();
    p.ui_invert_alpha = get_overlay_component().get_ui_invert_alpha(); // same UI_InvertAlpha config/slider as the VR path

    // The UI's FINAL on-screen shift (after the crop map) is px(z_ui); the
    // drawn shift/scale are pre-divided by scene_scale so the crop map's
    // zoom lands them exactly. GUI/menu depth is a MULTIPLE of the
    // (effective) convergence: at 1.0 the overlay sits exactly on the screen
    // plane (zero shift at any convergence, manual or auto).
    //
    // Auto-scale FITS rather than covers: the overlay is scaled DOWN
    // HORIZONTALLY by (1 - 2|shift|) so the shifted image stays entirely on
    // screen in BOTH eyes — no content is ever pushed off screen (a
    // cover-zoom loses 2|shift| of the UI on one side per eye). Horizontal
    // only: the depth shift is horizontal, and a vertical shrink would just
    // letterbox fullscreen UIs (the compositors keep vertical identity).
    // The vacated edge strips show the scene in one eye only, which reads
    // as a thin border at the overlay's depth; the slight anamorphic
    // squeeze is imperceptible at sane depth offsets.
    const auto overlay_fit_scale = [&](float final_shift_px) {
        const float fit = ((float)eye_w - 2.0f * std::fabs(final_shift_px)) / (float)eye_w;
        return std::max(fit, 0.25f) / p.scene_scale;
    };

    const float ui_final_shift_px = shift_px_at(m_flat3d_gui_depth->value() * conv_uu);
    p.ui_shift_px = (ui_final_shift_px - overlay_shift_bias) / p.scene_scale;
    p.ui_scale = overlay_fit_scale(ui_final_shift_px);

    // UEVR's own menu layer — same depth-shift + auto-scale treatment.
    const float menu_final_shift_px = shift_px_at(m_flat3d_menu_depth->value() * conv_uu);
    p.menu_shift_px = (menu_final_shift_px - overlay_shift_bias) / p.scene_scale;
    p.menu_scale = overlay_fit_scale(menu_final_shift_px);

    // Depth-aware GUI elements (HUD depth, crosshair, geometry cursor) fall back
    // to flat while a full-screen UI is up — depth-warping or region-shifting a
    // fullscreen GUI reads as corruption. Detect that cursor-independently: the
    // game being paused (controller-driven pause menus), OR the redirected UI
    // covering most of the screen (cursorless map/inventory/menu). Coverage is
    // hysteretic so it doesn't chatter around the threshold. A mouse cursor alone
    // no longer flattens anything — the stereo cursor handles it, so HUD depth
    // stays live during cursor-driven gameplay.
    const float cov_enter = m_flat3d_fullscreen_coverage->value() / 100.0f;
    static bool s_cov_fullscreen = false;
    if (cov_enter <= 0.0f) {
        s_cov_fullscreen = false;
    } else {
        const float coverage = flat3d->ui_coverage.load();
        if (coverage >= cov_enter) {
            s_cov_fullscreen = true;
        } else if (coverage < cov_enter - 0.08f) {
            s_cov_fullscreen = false;
        }
    }

    const bool in_menus = flat3d->game_paused.load() || s_cov_fullscreen;

    // Stereo cursor (None / GUI Depth / Geometry): whenever the game wants a
    // mouse cursor, draw a per-eye arrow at the OS cursor position (the hardware
    // cursor is suppressed in on_message). Geometry mode lands it on the scene
    // surface under the tip, but flattens to GUI depth while a full-screen UI is
    // up (same rule as the other depth elements).
    const int32_t cursor_mode = m_flat3d_cursor_mode->value(); // 0 None, 1 GUI, 2 Geometry
    p.cursor_size_px = (float)m_flat3d_cursor_size->value();

    if (cursor_mode != 0 && flat3d->game_wants_cursor.load()) {
        POINT pt{};
        RECT client{};
        const auto wnd = g_framework->get_window();

        if (wnd != nullptr && GetCursorPos(&pt) && ScreenToClient(wnd, &pt) &&
            GetClientRect(wnd, &client) && client.right > 0 && client.bottom > 0) {
            const float cu = (float)pt.x / (float)client.right;
            const float cv = (float)pt.y / (float)client.bottom;

            if (cu >= 0.0f && cu <= 1.0f && cv >= 0.0f && cv <= 1.0f) {
                p.cursor_enabled = true;
                p.cursor_uv[0] = cu;
                p.cursor_uv[1] = cv;
                p.cursor_depth_mode = (cursor_mode == 2 && !in_menus) ? 1 : 0;
            }
        }
    }

    p.hud_depth_mode = in_menus ? 0 : m_flat3d_hud_depth_mode->value();
    p.hud_k_px = (sep_uu * p00 * 0.25f) * (float)eye_w; // px per (1/z - 1/conv)
    p.hud_inv_conv_uu = 1.0f / conv_uu;
    p.hud_nearz_uu = flat3d->game_nearz.load();
    p.hud_marker_radius = m_flat3d_hud_marker_radius->value();
    p.ui_color_gate = m_flat3d_ui_color_gate->value();

    // Predicted screen flow of world-anchored UI from the camera rotation:
    // yaw moves it horizontally, pitch vertically (markers track both).
    if (p.hud_depth_mode == 1) {
        const float tan_half_v = std::max(flat3d->game_tan_half_v.load(), 1e-3f);
        const float safe_tan_h = std::max(tan_half, 1e-3f);

        p.hud_flow_du = -flat3d->cam_dyaw.load() / (2.0f * safe_tan_h);
        p.hud_flow_dv = flat3d->cam_dpitch.load() / (2.0f * tan_half_v);

        const float mag = std::sqrt(p.hud_flow_du * p.hud_flow_du + p.hud_flow_dv * p.hud_flow_dv);
        // Too small = below tile-noise floor, too big = aliased match.
        p.hud_flow_valid = mag > 0.0015f && mag < 0.06f;

        // Coarse locomotion signal: lateral camera translation slides world
        // markers across the screen via parallax even with no look-rotation.
        // Expressed at the convergence depth so its floor matches the rotation
        // flow's; the shader treats "moved while translating" as world-evidence
        // rather than modelling the (depth-dependent) parallax vector per pixel.
        const float trans_mag = flat3d->cam_dtrans_lat.load()
                                / std::max(conv_uu, 1.0f) / (2.0f * safe_tan_h);
        p.hud_translating = trans_mag > m_flat3d_hud_trans_floor->value() && trans_mag < 0.5f;
        p.hud_debug = m_flat3d_hud_debug->value();
        p.hud_icon_radius = m_flat3d_hud_icon_radius->value();
        p.hud_stem_reach = m_flat3d_hud_stem_reach->value();

        // Mode-1 false-positive rejection tuning (see the classify shader).
        p.hud_trans_gate = m_flat3d_hud_trans_gate->value();
        p.hud_rot_gate = m_flat3d_hud_rot_gate->value();

        const bool occ_on = m_flat3d_hud_occlude_panels->value();
        p.hud_occ_gate = occ_on ? m_flat3d_hud_occ_gate->value() : 2.0f; // >1 disables permanent-panel suppression
        p.hud_halo_tiles = occ_on ? m_flat3d_hud_panel_halo->value() : 0;
        p.hud_occ_safe_hw = m_flat3d_hud_occ_safe_hw->value();
        p.hud_occ_safe_hh = m_flat3d_hud_occ_safe_hh->value();

        p.hud_fill_radius = (float)m_flat3d_hud_fill_radius->value();
        p.hud_fill_gate = m_flat3d_hud_reject_fills->value()
                              ? m_flat3d_hud_fill_gate->value()
                              : 2.0f; // >1 disables the areal-fill reject

        int ec = 0;
        for (int e = 0; e < 4; ++e) {
            const float hw = m_flat3d_hud_excl_hw[e]->value();
            const float hh = m_flat3d_hud_excl_hh[e]->value();
            if (hw > 0.0f && hh > 0.0f) {
                p.hud_excl[ec][0] = m_flat3d_hud_excl_cx[e]->value();
                p.hud_excl[ec][1] = m_flat3d_hud_excl_cy[e]->value();
                p.hud_excl[ec][2] = hw;
                p.hud_excl[ec][3] = hh;
                ++ec;
            }
        }
        p.hud_excl_count = ec;
    }

    if (p.hud_depth_mode == 2) {
        std::scoped_lock anchors_lock{flat3d->anchors_mtx};
        p.anchor_count = (uint32_t)std::min<size_t>(flat3d->anchors_published.size(),
                                                    vrmod::flat3d::Flat3DFrameParams::kMaxHudAnchors);
        for (uint32_t i = 0; i < p.anchor_count; ++i) {
            p.anchors[i][0] = flat3d->anchors_published[i][0];
            p.anchors[i][1] = flat3d->anchors_published[i][1];
            p.anchors[i][2] = flat3d->anchors_published[i][2];
        }
    }

    // Crosshair at the aimed depth (adaptive) or the static depth. Suppressed
    // while the game shows a mouse cursor — the region extraction/laser sits
    // in the middle of menu GUIs otherwise.
    p.crosshair_mode = in_menus ? 0 : (uint32_t)m_flat3d_crosshair_mode->value();

    if (p.crosshair_mode != 0) {
        float z_uu = -1.0f;

        if (m_flat3d_crosshair_adaptive->value()) {
            z_uu = flat3d->center_depth_uu.load();
        }

        if (z_uu <= 0.0f) {
            z_uu = m_flat3d_crosshair_static_depth->value() * w2m;
        }

        p.crosshair_shift_px = overlay_shift_at(z_uu);
        p.crosshair_region_radius = m_flat3d_crosshair_region_radius->value();
        p.crosshair_region_center_y = m_flat3d_crosshair_region_center_y->value();
        p.crosshair_size_px = (float)m_flat3d_crosshair_size->value();
        p.crosshair_color_argb = (uint32_t)m_flat3d_crosshair_color->value();
    }

    // Depth readback wanted while either consumer is active.
    p.want_depth = (p.crosshair_mode != 0 && m_flat3d_crosshair_adaptive->value())
                 || m_flat3d_autoconv_enabled->value()
                 || cursor_mode == 2; // geometry cursor needs SceneDepthZ

    // Display color correction (SDR only; the shader gates on colorspace).
    p.correction_enabled = m_flat3d_correction_enabled->value();
    p.lift[0] = m_flat3d_lift_r->value();
    p.lift[1] = m_flat3d_lift_g->value();
    p.lift[2] = m_flat3d_lift_b->value();
    p.gamma[0] = m_flat3d_gamma_r->value();
    p.gamma[1] = m_flat3d_gamma_g->value();
    p.gamma[2] = m_flat3d_gamma_b->value();
    p.gain[0] = m_flat3d_gain_r->value();
    p.gain[1] = m_flat3d_gain_g->value();
    p.gain[2] = m_flat3d_gain_b->value();
    p.curve = m_flat3d_curve->value();

    return p;
}

// VRto3D-matching hotkeys (always polled while flat3d is active, held-repeat):
//   Ctrl+F3 / Ctrl+F4  = depth -/+ 0.001 m per frame
//   Ctrl+F5 / Ctrl+F6  = convergence -/+ 0.005 m per frame
// The remappable single-key binds are checked as well.
void VR::handle_flat3d_keybinds() {
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    const auto adjust_depth = [this](float delta) {
        auto& v = m_flat3d_depth->value();
        v = std::clamp(v + delta, 0.0f, 0.5f);
    };
    const auto adjust_conv = [this](float delta) {
        auto& v = m_flat3d_convergence->value();
        v = std::clamp(v + delta, 0.01f, 25.0f);
    };

    if (ctrl && (GetAsyncKeyState(VK_F3) & 0x8000)) {
        adjust_depth(-0.001f);
    }
    if (ctrl && (GetAsyncKeyState(VK_F4) & 0x8000)) {
        adjust_depth(0.001f);
    }
    if (ctrl && (GetAsyncKeyState(VK_F5) & 0x8000)) {
        adjust_conv(-0.005f);
    }
    if (ctrl && (GetAsyncKeyState(VK_F6) & 0x8000)) {
        adjust_conv(0.005f);
    }

    // 3D screenshot: fixed hotkey Ctrl+F12 (also a button in the menu header).
    {
        static bool s_ss_was_down = false;
        const bool ss_down = ctrl && (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (ss_down && !s_ss_was_down) {
            request_flat3d_screenshot();
        }
        s_ss_was_down = ss_down;
    }
}

// Queues a 3D screenshot of the next composited stereo frame (Ctrl+F12 / the
// menu-header button).
void VR::request_flat3d_screenshot() {
    if (auto* f = get_flat3d_runtime()) {
        f->screenshot_requested.store(true);
    }
}

namespace {
// TextDisabled doesn't wrap — long hints get cut off at narrow menu widths.
void text_disabled_wrapped(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}
} // namespace

void VR::on_draw_sidebar_flat3d() {
    auto flat3d = get_flat3d_runtime();
    const bool active = is_using_flat3d();

    if (!active) {
        ImGui::TextWrapped("3D Display mode is not the active runtime.\n"
                           "Select \"3D Display\" in the injector frontend (or set\n"
                           "Frontend_RequestedRuntime=flat3d in config.txt) and restart.");
        return;
    }

    // Scrollable body: the page is taller than the menu window. NavFlattened so
    // gamepad/keyboard nav crosses into it directly — without it the scroll child
    // is a separate focus scope and the page needs a second "click" to enter.
    if (!ImGui::BeginChild("flat3d_page", ImVec2(0, 0), false, ImGuiWindowFlags_::ImGuiWindowFlags_NavFlattened)) {
        ImGui::EndChild();
        return;
    }
    utility::ScopeGuard scroll_guard{[]() { ImGui::EndChild(); }};

    m_flat3d_output_mode->draw("Output Mode");

    {
        using vrmod::flat3d::Flat3DOutputMode;
        const auto mode = (Flat3DOutputMode)m_flat3d_output_mode->value();

        if (mode == Flat3DOutputMode::LEIA_SR) {
            text_disabled_wrapped("LeiaSR needs a build with the SR SDK and a running SRService "
                                  "(D3D11 or D3D12). Falls back to Side-by-Side otherwise.");
        } else if (mode == Flat3DOutputMode::KATANGA) {
            text_disabled_wrapped("Katanga — shares a side-by-side stereo texture with Katanga.exe / "
                                  "VRScreenCap (launch either, any order). Forces the game into a normal "
                                  "window; the local window shows a left-eye preview.");
        } else if (mode >= Flat3DOutputMode::FRAMEPACKED_720P60 && mode <= Flat3DOutputMode::FRAMEPACKED_1080P60) {
            text_disabled_wrapped("Frame-packed HDMI 1.4 — auto-applies a custom display timing via "
                                  "NVIDIA/AMD (or a CRU-preconfigured mode); reverts on mode change/exit.");
        } else if (mode == Flat3DOutputMode::DUAL_DISPLAY || mode == Flat3DOutputMode::DUAL_DISPLAY_FLIP) {
            text_disabled_wrapped("Dual Display — set the game to span both monitors side by side.");
        }

        // Pixel-exact modes need a display-native output chain: the pattern /
        // lenticular weave is destroyed by any scaling AFTER our composite
        // (display mode scaling, DWM window upscale). Our internal upscaling
        // only covers the game's RENDER resolution/scale.
        if (mode == Flat3DOutputMode::LEIA_SR || mode == Flat3DOutputMode::ROW_INTERLACED ||
            mode == Flat3DOutputMode::COL_INTERLACED || mode == Flat3DOutputMode::CHECKERBOARD) {
            const auto rt = g_framework->get_rt_size();

            if (const auto di = query_display_info(g_framework->get_window())) {
                const bool display_scaled = di->cur_w != di->native_w || di->cur_h != di->native_h;
                const bool window_scaled = (int)rt.x != di->cur_w || (int)rt.y != di->cur_h;

                if (display_scaled || window_scaled) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.2f, 1.0f));
                    if (display_scaled) {
                        ImGui::TextWrapped("WARNING: display is running %dx%d but its native mode is %dx%d — "
                                           "this mode needs the native display mode (no 3D otherwise).",
                                           di->cur_w, di->cur_h, di->native_w, di->native_h);
                    }
                    if (window_scaled) {
                        ImGui::TextWrapped("WARNING: game output is %dx%d but the display area is %dx%d — "
                                           "set the in-game resolution to match (use render/resolution SCALE "
                                           "to lower GPU cost instead; that path is upscaled correctly).",
                                           (int)rt.x, (int)rt.y, di->cur_w, di->cur_h);
                    }
                    ImGui::PopStyleColor();
                }
            }
        }
    }

    m_flat3d_eye_swap->draw("Swap Eyes");
    ImGui::SameLine();
    ImGui::TextDisabled("(also flips interlace phase)");

    m_flat3d_vsync->draw("VSync Override");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Force On = tear-free at the display rate (classic vsync pacing).\n"
                          "No-Tear Fast (default) = overrides the game's own VSync: presents run\n"
                          "uncapped at sync interval 0 with the DXGI tearing flag stripped. A\n"
                          "flip-model swapchain (all DX12 games) still flips on vblank only -\n"
                          "tear-free AND both AFR eye frames land each refresh. t.MaxFPS\n"
                          "auto-caps at 2x refresh under AFR/Synced/AFW, 1x under Native Stereo.\n"
                          "(DX11 exclusive-fullscreen can still tear at interval 0 - use Force On\n"
                          "there, or run borderless windowed.)");
    }

    m_flat3d_force_sdr->draw("Force SDR Output");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("HDR output (PQ/scRGB swapchains) washes out the 3D modes and\n"
                          "disables color correction — this switches the game back to SDR.\n"
                          "Turn off to experiment with HDR passthrough.");
    }

    text_disabled_wrapped("Output is held at the display's native resolution; the in-game "
                          "resolution setting controls the 3D render resolution (upscaled).");

    m_flat3d_render_scale->draw("3D Render Resolution");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Per-eye render resolution as a fraction of the display's native size.\n"
                          "Auto preserves the game's own resolution setting — but if the game runs\n"
                          "(or persists) native resolution, Auto renders each eye at full native.\n"
                          "Pick an explicit fraction to cap the render cost regardless.");
    }

    if (flat3d != nullptr && flat3d->get_width() != 0) {
        ImGui::Text("Rendering %ux%u per eye", flat3d->get_width(), flat3d->get_height());
    }

    ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
    if (ImGui::TreeNode("3D Calibration")) {
        text_disabled_wrapped("Depth & Convergence are calibrated AT the Reference FoV. "
                              "Separation auto-scales with the game's live FoV (always on). "
                              "Hotkeys: Ctrl+F3/F4 depth, Ctrl+F5/F6 convergence.");
        m_flat3d_depth->draw("Depth");
        m_flat3d_convergence->draw("Convergence");
        m_flat3d_reference_fov->draw("Reference FoV (deg)");

        if (flat3d != nullptr) {
            ImGui::Text("Effective separation: %.4f m  convergence: %.3f m",
                        flat3d->separation_m.load(), flat3d->convergence_m.load());
            const auto tan_half = flat3d->game_tan_half_h.load();
            if (tan_half > 0.0f) {
                ImGui::Text("Live game hFoV: %.1f deg", glm::degrees(2.0f * std::atan(tan_half)));
            }
        }

        if (ImGui::Button("Reset to Defaults##flat3d_calib")) {
            m_flat3d_depth->value() = 0.1f;
            m_flat3d_convergence->value() = 1.0f;
            m_flat3d_reference_fov->value() = 90.0f;
        }

        ImGui::TreePop();
    }

    if (m_flat3d_depth_source->draw("Depth Source")) {
        // Mirror the legacy toggle so pre-combo configs/builds stay coherent
        // and the read-through promotion never fights an explicit choice.
        m_flat3d_use_engine_depth->value() =
            (m_flat3d_depth_source->value() == FLAT3D_DEPTH_ENGINE_POOL);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Scene-depth source for Adaptive Crosshair and HUD Depth.\n"
                          "Per-Draw Capture (default): safe API-level capture of the game's\n"
                          "   rendering. Works on most titles; blind to depth allocated\n"
                          "   before the overlay hooks (features stay flat).\n"
                          "Engine Pool: read the engine's SceneDepthZ from its render-target\n"
                          "   pool. Fixes titles that allocate depth once at load (e.g.\n"
                          "   SMT5V), but installs an engine hook that crashes a few games\n"
                          "   (e.g. Jedi: Survivor).\n"
                          "DSV Observer (D3D12 only): watch depth-stencil views and resource\n"
                          "   barriers at the API level and snapshot the live scene depth.\n"
                          "   No engine hook (safe where Engine Pool crashes) and sees depth\n"
                          "   allocated at any time (works where Per-Draw stays flat).\n"
                          "DLSS Depth (D3D12 only): copy the depth buffer the game hands to\n"
                          "   DLSS each frame. No plugin and no AFW needed; exact and cheap,\n"
                          "   but only has data while the game's DLSS is enabled and active\n"
                          "   (render-resolution copy). Best pick for DLSS titles.");
    }

    if (flat3d_depth_source() == FLAT3D_DEPTH_DSV_OBSERVER) {
        if (!m_is_d3d12) {
            ImGui::TextDisabled("DSV Observer requires D3D12 - falling back to Per-Draw Capture.");
        } else {
            ImGui::TextDisabled("DSV: %s", m_d3d12.get_flat3d_depth_trace_summary().c_str());
        }
    }
    if (flat3d_depth_source() == FLAT3D_DEPTH_DLSS) {
        if (!m_is_d3d12) {
            ImGui::TextDisabled("DLSS Depth requires D3D12 - falling back to Per-Draw Capture.");
        } else if (last_dlss_frame_count == 0) {
            ImGui::TextDisabled("DLSS Depth: waiting for the game's DLSS (enable DLSS in-game).");
        } else {
            ImGui::TextDisabled("DLSS Depth: active.");
        }
    }

    if (ImGui::TreeNode("Auto-Convergence")) {
        m_flat3d_autoconv_enabled->draw("Enable");
        m_flat3d_autoconv_target_disparity->draw("Target Disparity");
        m_flat3d_autoconv_smoothing->draw("Smoothing");
        m_flat3d_autoconv_min_conv->draw("Min Convergence");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Auto-convergence never pulls the screen plane closer than this.\n"
                              "Raise it if close objects drag the whole scene too deep.");
        }
        m_flat3d_autoconv_logging->draw("Log Samples");
        text_disabled_wrapped("Separation auto-scales with the pull-in so the background "
                              "stays exactly where you calibrated it.");

        if (flat3d != nullptr) {
            const auto nearest = flat3d->nearest_depth_uu.load();
            const auto center = flat3d->center_depth_uu.load();
            const float w2m = m_world_to_meters != 0.0f ? m_world_to_meters : 100.0f;
            if (nearest > 0.0f) {
                ImGui::Text("Nearest scene depth: %.2f m", nearest / w2m);
            } else {
                ImGui::TextDisabled("Nearest scene depth: (no signal)");
            }
            if (center > 0.0f) {
                ImGui::Text("Aim depth: %.2f m", center / w2m);
            } else {
                ImGui::TextDisabled("Aim depth: (no signal)");
            }
        }

        if (ImGui::Button("Reset to Defaults##flat3d_autoconv")) {
            m_flat3d_autoconv_enabled->value() = false;
            m_flat3d_autoconv_target_disparity->value() = 0.005f;
            m_flat3d_autoconv_smoothing->value() = 0.08f;
            m_flat3d_autoconv_min_conv->value() = 0.5f;
            m_flat3d_autoconv_logging->value() = false;
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Crosshair")) {
        m_flat3d_crosshair_mode->draw("Mode");

        const auto ch_mode = m_flat3d_crosshair_mode->value();

        if (ch_mode != 0) {
            m_flat3d_crosshair_adaptive->draw("Adaptive Depth");
        }
        if (ch_mode == 1) {
            m_flat3d_crosshair_region_radius->draw("Region Radius");
            m_flat3d_crosshair_region_center_y->draw("Region Center Y");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Vertical position of the extracted crosshair region.\n"
                                  "0 = top of the screen, 1 = bottom.");
            }
        }
        if (ch_mode == 2) {
            m_flat3d_crosshair_size->draw("Dot Size (px)");
            m_flat3d_crosshair_color->draw("Dot Color (ARGB)");
        }
        if (ch_mode != 0) {
            m_flat3d_crosshair_static_depth->draw("Static Depth");
        }

        if (ImGui::Button("Reset to Defaults##flat3d_crosshair")) {
            m_flat3d_crosshair_mode->value() = 1; // Game Crosshair
            m_flat3d_crosshair_adaptive->value() = true;
            m_flat3d_crosshair_region_radius->value() = 0.04f;
            m_flat3d_crosshair_region_center_y->value() = 0.5f;
            m_flat3d_crosshair_size->value() = 8;
            m_flat3d_crosshair_color->value() = (int32_t)0xFFFFFFFF;
            m_flat3d_crosshair_static_depth->value() = 2.0f;
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("GUI")) {
        m_flat3d_hud_depth_mode->draw("HUD Depth Mode");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Flat: whole UI on one plane at GUI Depth.\n"
                              "Depth-Adaptive: UI regions that MOVE with the camera (waypoints,\n"
                              "nameplates) sit at the scene depth below them; static HUD stays\n"
                              "flat. Classification learns as you look around (yaw AND pitch).\n"
                              "World Markers: hooks the engine's world->screen projection calls\n"
                              "and puts the UI around each marker at that object's true depth\n"
                              "(coverage depends on how the game drives its HUD).\n"
                              "Non-flat modes fall back to Flat while a mouse cursor is shown.");
        }

        m_flat3d_fullscreen_coverage->draw("Full-Screen UI Coverage %");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Flatten the HUD/crosshair depth once the game's UI covers at least\n"
                              "this %% of the screen — catches full-screen menus that DON'T show a\n"
                              "mouse cursor (map/inventory/controller menus). Raise it if a busy\n"
                              "gameplay HUD trips it; lower it if a full-screen menu isn't caught.\n"
                              "0 disables coverage (mouse-cursor + game-paused detection still apply).");
        }


        if (m_flat3d_hud_depth_mode->value() == 1) {
            m_flat3d_hud_icon_radius->draw("Icon Region Radius");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How far around a detected world-tracking element the depth\n"
                                  "shift extends. Raise it if icons get cut off or flicker at\n"
                                  "their edges; lower it if nearby static HUD gets dragged along.");
            }
            m_flat3d_hud_stem_reach->draw("Stem Reach");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Some markers have a thin leader-line stem hanging off the icon\n"
                                  "(usually straight down). Thin lines classify unstably on their\n"
                                  "own, so extend the icon region vertically to pull the stem to the\n"
                                  "icon's depth. Positive = stem hangs DOWN, negative = hangs UP,\n"
                                  "0 = off. Unlike Icon Region Radius this only reaches one way, so\n"
                                  "it doesn't drag sideways HUD along. Raise the magnitude until the\n"
                                  "whole stem picks up depth; back off if HUD past the stem drifts.");
            }
            m_flat3d_hud_debug->draw("Show Classification (debug)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Tints the HUD by its classification: red = world-anchored\n"
                                  "(gets scene depth), green = static (stays flat). Look around\n"
                                  "to train it.");
            }

            if (ImGui::TreeNode("False-Positive Rejection")) {
                ImGui::TextWrapped("Stops animated-but-fixed HUD (minimap radar, gauges) and "
                                   "permanent panels from being read as world-tracking UI. Turn on "
                                   "the debug view and watch the red while tuning.");

                m_flat3d_hud_occlude_panels->draw("Suppress Permanent Panels");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Tiles covered every frame (minimap disc, gauge cluster) are\n"
                                      "permanent HUD; force them and a halo around them flat.");
                }
                if (m_flat3d_hud_occlude_panels->value()) {
                    ImGui::Indent();
                    m_flat3d_hud_occ_gate->draw("Panel Occupancy");
                    m_flat3d_hud_panel_halo->draw("Panel Halo (tiles)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Grows the flat region a few tiles past a panel edge so\n"
                                          "icons protruding past the minimap rim are absorbed too.");
                    }
                    m_flat3d_hud_occ_safe_hw->draw("Marker Safe Zone W");
                    m_flat3d_hud_occ_safe_hh->draw("Marker Safe Zone H");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Occupancy is DISABLED inside this central box (half-size\n"
                                          "from screen center), so world markers that linger there are\n"
                                          "never flattened — panels live outside it. Shrink to suppress\n"
                                          "a more central panel; 0.5 protects the whole screen.");
                    }
                    ImGui::Unindent();
                }

                m_flat3d_hud_reject_fills->draw("Reject Large UI Fills");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("A big contiguous UI fill (menu backdrop, blurred scene scrim)\n"
                                      "is never a world marker — a marker is a small island. Forces\n"
                                      "flat immediately (no occupancy delay) and works dead-center,\n"
                                      "unlike the safe zone. Fixes menu backdrops warping the HUD.");
                }
                if (m_flat3d_hud_reject_fills->value()) {
                    ImGui::Indent();
                    m_flat3d_hud_fill_radius->draw("Fill Radius (tiles)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("How large a filled region counts as a backdrop/panel.\n"
                                          "Bigger = only very large fills are rejected (safer for\n"
                                          "big markers); smaller catches smaller panels too.");
                    }
                    m_flat3d_hud_fill_gate->draw("Fill Coverage");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Fraction of the block that must be covered to call it a\n"
                                          "fill. Lower to also catch a backdrop's edge; raise if real\n"
                                          "markers near dense HUD get flattened.");
                    }
                    ImGui::Unindent();
                }

                m_flat3d_hud_trans_gate->draw("Translation Gate");
                m_flat3d_hud_rot_gate->draw("Rotation Gate");
                m_flat3d_hud_trans_floor->draw("Translation Floor");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Raise the gates to demand stronger motion evidence before a\n"
                                      "tile is called world (fewer false positives, may miss subtle\n"
                                      "markers). Floor ignores tiny camera drift.");
                }

                if (ImGui::TreeNode("Exclusion Zones")) {
                    ImGui::TextWrapped("Rectangles that are always flat HUD. Center + half-size in "
                                       "output UV (0..1). Set a zone's Half Width or Half Height to 0 "
                                       "to disable it.");
                    for (int e = 0; e < 4; ++e) {
                        ImGui::PushID(e);
                        ImGui::Text("Zone %d", e + 1);
                        ImGui::Indent();
                        m_flat3d_hud_excl_cx[e]->draw("Center X");
                        m_flat3d_hud_excl_cy[e]->draw("Center Y");
                        m_flat3d_hud_excl_hw[e]->draw("Half Width");
                        m_flat3d_hud_excl_hh[e]->draw("Half Height");
                        ImGui::Unindent();
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }

                ImGui::TreePop();
            }
        }

        if (m_flat3d_hud_depth_mode->value() == 2) {
            m_flat3d_hud_marker_radius->draw("Marker Region Radius");

            if (auto* f = get_flat3d_runtime()) {
                std::scoped_lock _{f->anchors_mtx};
                ImGui::Text("Detected markers this frame: %zu", f->anchors_published.size());
            }
        }

        m_flat3d_gui_depth->draw("GUI Depth");
        m_flat3d_menu_depth->draw("UU3D Menu Depth");
        m_flat3d_cursor_mode->draw("Stereo Cursor");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Replaces the system cursor (drawn flat by the OS at screen depth,\n"
                              "one copy) with a per-eye cursor whenever the game shows one.\n"
                              "None: off. GUI Depth: at the GUI plane. Geometry: lands on the\n"
                              "scene surface under the tip (needs a scene depth source; falls\n"
                              "back to GUI depth over full-screen menus).");
        }
        if (m_flat3d_cursor_mode->value() != 0) {
            m_flat3d_cursor_size->draw("Cursor Size (px)");
        }
        text_disabled_wrapped("Depths are relative to the (auto-)convergence: 1.0 = at the "
                              "screen plane (no shift, no scaling). Below 1 pops out, above 1 "
                              "sits behind; layers scale down as they shift so nothing is "
                              "pushed off screen. "
                              "Menu font size: main UU3D menu > Configuration > Font Size.");

        if (ImGui::Button("Reset to Defaults##flat3d_gui")) {
            m_flat3d_hud_depth_mode->value() = 0;
            m_flat3d_hud_marker_radius->value() = 0.06f;
            m_flat3d_hud_icon_radius->value() = 0.035f;
            m_flat3d_hud_stem_reach->value() = 0.0f;
            m_flat3d_gui_depth->value() = 1.0f;
            m_flat3d_menu_depth->value() = 1.0f;
            m_flat3d_cursor_mode->value() = 0;
            m_flat3d_cursor_size->value() = 32;

            // Mode-1 false-positive rejection.
            m_flat3d_hud_trans_gate->value() = 0.06f;
            m_flat3d_hud_rot_gate->value() = 0.05f;
            m_flat3d_hud_trans_floor->value() = 0.0015f;
            m_flat3d_hud_occlude_panels->value() = true;
            m_flat3d_hud_occ_gate->value() = 0.85f;
            m_flat3d_hud_panel_halo->value() = 2;
            m_flat3d_hud_occ_safe_hw->value() = 0.35f;
            m_flat3d_hud_occ_safe_hh->value() = 0.35f;
            m_flat3d_hud_reject_fills->value() = true;
            m_flat3d_hud_fill_radius->value() = 6;
            m_flat3d_hud_fill_gate->value() = 0.6f;
            for (int e = 0; e < 4; ++e) {
                m_flat3d_hud_excl_cx[e]->value() = 0.0f;
                m_flat3d_hud_excl_cy[e]->value() = 0.0f;
                m_flat3d_hud_excl_hw[e]->value() = 0.0f;
                m_flat3d_hud_excl_hh[e]->value() = 0.0f;
            }
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Head Tracking (OpenTrack)")) {
        m_flat3d_opentrack_enabled->draw("Enable");
        m_flat3d_opentrack_port->draw("UDP Port");
        m_flat3d_opentrack_rot_scale->draw("Look Sensitivity");
        m_flat3d_opentrack_pos_scale->draw("Parallax Sensitivity");
        text_disabled_wrapped("Point OpenTrack's 'UDP over network' output at 127.0.0.1:<port>. "
                              "Bind a Recenter key in Advanced to zero the neutral pose.");
        if (flat3d != nullptr && flat3d->opentrack_active.load()) {
            ImGui::Text("Head: yaw %.1f pitch %.1f (deg)",
                        glm::degrees(flat3d->head_yaw.load()), glm::degrees(flat3d->head_pitch.load()));
        }

        if (ImGui::Button("Reset to Defaults##flat3d_opentrack")) {
            m_flat3d_opentrack_enabled->value() = false;
            m_flat3d_opentrack_port->value() = 4242;
            m_flat3d_opentrack_rot_scale->value() = 1.0f;
            m_flat3d_opentrack_pos_scale->value() = 1.0f;
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Color Correction (SDR)")) {
        m_flat3d_correction_enabled->draw("Enable");
        m_flat3d_curve->draw("S-Curve Contrast");
        m_flat3d_lift_r->draw("Lift R"); m_flat3d_lift_g->draw("Lift G"); m_flat3d_lift_b->draw("Lift B");
        m_flat3d_gamma_r->draw("Gamma R"); m_flat3d_gamma_g->draw("Gamma G"); m_flat3d_gamma_b->draw("Gamma B");
        m_flat3d_gain_r->draw("Gain R"); m_flat3d_gain_g->draw("Gain G"); m_flat3d_gain_b->draw("Gain B");

        if (ImGui::Button("Reset to Defaults##flat3d_color")) {
            m_flat3d_correction_enabled->value() = false;
            m_flat3d_curve->value() = 1.0f;
            m_flat3d_lift_r->value() = 0.0f;  m_flat3d_lift_g->value() = 0.0f;  m_flat3d_lift_b->value() = 0.0f;
            m_flat3d_gamma_r->value() = 1.0f; m_flat3d_gamma_g->value() = 1.0f; m_flat3d_gamma_b->value() = 1.0f;
            m_flat3d_gain_r->value() = 1.0f;  m_flat3d_gain_g->value() = 1.0f;  m_flat3d_gain_b->value() = 1.0f;
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Advanced")) {
        m_flat3d_hdr_paper_white->draw("HDR Paper White (nits)");

        m_flat3d_d3d12_debug_layer->draw("D3D12 Debug Layer Log");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Logs D3D12 validation messages (e.g. SceneDepthZ resource-barrier state\n"
                "mismatches) into the UEVR log to diagnose depth-feature crashes.\n"
                "Requires the debug layer on the GAME's device, which must be enabled BEFORE\n"
                "it creates the device:\n"
                "  - Install the Windows 'Graphics Tools' optional feature, then run dxcpl.exe,\n"
                "    Add the game's .exe, set the Direct3D Debug Layer to 'Force On' (scope: this\n"
                "    app), Apply, and relaunch; OR\n"
                "  - Set the environment variable UEVR_D3D12_DEBUG=1 if UEVR injects at startup.\n"
                "If neither is done, the log explains that no debug layer was found. D3D12 only.");
        }

        ImGui::TreePop();
    }
}
