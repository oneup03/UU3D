#pragma once

// ── UEVR "dumper mode" ────────────────────────────────────────────
//
// Opt-in runtime flag that gates the render-pipeline hooks UEVR installs
// for VR work. When a plugin (e.g. uevr-mcp's reflection dumper) only
// needs UObject access + the engine-tick callback, the stereo rendering,
// scene view, slate thread, XR tracking, D3D rehook-monitor, and Windows
// message hooks are dead weight — and on some titles (RoboQuest's UE 4.26
// fork, Stellar Blade's UE 4.26.2 fork) they deterministically crash the
// render thread within seconds of injection.
//
// Dumper mode turns them off. The plugin loader, UObject reflection
// subsystem, and UGameEngine::Tick hook all stay active, so any
// plugin that only uses the reflection API (like uevr-mcp's
// uevr_dump_*) works normally but the crash window disappears.
//
// Two ways to enable:
//   1. Environment variable UEVR_DUMPER_MODE=1 in the game process.
//   2. Sentinel file `dumper_mode` in the per-game config directory —
//      %APPDATA%\UU3D\<GameExeStem>\dumper_mode
//
// File sentinel is preferred for games launched via Steam / Epic where
// env vars don't propagate through the launcher shell. The MCP server's
// setup-game flow writes the sentinel before injection.
//
// Read once, cached for the lifetime of the process. Zero overhead when
// disabled.

#include <windows.h>
#include <shlobj.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace uevr {

inline bool is_dumper_mode() noexcept {
    static const bool cached = []() noexcept {
        // 1. Env var — explicit per-process override.
        if (const char* env = std::getenv("UEVR_DUMPER_MODE")) {
            if (std::strcmp(env, "1") == 0) return true;
            if (_stricmp(env, "true") == 0) return true;
            if (_stricmp(env, "yes") == 0) return true;
            if (_stricmp(env, "on") == 0) return true;
        }

        // 2. Sentinel file under %APPDATA%\UU3D\<GameExeStem>\dumper_mode
        // Build the path manually (UEVR's Framework::get_persistent_dir has
        // similar logic but this header is included early).
        try {
            wchar_t appdata[MAX_PATH] = {0};
            if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdata) == S_OK) {
                wchar_t exe_path[MAX_PATH] = {0};
                GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
                std::filesystem::path exe_fs(exe_path);
                const auto game_name = exe_fs.stem().wstring();
                std::filesystem::path sentinel = std::filesystem::path(appdata) / L"UU3D" / game_name / L"dumper_mode";
                if (std::filesystem::exists(sentinel)) return true;
            }
        } catch (...) { /* fall through to return false */ }

        return false;
    }();
    return cached;
}

} // namespace uevr
