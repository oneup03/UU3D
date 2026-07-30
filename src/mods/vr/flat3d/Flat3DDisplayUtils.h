#pragma once

// Display position snapshot/restore + modeset settle helpers, ported verbatim
// from VRto3D's display_utils.h. Used by Flat3DDisplayTiming to keep the
// multi-monitor layout stable across a custom-timing modeset.

#include <string>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vrmod::flat3d {
namespace display_utils {

struct DisplayPositionSnapshot {
    std::wstring deviceName;
    POINTL       position;
};

inline std::vector<DisplayPositionSnapshot> SnapshotDisplayPositions()
{
    std::vector<DisplayPositionSnapshot> snapshots;
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
            dd = {}; dd.cb = sizeof(dd); continue;
        }
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0)) {
            DisplayPositionSnapshot snap;
            snap.deviceName = dd.DeviceName;
            snap.position   = dm.dmPosition;
            snapshots.push_back(snap);
        }
        dd = {}; dd.cb = sizeof(dd);
    }
    return snapshots;
}

inline void RestoreDisplayPositions(const std::vector<DisplayPositionSnapshot>& snapshots)
{
    if (snapshots.empty()) return;

    UINT32 numPaths = 0, numModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPaths, &numModes) != ERROR_SUCCESS)
        return;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPaths, paths.data(),
                           &numModes, modes.data(), nullptr) != ERROR_SUCCESS)
        return;
    paths.resize(numPaths);
    modes.resize(numModes);

    std::unordered_map<std::wstring, POINTL> posMap;
    for (const auto& snap : snapshots) posMap[snap.deviceName] = snap.position;

    for (auto& mode : modes) {
        if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName = {};
        srcName.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size      = sizeof(srcName);
        srcName.header.adapterId = mode.adapterId;
        srcName.header.id        = mode.id;
        if (DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS) continue;
        auto it = posMap.find(srcName.viewGdiDeviceName);
        if (it == posMap.end()) continue;
        mode.sourceMode.position.x = it->second.x;
        mode.sourceMode.position.y = it->second.y;
    }

    SetDisplayConfig(numPaths, paths.data(), numModes, modes.data(),
                     SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_NO_OPTIMIZATION);
}

inline void WaitForModesetSettle(DWORD timeoutMs)
{
    DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

}  // namespace display_utils
}  // namespace vrmod::flat3d
