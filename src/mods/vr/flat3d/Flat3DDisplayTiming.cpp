// Frame-packing display-timing helper — ported from VRto3D
// display_timing_helper.cpp (NVIDIA NvAPI + AMD ADL + CRU fallback). The
// vendor bodies are kept verbatim; only the logging shim, device-name
// resolution, and the FramePackTimingSpec table are UEVR-local.

#include "Flat3DDisplayTiming.hpp"
#include "Flat3DDisplayUtils.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <delayimp.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include <spdlog/spdlog.h>

// NVAPI at file scope so its global typedefs land in the global namespace.
#include "nvapi.h"

// AMD ADL — pure C headers.
#include "adl_sdk.h"
#include "adl_structures.h"
#include "adl_defines.h"

namespace vrmod::flat3d {

// ---------------------------------------------------------------------------
// Streaming log shim so the vendor `LOG() << ...` lines port verbatim.
// ---------------------------------------------------------------------------
namespace {
struct FpLog {
    std::ostringstream ss;
    template <typename T> FpLog& operator<<(const T& v) { ss << v; return *this; }
    ~FpLog() { spdlog::info("{}", ss.str()); }
};
}  // namespace
#define LOG() ::vrmod::flat3d::FpLog{}

// ---------------------------------------------------------------------------
// FramePackTimingSpec table (HDMI 1.4 frame-packing; VRto3D values).
// ---------------------------------------------------------------------------
namespace {
const FramePackTimingSpec s_frame_pack_timings[] = {
    // 720p60:  1280x1470 @60Hz, 30px gap. H 1650, V 1500. pclk 148.5 MHz.
    { 1280, 1470, 720, 30, 60.0f,   1650, 110, 40, 220,   1500, 5, 5, 20 },
    // 1080p24: 1920x2205 @24Hz, 45px gap. H 2750, V 2250. pclk 148.5 MHz.
    { 1920, 2205, 1080, 45, 24.0f,  2750, 638, 44, 148,   2250, 4, 5, 36 },
    // 1080p60: 1920x2205 @60Hz, 45px gap. H 2750, V 2250. pclk 371.25 MHz (HDMI 2.0+).
    { 1920, 2205, 1080, 45, 60.0f,  2750, 638, 44, 148,   2250, 4, 5, 36 },
};
}  // namespace

const FramePackTimingSpec* GetFramePackTimingSpec(Flat3DOutputMode m)
{
    switch (m) {
        case Flat3DOutputMode::FRAMEPACKED_720P60:  return &s_frame_pack_timings[0];
        case Flat3DOutputMode::FRAMEPACKED_1080P24: return &s_frame_pack_timings[1];
        case Flat3DOutputMode::FRAMEPACKED_1080P60: return &s_frame_pack_timings[2];
        default:                                    return nullptr;
    }
}

// ===========================================================================
// Shared helpers
// ===========================================================================

namespace {

using display_utils::DisplayPositionSnapshot;
using display_utils::SnapshotDisplayPositions;
using display_utils::RestoreDisplayPositions;
using display_utils::WaitForModesetSettle;

// Resolve the GDI device name for a display_index (0 = primary/auto, 1..N =
// explicit). Self-contained EnumDisplayDevices walk of desktop-attached
// devices (no external monitor-enumeration dependency).
std::wstring ResolveDeviceName(int32_t display_index)
{
    std::vector<std::wstring> names;
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
            names.emplace_back(dd.DeviceName);
        }
        dd = {}; dd.cb = sizeof(dd);
    }
    if (names.empty()) return {};

    size_t idx = 0;
    if (display_index > 0 && display_index <= static_cast<int32_t>(names.size())) {
        idx = static_cast<size_t>(display_index - 1);
    }
    return names[idx];
}

bool SnapshotCurrentMode(const std::wstring& device_name, DEVMODEW& out_mode)
{
    out_mode = {};
    out_mode.dmSize = sizeof(out_mode);
    return EnumDisplaySettingsExW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &out_mode, 0) != 0;
}

