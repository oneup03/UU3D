#include <array>
#include <thread>
#include <future>
#include <optional>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>
#include <wrl/client.h>
#include <utility/Thread.hpp>
#include <utility/Module.hpp>
#include <utility/RTTI.hpp>
#include <utility/String.hpp>

#include <d3d12sdklayers.h>
#include <wrl.h>

#include "WindowFilter.hpp"
#include "Framework.hpp"
#include "render/D3D12Diagnostics.hpp"
#include "render/ShaderOverrideRegistry.hpp"

#include "render/RenderDocCaptureService.hpp"

#include "D3D12Hook.hpp"

static D3D12Hook* g_d3d12_hook = nullptr;

// True while a depth-stencil observer's on_resource_barriers is running, so the
// self-contained depth copy's own ResourceBarrier calls aren't re-observed.
thread_local bool g_inside_depth_stencil_observer = false;

namespace {
constexpr size_t CREATE_GRAPHICS_PIPELINE_STATE_VTABLE_INDEX = 10;
constexpr size_t CREATE_PIPELINE_STATE_VTABLE_INDEX = 47;
constexpr size_t CREATE_DESCRIPTOR_HEAP_VTABLE_INDEX = 14;
constexpr size_t CREATE_SHADER_RESOURCE_VIEW_VTABLE_INDEX = 18;
constexpr size_t CREATE_RENDER_TARGET_VIEW_VTABLE_INDEX = 20;
constexpr size_t CREATE_DEPTH_STENCIL_VIEW_VTABLE_INDEX = 21;
constexpr size_t COPY_DESCRIPTORS_VTABLE_INDEX = 23;
constexpr size_t COPY_DESCRIPTORS_SIMPLE_VTABLE_INDEX = 24;
constexpr size_t DRAW_INSTANCED_VTABLE_INDEX = 12;
constexpr size_t DRAW_INDEXED_INSTANCED_VTABLE_INDEX = 13;
constexpr size_t DISPATCH_VTABLE_INDEX = 14;
constexpr size_t RS_SET_VIEWPORTS_VTABLE_INDEX = 21;
constexpr size_t SET_PIPELINE_STATE_VTABLE_INDEX = 25;
constexpr size_t RESOURCE_BARRIER_VTABLE_INDEX = 26;
constexpr size_t CLEAR_DEPTH_STENCIL_VIEW_CMDLIST_VTABLE_INDEX = 47;
constexpr size_t SET_DESCRIPTOR_HEAPS_VTABLE_INDEX = 28;
constexpr size_t SET_COMPUTE_ROOT_DESCRIPTOR_TABLE_VTABLE_INDEX = 31;
constexpr size_t SET_GRAPHICS_ROOT_DESCRIPTOR_TABLE_VTABLE_INDEX = 32;
constexpr size_t OM_SET_RENDER_TARGETS_VTABLE_INDEX = 46;

bool should_preserve_present_params_for_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"MafiaTheOldCountry") != std::wstring::npos;
    }();

    return result;
}

bool is_dune_awakening_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"DuneSandbox-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

std::optional<std::wstring> get_d3d12_debug_name(ID3D12Object* object) {
    if (object == nullptr) {
        return std::nullopt;
    }

    UINT byte_count = 0;
    if (FAILED(object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &byte_count, nullptr)) ||
        byte_count <= sizeof(wchar_t)) {
        return std::nullopt;
    }

    std::wstring name((byte_count + sizeof(wchar_t) - 1) / sizeof(wchar_t), L'\0');
    if (FAILED(object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &byte_count, name.data()))) {
        return std::nullopt;
    }

    name.resize(byte_count / sizeof(wchar_t));
    while (!name.empty() && name.back() == L'\0') {
        name.pop_back();
    }

    return name.empty() ? std::nullopt : std::optional<std::wstring>{std::move(name)};
}

void log_dune_present_path_once(IDXGISwapChain3* swapchain, ID3D12CommandQueue* command_queue) {
    static bool logged = false;

    if (logged || !is_dune_awakening_current_game() || swapchain == nullptr) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer{};
    const auto index = swapchain->GetCurrentBackBufferIndex();
    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&buffer))) || buffer == nullptr) {
        return;
    }

    const auto buffer_name = get_d3d12_debug_name(buffer.Get());
    const auto queue_name = get_d3d12_debug_name(command_queue);
    const auto desc = buffer->GetDesc();

    spdlog::info(
        "[Dune][FSR] Real Present path swapchain={:x} index={} buffer={:x} name='{}' [{}x{} fmt={}] queue={:x} name='{}'. "
        "Dune uses one DXGI swapchain with an FFX custom-present pipeline; no nested swapchain will be selected.",
        reinterpret_cast<uintptr_t>(swapchain),
        index,
        reinterpret_cast<uintptr_t>(buffer.Get()),
        buffer_name ? utility::narrow(*buffer_name) : "<unnamed>",
        desc.Width,
        desc.Height,
        static_cast<uint32_t>(desc.Format),
        reinterpret_cast<uintptr_t>(command_queue),
        queue_name ? utility::narrow(*queue_name) : "<unnamed>");

    logged = true;
}

template <typename TInterface>
void add_unique_pointer_hook(
    TInterface* iface,
    size_t vtable_index,
    void* detour,
    std::vector<std::unique_ptr<PointerHook>>& storage,
    std::unordered_map<uintptr_t, PointerHook*>& lookup,
    std::unordered_set<uintptr_t>& seen_slots
) {
    if (iface == nullptr) {
        return;
    }

    auto** slot = &(*(void***)iface)[vtable_index];
    const auto slot_key = reinterpret_cast<uintptr_t>(slot);

    if (!seen_slots.emplace(slot_key).second) {
        return;
    }

    auto hook = std::make_unique<PointerHook>(slot, detour);
    lookup.emplace(slot_key, hook.get());
    storage.emplace_back(std::move(hook));
}
}

namespace {
const char* d3d12_severity_name(D3D12_MESSAGE_SEVERITY sev) {
    switch (sev) {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "CORRUPTION";
    case D3D12_MESSAGE_SEVERITY_ERROR:      return "ERROR";
    case D3D12_MESSAGE_SEVERITY_WARNING:    return "WARNING";
    case D3D12_MESSAGE_SEVERITY_INFO:       return "INFO";
    default:                                return "MESSAGE";
    }
}

void log_d3d12_message(D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, const char* description) {
    const char* sev = d3d12_severity_name(severity);
    const char* desc = description != nullptr ? description : "";

    if (severity == D3D12_MESSAGE_SEVERITY_CORRUPTION || severity == D3D12_MESSAGE_SEVERITY_ERROR) {
        spdlog::error("[D3D12 Debug] {} id={} : {}", sev, (uint32_t)id, desc);
        // Flush now: the very next thing may be the corruption/crash this message
        // is warning about, so the log must already be on disk.
        if (auto logger = spdlog::default_logger(); logger != nullptr) {
            logger->flush();
        }
    } else {
        spdlog::warn("[D3D12 Debug] {} id={} : {}", sev, (uint32_t)id, desc);
    }
}

// Synchronous ID3D12InfoQueue1 callback: fires at the D3D12 call site (e.g. the
// exact ResourceBarrier with a mismatched StateBefore) BEFORE control returns to
// the caller, so we capture the offending message even when the frame goes on to
// corrupt state and crash.
void CALLBACK d3d12_message_callback(D3D12_MESSAGE_CATEGORY /*category*/, D3D12_MESSAGE_SEVERITY severity,
                                     D3D12_MESSAGE_ID id, LPCSTR description, void* /*context*/) {
    log_d3d12_message(severity, id, description);
}
} // namespace

D3D12Hook::~D3D12Hook() {
    unhook();
}