bool RestoreSavedMode(const std::wstring& device_name, DEVMODEW& saved_mode)
{
    if (device_name.empty() || saved_mode.dmSize == 0) return false;

    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (EnumDisplaySettingsExW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &current, 0)) {
        if (current.dmPelsWidth        == saved_mode.dmPelsWidth
         && current.dmPelsHeight       == saved_mode.dmPelsHeight
         && current.dmDisplayFrequency == saved_mode.dmDisplayFrequency) {
            return true;
        }
    }

    LONG result = ChangeDisplaySettingsExW(
        device_name.c_str(), &saved_mode, nullptr, CDS_FULLSCREEN, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        LOG() << "RestoreSavedMode: ChangeDisplaySettingsExW failed result=" << result
              << " — falling back to driver default";
        ChangeDisplaySettingsExW(device_name.c_str(), nullptr, nullptr, 0, nullptr);
    }
    return result == DISP_CHANGE_SUCCESSFUL;
}

uint32_t ComputePixelClock10kHz(const FramePackTimingSpec& spec)
{
    double pclk_hz = static_cast<double>(spec.h_total)
                   * static_cast<double>(spec.v_total)
                   * static_cast<double>(spec.refresh_hz);
    return static_cast<uint32_t>(std::round(pclk_hz / 10000.0));
}

}  // namespace


// ===========================================================================
// Backend state structures
// ===========================================================================

struct Flat3DDisplayTiming::NvidiaState {
    std::vector<NvU32> display_ids;
    std::wstring       device_name;
    DEVMODEW           original_mode{};
};

struct Flat3DDisplayTiming::AmdState {
    int          adapter_index  = -1;
    int          display_index  = -1;
    std::wstring device_name;
    DEVMODEW     original_mode{};
};

struct Flat3DDisplayTiming::CruState {
    std::wstring device_name;
    DEVMODEW     original_mode{};
};


// ===========================================================================
// lifecycle
// ===========================================================================

Flat3DDisplayTiming::~Flat3DDisplayTiming()
{
    Revert();
}


bool Flat3DDisplayTiming::Apply(const FramePackTimingSpec& spec, int32_t display_index)
{
    if (active_) {
        LOG() << "Flat3DDisplayTiming::Apply: already active, reverting first";
        Revert();
    }

    LOG() << "Flat3DDisplayTiming::Apply: "
          << spec.active_w << "x" << spec.active_h
          << " @" << spec.refresh_hz << "Hz"
          << " h_total=" << spec.h_total << " v_total=" << spec.v_total
          << " display_index=" << display_index;

    if (TryNvidia(spec, display_index)) {
        backend_ = Backend::Nvidia; active_ = true;
        LOG() << "Flat3DDisplayTiming: NVIDIA backend succeeded";
        return true;
    }
    if (TryAmd(spec, display_index)) {
        backend_ = Backend::Amd; active_ = true;
        LOG() << "Flat3DDisplayTiming: AMD backend succeeded";
        return true;
    }
    if (TryCruFallback(spec, display_index)) {
        backend_ = Backend::CruFallback; active_ = true;
        LOG() << "Flat3DDisplayTiming: CRU fallback succeeded";
        return true;
    }

    LOG() << "Flat3DDisplayTiming::Apply: all backends failed. Frame-pack TaB "
          << "rendering still works, but the HDMI 3D InfoFrame may not be sent. "
          << "Consider adding the timing via CRU.";
    return false;
}


void Flat3DDisplayTiming::Revert()
{
    if (!active_) return;

    LOG() << "Flat3DDisplayTiming::Revert (backend=" << static_cast<int>(backend_) << ")";

    switch (backend_) {
        case Backend::Nvidia:      RevertNvidia();      break;
        case Backend::Amd:         RevertAmd();         break;
        case Backend::CruFallback: RevertCruFallback(); break;
        default: break;
    }

    if (backend_state_) {
        switch (backend_) {
            case Backend::Nvidia:      delete static_cast<NvidiaState*>(backend_state_); break;
            case Backend::Amd:         delete static_cast<AmdState*>(backend_state_); break;
            case Backend::CruFallback: delete static_cast<CruState*>(backend_state_); break;
            default: break;
        }
        backend_state_ = nullptr;
    }

    active_  = false;
    backend_ = Backend::None;
}


// ===========================================================================
// NVIDIA backend
// ===========================================================================

namespace {

bool NvApiAvailable()
{
    __try {
        NvAPI_Status status = NvAPI_Initialize();
        return (status == NVAPI_OK);
    } __except (
        (GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
         GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND))
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH
    ) {
        return false;
    }
}

}  // namespace


bool Flat3DDisplayTiming::TryNvidia(const FramePackTimingSpec& spec, int32_t display_index)
{
    if (!NvApiAvailable()) {
        LOG() << "Flat3DDisplayTiming::TryNvidia: NVAPI not available";
        return false;
    }

    std::wstring w_name = ResolveDeviceName(display_index);
    if (w_name.empty()) {
        LOG() << "Flat3DDisplayTiming::TryNvidia: could not resolve device name for index "
              << display_index;
        return false;
    }

    char narrow_name[NVAPI_SHORT_STRING_MAX] = {};
    for (size_t i = 0; i < w_name.size() && i < sizeof(narrow_name) - 1; ++i)
        narrow_name[i] = static_cast<char>(w_name[i]);

    NvU32 displayId = 0;
    NvAPI_Status status = NvAPI_DISP_GetDisplayIdByDisplayName(narrow_name, &displayId);
    if (status != NVAPI_OK) {
        LOG() << "Flat3DDisplayTiming::TryNvidia: GetDisplayIdByDisplayName failed name='"
              << narrow_name << "' status=" << status;
        return false;
    }

    {
        NV_MOSAIC_TOPO_BRIEF      topoBrief   = {};
        NV_MOSAIC_DISPLAY_SETTING dispSetting = {};
        NvS32 overlapX = 0, overlapY = 0;
        topoBrief.version   = NVAPI_MOSAIC_TOPO_BRIEF_VER;
        dispSetting.version = NVAPI_MOSAIC_DISPLAY_SETTING_VER;
        NvAPI_Status ms = NvAPI_Mosaic_GetCurrentTopo(&topoBrief, &dispSetting, &overlapX, &overlapY);
        if (ms == NVAPI_OK && topoBrief.enabled) {
            LOG() << "Flat3DDisplayTiming::TryNvidia: Surround/Mosaic active — skipping.";
            return false;
        }
    }

    DEVMODEW original_mode{};
    SnapshotCurrentMode(w_name, original_mode);

    NV_TIMING live_timing = {};
    {
        NV_TIMING_INPUT ti = {};
        ti.version = NV_TIMING_INPUT_VER;
        NvAPI_Status ts = NvAPI_DISP_GetTiming(displayId, &ti, &live_timing);
        if (ts != NVAPI_OK) {
            LOG() << "Flat3DDisplayTiming::TryNvidia: GetTiming failed status=" << ts
                  << " — proceeding with default polarity";
        }
    }

    NV_TIMING t = {};
    t.pclk        = static_cast<NvU32>(ComputePixelClock10kHz(spec));
    t.HTotal      = spec.h_total;
    t.HVisible    = static_cast<NvU16>(spec.active_w);
    t.HBorder     = 0;
    t.HFrontPorch = spec.h_front_porch;
    t.HSyncWidth  = spec.h_sync_width;
    t.HSyncPol    = live_timing.HSyncPol;
    t.VTotal      = spec.v_total;
    t.VVisible    = static_cast<NvU16>(spec.active_h);
    t.VBorder     = 0;
    t.VFrontPorch = spec.v_front_porch;
    t.VSyncWidth  = spec.v_sync_width;
    t.VSyncPol    = live_timing.VSyncPol;
    t.interlaced  = 0;
    t.etc.rr      = static_cast<NvU16>(spec.refresh_hz + 0.5f);
    t.etc.rrx1k   = static_cast<NvU32>(spec.refresh_hz * 1000.0f + 0.5f);

    NV_CUSTOM_DISPLAY cd = {};
    cd.version      = NV_CUSTOM_DISPLAY_VER;
    cd.timing       = t;
    cd.srcPartition = { 0.0f, 0.0f, 1.0f, 1.0f };
    cd.width        = spec.active_w;
    cd.height       = spec.active_h;
    cd.colorFormat  = NV_FORMAT_A8R8G8B8;
    cd.xRatio       = 1.0f;
    cd.yRatio       = 1.0f;
    cd.depth        = 32;

    LOG() << "Flat3DDisplayTiming::TryNvidia: applying "
          << spec.active_w << "x" << spec.active_h << "@" << spec.refresh_hz << "Hz"
          << " pclk=" << t.pclk << " HTotal=" << t.HTotal << " VTotal=" << t.VTotal
          << " displayId=" << displayId;

    auto posSnapshot = SnapshotDisplayPositions();
    WaitForModesetSettle(100);

    NvU32             ids[1] = { displayId };
    NV_CUSTOM_DISPLAY cds[1] = { cd };
    status = NvAPI_DISP_TryCustomDisplay(ids, 1, cds);
    if (status != NVAPI_OK) {
        LOG() << "Flat3DDisplayTiming::TryNvidia: TryCustomDisplay failed status=" << status
              << " — reverting trial";
        NvAPI_DISP_RevertCustomDisplayTrial(ids, 1);
        WaitForModesetSettle(500);
        RestoreDisplayPositions(posSnapshot);
        return false;
    }

    WaitForModesetSettle(500);
    RestoreDisplayPositions(posSnapshot);

    auto* state = new NvidiaState();
    state->display_ids.push_back(displayId);
    state->device_name   = w_name;
    state->original_mode = original_mode;
    backend_state_ = state;

    LOG() << "Flat3DDisplayTiming::TryNvidia: succeeded displayId=" << displayId;
    return true;
}