void D3D12Hook::setup_debug_info_queue() {
    if (m_device == nullptr) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> info_queue{};
    if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&info_queue))) || info_queue == nullptr) {
        spdlog::warn("[D3D12 Debug] Requested, but the game's D3D12 device has NO debug layer, so validation "
                     "messages can't be captured. To enable it: install the Windows 'Graphics Tools' optional "
                     "feature, run dxcpl.exe, Add the game's .exe, set 'Direct3D / Debug Layer' to 'Force On' "
                     "(Scope: this application only), Apply, then relaunch with this option enabled. "
                     "(Or set the environment variable UEVR_D3D12_DEBUG=1 if UEVR injects at game startup.)");
        return;
    }

    // Never break into a debugger: one may not be attached, and an unhandled
    // breakpoint would crash the game. We only want the messages recorded.
    info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
    info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
    info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);

    // Drop the INFO/MESSAGE firehose; keep WARNING and above.
    D3D12_MESSAGE_SEVERITY hide[] = { D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE };
    D3D12_INFO_QUEUE_FILTER filter{};
    filter.DenyList.NumSeverities = (UINT)_countof(hide);
    filter.DenyList.pSeverityList = hide;
    info_queue->PushStorageFilter(&filter);

    // Prefer the synchronous callback (ID3D12InfoQueue1, Win10 2004+): it fires at
    // the call site, so a message from the frame that crashes is still captured —
    // a Present-time poll would miss it.
    Microsoft::WRL::ComPtr<ID3D12InfoQueue1> info_queue1{};
    if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&info_queue1))) && info_queue1 != nullptr) {
        DWORD cookie = 0;
        if (SUCCEEDED(info_queue1->RegisterMessageCallback(&d3d12_message_callback,
                                                           D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie))) {
            spdlog::info("[D3D12 Debug] Debug layer attached (synchronous callback). D3D12 validation messages "
                         "(incl. resource-barrier state mismatches) will be logged as they occur.");
            return;
        }
    }

    // Fallback: poll stored messages at each Present (may miss the crashing frame).
    m_debug_poll = true;
    spdlog::info("[D3D12 Debug] Debug layer attached (polled at Present — may miss a message from the exact "
                 "frame that crashes).");
}

void D3D12Hook::drain_debug_messages() {
    if (m_device == nullptr) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> info_queue{};
    if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&info_queue))) || info_queue == nullptr) {
        return;
    }

    const UINT64 count = info_queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T len = 0;
        if (FAILED(info_queue->GetMessage(i, nullptr, &len)) || len == 0) {
            continue;
        }

        std::vector<uint8_t> bytes(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        if (SUCCEEDED(info_queue->GetMessage(i, msg, &len))) {
            log_d3d12_message(msg->Severity, msg->ID, msg->pDescription);
        }
    }

    info_queue->ClearStoredMessages();
}