void Flat3DDisplayTiming::RevertNvidia()
{
    if (!backend_state_) return;
    auto* state = static_cast<NvidiaState*>(backend_state_);
    if (state->display_ids.empty()) return;

    LOG() << "Flat3DDisplayTiming::RevertNvidia: reverting " << state->display_ids.size() << " display(s)";

    auto posSnapshot = SnapshotDisplayPositions();
    WaitForModesetSettle(100);

    NvAPI_DISP_RevertCustomDisplayTrial(state->display_ids.data(),
                                        static_cast<NvU32>(state->display_ids.size()));

    WaitForModesetSettle(500);

    if (!state->device_name.empty()) {
        RestoreSavedMode(state->device_name, state->original_mode);
        WaitForModesetSettle(300);
    }

    RestoreDisplayPositions(posSnapshot);
}


// ===========================================================================
// AMD backend
// ===========================================================================

namespace {

using ADL_MAIN_CONTROL_CREATE_t          = int (*)(ADL_MAIN_MALLOC_CALLBACK, int);
using ADL_MAIN_CONTROL_DESTROY_t         = int (*)();
using ADL_ADAPTER_NUMBEROFADAPTERS_GET_t = int (*)(int*);
using ADL_ADAPTER_ADAPTERINFO_GET_t      = int (*)(LPAdapterInfo, int);
using ADL_DISPLAY_DISPLAYINFO_GET_t      = int (*)(int, int*, ADLDisplayInfo**, int);
using ADL_DISPLAY_MODETIMINGOVERRIDE_SET_t =
    int (*)(int, int, ADLDisplayModeInfo*, int);
using ADL_DISPLAY_MODETIMINGOVERRIDE_DELETE_t =
    int (*)(int, int, ADLDisplayMode*, int);

void* __stdcall AdlMallocCallback(int sz) { return malloc(static_cast<size_t>(sz)); }

HMODULE LoadAdlLibrary()
{
    HMODULE hmod = LoadLibraryW(L"atiadlxx.dll");
    if (!hmod) hmod = LoadLibraryW(L"atiadlxy.dll");
    return hmod;
}

class AdlSession {
public:
    AdlSession() = default;
    ~AdlSession() {
        if (destroy_) destroy_();
        if (dll_)     FreeLibrary(dll_);
    }

    bool Init() {
        dll_ = LoadAdlLibrary();
        if (!dll_) return false;

        auto pfnCreate = reinterpret_cast<ADL_MAIN_CONTROL_CREATE_t>(
            GetProcAddress(dll_, "ADL_Main_Control_Create"));
        destroy_ = reinterpret_cast<ADL_MAIN_CONTROL_DESTROY_t>(
            GetProcAddress(dll_, "ADL_Main_Control_Destroy"));
        if (!pfnCreate || !destroy_) {
            FreeLibrary(dll_); dll_ = nullptr;
            return false;
        }
        if (pfnCreate(AdlMallocCallback, 1) != ADL_OK) {
            destroy_ = nullptr;
            FreeLibrary(dll_); dll_ = nullptr;
            return false;
        }

        num_adapters_get_ = reinterpret_cast<ADL_ADAPTER_NUMBEROFADAPTERS_GET_t>(
            GetProcAddress(dll_, "ADL_Adapter_NumberOfAdapters_Get"));
        adapter_info_get_ = reinterpret_cast<ADL_ADAPTER_ADAPTERINFO_GET_t>(
            GetProcAddress(dll_, "ADL_Adapter_AdapterInfo_Get"));
        display_info_get_ = reinterpret_cast<ADL_DISPLAY_DISPLAYINFO_GET_t>(
            GetProcAddress(dll_, "ADL_Display_DisplayInfo_Get"));
        timing_set_       = reinterpret_cast<ADL_DISPLAY_MODETIMINGOVERRIDE_SET_t>(
            GetProcAddress(dll_, "ADL_Display_ModeTimingOverride_Set"));
        timing_delete_    = reinterpret_cast<ADL_DISPLAY_MODETIMINGOVERRIDE_DELETE_t>(
            GetProcAddress(dll_, "ADL_Display_ModeTimingOverride_Delete"));

        return num_adapters_get_ && adapter_info_get_ && display_info_get_ && timing_set_;
    }

    int NumAdapters(int* out) const { return num_adapters_get_(out); }
    int AdapterInfoGet(LPAdapterInfo info, int size) const { return adapter_info_get_(info, size); }
    int DisplayInfoGet(int adapter, int* num, ADLDisplayInfo** info, int force) const {
        return display_info_get_(adapter, num, info, force);
    }
    int TimingSet(int adapter, int display, ADLDisplayModeInfo* mode, int force) const {
        return timing_set_(adapter, display, mode, force);
    }
    int TimingDelete(int adapter, int display, ADLDisplayMode* mode, int force) const {
        return timing_delete_ ? timing_delete_(adapter, display, mode, force) : ADL_ERR;
    }
    bool HasDelete() const { return timing_delete_ != nullptr; }

private:
    HMODULE dll_ = nullptr;
    ADL_MAIN_CONTROL_DESTROY_t          destroy_           = nullptr;
    ADL_ADAPTER_NUMBEROFADAPTERS_GET_t  num_adapters_get_  = nullptr;
    ADL_ADAPTER_ADAPTERINFO_GET_t       adapter_info_get_  = nullptr;
    ADL_DISPLAY_DISPLAYINFO_GET_t       display_info_get_  = nullptr;
    ADL_DISPLAY_MODETIMINGOVERRIDE_SET_t    timing_set_    = nullptr;
    ADL_DISPLAY_MODETIMINGOVERRIDE_DELETE_t timing_delete_ = nullptr;
};

bool FindAdlAdapterDisplay(const AdlSession& adl, int32_t target_display_index,
                           int& out_adapter, int& out_display)
{
    int num_adapters = 0;
    if (adl.NumAdapters(&num_adapters) != ADL_OK || num_adapters <= 0) {
        LOG() << "FindAdlAdapterDisplay: no ADL adapters";
        return false;
    }

    std::vector<AdapterInfo> adapters(static_cast<size_t>(num_adapters));
    std::memset(adapters.data(), 0, adapters.size() * sizeof(AdapterInfo));
    if (adl.AdapterInfoGet(adapters.data(),
                           static_cast<int>(adapters.size() * sizeof(AdapterInfo))) != ADL_OK) {
        LOG() << "FindAdlAdapterDisplay: AdapterInfo_Get failed";
        return false;
    }

    const int target_os_index = target_display_index - 1;

    int adapter_index = -1;
    for (const auto& a : adapters) {
        if (!a.iPresent) continue;
        if (a.iOSDisplayIndex == target_os_index) { adapter_index = a.iAdapterIndex; break; }
    }
    if (adapter_index < 0) {
        for (const auto& a : adapters) {
            if (a.iPresent) { adapter_index = a.iAdapterIndex; break; }
        }
    }
    if (adapter_index < 0) {
        LOG() << "FindAdlAdapterDisplay: no present adapter (likely no AMD GPU)";
        return false;
    }

    int             num_displays = 0;
    ADLDisplayInfo* display_info = nullptr;
    if (adl.DisplayInfoGet(adapter_index, &num_displays, &display_info, 0) != ADL_OK
        || num_displays <= 0 || !display_info) {
        LOG() << "FindAdlAdapterDisplay: DisplayInfo_Get failed for adapter " << adapter_index;
        return false;
    }

    int display_index = -1;
    constexpr int kMappedMask =
        ADL_DISPLAY_DISPLAYINFO_DISPLAYCONNECTED | ADL_DISPLAY_DISPLAYINFO_DISPLAYMAPPED;
    for (int i = 0; i < num_displays; ++i) {
        if ((display_info[i].iDisplayInfoValue & kMappedMask) == kMappedMask) {
            display_index = display_info[i].displayID.iDisplayLogicalIndex;
            break;
        }
    }
    free(display_info);

    if (display_index < 0) {
        LOG() << "FindAdlAdapterDisplay: no mapped display on adapter " << adapter_index;
        return false;
    }

    out_adapter = adapter_index;
    out_display = display_index;
    return true;
}

void FillAdlModeInfo(const FramePackTimingSpec& spec, ADLDisplayModeInfo& out)
{
    out = {};
    out.iTimingStandard   = ADL_DL_MODETIMING_STANDARD_CUSTOM;
    out.iPossibleStandard = 0;
    out.iRefreshRate      = static_cast<int>(spec.refresh_hz + 0.5f);
    out.iPelsWidth        = spec.active_w;
    out.iPelsHeight       = spec.active_h;

    ADLDetailedTiming& dt = out.sDetailedTiming;
    dt.iSize        = sizeof(dt);
    dt.sTimingFlags = 0;
    dt.sHTotal      = static_cast<short>(spec.h_total);
    dt.sHDisplay    = static_cast<short>(spec.active_w);
    dt.sHSyncStart  = static_cast<short>(spec.h_front_porch);
    dt.sHSyncWidth  = static_cast<short>(spec.h_sync_width);
    dt.sVTotal      = static_cast<short>(spec.v_total);
    dt.sVDisplay    = static_cast<short>(spec.active_h);
    dt.sVSyncStart  = static_cast<short>(spec.v_front_porch);
    dt.sVSyncWidth  = static_cast<short>(spec.v_sync_width);
    dt.sPixelClock  = static_cast<short>(ComputePixelClock10kHz(spec));
}

}  // namespace