bool D3D12Hook::hook() {
    spdlog::info("Hooking D3D12");

    g_d3d12_hook = this;

    // Diagnostic: enable the D3D12 debug layer as early as we can. This only
    // takes effect for devices created AFTER this call, so it helps only when
    // UEVR is injected before the game creates its device — hence the env-var
    // opt-in (the cvar isn't loaded this early). When the game's device already
    // exists, use dxcpl.exe "Force On" instead; setup_debug_info_queue() detects
    // that and logs instructions. Harmless if Graphics Tools isn't installed.
    if (GetEnvironmentVariableW(L"UEVR_D3D12_DEBUG", nullptr, 0) != 0) {
        Microsoft::WRL::ComPtr<ID3D12Debug> debug_iface{};
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_iface))) && debug_iface != nullptr) {
            debug_iface->EnableDebugLayer();
            spdlog::info("[D3D12 Debug] EnableDebugLayer() called early (UEVR_D3D12_DEBUG set)");
        } else {
            spdlog::warn("[D3D12 Debug] UEVR_D3D12_DEBUG set but D3D12GetDebugInterface failed "
                         "(install the Windows 'Graphics Tools' optional feature)");
        }
    }

    IDXGISwapChain1* swap_chain1{ nullptr };
    IDXGISwapChain3* swap_chain{ nullptr };
    ID3D12Device* device{ nullptr };
    ID3D12CommandAllocator* command_allocator{ nullptr };
    ID3D12GraphicsCommandList* command_list{ nullptr };

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc1;

    ZeroMemory(&swap_chain_desc1, sizeof(swap_chain_desc1));

    swap_chain_desc1.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap_chain_desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swap_chain_desc1.BufferCount = 2;
    swap_chain_desc1.SampleDesc.Count = 1;
    swap_chain_desc1.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    swap_chain_desc1.Width = 1;
    swap_chain_desc1.Height = 1;

    // Manually get D3D12CreateDevice export because the user may be running Windows 7
    const auto d3d12_module = LoadLibraryA("d3d12.dll");
    if (d3d12_module == nullptr) {
        spdlog::error("Failed to load d3d12.dll");
        return false;
    }

    auto d3d12_create_device = (decltype(D3D12CreateDevice)*)GetProcAddress(d3d12_module, "D3D12CreateDevice");
    if (d3d12_create_device == nullptr) {
        spdlog::error("Failed to get D3D12CreateDevice export");
        return false;
    }

    spdlog::info("Creating dummy device");

    // Get the original on-disk bytes of the D3D12CreateDevice export
    const auto original_bytes = utility::get_original_bytes(d3d12_create_device);

    // Temporarily unhook D3D12CreateDevice
    // it allows compatibility with ReShade and other overlays that hook it
    // this is just a dummy device anyways, we don't want the other overlays to be able to use it
    if (original_bytes) {
        spdlog::info("D3D12CreateDevice appears to be hooked, temporarily unhooking");

        std::vector<uint8_t> hooked_bytes(original_bytes->size());
        memcpy(hooked_bytes.data(), d3d12_create_device, original_bytes->size());

        ProtectionOverride protection_override{ d3d12_create_device, original_bytes->size(), PAGE_EXECUTE_READWRITE };
        memcpy(d3d12_create_device, original_bytes->data(), original_bytes->size());
        
        if (FAILED(d3d12_create_device(nullptr, feature_level, IID_PPV_ARGS(&device)))) {
            spdlog::error("Failed to create D3D12 Dummy device");
            memcpy(d3d12_create_device, hooked_bytes.data(), hooked_bytes.size());
            return false;
        }

        spdlog::info("Restoring hooked bytes for D3D12CreateDevice");
        memcpy(d3d12_create_device, hooked_bytes.data(), hooked_bytes.size());
    } else { // D3D12CreateDevice is not hooked
        if (FAILED(d3d12_create_device(nullptr, feature_level, IID_PPV_ARGS(&device)))) {
            spdlog::error("Failed to create D3D12 Dummy device");
            return false;
        }
    }

    spdlog::info("Dummy device: {:x}", (uintptr_t)device);

    // Manually get CreateDXGIFactory export because the user may be running Windows 7
    const auto dxgi_module = LoadLibraryA("dxgi.dll");
    if (dxgi_module == nullptr) {
        spdlog::error("Failed to load dxgi.dll");
        return false;
    }

    auto create_dxgi_factory = (decltype(CreateDXGIFactory)*)GetProcAddress(dxgi_module, "CreateDXGIFactory");

    if (create_dxgi_factory == nullptr) {
        spdlog::error("Failed to get CreateDXGIFactory export");
        return false;
    }

    spdlog::info("Creating dummy DXGI factory");

    IDXGIFactory4* factory{ nullptr };
    if (FAILED(create_dxgi_factory(IID_PPV_ARGS(&factory)))) {
        spdlog::error("Failed to create D3D12 Dummy DXGI Factory");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = 0;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_desc.NodeMask = 0;

    spdlog::info("Creating dummy command queue");

    ID3D12CommandQueue* command_queue{ nullptr };
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue)))) {
        spdlog::error("Failed to create D3D12 Dummy Command Queue");
        return false;
    }

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocator)))) {
        spdlog::error("Failed to create D3D12 Dummy Command Allocator");
        return false;
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator, nullptr, IID_PPV_ARGS(&command_list)))) {
        spdlog::error("Failed to create D3D12 Dummy Graphics Command List");
        return false;
    }

    spdlog::info("Creating dummy swapchain");

    // used in CreateSwapChainForHwnd fallback
    HWND hwnd = 0;
    WNDCLASSEX wc{};

    auto init_dummy_window = [&]() {
        // fallback to CreateSwapChainForHwnd
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DefWindowProc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 0;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hIcon = NULL;
        wc.hCursor = NULL;
        wc.hbrBackground = NULL;
        wc.lpszMenuName = NULL;
        wc.lpszClassName = TEXT("REFRAMEWORK_DX12_DUMMY");
        wc.hIconSm = NULL;

        ::RegisterClassEx(&wc);

        hwnd = ::CreateWindow(wc.lpszClassName, TEXT("REF DX Dummy Window"), WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

        swap_chain_desc1.BufferCount = 3;
        swap_chain_desc1.Width = 0;
        swap_chain_desc1.Height = 0;
        swap_chain_desc1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap_chain_desc1.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        swap_chain_desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_chain_desc1.SampleDesc.Count = 1;
        swap_chain_desc1.SampleDesc.Quality = 0;
        swap_chain_desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swap_chain_desc1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        swap_chain_desc1.Scaling = DXGI_SCALING_STRETCH;
        swap_chain_desc1.Stereo = FALSE;
    };

    std::vector<std::function<bool ()>> swapchain_attempts{
        // we call CreateSwapChainForComposition instead of CreateSwapChainForHwnd
        // because some overlays will have hooks on CreateSwapChainForHwnd
        // and all we're doing is creating a dummy swapchain
        // we don't want to screw up the overlay
        [&]() {
            return !FAILED(factory->CreateSwapChainForComposition(command_queue, &swap_chain_desc1, nullptr, &swap_chain1));
        },
        [&]() {
            init_dummy_window();

            return !FAILED(factory->CreateSwapChainForHwnd(command_queue, hwnd, &swap_chain_desc1, nullptr, nullptr, &swap_chain1));
        },
        [&]() {
            return !FAILED(factory->CreateSwapChainForHwnd(command_queue, GetDesktopWindow(), &swap_chain_desc1, nullptr, nullptr, &swap_chain1));
        },
    };

    bool any_succeed = false;

    for (auto i = 0; i < swapchain_attempts.size(); i++) {
        auto& attempt = swapchain_attempts[i];
        
        try {
            spdlog::info("Trying swapchain attempt {}", i);

            if (attempt()) {
                spdlog::info("Created dummy swapchain on attempt {}", i);
                any_succeed = true;
                break;
            }
        } catch (std::exception& e) {
            spdlog::error("Failed to create dummy swapchain on attempt {}: {}", i, e.what());
        } catch(...) {
            spdlog::error("Failed to create dummy swapchain on attempt {}: unknown exception", i);
        }

        spdlog::error("Attempt {} failed", i);
    }

    if (!any_succeed) {
        spdlog::error("Failed to create D3D12 Dummy Swap Chain");

        if (hwnd) {
            ::DestroyWindow(hwnd);
        }

        if (wc.lpszClassName != nullptr) {
            ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        }

        return false;
    }

    spdlog::info("Querying dummy swapchain");

    if (FAILED(swap_chain1->QueryInterface(IID_PPV_ARGS(&swap_chain)))) {
        spdlog::error("Failed to retrieve D3D12 DXGI SwapChain");
        return false;
    }

    if (is_dune_awakening_current_game()) {
        // This object is UEVR's startup dummy, not Dune's later FFX custom
        // present. DXGI COM implementations are not required to expose MSVC
        // RTTI, so probing vtable[-1] cannot identify Dune's FSR path.
        m_skip_dummy_swapchain_type_info_probe = true;
        spdlog::info(
            "[Dune][FSR] Skipping MSVC RTTI on UEVR's dummy DXGI swapchain; "
            "the real single-swapchain FFX custom-present path will be identified at Present");
    }

    if (!m_skip_dummy_swapchain_type_info_probe) {
        try {
            const auto ti = utility::rtti::get_type_info(swap_chain1);
            const auto swapchain_classname = ti != nullptr && ti->name() != nullptr ? std::string_view{ti->name()} : "unknown";
            const auto raw_name = ti != nullptr && ti->raw_name() != nullptr ? std::string_view{ti->raw_name()} : "unknown";

            spdlog::info("Swapchain type info: {}", swapchain_classname);
            spdlog::info("Swapchain raw type info: {}", raw_name);
            
            if (swapchain_classname.contains("interposer::DXGISwapChain")) { // DLSS3
                spdlog::info("Found Streamline (DLSSFG) swapchain during dummy initialization: {:x}", (uintptr_t)swap_chain1);
                m_using_frame_generation_swapchain = true;
            }
            // Need to test this one to see if it actually has the same issues - disabling it for now
            /*else if (swapchain_classname.contains("FrameInterpolationSwapChain")) { // FSR3
                spdlog::info("Found FSR3 swapchain during dummy initialization: {:x}", (uintptr_t)swap_chain1);
                m_using_frame_generation_swapchain = true;
            }*/
        } catch (const std::exception& e) {
            spdlog::error("Failed to get type info: {}. Disabling dummy swapchain RTTI probe for this session.", e.what());
            m_skip_dummy_swapchain_type_info_probe = true;
        } catch (...) {
            spdlog::error("Failed to get type info: unknown exception. Disabling dummy swapchain RTTI probe for this session.");
            m_skip_dummy_swapchain_type_info_probe = true;
        }
    }

    spdlog::info("Finding command queue offset");

    m_command_queue_offset = 0;

    // Find the command queue offset in the swapchain
    for (auto i = 0; i < 512 * sizeof(void*); i += sizeof(void*)) {
        const auto base = (uintptr_t)swap_chain1 + i;

        // reached the end
        if (IsBadReadPtr((void*)base, sizeof(void*))) {
            break;
        }

        auto data = *(ID3D12CommandQueue**)base;

        if (data == command_queue) {
            m_command_queue_offset = i;
            spdlog::info("Found command queue offset: {:x}", i);
            break;
        }
    }

    auto target_swapchain = swap_chain;

    // Scan throughout the swapchain for a valid pointer to scan through
    // this is usually only necessary for Proton
    if (m_command_queue_offset == 0) {
        bool should_break = false;

        for (auto base = 0; base < 512 * sizeof(void*); base += sizeof(void*)) {
            const auto pre_scan_base = (uintptr_t)swap_chain1 + base;

            // reached the end
            if (IsBadReadPtr((void*)pre_scan_base, sizeof(void*))) {
                break;
            }

            const auto scan_base = *(uintptr_t*)pre_scan_base;

            if (scan_base == 0 || IsBadReadPtr((void*)scan_base, sizeof(void*))) {
                continue;
            }

            for (auto i = 0; i < 512 * sizeof(void*); i += sizeof(void*)) {
                const auto pre_data = scan_base + i;

                if (IsBadReadPtr((void*)pre_data, sizeof(void*))) {
                    break;
                }

                auto data = *(ID3D12CommandQueue**)pre_data;

                if (data == command_queue) {
                    // If we hook Streamline's Swapchain, the menu fails to render correctly/flickers
                    // So we switch out the swapchain with the internal one owned by Streamline
                    // Side note: Even though we are scanning for Proton here,
                    // this doubles as an offset scanner for the real swapchain inside Streamline (or FSR3)
                    if (m_using_frame_generation_swapchain) {
                        target_swapchain = (IDXGISwapChain3*)scan_base;
                    }

                    if (!m_using_frame_generation_swapchain) {
                        m_using_proton_swapchain = true;
                    }

                    m_command_queue_offset = i;
                    m_proton_swapchain_offset = base;
                    should_break = true;

                    spdlog::info("Proton potentially detected");
                    spdlog::info("Found command queue offset: {:x}", i);
                    break;
                }
            }

            if (m_using_proton_swapchain || should_break) {
                break;
            }
        }
    }

    if (m_command_queue_offset == 0) {
        spdlog::error("Failed to find command queue offset");
        return false;
    }

    try {
        spdlog::info("Initializing hooks");
        m_present_hook.reset();
        m_present1_hook.reset();
        m_create_graphics_pipeline_state_hooks.clear();
        m_create_pipeline_state_hooks.clear();
        m_create_render_target_view_hooks.clear();
        m_create_depth_stencil_view_hooks.clear();
        m_set_pipeline_state_hooks.clear();
        m_resource_barrier_hooks.clear();
        m_clear_depth_stencil_view_cmd_hooks.clear();
        m_create_graphics_pipeline_state_hook_lookup.clear();
        m_create_pipeline_state_hook_lookup.clear();
        m_create_render_target_view_hook_lookup.clear();
        m_create_depth_stencil_view_hook_lookup.clear();
        m_set_pipeline_state_hook_lookup.clear();
        m_set_pipeline_state_hook_generation.fetch_add(1, std::memory_order_release);
        m_resource_barrier_hook_lookup.clear();
        m_clear_depth_stencil_view_cmd_hook_lookup.clear();
        m_swapchain_hook.reset();

        m_is_phase_1 = true;

        auto& present_fn = (*(void***)target_swapchain)[8]; // Present
        auto& present1_fn = (*(void***)target_swapchain)[22]; // Present1
        m_present_hook = std::make_unique<PointerHook>(&present_fn, (void*)&D3D12Hook::present);
        m_present1_hook = std::make_unique<PointerHook>(&present1_fn, (void*)&D3D12Hook::present1);

        std::unordered_set<uintptr_t> graphics_pipeline_state_slots{};
        std::unordered_set<uintptr_t> pipeline_state_stream_slots{};
        std::unordered_set<uintptr_t> render_target_view_slots{};
        std::unordered_set<uintptr_t> depth_stencil_view_slots{};
        std::unordered_set<uintptr_t> set_pipeline_state_slots{};
        std::unordered_set<uintptr_t> resource_barrier_slots{};
        std::unordered_set<uintptr_t> clear_depth_stencil_view_cmd_slots{};

        add_unique_pointer_hook(
            device,
            CREATE_GRAPHICS_PIPELINE_STATE_VTABLE_INDEX,
            reinterpret_cast<void*>(&D3D12Hook::create_graphics_pipeline_state),
            m_create_graphics_pipeline_state_hooks,
            m_create_graphics_pipeline_state_hook_lookup,
            graphics_pipeline_state_slots
        );

        Microsoft::WRL::ComPtr<ID3D12Device1> device1{};
        Microsoft::WRL::ComPtr<ID3D12Device2> device2{};
        Microsoft::WRL::ComPtr<ID3D12Device3> device3{};
        Microsoft::WRL::ComPtr<ID3D12Device4> device4{};
        Microsoft::WRL::ComPtr<ID3D12Device5> device5{};
        Microsoft::WRL::ComPtr<ID3D12Device6> device6{};
        Microsoft::WRL::ComPtr<ID3D12Device7> device7{};
        Microsoft::WRL::ComPtr<ID3D12Device8> device8{};
        Microsoft::WRL::ComPtr<ID3D12Device9> device9{};
        Microsoft::WRL::ComPtr<ID3D12Device10> device10{};

        device->QueryInterface(IID_PPV_ARGS(&device1));
        device->QueryInterface(IID_PPV_ARGS(&device2));
        device->QueryInterface(IID_PPV_ARGS(&device3));
        device->QueryInterface(IID_PPV_ARGS(&device4));
        device->QueryInterface(IID_PPV_ARGS(&device5));
        device->QueryInterface(IID_PPV_ARGS(&device6));
        device->QueryInterface(IID_PPV_ARGS(&device7));
        device->QueryInterface(IID_PPV_ARGS(&device8));
        device->QueryInterface(IID_PPV_ARGS(&device9));
        device->QueryInterface(IID_PPV_ARGS(&device10));

        const std::array<IUnknown*, 10> device_interfaces{
            device1.Get(),
            device2.Get(),
            device3.Get(),
            device4.Get(),
            device5.Get(),
            device6.Get(),
            device7.Get(),
            device8.Get(),
            device9.Get(),
            device10.Get()
        };

        for (auto* iface : device_interfaces) {
            add_unique_pointer_hook(
                iface,
                CREATE_GRAPHICS_PIPELINE_STATE_VTABLE_INDEX,
                reinterpret_cast<void*>(&D3D12Hook::create_graphics_pipeline_state),
                m_create_graphics_pipeline_state_hooks,
                m_create_graphics_pipeline_state_hook_lookup,
                graphics_pipeline_state_slots
            );

            add_unique_pointer_hook(
                iface,
                CREATE_RENDER_TARGET_VIEW_VTABLE_INDEX,
                reinterpret_cast<void*>(&D3D12Hook::create_render_target_view),
                m_create_render_target_view_hooks,
                m_create_render_target_view_hook_lookup,
                render_target_view_slots
            );

            add_unique_pointer_hook(
                iface,
                CREATE_DEPTH_STENCIL_VIEW_VTABLE_INDEX,
                reinterpret_cast<void*>(&D3D12Hook::create_depth_stencil_view),
                m_create_depth_stencil_view_hooks,
                m_create_depth_stencil_view_hook_lookup,
                depth_stencil_view_slots
            );
        }

        const std::array<IUnknown*, 9> pipeline_stream_interfaces{
            device2.Get(),
            device3.Get(),
            device4.Get(),
            device5.Get(),
            device6.Get(),
            device7.Get(),
            device8.Get(),
            device9.Get(),
            device10.Get()
        };

        for (auto* iface : pipeline_stream_interfaces) {
            add_unique_pointer_hook(
                iface,
                CREATE_PIPELINE_STATE_VTABLE_INDEX,
                reinterpret_cast<void*>(&D3D12Hook::create_pipeline_state),
                m_create_pipeline_state_hooks,
                m_create_pipeline_state_hook_lookup,
                pipeline_state_stream_slots
            );
        }

        add_unique_pointer_hook(
            command_list,
            SET_PIPELINE_STATE_VTABLE_INDEX,
            reinterpret_cast<void*>(&D3D12Hook::set_pipeline_state),
            m_set_pipeline_state_hooks,
            m_set_pipeline_state_hook_lookup,
            set_pipeline_state_slots
        );

        add_unique_pointer_hook(
            command_list,
            RESOURCE_BARRIER_VTABLE_INDEX,
            reinterpret_cast<void*>(&D3D12Hook::resource_barrier),
            m_resource_barrier_hooks,
            m_resource_barrier_hook_lookup,
            resource_barrier_slots
        );

        add_unique_pointer_hook(
            command_list,
            CLEAR_DEPTH_STENCIL_VIEW_CMDLIST_VTABLE_INDEX,
            reinterpret_cast<void*>(&D3D12Hook::clear_depth_stencil_view_cmd),
            m_clear_depth_stencil_view_cmd_hooks,
            m_clear_depth_stencil_view_cmd_hook_lookup,
            clear_depth_stencil_view_cmd_slots
        );

        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> command_list1{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> command_list2{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList3> command_list3{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> command_list4{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList5> command_list5{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> command_list6{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> command_list7{};

        command_list->QueryInterface(IID_PPV_ARGS(&command_list1));
        command_list->QueryInterface(IID_PPV_ARGS(&command_list2));
        command_list->QueryInterface(IID_PPV_ARGS(&command_list3));
        command_list->QueryInterface(IID_PPV_ARGS(&command_list4));
        command_list->QueryInterface(IID_PPV_ARGS(&command_list5));
        command_list->QueryInterface(IID_PPV_ARGS(&command_list6));
        command_list->QueryInterface(IID_PPV_ARGS(&command_list7));

        const std::array<IUnknown*, 7> command_list_interfaces{
            command_list1.Get(),
            command_list2.Get(),
            command_list3.Get(),
            command_list4.Get(),
            command_list5.Get(),
            command_list6.Get(),
            command_list7.Get()
        };

        for (auto* iface : command_list_interfaces) {
            add_unique_pointer_hook(
                iface,
                SET_PIPELINE_STATE_VTABLE_INDEX,
                reinterpret_cast<void*>(&D3D12Hook::set_pipeline_state),
                m_set_pipeline_state_hooks,
                m_set_pipeline_state_hook_lookup,
                set_pipeline_state_slots
            );

            add_unique_pointer_hook(
                iface,
                RESOURCE_BARRIER_VTABLE_INDEX,
                reinterpret_cast<void*>(&D3D12Hook::resource_barrier),
                m_resource_barrier_hooks,
                m_resource_barrier_hook_lookup,
                resource_barrier_slots
            );

            add_unique_pointer_hook(
                iface,
                CLEAR_DEPTH_STENCIL_VIEW_CMDLIST_VTABLE_INDEX,
                reinterpret_cast<void*>(&D3D12Hook::clear_depth_stencil_view_cmd),
                m_clear_depth_stencil_view_cmd_hooks,
                m_clear_depth_stencil_view_cmd_hook_lookup,
                clear_depth_stencil_view_cmd_slots
            );
        }

        m_hooked = true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize hooks: {}", e.what());
        m_hooked = false;
    }

    if (command_list != nullptr) {
        command_list->Release();
    }

    if (command_allocator != nullptr) {
        command_allocator->Release();
    }

    device->Release();
    command_queue->Release();
    factory->Release();
    swap_chain1->Release();
    swap_chain->Release();

    if (hwnd) {
        ::DestroyWindow(hwnd);
    }

    if (wc.lpszClassName != nullptr) {
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
    }

    return m_hooked;
}

bool D3D12Hook::unhook() {
    if (!m_hooked) {
        return true;
    }

    spdlog::info("Unhooking D3D12");

    m_present_hook.reset();
    m_present1_hook.reset();
    m_create_graphics_pipeline_state_hooks.clear();
    m_create_pipeline_state_hooks.clear();
    m_create_render_target_view_hooks.clear();
    m_create_depth_stencil_view_hooks.clear();
    m_set_pipeline_state_hooks.clear();
    m_resource_barrier_hooks.clear();
    m_clear_depth_stencil_view_cmd_hooks.clear();
    m_create_graphics_pipeline_state_hook_lookup.clear();
    m_create_pipeline_state_hook_lookup.clear();
    m_create_render_target_view_hook_lookup.clear();
    m_create_depth_stencil_view_hook_lookup.clear();
    m_set_pipeline_state_hook_lookup.clear();
    m_set_pipeline_state_hook_generation.fetch_add(1, std::memory_order_release);
    m_resource_barrier_hook_lookup.clear();
    m_clear_depth_stencil_view_cmd_hook_lookup.clear();
    m_depth_stencil_observer.store(nullptr, std::memory_order_release);
    m_swapchain_hook.reset();

    m_hooked = false;
    m_is_phase_1 = true;

    return true;
}

PointerHook* D3D12Hook::find_create_graphics_pipeline_state_hook(void* slot) const {
    if (const auto it = m_create_graphics_pipeline_state_hook_lookup.find(reinterpret_cast<uintptr_t>(slot)); it != m_create_graphics_pipeline_state_hook_lookup.end()) {
        return it->second;
    }

    return m_create_graphics_pipeline_state_hooks.empty() ? nullptr : m_create_graphics_pipeline_state_hooks.front().get();
}

PointerHook* D3D12Hook::find_create_pipeline_state_hook(void* slot) const {
    if (const auto it = m_create_pipeline_state_hook_lookup.find(reinterpret_cast<uintptr_t>(slot)); it != m_create_pipeline_state_hook_lookup.end()) {
        return it->second;
    }

    return m_create_pipeline_state_hooks.empty() ? nullptr : m_create_pipeline_state_hooks.front().get();
}

PointerHook* D3D12Hook::find_create_render_target_view_hook(void* slot) const {
    if (const auto it = m_create_render_target_view_hook_lookup.find(reinterpret_cast<uintptr_t>(slot)); it != m_create_render_target_view_hook_lookup.end()) {
        return it->second;
    }

    return m_create_render_target_view_hooks.empty() ? nullptr : m_create_render_target_view_hooks.front().get();
}

PointerHook* D3D12Hook::find_create_depth_stencil_view_hook(void* slot) const {
    if (const auto it = m_create_depth_stencil_view_hook_lookup.find(reinterpret_cast<uintptr_t>(slot)); it != m_create_depth_stencil_view_hook_lookup.end()) {
        return it->second;
    }

    return m_create_depth_stencil_view_hooks.empty() ? nullptr : m_create_depth_stencil_view_hooks.front().get();
}

PointerHook* D3D12Hook::find_set_pipeline_state_hook(void* slot) const {
    if (const auto it = m_set_pipeline_state_hook_lookup.find(reinterpret_cast<uintptr_t>(slot)); it != m_set_pipeline_state_hook_lookup.end()) {
        return it->second;
    }

    return m_set_pipeline_state_hooks.empty() ? nullptr : m_set_pipeline_state_hooks.front().get();
}

PointerHook* D3D12Hook::find_resource_barrier_hook(void* slot) const {
    if (const auto it = m_resource_barrier_hook_lookup.find(reinterpret_cast<uintptr_t>(slot)); it != m_resource_barrier_hook_lookup.end()) {
        return it->second;
    }

    return m_resource_barrier_hooks.empty() ? nullptr : m_resource_barrier_hooks.front().get();
}

PointerHook* D3D12Hook::find_clear_depth_stencil_view_cmd_hook(void* slot) const {
    if (const auto it = m_clear_depth_stencil_view_cmd_hook_lookup.find(reinterpret_cast<uintptr_t>(slot)); it != m_clear_depth_stencil_view_cmd_hook_lookup.end()) {
        return it->second;
    }

    return m_clear_depth_stencil_view_cmd_hooks.empty() ? nullptr : m_clear_depth_stencil_view_cmd_hooks.front().get();
}

thread_local int32_t g_present_depth = 0;

HRESULT D3D12Hook::present_internal(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags, DXGI_PRESENT_PARAMETERS* params, bool present1) {
    auto d3d12 = g_d3d12_hook;
    const auto original_sync_interval = sync_interval;
    const auto original_flags = flags;

    HWND swapchain_wnd{nullptr};
    swap_chain->GetHwnd(&swapchain_wnd);

    using Present1Fn = HRESULT(*)(IDXGISwapChain3*, UINT, UINT, DXGI_PRESENT_PARAMETERS*);
    Present1Fn present_fn{nullptr};

    if (!present1) {
        present_fn = d3d12->m_present_hook->get_original<Present1Fn>();
    } else {
        present_fn = d3d12->m_present1_hook->get_original<Present1Fn>();
    }

    if (d3d12->m_is_phase_1 && WindowFilter::get().is_filtered(swapchain_wnd)) {
        return present_fn(swap_chain, sync_interval, flags, params);
    }

    if (!d3d12->m_is_phase_1 && swap_chain != d3d12->m_swapchain_hook->get_instance()) {
        const auto og_instance = d3d12->m_swapchain_hook->get_instance();

        // If the original swapchain instance is invalid, then we should not proceed, and rehook the swapchain
        if (IsBadReadPtr(og_instance, sizeof(void*)) || IsBadReadPtr(og_instance.deref(), sizeof(void*))) {
            spdlog::error("Bad read pointer for original swapchain instance, re-hooking");
            d3d12->m_is_phase_1 = true;
        }

        if (!d3d12->m_is_phase_1) {
            return present_fn(swap_chain, sync_interval, flags, params);
        }
    }

    if (d3d12->m_is_phase_1) {
        //d3d12->m_present_hook.reset();
        d3d12->m_swapchain_hook.reset();

        // vtable hook the swapchain instead of global hooking
        // this seems safer for whatever reason
        // if we globally hook the vtable pointers, it causes all sorts of weird conflicts with other hooks
        // dont hook present though via this hook so other hooks dont get confused
        d3d12->m_swapchain_hook = std::make_unique<VtableHook>(swap_chain);
        //d3d12->m_swapchain_hook->hook_method(8, (uintptr_t)&D3D12Hook::present);
        d3d12->m_swapchain_hook->hook_method(13, (uintptr_t)&D3D12Hook::resize_buffers);
        d3d12->m_swapchain_hook->hook_method(14, (uintptr_t)&D3D12Hook::resize_target);
        d3d12->m_swapchain_hook->hook_method(38, (uintptr_t)&D3D12Hook::set_color_space1); // IDXGISwapChain3::SetColorSpace1
        d3d12->m_is_phase_1 = false;
    }

    d3d12->m_inside_present = true;
    d3d12->m_swap_chain = swap_chain;

    swap_chain->GetDevice(IID_PPV_ARGS(&d3d12->m_device));

    if (d3d12->m_device != nullptr) {
        if (d3d12->m_using_proton_swapchain) {
            const auto real_swapchain = *(uintptr_t*)((uintptr_t)swap_chain + d3d12->m_proton_swapchain_offset);
            d3d12->m_command_queue = *(ID3D12CommandQueue**)(real_swapchain + d3d12->m_command_queue_offset);
        } else {
            d3d12->m_command_queue = *(ID3D12CommandQueue**)((uintptr_t)swap_chain + d3d12->m_command_queue_offset);
        }

        render::D3D12Diagnostics::get().begin_frame(
            d3d12->m_device,
            swap_chain,
            d3d12->m_command_queue,
            d3d12->m_render_width,
            d3d12->m_render_height,
            d3d12->m_display_width,
            d3d12->m_display_height,
            d3d12->m_using_proton_swapchain,
            d3d12->m_using_frame_generation_swapchain
        );

        log_dune_present_path_once(swap_chain, d3d12->m_command_queue);
    }

    // Diagnostic D3D12 debug-layer capture (see set_debug_layer_wanted). Attach
    // once, the first present after the option is enabled and the device exists.
    if (d3d12->m_device != nullptr && d3d12->m_debug_layer_wanted.load() && !d3d12->m_debug_layer_setup_done) {
        d3d12->m_debug_layer_setup_done = true;
        d3d12->setup_debug_info_queue();
    }
    if (d3d12->m_debug_poll) {
        d3d12->drain_debug_messages();
    }

    if (d3d12->m_swapchain_0 == nullptr) {
        d3d12->m_swapchain_0 = swap_chain;
    } else if (d3d12->m_swapchain_1 == nullptr && swap_chain != d3d12->m_swapchain_0) {
        d3d12->m_swapchain_1 = swap_chain;
    }
    
    // Restore the original bytes
    // if an infinite loop occurs, this will prevent the game from crashing
    // while keeping our hook intact
    if (g_present_depth > 0) {
        auto original_bytes = utility::get_original_bytes(Address{present_fn});

        if (original_bytes) {
            ProtectionOverride protection_override{present_fn, original_bytes->size(), PAGE_EXECUTE_READWRITE};

            memcpy(present_fn, original_bytes->data(), original_bytes->size());

            spdlog::info("Present fixed");
        }

        if ((uintptr_t)present_fn != (uintptr_t)D3D12Hook::present && g_present_depth == 1) {
            spdlog::info("Attempting to call real present function");

            ++g_present_depth;
            const auto result = present_fn(swap_chain, sync_interval, flags, params);
            --g_present_depth;

            if (result != S_OK) {
                spdlog::error("Present failed: {:x}", result);
            }

            return result;
        }

        spdlog::info("Just returning S_OK");
        return S_OK;
    }

    if (d3d12->m_on_present) {
        d3d12->m_on_present(*d3d12);

        if (d3d12->m_next_present_interval) {
            const auto requested_sync_interval = *d3d12->m_next_present_interval;
            const auto no_tearing = d3d12->m_next_present_no_tearing;
            d3d12->m_next_present_interval = std::nullopt;
            d3d12->m_next_present_no_tearing = false;

            const auto swapchain_key = reinterpret_cast<uintptr_t>(swap_chain);
            const auto preserve_for_current_game = should_preserve_present_params_for_current_game();

            if (preserve_for_current_game || d3d12->m_swapchains_requiring_original_present_params.contains(swapchain_key)) {
                if (d3d12->m_original_present_param_skip_logged_swapchains.insert(swapchain_key).second) {
                    if (preserve_for_current_game) {
                        spdlog::warn(
                            "Skipping UEVR Present param override for MafiaTheOldCountry swapchain {:x}; preserving original sync={} flags={:x}",
                            swapchain_key,
                            original_sync_interval,
                            original_flags);
                    } else {
                        spdlog::warn(
                            "Skipping UEVR Present param override for swapchain {:x} after prior original-param recovery",
                            swapchain_key);
                    }
                }
            } else {
                sync_interval = requested_sync_interval;

                if (sync_interval == 0) {
                    BOOL is_fullscreen = 0;
                    swap_chain->GetFullscreenState(&is_fullscreen, nullptr);
                    flags &= ~DXGI_PRESENT_DO_NOT_SEQUENCE;

                    DXGI_SWAP_CHAIN_DESC swap_desc{};
                    swap_chain->GetDesc(&swap_desc);

                    if (no_tearing) {
                        // No-Tear Fast: interval 0 WITHOUT ALLOW_TEARING. On a
                        // flip-model swapchain this cannot tear (the display
                        // still flips on vblank; the newest present wins) while
                        // presents run unthrottled — both AFR eye frames land
                        // each refresh.
                        flags &= ~DXGI_PRESENT_ALLOW_TEARING;
                    } else if (!is_fullscreen && (swap_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0) {
                        flags |= DXGI_PRESENT_ALLOW_TEARING;
                    }
                } else {
                    // Forcing vsync ON while the game presents with tearing
                    // enabled: ALLOW_TEARING is only valid with interval 0 —
                    // leaving it set fails DXGI_ERROR_INVALID_CALL (fatal in UE).
                    flags &= ~DXGI_PRESENT_ALLOW_TEARING;
                }
            }
        }
    }

    ++g_present_depth;

    auto result = S_OK;
    
    if (!d3d12->m_ignore_next_present) {
        result = present_fn(swap_chain, sync_interval, flags, params);

        if (result == DXGI_ERROR_INVALID_CALL &&
            (sync_interval != original_sync_interval || flags != original_flags))
        {
            spdlog::warn(
                "Present failed with modified params, retrying original params. modified_sync={} modified_flags={:x} original_sync={} original_flags={:x}",
                sync_interval,
                flags,
                original_sync_interval,
                original_flags);

            result = present_fn(swap_chain, original_sync_interval, original_flags, params);

            if (result == S_OK) {
                spdlog::warn("Present retry with original params succeeded");
                const auto swapchain_key = reinterpret_cast<uintptr_t>(swap_chain);

                if (d3d12->m_swapchains_requiring_original_present_params.insert(swapchain_key).second) {
                    spdlog::warn(
                        "Marked swapchain {:x} to preserve original Present params after DXGI_ERROR_INVALID_CALL recovery",
                        swapchain_key);
                }
            } else {
                spdlog::error("Present retry with original params failed: {:x}", result);
            }
        }

        if (result != S_OK) {
            spdlog::error("Present failed: {:x}", result);
        }
    } else {
        d3d12->m_ignore_next_present = false;
    }

    --g_present_depth;

    if (d3d12->m_on_post_present) {
        d3d12->m_on_post_present(*d3d12);
    }

    d3d12->m_inside_present = false;

    return result;
}

namespace {
// Publish the active {ID3D12Device*, HWND} pair to the shared RenderDoc capture
// service so StartFrameCapture can target the exact device/window instead of a
// wildcard. Capture-critical for embedded RenderDoc; cheap and harmless when
// RenderDoc isn't loaded (the env gate + is_api_loaded short-circuit it).
// Gated behind UEVR_RENDERDOC_BOOTSTRAP / UEVR_RENDERDOC_TRACK_ACTIVE_PAIR.
bool renderdoc_active_pair_tracking_enabled() {
    static const bool enabled =
        uevr::renderdoc_capture::env_truthy_w(L"UEVR_RENDERDOC_BOOTSTRAP") ||
        uevr::renderdoc_capture::env_truthy_w(L"UEVR_RENDERDOC_TRACK_ACTIVE_PAIR");
    return enabled;
}

void renderdoc_update_active_pair_on_present(IDXGISwapChain3* swap_chain) {
    if (!renderdoc_active_pair_tracking_enabled() || !uevr::renderdoc_capture::is_api_loaded()) {
        return;
    }

    uevr::renderdoc_capture::CapturePair pair{};
    pair.device = (g_d3d12_hook != nullptr) ? static_cast<void*>(g_d3d12_hook->get_device()) : nullptr;
    if (swap_chain != nullptr) {
        DXGI_SWAP_CHAIN_DESC sc_desc{};
        if (SUCCEEDED(swap_chain->GetDesc(&sc_desc))) {
            pair.window = static_cast<void*>(sc_desc.OutputWindow);
        }
    }

    if (pair.device != nullptr || pair.window != nullptr) {
        uevr::renderdoc_capture::set_active_window(pair);
    }
}
} // namespace

HRESULT WINAPI D3D12Hook::present(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags) {
    renderdoc_update_active_pair_on_present(swap_chain);

    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    return D3D12Hook::present_internal(swap_chain, sync_interval, flags, nullptr, false);
}

HRESULT WINAPI D3D12Hook::present1(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags, DXGI_PRESENT_PARAMETERS* params) {
    renderdoc_update_active_pair_on_present(swap_chain);

    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    return D3D12Hook::present_internal(swap_chain, sync_interval, flags, params, true);
}

HRESULT WINAPI D3D12Hook::create_graphics_pipeline_state(
    ID3D12Device* device,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
    REFIID riid,
    void** pipeline_state
) {
    auto d3d12 = g_d3d12_hook;
    const auto slot = &(*(void***)device)[CREATE_GRAPHICS_PIPELINE_STATE_VTABLE_INDEX];
    auto* hook = d3d12->find_create_graphics_pipeline_state_hook(slot);
    auto original = hook != nullptr ? hook->get_original<decltype(D3D12Hook::create_graphics_pipeline_state)*>() : nullptr;

    if (original == nullptr) {
        return E_FAIL;
    }

    const auto result = original(device, desc, riid, pipeline_state);

    auto& shader_registry = render::ShaderOverrideRegistry::get();
    if (shader_registry.should_track_d3d12_pipelines() &&
        SUCCEEDED(result) &&
        pipeline_state != nullptr &&
        *pipeline_state != nullptr &&
        riid == __uuidof(ID3D12PipelineState) &&
        desc != nullptr) {
        shader_registry.register_d3d12_graphics_pipeline_state_creation(
            device,
            static_cast<ID3D12PipelineState*>(*pipeline_state),
            desc
        );
    }

    return result;
}

HRESULT WINAPI D3D12Hook::create_pipeline_state(
    ID3D12Device2* device,
    const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
    REFIID riid,
    void** pipeline_state
) {
    auto d3d12 = g_d3d12_hook;
    const auto slot = &(*(void***)device)[CREATE_PIPELINE_STATE_VTABLE_INDEX];
    auto* hook = d3d12->find_create_pipeline_state_hook(slot);
    auto original = hook != nullptr ? hook->get_original<decltype(D3D12Hook::create_pipeline_state)*>() : nullptr;

    if (original == nullptr) {
        return E_FAIL;
    }

    const auto result = original(device, desc, riid, pipeline_state);

    auto& shader_registry = render::ShaderOverrideRegistry::get();
    if (shader_registry.should_track_d3d12_pipelines() &&
        SUCCEEDED(result) &&
        pipeline_state != nullptr &&
        *pipeline_state != nullptr &&
        riid == __uuidof(ID3D12PipelineState) &&
        desc != nullptr) {
        shader_registry.register_d3d12_pipeline_state_stream_creation(
            device,
            static_cast<ID3D12PipelineState*>(*pipeline_state),
            desc
        );
    }

    return result;
}

void WINAPI D3D12Hook::create_render_target_view(
    ID3D12Device* device,
    ID3D12Resource* resource,
    const D3D12_RENDER_TARGET_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor
) {
    (void)desc;
    auto d3d12 = g_d3d12_hook;
    const auto slot = device != nullptr ? &(*(void***)device)[CREATE_RENDER_TARGET_VIEW_VTABLE_INDEX] : nullptr;
    auto* hook = d3d12 != nullptr ? d3d12->find_create_render_target_view_hook(slot) : nullptr;
    auto original = hook != nullptr ? hook->get_original<decltype(D3D12Hook::create_render_target_view)*>() : nullptr;

    if (original != nullptr) {
        original(device, resource, desc, descriptor);
    }

    render::D3D12Diagnostics::get().register_rtv_descriptor("D3D12Hook::CreateRenderTargetView", resource, descriptor);
}

void WINAPI D3D12Hook::create_depth_stencil_view(
    ID3D12Device* device,
    ID3D12Resource* resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor
) {
    (void)desc;
    auto d3d12 = g_d3d12_hook;
    const auto slot = device != nullptr ? &(*(void***)device)[CREATE_DEPTH_STENCIL_VIEW_VTABLE_INDEX] : nullptr;
    auto* hook = d3d12 != nullptr ? d3d12->find_create_depth_stencil_view_hook(slot) : nullptr;
    auto original = hook != nullptr ? hook->get_original<decltype(D3D12Hook::create_depth_stencil_view)*>() : nullptr;

    if (original != nullptr) {
        original(device, resource, desc, descriptor);
    }

    render::D3D12Diagnostics::get().register_dsv_descriptor("D3D12Hook::CreateDepthStencilView", resource, descriptor);

    if (const auto observer = d3d12 != nullptr ? d3d12->m_depth_stencil_observer.load(std::memory_order_acquire) : nullptr;
        observer != nullptr) {
        observer->on_depth_stencil_view_created(resource, desc, descriptor);
    }

    // AFW/NeverDLSS raw harvest DSV map (see resource_barrier for why this is a
    // callback instead of a second vtable patch on the same slot).
    if (const auto cb = s_on_raw_create_depth_stencil_view.load(std::memory_order_acquire); cb != nullptr) {
        cb(device, resource, desc, descriptor);
    }
}

void WINAPI D3D12Hook::resource_barrier(
    ID3D12GraphicsCommandList* command_list,
    UINT count,
    const D3D12_RESOURCE_BARRIER* barriers)
{
    auto d3d12 = g_d3d12_hook;
    const auto slot = command_list != nullptr ? &(*(void***)command_list)[RESOURCE_BARRIER_VTABLE_INDEX] : nullptr;
    auto* hook = d3d12 != nullptr ? d3d12->find_resource_barrier_hook(slot) : nullptr;
    auto original = hook != nullptr ? hook->get_original<decltype(D3D12Hook::resource_barrier)*>() : nullptr;

    // The DSV-observer depth capture must run before the engine restores a depth
    // resource to DEPTH_WRITE. Nested ResourceBarrier calls made by that copy
    // bypass observation and continue through the real vtable entry normally.
    if (original != nullptr && !g_inside_depth_stencil_observer && d3d12 != nullptr) {
        if (const auto observer = d3d12->m_depth_stencil_observer.load(std::memory_order_acquire); observer != nullptr) {
            g_inside_depth_stencil_observer = true;
            observer->on_resource_barriers(command_list, count, barriers);
            g_inside_depth_stencil_observer = false;
        }
    }

    if (original != nullptr) {
        original(command_list, count, barriers);
    }

    // AFW/NeverDLSS raw harvest (post-original, matching its old vtable patch
    // which called the original first). Registered here instead of a second
    // vtable patch on the same slot — that double-hook went mutually recursive.
    if (const auto cb = s_on_raw_resource_barrier.load(std::memory_order_acquire); cb != nullptr) {
        cb(command_list, count, barriers);
    }
}

void WINAPI D3D12Hook::clear_depth_stencil_view_cmd(ID3D12GraphicsCommandList* command_list, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
    D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT num_rects, const D3D12_RECT* rects)
{
    auto d3d12 = g_d3d12_hook;
    const auto slot = command_list != nullptr ? &(*(void***)command_list)[CLEAR_DEPTH_STENCIL_VIEW_CMDLIST_VTABLE_INDEX] : nullptr;
    auto* hook = d3d12 != nullptr ? d3d12->find_clear_depth_stencil_view_cmd_hook(slot) : nullptr;
    auto original = hook != nullptr ? hook->get_original<decltype(D3D12Hook::clear_depth_stencil_view_cmd)*>() : nullptr;

    if (original != nullptr) {
        original(command_list, dsv, flags, depth, stencil, num_rects, rects);
    }

    // AFW/NeverDLSS raw depth harvest (see resource_barrier above).
    if (const auto cb = s_on_raw_clear_depth_stencil_view.load(std::memory_order_acquire); cb != nullptr) {
        cb(command_list, dsv, flags, depth, stencil, num_rects, rects);
    }
}

void WINAPI D3D12Hook::set_pipeline_state(ID3D12GraphicsCommandList* command_list, ID3D12PipelineState* pipeline_state) {
    auto d3d12 = g_d3d12_hook;
    const auto slot = &(*(void***)command_list)[SET_PIPELINE_STATE_VTABLE_INDEX];
    using SetPipelineStateFn = decltype(D3D12Hook::set_pipeline_state)*;

    struct OriginalCache {
        D3D12Hook* owner{};
        void* slot{};
        uint64_t generation{};
        SetPipelineStateFn original{};
    };

    static thread_local OriginalCache cache{};
    const auto generation = d3d12->m_set_pipeline_state_hook_generation.load(std::memory_order_acquire);

    if (cache.owner != d3d12 || cache.slot != slot || cache.generation != generation || cache.original == nullptr) {
        auto* hook = d3d12->find_set_pipeline_state_hook(slot);
        cache.owner = d3d12;
        cache.slot = slot;
        cache.generation = generation;
        cache.original = hook != nullptr ? hook->get_original<SetPipelineStateFn>() : nullptr;
    }

    const auto original = cache.original;

    if (original == nullptr) {
        return;
    }

    auto& shader_registry = render::ShaderOverrideRegistry::get();
    if (!shader_registry.should_track_d3d12_pipelines()) {
        original(command_list, pipeline_state);
        return;
    }

    auto bound_pipeline_state = shader_registry.resolve_d3d12_pipeline_state(pipeline_state);
    shader_registry.note_d3d12_pipeline_state_bound(pipeline_state, bound_pipeline_state);
    original(command_list, bound_pipeline_state);
}

thread_local int32_t g_resize_buffers_depth = 0;

HRESULT WINAPI D3D12Hook::resize_buffers(IDXGISwapChain3* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags) {
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    spdlog::info("D3D12 resize buffers called");
    spdlog::info(" Parameters: buffer_count {} width {} height {} new_format {} swap_chain_flags {}", buffer_count, width, height, (uint32_t)new_format, swap_chain_flags);

    auto d3d12 = g_d3d12_hook;
    //auto& hook = d3d12->m_resize_buffers_hook;
    //auto resize_buffers_fn = hook->get_original<decltype(D3D12Hook::resize_buffers)*>();

    HWND swapchain_wnd{nullptr};
    swap_chain->GetHwnd(&swapchain_wnd);

    auto resize_buffers_fn = d3d12->m_swapchain_hook->get_method<decltype(D3D12Hook::resize_buffers)*>(13);

    if (WindowFilter::get().is_filtered(swapchain_wnd)) {
        return resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
    }

    // 3D Display mode: force sub-native swapchain sizes up to the display's
    // native size (pixel-exact output modes die under post-composite scaling).
    if (const auto fw = d3d12->m_forced_resize_w.load(), fh = d3d12->m_forced_resize_h.load();
        fw != 0 && fh != 0) {
        // The engine's belief — Slate sizes its UI draw from this.
        d3d12->m_engine_believed_w = width;
        d3d12->m_engine_believed_h = height;

        // Preserve the request as the 3D render resolution — except within
        // the suppress window after our own nudge/kick (games can re-apply
        // OUR requested resolution multiple times, seconds apart; capturing
        // those would silently jump the render resolution to native).
        if (!d3d12->is_render_res_capture_suppressed()) {
            d3d12->m_game_requested_w = width;
            d3d12->m_game_requested_h = height;
        }

        // Pin in BOTH directions: UE's high-DPI viewport path multiplies the
        // requested resolution by the monitor scale (e.g. SMT5V echoes our
        // r.SetRes 3840x2160 back as 5760x3240 at 150%), and an oversized
        // swapchain breaks the pixel-exact output modes just like a small one.
        if (width != fw || height != fh) {
            spdlog::info("D3D12 resize buffers: forcing {}x{} -> {}x{} (native output)", width, height, fw, fh);
            width = fw;
            height = fh;
        }
    }

    d3d12->m_display_width = width;
    d3d12->m_display_height = height;

    if (g_resize_buffers_depth > 0) {
        auto original_bytes = utility::get_original_bytes(Address{resize_buffers_fn});

        if (original_bytes) {
            ProtectionOverride protection_override{resize_buffers_fn, original_bytes->size(), PAGE_EXECUTE_READWRITE};

            memcpy(resize_buffers_fn, original_bytes->data(), original_bytes->size());

            spdlog::info("Resize buffers fixed");
        }

        if ((uintptr_t)resize_buffers_fn != (uintptr_t)&D3D12Hook::resize_buffers && g_resize_buffers_depth == 1) {
            spdlog::info("Attempting to call the real resize buffers function");

            ++g_resize_buffers_depth;
            const auto result = resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
            --g_resize_buffers_depth;

            if (result != S_OK) {
                spdlog::error("Resize buffers failed: {:x}", result);
            }

            return result;
        } else {
            spdlog::info("Just returning S_OK");
            return S_OK;
        }
    }

    if (d3d12->m_on_resize_buffers) {
        d3d12->m_on_resize_buffers(*d3d12, width, height);
    }

    ++g_resize_buffers_depth;

    const auto result = resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
    
    if (result != S_OK) {
        spdlog::error("Resize buffers failed: {:x}", result);
    }

    --g_resize_buffers_depth;

    return result;
}

thread_local int32_t g_resize_target_depth = 0;

HRESULT WINAPI D3D12Hook::resize_target(IDXGISwapChain3* swap_chain, const DXGI_MODE_DESC* new_target_parameters) {
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    spdlog::info("D3D12 resize target called");
    spdlog::info(" Parameters: new_target_parameters {:x}", (uintptr_t)new_target_parameters);

    auto d3d12 = g_d3d12_hook;
    //auto resize_target_fn = d3d12->m_resize_target_hook->get_original<decltype(D3D12Hook::resize_target)*>();

    HWND swapchain_wnd{nullptr};
    swap_chain->GetHwnd(&swapchain_wnd);

    auto resize_target_fn = d3d12->m_swapchain_hook->get_method<decltype(D3D12Hook::resize_target)*>(14);

    if (WindowFilter::get().is_filtered(swapchain_wnd)) {
        return resize_target_fn(swap_chain, new_target_parameters);
    }

    // 3D Display mode: block sub-native display-mode switches — substitute the
    // native size and let DXGI pick the refresh (pixel-exact output modes need
    // the panel's native mode).
    DXGI_MODE_DESC forced_mode{};
    if (const auto fw = d3d12->m_forced_resize_w.load(), fh = d3d12->m_forced_resize_h.load();
        fw != 0 && fh != 0 && new_target_parameters != nullptr &&
        (new_target_parameters->Width != fw || new_target_parameters->Height != fh)) {
        spdlog::info("D3D12 resize target: forcing {}x{} -> {}x{} (native output)",
                     new_target_parameters->Width, new_target_parameters->Height, fw, fh);
        d3d12->m_game_requested_w = new_target_parameters->Width;  // preserved as the 3D render resolution
        d3d12->m_game_requested_h = new_target_parameters->Height;
        forced_mode = *new_target_parameters;
        forced_mode.Width = fw;
        forced_mode.Height = fh;
        forced_mode.RefreshRate = {};                                // let DXGI choose
        forced_mode.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
        forced_mode.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        new_target_parameters = &forced_mode;
    }

    d3d12->m_render_width = new_target_parameters->Width;
    d3d12->m_render_height = new_target_parameters->Height;

    // Restore the original code to the resize_buffers function.
    if (g_resize_target_depth > 0) {
        auto original_bytes = utility::get_original_bytes(Address{resize_target_fn});

        if (original_bytes) {
            ProtectionOverride protection_override{resize_target_fn, original_bytes->size(), PAGE_EXECUTE_READWRITE};

            memcpy(resize_target_fn, original_bytes->data(), original_bytes->size());

            spdlog::info("Resize target fixed");
        }

        if ((uintptr_t)resize_target_fn != (uintptr_t)&D3D12Hook::resize_target && g_resize_target_depth == 1) {
            spdlog::info("Attempting to call the real resize target function");

            ++g_resize_target_depth;
            const auto result = resize_target_fn(swap_chain, new_target_parameters);
            --g_resize_target_depth;

            if (result != S_OK) {
                spdlog::error("Resize target failed: {:x}", result);
            }

            return result;
        } else {
            spdlog::info("Just returning S_OK");
            return S_OK;
        }
    }

    if (d3d12->m_on_resize_target) {
        d3d12->m_on_resize_target(*d3d12, new_target_parameters->Width, new_target_parameters->Height);
    }

    ++g_resize_target_depth;

    const auto result = resize_target_fn(swap_chain, new_target_parameters);

    if (result != S_OK) {
        spdlog::error("Resize target failed: {:x}", result);
    }

    --g_resize_target_depth;

    return result;
}

HRESULT WINAPI D3D12Hook::set_color_space1(IDXGISwapChain3* swap_chain, DXGI_COLOR_SPACE_TYPE color_space) {
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    auto d3d12 = g_d3d12_hook;

    spdlog::info("D3D12 SetColorSpace1 called: {}", (uint32_t)color_space);
    // A 10-bit swapchain is only HDR if the game explicitly sets a PQ/scRGB
    // color space — record it so the flat3d compositor picks the right
    // encoding (SDR games commonly use R10G10B10A2 with plain G22).
    d3d12->m_swapchain_colorspace = (uint32_t)color_space;

    auto set_color_space1_fn = d3d12->m_swapchain_hook->get_method<decltype(D3D12Hook::set_color_space1)*>(38);
    return set_color_space1_fn(swap_chain, color_space);
}

/*HRESULT WINAPI D3D12Hook::create_swap_chain(IDXGIFactory4* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* p_fullscreen_desc, IDXGIOutput* p_restrict_to_output, IDXGISwapChain** swap_chain)
{
    spdlog::info("D3D12 create swapchain called");

    auto d3d12 = g_d3d12_hook;

    d3d12->m_command_queue = (ID3D12CommandQueue*)device;
    
    if (d3d12->m_on_create_swap_chain) {
        d3d12->m_on_create_swap_chain(*d3d12);
    }

    auto create_swap_chain_fn = d3d12->m_create_swap_chain_hook->get_original<decltype(D3D12Hook::create_swap_chain)>();

    return create_swap_chain_fn(factory, device, hwnd, desc, p_fullscreen_desc, p_restrict_to_output, swap_chain);
}*/