bool Flat3DDisplayTiming::TryAmd(const FramePackTimingSpec& spec, int32_t display_index)
{
    AdlSession adl;
    if (!adl.Init()) {
        LOG() << "Flat3DDisplayTiming::TryAmd: ADL not available (not AMD GPU?)";
        return false;
    }

    int adapter = -1, ddisplay = -1;
    if (!FindAdlAdapterDisplay(adl, display_index, adapter, ddisplay)) {
        return false;
    }

    std::wstring w_name = ResolveDeviceName(display_index);
    DEVMODEW original_mode{};
    SnapshotCurrentMode(w_name, original_mode);

    ADLDisplayModeInfo mode_info{};
    FillAdlModeInfo(spec, mode_info);

    LOG() << "Flat3DDisplayTiming::TryAmd: applying timing override "
          << spec.active_w << "x" << spec.active_h << "@" << spec.refresh_hz << "Hz"
          << " adapter=" << adapter << " display=" << ddisplay;

    auto posSnapshot = SnapshotDisplayPositions();
    WaitForModesetSettle(100);

    int rc = adl.TimingSet(adapter, ddisplay, &mode_info, 1);
    if (rc != ADL_OK && rc != ADL_OK_WARNING && rc != ADL_OK_MODE_CHANGE) {
        LOG() << "Flat3DDisplayTiming::TryAmd: ModeTimingOverride_Set failed rc=" << rc;
        return false;
    }

    if (!w_name.empty()) {
        DEVMODEW dm{};
        dm.dmSize             = sizeof(dm);
        dm.dmPelsWidth        = spec.active_w;
        dm.dmPelsHeight       = spec.active_h;
        dm.dmDisplayFrequency = static_cast<DWORD>(spec.refresh_hz + 0.5f);
        dm.dmBitsPerPel       = 32;
        dm.dmFields           = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
        ChangeDisplaySettingsExW(w_name.c_str(), &dm, nullptr, CDS_FULLSCREEN, nullptr);
    }

    WaitForModesetSettle(500);
    RestoreDisplayPositions(posSnapshot);

    auto* state = new AmdState();
    state->adapter_index = adapter;
    state->display_index = ddisplay;
    state->device_name   = w_name;
    state->original_mode = original_mode;
    backend_state_ = state;

    LOG() << "Flat3DDisplayTiming::TryAmd: succeeded (adapter=" << adapter
          << " display=" << ddisplay << ")";
    return true;
}


void Flat3DDisplayTiming::RevertAmd()
{
    if (!backend_state_) return;
    auto* state = static_cast<AmdState*>(backend_state_);

    LOG() << "Flat3DDisplayTiming::RevertAmd: reverting (adapter="
          << state->adapter_index << " display=" << state->display_index << ")";

    auto posSnapshot = SnapshotDisplayPositions();

    if (!state->device_name.empty()) {
        RestoreSavedMode(state->device_name, state->original_mode);
        WaitForModesetSettle(300);
    }

    AdlSession adl;
    if (adl.Init()) {
        if (adl.HasDelete()) {
            ADLDisplayMode mode{};
            mode.iPelsWidth        = state->original_mode.dmPelsWidth;
            mode.iPelsHeight       = state->original_mode.dmPelsHeight;
            mode.iDisplayFrequency = state->original_mode.dmDisplayFrequency;
            mode.iBitsPerPel       = 32;
            adl.TimingDelete(state->adapter_index, state->display_index, &mode, 1);
        }
        ADLDisplayModeInfo info{};
        info.iTimingStandard = ADL_DL_MODETIMING_STANDARD_DRIVER_DEFAULT;
        info.iPelsWidth      = state->original_mode.dmPelsWidth;
        info.iPelsHeight     = state->original_mode.dmPelsHeight;
        info.iRefreshRate    = state->original_mode.dmDisplayFrequency;
        info.sDetailedTiming.iSize = sizeof(info.sDetailedTiming);
        adl.TimingSet(state->adapter_index, state->display_index, &info, 1);
    }

    WaitForModesetSettle(300);
    RestoreDisplayPositions(posSnapshot);
}


// ===========================================================================
// CRU fallback — ChangeDisplaySettingsExW
// ===========================================================================

bool Flat3DDisplayTiming::TryCruFallback(const FramePackTimingSpec& spec, int32_t display_index)
{
    std::wstring device_name = ResolveDeviceName(display_index);
    if (device_name.empty()) {
        LOG() << "Flat3DDisplayTiming::TryCruFallback: could not resolve device name";
        return false;
    }

    DEVMODEW original{};
    original.dmSize = sizeof(original);
    if (!EnumDisplaySettingsExW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &original, 0)) {
        LOG() << "Flat3DDisplayTiming::TryCruFallback: EnumDisplaySettingsExW(current) failed";
        return false;
    }

    bool mode_found = false;
    DEVMODEW candidate{};
    candidate.dmSize = sizeof(candidate);
    for (DWORD i = 0; EnumDisplaySettingsExW(device_name.c_str(), i, &candidate, 0); ++i) {
        if (candidate.dmPelsWidth  == spec.active_w &&
            candidate.dmPelsHeight == spec.active_h &&
            candidate.dmDisplayFrequency == static_cast<DWORD>(spec.refresh_hz + 0.5f)) {
            mode_found = true;
            break;
        }
    }

    if (!mode_found) {
        LOG() << "Flat3DDisplayTiming::TryCruFallback: mode "
              << spec.active_w << "x" << spec.active_h << "@" << spec.refresh_hz
              << "Hz not in mode list. Use CRU to add it first.";
        return false;
    }

    auto posSnapshot = SnapshotDisplayPositions();

    DEVMODEW dm{};
    dm.dmSize             = sizeof(dm);
    dm.dmPelsWidth        = spec.active_w;
    dm.dmPelsHeight       = spec.active_h;
    dm.dmDisplayFrequency = static_cast<DWORD>(spec.refresh_hz + 0.5f);
    dm.dmBitsPerPel       = 32;
    dm.dmFields           = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;

    LONG result = ChangeDisplaySettingsExW(device_name.c_str(), &dm, nullptr, CDS_FULLSCREEN, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        LOG() << "Flat3DDisplayTiming::TryCruFallback: ChangeDisplaySettingsExW failed result=" << result;
        return false;
    }

    WaitForModesetSettle(500);
    RestoreDisplayPositions(posSnapshot);

    auto* state = new CruState();
    state->device_name   = device_name;
    state->original_mode = original;
    backend_state_ = state;

    LOG() << "Flat3DDisplayTiming::TryCruFallback: switched to "
          << spec.active_w << "x" << spec.active_h << "@" << spec.refresh_hz << "Hz";
    return true;
}


void Flat3DDisplayTiming::RevertCruFallback()
{
    if (!backend_state_) return;
    auto* state = static_cast<CruState*>(backend_state_);

    LOG() << "Flat3DDisplayTiming::RevertCruFallback: reverting to original mode";

    auto posSnapshot = SnapshotDisplayPositions();
    RestoreSavedMode(state->device_name, state->original_mode);
    WaitForModesetSettle(500);
    RestoreDisplayPositions(posSnapshot);
}

}  // namespace vrmod::flat3d
