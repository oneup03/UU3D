#define NOMINMAX

#include <windows.h>
#include <winternl.h>

#include <asmjit/asmjit.h>
#include <future>

#include <spdlog/spdlog.h>
#include <utility/Memory.hpp>
#include <utility/Module.hpp>
#include <utility/Scan.hpp>
#include <utility/String.hpp>
#include <utility/Thread.hpp>
#include <utility/Emulation.hpp>
#include <utility/ScopeGuard.hpp>

#include <sdk/EngineModule.hpp>
#include <sdk/UEngine.hpp>
#include <sdk/UGameEngine.hpp>
#include <sdk/CVar.hpp>
#include <sdk/Slate.hpp>
#include <sdk/DynamicRHI.hpp>
#include <sdk/FViewportInfo.hpp>
#include <sdk/Utility.hpp>
#include <sdk/RHICommandList.hpp>
#include <sdk/UGameViewportClient.hpp>
#include <sdk/Globals.hpp>
#include <sdk/FName.hpp>
#include <sdk/UObjectArray.hpp>
#include <sdk/FBoolProperty.hpp>
#include <sdk/FViewport.hpp>
#include <sdk/UKismetRenderingLibrary.hpp>
#include <sdk/UTexture.hpp>
#include <sdk/APlayerCameraManager.hpp>
#include <sdk/FStructProperty.hpp>
#include <sdk/FSceneViewFamily.hpp>

#include <sdk/UGameplayStatics.hpp>
#include <sdk/APawn.hpp>
#include <sdk/APlayerController.hpp>
#include <sdk/USceneCaptureComponent2D.hpp>
#include <sdk/FTextureRenderTargetResource.hpp>

#include "Framework.hpp"
#include "Mods.hpp"
#include "DumperMode.hpp"
#include "mods/UObjectHook.hpp"

#include <bdshemu.h>
#include <bddisasm.h>
#include <disasmtypes.h>

#include <sdk/threading/GameThreadWorker.hpp>
#include <sdk/threading/RenderThreadWorker.hpp>
#include <sdk/threading/RHIThreadWorker.hpp>
#include "../VR.hpp"
#include "../../utility/Logging.hpp"

#include "FFakeStereoRenderingHook.hpp"

#include <tracy/Tracy.hpp>

//#define FFAKE_STEREO_RENDERING_LOG_ALL_CALLS

FFakeStereoRenderingHook* g_hook = nullptr;
uint32_t g_frame_count{};

namespace {
bool is_writable_process_range(uintptr_t address, size_t size);
bool is_readable_process_range(uintptr_t address, size_t size);
bool is_executable_process_range(uintptr_t address, size_t size);

uint64_t steady_clock_milliseconds() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

constexpr uint64_t SCENE_CAPTURE_FAILED_RETRY_DELAY_MS = 500;

template <typename T, size_t Capacity>
class FixedCapacityList {
public:
    bool try_push_back(const T& value) {
        if (m_size >= Capacity) {
            return false;
        }

        m_storage[m_size++] = value;
        return true;
    }

    bool try_push_back(T&& value) {
        if (m_size >= Capacity) {
            return false;
        }

        m_storage[m_size++] = std::move(value);
        return true;
    }

    void clear() { m_size = 0; }
    bool empty() const { return m_size == 0; }
    size_t size() const { return m_size; }
    T* begin() { return m_storage.data(); }
    T* end() { return m_storage.data() + m_size; }
    const T* begin() const { return m_storage.data(); }
    const T* end() const { return m_storage.data() + m_size; }
    T& front() { return m_storage[0]; }
    const T& front() const { return m_storage[0]; }
    T& operator[](size_t index) { return m_storage[index]; }
    const T& operator[](size_t index) const { return m_storage[index]; }

private:
    std::array<T, Capacity> m_storage{};
    size_t m_size{};
};

struct UE57FSceneViewFamilyFunctions {
    using CopyConstructor = sdk::FSceneViewFamily*(__fastcall*)(
        sdk::FSceneViewFamily*,
        const sdk::FSceneViewFamily*);
    using DeletingDestructor = void*(__fastcall*)(sdk::FSceneViewFamily*, uint32_t);

    CopyConstructor copy_constructor{};
    DeletingDestructor deleting_destructor{};
    uintptr_t vtable{};
};

std::optional<UE57FSceneViewFamilyFunctions> resolve_ue57_fsceneviewfamily_functions(uintptr_t expected_vtable) {
    struct ResolverCache {
        std::mutex mutex{};
        bool attempted{};
        HMODULE module{};
        std::optional<UE57FSceneViewFamilyFunctions> functions{};
    };

    static ResolverCache cache{};

    if (expected_vtable == 0 || !is_readable_process_range(expected_vtable, sizeof(uintptr_t))) {
        SPDLOG_ERROR("[UE5.7][NativeStereoFix] FSceneViewFamily vtable is unavailable for copy-constructor resolution");
        return std::nullopt;
    }

    const auto module = utility::get_module_within(expected_vtable).value_or(nullptr);
    if (module == nullptr) {
        SPDLOG_ERROR("[UE5.7][NativeStereoFix] FSceneViewFamily vtable is not owned by a loaded module");
        return std::nullopt;
    }

    std::scoped_lock lock{cache.mutex};

    // The live object is normally FSceneViewFamilyContext, while the copy
    // constructor produces a base FSceneViewFamily. Cache by module instead of
    // the live derived vtable so alternate family subclasses cannot rescan the
    // executable from the render thread.
    if (cache.attempted && cache.module == module) {
        return cache.functions;
    }

    cache.attempted = true;
    cache.module = module;
    cache.functions.reset();

    constexpr std::array<uint8_t, 10> copy_prefix{
        0x48, 0x8D, 0x71, 0x08, 0x45, 0x33, 0xE4, 0x4C, 0x89, 0x26,
    };
    constexpr std::array<uint8_t, 3> vtable_lea_prefix{0x48, 0x8D, 0x05};
    constexpr std::array<uint8_t, 10> copy_source_suffix{
        0x48, 0x89, 0x01, 0x48, 0x8B, 0xFA, 0x48, 0x63, 0x6A, 0x10,
    };
    constexpr std::array<uint8_t, 15> deleting_destructor_prefix{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
        0x8B, 0xDA, 0x48, 0x8B, 0xF9,
    };
    constexpr std::array<uint8_t, 13> deleting_destructor_size_guard{
        0xF6, 0xC3, 0x01, 0x74, 0x0D, 0xBA, 0x98, 0x01, 0x00, 0x00,
        0x48, 0x8B, 0xCF,
    };
    constexpr size_t complete_destructor_call_offset = 0x0F;
    constexpr size_t deleting_destructor_guard_offset = 0x14;
    constexpr size_t delete_call_offset = 0x21;
    constexpr std::array<std::array<uint8_t, 7>, 4> owned_interface_loads{{
        {0x48, 0x8B, 0x89, 0x60, 0x01, 0x00, 0x00},
        {0x48, 0x8B, 0x8B, 0x68, 0x01, 0x00, 0x00},
        {0x48, 0x8B, 0x8B, 0x70, 0x01, 0x00, 0x00},
        {0x48, 0x8B, 0x8B, 0x78, 0x01, 0x00, 0x00},
    }};
    constexpr size_t complete_destructor_probe_size = 0x70;

    const auto resolve_deleting_destructor = [&](uintptr_t candidate_vtable) -> std::optional<uintptr_t> {
        uintptr_t deleting_destructor_address{};
        std::memcpy(
            &deleting_destructor_address,
            reinterpret_cast<const void*>(candidate_vtable),
            sizeof(deleting_destructor_address));

        if (!is_executable_process_range(deleting_destructor_address, sizeof(void*)) ||
            utility::get_module_within(deleting_destructor_address).value_or(nullptr) != module ||
            !is_readable_process_range(deleting_destructor_address, delete_call_offset + 1) ||
            std::memcmp(
                reinterpret_cast<const void*>(deleting_destructor_address),
                deleting_destructor_prefix.data(),
                deleting_destructor_prefix.size()) != 0 ||
            *reinterpret_cast<const uint8_t*>(deleting_destructor_address + complete_destructor_call_offset) != 0xE8 ||
            std::memcmp(
                reinterpret_cast<const void*>(deleting_destructor_address + deleting_destructor_guard_offset),
                deleting_destructor_size_guard.data(),
                deleting_destructor_size_guard.size()) != 0 ||
            *reinterpret_cast<const uint8_t*>(deleting_destructor_address + delete_call_offset) != 0xE8)
        {
            return std::nullopt;
        }

        int32_t complete_destructor_displacement{};
        std::memcpy(
            &complete_destructor_displacement,
            reinterpret_cast<const void*>(deleting_destructor_address + complete_destructor_call_offset + 1),
            sizeof(complete_destructor_displacement));
        const auto complete_destructor_address = deleting_destructor_address +
            complete_destructor_call_offset + 5 + complete_destructor_displacement;

        if (!is_executable_process_range(complete_destructor_address, complete_destructor_probe_size) ||
            utility::get_module_within(complete_destructor_address).value_or(nullptr) != module)
        {
            return std::nullopt;
        }

        const auto complete_destructor_begin = reinterpret_cast<const uint8_t*>(complete_destructor_address);
        const auto complete_destructor_end = complete_destructor_begin + complete_destructor_probe_size;
        for (const auto& load : owned_interface_loads) {
            if (std::search(
                    complete_destructor_begin,
                    complete_destructor_end,
                    load.begin(),
                    load.end()) == complete_destructor_end)
            {
                return std::nullopt;
            }
        }

        return deleting_destructor_address;
    };

    uintptr_t copy_constructor_address{};
    uintptr_t copy_constructor_vtable{};
    uintptr_t deleting_destructor_address{};
    size_t copy_constructor_candidates{};
    size_t copy_signature_matches{};
    const auto module_base = reinterpret_cast<uintptr_t>(module);
    const auto module_size = utility::get_module_size(module).value_or(0);
    const auto module_end = module_base + module_size;
    constexpr const char* copy_signature =
        "48 8D 71 08 45 33 E4 4C 89 26 48 8D 05 ? ? ? ? "
        "48 89 01 48 8B FA 48 63 6A 10";

    uintptr_t scan_cursor = module_base;
    while (module_size != 0 && scan_cursor < module_end) {
        const auto match = utility::scan(scan_cursor, module_end - scan_cursor, copy_signature);
        if (!match) {
            break;
        }

        scan_cursor = *match + 1;
        ++copy_signature_matches;
        const auto vtable_instruction_address = *match + copy_prefix.size();
        const auto instruction = utility::resolve_instruction(vtable_instruction_address);
        if (!instruction || !std::string_view{instruction->instrux.Mnemonic}.starts_with("LEA")) {
            continue;
        }

        const auto resolved_vtable = utility::resolve_displacement(instruction->addr, &instruction->instrux);
        const auto function_start = utility::find_function_start_unwind(instruction->addr);
        const auto function_entry = utility::find_function_entry(instruction->addr);
        if (!resolved_vtable || !function_start || !function_entry ||
            !is_readable_process_range(*resolved_vtable, sizeof(uintptr_t)) ||
            utility::get_module_within(*resolved_vtable).value_or(nullptr) != module)
        {
            continue;
        }

        const auto entry_start = module_base + function_entry->BeginAddress;
        const auto function_size = static_cast<size_t>(function_entry->EndAddress - function_entry->BeginAddress);
        if (*function_start != entry_start || function_size < 0x300 || function_size > 0x1000 ||
            instruction->addr < *function_start + copy_prefix.size())
        {
            continue;
        }

        const auto vtable_write_offset = instruction->addr - *function_start;
        if (vtable_write_offset < 0x20 || vtable_write_offset > 0x38 ||
            !is_readable_process_range(instruction->addr - copy_prefix.size(), copy_prefix.size()) ||
            !is_readable_process_range(instruction->addr, 7 + copy_source_suffix.size()) ||
            std::memcmp(
                reinterpret_cast<const void*>(instruction->addr - copy_prefix.size()),
                copy_prefix.data(),
                copy_prefix.size()) != 0 ||
            std::memcmp(
                reinterpret_cast<const void*>(instruction->addr),
                vtable_lea_prefix.data(),
                vtable_lea_prefix.size()) != 0 ||
            std::memcmp(
                reinterpret_cast<const void*>(instruction->addr + 7),
                copy_source_suffix.data(),
                copy_source_suffix.size()) != 0)
        {
            continue;
        }

        const auto candidate_deleting_destructor = resolve_deleting_destructor(*resolved_vtable);
        if (!candidate_deleting_destructor) {
            continue;
        }

        if (copy_constructor_address != *function_start) {
            copy_constructor_address = *function_start;
            copy_constructor_vtable = *resolved_vtable;
            deleting_destructor_address = *candidate_deleting_destructor;
            ++copy_constructor_candidates;
        }
    }

    if (copy_constructor_candidates != 1 ||
        !is_executable_process_range(copy_constructor_address, sizeof(void*)) ||
        !is_executable_process_range(deleting_destructor_address, sizeof(void*)))
    {
        SPDLOG_ERROR(
            "[UE5.7][NativeStereoFix] Expected exactly one source-and-layout-proven FSceneViewFamily copy constructor; "
            "found {} from {} source-shaped signature match(es)",
            copy_constructor_candidates,
            copy_signature_matches);
        return std::nullopt;
    }

    cache.functions = UE57FSceneViewFamilyFunctions{
        .copy_constructor = reinterpret_cast<UE57FSceneViewFamilyFunctions::CopyConstructor>(copy_constructor_address),
        .deleting_destructor = reinterpret_cast<UE57FSceneViewFamilyFunctions::DeletingDestructor>(deleting_destructor_address),
        .vtable = copy_constructor_vtable,
    };

    SPDLOG_INFO(
        "[UE5.7][NativeStereoFix] Resolved base FSceneViewFamily vtable RVA 0x{:x}, copy constructor RVA 0x{:x}, "
        "deleting destructor RVA 0x{:x}; live family vtable RVA 0x{:x}",
        copy_constructor_vtable - module_base,
        copy_constructor_address - module_base,
        deleting_destructor_address - module_base,
        expected_vtable - module_base);
    return cache.functions;
}

bool validate_ue57_cloned_view_array(
    const sdk::TArray<sdk::FSceneView*>* source,
    const sdk::TArray<sdk::FSceneView*>* clone,
    int32_t max_count)
{
    if (source == nullptr || clone == nullptr ||
        !is_readable_process_range(reinterpret_cast<uintptr_t>(source), sizeof(*source)) ||
        !is_readable_process_range(reinterpret_cast<uintptr_t>(clone), sizeof(*clone)) ||
        source->count < 0 || source->count > max_count || source->capacity < source->count ||
        clone->count != source->count || clone->capacity < clone->count)
    {
        return false;
    }

    if (source->count == 0) {
        return true;
    }

    const auto entries_size = sizeof(sdk::FSceneView*) * static_cast<size_t>(source->count);
    if (source->data == nullptr || clone->data == nullptr || source->data == clone->data ||
        !is_readable_process_range(reinterpret_cast<uintptr_t>(source->data), entries_size) ||
        !is_readable_process_range(reinterpret_cast<uintptr_t>(clone->data), entries_size))
    {
        return false;
    }

    return std::equal(source->data, source->data + source->count, clone->data);
}

bool try_set_ue57_scene_view_family(
    sdk::FSceneView* view,
    sdk::FSceneViewFamily* expected_current,
    sdk::FSceneViewFamily* replacement)
{
    if (view == nullptr || expected_current == nullptr || replacement == nullptr ||
        !is_readable_process_range(reinterpret_cast<uintptr_t>(view), sizeof(uintptr_t)))
    {
        return false;
    }

    const auto family_offset = view->get_view_family_offset(expected_current);
    if (!family_offset.has_value()) {
        return false;
    }

    const auto family_slot = reinterpret_cast<uintptr_t>(view) + *family_offset;

    if (!is_writable_process_range(family_slot, sizeof(replacement))) {
        return false;
    }

    std::memcpy(reinterpret_cast<void*>(family_slot), &replacement, sizeof(replacement));
    return view->has_view_family(replacement);
}

class UE57FSceneViewSingletonPrimaryOverride {
public:
    UE57FSceneViewSingletonPrimaryOverride() = default;
    UE57FSceneViewSingletonPrimaryOverride(const UE57FSceneViewSingletonPrimaryOverride&) = delete;
    UE57FSceneViewSingletonPrimaryOverride& operator=(const UE57FSceneViewSingletonPrimaryOverride&) = delete;

    ~UE57FSceneViewSingletonPrimaryOverride() {
        restore();
    }

    bool initialize(
        sdk::FSceneView* view,
        sdk::FSceneViewFamily* expected_family,
        const char*& failure_reason)
    {
        failure_reason = nullptr;

        if (view == nullptr || expected_family == nullptr || !view->has_view_family(expected_family)) {
            failure_reason = "secondary singleton view or Family backlink changed";
            return false;
        }

        const auto view_address = reinterpret_cast<uintptr_t>(view);
        const auto metadata_address = view_address + ue57_stereo_pass_offset;
        if (!is_readable_process_range(metadata_address, ue57_stereo_metadata_span) ||
            !is_writable_process_range(metadata_address, ue57_stereo_metadata_span))
        {
            failure_reason = "secondary singleton stereo metadata is not safely writable";
            return false;
        }

        std::memcpy(&m_saved_stereo_pass, reinterpret_cast<const void*>(metadata_address), sizeof(m_saved_stereo_pass));
        std::memcpy(
            &m_saved_primary_view_index,
            reinterpret_cast<const void*>(view_address + ue57_primary_view_index_offset),
            sizeof(m_saved_primary_view_index));

        if (m_saved_stereo_pass != static_cast<uint32_t>(EStereoscopicPass::eSSP_SECONDARY) ||
            m_saved_primary_view_index != 0)
        {
            failure_reason = "secondary singleton stereo metadata did not match the proven two-eye layout";
            return false;
        }

        m_view = view;
        m_active = true;

        constexpr uint32_t primary_stereo_pass = EStereoscopicPass::eSSP_PRIMARY;
        constexpr int32_t singleton_primary_view_index = 0;
        std::memcpy(
            reinterpret_cast<void*>(metadata_address),
            &primary_stereo_pass,
            sizeof(primary_stereo_pass));
        std::memcpy(
            reinterpret_cast<void*>(view_address + ue57_primary_view_index_offset),
            &singleton_primary_view_index,
            sizeof(singleton_primary_view_index));

        uint32_t applied_stereo_pass{};
        int32_t applied_primary_view_index{-1};
        std::memcpy(&applied_stereo_pass, reinterpret_cast<const void*>(metadata_address), sizeof(applied_stereo_pass));
        std::memcpy(
            &applied_primary_view_index,
            reinterpret_cast<const void*>(view_address + ue57_primary_view_index_offset),
            sizeof(applied_primary_view_index));

        if (applied_stereo_pass != primary_stereo_pass ||
            applied_primary_view_index != singleton_primary_view_index)
        {
            const bool restored = restore();
            failure_reason = restored
                ? "secondary singleton could not be adapted to an internal primary view"
                : "secondary singleton metadata could not be restored after adaptation failed";
            return false;
        }

        return true;
    }

    bool restore() {
        if (!m_active) {
            return true;
        }

        if (m_view == nullptr) {
            return false;
        }

        const auto view_address = reinterpret_cast<uintptr_t>(m_view);
        const auto metadata_address = view_address + ue57_stereo_pass_offset;
        if (!is_readable_process_range(metadata_address, ue57_stereo_metadata_span) ||
            !is_writable_process_range(metadata_address, ue57_stereo_metadata_span))
        {
            return false;
        }

        std::memcpy(
            reinterpret_cast<void*>(metadata_address),
            &m_saved_stereo_pass,
            sizeof(m_saved_stereo_pass));
        std::memcpy(
            reinterpret_cast<void*>(view_address + ue57_primary_view_index_offset),
            &m_saved_primary_view_index,
            sizeof(m_saved_primary_view_index));

        uint32_t restored_stereo_pass{};
        int32_t restored_primary_view_index{-1};
        std::memcpy(&restored_stereo_pass, reinterpret_cast<const void*>(metadata_address), sizeof(restored_stereo_pass));
        std::memcpy(
            &restored_primary_view_index,
            reinterpret_cast<const void*>(view_address + ue57_primary_view_index_offset),
            sizeof(restored_primary_view_index));

        if (restored_stereo_pass != m_saved_stereo_pass ||
            restored_primary_view_index != m_saved_primary_view_index)
        {
            return false;
        }

        m_active = false;
        m_view = nullptr;
        return true;
    }

private:
    // Verified against UE5.7 FSceneView's native copy constructor in Venice.
    static constexpr size_t ue57_stereo_pass_offset = 0xDD0;
    static constexpr size_t ue57_primary_view_index_offset = 0xDD8;
    static constexpr size_t ue57_stereo_metadata_span =
        ue57_primary_view_index_offset + sizeof(int32_t) - ue57_stereo_pass_offset;

    sdk::FSceneView* m_view{};
    uint32_t m_saved_stereo_pass{};
    int32_t m_saved_primary_view_index{};
    bool m_active{};
};

class UE57FSceneViewFamilyClone {
public:
    UE57FSceneViewFamilyClone() = default;
    UE57FSceneViewFamilyClone(const UE57FSceneViewFamilyClone&) = delete;
    UE57FSceneViewFamilyClone& operator=(const UE57FSceneViewFamilyClone&) = delete;

    ~UE57FSceneViewFamilyClone() {
        reset();
    }

    bool initialize(
        sdk::FSceneViewFamily* source,
        uintptr_t expected_vtable,
        const char*& failure_reason,
        bool& capability_failure)
    {
        failure_reason = nullptr;
        capability_failure = false;

        if (source == nullptr ||
            !is_readable_process_range(reinterpret_cast<uintptr_t>(source), ue57_scene_view_family_size))
        {
            failure_reason = "source family is unreadable";
            return false;
        }

        uintptr_t source_vtable{};
        std::memcpy(&source_vtable, source, sizeof(source_vtable));
        if (source_vtable != expected_vtable) {
            failure_reason = "source family vtable changed before cloning";
            return false;
        }

        m_functions = resolve_ue57_fsceneviewfamily_functions(expected_vtable);
        if (!m_functions) {
            failure_reason = "copy constructor or destructor could not be proven";
            capability_failure = true;
            return false;
        }

        for (size_t index = 0; index < m_borrowed_owned_interfaces.size(); ++index) {
            std::memcpy(
                &m_borrowed_owned_interfaces[index],
                reinterpret_cast<const void*>(
                    reinterpret_cast<uintptr_t>(source) + ue57_owned_interface_offset + index * sizeof(uintptr_t)),
                sizeof(uintptr_t));
        }

        m_allocator = sdk::FMalloc::get();
        if (m_allocator == nullptr) {
            failure_reason = "engine allocator is unavailable";
            return false;
        }

        constexpr size_t guarded_family_storage_size = 0x1000;
        m_object = reinterpret_cast<sdk::FSceneViewFamily*>(
            m_allocator->malloc(guarded_family_storage_size, 16));
        if (m_object == nullptr) {
            failure_reason = "engine allocation for cloned family failed";
            return false;
        }
        std::memset(m_object, 0, guarded_family_storage_size);

        const auto copy_result = m_functions->copy_constructor(m_object, source);
        m_constructed = true;
        if (copy_result != m_object) {
            failure_reason = "copy constructor returned an unexpected object";
            reset();
            return false;
        }

        uintptr_t cloned_vtable{};
        std::memcpy(&cloned_vtable, m_object, sizeof(cloned_vtable));
        const auto source_views = source->get_views();
        const auto cloned_views = m_object->get_views();
        const auto source_all_views = source_views != nullptr ? source_views + 1 : nullptr;
        const auto cloned_all_views = cloned_views != nullptr ? cloned_views + 1 : nullptr;
        std::array<uintptr_t, ue57_owned_interface_count> cloned_owned_interfaces{};
        for (size_t index = 0; index < cloned_owned_interfaces.size(); ++index) {
            std::memcpy(
                &cloned_owned_interfaces[index],
                reinterpret_cast<const void*>(
                    reinterpret_cast<uintptr_t>(m_object) + ue57_owned_interface_offset + index * sizeof(uintptr_t)),
                sizeof(uintptr_t));
        }

        if (cloned_vtable != m_functions->vtable ||
            !validate_ue57_cloned_view_array(source_views, cloned_views, 16) ||
            !validate_ue57_cloned_view_array(source_all_views, cloned_all_views, 32) ||
            source_all_views->count != 0 || cloned_all_views->count != 0 ||
            cloned_owned_interfaces != m_borrowed_owned_interfaces ||
            m_object->get_render_target() != source->get_render_target() ||
            m_object->get_scene_interface() != source->get_scene_interface())
        {
            failure_reason = "copy constructor output failed structural validation";
            reset();
            return false;
        }

        return true;
    }

    sdk::FSceneViewFamily* get() const {
        return m_object;
    }

    bool mark_additional_view_family(const char*& failure_reason) {
        failure_reason = nullptr;

        if (!m_constructed || m_object == nullptr) {
            failure_reason = "cloned family was not constructed";
            return false;
        }

        const auto flag_address = reinterpret_cast<uintptr_t>(m_object) + ue57_additional_view_family_offset;
        if (!is_readable_process_range(flag_address, sizeof(uint8_t)) ||
            !is_writable_process_range(flag_address, sizeof(uint8_t)))
        {
            failure_reason = "cloned family additional-family flag is not safely writable";
            return false;
        }

        uint8_t current_value{};
        std::memcpy(&current_value, reinterpret_cast<const void*>(flag_address), sizeof(current_value));
        if (current_value > 1) {
            failure_reason = "cloned family additional-family flag had an invalid value";
            return false;
        }

        constexpr uint8_t additional_view_family = 1;
        std::memcpy(
            reinterpret_cast<void*>(flag_address),
            &additional_view_family,
            sizeof(additional_view_family));

        uint8_t applied_value{};
        std::memcpy(&applied_value, reinterpret_cast<const void*>(flag_address), sizeof(applied_value));
        if (applied_value != additional_view_family) {
            failure_reason = "cloned family additional-family flag did not persist";
            return false;
        }

        return true;
    }

private:
    static constexpr size_t ue57_scene_view_family_size = 0x198;
    static constexpr size_t ue57_additional_view_family_offset = 0xB0;
    static constexpr size_t ue57_owned_interface_offset = 0x160;
    static constexpr size_t ue57_owned_interface_count = 4;

    void release_borrowed_interface_ownership() {
        if (!m_constructed || m_object == nullptr) {
            return;
        }

        for (size_t index = 0; index < m_borrowed_owned_interfaces.size(); ++index) {
            const auto slot = reinterpret_cast<uintptr_t>(m_object) +
                ue57_owned_interface_offset + index * sizeof(uintptr_t);
            uintptr_t current{};
            std::memcpy(&current, reinterpret_cast<const void*>(slot), sizeof(current));

            // A non-null pointer copied from the source remains source-owned.
            // A pointer installed later by a view extension remains clone-owned.
            if (m_borrowed_owned_interfaces[index] != 0 &&
                current == m_borrowed_owned_interfaces[index])
            {
                constexpr uintptr_t null_interface{};
                std::memcpy(reinterpret_cast<void*>(slot), &null_interface, sizeof(null_interface));
            }
        }
    }

    void reset() {
        if (m_constructed && m_object != nullptr && m_functions) {
            release_borrowed_interface_ownership();
            m_functions->deleting_destructor(m_object, 0);
        }

        if (m_object != nullptr && m_allocator != nullptr) {
            m_allocator->free(m_object);
        }

        m_constructed = false;
        m_object = nullptr;
        m_allocator = nullptr;
        m_functions.reset();
        m_borrowed_owned_interfaces = {};
    }

    std::optional<UE57FSceneViewFamilyFunctions> m_functions{};
    sdk::FMalloc* m_allocator{};
    sdk::FSceneViewFamily* m_object{};
    std::array<uintptr_t, ue57_owned_interface_count> m_borrowed_owned_interfaces{};
    bool m_constructed{};
};

std::mutex g_shf_texture_probe_mutex{};
std::unordered_set<uintptr_t> g_shf_logged_texture_probe_keys{};
std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point> g_shf_last_texture_probe_by_base{};
std::unordered_set<uintptr_t> g_shf_logged_rtm_candidate_natives{};
uint64_t g_shf_rtm_candidate_count{};
uint64_t g_shf_rtm_candidate_suppressed{};
std::atomic_bool g_dune_force_viewport_rhi_once{false};
std::atomic_uint64_t g_dune_rejected_flat_viewport_rts{0};

struct DunePendingTrueStereoView {
    bool valid{false};
    uint32_t render_frame{};
    uint8_t eye{};
    uintptr_t player_controller{};
    uintptr_t view_info{};
    glm::quat adjusted_quaternion{1.0f, 0.0f, 0.0f, 0.0f};
};

thread_local DunePendingTrueStereoView g_dune_pending_true_stereo_view{};

struct SplitFictionHazeViewBuildContext {
    bool active{};
    bool metadata_valid{true};
    uint8_t eye{};
    uint32_t player_sequence{};
};

struct SplitFictionHazeArrayHeader {
    void** data{};
    int32_t count{};
    int32_t capacity{};
};

thread_local SplitFictionHazeViewBuildContext g_split_fiction_haze_view_build{};

struct DuneTemporalUpscalerOutputsPrefix {
    uintptr_t full_res_texture{};
    int32_t min_x{};
    int32_t min_y{};
    int32_t max_x{};
    int32_t max_y{};
};

static_assert(sizeof(DuneTemporalUpscalerOutputsPrefix) == 0x18);

struct DuneTemporalUpscalerInputsPrefix {
    uint8_t generation_flags[4]{};
    int32_t downsample_override_format{};
    uintptr_t scene_color{};
    uintptr_t scene_depth{};
    uintptr_t scene_velocity{};
};

static_assert(sizeof(DuneTemporalUpscalerInputsPrefix) == 0x20);

enum class DuneFinalOutputVerdict : uint8_t {
    Disabled,
    Collecting,
    NativePairVerified,
    SingleFinalOutput,
    UnsafeOrAmbiguous,
};

struct DuneFinalViewObservation {
    uint64_t sequence{};
    uint32_t global_frame{};
    uint32_t render_frame{};
    uintptr_t view{};
    uintptr_t family{};
    uintptr_t scene_state{};
    uintptr_t full_res_texture{};
    uint32_t stereo_pass{};
    int32_t width{};
    int32_t height{};
    bool eligible{};
};

struct DuneFinalFrameRecord {
    bool occupied{};
    uint64_t frame_id{};
    uint32_t first_global_frame{};
    uint32_t last_global_frame{};
    std::array<uintptr_t, 4> native_resources{};
    std::array<D3D12_RESOURCE_DESC, 4> native_descs{};
    uint8_t native_count{};
    uint8_t eligible_views{};
    uint32_t stereo_pass_mask{};
};

struct DuneFinalOutputTraceState {
    std::mutex mutex{};
    bool armed{};
    bool collection_started{};
    uint64_t session{};
    uint64_t view_sequence{};
    std::array<DuneFinalViewObservation, 96> views{};
    size_t next_view{};
    std::array<DuneFinalFrameRecord, 64> frames{};
    std::array<uint8_t, 300> outcomes{};
    size_t next_outcome{};
    size_t outcome_count{};
    uint32_t pair_frames{};
    uint32_t single_frames{};
    uint32_t ambiguous_frames{};
    uint64_t add_pass_calls{};
    uint64_t eligible_add_pass_calls{};
    uint64_t register_calls{};
    uint64_t native_resource_failures{};
    uint32_t start_global_frame{};
    uint64_t last_frame_id{};
    uintptr_t last_native_resource{};
    D3D12_RESOURCE_DESC last_native_desc{};
    std::chrono::steady_clock::time_point last_log{};
};

std::atomic<DuneFinalOutputVerdict> g_dune_final_output_verdict{DuneFinalOutputVerdict::Disabled};
std::atomic_bool g_dune_final_output_probe_armed{false};
std::atomic_uintptr_t g_dune_get_native_resource_fn{};
DuneFinalOutputTraceState g_dune_final_output_trace{};

const char* dune_final_output_verdict_name(DuneFinalOutputVerdict verdict) {
    switch (verdict) {
    case DuneFinalOutputVerdict::Disabled:
        return "disabled";
    case DuneFinalOutputVerdict::Collecting:
        return "collecting";
    case DuneFinalOutputVerdict::NativePairVerified:
        return "native pair verified";
    case DuneFinalOutputVerdict::SingleFinalOutput:
        return "single final output";
    case DuneFinalOutputVerdict::UnsafeOrAmbiguous:
        return "unsafe or ambiguous";
    default:
        return "unknown";
    }
}

void dune_reset_final_output_trace_locked(bool armed) {
    auto& state = g_dune_final_output_trace;
    state.armed = armed;
    state.collection_started = false;
    ++state.session;
    state.view_sequence = 0;
    state.views = {};
    state.next_view = 0;
    state.frames = {};
    state.outcomes = {};
    state.next_outcome = 0;
    state.outcome_count = 0;
    state.pair_frames = 0;
    state.single_frames = 0;
    state.ambiguous_frames = 0;
    state.add_pass_calls = 0;
    state.eligible_add_pass_calls = 0;
    state.register_calls = 0;
    state.native_resource_failures = 0;
    state.start_global_frame = 0;
    state.last_frame_id = 0;
    state.last_native_resource = 0;
    state.last_native_desc = {};
    state.last_log = {};
    g_dune_final_output_verdict.store(
        armed ? DuneFinalOutputVerdict::Collecting : DuneFinalOutputVerdict::Disabled,
        std::memory_order_release);
}

void dune_set_final_output_probe_armed(bool armed) {
    const auto previous = g_dune_final_output_probe_armed.exchange(armed, std::memory_order_acq_rel);
    if (previous == armed) {
        return;
    }

    std::scoped_lock lock{g_dune_final_output_trace.mutex};
    dune_reset_final_output_trace_locked(armed);
    SPDLOG_INFO(
        "[Dune][FinalOutput] Native dual-view probe {} (session={})",
        armed ? "armed" : "disabled",
        g_dune_final_output_trace.session);
}

void dune_fail_closed_if_probe_stalled() {
    if (!g_dune_final_output_probe_armed.load(std::memory_order_acquire) ||
        g_dune_final_output_verdict.load(std::memory_order_acquire) !=
            DuneFinalOutputVerdict::Collecting) {
        return;
    }

    std::scoped_lock lock{g_dune_final_output_trace.mutex};
    auto& state = g_dune_final_output_trace;
    if (!state.armed ||
        g_dune_final_output_verdict.load(std::memory_order_relaxed) !=
            DuneFinalOutputVerdict::Collecting) {
        return;
    }

    // Injection can precede Dune's first gameplay view by thousands of frames.
    // Do not consume the bounded probe window until an eligible view is observed.
    if (!state.collection_started) {
        return;
    }

    const auto age =
        g_frame_count >= state.start_global_frame
            ? g_frame_count - state.start_global_frame
            : 0;
    const auto missing_ffx_handoff =
        state.eligible_add_pass_calls >= 300 && state.register_calls == 0;
    const auto invalid_native_handoff =
        state.register_calls >= 300 &&
        state.native_resource_failures == state.register_calls &&
        state.outcome_count == 0;
    const auto insufficient_correlated_samples =
        age >= 900 && state.outcome_count < 60;

    if (!missing_ffx_handoff &&
        !invalid_native_handoff &&
        !insufficient_correlated_samples) {
        return;
    }

    g_dune_final_output_verdict.store(
        DuneFinalOutputVerdict::UnsafeOrAmbiguous,
        std::memory_order_release);
    SPDLOG_WARN(
        "[Dune][FinalOutput] Probe failed closed after {} frames: eligible_views={} registrations={} "
        "native_failures={} correlated={}. Native returns to head-tracked mono; select Synchronized "
        "Sequential manually for true stereo.",
        age,
        state.eligible_add_pass_calls,
        state.register_calls,
        state.native_resource_failures,
        state.outcome_count);
}

bool dune_final_output_descs_match(
    const D3D12_RESOURCE_DESC& lhs,
    const D3D12_RESOURCE_DESC& rhs) {
    return lhs.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        rhs.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        lhs.Width == rhs.Width &&
        lhs.Height == rhs.Height &&
        lhs.Format == rhs.Format &&
        lhs.SampleDesc.Count == rhs.SampleDesc.Count;
}

void dune_replace_outcome_locked(uint8_t outcome) {
    auto& state = g_dune_final_output_trace;

    auto remove_outcome = [&](uint8_t old) {
        if (old == 1 && state.pair_frames > 0) {
            --state.pair_frames;
        } else if (old == 2 && state.single_frames > 0) {
            --state.single_frames;
        } else if (old == 3 && state.ambiguous_frames > 0) {
            --state.ambiguous_frames;
        }
    };
    auto add_outcome = [&](uint8_t value) {
        if (value == 1) {
            ++state.pair_frames;
        } else if (value == 2) {
            ++state.single_frames;
        } else if (value == 3) {
            ++state.ambiguous_frames;
        }
    };

    if (state.outcome_count == state.outcomes.size()) {
        remove_outcome(state.outcomes[state.next_outcome]);
    } else {
        ++state.outcome_count;
    }

    state.outcomes[state.next_outcome] = outcome;
    state.next_outcome = (state.next_outcome + 1) % state.outcomes.size();
    add_outcome(outcome);

    if (state.outcome_count < state.outcomes.size()) {
        return;
    }

    const auto total = static_cast<uint32_t>(state.outcome_count);
    DuneFinalOutputVerdict verdict = DuneFinalOutputVerdict::UnsafeOrAmbiguous;

    if (state.pair_frames >= 120 && state.pair_frames * 100 >= total * 95) {
        verdict = DuneFinalOutputVerdict::NativePairVerified;
    } else if (state.single_frames * 100 >= total * 95) {
        verdict = DuneFinalOutputVerdict::SingleFinalOutput;
    }

    const auto previous = g_dune_final_output_verdict.exchange(verdict, std::memory_order_acq_rel);
    if (previous != verdict) {
        SPDLOG_INFO(
            "[Dune][FinalOutput] Verdict={} window={} pair={} single={} ambiguous={}. {}",
            dune_final_output_verdict_name(verdict),
            total,
            state.pair_frames,
            state.single_frames,
            state.ambiguous_frames,
            verdict == DuneFinalOutputVerdict::NativePairVerified
                ? "Native engine stereo remains enabled; final output exposes a stable resource pair."
                : "Native returns to the head-tracked mono safety path. Select Synchronized Sequential manually for true stereo.");
    }
}

void dune_finalize_frame_record_locked(DuneFinalFrameRecord& frame) {
    if (!frame.occupied) {
        return;
    }

    if (frame.eligible_views == 0 || frame.native_count == 0) {
        frame = {};
        return;
    }

    const auto has_distinct_stereo_passes = std::popcount(frame.stereo_pass_mask) >= 2;
    const auto has_compatible_native_pair =
        frame.native_count >= 2 &&
        frame.native_resources[0] != frame.native_resources[1] &&
        dune_final_output_descs_match(frame.native_descs[0], frame.native_descs[1]);

    dune_replace_outcome_locked(
        has_distinct_stereo_passes && has_compatible_native_pair
            ? 1
            : frame.native_count == 1
                ? 2
                : 3);
    frame = {};
}

void dune_record_final_view_observation(const DuneFinalViewObservation& observation) {
    if (!g_dune_final_output_probe_armed.load(std::memory_order_acquire)) {
        return;
    }

    std::scoped_lock lock{g_dune_final_output_trace.mutex};
    auto& state = g_dune_final_output_trace;
    if (!state.armed) {
        return;
    }

    auto stored = observation;
    stored.sequence = ++state.view_sequence;
    state.views[state.next_view] = stored;
    state.next_view = (state.next_view + 1) % state.views.size();
    ++state.add_pass_calls;
    if (stored.eligible) {
        if (!state.collection_started) {
            state.collection_started = true;
            state.start_global_frame = g_frame_count;
            SPDLOG_INFO(
                "[Dune][FinalOutput] Probe collection started on the first eligible gameplay view "
                "(session={} global_frame={})",
                state.session,
                state.start_global_frame);
        }
        ++state.eligible_add_pass_calls;
    }
}

constexpr uint32_t AVOWED_NATIVE_FIX_STABLE_FRAMES = 180;
constexpr uint32_t AVOWED_NATIVE_FIX_FAST_REACQUIRE_STABLE_FRAMES = 45;
constexpr auto AVOWED_NATIVE_FIX_RENDER_GAP = std::chrono::milliseconds(250);
constexpr auto AVOWED_NATIVE_FIX_TRANSITION_HOLD = std::chrono::seconds(10);
constexpr auto AVOWED_NATIVE_FIX_FAST_REACQUIRE_HOLD = std::chrono::milliseconds(1500);
constexpr auto AVOWED_NATIVE_FIX_FAST_REACQUIRE_MAX_MISSING = std::chrono::seconds(60);

// Engine version, resolved without depending on the executable's version
// resource being the ENGINE's version. Games built from a private engine tree
// ship neither of the two things the SDK looks at: Elliot (UE5.6.1) stamps
// Square Enix's own 1.2.0.0 in the version resource and its BRANCH_NAME is just
// "UE5", not "++UE5+Release-5.6", so every version gate below took its UE4
// fallback. The engine's own "Unreal Engine <major>.<minor>.<patch>" display
// string survives that, so use it as a last resort.
// Returns "<major>.<minor>", or "0.00" when nothing could be established.
std::string resolved_engine_version_string() {
    static const std::string result = []() -> std::string {
        if (const auto branch = sdk::search_for_version(utility::get_executable())) {
            return utility::narrow(*branch);
        }

        const auto module = utility::get_executable();
        const auto module_size = utility::get_module_size(module).value_or(0);
        constexpr std::wstring_view needle{L"Unreal Engine "};
        constexpr size_t version_headroom = 16; // major.minor.patch + terminator

        const auto count = module_size / sizeof(wchar_t);
        const auto start = (const wchar_t*)module;

        auto parse_digits = [](const wchar_t* str, size_t& index, uint32_t& out) {
            size_t digits = 0;
            out = 0;

            while (digits < 3 && str[index] >= L'0' && str[index] <= L'9') {
                out = (out * 10) + (uint32_t)(str[index] - L'0');
                ++index;
                ++digits;
            }

            return digits > 0;
        };

        for (size_t i = 0; i + needle.size() + version_headroom < count; ++i) try {
            const wchar_t* ptr = start + i;

            if (ptr[0] != L'U' || ptr[1] != L'n' || std::wmemcmp(ptr, needle.data(), needle.size()) != 0) {
                continue;
            }

            const wchar_t* version = ptr + needle.size();
            size_t index = 0;
            uint32_t major{};
            uint32_t minor{};

            if (!parse_digits(version, index, major) || version[index] != L'.') {
                continue;
            }

            ++index;

            if (!parse_digits(version, index, minor)) {
                continue;
            }

            // Only trust it when it looks like an engine version rather than prose
            if (major != 4 && major != 5) {
                continue;
            }

            SPDLOG_INFO("[EngineVersion] Recovered {}.{} from the embedded \"Unreal Engine\" string", major, minor);
            return std::to_string(major) + "." + std::to_string(minor);
        } catch (...) {
            continue;
        }

        return "0.00";
    }();

    return result;
}

// The SDK resolves FRHITexture::GetNativeResource's vtable slot ONCE, on the
// first call, and only takes its safe UE5.5/5.6 direct-slot path when the
// executable's version resource says 5.5/5.6. When the version resource holds
// a game version instead (see resolved_engine_version_string), it falls back to
// blind-calling vtable slots 2..14 on a live FRHITexture — which on Elliot hung
// the render thread for 60 seconds and then killed the engine with
// "A FRenderResource was deleted without being released first!".
//
// We cannot change what the SDK reads, but it consults its discovery cache
// before that fallback, and it VALIDATES any cached slot with an SEH-guarded
// call before trusting it. So seed the cache with a slot we established safely
// ourselves: probe only the slots the UE5.5/5.6 D3D12 layouts actually use,
// under SEH, and require a real D3D resource back. Must run before the first
// get_native_resource() call anywhere - see the callers.
void* seh_call_native_resource_candidate(const void* texture, void* function) {
    __try {
        using GetNativeResourceFn = void* (*)(const void*);
        return ((GetNativeResourceFn)function)(texture);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void seed_frhitexture_native_resource_slot(FRHITexture2D* texture) {
    static bool s_done = false;

    if (s_done || texture == nullptr || !g_framework->is_dx12() || IsBadReadPtr(texture, sizeof(void*))) {
        return;
    }

    // Only needed while the SDK mis-reads the engine version; when the version
    // resource is right it already takes the safe path on its own.
    const auto sdk_version = sdk::get_file_version_info().dwFileVersionMS;
    if (sdk_version == 0x00050005 || sdk_version == 0x00050006) {
        s_done = true;
        return;
    }

    const auto vtable = *(void***)texture;

    if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*) * 15) ||
        !utility::get_module_within(vtable).has_value()) {
        return; // not a usable candidate yet; try again on the next texture
    }

    // FRHITextureDesc placement identifies the layout, and each layout has its
    // own defensible slot set (same table the SDK uses).
    const auto texture_address = (uintptr_t)texture;
    std::optional<uintptr_t> desc_offset{};

    for (const uintptr_t candidate : {(uintptr_t)0x20, (uintptr_t)0xe0, (uintptr_t)0xf0}) {
        if (IsBadReadPtr((void*)(texture_address + candidate), 0x38)) {
            continue;
        }

        const auto extent_x = *(const int32_t*)(texture_address + candidate + 0x24);
        const auto extent_y = *(const int32_t*)(texture_address + candidate + 0x28);
        const auto num_mips = *(const uint8_t*)(texture_address + candidate + 0x30);
        const auto num_samples = *(const uint8_t*)(texture_address + candidate + 0x31);
        const auto dimension = *(const uint8_t*)(texture_address + candidate + 0x32);
        const auto format = *(const uint8_t*)(texture_address + candidate + 0x33);

        if (extent_x <= 0 || extent_y <= 0 || extent_x > 65536 || extent_y > 65536 ||
            num_mips == 0 || num_mips > 32 ||
            !(num_samples == 1 || num_samples == 2 || num_samples == 4 || num_samples == 8 || num_samples == 16) ||
            dimension > 8 || format == 0 || format > 128) {
            continue;
        }

        desc_offset = candidate;
        break;
    }

    if (!desc_offset) {
        return;
    }

    std::vector<size_t> slots{};
    if (*desc_offset == 0xe0) {
        slots = {7};
    } else if (*desc_offset == 0xf0) {
        slots = {5, 4};
    } else {
        slots = {4, 5};
    }

    for (const auto slot : slots) {
        auto* const func = vtable[slot];

        if (func == nullptr || IsBadReadPtr(func, 1)) {
            continue;
        }

        auto* const resource = seh_call_native_resource_candidate(texture, func);

        if (resource == nullptr || IsBadReadPtr(resource, sizeof(void*))) {
            continue;
        }

        auto* const resource_vtable = *(void**)resource;

        if (resource_vtable == nullptr || IsBadReadPtr(resource_vtable, sizeof(void*))) {
            continue;
        }

        const auto resource_module = utility::get_module_within(resource_vtable);
        const auto resource_module_path = resource_module ? utility::get_module_path(*resource_module) : std::nullopt;

        if (!resource_module_path) {
            continue;
        }

        auto lowered = std::string{*resource_module_path};
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);

        if (!lowered.ends_with("d3d12.dll") && !lowered.ends_with("d3d12core.dll") && !lowered.ends_with("dxgi.dll")) {
            continue;
        }

        if (const auto vtable_module = utility::get_module_within(vtable); vtable_module) {
            sdk::discovery_cache::save_entry("frhitexture_get_native_resource", *vtable_module, {
                {"vtable_index", (uint32_t)slot}
            });
        }

        s_done = true;
        SPDLOG_INFO(
            "[NativeResource] Seeded FRHITexture::GetNativeResource slot {} (desc offset 0x{:x}) so the SDK never "
            "blind-probes this title's vtable",
            slot,
            *desc_offset);
        return;
    }

    SPDLOG_WARN_ONCE(
        "[NativeResource] Could not establish a safe FRHITexture::GetNativeResource slot for texture {:x} "
        "(desc offset 0x{:x})",
        (uintptr_t)texture,
        *desc_offset);
}

bool is_deadzone_ue56_executable() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());

        if (!exe_path || exe_path->find(L"DeadzoneSteam-Win64-Shipping") == std::wstring::npos) {
            return false;
        }

        const auto str_version = resolved_engine_version_string();
        const auto file_version = sdk::get_file_version_info();

        return str_version.starts_with("5.6") || file_version.dwFileVersionMS == 0x00050006;
    }();

    return result;
}

bool is_payday3_aim_guard_enabled() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"PAYDAY3-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

sdk::APlayerController* resolve_player_controller_for_aim(sdk::UEngine* engine, sdk::UWorld* world) {
    if (engine != nullptr) {
        if (const auto local_player = reinterpret_cast<sdk::UObject*>(engine->get_localplayer(0)); local_player != nullptr) {
            if (const auto data = local_player->get_property_data(L"PlayerController"); data != nullptr && !IsBadReadPtr(data, sizeof(void*))) {
                if (const auto controller = *(sdk::APlayerController**)data; controller != nullptr) {
                    return controller;
                }
            }
        }
    }

    if (is_payday3_aim_guard_enabled()) {
        SPDLOG_WARNING_EVERY_N_SEC(
            2,
            "[PAYDAY3][Aim] PlayerController unavailable through LocalPlayer reflection; skipping GameplayStatics fallback");
        return nullptr;
    }

    if (world == nullptr || sdk::UGameplayStatics::static_class() == nullptr) {
        return nullptr;
    }

    const auto gameplay = sdk::UGameplayStatics::get();
    return gameplay != nullptr ? gameplay->get_player_controller(world, 0) : nullptr;
}

sdk::APawn* resolve_acknowledged_pawn_for_aim(sdk::APlayerController* controller) {
    if (controller == nullptr) {
        return nullptr;
    }

    if (is_payday3_aim_guard_enabled()) {
        const auto controller_obj = reinterpret_cast<sdk::UObject*>(controller);

        if (const auto data = controller_obj->get_property_data(L"AcknowledgedPawn"); data != nullptr && !IsBadReadPtr(data, sizeof(void*))) {
            return *(sdk::APawn**)data;
        }

        return nullptr;
    }

    return controller->get_acknowledged_pawn();
}

struct AvowedNativeFixGateState {
    uintptr_t scene{};
    uintptr_t render_target{};
    uintptr_t scene_capture_render_target{};
    uintptr_t scene_capture_native{};
    uintptr_t last_ready_scene{};
    uintptr_t last_ready_render_target{};
    std::chrono::steady_clock::time_point last_update{};
    std::chrono::steady_clock::time_point hold_until{};
    std::chrono::steady_clock::time_point missing_since{};
    uint32_t stable_frames{};
    uint32_t required_stable_frames{AVOWED_NATIVE_FIX_STABLE_FRAMES};
    bool ready{};
    bool has_baseline{};
    bool had_ready_baseline{};
    bool targets_missing{};
    bool fast_reacquire{};
};

std::mutex g_avowed_native_fix_gate_mutex{};
AvowedNativeFixGateState g_avowed_native_fix_gate{};
std::mutex g_ue56_rt_probe_mutex{};
std::unordered_map<uintptr_t, bool> g_ue56_native_resource_probe_cache{};

struct UE51RenderTargetChurnStats {
    uint64_t allocate_seen{};
    uint64_t ui_created{};
    uint64_t ui_reused{};
    uintptr_t last_allocate_return_address{};
    uintptr_t last_ui_create_return_address{};
    uintptr_t last_ui_texture{};
    uint32_t last_ui_width{};
    uint32_t last_ui_height{};
    std::chrono::steady_clock::time_point last_log{};
};

std::mutex g_ue51_rt_churn_mutex{};
UE51RenderTargetChurnStats g_ue51_rt_churn{};
constexpr auto ENGINE_RENDER_TIMING_LOG_INTERVAL = std::chrono::seconds(5);

struct EngineRenderTimingStats {
    uint64_t count{};
    double total_ms{};
    double max_ms{};

    void add(std::chrono::steady_clock::duration duration) {
        const auto ms = std::chrono::duration<double, std::milli>{duration}.count();
        ++count;
        total_ms += ms;
        if (ms > max_ms) {
            max_ms = ms;
        }
    }

    double avg() const {
        return count == 0 ? 0.0 : total_ms / (double)count;
    }

    void reset() {
        count = 0;
        total_ms = 0.0;
        max_ms = 0.0;
    }
};

EngineRenderTimingStats g_begin_render_viewfamily_real_timing{};
EngineRenderTimingStats g_begin_render_viewfamily_timing{};
EngineRenderTimingStats g_prerender_viewfamily_rt_timing{};
std::chrono::steady_clock::time_point g_engine_render_last_log{};

bool shf_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"SHf-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool aphelion_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path &&
            (exe_path->find(L"PIO-WinGDK-Shipping") != std::wstring::npos ||
             exe_path->find(L"PIO-Win64-Shipping") != std::wstring::npos ||
             exe_path->find(L"Aphelion") != std::wstring::npos);
    }();

    return result;
}

bool ark_ascended_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path &&
            (exe_path->find(L"ArkAscended.exe") != std::wstring::npos ||
             exe_path->find(L"ArkAscended-Win64-Shipping") != std::wstring::npos);
    }();

    return result;
}

bool mechwarrior_clans_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path &&
            (exe_path->find(L"MechWarrior-Win64-Shipping") != std::wstring::npos ||
             exe_path->find(L"MW5Clans") != std::wstring::npos);
    }();

    return result;
}

bool redemption_sin_eternal_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path &&
            (exe_path->find(L"Redemption_Sin_Eternal.exe") != std::wstring::npos ||
             exe_path->find(L"Redemption_Sin_Eternal-Win64-Shipping") != std::wstring::npos);
    }();

    return result;
}

bool directive8020_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"Directive8020") != std::wstring::npos;
    }();

    return result;
}

bool everwind_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"Everwind-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool stalker2_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"Stalker2-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool avowed_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && uevr::games::is_avowed_executable_path(*exe_path);
    }();

    return result;
}

bool dune_awakening_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && uevr::games::is_dune_awakening_executable_path(*exe_path);
    }();

    return result;
}

bool halo_campaign_evolved_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());

        if (!exe_path) {
            return false;
        }

        auto lowered = *exe_path;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });

        return lowered.ends_with(L"\\halocampaignevolved.exe") ||
               lowered.ends_with(L"/halocampaignevolved.exe") ||
               lowered == L"halocampaignevolved.exe";
    }();

    return result;
}

bool medium_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());

        if (!exe_path) {
            return false;
        }

        auto lowered = *exe_path;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });

        return lowered.ends_with(L"\\medium-win64-shipping.exe") ||
               lowered.ends_with(L"/medium-win64-shipping.exe") ||
               lowered == L"medium-win64-shipping.exe";
    }();

    return result;
}

bool dead_island_2_ue425_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());

        if (!exe_path) {
            return false;
        }

        auto lowered = *exe_path;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });

        const bool matching_executable =
            lowered.ends_with(L"\\deadisland-win64-shipping.exe") ||
            lowered.ends_with(L"/deadisland-win64-shipping.exe") ||
            lowered == L"deadisland-win64-shipping.exe";

        if (!matching_executable) {
            return false;
        }

        // The fixed-file version is synchronous and reports 0x0004:0x0019
        // for UE4.25. Do not depend on the asynchronous embedded-version
        // scan, which may still be empty when this injection-time guard runs.
        const auto version = sdk::get_file_version_info();
        return HIWORD(version.dwFileVersionMS) == 4 &&
            LOWORD(version.dwFileVersionMS) == 25;
    }();

    return result;
}

using DeadIsland2DeviceIsAPrimaryViewFn = bool (*)(void*, const void*);

std::atomic<DeadIsland2DeviceIsAPrimaryViewFn> g_dead_island_2_device_is_primary_view{};
std::atomic_bool g_dead_island_2_secondary_eye_logged{false};

bool dead_island_2_device_is_primary_view_hook(void* stereo_device, const void* scene_view) {
    const auto original = g_dead_island_2_device_is_primary_view.load(std::memory_order_acquire);

    if (!dead_island_2_ue425_is_current_game() ||
        scene_view == nullptr ||
        !is_readable_process_range(reinterpret_cast<uintptr_t>(scene_view) + 0xB00, sizeof(int32_t)))
    {
        return original != nullptr && original(stereo_device, scene_view);
    }

    int32_t stereo_pass{};
    std::memcpy(
        &stereo_pass,
        reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(scene_view) + 0xB00),
        sizeof(stereo_pass));

    if (stereo_pass == 2) {
        if (!g_dead_island_2_secondary_eye_logged.exchange(true, std::memory_order_acq_rel)) {
            SPDLOG_INFO(
                "[DeadIsland2][UE4.25][StereoState] Treating the right eye as secondary so UE shares the primary eye's temporal exposure state");
        }

        return false;
    }

    return original != nullptr && original(stereo_device, scene_view);
}

bool install_dead_island_2_primary_view_override(std::vector<uintptr_t>& shadow_vtable) {
    constexpr size_t device_is_primary_view_index = 10;
    constexpr size_t slot = device_is_primary_view_index;

    if (!dead_island_2_ue425_is_current_game() || slot >= shadow_vtable.size()) {
        return false;
    }

    const auto original = shadow_vtable[slot];
    if (!is_executable_process_range(original, 0x24)) {
        SPDLOG_WARN(
            "[DeadIsland2][UE4.25][StereoState] Primary-view slot {} is not executable; preserving the engine vtable",
            device_is_primary_view_index);
        return false;
    }

    // Validate the UE4.25 FFakeStereoRenderingDevice mGPU override before
    // replacing it. The call displacement and branch distance are build-local.
    std::array<uint8_t, 0x24> bytes{};
    SIZE_T bytes_read{};
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(original),
            bytes.data(),
            bytes.size(),
            &bytes_read) ||
        bytes_read != bytes.size())
    {
        SPDLOG_WARN(
            "[DeadIsland2][UE4.25][StereoState] Could not read primary-view slot {}; preserving the engine vtable",
            device_is_primary_view_index);
        return false;
    }

    constexpr std::array<uint8_t, 0x24> expected{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
        0x48, 0x8B, 0xF9, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xCA, 0xE8,
        0x00, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x74, 0x00, 0x8B, 0x83,
        0x08, 0x0B, 0x00, 0x00, 0x85, 0xC0};

    for (size_t i = 0; i < expected.size(); ++i) {
        const bool wildcard = (i >= 20 && i <= 23) || i == 27;
        if (!wildcard && bytes[i] != expected[i]) {
            SPDLOG_WARN(
                "[DeadIsland2][UE4.25][StereoState] Primary-view slot {} failed byte validation at +0x{:x}; preserving the engine vtable",
                device_is_primary_view_index,
                i);
            return false;
        }
    }

    g_dead_island_2_device_is_primary_view.store(
        reinterpret_cast<DeadIsland2DeviceIsAPrimaryViewFn>(original),
        std::memory_order_release);
    shadow_vtable[slot] = reinterpret_cast<uintptr_t>(&dead_island_2_device_is_primary_view_hook);

    SPDLOG_INFO(
        "[DeadIsland2][UE4.25][StereoState] Installed validated right-eye secondary-view override at stereo vtable slot {}",
        device_is_primary_view_index);
    return true;
}

struct DeadIslandUE425RawArray {
    void* data;
    int32_t num;
    int32_t max;
};

struct DeadIslandUE425SimpleLightArray {
    DeadIslandUE425RawArray instance_data;
    DeadIslandUE425RawArray per_view_data;
    DeadIslandUE425RawArray instance_per_view_indices;
};

struct DeadIslandUE425SimpleLightRepair {
    uint64_t* packed_indices;
    int32_t instance_count;
    int32_t view_count;
};

static_assert(sizeof(DeadIslandUE425RawArray) == 0x10);
static_assert(offsetof(DeadIslandUE425SimpleLightArray, per_view_data) == 0x10);
static_assert(offsetof(DeadIslandUE425SimpleLightArray, instance_per_view_indices) == 0x20);

std::optional<DeadIslandUE425SimpleLightRepair> repair_dead_island_ue425_simple_light_indices(
    uintptr_t simple_lights_address,
    uint64_t light_index)
{
    if (!is_readable_process_range(simple_lights_address, sizeof(DeadIslandUE425SimpleLightArray))) {
        return std::nullopt;
    }

    auto& lights = *reinterpret_cast<DeadIslandUE425SimpleLightArray*>(simple_lights_address);
    auto& indices = lights.instance_per_view_indices;
    const auto instance_count = lights.instance_data.num;
    const auto per_view_count = lights.per_view_data.num;

    const bool malformed_indices =
        instance_count > 0 && instance_count <= 4096 &&
        per_view_count > instance_count && per_view_count <= 16384 &&
        indices.data == nullptr && indices.num == 0 && indices.max == 0 &&
        light_index < static_cast<uint64_t>(instance_count);

    if (!malformed_indices) {
        return std::nullopt;
    }

    const auto inferred_view_count = per_view_count / instance_count;
    const bool can_reconstruct_all_per_view =
        inferred_view_count >= 2 && inferred_view_count <= 4 &&
        per_view_count == instance_count * inferred_view_count &&
        lights.instance_data.data != nullptr &&
        lights.per_view_data.data != nullptr &&
        lights.instance_data.max >= instance_count &&
        lights.per_view_data.max >= per_view_count &&
        is_readable_process_range(
            reinterpret_cast<uintptr_t>(lights.instance_data.data),
            static_cast<size_t>(instance_count) * 0x1C) &&
        is_readable_process_range(
            reinterpret_cast<uintptr_t>(lights.per_view_data.data),
            static_cast<size_t>(per_view_count) * 0x0C) &&
        is_writable_process_range(
            reinterpret_cast<uintptr_t>(&indices),
            sizeof(indices));

    if (!can_reconstruct_all_per_view) {
        return std::nullopt;
    }

    using PackedIndexTable = std::array<uint64_t, 4096>;
    static auto packed_indices = []() {
        std::array<PackedIndexTable, 5> tables{};
        for (uint32_t view_count = 2; view_count <= 4; ++view_count) {
            for (uint32_t index = 0; index < tables[view_count].size(); ++index) {
                tables[view_count][index] =
                    (uint64_t{1} << 32) | static_cast<uint64_t>(index * view_count);
            }
        }
        return tables;
    }();

    auto* const table = packed_indices[inferred_view_count].data();

    // All downstream UE4.25 consumers call GetViewDependentData on this same
    // FSimpleLightArray. Persist the validated map instead of substituting one
    // register at each inlined call site.
    indices.data = table;
    indices.num = instance_count;
    indices.max = instance_count;

    return DeadIslandUE425SimpleLightRepair{
        .packed_indices = table,
        .instance_count = instance_count,
        .view_count = inferred_view_count,
    };
}

std::optional<uintptr_t> resolve_dead_island_2_game_viewport_draw(uintptr_t base_draw) try {
    if (!dead_island_2_ue425_is_current_game() || base_draw == 0) {
        return std::nullopt;
    }

    constexpr size_t draw_slot = 107;
    constexpr size_t relative_jump_size = 5;

    const auto executable = utility::get_executable();
    auto engine = sdk::UEngine::get();

    if (executable == nullptr || engine == nullptr || IsBadReadPtr(engine, sizeof(void*))) {
        SPDLOG_WARN("[DeadIsland2][UE4.25][Draw] Live viewport resolution deferred because GEngine is unavailable");
        return std::nullopt;
    }

    auto game_viewport_storage = engine->get_property_data(L"GameViewport");

    if (game_viewport_storage == nullptr || IsBadReadPtr(game_viewport_storage, sizeof(void*))) {
        SPDLOG_WARN("[DeadIsland2][UE4.25][Draw] Could not read GEngine.GameViewport");
        return std::nullopt;
    }

    auto game_viewport = *reinterpret_cast<sdk::UGameViewportClient**>(game_viewport_storage);

    if (game_viewport == nullptr || IsBadReadPtr(game_viewport, sizeof(void*))) {
        SPDLOG_WARN("[DeadIsland2][UE4.25][Draw] No live GameViewport is available");
        return std::nullopt;
    }

    auto vtable = *reinterpret_cast<uintptr_t**>(game_viewport);

    if (vtable == nullptr ||
        IsBadReadPtr(vtable, (draw_slot + 1) * sizeof(uintptr_t)) ||
        utility::get_module_within(vtable).value_or(nullptr) != executable)
    {
        SPDLOG_ERROR("[DeadIsland2][UE4.25][Draw] Rejected unreadable or foreign live GameViewport vtable {:x}",
            reinterpret_cast<uintptr_t>(vtable));
        return std::nullopt;
    }

    const auto live_target = vtable[draw_slot];

    if (live_target == 0 ||
        !is_executable_process_range(live_target, relative_jump_size) ||
        utility::get_module_within(live_target).value_or(nullptr) != executable)
    {
        SPDLOG_ERROR("[DeadIsland2][UE4.25][Draw] Rejected live vtable slot {} target {:x}",
            draw_slot,
            live_target);
        return std::nullopt;
    }

    if (live_target == base_draw) {
        SPDLOG_INFO("[DeadIsland2][UE4.25][Draw] Live vtable slot {} directly matches base Draw {:x}",
            draw_slot,
            base_draw);
        return live_target;
    }

    std::array<uint8_t, relative_jump_size> jump_bytes{};
    std::memcpy(jump_bytes.data(), reinterpret_cast<const void*>(live_target), jump_bytes.size());

    if (jump_bytes[0] != 0xE9) {
        SPDLOG_ERROR("[DeadIsland2][UE4.25][Draw] Live vtable slot {} target {:x} is not the validated E9 Draw thunk",
            draw_slot,
            live_target);
        return std::nullopt;
    }

    int32_t displacement{};
    std::memcpy(&displacement, jump_bytes.data() + 1, sizeof(displacement));
    const auto resolved_target = live_target + relative_jump_size + displacement;

    if (resolved_target != base_draw) {
        SPDLOG_ERROR(
            "[DeadIsland2][UE4.25][Draw] Live vtable slot {} thunk {:x} resolves to {:x}, expected scanned base Draw {:x}",
            draw_slot,
            live_target,
            resolved_target,
            base_draw);
        return std::nullopt;
    }

    SPDLOG_INFO(
        "[DeadIsland2][UE4.25][Draw] Validated live DIGameViewportClient vtable slot {} thunk {:x} -> {:x}",
        draw_slot,
        live_target,
        resolved_target);
    return live_target;
} catch (const std::exception& e) {
    SPDLOG_ERROR("[DeadIsland2][UE4.25][Draw] Live viewport resolution failed: {}", e.what());
    return std::nullopt;
} catch (...) {
    SPDLOG_ERROR("[DeadIsland2][UE4.25][Draw] Live viewport resolution failed with an unknown exception");
    return std::nullopt;
}

bool split_fiction_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());

        if (!exe_path) {
            return false;
        }

        auto lowered = *exe_path;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });

        return lowered.ends_with(L"\\splitfiction.exe") ||
               lowered.ends_with(L"/splitfiction.exe") ||
               lowered == L"splitfiction.exe";
    }();

    return result;
}

bool bimbo_paradise_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());

        if (!exe_path) {
            return false;
        }

        auto lowered = *exe_path;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });

        return lowered.ends_with(L"\\bimboparadise.exe") ||
               lowered.ends_with(L"/bimboparadise.exe") ||
               lowered == L"bimboparadise.exe";
    }();

    return result;
}

bool dimension_shift_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"DimensionShift-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool dimension_shift_is_auxiliary_view_family(sdk::FSceneViewFamily* view_family, const char* source) {
    if (!dimension_shift_is_current_game() || view_family == nullptr || g_hook == nullptr) {
        return false;
    }

    try {
        auto* rtm = g_hook->get_render_target_manager();
        auto* main_viewport = rtm != nullptr ? rtm->get_viewport() : nullptr;
        auto* family_target = view_family->get_render_target();

        if (main_viewport == nullptr || family_target == nullptr ||
            reinterpret_cast<void*>(family_target) == reinterpret_cast<void*>(main_viewport))
        {
            return false;
        }

        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[DimensionShift] Isolating auxiliary SceneCapture view family at {} target={:x} main_viewport={:x}",
            source != nullptr ? source : "<unknown>",
            reinterpret_cast<uintptr_t>(family_target),
            reinterpret_cast<uintptr_t>(main_viewport));
        return true;
    } catch (...) {
        SPDLOG_WARNING_EVERY_N_SEC(
            2,
            "[DimensionShift] Failed to classify view family at {}; preserving normal UEVR behavior",
            source != nullptr ? source : "<unknown>");
        return false;
    }
}

bool dune_should_preserve_native_viewport_target() {
    return dune_awakening_is_current_game() &&
        g_hook != nullptr &&
        (g_hook->is_dune_character_creation_active() || g_hook->dune_has_live_pawn());
}

bool dune_is_auxiliary_view_family(sdk::FSceneViewFamily* view_family, const char* source) {
    if (!dune_awakening_is_current_game() || view_family == nullptr) {
        return false;
    }

    if (g_hook == nullptr) {
        return false;
    }

    // A different render-target pointer is not sufficient to identify a
    // showroom family once Dune has entered a playable world. Its custom
    // FidelityFX pipeline legitimately replaces the main family target.
    if (g_hook->dune_has_live_pawn()) {
        return false;
    }

    try {
        auto* rtm = g_hook->get_render_target_manager();
        auto* main_viewport = rtm != nullptr ? rtm->get_viewport() : nullptr;
        auto* family_target = view_family->get_render_target();

        if (main_viewport == nullptr || family_target == nullptr) {
            return false;
        }

        if (reinterpret_cast<void*>(family_target) == reinterpret_cast<void*>(main_viewport)) {
            return false;
        }

        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[Dune][Showroom] Isolating auxiliary view family at {} target={:x} main_viewport={:x}",
            source != nullptr ? source : "<unknown>",
            reinterpret_cast<uintptr_t>(family_target),
            reinterpret_cast<uintptr_t>(main_viewport));
        return true;
    } catch (...) {
        SPDLOG_WARNING_EVERY_N_SEC(
            2,
            "[Dune][Showroom] Failed to classify view family at {}; preserving normal UEVR behavior",
            source != nullptr ? source : "<unknown>");
        return false;
    }
}

bool subnautica2_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path &&
            (exe_path->find(L"Subnautica2-Win64-Shipping") != std::wstring::npos ||
             exe_path->find(L"Subnautica2-WinGDK-Shipping") != std::wstring::npos);
    }();

    return result;
}

bool daysgone_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path &&
            (exe_path->find(L"DaysGone.exe") != std::wstring::npos ||
             exe_path->find(L"BendGame") != std::wstring::npos);
    }();

    return result;
}

bool strikers_club_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path &&
            exe_path->find(L"StrikersClubDemo") != std::wstring::npos &&
            exe_path->find(L"UFG-Win64-Shipping.exe") != std::wstring::npos;
    }();

    return result;
}

std::atomic<uintptr_t> g_strikers_club_shadow_object{};
std::atomic<uintptr_t> g_strikers_club_shadow_vtable{};

struct StrikersClubViewExtensionsHeader {
    uintptr_t data{};
    int32_t count{};
    int32_t capacity{};
};

bool strikers_club_has_valid_engine_stereo_layout(uintptr_t engine, uintptr_t stereo_device_offset) {
    constexpr auto shared_ptr_size = sizeof(TWeakPtr<void*>);
    constexpr int32_t max_reasonable_view_extensions = 4096;

    if (engine == 0 || stereo_device_offset == 0) {
        return false;
    }

    uintptr_t view_extensions{};
    SIZE_T bytes_read{};
    const auto view_extensions_slot = engine + stereo_device_offset + (shared_ptr_size * 2);

    if (!ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(view_extensions_slot),
            &view_extensions,
            sizeof(view_extensions),
            &bytes_read) ||
        bytes_read != sizeof(view_extensions) ||
        view_extensions == 0) {
        return false;
    }

    StrikersClubViewExtensionsHeader header{};
    bytes_read = 0;
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(view_extensions),
            &header,
            sizeof(header),
            &bytes_read) ||
        bytes_read != sizeof(header)) {
        return false;
    }

    if (header.count < 0 ||
        header.capacity < 0 ||
        header.count > header.capacity ||
        header.capacity > max_reasonable_view_extensions) {
        return false;
    }

    if (header.data == 0) {
        return header.count == 0;
    }

    if (header.count == 0) {
        return true;
    }

    uintptr_t first_extension{};
    bytes_read = 0;
    return ReadProcessMemory(
               GetCurrentProcess(),
               reinterpret_cast<const void*>(header.data),
               &first_extension,
               sizeof(first_extension),
               &bytes_read) &&
        bytes_read == sizeof(first_extension);
}

bool install_strikers_club_shadow_vtable(uintptr_t stereo_device) {
    // UE 5.7.1 IStereoRendering ends at EndFinalPostprocessSettings (slot 20).
    constexpr size_t vtable_entry_count = 21;
    static std::array<uintptr_t, vtable_entry_count> shadow_vtable{};
    uintptr_t original_vtable{};
    SIZE_T bytes_read{};

    if (stereo_device == 0 ||
        !ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(stereo_device),
            &original_vtable,
            sizeof(original_vtable),
            &bytes_read) ||
        bytes_read != sizeof(original_vtable) ||
        original_vtable == 0) {
        SPDLOG_WARN(
            "[StrikersClub] Failed to read stereo-device vtable pointer object={:x} error={}",
            stereo_device,
            GetLastError());
        return false;
    }

    MEMORY_BASIC_INFORMATION object_page{};
    MEMORY_BASIC_INFORMATION vtable_page{};
    VirtualQuery(reinterpret_cast<const void*>(stereo_device), &object_page, sizeof(object_page));
    VirtualQuery(reinterpret_cast<const void*>(original_vtable), &vtable_page, sizeof(vtable_page));

    const auto vtable_bytes = shadow_vtable.size() * sizeof(uintptr_t);
    bytes_read = 0;
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(original_vtable),
            shadow_vtable.data(),
            vtable_bytes,
            &bytes_read) ||
        bytes_read != vtable_bytes) {
        SPDLOG_WARN(
            "[StrikersClub] Failed to clone stereo vtable object={:x} vtable={:x} bytes={}/{} object_protect={:x} "
            "vtable_protect={:x} error={}",
            stereo_device,
            original_vtable,
            bytes_read,
            vtable_bytes,
            object_page.Protect,
            vtable_page.Protect,
            GetLastError());
        return false;
    }

    auto* const shadow_vtable_ptr = shadow_vtable.data();
    SIZE_T bytes_written{};
    if (!WriteProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<void*>(stereo_device),
            &shadow_vtable_ptr,
            sizeof(shadow_vtable_ptr),
            &bytes_written) ||
        bytes_written != sizeof(shadow_vtable_ptr)) {
        DWORD old_protect{};
        if (!VirtualProtect(
                reinterpret_cast<void*>(stereo_device),
                sizeof(shadow_vtable_ptr),
                PAGE_READWRITE,
                &old_protect)) {
            SPDLOG_WARN(
                "[StrikersClub] Failed to make stereo object writable object={:x} vtable={:x} protect={:x} error={}",
                stereo_device,
                original_vtable,
                object_page.Protect,
                GetLastError());
            return false;
        }

        std::memcpy(reinterpret_cast<void*>(stereo_device), &shadow_vtable_ptr, sizeof(shadow_vtable_ptr));

        DWORD restored_protect{};
        VirtualProtect(
            reinterpret_cast<void*>(stereo_device),
            sizeof(shadow_vtable_ptr),
            old_protect,
            &restored_protect);
    }

    uintptr_t installed_vtable{};
    bytes_read = 0;
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(stereo_device),
            &installed_vtable,
            sizeof(installed_vtable),
            &bytes_read) ||
        bytes_read != sizeof(installed_vtable) ||
        installed_vtable != reinterpret_cast<uintptr_t>(shadow_vtable_ptr)) {
        SPDLOG_WARN(
            "[StrikersClub] Stereo shadow vtable verification failed object={:x} expected={:x} actual={:x}",
            stereo_device,
            reinterpret_cast<uintptr_t>(shadow_vtable_ptr),
            installed_vtable);
        return false;
    }

    SPDLOG_INFO(
        "[StrikersClub] Stereo shadow vtable installed object={:x} original={:x} shadow={:x} object_protect={:x} "
        "vtable_protect={:x}",
        stereo_device,
        original_vtable,
        installed_vtable,
        object_page.Protect,
        vtable_page.Protect);
    g_strikers_club_shadow_object.store(stereo_device, std::memory_order_release);
    g_strikers_club_shadow_vtable.store(installed_vtable, std::memory_order_release);
    return true;
}

bool everspace2_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && uevr::games::is_everspace2_executable_path(*exe_path);
    }();

    return result;
}

enum class Everspace2PoolTraceKind : uint32_t {
    CreateTracked,
    RefAssignment,
    FinalRelease,
};

constexpr size_t EVERSPACE2_POOL_TRACE_STACK_DEPTH = 12;

struct Everspace2PoolTraceEvent {
    std::atomic<uint64_t> committed_sequence{};
    Everspace2PoolTraceKind kind{};
    uintptr_t pooled_target{};
    uintptr_t targetable_texture{};
    uintptr_t shader_resource_texture{};
    uintptr_t owning_pool{};
    uintptr_t owner_slot{};
    uintptr_t replacement{};
    uint32_t ref_count{};
    uint32_t thread_id{};
    uint16_t stack_depth{};
    std::array<uintptr_t, EVERSPACE2_POOL_TRACE_STACK_DEPTH> stack{};
    std::array<wchar_t, 64> name{};
};

struct Everspace2ExecutableProfile {
    const char* name;
    uint32_t image_timestamp;
    uint32_t image_size;
    uintptr_t compute_memory_size_rva;
    uintptr_t create_render_target_rva;
    uintptr_t ref_assignment_rva;
    uintptr_t preshadow_depth_assignment_return_rva;
    std::array<uint8_t, 5> preshadow_assignment_call_signature;
    uintptr_t final_release_path_rva;
    std::array<uint8_t, 9> final_release_signature;
    uintptr_t world_cleanup_rva;
    std::array<uint8_t, 19> world_cleanup_signature;
};

constexpr Everspace2ExecutableProfile EVERSPACE2_DEMO_PROFILE{
    "Steam Demo",
    0xECBB6BE7,
    0x0A7B1000,
    0x1353B8C,
    0x1600E44,
    0x1583B68,
    0x1356DEB,
    {0xE8, 0x7D, 0xCD, 0x22, 0x00},
    0x11AF8E8,
    {0x48, 0x83, 0xC1, 0x70, 0xE8, 0x3B, 0x21, 0x45, 0x00},
    0,
    {},
};

constexpr Everspace2ExecutableProfile EVERSPACE2_RETAIL_PROFILE{
    "Steam Retail",
    0xFBD525DD,
    0x0A7B2000,
    0x15A0D18,
    0x162386C,
    0x15877B8,
    0x15A3EC3,
    {0xE8, 0xF5, 0x38, 0xFE, 0xFF},
    0x11AD448,
    {0x48, 0x83, 0xC1, 0x70, 0xE8, 0x03, 0x70, 0x47, 0x00},
    0x3BA4AA8,
    {
        0x48, 0x89, 0x5C, 0x24, 0x08,
        0x48, 0x89, 0x74, 0x24, 0x10,
        0x57, 0x48, 0x83, 0xEC, 0x20,
        0x48, 0x8D, 0x59, 0x30,
    },
};

constexpr size_t EVERSPACE2_POOL_TRACE_CAPACITY = 16384;

std::array<Everspace2PoolTraceEvent, EVERSPACE2_POOL_TRACE_CAPACITY> g_everspace2_pool_trace{};
std::atomic<uint64_t> g_everspace2_pool_trace_sequence{};
std::atomic<uintptr_t> g_everspace2_last_bad_pool_entry{};
safetyhook::MidHook g_everspace2_pool_trace_hook{};
safetyhook::InlineHook g_everspace2_create_render_target_hook{};
safetyhook::InlineHook g_everspace2_ref_assignment_hook{};
safetyhook::MidHook g_everspace2_final_release_hook{};
safetyhook::InlineHook g_everspace2_world_cleanup_hook{};
std::atomic_bool g_everspace2_pool_trace_attempted{};
std::atomic<uint32_t> g_everspace2_last_view_pose_frame{std::numeric_limits<uint32_t>::max()};
std::atomic<uint32_t> g_everspace2_next_view_pose_frame{};
std::mutex g_everspace2_preshadow_depth_assignment_mutex{};
const Everspace2ExecutableProfile* g_everspace2_active_profile{};

uint32_t everspace2_get_next_view_pose_frame(VRRuntime* runtime) {
    auto frame_count = g_everspace2_next_view_pose_frame.load(std::memory_order_acquire);

    if (frame_count != 0) {
        return frame_count;
    }

    const auto initial_frame_count = runtime->internal_frame_count + 1;
    if (g_everspace2_next_view_pose_frame.compare_exchange_strong(
            frame_count,
            initial_frame_count,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return initial_frame_count;
    }

    return frame_count;
}

bool everspace2_is_live_uniform_buffer(uintptr_t buffer) {
    if (buffer == 0 || (buffer & (alignof(void*) - 1)) != 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION object_region{};
    if (VirtualQuery(reinterpret_cast<void*>(buffer), &object_region, sizeof(object_region)) == 0 ||
        object_region.State != MEM_COMMIT ||
        (object_region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        buffer + 0x10 > reinterpret_cast<uintptr_t>(object_region.BaseAddress) + object_region.RegionSize)
    {
        return false;
    }

    const auto vtable = *reinterpret_cast<const uintptr_t*>(buffer);
    const auto module = reinterpret_cast<uintptr_t>(utility::get_executable());
    const auto module_size = utility::get_module_size(reinterpret_cast<HMODULE>(module));

    if (!module_size || vtable < module || vtable >= module + *module_size) {
        return false;
    }

    MEMORY_BASIC_INFORMATION vtable_region{};
    if (VirtualQuery(reinterpret_cast<void*>(vtable), &vtable_region, sizeof(vtable_region)) == 0 ||
        vtable_region.State != MEM_COMMIT ||
        (vtable_region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        vtable + sizeof(uintptr_t) >
            reinterpret_cast<uintptr_t>(vtable_region.BaseAddress) + vtable_region.RegionSize)
    {
        return false;
    }

    const auto first_virtual = *reinterpret_cast<const uintptr_t*>(vtable);
    return first_virtual >= module && first_virtual < module + *module_size;
}

void everspace2_world_cleanup_hook(void* scene) {
    constexpr uintptr_t uniform_buffers_offset = 0x30;
    constexpr size_t uniform_buffer_count = 5;
    size_t sanitized{};
    std::shared_ptr<const VRRenderTargetManager_Base::Everspace2D3D12SceneTargetSnapshot>
        retired_scene_target{};

    if (g_hook != nullptr) {
        if (auto* rtm = g_hook->get_render_target_manager(); rtm != nullptr) {
            // Stop new D3D12 frames from acquiring the outgoing world's target.
            // Keep the COM reference alive until the engine cleanup call returns.
            retired_scene_target =
                rtm->retire_everspace2_scene_target_snapshot("FScene::OnWorldCleanup");
        }
    }

    if (scene != nullptr && !IsBadWritePtr(
            reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(scene) + uniform_buffers_offset),
            sizeof(uintptr_t) * uniform_buffer_count))
    {
        auto* slots = reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uintptr_t>(scene) + uniform_buffers_offset);

        for (size_t index = 0; index < uniform_buffer_count; ++index) {
            const auto buffer = slots[index];
            if (buffer != 0 && !everspace2_is_live_uniform_buffer(buffer)) {
                slots[index] = 0;
                ++sanitized;
                SPDLOG_ERROR(
                    "[Everspace2][WorldCleanup] Dropped stale persistent uniform buffer "
                    "scene={:x} slot={} buffer={:x} before FScene::OnWorldCleanup",
                    reinterpret_cast<uintptr_t>(scene),
                    index,
                    buffer);
            }
        }
    }

    if (sanitized != 0) {
        if (const auto logger = spdlog::default_logger(); logger != nullptr) {
            logger->flush();
        }
    }

    g_everspace2_world_cleanup_hook.call<void>(scene);
}

const Everspace2ExecutableProfile* everspace2_find_executable_profile(HMODULE module) {
    if (module == nullptr || IsBadReadPtr(module, sizeof(IMAGE_DOS_HEADER))) {
        return nullptr;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>((uintptr_t)module + dos->e_lfanew);
    if (IsBadReadPtr(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }

    constexpr std::array profiles{
        &EVERSPACE2_DEMO_PROFILE,
        &EVERSPACE2_RETAIL_PROFILE,
    };

    for (const auto* profile : profiles) {
        if (nt->OptionalHeader.SizeOfImage == profile->image_size) {
            if (nt->FileHeader.TimeDateStamp != profile->image_timestamp) {
                SPDLOG_WARN(
                    "[Everspace2][PoolTrace] {} image timestamp differs "
                    "(expected=0x{:x}, actual=0x{:x}); continuing with strict RVA signature validation",
                    profile->name,
                    profile->image_timestamp,
                    nt->FileHeader.TimeDateStamp);
            }

            return profile;
        }
    }

    return nullptr;
}

void record_everspace2_pool_trace(
    Everspace2PoolTraceKind kind,
    uintptr_t pooled_target,
    uintptr_t targetable_texture,
    uintptr_t shader_resource_texture,
    uintptr_t owning_pool,
    uint32_t ref_count,
    const wchar_t* name,
    uintptr_t direct_caller = 0,
    uintptr_t owner_slot = 0,
    uintptr_t replacement = 0,
    bool capture_stack = true)
{
    const auto sequence = g_everspace2_pool_trace_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    auto& event = g_everspace2_pool_trace[(sequence - 1) % g_everspace2_pool_trace.size()];
    event.committed_sequence.store(0, std::memory_order_relaxed);
    event.kind = kind;
    event.pooled_target = pooled_target;
    event.targetable_texture = targetable_texture;
    event.shader_resource_texture = shader_resource_texture;
    event.owning_pool = owning_pool;
    event.owner_slot = owner_slot;
    event.replacement = replacement;
    event.ref_count = ref_count;
    event.thread_id = GetCurrentThreadId();
    event.stack.fill(0);
    event.name.fill(L'\0');

    uint16_t stack_offset{};
    if (direct_caller != 0) {
        event.stack[0] = direct_caller;
        stack_offset = 1;
    }

    if (capture_stack) {
        event.stack_depth = stack_offset + RtlCaptureStackBackTrace(
            1,
            static_cast<DWORD>(event.stack.size() - stack_offset),
            reinterpret_cast<void**>(event.stack.data() + stack_offset),
            nullptr);
    } else {
        event.stack_depth = stack_offset;
    }

    if (name != nullptr) {
        wcsncpy_s(event.name.data(), event.name.size(), name, _TRUNCATE);
    }

    event.committed_sequence.store(sequence, std::memory_order_release);
}

void* everspace2_create_render_target_hook(
    void* pool,
    void* command_list,
    const void* desc,
    uint32_t desc_hash,
    const wchar_t* name)
{
    auto* result = g_everspace2_create_render_target_hook.call<void*>(
        pool,
        command_list,
        desc,
        desc_hash,
        name);

    if (result != nullptr) {
        record_everspace2_pool_trace(
            Everspace2PoolTraceKind::CreateTracked,
            reinterpret_cast<uintptr_t>(result),
            *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(result) + 0x08),
            *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(result) + 0x10),
            *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(result) + 0x20),
            *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(result) + 0x68),
            name);
    }

    return result;
}

void* everspace2_ref_assignment_hook(void* owner_slot_ptr, void* replacement_ptr) {
    const auto direct_caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const auto* profile = g_everspace2_active_profile;
    if (profile == nullptr) {
        return g_everspace2_ref_assignment_hook.call<void*>(owner_slot_ptr, replacement_ptr);
    }

    const auto expected_caller =
        reinterpret_cast<uintptr_t>(utility::get_executable()) +
        profile->preshadow_depth_assignment_return_rva;

    if (direct_caller != expected_caller) {
        return g_everspace2_ref_assignment_hook.call<void*>(owner_slot_ptr, replacement_ptr);
    }

    // ES2 can execute overlapping shadow-depth setup during the cutscene
    // transition. TRefCountPtr assignment itself is not safe when two callers
    // replace the same slot concurrently: both can capture and Release the same
    // old pointer, consuming the render-target pool's final reference.
    std::scoped_lock lock{g_everspace2_preshadow_depth_assignment_mutex};

    const auto owner_slot = reinterpret_cast<uintptr_t>(owner_slot_ptr);
    if (owner_slot == 0) {
        return g_everspace2_ref_assignment_hook.call<void*>(owner_slot_ptr, replacement_ptr);
    }

    // This is TRefCountPtr<IPooledRenderTarget>::operator=. At entry, RCX is
    // the destination TRefCountPtr and RDX is its replacement raw pointer.
    // The old pointer is still valid until operator= releases it.
    const auto pooled_target = *reinterpret_cast<const uintptr_t*>(owner_slot);
    if (pooled_target == 0) {
        return g_everspace2_ref_assignment_hook.call<void*>(owner_slot_ptr, replacement_ptr);
    }

    const auto owning_pool = *reinterpret_cast<const uintptr_t*>(pooled_target + 0x20);
    if (owning_pool != 0) {
        record_everspace2_pool_trace(
            Everspace2PoolTraceKind::RefAssignment,
            pooled_target,
            *reinterpret_cast<const uintptr_t*>(pooled_target + 0x08),
            *reinterpret_cast<const uintptr_t*>(pooled_target + 0x10),
            owning_pool,
            *reinterpret_cast<const uint32_t*>(pooled_target + 0x68),
            nullptr,
            direct_caller,
            owner_slot,
            reinterpret_cast<uintptr_t>(replacement_ptr),
            false);
    }

    return g_everspace2_ref_assignment_hook.call<void*>(owner_slot_ptr, replacement_ptr);
}

void everspace2_final_release_trace(safetyhook::Context& ctx) {
    const auto pooled_target = static_cast<uintptr_t>(ctx.rbx);
    if (pooled_target == 0) {
        return;
    }

    // At this exact instruction Release has decremented NumRefs to zero but
    // has not run the destructor. The original caller is 0x28 bytes above the
    // current stack pointer after Release's prologue.
    const auto direct_caller = *reinterpret_cast<const uintptr_t*>(ctx.rsp + 0x28);
    record_everspace2_pool_trace(
        Everspace2PoolTraceKind::FinalRelease,
        pooled_target,
        *reinterpret_cast<const uintptr_t*>(pooled_target + 0x08),
        *reinterpret_cast<const uintptr_t*>(pooled_target + 0x10),
        *reinterpret_cast<const uintptr_t*>(pooled_target + 0x20),
        0,
        nullptr,
        direct_caller);
}

const char* everspace2_pool_trace_kind_name(Everspace2PoolTraceKind kind) {
    switch (kind) {
    case Everspace2PoolTraceKind::CreateTracked:
        return "create-tracked";
    case Everspace2PoolTraceKind::RefAssignment:
        return "ref-assignment";
    case Everspace2PoolTraceKind::FinalRelease:
        return "final-release";
    default:
        return "unknown";
    }
}

void log_everspace2_pool_owner_history(uintptr_t pooled_target) {
    const auto module_base = reinterpret_cast<uintptr_t>(utility::get_executable());
    size_t matching_events{};

    for (const auto& event : g_everspace2_pool_trace) {
        const auto sequence = event.committed_sequence.load(std::memory_order_acquire);
        if (sequence == 0 || event.pooled_target != pooled_target) {
            continue;
        }

        ++matching_events;
        const auto name = event.name[0] != L'\0'
            ? utility::narrow(std::wstring{event.name.data()})
            : std::string{"<none>"};

        SPDLOG_ERROR(
            "[Everspace2][PoolOwner] sequence={} kind={} object={:x} targetable={:x} "
            "shader_resource={:x} owning_pool={:x} owner_slot={:x} replacement={:x} "
            "refs={} thread={} name={} "
            "stack=[{:x},{:x},{:x},{:x},{:x},{:x},{:x},{:x},{:x},{:x},{:x},{:x}] "
            "rvas=[{:x},{:x},{:x},{:x}]",
            sequence,
            everspace2_pool_trace_kind_name(event.kind),
            event.pooled_target,
            event.targetable_texture,
            event.shader_resource_texture,
            event.owning_pool,
            event.owner_slot,
            event.replacement,
            event.ref_count,
            event.thread_id,
            name,
            event.stack[0],
            event.stack[1],
            event.stack[2],
            event.stack[3],
            event.stack[4],
            event.stack[5],
            event.stack[6],
            event.stack[7],
            event.stack[8],
            event.stack[9],
            event.stack[10],
            event.stack[11],
            event.stack[0] >= module_base ? event.stack[0] - module_base : 0,
            event.stack[1] >= module_base ? event.stack[1] - module_base : 0,
            event.stack[2] >= module_base ? event.stack[2] - module_base : 0,
            event.stack[3] >= module_base ? event.stack[3] - module_base : 0);
    }

    if (matching_events == 0) {
        SPDLOG_ERROR(
            "[Everspace2][PoolOwner] No tracked allocation or final-release event remained for object {:x}; "
            "this points to an overwrite of a still-live pooled object or an allocation older than the bounded trace",
            pooled_target);
    }
}

void everspace2_compute_memory_size_trace(safetyhook::Context& ctx) {
    const auto pooled_target = (uintptr_t)ctx.rcx;
    if (pooled_target == 0) {
        return;
    }

    // The engine immediately reads these fields at this callsite, so avoid
    // expensive Win32 pointer probes in a render-thread hot path.
    const auto targetable_texture = *(const uintptr_t*)(pooled_target + 0x08);
    const auto shader_resource_texture = *(const uintptr_t*)(pooled_target + 0x10);
    const auto is_probable_process_pointer = [](uintptr_t pointer) {
        constexpr uintptr_t minimum_user_object = 0x0000010000000000ULL;
        constexpr uintptr_t maximum_user_address = 0x00007FFFFFFFFFFFULL;
        return pointer == 0 ||
            (pointer >= minimum_user_object &&
             pointer <= maximum_user_address &&
             (pointer & (alignof(void*) - 1)) == 0);
    };

    const auto targetable_bad = !is_probable_process_pointer(targetable_texture);
    const auto shader_resource_bad = !is_probable_process_pointer(shader_resource_texture);

    if (!targetable_bad && !shader_resource_bad) {
        return;
    }

    const auto bad_key = pooled_target ^ targetable_texture ^ std::rotl(shader_resource_texture, 17);
    if (g_everspace2_last_bad_pool_entry.exchange(bad_key, std::memory_order_relaxed) == bad_key) {
        return;
    }

    SPDLOG_ERROR(
        "[Everspace2][PoolTrace] corrupt pooled target observed sequence={} pool={:x} "
        "targetable={:x} shader_resource={:x} targetable_bad={} shader_bad={} thread={}",
        g_everspace2_pool_trace_sequence.load(std::memory_order_relaxed),
        pooled_target,
        targetable_texture,
        shader_resource_texture,
        targetable_bad,
        shader_resource_bad,
        GetCurrentThreadId());

    log_everspace2_pool_owner_history(pooled_target);

    if (const auto logger = spdlog::default_logger(); logger != nullptr) {
        logger->flush();
    }
}

void attempt_everspace2_pool_trace() {
    if (!everspace2_is_current_game() ||
        g_everspace2_pool_trace_attempted.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    const auto module = utility::get_executable();
    const auto* profile = everspace2_find_executable_profile(module);
    if (profile == nullptr) {
        SPDLOG_WARN(
            "[Everspace2][PoolTrace] Disabled because the executable fingerprint does not match "
            "a supported demo or retail build");
        return;
    }

    const auto hook_address = (uintptr_t)module + profile->compute_memory_size_rva;
    constexpr std::array<uint8_t, 13> expected{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x41, 0x54,
    };

    if (IsBadReadPtr((void*)hook_address, expected.size()) ||
        std::memcmp((void*)hook_address, expected.data(), expected.size()) != 0)
    {
        SPDLOG_WARN(
            "[Everspace2][PoolTrace] Disabled because FPooledRenderTarget::ComputeMemorySize "
            "signature did not match at {:x}",
            hook_address);
        return;
    }

    const auto create_render_target_address =
        reinterpret_cast<uintptr_t>(module) + profile->create_render_target_rva;
    constexpr std::array<uint8_t, 13> create_render_target_expected{
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41,
    };

    if (IsBadReadPtr((void*)create_render_target_address, create_render_target_expected.size()) ||
        std::memcmp(
            (void*)create_render_target_address,
            create_render_target_expected.data(),
            create_render_target_expected.size()) != 0)
    {
        SPDLOG_ERROR(
            "[Everspace2][PoolTrace] CreateRenderTarget signature did not match at {:x}",
            create_render_target_address);
        return;
    }

    const auto final_release_address =
        reinterpret_cast<uintptr_t>(module) + profile->final_release_path_rva;

    if (IsBadReadPtr((void*)final_release_address, profile->final_release_signature.size()) ||
        std::memcmp(
            (void*)final_release_address,
            profile->final_release_signature.data(),
            profile->final_release_signature.size()) != 0)
    {
        SPDLOG_ERROR(
            "[Everspace2][PoolTrace] FPooledRenderTarget final-release signature did not match at {:x}",
            final_release_address);
        return;
    }

    const auto ref_assignment_address =
        reinterpret_cast<uintptr_t>(module) + profile->ref_assignment_rva;
    constexpr std::array<uint8_t, 13> ref_assignment_expected{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x19,
    };

    if (IsBadReadPtr((void*)ref_assignment_address, ref_assignment_expected.size()) ||
        std::memcmp(
            (void*)ref_assignment_address,
            ref_assignment_expected.data(),
            ref_assignment_expected.size()) != 0)
    {
        SPDLOG_ERROR(
            "[Everspace2][PoolTrace] TRefCountPtr assignment signature did not match at {:x}",
            ref_assignment_address);
        return;
    }

    const auto preshadow_assignment_call_address =
        reinterpret_cast<uintptr_t>(module) +
        profile->preshadow_depth_assignment_return_rva -
        profile->preshadow_assignment_call_signature.size();

    if (IsBadReadPtr(
            reinterpret_cast<void*>(preshadow_assignment_call_address),
            profile->preshadow_assignment_call_signature.size()) ||
        std::memcmp(
            reinterpret_cast<void*>(preshadow_assignment_call_address),
            profile->preshadow_assignment_call_signature.data(),
            profile->preshadow_assignment_call_signature.size()) != 0)
    {
        SPDLOG_ERROR(
            "[Everspace2][PoolTrace] PreshadowCache assignment call signature did not match at {:x}",
            preshadow_assignment_call_address);
        return;
    }

    int32_t preshadow_assignment_displacement{};
    std::memcpy(
        &preshadow_assignment_displacement,
        reinterpret_cast<void*>(preshadow_assignment_call_address + 1),
        sizeof(preshadow_assignment_displacement));

    const auto preshadow_assignment_target =
        preshadow_assignment_call_address +
        profile->preshadow_assignment_call_signature.size() +
        preshadow_assignment_displacement;

    if (preshadow_assignment_target != ref_assignment_address) {
        SPDLOG_ERROR(
            "[Everspace2][PoolTrace] PreshadowCache assignment call target mismatch "
            "expected={:x} actual={:x}",
            ref_assignment_address,
            preshadow_assignment_target);
        return;
    }

    uintptr_t world_cleanup_address{};
    if (profile->world_cleanup_rva != 0) {
        world_cleanup_address =
            reinterpret_cast<uintptr_t>(module) + profile->world_cleanup_rva;

        if (IsBadReadPtr(
                reinterpret_cast<void*>(world_cleanup_address),
                profile->world_cleanup_signature.size()) ||
            std::memcmp(
                reinterpret_cast<void*>(world_cleanup_address),
                profile->world_cleanup_signature.data(),
                profile->world_cleanup_signature.size()) != 0)
        {
            SPDLOG_ERROR(
                "[Everspace2][WorldCleanup] FScene::OnWorldCleanup signature did not match at {:x}",
                world_cleanup_address);
            return;
        }
    }

    g_everspace2_active_profile = profile;
    g_everspace2_pool_trace_hook =
        safetyhook::create_mid((void*)hook_address, &everspace2_compute_memory_size_trace);
    g_everspace2_create_render_target_hook = safetyhook::create_inline(
        reinterpret_cast<void*>(create_render_target_address),
        &everspace2_create_render_target_hook);
    g_everspace2_ref_assignment_hook =
        safetyhook::create_inline(reinterpret_cast<void*>(ref_assignment_address), &everspace2_ref_assignment_hook);
    g_everspace2_final_release_hook =
        safetyhook::create_mid(reinterpret_cast<void*>(final_release_address), &everspace2_final_release_trace);
    if (world_cleanup_address != 0) {
        g_everspace2_world_cleanup_hook = safetyhook::create_inline(
            reinterpret_cast<void*>(world_cleanup_address),
            &everspace2_world_cleanup_hook);
    }

    if (!g_everspace2_pool_trace_hook ||
        !g_everspace2_create_render_target_hook ||
        !g_everspace2_ref_assignment_hook ||
        !g_everspace2_final_release_hook ||
        (world_cleanup_address != 0 && !g_everspace2_world_cleanup_hook))
    {
        SPDLOG_ERROR(
            "[Everspace2][PoolTrace] Failed to install provenance hooks "
            "observer={} create={} assignment={} final_release={} world_cleanup={}",
            static_cast<bool>(g_everspace2_pool_trace_hook),
            static_cast<bool>(g_everspace2_create_render_target_hook),
            static_cast<bool>(g_everspace2_ref_assignment_hook),
            static_cast<bool>(g_everspace2_final_release_hook),
            world_cleanup_address == 0 || static_cast<bool>(g_everspace2_world_cleanup_hook));
        return;
    }

    SPDLOG_INFO(
        "[Everspace2][PoolTrace] Installed {} passive bounded owner trace "
        "observer={:x} create={:x} assignment={:x} final_release={:x} world_cleanup={:x}; "
        "the exact PreshadowCache depth assignment at return RVA 0x{:x} is serialized",
        profile->name,
        hook_address,
        create_render_target_address,
        ref_assignment_address,
        final_release_address,
        world_cleanup_address,
        profile->preshadow_depth_assignment_return_rva);
}

namespace home_together_pool_guard {
constexpr uint32_t IMAGE_TIMESTAMP = 0x27A677D3;
constexpr uint32_t IMAGE_SIZE = 0x0B822000;
constexpr uintptr_t POOLED_RENDER_TARGET_VTABLE_RVA = 0x08D2D560;
constexpr uintptr_t POOLED_RENDER_TARGET_RELEASE_RVA = 0x0346C3C0;
constexpr uintptr_t POOLED_RENDER_TARGET_FINAL_RELEASE_RVA = 0x0346C3E1;
constexpr uintptr_t RENDER_TARGET_POOL_RVA = 0x0A89E0F0;
constexpr uintptr_t RENDER_TARGET_POOL_TARGETS_OFFSET = 0x28;
constexpr uintptr_t POOLED_RENDER_TARGET_POOL_OFFSET = 0x20;
constexpr uintptr_t POOLED_RENDER_TARGET_REFCOUNT_OFFSET = 0x88;

std::atomic_bool attempted{};
std::atomic_uint64_t prevented_final_releases{};
safetyhook::MidHook final_release_hook{};

bool is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"HomeTogether-Win64-Shipping.exe") != std::wstring::npos;
    }();

    return result;
}

void final_release_guard(safetyhook::Context& ctx) {
    // The xadd immediately before this hook has already changed NumRefs from
    // one to zero. A tracked object must still have the pool's owning ref, so
    // reaching zero while its pool slot still points at it is an over-release.
    if (static_cast<uint32_t>(ctx.rdi) != 1) {
        return;
    }

    const auto module = reinterpret_cast<uintptr_t>(utility::get_executable());
    const auto pooled_target = static_cast<uintptr_t>(ctx.rbx);

    if (module == 0 ||
        pooled_target == 0 ||
        !is_readable_process_range(pooled_target, POOLED_RENDER_TARGET_REFCOUNT_OFFSET + sizeof(uint32_t)))
    {
        return;
    }

    uintptr_t vtable{};
    uintptr_t owning_pool{};
    std::memcpy(&vtable, reinterpret_cast<const void*>(pooled_target), sizeof(vtable));
    std::memcpy(
        &owning_pool,
        reinterpret_cast<const void*>(pooled_target + POOLED_RENDER_TARGET_POOL_OFFSET),
        sizeof(owning_pool));

    const auto expected_pool = module + RENDER_TARGET_POOL_RVA;
    if (vtable != module + POOLED_RENDER_TARGET_VTABLE_RVA || owning_pool != expected_pool) {
        return;
    }

    uintptr_t pool_elements{};
    int32_t pool_count{};
    int32_t pool_capacity{};
    const auto pool_array = owning_pool + RENDER_TARGET_POOL_TARGETS_OFFSET;

    if (!is_readable_process_range(pool_array, sizeof(uintptr_t) + sizeof(int32_t) * 2)) {
        return;
    }

    std::memcpy(&pool_elements, reinterpret_cast<const void*>(pool_array), sizeof(pool_elements));
    std::memcpy(&pool_count, reinterpret_cast<const void*>(pool_array + 0x08), sizeof(pool_count));
    std::memcpy(&pool_capacity, reinterpret_cast<const void*>(pool_array + 0x0C), sizeof(pool_capacity));

    constexpr int32_t MAX_REASONABLE_POOL_ELEMENTS = 65536;
    if (pool_elements == 0 ||
        pool_count <= 0 ||
        pool_count > MAX_REASONABLE_POOL_ELEMENTS ||
        pool_capacity < pool_count ||
        pool_capacity > MAX_REASONABLE_POOL_ELEMENTS ||
        !is_readable_process_range(pool_elements, static_cast<size_t>(pool_count) * sizeof(uintptr_t)))
    {
        return;
    }

    int32_t owning_index = -1;
    for (int32_t i = 0; i < pool_count; ++i) {
        uintptr_t candidate{};
        std::memcpy(
            &candidate,
            reinterpret_cast<const void*>(pool_elements + static_cast<uintptr_t>(i) * sizeof(uintptr_t)),
            sizeof(candidate));

        if (candidate == pooled_target) {
            owning_index = i;
            break;
        }
    }

    if (owning_index < 0) {
        // The pool clears its slot before its legitimate final Release.
        return;
    }

    auto* ref_count = reinterpret_cast<std::atomic_uint32_t*>(
        pooled_target + POOLED_RENDER_TARGET_REFCOUNT_OFFSET);
    uint32_t expected_zero{};
    uint32_t restored_count{};

    if (ref_count->compare_exchange_strong(
            expected_zero,
            1,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        restored_count = 1;
    } else if (expected_zero > 0 && expected_zero < 0x100000) {
        // Another owner revived the target after the decrement. It is no
        // longer final and must not enter the destructor path either.
        restored_count = expected_zero;
    } else {
        return;
    }

    // The original function compares EDI with one and returns EDI - 1. Make
    // it follow the non-final path and return the restored reference count.
    ctx.rdi = static_cast<uint64_t>(restored_count) + 1;

    uintptr_t direct_caller{};
    if (is_readable_process_range(ctx.rsp + 0x28, sizeof(direct_caller))) {
        std::memcpy(&direct_caller, reinterpret_cast<const void*>(ctx.rsp + 0x28), sizeof(direct_caller));
    }

    const auto event = prevented_final_releases.fetch_add(1, std::memory_order_relaxed) + 1;
    SPDLOG_ERROR(
        "[HomeTogether][RenderTargetPool] Prevented tracked pooled target over-release "
        "event={} object={:x} pool={:x} index={} restored_refs={} thread={} caller={:x} caller_rva={:x}",
        event,
        pooled_target,
        owning_pool,
        owning_index,
        restored_count,
        GetCurrentThreadId(),
        direct_caller,
        direct_caller >= module ? direct_caller - module : 0);
}

void attempt_install() {
    if (!is_current_game() ||
        g_framework == nullptr ||
        !g_framework->is_dx12() ||
        attempted.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    const auto module = utility::get_executable();
    if (module == nullptr || IsBadReadPtr(module, sizeof(IMAGE_DOS_HEADER))) {
        return;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<uintptr_t>(module) + dos->e_lfanew);
    if (IsBadReadPtr(nt, sizeof(*nt)) ||
        nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.TimeDateStamp != IMAGE_TIMESTAMP ||
        nt->OptionalHeader.SizeOfImage != IMAGE_SIZE)
    {
        SPDLOG_WARN(
            "[HomeTogether][RenderTargetPool] Guard disabled because the executable fingerprint changed");
        return;
    }

    const auto module_base = reinterpret_cast<uintptr_t>(module);
    const auto release = module_base + POOLED_RENDER_TARGET_RELEASE_RVA;
    const auto final_release = module_base + POOLED_RENDER_TARGET_FINAL_RELEASE_RVA;
    const auto vtable_release_slot = module_base + POOLED_RENDER_TARGET_VTABLE_RVA + 7 * sizeof(uintptr_t);
    constexpr std::array<uint8_t, 37> release_signature{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
        0x57, 0x48, 0x83, 0xEC, 0x20, 0xBE, 0xFF, 0xFF, 0xFF, 0xFF,
        0x48, 0x8B, 0xD9, 0x8B, 0xFE, 0xF0, 0x0F, 0xC1, 0xB9, 0x88,
        0x00, 0x00, 0x00, 0x83, 0xFF, 0x01, 0x75,
    };

    uintptr_t vtable_release{};
    if (IsBadReadPtr(reinterpret_cast<void*>(release), release_signature.size()) ||
        std::memcmp(reinterpret_cast<void*>(release), release_signature.data(), release_signature.size()) != 0 ||
        !is_readable_process_range(vtable_release_slot, sizeof(vtable_release)))
    {
        SPDLOG_WARN(
            "[HomeTogether][RenderTargetPool] Guard disabled because FPooledRenderTarget::Release validation failed");
        return;
    }

    std::memcpy(&vtable_release, reinterpret_cast<const void*>(vtable_release_slot), sizeof(vtable_release));
    if (vtable_release != release) {
        SPDLOG_WARN(
            "[HomeTogether][RenderTargetPool] Guard disabled because the FPooledRenderTarget vtable did not match");
        return;
    }

    final_release_hook = safetyhook::create_mid(
        reinterpret_cast<void*>(final_release),
        &final_release_guard);

    if (!final_release_hook) {
        SPDLOG_ERROR(
            "[HomeTogether][RenderTargetPool] Failed to install tracked-target final-release guard at {:x}",
            final_release);
        return;
    }

    SPDLOG_WARN(
        "[HomeTogether][RenderTargetPool] Installed exact-build tracked-target final-release guard at {:x}; "
        "legitimate pool removals remain unchanged",
        final_release);
}
}

bool everspace2_set_dedicated_ui_root(sdk::UObjectBase* object, bool rooted) {
    if (object == nullptr) {
        return false;
    }

    auto* object_array = sdk::FUObjectArray::get();
    auto* object_item = object_array != nullptr ? object_array->get_object(object->get_internal_index()) : nullptr;

    if (object_item == nullptr || object_item->get_object() != object) {
        return false;
    }

    constexpr uint32_t root_set_flag = 1u << 30;

    for (;;) {
        const auto current = object_item->get_flags();
        const auto desired = rooted ? current | root_set_flag : current & ~root_set_flag;

        if (current == desired || object_item->compare_exchange_flags(current, desired)) {
            return true;
        }
    }
}

void root_dedicated_ui_texture(sdk::UTexture* texture) {
    if (texture == nullptr) {
        return;
    }

    if (everspace2_is_current_game()) {
        if (!everspace2_set_dedicated_ui_root(texture, true)) {
            SPDLOG_ERROR("[Everspace2][UE5.5][SlateUI] Failed to root the persistent dedicated UI texture");
        }
        return;
    }

    texture->add_to_root();
}

void unroot_dedicated_ui_texture(sdk::UTexture* texture) {
    if (texture == nullptr) {
        return;
    }

    if (everspace2_is_current_game()) {
        everspace2_set_dedicated_ui_root(texture, false);
        return;
    }

    texture->remove_from_root();
}

bool pitpanic_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        // Cover both the demo and future/full Pit Panic executable names.
        return exe_path && exe_path->find(L"PitPanic") != std::wstring::npos;
    }();

    return result;
}

bool windrose_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"Windrose-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool windrose_contains_i(std::wstring_view value, std::wstring_view needle) {
    if (needle.empty()) {
        return true;
    }

    return std::search(
        value.begin(),
        value.end(),
        needle.begin(),
        needle.end(),
        [](wchar_t a, wchar_t b) {
            return std::towlower(a) == std::towlower(b);
        }) != value.end();
}

std::wstring windrose_object_full_name(void* object) {
    if (object == nullptr || IsBadReadPtr(object, sizeof(void*) * 4)) {
        return {};
    }

    try {
        return ((sdk::UObjectBase*)object)->get_full_name();
    } catch (...) {
        return {};
    }
}

bool windrose_hfsm_name_is_interesting(std::wstring_view name) {
    return windrose_contains_i(name, L"BP_HFSM_") ||
           windrose_contains_i(name, L"NegativeSpace") ||
           windrose_contains_i(name, L"UILayoutTemplate") ||
           windrose_contains_i(name, L"R5WidgetPool");
}

std::wstring windrose_hfsm_self_name(std::wstring_view full_name) {
    if (full_name.empty()) {
        return {};
    }

    const auto space = full_name.find(L' ');
    const auto class_name = full_name.substr(0, space == std::wstring_view::npos ? full_name.size() : space);
    std::wstring result{class_name};

    if (space != std::wstring_view::npos && space + 1 < full_name.size()) {
        auto object_path = full_name.substr(space + 1);
        const auto leaf_start = object_path.find_last_of(L".:");
        const auto leaf = object_path.substr(leaf_start == std::wstring_view::npos ? 0 : leaf_start + 1);

        if (!leaf.empty() && leaf != class_name) {
            result += L" ";
            result += leaf;
        }
    }

    return result;
}

enum class WindroseMetaUiClass {
    Ignored,
    Transient,
    HardMenu,
};

const char* windrose_meta_ui_class_label(WindroseMetaUiClass value) {
    switch (value) {
    case WindroseMetaUiClass::HardMenu:
        return "hard_menu";
    case WindroseMetaUiClass::Transient:
        return "transient";
    default:
        return "ignored";
    }
}

WindroseMetaUiClass windrose_classify_hfsm_meta_ui(std::wstring_view self_name) {
    if (self_name.empty()) {
        return WindroseMetaUiClass::Ignored;
    }

    if (windrose_contains_i(self_name, L"BP_HFSM_FullscreenMap") ||
        windrose_contains_i(self_name, L"BP_FullscreenMap"))
    {
        return WindroseMetaUiClass::Ignored;
    }

    static constexpr std::wstring_view hard_menu_targets[] = {
        L"BP_HFSM_InventoryAndEquipment",
        L"BP_HFSM_Discovery",
        L"BP_HFSM_Progression",
        L"BP_HFSM_Talents",
        L"BP_HFSM_PlayerFlagShip",
        L"BP_HFSM_Rarities",
        L"BP_HFSM_ShipInventory",
        L"BP_HFSM_ShipManager",
        L"BP_HFSM_ShipDock",
        L"BP_HFSM_LootStorage",
        L"BP_HFSM_WaterLootStorage",
        L"BP_HFSM_PosthumousContainer",
        L"BP_HFSM_Storage",
        L"BP_HFSM_Craft_",
        L"BP_CraftUIMounter_",
    };

    for (const auto target : hard_menu_targets) {
        if (windrose_contains_i(self_name, target)) {
            return WindroseMetaUiClass::HardMenu;
        }
    }

    static constexpr std::wstring_view transient_targets[] = {
        L"BP_HFSM_MetaUI",
        L"BP_HFSM_MetaUIBuffer",
        L"BP_HFSM_Adventure",
        L"BP_HFSM_ShipInteraction",
        L"BP_HFSM_MetaInteraction",
        L"BP_NPC_ViewAll_SC",
        L"WBP_NPCView_Screen",
        L"WBP_NPCAssignment_",
        L"Cutscene",
        L"Cinematic",
        L"Dialogue",
        L"Dialog",
        L"BP_HFSM_OverlayShow",
    };

    for (const auto target : transient_targets) {
        if (windrose_contains_i(self_name, target)) {
            return WindroseMetaUiClass::Transient;
        }
    }

    return WindroseMetaUiClass::Ignored;
}

void windrose_note_hfsm_transition(void* object, bool entering, const char* source) {
    if (!windrose_is_current_game()) {
        return;
    }

    const auto name = windrose_object_full_name(object);
    if (name.empty()) {
        return;
    }

    const auto self_name = windrose_hfsm_self_name(name);
    const bool interesting = windrose_hfsm_name_is_interesting(name);
    const auto meta_ui_class = windrose_classify_hfsm_meta_ui(self_name);

    if (interesting || meta_ui_class != WindroseMetaUiClass::Ignored) {
        SPDLOG_INFO(
            "[Windrose][HFSM] {} {} class={} self={} object={}",
            source != nullptr ? source : "unknown",
            entering ? "enter" : "exit",
            windrose_meta_ui_class_label(meta_ui_class),
            utility::narrow(self_name),
            utility::narrow(name));
    }

    if (meta_ui_class == WindroseMetaUiClass::Ignored) {
        return;
    }

    auto& vr = VR::get();
    if (vr != nullptr) {
        vr->set_windrose_meta_ui_2d_state_active(
            utility::narrow(self_name.empty() ? name : self_name),
            reinterpret_cast<uintptr_t>(object),
            source != nullptr ? source : "unknown",
            meta_ui_class == WindroseMetaUiClass::HardMenu,
            entering);
    }
}

std::optional<uintptr_t> windrose_resolve_hfsm_symbol(
    const char* label,
    uintptr_t expected_rva,
    const char* pattern)
{
    const auto module = utility::get_executable();
    const auto scanned = utility::scan(module, pattern);

    if (scanned) {
        SPDLOG_INFO("[Windrose][HFSM] Resolved {} by signature at {:x} (rva {:x})", label, *scanned, *scanned - (uintptr_t)module);
        return scanned;
    }

    SPDLOG_WARN("[Windrose][HFSM] Failed to resolve {} by update-proof signature (expected old RVA {:x}, not hooking stale RVA)", label, expected_rva);
    return std::nullopt;
}

void avowed_native_fix_gate_reset(const char* reason) {
    if (!avowed_is_current_game()) {
        return;
    }

    std::scoped_lock _{g_avowed_native_fix_gate_mutex};

    if (g_avowed_native_fix_gate.has_baseline || g_avowed_native_fix_gate.ready || g_avowed_native_fix_gate.stable_frames != 0) {
        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[Avowed][NativeStereoFix] Resetting render transition gate: {}",
            reason != nullptr ? reason : "<unknown>");
    }

    g_avowed_native_fix_gate = {};
}

uintptr_t avowed_try_get_native_resource(FRHITexture2D* texture) {
    if (!avowed_is_current_game() || texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return 0;
    }

    try {
        const auto native = texture->get_native_resource();

        if (native == nullptr || IsBadReadPtr(native, sizeof(void*))) {
            return 0;
        }

        return (uintptr_t)native;
    } catch (...) {
        return 0;
    }
}

bool avowed_native_fix_gate_update(
    uintptr_t scene,
    uintptr_t render_target,
    uintptr_t scene_capture_render_target,
    uintptr_t scene_capture_native,
    bool prerequisites_ready,
    uint32_t* out_stable_frames = nullptr,
    uint32_t* out_required_stable_frames = nullptr)
{
    if (!avowed_is_current_game()) {
        return true;
    }

    std::scoped_lock _{g_avowed_native_fix_gate_mutex};

    if (out_stable_frames != nullptr) {
        *out_stable_frames = g_avowed_native_fix_gate.stable_frames;
    }

    if (out_required_stable_frames != nullptr) {
        *out_required_stable_frames = g_avowed_native_fix_gate.required_stable_frames != 0
            ? g_avowed_native_fix_gate.required_stable_frames
            : AVOWED_NATIVE_FIX_STABLE_FRAMES;
    }

    const auto now = std::chrono::steady_clock::now();

    if (g_avowed_native_fix_gate.last_update.time_since_epoch().count() != 0) {
        const auto render_gap = now - g_avowed_native_fix_gate.last_update;

        if (render_gap > AVOWED_NATIVE_FIX_RENDER_GAP) {
            const auto can_fast_reacquire = g_avowed_native_fix_gate.had_ready_baseline || g_avowed_native_fix_gate.ready;
            g_avowed_native_fix_gate.ready = false;
            g_avowed_native_fix_gate.stable_frames = 0;
            g_avowed_native_fix_gate.required_stable_frames =
                can_fast_reacquire ? AVOWED_NATIVE_FIX_FAST_REACQUIRE_STABLE_FRAMES : AVOWED_NATIVE_FIX_STABLE_FRAMES;
            g_avowed_native_fix_gate.fast_reacquire = can_fast_reacquire;
            const auto requested_hold_duration = can_fast_reacquire
                ? std::chrono::steady_clock::duration{AVOWED_NATIVE_FIX_FAST_REACQUIRE_HOLD}
                : std::chrono::steady_clock::duration{AVOWED_NATIVE_FIX_TRANSITION_HOLD};
            g_avowed_native_fix_gate.hold_until = std::max(
                g_avowed_native_fix_gate.hold_until,
                now + requested_hold_duration);

            SPDLOG_INFO_EVERY_N_SEC(
                2,
                "[Avowed][NativeStereoFix] Render gap {:.0f}ms detected; holding one view for transition safety fast_reacquire={}",
                std::chrono::duration<double, std::milli>(render_gap).count(),
                can_fast_reacquire);
        }
    }

    g_avowed_native_fix_gate.last_update = now;

    if (!prerequisites_ready || scene == 0 || render_target == 0 || scene_capture_render_target == 0) {
        if (g_avowed_native_fix_gate.has_baseline || g_avowed_native_fix_gate.ready || g_avowed_native_fix_gate.stable_frames != 0) {
            SPDLOG_INFO_EVERY_N_SEC(
                2,
                "[Avowed][NativeStereoFix] Holding one view while render targets are unavailable scene={:x} target={:x} capture_rt={:x} capture_native={:x} prereqs={}",
                scene,
                render_target,
                scene_capture_render_target,
                scene_capture_native,
                prerequisites_ready);
        }

        if (g_avowed_native_fix_gate.had_ready_baseline || g_avowed_native_fix_gate.ready) {
            if (!g_avowed_native_fix_gate.targets_missing) {
                g_avowed_native_fix_gate.missing_since = now;
            }

            // Inventory/menu transitions briefly remove Avowed's capture target. Keep the last
            // known-good gameplay baseline so reacquiring the same path can use a short gate.
            g_avowed_native_fix_gate.targets_missing = true;
            g_avowed_native_fix_gate.ready = false;
            g_avowed_native_fix_gate.stable_frames = 0;
            g_avowed_native_fix_gate.fast_reacquire = false;
            g_avowed_native_fix_gate.required_stable_frames = AVOWED_NATIVE_FIX_STABLE_FRAMES;
        } else {
            g_avowed_native_fix_gate = {};
        }

        if (out_stable_frames != nullptr) {
            *out_stable_frames = g_avowed_native_fix_gate.stable_frames;
        }

        if (out_required_stable_frames != nullptr) {
            *out_required_stable_frames = g_avowed_native_fix_gate.required_stable_frames != 0
                ? g_avowed_native_fix_gate.required_stable_frames
                : AVOWED_NATIVE_FIX_STABLE_FRAMES;
        }

        return false;
    }

    const auto baseline_changed =
        !g_avowed_native_fix_gate.has_baseline ||
        g_avowed_native_fix_gate.scene != scene ||
        g_avowed_native_fix_gate.render_target != render_target ||
        g_avowed_native_fix_gate.scene_capture_render_target != scene_capture_render_target ||
        g_avowed_native_fix_gate.scene_capture_native != scene_capture_native;

    if (baseline_changed) {
        const auto missing_duration = g_avowed_native_fix_gate.missing_since.time_since_epoch().count() != 0
            ? now - g_avowed_native_fix_gate.missing_since
            : std::chrono::steady_clock::duration{};
        const auto matches_last_ready_path =
            g_avowed_native_fix_gate.had_ready_baseline &&
            g_avowed_native_fix_gate.last_ready_scene == scene &&
            g_avowed_native_fix_gate.last_ready_render_target == render_target;
        const auto can_fast_reacquire =
            g_avowed_native_fix_gate.targets_missing &&
            matches_last_ready_path &&
            missing_duration <= AVOWED_NATIVE_FIX_FAST_REACQUIRE_MAX_MISSING;

        g_avowed_native_fix_gate.scene = scene;
        g_avowed_native_fix_gate.render_target = render_target;
        g_avowed_native_fix_gate.scene_capture_render_target = scene_capture_render_target;
        g_avowed_native_fix_gate.scene_capture_native = scene_capture_native;
        const auto requested_hold_duration = can_fast_reacquire
            ? std::chrono::steady_clock::duration{AVOWED_NATIVE_FIX_FAST_REACQUIRE_HOLD}
            : std::chrono::steady_clock::duration{AVOWED_NATIVE_FIX_TRANSITION_HOLD};
        const auto requested_hold_until = now + requested_hold_duration;
        g_avowed_native_fix_gate.hold_until = can_fast_reacquire
            ? requested_hold_until
            : std::max(g_avowed_native_fix_gate.hold_until, requested_hold_until);
        g_avowed_native_fix_gate.stable_frames = 0;
        g_avowed_native_fix_gate.required_stable_frames =
            can_fast_reacquire ? AVOWED_NATIVE_FIX_FAST_REACQUIRE_STABLE_FRAMES : AVOWED_NATIVE_FIX_STABLE_FRAMES;
        g_avowed_native_fix_gate.ready = false;
        g_avowed_native_fix_gate.has_baseline = true;
        g_avowed_native_fix_gate.targets_missing = false;
        g_avowed_native_fix_gate.missing_since = {};
        g_avowed_native_fix_gate.fast_reacquire = can_fast_reacquire;

        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[Avowed][NativeStereoFix] Render transition detected; holding one view scene={:x} target={:x} capture_rt={:x} capture_native={:x} fast_reacquire={} stable_required={}",
            scene,
            render_target,
            scene_capture_render_target,
            scene_capture_native,
            can_fast_reacquire,
            g_avowed_native_fix_gate.required_stable_frames);

        if (out_required_stable_frames != nullptr) {
            *out_required_stable_frames = g_avowed_native_fix_gate.required_stable_frames;
        }

        return false;
    }

    if (g_avowed_native_fix_gate.targets_missing) {
        const auto missing_duration = g_avowed_native_fix_gate.missing_since.time_since_epoch().count() != 0
            ? now - g_avowed_native_fix_gate.missing_since
            : std::chrono::steady_clock::duration{};
        const auto matches_last_ready_path =
            g_avowed_native_fix_gate.had_ready_baseline &&
            g_avowed_native_fix_gate.last_ready_scene == scene &&
            g_avowed_native_fix_gate.last_ready_render_target == render_target;
        const auto can_fast_reacquire =
            matches_last_ready_path &&
            missing_duration <= AVOWED_NATIVE_FIX_FAST_REACQUIRE_MAX_MISSING;

        const auto requested_hold_duration = can_fast_reacquire
            ? std::chrono::steady_clock::duration{AVOWED_NATIVE_FIX_FAST_REACQUIRE_HOLD}
            : std::chrono::steady_clock::duration{AVOWED_NATIVE_FIX_TRANSITION_HOLD};
        const auto requested_hold_until = now + requested_hold_duration;
        g_avowed_native_fix_gate.hold_until = can_fast_reacquire
            ? requested_hold_until
            : std::max(g_avowed_native_fix_gate.hold_until, requested_hold_until);
        g_avowed_native_fix_gate.ready = false;
        g_avowed_native_fix_gate.stable_frames = 0;
        g_avowed_native_fix_gate.required_stable_frames =
            can_fast_reacquire ? AVOWED_NATIVE_FIX_FAST_REACQUIRE_STABLE_FRAMES : AVOWED_NATIVE_FIX_STABLE_FRAMES;
        g_avowed_native_fix_gate.targets_missing = false;
        g_avowed_native_fix_gate.missing_since = {};
        g_avowed_native_fix_gate.fast_reacquire = can_fast_reacquire;

        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[Avowed][NativeStereoFix] Render targets reacquired on existing baseline fast_reacquire={} stable_required={}",
            can_fast_reacquire,
            g_avowed_native_fix_gate.required_stable_frames);
    }

    if (g_avowed_native_fix_gate.hold_until > now) {
        if (out_stable_frames != nullptr) {
            *out_stable_frames = 0;
        }

        if (out_required_stable_frames != nullptr) {
            *out_required_stable_frames = g_avowed_native_fix_gate.required_stable_frames;
        }

        g_avowed_native_fix_gate.ready = false;
        g_avowed_native_fix_gate.stable_frames = 0;

        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[Avowed][NativeStereoFix] Holding one view during transition grace window remaining={:.1f}s",
            std::chrono::duration<double>(g_avowed_native_fix_gate.hold_until - now).count());

        return false;
    }

    if (!g_avowed_native_fix_gate.ready) {
        const auto required_stable_frames = g_avowed_native_fix_gate.required_stable_frames != 0
            ? g_avowed_native_fix_gate.required_stable_frames
            : AVOWED_NATIVE_FIX_STABLE_FRAMES;

        if (g_avowed_native_fix_gate.stable_frames < required_stable_frames) {
            ++g_avowed_native_fix_gate.stable_frames;
        }

        if (out_stable_frames != nullptr) {
            *out_stable_frames = g_avowed_native_fix_gate.stable_frames;
        }

        if (out_required_stable_frames != nullptr) {
            *out_required_stable_frames = required_stable_frames;
        }

        if (g_avowed_native_fix_gate.stable_frames >= required_stable_frames) {
            g_avowed_native_fix_gate.ready = true;
            g_avowed_native_fix_gate.had_ready_baseline = true;
            g_avowed_native_fix_gate.last_ready_scene = scene;
            g_avowed_native_fix_gate.last_ready_render_target = render_target;
            SPDLOG_INFO(
                "[Avowed][NativeStereoFix] Render transition stabilized after {} frames; enabling two-view native fix fast_reacquire={}",
                g_avowed_native_fix_gate.stable_frames,
                g_avowed_native_fix_gate.fast_reacquire);
        }
    }

    return g_avowed_native_fix_gate.ready;
}

bool avowed_native_fix_gate_ready(uint32_t* out_stable_frames = nullptr, uint32_t* out_required_stable_frames = nullptr) {
    if (!avowed_is_current_game()) {
        return true;
    }

    std::scoped_lock _{g_avowed_native_fix_gate_mutex};

    if (out_stable_frames != nullptr) {
        *out_stable_frames = g_avowed_native_fix_gate.stable_frames;
    }

    if (out_required_stable_frames != nullptr) {
        *out_required_stable_frames = g_avowed_native_fix_gate.required_stable_frames != 0
            ? g_avowed_native_fix_gate.required_stable_frames
            : AVOWED_NATIVE_FIX_STABLE_FRAMES;
    }

    return g_avowed_native_fix_gate.ready;
}

bool is_ue_5_7_or_newer() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = resolved_engine_version_string();

    if (str_version != "0.00") {
        if (str_version.starts_with("5.7") || str_version.starts_with("5.8") || str_version.starts_with("5.9")) {
            return true;
        }
    }

    return disk_version.dwFileVersionMS >= 0x50007;
}

bool is_ue_4_27_runtime() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = resolved_engine_version_string();

    if (str_version != "0.00") {
        return str_version.starts_with("4.27");
    }

    return HIWORD(disk_version.dwFileVersionMS) == 4 && LOWORD(disk_version.dwFileVersionMS) == 27;
}

bool is_ue_4_26_runtime() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));

    if (str_version != "0.00") {
        return str_version.starts_with("4.26");
    }

    return HIWORD(disk_version.dwFileVersionMS) == 4 && LOWORD(disk_version.dwFileVersionMS) == 26;
}

bool is_ue_4_25_runtime() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));

    if (str_version != "0.00") {
        return str_version.starts_with("4.25");
    }

    return HIWORD(disk_version.dwFileVersionMS) == 4 && LOWORD(disk_version.dwFileVersionMS) == 25;
}

bool is_ue_4_16_runtime() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = resolved_engine_version_string();

    if (str_version != "0.00") {
        return str_version.starts_with("4.16");
    }

    return HIWORD(disk_version.dwFileVersionMS) == 4 && LOWORD(disk_version.dwFileVersionMS) == 16;
}

bool naruto_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());

        if (!exe_path) {
            return false;
        }

        auto lowered = *exe_path;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });

        return lowered.find(L"naruto-win64-shipping") != std::wstring::npos;
    }();

    return result;
}

bool prospi_is_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());

        if (!exe_path) {
            return false;
        }

        auto lowered = *exe_path;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });

        return lowered.find(L"prospi-win64-shipping") != std::wstring::npos;
    }();

    return result;
}

bool is_ue_5_8_or_newer() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = resolved_engine_version_string();

    if (str_version != "0.00") {
        if (str_version.starts_with("5.8") || str_version.starts_with("5.9")) {
            return true;
        }
    }

    return disk_version.dwFileVersionMS >= 0x50008;
}

bool is_ue_5_8() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = resolved_engine_version_string();

    if (str_version != "0.00") {
        return str_version.starts_with("5.8");
    }

    return disk_version.dwFileVersionMS == 0x50008;
}


bool is_ue_5_6_or_newer() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = resolved_engine_version_string();

    if (str_version != "0.00") {
        if (str_version.starts_with("5.6") || str_version.starts_with("5.7") || str_version.starts_with("5.8") || str_version.starts_with("5.9")) {
            return true;
        }
    }

    return disk_version.dwFileVersionMS >= 0x50006;
}

bool is_ue_5_1_dx12_backend() {
    if (g_framework == nullptr || !g_framework->is_dx12()) {
        return false;
    }

    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = resolved_engine_version_string();

    if (str_version != "0.00") {
        return str_version.starts_with("5.1");
    }

    return disk_version.dwFileVersionMS >= 0x50001 && disk_version.dwFileVersionMS < 0x50002;
}

bool is_ue_5_1_dx_backend() {
    if (g_framework == nullptr || (!g_framework->is_dx12() && !g_framework->is_dx11())) {
        return false;
    }

    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = resolved_engine_version_string();

    if (str_version != "0.00") {
        return str_version.starts_with("5.1");
    }

    return disk_version.dwFileVersionMS >= 0x50001 && disk_version.dwFileVersionMS < 0x50002;
}

bool is_ue_5_2_dx_backend() {
    if (g_framework == nullptr || (!g_framework->is_dx12() && !g_framework->is_dx11())) {
        return false;
    }

    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = resolved_engine_version_string();

    if (str_version != "0.00") {
        return str_version.starts_with("5.2");
    }

    return disk_version.dwFileVersionMS >= 0x50002 && disk_version.dwFileVersionMS < 0x50003;
}

bool is_ue_5_3_dx_backend() {
    if (g_framework == nullptr || (!g_framework->is_dx12() && !g_framework->is_dx11())) {
        return false;
    }

    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = resolved_engine_version_string();

    if (str_version != "0.00") {
        return str_version.starts_with("5.3");
    }

    return disk_version.dwFileVersionMS >= 0x50003 && disk_version.dwFileVersionMS < 0x50004;
}

bool is_ue_5_0_to_5_3_runtime() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = resolved_engine_version_string();

    if (str_version != "0.00") {
        return str_version.starts_with("5.0") ||
            str_version.starts_with("5.1") ||
            str_version.starts_with("5.2") ||
            str_version.starts_with("5.3");
    }

    return disk_version.dwFileVersionMS >= 0x50000 && disk_version.dwFileVersionMS < 0x50004;
}

bool is_ue_5_4_runtime() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = resolved_engine_version_string();

    if (str_version != "0.00") {
        return str_version.starts_with("5.4");
    }

    return disk_version.dwFileVersionMS >= 0x50004 && disk_version.dwFileVersionMS < 0x50005;
}

bool is_ue_5_4_dx_backend() {
    if (g_framework == nullptr || (!g_framework->is_dx12() && !g_framework->is_dx11())) {
        return false;
    }

    return is_ue_5_4_runtime();
}

bool is_ue_5_5_runtime() {
    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = resolved_engine_version_string();

    if (str_version != "0.00") {
        return str_version.starts_with("5.5");
    }

    return disk_version.dwFileVersionMS >= 0x50005 && disk_version.dwFileVersionMS < 0x50006;
}

bool is_ue_5_5_dx_backend() {
    if (g_framework == nullptr || (!g_framework->is_dx12() && !g_framework->is_dx11())) {
        return false;
    }

    return is_ue_5_5_runtime();
}

bool is_ue_5_5_dx12_backend() {
    if (g_framework == nullptr || !g_framework->is_dx12()) {
        return false;
    }

    return is_ue_5_5_runtime();
}

bool is_ue_5_6_dx12_backend() {
    if (g_framework == nullptr || !g_framework->is_dx12()) {
        return false;
    }

    static const auto disk_version = sdk::get_file_version_info();
    static const auto str_version = resolved_engine_version_string();

    if (str_version != "0.00") {
        return str_version.starts_with("5.6");
    }

    return disk_version.dwFileVersionMS >= 0x50006 && disk_version.dwFileVersionMS < 0x50007;
}

bool ue56_dx12_try_get_native_resource(FRHITexture2D* texture, const char* source, ID3D12Resource** out_native = nullptr, D3D12_RESOURCE_DESC* out_desc = nullptr) {
    if (out_native != nullptr) {
        *out_native = nullptr;
    }

    if (out_desc != nullptr) {
        *out_desc = {};
    }

    if (!is_ue_5_6_dx12_backend()) {
        return true;
    }

    if (texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.6][RT] {} candidate is not readable yet: tex={:x}",
            source != nullptr ? source : "<unknown>", (uintptr_t)texture);
        return false;
    }

    void* vtable = nullptr;

    try {
        vtable = *(void**)texture;
    } catch (...) {
        return false;
    }

    if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*))) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.6][RT] {} candidate has no readable vtable: tex={:x} vtable={:x}",
            source != nullptr ? source : "<unknown>", (uintptr_t)texture, (uintptr_t)vtable);
        return false;
    }

    {
        std::scoped_lock _{g_ue56_rt_probe_mutex};
        if (const auto it = g_ue56_native_resource_probe_cache.find((uintptr_t)vtable);
            it != g_ue56_native_resource_probe_cache.end() && !it->second)
        {
            return false;
        }
    }

    // UE 5.6 can expose Slate/viewport FRHITexture candidates whose native-resource
    // vtable discovery executes unsafe render-thread thunks. Do not probe them from
    // the fallback path; let the D3D12 backbuffer/texture hooks discover the scene.
    SPDLOG_WARNING_EVERY_N_SEC(2, "[UE5.6][RT] Refusing unsafe FRHITexture::GetNativeResource probing for {} candidate: tex={:x} vtable={:x}",
        source != nullptr ? source : "<unknown>", (uintptr_t)texture, (uintptr_t)vtable);
    {
        std::scoped_lock _{g_ue56_rt_probe_mutex};
        g_ue56_native_resource_probe_cache[(uintptr_t)vtable] = false;
    }
    return false;
}

bool is_ue57_dx11_backend() {
    return is_ue_5_7_or_newer() && g_framework != nullptr && g_framework->is_dx11();
}

bool is_ue58_dx11_dedicated_ui_backend() {
    return is_ue_5_8() && g_framework != nullptr && g_framework->is_dx11();
}

bool is_ue58_dx12_backend() {
    return is_ue_5_8() && g_framework != nullptr && g_framework->is_dx12();
}

bool supports_bimbo_ue58_dx12_owned_ui_target() {
    return bimbo_paradise_is_current_game() &&
        is_ue58_dx12_backend();
}

bool supports_naruto_ue416_dedicated_ui_target() {
    return naruto_is_current_game() &&
        is_ue_4_16_runtime() &&
        g_framework != nullptr &&
        g_framework->is_dx11();
}

bool supports_dead_island_2_ue425_dedicated_ui_target() {
    return dead_island_2_ue425_is_current_game() &&
        g_framework != nullptr &&
        g_framework->is_dx12();
}

std::optional<std::pair<uint32_t, uint32_t>> get_dead_island_2_ue425_ui_extent() {
    if (!supports_dead_island_2_ue425_dedicated_ui_target()) {
        return std::nullopt;
    }

    const auto& d3d12_hook = g_framework->get_d3d12_hook();
    auto* const swapchain = d3d12_hook != nullptr ? d3d12_hook->get_swap_chain() : nullptr;

    if (swapchain == nullptr) {
        return std::nullopt;
    }

    DXGI_SWAP_CHAIN_DESC swapchain_desc{};
    uint32_t width{};
    uint32_t height{};

    if (SUCCEEDED(swapchain->GetDesc(&swapchain_desc))) {
        width = swapchain_desc.BufferDesc.Width;
        height = swapchain_desc.BufferDesc.Height;
    }

    if (width == 0 || height == 0) {
        Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer{};

        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) && backbuffer != nullptr) {
            const auto desc = backbuffer->GetDesc();
            width = static_cast<uint32_t>(desc.Width);
            height = desc.Height;
        }
    }

    if (width < 320 || height < 240 || width > 16384 || height > 16384) {
        return std::nullopt;
    }

    return std::pair{width, height};
}

bool supports_ue57_dedicated_ui_target() {
    if (!is_ue_5_7_or_newer() || g_framework == nullptr) {
        return false;
    }

    return g_framework->is_dx12() || g_framework->is_dx11();
}

bool supports_ue55_dedicated_ui_target_for_current_game() {
    // These UE5.5/5.6 titles expose a valid Slate UI texture but route Slate to
    // the wrong target, leaving the HUD clipped in the upper-left/left-eye path.
    // Keep this allowlisted and DX12-only until more games validate it.
    return (aphelion_is_current_game() ||
            ark_ascended_is_current_game() ||
            mechwarrior_clans_is_current_game() ||
            redemption_sin_eternal_is_current_game() ||
            everspace2_is_current_game() ||
            directive8020_is_current_game() ||
            everwind_is_current_game() ||
            is_deadzone_ue56_executable()) &&
        g_framework != nullptr &&
        g_framework->is_dx12() &&
        !is_ue_5_7_or_newer();
}

bool supports_dedicated_ui_target_for_current_game() {
    return supports_ue57_dedicated_ui_target() ||
        supports_ue55_dedicated_ui_target_for_current_game() ||
        supports_naruto_ue416_dedicated_ui_target() ||
        supports_dead_island_2_ue425_dedicated_ui_target();
}

bool should_preserve_promoted_ue55_slate_target() {
    // Once these games expose a validated engine-owned DrawWindow output, stop
    // the synthetic UObject request. Letting it time out can clear the promoted
    // FRHI target and start a second render-resource bootstrap while Slate and
    // D3D12 still reference the first target.
    return aphelion_is_current_game() ||
        mechwarrior_clans_is_current_game() ||
        everwind_is_current_game() ||
        is_deadzone_ue56_executable();
}

bool is_probable_ue57_dx11_texture_desc_prepare_function(uintptr_t fn) {
    if (fn == 0 || IsBadReadPtr((void*)fn, 0x80)) {
        return false;
    }

    try {
        // D3D11 UE 5.7 can put a descriptor prepare/copy helper before the real
        // create wrapper. Calling that helper with the create signature is unsafe.
        return utility::scan(fn, 0x100, "0F B6 42 32").has_value()
            && utility::scan(fn, 0x100, "48 83 C2 38").has_value()
            && (utility::scan(fn, 0x100, "0F 10 42 08").has_value() ||
                utility::scan(fn, 0x100, "48 8B 02").has_value() ||
                utility::scan(fn, 0x100, "48 8B 42 24").has_value());
    } catch (...) {
        return false;
    }
}

void log_engine_render_timing_if_needed() {
    if (!is_ue_5_7_or_newer()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (g_engine_render_last_log.time_since_epoch().count() == 0) {
        g_engine_render_last_log = now;
        return;
    }

    if (now - g_engine_render_last_log < ENGINE_RENDER_TIMING_LOG_INTERVAL) {
        return;
    }

    if (g_begin_render_viewfamily_real_timing.count == 0 &&
        g_begin_render_viewfamily_timing.count == 0 &&
        g_prerender_viewfamily_rt_timing.count == 0)
    {
        g_engine_render_last_log = now;
        return;
    }

    auto vr = VR::get();
    const auto hmd_active = vr != nullptr && vr->is_hmd_active();
    const auto native_stereo = vr != nullptr && vr->is_native_stereo_fix_enabled();

    spdlog::info(
        "[UE57][engine-render-profiler] begin_render_viewfamily_real avg={:.2f}ms max={:.2f}ms n={} begin_render_viewfamily avg={:.2f}ms max={:.2f}ms n={} prerender_viewfamily_rt avg={:.2f}ms max={:.2f}ms n={} hmd={} native_stereo={}",
        g_begin_render_viewfamily_real_timing.avg(),
        g_begin_render_viewfamily_real_timing.max_ms,
        g_begin_render_viewfamily_real_timing.count,
        g_begin_render_viewfamily_timing.avg(),
        g_begin_render_viewfamily_timing.max_ms,
        g_begin_render_viewfamily_timing.count,
        g_prerender_viewfamily_rt_timing.avg(),
        g_prerender_viewfamily_rt_timing.max_ms,
        g_prerender_viewfamily_rt_timing.count,
        hmd_active,
        native_stereo
    );

    g_engine_render_last_log = now;
    g_begin_render_viewfamily_real_timing.reset();
    g_begin_render_viewfamily_timing.reset();
    g_prerender_viewfamily_rt_timing.reset();
}

bool should_profile_engine_render_timing() {
    const auto vr = VR::get();
    return is_ue_5_7_or_newer() && vr != nullptr && vr->is_hitch_diagnostics_enabled();
}

bool shf_is_valid_texture_with_vtable(FRHITexture2D* texture, void* required_vtable) {
    if (texture == nullptr || required_vtable == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return false;
    }

    void* vtable{};

    try {
        vtable = *(void**)texture;
    } catch (...) {
        return false;
    }

    if (vtable != required_vtable) {
        return false;
    }

    FRHITexture2D::set_vtable(vtable);
    return true;
}

std::optional<D3D12_RESOURCE_DESC> shf_try_get_d3d12_desc(FRHITexture2D* texture) {
    if (texture == nullptr) {
        return std::nullopt;
    }

    try {
        const auto native = (ID3D12Resource*)texture->get_native_resource();

        if (native == nullptr || IsBadReadPtr(native, sizeof(void*))) {
            return std::nullopt;
        }

        return native->GetDesc();
    } catch (...) {
        return std::nullopt;
    }
}

bool is_probable_d3d_native_resource(void* native) {
    if (native == nullptr || IsBadReadPtr(native, sizeof(void*))) {
        return false;
    }

    void* vtable{};

    try {
        vtable = *(void**)native;
    } catch (...) {
        return false;
    }

    if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*))) {
        return false;
    }

    const auto module = utility::get_module_within(vtable);
    if (!module) {
        return false;
    }

    const auto module_path = utility::get_module_path(*module);
    if (!module_path) {
        return false;
    }

    auto module_path_lower = std::string(*module_path);
    std::transform(module_path_lower.begin(), module_path_lower.end(), module_path_lower.begin(), ::tolower);
    return module_path_lower.ends_with("d3d12.dll") ||
        module_path_lower.ends_with("d3d12core.dll") ||
        module_path_lower.ends_with("dxgi.dll") ||
        module_path_lower.ends_with("d3d12sdklayers.dll");
}

bool ue58_try_get_d3d11_texture_desc(void* native, D3D11_TEXTURE2D_DESC& out_desc) {
    out_desc = {};

    if (native == nullptr || IsBadReadPtr(native, sizeof(void*))) {
        return false;
    }

    void* vtable{};

    try {
        vtable = *(void**)native;
    } catch (...) {
        return false;
    }

    if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*))) {
        return false;
    }

    const auto module = utility::get_module_within(vtable);
    if (!module) {
        return false;
    }

    const auto module_path = utility::get_module_path(*module);
    if (!module_path) {
        return false;
    }

    auto lower = std::string{*module_path};
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("d3d11") == std::string::npos && lower.find("dxgi") == std::string::npos) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture{};
    if (FAILED(reinterpret_cast<IUnknown*>(native)->QueryInterface(IID_PPV_ARGS(&texture))) || texture == nullptr) {
        return false;
    }

    texture->GetDesc(&out_desc);
    return out_desc.Width != 0 && out_desc.Height != 0;
}

bool naruto_ue416_try_get_d3d11_texture_desc(
    FRHITexture2D* texture,
    D3D11_TEXTURE2D_DESC& out_desc,
    ID3D11Resource** out_native = nullptr)
{
    out_desc = {};

    if (out_native != nullptr) {
        *out_native = nullptr;
    }

    if (!supports_naruto_ue416_dedicated_ui_target() ||
        texture == nullptr ||
        IsBadReadPtr(texture, sizeof(void*)))
    {
        return false;
    }

    try {
        auto* const vtable = *(void**)texture;
        if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*)) || !utility::get_module_within(vtable)) {
            return false;
        }

        FRHITexture2D::set_vtable(vtable);
        auto* const native = reinterpret_cast<ID3D11Resource*>(texture->get_native_resource());

        if (!ue58_try_get_d3d11_texture_desc(native, out_desc)) {
            return false;
        }

        if (out_native != nullptr) {
            *out_native = native;
        }

        return true;
    } catch (...) {
        return false;
    }
}

bool dune_try_get_registered_native_resource(
    void* resource,
    ID3D12Resource*& native,
    D3D12_RESOURCE_DESC& desc) {
    native = nullptr;
    desc = {};

    const auto get_native_address = g_dune_get_native_resource_fn.load(std::memory_order_acquire);
    if (resource == nullptr ||
        get_native_address == 0 ||
        !is_readable_process_range(reinterpret_cast<uintptr_t>(resource), 0xC0) ||
        !is_executable_process_range(get_native_address, 1)) {
        return false;
    }

    using GetNativeResourceFn = ID3D12Resource* (*)(void*);

    __try {
        native = reinterpret_cast<GetNativeResourceFn>(get_native_address)(resource);
        if (!is_probable_d3d_native_resource(native)) {
            native = nullptr;
            return false;
        }

        desc = native->GetDesc();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        native = nullptr;
        desc = {};
        return false;
    }

    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        desc.Width > 0 &&
        desc.Width <= 65536 &&
        desc.Height > 0 &&
        desc.Height <= 65536 &&
        desc.Format != DXGI_FORMAT_UNKNOWN;
}

void dune_record_registered_frame_resource(
    void* resource,
    uint64_t frame_id,
    ID3D12Resource* native,
    const D3D12_RESOURCE_DESC* desc) {
    if (!g_dune_final_output_probe_armed.load(std::memory_order_acquire)) {
        return;
    }

    std::scoped_lock lock{g_dune_final_output_trace.mutex};
    auto& state = g_dune_final_output_trace;
    if (!state.armed) {
        return;
    }

    ++state.register_calls;
    if (native == nullptr || desc == nullptr) {
        ++state.native_resource_failures;
    }

    // The FFX frame ID is monotonic. Finish records only after two newer IDs
    // have appeared so late resource registrations from the same frame remain
    // grouped without waiting on a CPU/GPU fence.
    for (auto& candidate : state.frames) {
        if (candidate.occupied &&
            frame_id > candidate.frame_id &&
            frame_id - candidate.frame_id >= 2) {
            dune_finalize_frame_record_locked(candidate);
        }
    }

    auto& frame = state.frames[frame_id % state.frames.size()];
    if (frame.occupied && frame.frame_id != frame_id) {
        dune_finalize_frame_record_locked(frame);
    }

    if (!frame.occupied) {
        frame.occupied = true;
        frame.frame_id = frame_id;
        frame.first_global_frame = g_frame_count;
    }
    frame.last_global_frame = g_frame_count;

    const auto newest_view_sequence = state.view_sequence;
    for (const auto& observation : state.views) {
        if (!observation.eligible ||
            observation.sequence == 0 ||
            newest_view_sequence - observation.sequence > 24) {
            continue;
        }

        const auto frame_distance =
            observation.global_frame > g_frame_count
                ? observation.global_frame - g_frame_count
                : g_frame_count - observation.global_frame;
        if (frame_distance > 2) {
            continue;
        }

        frame.eligible_views =
            static_cast<uint8_t>(std::min<uint32_t>(255, frame.eligible_views + 1));
        if (observation.stereo_pass < 32) {
            frame.stereo_pass_mask |= 1u << observation.stereo_pass;
        }
    }

    if (native != nullptr && desc != nullptr) {
        const auto native_address = reinterpret_cast<uintptr_t>(native);
        const auto already_present =
            std::find(
                frame.native_resources.begin(),
                frame.native_resources.begin() + frame.native_count,
                native_address) != frame.native_resources.begin() + frame.native_count;

        if (!already_present && frame.native_count < frame.native_resources.size()) {
            frame.native_resources[frame.native_count] = native_address;
            frame.native_descs[frame.native_count] = *desc;
            ++frame.native_count;
        }

        state.last_native_resource = native_address;
        state.last_native_desc = *desc;
    }
    state.last_frame_id = frame_id;

    const auto now = std::chrono::steady_clock::now();
    if (state.last_log.time_since_epoch().count() == 0 ||
        now - state.last_log >= std::chrono::seconds(2)) {
        state.last_log = now;
        SPDLOG_INFO(
            "[Dune][FinalOutput] verdict={} session={} add_pass={} eligible_views={} registrations={} "
            "native_failures={} ffx_frame={} frame_resources={} stereo_mask=0x{:x} "
            "last_native={:x} desc={}x{} fmt={} window={}/{} pair={} single={} ambiguous={} rhi={:x}",
            dune_final_output_verdict_name(
                g_dune_final_output_verdict.load(std::memory_order_acquire)),
            state.session,
            state.add_pass_calls,
            state.eligible_add_pass_calls,
            state.register_calls,
            state.native_resource_failures,
            frame_id,
            frame.native_count,
            frame.stereo_pass_mask,
            state.last_native_resource,
            state.last_native_desc.Width,
            state.last_native_desc.Height,
            static_cast<uint32_t>(state.last_native_desc.Format),
            state.outcome_count,
            state.outcomes.size(),
            state.pair_frames,
            state.single_frames,
            state.ambiguous_frames,
            reinterpret_cast<uintptr_t>(resource));
    }
}

void* call_get_native_resource_guarded(FRHITexture2D* texture, uintptr_t function) {
    __try {
        using GetNativeResourceFn = void* (*)(const FRHITexture2D*);
        return reinterpret_cast<GetNativeResourceFn>(function)(texture);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool get_d3d12_resource_desc_guarded(ID3D12Resource* resource, D3D12_RESOURCE_DESC& out) {
    __try {
        out = resource->GetDesc();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = {};
        return false;
    }
}

std::optional<uintptr_t> ue55_find_texture_desc_offset(FRHITexture2D* texture) {
    if (texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        return std::nullopt;
    }

    const auto texture_address = (uintptr_t)texture;
    constexpr std::array<uintptr_t, 3> desc_offsets{0x20, 0xe0, 0xf0};

    for (const auto desc_offset : desc_offsets) {
        if (desc_offset == 0xe0 &&
            !is_ue_5_6_dx12_backend() &&
            !is_ue58_dx12_backend())
        {
            continue;
        }

        if (texture_address + desc_offset < texture_address ||
            IsBadReadPtr((void*)(texture_address + desc_offset), 0x38)) {
            continue;
        }

        const auto extent_x = *(const int32_t*)(texture_address + desc_offset + 0x24);
        const auto extent_y = *(const int32_t*)(texture_address + desc_offset + 0x28);
        const auto num_mips = *(const uint8_t*)(texture_address + desc_offset + 0x30);
        const auto num_samples = *(const uint8_t*)(texture_address + desc_offset + 0x31);
        const auto dimension = *(const uint8_t*)(texture_address + desc_offset + 0x32);
        const auto format = *(const uint8_t*)(texture_address + desc_offset + 0x33);

        if (extent_x <= 0 || extent_y <= 0 || extent_x > 65536 || extent_y > 65536) {
            continue;
        }

        if (num_mips == 0 || num_mips > 32) {
            continue;
        }

        if (!(num_samples == 1 || num_samples == 2 || num_samples == 4 || num_samples == 8 || num_samples == 16)) {
            continue;
        }

        if (dimension > 8 || format == 0 || format > 128) {
            continue;
        }

        return desc_offset;
    }

    return std::nullopt;
}

bool ue55_dx12_try_get_native_resource_direct(
    FRHITexture2D* texture,
    const char* source,
    ID3D12Resource** out_native = nullptr,
    D3D12_RESOURCE_DESC* out_desc = nullptr)
{
    if (out_native != nullptr) {
        *out_native = nullptr;
    }

    if (out_desc != nullptr) {
        *out_desc = {};
    }

    if (texture == nullptr || IsBadReadPtr(texture, sizeof(void*))) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.5][SlateUI] {} candidate is not readable: tex={:x}",
            source != nullptr ? source : "<unknown>", (uintptr_t)texture);
        return false;
    }

    void** vtable{};

    try {
        vtable = *(void***)texture;
    } catch (...) {
        return false;
    }

    if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*) * 8)) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.5][SlateUI] {} candidate has no readable vtable: tex={:x} vtable={:x}",
            source != nullptr ? source : "<unknown>", (uintptr_t)texture, (uintptr_t)vtable);
        return false;
    }

    const auto desc_offset = ue55_find_texture_desc_offset(texture);
    if (!desc_offset) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[UE5.5][SlateUI] refusing {} candidate without a UE5.5/5.6 FRHITextureDesc: tex={:x}",
            source != nullptr ? source : "<unknown>", (uintptr_t)texture);
        return false;
    }

    std::array<size_t, 2> direct_slots{};
    size_t direct_slot_count = 2;

    if (*desc_offset == 0xe0) {
        // Confirmed from Redemption's UE5.6 FD3D12Texture vtable/PDB.
        direct_slots = {7ull, 0ull};
        direct_slot_count = 1;
    } else if (*desc_offset == 0xf0) {
        direct_slots = {5ull, 4ull};
    } else {
        direct_slots = {4ull, 5ull};
    }

    for (size_t slot_index = 0; slot_index < direct_slot_count; ++slot_index) {
        const auto slot = direct_slots[slot_index];
        const auto fn = reinterpret_cast<uintptr_t>(vtable[slot]);

        if (fn == 0 ||
            !is_executable_process_range(fn, 1) ||
            !utility::get_module_within(reinterpret_cast<void*>(fn)).has_value())
        {
            continue;
        }

        auto* native_raw = call_get_native_resource_guarded(texture, fn);

        if (!is_probable_d3d_native_resource(native_raw)) {
            continue;
        }

        auto* native = (ID3D12Resource*)native_raw;
        D3D12_RESOURCE_DESC desc{};

        if (!get_d3d12_resource_desc_guarded(native, desc)) {
            continue;
        }

        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.Width == 0 ||
            desc.Height == 0 ||
            desc.Width > 65536 ||
            desc.Height > 65536)
        {
            continue;
        }

        FRHITexture2D::set_vtable(vtable);

        if (out_native != nullptr) {
            *out_native = native;
        }

        if (out_desc != nullptr) {
            *out_desc = desc;
        }

        return true;
    }

    if (direct_slot_count == 1) {
        SPDLOG_INFO_EVERY_N_SEC(2,
            "[UE5.6][SlateUI] direct GetNativeResource slot {} did not produce a valid D3D12 texture for {} tex={:x}",
            direct_slots[0],
            source != nullptr ? source : "<unknown>",
            (uintptr_t)texture);
    } else {
        SPDLOG_INFO_EVERY_N_SEC(2,
            "[UE5.5][SlateUI] direct GetNativeResource slots {} and {} did not produce a valid D3D12 texture for {} tex={:x}",
            direct_slots[0],
            direct_slots[1],
            source != nullptr ? source : "<unknown>",
            (uintptr_t)texture);
    }

    return false;
}

std::optional<D3D12_RESOURCE_DESC> ue55_try_get_d3d12_desc(FRHITexture2D* texture, const char* source) {
    D3D12_RESOURCE_DESC desc{};

    if (!ue55_dx12_try_get_native_resource_direct(texture, source, nullptr, &desc)) {
        return std::nullopt;
    }

    return desc;
}

struct Everspace2ViewportTextureCandidate {
    FRHITexture2D* texture{};
    Microsoft::WRL::ComPtr<ID3D12Resource> native_resource{};
    D3D12_RESOURCE_DESC desc{};
    const char* source{};
};

std::optional<Everspace2ViewportTextureCandidate> everspace2_get_scene_viewport_texture(sdk::FViewport* viewport) {
    if (!everspace2_is_current_game() || !is_ue_5_5_dx12_backend() ||
        viewport == nullptr || IsBadReadPtr(viewport, 0x2F0))
    {
        return std::nullopt;
    }

    // ES2's shipped 5.5.4 PDB places FViewport at complete-object +0x8.
    // These offsets are therefore relative to the FViewport* passed to Draw.
    constexpr uintptr_t base_render_target_texture_offset = 0x08;
    constexpr uintptr_t buffered_render_targets_data_offset = 0x2B8;
    constexpr uintptr_t buffered_render_targets_count_offset = 0x2C0;
    constexpr uintptr_t render_thread_texture_offset = 0x2D8;
    constexpr uintptr_t buffered_render_targets_index_offset = 0x2E8;

    const auto viewport_address = (uintptr_t)viewport;
    const auto validate = [](FRHITexture2D* texture, const char* source)
        -> std::optional<Everspace2ViewportTextureCandidate>
    {
        ID3D12Resource* native_resource{};
        D3D12_RESOURCE_DESC desc{};

        if (!ue55_dx12_try_get_native_resource_direct(texture, source, &native_resource, &desc) ||
            native_resource == nullptr)
        {
            return std::nullopt;
        }

        if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
            SPDLOG_INFO_EVERY_N_SEC(
                2,
                "[Everspace2][ViewportRT] Rejected {} texture {:x}: not render-target capable flags=0x{:x}",
                source,
                (uintptr_t)texture,
                (uint32_t)desc.Flags);
            return std::nullopt;
        }

        return Everspace2ViewportTextureCandidate{
            .texture = texture,
            .native_resource = native_resource,
            .desc = desc,
            .source = source,
        };
    };

    const auto render_thread_texture =
        *(FRHITexture2D**)(viewport_address + render_thread_texture_offset);
    if (const auto candidate = validate(render_thread_texture, "FSceneViewport render-thread target")) {
        return candidate;
    }

    const auto base_render_target_texture =
        *(FRHITexture2D**)(viewport_address + base_render_target_texture_offset);
    if (const auto candidate = validate(base_render_target_texture, "FViewport render target")) {
        return candidate;
    }

    const auto count = *(const int32_t*)(viewport_address + buffered_render_targets_count_offset);
    const auto index = *(const int32_t*)(viewport_address + buffered_render_targets_index_offset);
    const auto data = *(FRHITexture2D***)(viewport_address + buffered_render_targets_data_offset);

    if (count <= 0 || count > 8 || index < 0 || index >= count ||
        data == nullptr || IsBadReadPtr(data, sizeof(FRHITexture2D*) * count))
    {
        return std::nullopt;
    }

    return validate(data[index], "FSceneViewport buffered target");
}

void shf_log_rtm_candidate(VRRenderTargetManager_Base* rtm, FRHITexture2D* texture, const char* source) {
    if (!shf_is_current_game() || !g_framework->is_dx12() || rtm == nullptr || texture == nullptr) {
        return;
    }

    ID3D12Resource* native = nullptr;
    std::optional<D3D12_RESOURCE_DESC> desc{};

    try {
        native = (ID3D12Resource*)texture->get_native_resource();

        if (native != nullptr && !IsBadReadPtr(native, sizeof(void*))) {
            desc = native->GetDesc();
        }
    } catch (...) {
    }

    bool log_unique = false;
    uint64_t seen = 0;
    uint64_t unique = 0;
    uint64_t suppressed = 0;

    {
        std::scoped_lock _{g_shf_texture_probe_mutex};
        ++g_shf_rtm_candidate_count;
        seen = g_shf_rtm_candidate_count;

        const auto key = (uintptr_t)(native != nullptr ? native : (ID3D12Resource*)texture);

        if (!g_shf_logged_rtm_candidate_natives.contains(key)) {
            g_shf_logged_rtm_candidate_natives.insert(key);
            log_unique = g_shf_logged_rtm_candidate_natives.size() <= 64;
        } else {
            ++g_shf_rtm_candidate_suppressed;
        }

        unique = g_shf_logged_rtm_candidate_natives.size();
        suppressed = g_shf_rtm_candidate_suppressed;
    }

    if (log_unique && desc) {
        SPDLOG_WARN("[SHf][RTM] accepting unique texture candidate #{} source={} tex={:x} native={:x} [{}x{} fmt={} flags=0x{:x}] current_rt={:x}",
            seen, source, (uintptr_t)texture, (uintptr_t)native, desc->Width, desc->Height, (uint32_t)desc->Format,
            (uint32_t)desc->Flags, (uintptr_t)rtm->get_render_target());
    } else if (log_unique) {
        SPDLOG_WARN("[SHf][RTM] accepting unique texture candidate #{} source={} tex={:x} native={:x} desc=<unavailable> current_rt={:x}",
            seen, source, (uintptr_t)texture, (uintptr_t)native, (uintptr_t)rtm->get_render_target());
    } else {
        SPDLOG_INFO_EVERY_N_SEC(2,
            "[SHf][RTM] texture candidate summary seen={} unique_natives={} duplicate_suppressed={} last_source={} last_tex={:x} last_native={:x} current_rt={:x}",
            seen, unique, suppressed, source, (uintptr_t)texture, (uintptr_t)native, (uintptr_t)rtm->get_render_target());
    }
}

bool shf_can_reuse_current_ui_target(VRRenderTargetManager_Base* rtm, uint32_t expected_width, uint32_t expected_height) {
    if (!shf_is_current_game() || !g_framework->is_dx12() || rtm == nullptr || expected_width == 0 || expected_height == 0) {
        return false;
    }

    auto* ui_target = rtm->get_ui_target();

    if (ui_target == nullptr || ui_target == rtm->get_render_target()) {
        return false;
    }

    const auto desc = shf_try_get_d3d12_desc(ui_target);

    if (!desc || desc->Width != expected_width || desc->Height != expected_height) {
        return false;
    }

    SPDLOG_INFO_EVERY_N_SEC(2,
        "[SHf] Reusing stable UI texture {:x} [{}x{} fmt={}]; skipping duplicate UI texture creation",
        (uintptr_t)ui_target, desc->Width, desc->Height, (uint32_t)desc->Format);

    return true;
}

void ue51_log_rt_churn_summary_locked(const char* reason) {
    const auto now = std::chrono::steady_clock::now();

    if (g_ue51_rt_churn.last_log.time_since_epoch().count() != 0 &&
        now - g_ue51_rt_churn.last_log < std::chrono::seconds(30))
    {
        return;
    }

    g_ue51_rt_churn.last_log = now;

    uint32_t current_width = 0;
    uint32_t current_height = 0;

    if (g_framework != nullptr) {
        const auto size = g_framework->get_d3d12_rt_size();
        current_width = (uint32_t)size.x;
        current_height = (uint32_t)size.y;
    }

    SPDLOG_INFO(
        "[UE5.1][RTChurn] summary reason={} alloc_seen={} ui_created={} ui_reused={} last_alloc={:x} last_create={:x} last_ui={:x} last_ui_size={}x{} current_rt_size={}x{}",
        reason != nullptr ? reason : "<unknown>",
        g_ue51_rt_churn.allocate_seen,
        g_ue51_rt_churn.ui_created,
        g_ue51_rt_churn.ui_reused,
        g_ue51_rt_churn.last_allocate_return_address,
        g_ue51_rt_churn.last_ui_create_return_address,
        g_ue51_rt_churn.last_ui_texture,
        g_ue51_rt_churn.last_ui_width,
        g_ue51_rt_churn.last_ui_height,
        current_width,
        current_height);
}

void ue51_note_rt_allocation(uintptr_t relative_return_address) {
    if (!is_ue_5_1_dx12_backend()) {
        return;
    }

    std::scoped_lock _{g_ue51_rt_churn_mutex};
    ++g_ue51_rt_churn.allocate_seen;
    g_ue51_rt_churn.last_allocate_return_address = relative_return_address;
    ue51_log_rt_churn_summary_locked("allocate");
}

void ue51_note_ui_created(FRHITexture2D* ui_texture, uint32_t width, uint32_t height) {
    if (!is_ue_5_1_dx12_backend() || ui_texture == nullptr) {
        return;
    }

    std::scoped_lock _{g_ue51_rt_churn_mutex};
    ++g_ue51_rt_churn.ui_created;
    g_ue51_rt_churn.last_ui_create_return_address = g_ue51_rt_churn.last_allocate_return_address;
    g_ue51_rt_churn.last_ui_texture = (uintptr_t)ui_texture;
    g_ue51_rt_churn.last_ui_width = width;
    g_ue51_rt_churn.last_ui_height = height;
    ue51_log_rt_churn_summary_locked("ui_created");
}

bool ue51_can_reuse_current_ui_target(VRRenderTargetManager_Base* rtm, uint32_t expected_width, uint32_t expected_height) {
    if (!is_ue_5_1_dx12_backend() || rtm == nullptr || expected_width == 0 || expected_height == 0) {
        return false;
    }

    const auto* ui_target = rtm->get_ui_target();

    if (ui_target == nullptr || ui_target == rtm->get_render_target()) {
        return false;
    }

    std::scoped_lock _{g_ue51_rt_churn_mutex};

    if (g_ue51_rt_churn.ui_created == 0 ||
        g_ue51_rt_churn.last_allocate_return_address == 0 ||
        g_ue51_rt_churn.last_allocate_return_address != g_ue51_rt_churn.last_ui_create_return_address ||
        g_ue51_rt_churn.last_ui_texture != (uintptr_t)ui_target ||
        g_ue51_rt_churn.last_ui_width != expected_width ||
        g_ue51_rt_churn.last_ui_height != expected_height)
    {
        return false;
    }

    ++g_ue51_rt_churn.ui_reused;
    ue51_log_rt_churn_summary_locked("ui_reused");

    return true;
}

void shf_log_texture_probe_candidate(
    const char* source,
    const char* base_name,
    uintptr_t base,
    uintptr_t offset,
    int32_t array_index,
    FRHITexture2D* texture,
    const D3D12_RESOURCE_DESC& desc)
{
    const auto key = ((uintptr_t)texture >> 4) ^
                     (base << 9) ^
                     (offset << 21) ^
                     ((uintptr_t)(array_index + 1) << 53);

    {
        std::scoped_lock _{g_shf_texture_probe_mutex};

        if (g_shf_logged_texture_probe_keys.contains(key)) {
            return;
        }

        g_shf_logged_texture_probe_keys.insert(key);
    }

    if (array_index >= 0) {
        SPDLOG_WARN("[SHf] FSceneViewport probe {} {}+0x{:x}[{}] -> tex {:x} [{}x{} fmt={} flags=0x{:x}]",
            source, base_name, offset, array_index, (uintptr_t)texture, desc.Width, desc.Height, (uint32_t)desc.Format, (uint32_t)desc.Flags);
    } else {
        SPDLOG_WARN("[SHf] FSceneViewport probe {} {}+0x{:x} -> tex {:x} [{}x{} fmt={} flags=0x{:x}]",
            source, base_name, offset, (uintptr_t)texture, desc.Width, desc.Height, (uint32_t)desc.Format, (uint32_t)desc.Flags);
    }
}

void shf_probe_scene_viewport_memory(sdk::FViewport* viewport, const char* source, FRHITexture2D* known_texture) {
    if (viewport == nullptr || IsBadReadPtr(viewport, sizeof(void*))) {
        return;
    }

    void* required_vtable = nullptr;

    if (known_texture != nullptr && !IsBadReadPtr(known_texture, sizeof(void*))) {
        required_vtable = *(void**)known_texture;
    }

    if (required_vtable == nullptr) {
        required_vtable = FRHITexture2D::get_vtable();
    }

    if (required_vtable == nullptr) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto viewport_base = (uintptr_t)viewport;

    {
        std::scoped_lock _{g_shf_texture_probe_mutex};
        auto& last_probe = g_shf_last_texture_probe_by_base[viewport_base];

        if (last_probe.time_since_epoch().count() != 0 && now - last_probe < std::chrono::seconds(2)) {
            return;
        }

        last_probe = now;
    }

    auto probe_base = [&](uintptr_t base, const char* base_name) {
        if (base == 0 || IsBadReadPtr((void*)base, 0x340)) {
            return;
        }

        for (uintptr_t offset = 0; offset <= 0x330; offset += sizeof(void*)) try {
            const auto field = base + offset;

            if (IsBadReadPtr((void*)field, sizeof(void*))) {
                continue;
            }

            const auto texture = *(FRHITexture2D**)field;

            if (shf_is_valid_texture_with_vtable(texture, required_vtable)) {
                if (const auto desc = shf_try_get_d3d12_desc(texture)) {
                    shf_log_texture_probe_candidate(source, base_name, base, offset, -1, texture, *desc);
                }
            }

            if (IsBadReadPtr((void*)field, sizeof(void*) + sizeof(int32_t) * 2)) {
                continue;
            }

            const auto array_data = *(FRHITexture2D***)field;
            const auto array_count = *(int32_t*)(field + sizeof(void*));
            const auto array_capacity = *(int32_t*)(field + sizeof(void*) + sizeof(int32_t));

            if (array_data == nullptr || array_count <= 0 || array_count > 8 || array_capacity < array_count ||
                IsBadReadPtr(array_data, sizeof(FRHITexture2D*) * array_count))
            {
                continue;
            }

            for (int32_t i = 0; i < array_count; ++i) {
                const auto array_texture = array_data[i];

                if (!shf_is_valid_texture_with_vtable(array_texture, required_vtable)) {
                    continue;
                }

                if (const auto desc = shf_try_get_d3d12_desc(array_texture)) {
                    shf_log_texture_probe_candidate(source, base_name, base, offset, i, array_texture, *desc);
                }
            }
        } catch (...) {
        }
    };

    probe_base(viewport_base, "FViewport");

    if (viewport_base > 0x1000) {
        probe_base(viewport_base - sizeof(void*), "FSceneViewport");
    }
}

void shf_force_scene_viewport_separate_rt(const sdk::FViewport& viewport, const char* source) {
    const bool should_force_scene_viewport_rt = shf_is_current_game() || dune_awakening_is_current_game();

    if (!should_force_scene_viewport_rt || g_framework == nullptr || !g_framework->is_game_data_intialized()) {
        return;
    }

    const auto vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active() || vr->is_stereo_emulation_enabled() || vr->is_extreme_compatibility_mode_enabled()) {
        return;
    }

    constexpr uintptr_t fviewport_base_offset = 0x08;
    constexpr uintptr_t b_use_separate_rt_full_offset = 0x287;
    constexpr uintptr_t b_force_separate_rt_full_offset = 0x288;
    constexpr uintptr_t b_use_separate_rt_fviewport_offset = b_use_separate_rt_full_offset - fviewport_base_offset;
    constexpr uintptr_t b_force_separate_rt_fviewport_offset = b_force_separate_rt_full_offset - fviewport_base_offset;

    const auto viewport_base = reinterpret_cast<uintptr_t>(&viewport);

    if (viewport_base <= fviewport_base_offset ||
        IsBadReadPtr(reinterpret_cast<void*>(viewport_base - fviewport_base_offset), sizeof(void*)) ||
        IsBadReadPtr(reinterpret_cast<void*>(viewport_base + b_force_separate_rt_fviewport_offset), sizeof(uint8_t)))
    {
        return;
    }

    const auto fscene_viewport_vtable = *reinterpret_cast<uintptr_t*>(viewport_base - fviewport_base_offset);

    if (fscene_viewport_vtable == 0 || !utility::get_module_within(fscene_viewport_vtable).has_value()) {
        return;
    }

    auto* use_separate_rt = reinterpret_cast<uint8_t*>(viewport_base + b_use_separate_rt_fviewport_offset);
    auto* force_separate_rt = reinterpret_cast<uint8_t*>(viewport_base + b_force_separate_rt_fviewport_offset);

    const bool is_dune = dune_awakening_is_current_game();
    const char* log_prefix = is_dune ? "[Dune][RT]" : "[SHf]";

    if (*use_separate_rt > 1 || *force_separate_rt > 1) {
        SPDLOG_WARN_ONCE("{} Refusing to force separate RT from {}; unexpected FSceneViewport bool bytes use={} force={}",
            log_prefix, source, *use_separate_rt, *force_separate_rt);
        return;
    }

    if (is_dune && dune_should_preserve_native_viewport_target()) {
        const bool was_separate = *use_separate_rt != 0 || *force_separate_rt != 0;
        *use_separate_rt = 0;
        *force_separate_rt = 0;

        if (was_separate) {
            SPDLOG_INFO_EVERY_N_SEC(
                2,
                "[Dune][CustomPresent] Restored Dune's native viewport target from {} mode={}",
                source,
                g_hook->is_dune_character_creation_active() ? "character_creation" : "gameplay");
        }

        return;
    }

    const bool was_missing_separate_rt = *use_separate_rt == 0 || *force_separate_rt == 0;

    if (was_missing_separate_rt) {
        SPDLOG_WARN_ONCE("{} Forcing FSceneViewport separate RT from {} at viewport {:x} use+0x{:x} force+0x{:x}",
            log_prefix, source, viewport_base, b_use_separate_rt_fviewport_offset, b_force_separate_rt_fviewport_offset);
    }

    *use_separate_rt = 1;
    *force_separate_rt = 1;

    if (is_dune && was_missing_separate_rt && g_hook != nullptr) {
        SPDLOG_WARN_ONCE("[Dune][RT] Requesting one viewport texture recreate after forcing separate RT");
        g_dune_force_viewport_rhi_once.store(true);
        g_hook->set_should_recreate_textures(true);
    }
}

constexpr auto UE57_SLATE_THREAD_PREFERENCE_CACHE_KEY = "ue57_prefer_slate_thread";
constexpr auto UE57_VIEW_EXTENSION_DISCOVERY_CACHE_KEY = "ue57_view_extension_discovery";

bool load_ue57_slate_thread_preference() {
    if (!is_ue_5_7_or_newer()) {
        return false;
    }

    if (const auto cached = sdk::discovery_cache::load_entry(UE57_SLATE_THREAD_PREFERENCE_CACHE_KEY, utility::get_executable())) {
        return cached->value("prefer_slate_thread", false);
    }

    return false;
}

void save_ue57_slate_thread_preference(bool prefer) {
    if (!is_ue_5_7_or_newer()) {
        return;
    }

    if (prefer) {
        sdk::discovery_cache::save_entry(UE57_SLATE_THREAD_PREFERENCE_CACHE_KEY, utility::get_executable(), {
            {"prefer_slate_thread", true}
        });
    } else {
        sdk::discovery_cache::invalidate_entry(UE57_SLATE_THREAD_PREFERENCE_CACHE_KEY);
    }
}

enum class UE57RenderTargetLoadAction : uint32_t {
    NoAction = 0,
    Load = 1,
    Clear = 2,
};

struct UE57SlateDrawElementsPassInputsHead {
    FRDGTexture* stencil_texture;
    FRDGTexture* elements_texture;
    FRDGTexture* scene_viewport_texture;
    UE57RenderTargetLoadAction elements_load_action;
};

bool looks_like_ue57_slate_draw_elements_inputs(const UE57SlateDrawElementsPassInputsHead* inputs) {
    if (inputs == nullptr || !is_readable_process_range((uintptr_t)inputs, sizeof(UE57SlateDrawElementsPassInputsHead))) {
        return false;
    }

    const auto action = static_cast<uint32_t>(inputs->elements_load_action);

    if (action > static_cast<uint32_t>(UE57RenderTargetLoadAction::Clear)) {
        return false;
    }

    const auto scene_viewport_texture = inputs->scene_viewport_texture;
    const auto elements_texture = inputs->elements_texture;

    if (scene_viewport_texture == nullptr || elements_texture == nullptr) {
        return false;
    }

    if (!is_readable_process_range((uintptr_t)scene_viewport_texture, sizeof(void*)) ||
        !is_readable_process_range((uintptr_t)elements_texture, sizeof(void*))) {
        return false;
    }

    return true;
}

using RegisterExternalTextureFromRHIFn = FRDGTexture* (*)(FRDGBuilder&, FRHITexture*, const wchar_t*);

bool looks_like_nontrivial_virtual(uintptr_t fn) {
    if (fn == 0 || IsBadReadPtr((void*)fn, 1) || !utility::get_module_within((void*)fn).has_value()) {
        return false;
    }

    size_t decoded_bytes = 0;
    size_t call_count = 0;
    bool saw_terminator = false;

    for (auto ip = (uint8_t*)fn; decoded_bytes < 512; ) {
        const auto decoded = utility::decode_one(ip);

        if (!decoded) {
            break;
        }

        decoded_bytes += decoded->Length;

        if (std::string_view{decoded->Mnemonic}.starts_with("CALL")) {
            ++call_count;
        }

        if (std::string_view{decoded->Mnemonic}.starts_with("RET") || std::string_view{decoded->Mnemonic}.starts_with("INT3")) {
            saw_terminator = true;
            break;
        }

        ip += decoded->Length;
    }

    return saw_terminator && decoded_bytes >= 64 && call_count >= 1;
}

bool looks_like_callable_virtual(uintptr_t fn) {
    if (fn == 0 || IsBadReadPtr((void*)fn, 1) || !utility::get_module_within((void*)fn).has_value()) {
        return false;
    }

    size_t decoded_bytes = 0;

    for (auto ip = (uint8_t*)fn; decoded_bytes < 256;) {
        const auto decoded = utility::decode_one(ip);

        if (!decoded || decoded->Length == 0) {
            break;
        }

        decoded_bytes += decoded->Length;

        const std::string_view mnemonic{decoded->Mnemonic};

        if (mnemonic.starts_with("RET")) {
            return decoded_bytes > 0;
        }

        if (mnemonic.starts_with("JMP")) {
            return decoded_bytes <= 16;
        }

        if (mnemonic.starts_with("INT3")) {
            return false;
        }

        ip += decoded->Length;
    }

    return false;
}

bool looks_like_post_init_properties_virtual(uintptr_t fn) {
    if (fn == 0 || IsBadReadPtr((void*)fn, 1) || !utility::get_module_within((void*)fn).has_value()) {
        return false;
    }

    size_t decoded_bytes = 0;
    size_t call_count = 0;

    for (auto ip = (uint8_t*)fn; decoded_bytes < 256;) {
        const auto decoded = utility::decode_one(ip);

        if (!decoded || decoded->Length == 0) {
            break;
        }

        decoded_bytes += decoded->Length;
        const std::string_view mnemonic{decoded->Mnemonic};

        if (mnemonic.starts_with("CALL")) {
            ++call_count;
        }

        if (mnemonic.starts_with("RET")) {
            return decoded_bytes > 8 || call_count > 0;
        }

        if (mnemonic.starts_with("JMP")) {
            return decoded_bytes > 8 || call_count > 0;
        }

        if (mnemonic.starts_with("INT3")) {
            return false;
        }

        ip += decoded->Length;
    }

    return call_count > 0;
}

std::optional<uint32_t> validate_source_informed_post_init_slot(
    uintptr_t* object_vtable,
    uintptr_t* localplayer_vtable,
    uint32_t slot,
    const char* source_note,
    bool require_inherited_uobject_slot,
    bool allow_callable_thunk = false)
{
    if (IsBadReadPtr(&object_vtable[slot], sizeof(uintptr_t)) ||
        IsBadReadPtr(&localplayer_vtable[slot], sizeof(uintptr_t)))
    {
        SPDLOG_WARN("[PostInitProperties] {} slot {} is not readable", source_note, slot);
        return std::nullopt;
    }

    const auto object_fn = object_vtable[slot];
    const auto localplayer_fn = localplayer_vtable[slot];

    // Shipping builds can fold UObject::PostInitProperties to a bare RET.
    // Accept that only at a source-verified slot; the broad fallback scan
    // below must continue requiring a non-trivial function body.
    const auto object_looks_valid = allow_callable_thunk
        ? looks_like_callable_virtual(object_fn)
        : looks_like_post_init_properties_virtual(object_fn);
    const auto localplayer_looks_valid = allow_callable_thunk
        ? looks_like_callable_virtual(localplayer_fn)
        : looks_like_post_init_properties_virtual(localplayer_fn);

    if (!object_looks_valid || !localplayer_looks_valid)
    {
        SPDLOG_WARN("[PostInitProperties] {} slot {} did not look callable object_fn={:x} localplayer_fn={:x}",
            source_note,
            slot,
            object_fn,
            localplayer_fn);
        return std::nullopt;
    }

    if (require_inherited_uobject_slot && object_fn != localplayer_fn) {
        SPDLOG_WARN("[PostInitProperties] {} slot {} did not inherit UObject function object_fn={:x} localplayer_fn={:x}",
            source_note,
            slot,
            object_fn,
            localplayer_fn);
        return std::nullopt;
    }

    SPDLOG_INFO("[PostInitProperties] Resolved {} slot {} object_fn={:x} localplayer_fn={:x}",
        source_note,
        slot,
        object_fn,
        localplayer_fn);
    return slot;
}

std::optional<uint32_t> resolve_post_init_properties_index_from_uobject(uintptr_t localplayer) {
    auto* object_class = sdk::UObject::static_class();

    if (object_class == nullptr) {
        SPDLOG_WARN("[PostInitProperties] UObject::static_class() is not ready");
        return std::nullopt;
    }

    auto* object_cdo = object_class->get_class_default_object<sdk::UObject>();

    if (object_cdo == nullptr || IsBadReadPtr(object_cdo, sizeof(void*))) {
        SPDLOG_WARN("[PostInitProperties] UObject CDO is not ready");
        return std::nullopt;
    }

    const auto object_vtable = *(uintptr_t**)object_cdo;
    const auto localplayer_vtable = *(uintptr_t**)localplayer;

    if (object_vtable == nullptr || localplayer_vtable == nullptr ||
        IsBadReadPtr(object_vtable, sizeof(void*)) || IsBadReadPtr(localplayer_vtable, sizeof(void*)))
    {
        SPDLOG_WARN("[PostInitProperties] UObject or LocalPlayer vtable is invalid");
        return std::nullopt;
    }

    // ProSpi 4.27.2 shipped layout validates at slot 8 in the live log/PDB path.
    // Do not force the modern UE5 slot here; if slot 8 is not provably callable,
    // fail closed instead of scanning broad/random LocalPlayer virtuals.
    if (is_ue_4_27_runtime() && prospi_is_current_game()) {
        constexpr uint32_t PROSPI_UE427_POST_INIT_PROPERTIES_SLOT = 8;

        if (validate_source_informed_post_init_slot(
                object_vtable,
                localplayer_vtable,
                PROSPI_UE427_POST_INIT_PROPERTIES_SLOT,
                "ProSpi UE4.27 UObject::PostInitProperties",
                false,
                true))
        {
            return PROSPI_UE427_POST_INIT_PROPERTIES_SLOT;
        }

        SPDLOG_WARN("[PostInitProperties] ProSpi UE4.27 slot 8 did not validate; skipping Ghosting Fix bootstrap for safety");
        return std::nullopt;
    }

    // UE4.25/4.25Plus and UE4.26 source place UObject::PostInitProperties at
    // slot 8. ULocalPlayer overrides it to size and allocate ViewStates from
    // GetDesiredNumberOfViews. Keep this source-informed path separate from
    // the changing UE5 layouts.
    if (is_ue_4_25_runtime() || is_ue_4_26_runtime()) {
        constexpr uint32_t UE425_426_POST_INIT_PROPERTIES_SLOT = 8;
        const auto runtime_label = is_ue_4_25_runtime() ? "UE4.25" : "UE4.26";

        if (validate_source_informed_post_init_slot(
                object_vtable,
                localplayer_vtable,
                UE425_426_POST_INIT_PROPERTIES_SLOT,
                runtime_label,
                false))
        {
            return UE425_426_POST_INIT_PROPERTIES_SLOT;
        }

        SPDLOG_WARN("[{}][PostInitProperties] Slot 8 did not validate; skipping LocalPlayer bootstrap for safety", runtime_label);
        return std::nullopt;
    }

    // UE4.27.2 source and shipping PDBs place UObject::PostInitProperties at
    // slot 8. Validate that slot directly instead of trying the UE5 slots first.
    if (is_ue_4_27_runtime()) {
        constexpr uint32_t UE427_POST_INIT_PROPERTIES_SLOT = 8;

        if (validate_source_informed_post_init_slot(
                object_vtable,
                localplayer_vtable,
                UE427_POST_INIT_PROPERTIES_SLOT,
                "UE4.27 UObject::PostInitProperties",
                false,
                true))
        {
            return UE427_POST_INIT_PROPERTIES_SLOT;
        }

        SPDLOG_WARN("[PostInitProperties] UE4.27 slot 8 did not validate; falling back to guarded nearby scan");
    }

    // UE5.1 source plus Stalker2/SOE PDBs place UObject::PostInitProperties at
    // slot 10 for shipped game layouts. Some UE5.1 games put a LocalPlayer
    // override/thunk at the same slot, so validate UObject strictly and only
    // require the LocalPlayer target to be callable.
    if (is_ue_5_1_dx_backend()) {
        constexpr uint32_t UE51_POST_INIT_PROPERTIES_SLOT = 10;

        if (validate_source_informed_post_init_slot(
                object_vtable,
                localplayer_vtable,
                UE51_POST_INIT_PROPERTIES_SLOT,
                "UE5.1 UObject::PostInitProperties",
                false,
                true))
        {
            return UE51_POST_INIT_PROPERTIES_SLOT;
        }

        SPDLOG_WARN("[PostInitProperties] UE5.1 slot 10 did not validate; skipping LocalPlayer bootstrap");
        return std::nullopt;
    }

    // UE5.2.1/5.3.2 source plus The Complex Expedition PDB place
    // UObject::PostInitProperties at slot 9 in shipped layouts. The older
    // legacy body scan sees the function but cannot identify it because this
    // implementation does not reference GEngine.
    if (is_ue_5_2_dx_backend() || is_ue_5_3_dx_backend()) {
        constexpr uint32_t UE52_53_POST_INIT_PROPERTIES_SLOT = 9;

        if (validate_source_informed_post_init_slot(
                object_vtable,
                localplayer_vtable,
                UE52_53_POST_INIT_PROPERTIES_SLOT,
                is_ue_5_3_dx_backend() ? "UE5.3 UObject::PostInitProperties" : "UE5.2 UObject::PostInitProperties",
                false,
                true))
        {
            return UE52_53_POST_INIT_PROPERTIES_SLOT;
        }

        SPDLOG_WARN("[PostInitProperties] UE5.2/5.3 slot 9 did not validate; skipping LocalPlayer bootstrap");
        return std::nullopt;
    }

    // UE 5.4.4, 5.5.4 and 5.6.1 source/PDB put UObject::PostInitProperties at slot 10
    // for shipped game layouts:
    // UObjectBase has 4 virtuals, UObjectBaseUtility has 5, then UObject adds
    // GetDetailedInfoInternal at 9 and PostInitProperties at 10.
    if (is_ue_5_4_dx_backend() || is_ue_5_5_dx_backend() || is_ue_5_6_dx12_backend() || is_ue_5_7_or_newer()) {
        constexpr uint32_t UE54_PLUS_POST_INIT_PROPERTIES_SLOT = 10;

        if (validate_source_informed_post_init_slot(
                object_vtable,
                localplayer_vtable,
                UE54_PLUS_POST_INIT_PROPERTIES_SLOT,
                "UE5.4+ UObject::PostInitProperties",
                false,
                is_ue_5_4_dx_backend()))
        {
            return UE54_PLUS_POST_INIT_PROPERTIES_SLOT;
        }
    }

    // Keep the nearby slots as a fail-closed fallback for unusual/custom layouts.
    constexpr std::array<uint32_t, 4> candidate_slots{10, 9, 8, 11};

    for (const auto slot : candidate_slots) {
        if (IsBadReadPtr(&object_vtable[slot], sizeof(uintptr_t)) ||
            IsBadReadPtr(&localplayer_vtable[slot], sizeof(uintptr_t)))
        {
            continue;
        }

        const auto object_fn = object_vtable[slot];
        const auto localplayer_fn = localplayer_vtable[slot];

        if (object_fn == 0 || localplayer_fn == 0) {
            continue;
        }

        if (!looks_like_post_init_properties_virtual(object_fn) ||
            !looks_like_post_init_properties_virtual(localplayer_fn))
        {
            continue;
        }

        SPDLOG_INFO("[PostInitProperties] Resolved UObject::PostInitProperties through nearby fallback slot {} object_fn={:x} localplayer_fn={:x}",
            slot,
            object_fn,
            localplayer_fn);
        return slot;
    }

    SPDLOG_WARN("[PostInitProperties] Could not validate the expected UObject::PostInitProperties slots on this build");
    return std::nullopt;
}
}

namespace {
bool is_writable_process_range(uintptr_t address, size_t size) {
    if (address == 0 || size == 0 || address + size < address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((void*)address, &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    const auto base = (uintptr_t)mbi.BaseAddress;
    if (address + size > base + mbi.RegionSize) {
        return false;
    }

    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const auto protect = mbi.Protect & 0xff;
    return protect == PAGE_READWRITE ||
           protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}

bool is_readable_process_range(uintptr_t address, size_t size) {
    if (address == 0 || size == 0 || address + size < address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((void*)address, &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    const auto base = (uintptr_t)mbi.BaseAddress;
    if (address + size > base + mbi.RegionSize) {
        return false;
    }

    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const auto protect = mbi.Protect & 0xff;
    return protect == PAGE_READONLY ||
           protect == PAGE_READWRITE ||
           protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}

bool is_executable_process_range(uintptr_t address, size_t size) {
    if (address == 0 || size == 0 || address + size < address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((void*)address, &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    const auto base = (uintptr_t)mbi.BaseAddress;
    if (address + size > base + mbi.RegionSize) {
        return false;
    }

    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const auto protect = mbi.Protect & 0xff;
    return protect == PAGE_EXECUTE ||
           protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}

bool overlaps_current_thread_stack(uintptr_t address, size_t size) {
    if (address == 0 || size == 0 || address + size < address) {
        return false;
    }

    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    if (tib == nullptr) {
        return false;
    }

    const auto stack_low = reinterpret_cast<uintptr_t>(tib->StackLimit);
    const auto stack_high = reinterpret_cast<uintptr_t>(tib->StackBase);
    return address < stack_high && address + size > stack_low;
}

bool is_probable_new_rhi_command(sdk::FRHICommandBase_New* command, const char*& reason) {
    const auto address = reinterpret_cast<uintptr_t>(command);

    if (command == nullptr || (address & (alignof(void*) - 1)) != 0) {
        reason = "null or unaligned command";
        return false;
    }

    if (overlaps_current_thread_stack(address, sizeof(*command))) {
        reason = "stack-resident command";
        return false;
    }

    if (!is_readable_process_range(address, sizeof(*command)) ||
        !is_writable_process_range(address, sizeof(void*)))
    {
        reason = "unreadable or non-writable command";
        return false;
    }

    const auto vtable = *reinterpret_cast<uintptr_t*>(address);
    if (!is_readable_process_range(vtable, sizeof(uintptr_t)) ||
        !utility::get_module_within(reinterpret_cast<void*>(vtable)).has_value())
    {
        reason = "vtable is not module-owned";
        return false;
    }

    const auto execute = *reinterpret_cast<uintptr_t*>(vtable);
    if (!is_executable_process_range(execute, 1)) {
        reason = "first vtable entry is not executable";
        return false;
    }

    reason = nullptr;
    return true;
}

bool is_probable_old_rhi_command(sdk::FRHICommandBase_Old* command, const char*& reason) {
    const auto address = reinterpret_cast<uintptr_t>(command);

    if (command == nullptr || (address & (alignof(void*) - 1)) != 0) {
        reason = "null or unaligned command";
        return false;
    }

    if (overlaps_current_thread_stack(address, sizeof(*command))) {
        reason = "stack-resident command";
        return false;
    }

    if (!is_readable_process_range(address, sizeof(*command)) ||
        !is_writable_process_range(
            address + offsetof(sdk::FRHICommandBase_Old, func),
            sizeof(command->func)))
    {
        reason = "unreadable or non-writable command";
        return false;
    }

    if (!is_executable_process_range(reinterpret_cast<uintptr_t>(command->func), 1)) {
        reason = "command function is not executable";
        return false;
    }

    reason = nullptr;
    return true;
}

struct RuntimeFunctionRange {
    uintptr_t begin{};
    uintptr_t end{};
    uintptr_t image_base{};

    size_t size() const {
        return end - begin;
    }
};

std::optional<RuntimeFunctionRange> get_runtime_function_range(uintptr_t address) {
    DWORD64 image_base{};
    const auto runtime_function = RtlLookupFunctionEntry(
        static_cast<DWORD64>(address),
        &image_base,
        nullptr);

    if (runtime_function == nullptr || image_base == 0) {
        return std::nullopt;
    }

    const auto begin = static_cast<uintptr_t>(image_base + runtime_function->BeginAddress);
    const auto end = static_cast<uintptr_t>(image_base + runtime_function->EndAddress);

    if (begin == 0 || end <= begin || address < begin || address >= end ||
        !is_executable_process_range(begin, std::min<size_t>(end - begin, 16)))
    {
        return std::nullopt;
    }

    return RuntimeFunctionRange{
        .begin = begin,
        .end = end,
        .image_base = static_cast<uintptr_t>(image_base),
    };
}

bool direct_call_returns_to(uintptr_t return_address, uintptr_t expected_target) {
    constexpr size_t direct_call_size = 5;

    if (return_address < direct_call_size ||
        !is_readable_process_range(return_address - direct_call_size, direct_call_size))
    {
        return false;
    }

    const auto call = reinterpret_cast<const uint8_t*>(return_address - direct_call_size);
    if (call[0] != 0xE8) {
        return false;
    }

    int32_t displacement{};
    std::memcpy(&displacement, call + 1, sizeof(displacement));
    return static_cast<uintptr_t>(return_address + displacement) == expected_target;
}

bool indirect_virtual_call_returns_to(uintptr_t return_address, uint8_t slot_offset) {
    constexpr size_t minimum_call_size = 3;

    if (return_address < minimum_call_size ||
        !is_readable_process_range(return_address - minimum_call_size, minimum_call_size))
    {
        return false;
    }

    // CALL qword ptr [reg+disp8]. A REX prefix, when needed, precedes these
    // final three bytes and does not change this validation.
    const auto call = reinterpret_cast<const uint8_t*>(return_address - minimum_call_size);
    const auto modrm = call[1];
    return call[0] == 0xFF &&
           (modrm & 0xC0) == 0x40 &&
           (modrm & 0x38) == 0x10 &&
           (modrm & 0x07) != 0x04 &&
           call[2] == slot_offset;
}

bool has_ue426_427_begin_rendering_viewfamily_shape(const RuntimeFunctionRange& function) {
    if (function.size() < 0x200 || function.size() > 0x4000 ||
        !is_readable_process_range(function.begin, function.size()))
    {
        return false;
    }

    const auto bytes = reinterpret_cast<const uint8_t*>(function.begin);
    bool writes_frame_number = false;
    bool calls_begin_render_viewfamily = false;

    for (size_t i = 0; i + 3 < function.size(); ++i) {
        size_t opcode_index = i;
        uint8_t rex{};
        if (bytes[opcode_index] >= 0x40 && bytes[opcode_index] <= 0x4F) {
            rex = bytes[opcode_index++];
        }

        if (opcode_index + 2 >= function.size()) {
            break;
        }

        const auto opcode = bytes[opcode_index];
        const auto modrm = bytes[opcode_index + 1];
        const auto uses_disp8 = (modrm & 0xC0) == 0x40;
        const auto uses_sib = (modrm & 0x07) == 0x04;
        const auto displacement_index = opcode_index + (uses_sib ? 3 : 2);

        if (!uses_disp8 || displacement_index >= function.size()) {
            continue;
        }

        // UE4.26/4.27 FSceneViewFamily::FrameNumber is a uint32 at +0x5c.
        // Accept any compiler-selected base/source register, but reject a
        // REX.W qword store.
        if (opcode == 0x89 && (rex & 0x08) == 0 && bytes[displacement_index] == 0x5C) {
            writes_frame_number = true;
        }

        // ISceneViewExtension::BeginRenderViewFamily is virtual slot 5, so its
        // byte displacement is 5 * sizeof(void*) == 0x28.
        if (opcode == 0xFF &&
            (modrm & 0x38) == 0x10 &&
            bytes[displacement_index] == 0x28)
        {
            calls_begin_render_viewfamily = true;
        }

        if (writes_frame_number && calls_begin_render_viewfamily) {
            return true;
        }
    }

    return false;
}

std::optional<RuntimeFunctionRange> get_ue426_427_begin_rendering_viewfamily_range(
    uintptr_t callback_return)
{
    constexpr uint8_t unwind_flag_chaininfo = 0x4;
    constexpr uint32_t max_chain_depth = 4;

    if (!indirect_virtual_call_returns_to(callback_return, 0x28)) {
        return std::nullopt;
    }

    DWORD64 image_base64{};
    const auto runtime_function = RtlLookupFunctionEntry(
        static_cast<DWORD64>(callback_return),
        &image_base64,
        nullptr);
    const auto current_range = get_runtime_function_range(callback_return);

    if (runtime_function == nullptr || image_base64 == 0 || !current_range) {
        return std::nullopt;
    }

    auto combined = *current_range;
    auto chained_entry = *runtime_function;
    const auto image_base = static_cast<uintptr_t>(image_base64);

    for (uint32_t depth = 0; depth <= max_chain_depth; ++depth) {
        if (has_ue426_427_begin_rendering_viewfamily_shape(combined)) {
            return combined;
        }

        if (depth == max_chain_depth || chained_entry.UnwindData == 0) {
            break;
        }

        const auto unwind_address = image_base + chained_entry.UnwindData;
        if (unwind_address < image_base || !is_readable_process_range(unwind_address, 4)) {
            break;
        }

        const auto unwind_info = reinterpret_cast<const uint8_t*>(unwind_address);
        const auto version = unwind_info[0] & 0x07;
        const auto flags = unwind_info[0] >> 3;
        if (version != 1 || (flags & unwind_flag_chaininfo) == 0) {
            break;
        }

        const auto unwind_code_count = static_cast<size_t>(unwind_info[2]);
        const auto aligned_code_count = (unwind_code_count + 1) & ~size_t{1};
        const auto chained_entry_address =
            unwind_address + 4 + aligned_code_count * sizeof(uint16_t);
        if (chained_entry_address < unwind_address ||
            !is_readable_process_range(chained_entry_address, sizeof(RUNTIME_FUNCTION)))
        {
            break;
        }

        RUNTIME_FUNCTION parent_entry{};
        std::memcpy(
            &parent_entry,
            reinterpret_cast<const void*>(chained_entry_address),
            sizeof(parent_entry));

        const auto parent_begin = image_base + parent_entry.BeginAddress;
        const auto parent_end = image_base + parent_entry.EndAddress;
        if (parent_begin < image_base || parent_end <= parent_begin ||
            parent_end != combined.begin || parent_entry.UnwindData == 0 ||
            combined.end - parent_begin > 0x4000 ||
            !is_executable_process_range(
                parent_begin,
                std::min<size_t>(parent_end - parent_begin, 16)))
        {
            break;
        }

        combined.begin = parent_begin;
        chained_entry = parent_entry;
    }

    return std::nullopt;
}

// SEH-probed read. Hooked functions can be reached with arbitrary arguments
// (ICF-folded modular builds — Returnal), including pointers into live thread
// stacks whose contents change between a validity check and the dereference.
// Pointwise IsBadReadPtr/VirtualQuery checks can never make that safe — copy
// first, interpret the copy.
static bool nsf_seh_read(const void* src, void* dst, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH isolation for the native-stereo-fix render passes. Returnal's modular
// build AVs inside BeginRenderingViewFamilies (its FSR2 view extension calling
// through nulled state) a few frames after the NSF flow engages. Contain the
// fault so the interaction disables NSF for the session instead of killing the
// game — and so the log pinpoints WHICH pass faulted. Plain args only: no
// unwindable objects may live in a __try frame, hence its own function.
__declspec(noinline) static bool nsf_run_pass_guarded(
    safetyhook::InlineHook& hook, void* render_module, sdk::FCanvas* canvas,
    sdk::FSceneViewFamily* view_family, void* trailing_ptr_arg, uintptr_t trailing_flag_arg) {
    __try {
        hook.unsafe_call<void>(render_module, canvas, view_family, trailing_ptr_arg, trailing_flag_arg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// One fault anywhere in the NSF flow permanently reverts this session to a
// plain passthrough (no view suppression, no capture pass).
static bool s_nsf_flow_disabled = false;

// Resolve a family's Views array by VERIFIED offset instead of trusting the one
// scanned offset. Returnal's Housemarque family has no vtable and a custom
// leading member (a float — its raw first field reads as 0x..3f800000), which
// puts Views at +0x8 while the scanner latches the no-vtable default of 0.
// Reading there yields a non-array: the eye-pair proving loop never sees its two
// views, so no pair is ever proven and NSF stays inert (doubled geometry in the
// left eye, black right eye), and writing there would scribble the family.
//
// Probe the plausible offsets and accept one only on full verification: sane
// TArray header, readable pointer array, and EVERY view's owning-family
// back-pointer naming this exact family. That back-pointer sits at slot 0 on
// UE4, after the vtable on UE5.6+, and deeper still on Hellblade 2's UE5 build,
// so search the first 16 qwords — a foreign struct containing this exact family
// pointer is essentially impossible, which keeps the test definitive. Returns
// nullptr when nothing verifies, so callers fall back rather than act on
// garbage. Every read goes through nsf_seh_read.
static sdk::TArray<sdk::FSceneView*>* nsf_resolve_verified_views(sdk::FSceneViewFamily* view_family) {
    if (view_family == nullptr) {
        return nullptr;
    }

    struct ViewsCopy {
        sdk::FSceneView** data;
        uint32_t count;
        uint32_t capacity;
    };

    const auto scanned = view_family->get_views(); // pointer arithmetic only, no deref
    const uintptr_t base = (uintptr_t)view_family;
    const uintptr_t candidates[6] = {
        scanned != nullptr ? (uintptr_t)scanned : 0,
        base + 0x8,
        base,
        base + 0x10,
        base + 0x18,
        base + 0x20,
    };

    for (const auto cand : candidates) {
        if (cand == 0) {
            continue;
        }

        ViewsCopy probe{};
        sdk::FSceneView* probe_views[8]{};

        if (!nsf_seh_read((void*)cand, &probe, sizeof(probe)) ||
            probe.count == 0 || probe.count > 8 || probe.capacity < probe.count ||
            probe.data == nullptr ||
            !nsf_seh_read(probe.data, probe_views, sizeof(void*) * probe.count)) {
            continue;
        }

        bool all_backptrs_ok = true;
        for (uint32_t i = 0; i < probe.count && all_backptrs_ok; ++i) {
            sdk::FSceneViewFamily* owner_slots[16]{};
            bool matched = false;

            if (probe_views[i] != nullptr &&
                nsf_seh_read(probe_views[i], owner_slots, sizeof(owner_slots))) {
                for (auto* slot : owner_slots) {
                    if (slot == view_family) {
                        matched = true;
                        break;
                    }
                }
            }

            all_backptrs_ok = matched;
        }

        if (all_backptrs_ok) {
            SPDLOG_INFO_ONCE(
                "[NativeStereoFix] Views verified at family+{:x} (count={}, scanned offset {})",
                cand - base, probe.count,
                scanned != nullptr ? (int64_t)((uintptr_t)scanned - base) : -1);
            return (sdk::TArray<sdk::FSceneView*>*)cand;
        }
    }

    // Nothing verified. Fall back to the scanned offset — that is exactly what
    // the engine-side code did before this helper existed, so titles whose views
    // simply do not carry a family back-pointer where we search keep working
    // instead of regressing to an inert NSF. Callers that WRITE through the
    // result still apply their own plausibility guard.
    //
    // This is expected, not an error: cloned families back-point at the original,
    // and candidates reached with stack-borne garbage never verify. Log it once
    // at info level so it stays diagnosable without spamming a normal session.
    SPDLOG_INFO_ONCE(
        "[NativeStereoFix] A family's views did not pass back-pointer verification; "
        "using the scanned offset (family {:x}, scanned {:x})",
        base, (uintptr_t)scanned);
    return scanned;
}


bool has_begin_rendering_viewfamily_wrapper_shape(const RuntimeFunctionRange& wrapper) {
    // UE5's singular wrapper builds a one-element TArrayView on the stack. Keep
    // this as corroborating evidence rather than the sole resolver condition.
    constexpr std::array<uint8_t, 4> store_r8_to_stack{0x4C, 0x89, 0x44, 0x24};
    constexpr std::array<uint8_t, 4> load_r8_from_stack{0x4C, 0x8D, 0x44, 0x24};

    if (wrapper.size() > 0x180 || !is_readable_process_range(wrapper.begin, wrapper.size())) {
        return false;
    }

    const auto bytes = reinterpret_cast<const uint8_t*>(wrapper.begin);
    const auto contains = [&](const auto& pattern) {
        return std::search(bytes, bytes + wrapper.size(), pattern.begin(), pattern.end()) !=
               bytes + wrapper.size();
    };

    return contains(store_r8_to_stack) && contains(load_r8_from_stack);
}

std::optional<uintptr_t> resolve_begin_rendering_viewfamilies_from_stack(
    uintptr_t direct_callback_return = 0)
{
    constexpr uint32_t max_stack_depth = 32;
    // The renderer entry is close to the view-extension callback. Launch-loop
    // frames farther up the stack can have the same small-wrapper/direct-call
    // shape but are not recurring render entry points.
    constexpr uint32_t max_renderer_stack_index = 10;
    std::array<uintptr_t, max_stack_depth> stack{};
    const auto depth = RtlCaptureStackBackTrace(
        0,
        max_stack_depth,
        reinterpret_cast<void**>(stack.data()),
        nullptr);

    const auto game_module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto source_validated_ue4 = is_ue_4_26_runtime() || is_ue_4_27_runtime();
    std::optional<uintptr_t> best_candidate{};
    int best_score = std::numeric_limits<int>::min();

    // UE4.26/4.27 calls slot 5 directly from FRendererModule::
    // BeginRenderingViewFamily. The callback's own return address is stronger
    // evidence than reconstructing that frame through RtlCaptureStackBackTrace.
    if (source_validated_ue4 && direct_callback_return != 0) {
        const auto direct_segment = get_runtime_function_range(direct_callback_return);
        const auto direct_candidate =
            get_ue426_427_begin_rendering_viewfamily_range(direct_callback_return);
        const auto direct_candidate_valid =
            direct_candidate.has_value() &&
            direct_candidate->image_base == game_module;

        if (direct_candidate_valid) {
            SPDLOG_INFO(
                "[UE4.26/4.27][ViewFamilySelector] Resolved source-validated callback caller "
                "target={:x} size={:x} return={:x}",
                direct_candidate->begin,
                direct_candidate->size(),
                direct_callback_return);
            return direct_candidate->begin;
        }

        SPDLOG_WARN_ONCE(
            "[UE4.26/4.27][ViewFamilySelector] Rejected callback caller return={:x} "
            "function={:x} size={:x} game_module={} slot5_call={} renderer_shape={}",
            direct_callback_return,
            direct_segment ? direct_segment->begin : 0,
            direct_segment ? direct_segment->size() : 0,
            direct_segment && direct_segment->image_base == game_module,
            indirect_virtual_call_returns_to(direct_callback_return, 0x28),
            direct_candidate.has_value());
    }

    // The view-extension callback runs inside CreateSceneRenderers. The next
    // frames are BeginRenderingViewFamilies and its small singular wrapper.
    // Validate that wrapper's exact direct CALL instead of guessing from the
    // first captured frame.
    for (uint32_t i = 1; !source_validated_ue4 && i + 1 < depth && i <= max_renderer_stack_index; ++i) {
        const auto callee = get_runtime_function_range(stack[i]);
        const auto caller = get_runtime_function_range(stack[i + 1]);

        if (!callee || !caller || callee->begin == caller->begin ||
            callee->image_base != game_module || caller->image_base != game_module ||
            caller->size() > 0x180 || callee->size() < 0x200 ||
            !direct_call_returns_to(stack[i + 1], callee->begin))
        {
            continue;
        }

        // Only UE5's singular wrapper is evidence for its plural callee. A
        // generic direct-call pair can otherwise resolve GuardedMain through
        // GuardedMainWrapper on UE4 and install a hook that never runs again.
        if (!has_begin_rendering_viewfamily_wrapper_shape(*caller)) {
            continue;
        }

        auto score = 100;
        score += static_cast<int>(std::min<size_t>(callee->size() / 0x100, 32));
        score += static_cast<int>((0x180 - caller->size()) / 8);

        if (score > best_score) {
            best_score = score;
            best_candidate = callee->begin;
            SPDLOG_INFO(
                "[ViewFamilySelector] BeginRenderingViewFamilies candidate target={:x} size={:x} "
                "wrapper={:x} wrapper_size={:x} return={:x} stack_index={} score={}",
                callee->begin,
                callee->size(),
                caller->begin,
                caller->size(),
                stack[i + 1],
                i,
                score);
        }
    }

    if (!best_candidate) {
        // UE4 and UE5.0 call the singular BeginRenderingViewFamily directly,
        // while optimized newer builds can inline away the small plural wrapper.
        // The first substantial game-module frame above this view-extension
        // callback is the renderer entry point itself.
        constexpr size_t min_renderer_size = 0x200;
        constexpr size_t max_renderer_size = 0x4000;

        const auto our_module = reinterpret_cast<uintptr_t>(g_framework->get_framework_module());

        for (uint32_t i = 1; i < depth && i <= max_renderer_stack_index; ++i) {
            const auto candidate = source_validated_ue4
                ? get_ue426_427_begin_rendering_viewfamily_range(stack[i])
                : get_runtime_function_range(stack[i]);
            if (!candidate) {
                continue;
            }

            // The direct caller is NOT necessarily in the main exe. Modular
            // shipping builds exist — Returnal ships the engine as per-module
            // DLLs, so the real caller lives in
            // Returnal-Renderer-Win64-Shipping.dll. An exe-only filter skips
            // every correct frame and settles on an unrelated exe frame higher
            // up the stack (observed: stack_index=10), which is then hooked:
            // the wrong function receives stack garbage instead of a view
            // family ("unreadable argument" forever), so the second view is
            // never suppressed (doubled geometry in the left eye) and the
            // capture pass never runs (black right eye).
            //
            // Accept the first frame in ANY module that is not our own backend.
            // Also do NOT size-filter a candidate outside the main exe: in
            // modular builds the vtable points at a tiny page-aligned export
            // thunk (Returnal: 0x17 bytes) and hooking the THUNK is correct —
            // every virtual call routes through it.
            const bool in_main_exe = candidate->image_base == game_module;
            uintptr_t entry = candidate->begin;

            if (in_main_exe) {
                if (candidate->size() < min_renderer_size || candidate->size() > max_renderer_size) {
                    continue;
                }

                // A chained unwind fragment is not modular-specific: Hellblade 2
                // ships a single exe and still resolved 0x..e5b, an unaligned
                // mid-function address that killed the process the instant the
                // hook installed. Resolve the real entry the same way as below.
                if (!source_validated_ue4) {
                    const auto unwind = utility::find_function_start_unwind(stack[i]);
                    const auto resolved = utility::find_virtual_function_start(unwind ? *unwind : stack[i]);

                    if (resolved) {
                        entry = *resolved;
                    } else if (unwind) {
                        entry = *unwind;
                    }
                }
            } else {
                if (source_validated_ue4) {
                    continue; // UE4.26/4.27 keeps upstream's exe-only source validation
                }

                if (candidate->image_base == 0 || candidate->image_base == our_module) {
                    continue;
                }

                // get_runtime_function_range().begin is the start of the
                // RUNTIME_FUNCTION covering this address, which for a CHAINED
                // unwind fragment is the fragment — not a callable entry
                // (Returnal resolved 0x..c017, not even 16-byte aligned;
                // hooking it killed the process the instant the hook installed).
                // Unwind first: find_function_start_unwind walks
                // UNW_FLAG_CHAININFO back to the primary RUNTIME_FUNCTION, and
                // find_virtual_function_start then confirms a real entry. Never
                // fall back to the raw return address — it points into the
                // middle of the frame.
                const auto unwind = utility::find_function_start_unwind(stack[i]);
                const auto resolved = utility::find_virtual_function_start(unwind ? *unwind : stack[i]);

                if (resolved) {
                    entry = *resolved;
                } else if (unwind) {
                    // No table reference to the entry (private engine trees
                    // devirtualize it — Elliot/UE5.6). The unwind entry still
                    // describes the real function start exactly.
                    entry = *unwind;
                    SPDLOG_INFO(
                        "[ViewFamilySelector] No table reference for the modular entry; using the "
                        "unwind-resolved start {:x} (stack_index={})",
                        entry, i);
                } else {
                    continue;
                }
            }

            if (source_validated_ue4 &&
                (!indirect_virtual_call_returns_to(stack[i], 0x28) ||
                 !has_ue426_427_begin_rendering_viewfamily_shape(*candidate)))
            {
                continue;
            }

            best_candidate = entry;
            if (source_validated_ue4) {
                SPDLOG_INFO(
                    "[UE4.26/4.27][ViewFamilySelector] Resolved source-validated direct "
                    "BeginRenderingViewFamily entry target={:x} size={:x} return={:x} stack_index={}",
                    candidate->begin,
                    candidate->size(),
                    stack[i],
                    i);
            } else {
                SPDLOG_INFO(
                    "[ViewFamilySelector] Resolved direct BeginRenderingViewFamily entry from stack "
                    "target={:x} (range_begin={:x}) size={:x} return={:x} stack_index={} module={} in_main_exe={}",
                    entry,
                    candidate->begin,
                    candidate->size(),
                    stack[i],
                    i,
                    utility::get_module_pathw(reinterpret_cast<HMODULE>(candidate->image_base))
                        .transform([](const auto& p) { return std::filesystem::path{p}.filename().string(); })
                        .value_or("<unknown>"),
                    in_main_exe);
            }
            break;
        }
    }

    return best_candidate;
}

bool validate_dune_begin_rendering_viewfamilies_target(uintptr_t target) {
    const auto game_module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto function = get_runtime_function_range(target);

    if (!function || function->begin != target || function->image_base != game_module || function->size() < 0x200) {
        return false;
    }

    // Dune's UE5.2 implementation consumes the TArrayView passed in R8 at the
    // start of the plural function: its data pointer is at +0 and count at +8.
    // Requiring both reads prevents an exact-wrapper false positive from ever
    // being installed as a Native Stereo Fix hook.
    const auto validation_size = std::min<size_t>(function->size(), 0x100);
    return utility::scan(target, validation_size, "4D 8B 20").has_value() &&
           utility::scan(target, validation_size, "49 63 40 08").has_value();
}

std::optional<uintptr_t> resolve_dune_begin_rendering_viewfamilies() {
    if (!dune_awakening_is_current_game()) {
        return std::nullopt;
    }

    static const auto resolved = []() -> std::optional<uintptr_t> {
        const auto module = utility::get_executable();

        // FRendererModule::BeginRenderingViewFamily(FCanvas*, FSceneViewFamily*)
        // builds a one-element TArrayView and directly calls the plural entry.
        // Resolve that CALL rather than accepting an unrelated render-stack
        // function based only on its size.
        constexpr auto wrapper_pattern =
            "4C 89 44 24 18 48 83 EC 38 C7 44 24 28 01 00 00 00 "
            "48 8D 44 24 50 48 89 44 24 20 4C 8D 44 24 20 "
            "C5 F8 10 44 24 20 C5 F9 7F 44 24 20 E8 ? ? ? ? "
            "48 83 C4 38 C3";
        constexpr size_t call_offset = 0x2C;

        const auto wrapper = utility::scan(module, wrapper_pattern);
        if (!wrapper) {
            SPDLOG_ERROR(
                "[Dune][NativeStereoFix] Refusing activation: the verified singular BeginRenderingViewFamily wrapper was not found");
            return std::nullopt;
        }

        const auto wrapper_function = get_runtime_function_range(*wrapper);
        if (!wrapper_function || wrapper_function->begin != *wrapper || wrapper_function->size() > 0x80) {
            SPDLOG_ERROR(
                "[Dune][NativeStereoFix] Refusing activation: wrapper at {:x} failed function-boundary validation",
                *wrapper);
            return std::nullopt;
        }

        const auto call_address = *wrapper + call_offset;
        if (!is_readable_process_range(call_address, 5) || *reinterpret_cast<const uint8_t*>(call_address) != 0xE8) {
            SPDLOG_ERROR(
                "[Dune][NativeStereoFix] Refusing activation: wrapper at {:x} has no verified direct CALL",
                *wrapper);
            return std::nullopt;
        }

        int32_t displacement{};
        std::memcpy(&displacement, reinterpret_cast<const void*>(call_address + 1), sizeof(displacement));
        const auto target = static_cast<uintptr_t>(call_address + 5 + displacement);

        if (!validate_dune_begin_rendering_viewfamilies_target(target)) {
            SPDLOG_ERROR(
                "[Dune][NativeStereoFix] Refusing activation: plural target {:x} failed TArrayView validation",
                target);
            return std::nullopt;
        }

        SPDLOG_INFO(
            "[Dune][NativeStereoFix] Resolved verified BeginRenderingViewFamilies wrapper={:x} target={:x}",
            *wrapper,
            target);
        return target;
    }();

    return resolved;
}

bool looks_like_virtual_function_table(uintptr_t table) {
    if (!is_readable_process_range(table, sizeof(uintptr_t) * 12)) {
        return false;
    }

    auto executable_entries = 0;

    for (auto i = 0; i < 12; ++i) {
        const auto fn = ((uintptr_t*)table)[i];

        if (fn == 0 || !is_executable_process_range(fn, 1)) {
            continue;
        }

        ++executable_entries;
    }

    return executable_entries >= 6;
}

template <typename T>
bool safe_read_value(uintptr_t address, T& out) {
    if (!is_readable_process_range(address, sizeof(T))) {
        return false;
    }

    memcpy(&out, (void*)address, sizeof(T));
    return true;
}

struct GhostingRawArrayHeader {
    uintptr_t data{};
    int32_t count{};
    int32_t capacity{};
};

static_assert(sizeof(GhostingRawArrayHeader) == 0x10);

bool ghosting_read_array_header(
    uintptr_t address,
    int32_t maximum_count,
    int32_t maximum_capacity,
    GhostingRawArrayHeader& out);

enum class GhostingUObjectValidationMode : uint8_t {
    ObjectArray,
    UObjectHook,
};

enum class GhostingOwnerResolveFailure : uint8_t {
    None,
    InvalidSceneStates,
    EngineUnavailable,
    EngineLifetime,
    GameInstanceProperty,
    GameInstanceLifetime,
    LocalPlayersProperty,
    LocalPlayersArray,
    LocalPlayerSlot,
    LocalPlayerLifetime,
    ViewStateStorage,
    ViewportClient,
    World,
};

const char* ghosting_owner_failure_name(GhostingOwnerResolveFailure failure) {
    switch (failure) {
    case GhostingOwnerResolveFailure::None: return "none";
    case GhostingOwnerResolveFailure::InvalidSceneStates: return "invalid scene states";
    case GhostingOwnerResolveFailure::EngineUnavailable: return "engine unavailable";
    case GhostingOwnerResolveFailure::EngineLifetime: return "engine lifetime";
    case GhostingOwnerResolveFailure::GameInstanceProperty: return "GameInstance property";
    case GhostingOwnerResolveFailure::GameInstanceLifetime: return "GameInstance lifetime";
    case GhostingOwnerResolveFailure::LocalPlayersProperty: return "LocalPlayers property";
    case GhostingOwnerResolveFailure::LocalPlayersArray: return "LocalPlayers array";
    case GhostingOwnerResolveFailure::LocalPlayerSlot: return "LocalPlayer slot";
    case GhostingOwnerResolveFailure::LocalPlayerLifetime: return "LocalPlayer lifetime";
    case GhostingOwnerResolveFailure::ViewStateStorage: return "scene-state storage";
    case GhostingOwnerResolveFailure::ViewportClient: return "viewport client";
    case GhostingOwnerResolveFailure::World: return "viewport world";
    default: return "unknown";
    }
}

struct GhostingUObjectIdentity {
    uintptr_t vtable{};
    uintptr_t object_class{};
    int32_t internal_index{-1};
    int32_t serial{};
};

struct GhostingOwnerResolveDiagnostic {
    GhostingOwnerResolveFailure failure{GhostingOwnerResolveFailure::None};
    int32_t local_player_index{-1};
    int32_t local_player_count{};
};

struct GhostingFixOwnerCandidate {
    sdk::UObject* engine{};
    uintptr_t engine_vtable{};
    uintptr_t engine_class{};
    int32_t engine_index{-1};
    int32_t engine_serial{};
    uintptr_t game_instance_slot{};
    sdk::UObject* game_instance{};
    uintptr_t game_instance_vtable{};
    uintptr_t game_instance_class{};
    int32_t game_instance_index{-1};
    int32_t game_instance_serial{};
    uintptr_t local_players_header{};
    uintptr_t local_players_data{};
    int32_t local_players_count{};
    int32_t local_players_capacity{};
    uintptr_t local_player_slot{};
    sdk::UObject* local_player{};
    uintptr_t local_player_vtable{};
    uintptr_t local_player_class{};
    int32_t local_player_index{-1};
    int32_t local_player_serial{};
    uintptr_t view_states_header{};
    uintptr_t view_states_data{};
    int32_t view_states_count{};
    int32_t view_states_capacity{};
    uint32_t view_state_stride{};
    uintptr_t view_state_reference_vtable{};
    uintptr_t eye_state_slot[2]{};
    uintptr_t viewport_client_slot{};
    sdk::UObject* viewport_client{};
    uintptr_t viewport_client_vtable{};
    uintptr_t viewport_client_class{};
    int32_t viewport_client_index{-1};
    int32_t viewport_client_serial{};
    uintptr_t world_slot{};
    sdk::UObject* world{};
    uintptr_t world_vtable{};
    uintptr_t world_class{};
    int32_t world_index{-1};
    int32_t world_serial{};
    bool view_states_are_array{};
    bool uses_uobject_hook_validation{};
};

bool ghosting_is_valid_scene_state(sdk::FSceneViewStateInterface* state) {
    if (state == nullptr || !is_readable_process_range((uintptr_t)state, sizeof(uintptr_t))) {
        return false;
    }

    uintptr_t vtable{};
    uintptr_t first_virtual{};
    return safe_read_value((uintptr_t)state, vtable) &&
        vtable != 0 &&
        safe_read_value(vtable, first_virtual) &&
        first_virtual != 0 &&
        is_executable_process_range(first_virtual, 1) &&
        utility::get_module_within((void*)vtable).has_value();
}

struct LegacyLocalPlayerViewStatesSnapshot {
    uintptr_t header_address{};
    GhostingRawArrayHeader header{};
    uintptr_t reference_vtable{};
};

bool validate_ue425_426_view_states_array(
    uintptr_t header_address,
    LegacyLocalPlayerViewStatesSnapshot& out)
{
    constexpr uint32_t VIEW_STATE_REFERENCE_STRIDE = 0x28;

    GhostingRawArrayHeader header{};
    if (!ghosting_read_array_header(header_address, 8, 16, header)) {
        return false;
    }

    const auto storage_size = static_cast<size_t>(header.count) * VIEW_STATE_REFERENCE_STRIDE;
    if (storage_size / VIEW_STATE_REFERENCE_STRIDE != static_cast<size_t>(header.count) ||
        !is_readable_process_range(header.data, storage_size))
    {
        return false;
    }

    uintptr_t expected_vtable{};
    for (int32_t i = 0; i < header.count; ++i) {
        const auto element = header.data + static_cast<uintptr_t>(i) * VIEW_STATE_REFERENCE_STRIDE;
        uintptr_t vtable{};
        uintptr_t first_virtual{};
        uintptr_t state{};

        if (!safe_read_value(element, vtable) ||
            vtable == 0 ||
            !safe_read_value(vtable, first_virtual) ||
            first_virtual == 0 ||
            !is_executable_process_range(first_virtual, 1) ||
            !utility::get_module_within(reinterpret_cast<void*>(vtable)).has_value() ||
            !safe_read_value(element + sizeof(uintptr_t), state))
        {
            return false;
        }

        if (expected_vtable == 0) {
            expected_vtable = vtable;
        } else if (vtable != expected_vtable) {
            return false;
        }

        if (state != 0 &&
            !ghosting_is_valid_scene_state(reinterpret_cast<sdk::FSceneViewStateInterface*>(state)))
        {
            return false;
        }
    }

    out = {
        .header_address = header_address,
        .header = header,
        .reference_vtable = expected_vtable,
    };
    return true;
}

bool ue425_426_view_state_is_valid(
    const LegacyLocalPlayerViewStatesSnapshot& snapshot,
    int32_t index)
{
    constexpr uint32_t VIEW_STATE_REFERENCE_STRIDE = 0x28;

    if (index < 0 || index >= snapshot.header.count) {
        return false;
    }

    uintptr_t state{};
    const auto state_slot = snapshot.header.data +
        static_cast<uintptr_t>(index) * VIEW_STATE_REFERENCE_STRIDE +
        sizeof(uintptr_t);
    if (!safe_read_value(state_slot, state) || state == 0) {
        return false;
    }

    return ghosting_is_valid_scene_state(
        reinterpret_cast<sdk::FSceneViewStateInterface*>(state));
}

bool ue425_426_view_states_have_distinct_pair(const LegacyLocalPlayerViewStatesSnapshot& snapshot) {
    constexpr uint32_t VIEW_STATE_REFERENCE_STRIDE = 0x28;

    if (snapshot.header.count < 2 ||
        !ue425_426_view_state_is_valid(snapshot, 0) ||
        !ue425_426_view_state_is_valid(snapshot, 1))
    {
        return false;
    }

    uintptr_t left{};
    uintptr_t right{};
    safe_read_value(snapshot.header.data + sizeof(uintptr_t), left);
    safe_read_value(
        snapshot.header.data + VIEW_STATE_REFERENCE_STRIDE + sizeof(uintptr_t),
        right);
    return left != right;
}

bool ue425_426_read_view_state_pair(
    const LegacyLocalPlayerViewStatesSnapshot& snapshot,
    sdk::FSceneViewStateInterface*& left,
    sdk::FSceneViewStateInterface*& right)
{
    constexpr uint32_t VIEW_STATE_REFERENCE_STRIDE = 0x28;
    uintptr_t left_address{};
    uintptr_t right_address{};

    if (!ue425_426_view_states_have_distinct_pair(snapshot) ||
        !safe_read_value(snapshot.header.data + sizeof(uintptr_t), left_address) ||
        !safe_read_value(
            snapshot.header.data + VIEW_STATE_REFERENCE_STRIDE + sizeof(uintptr_t),
            right_address))
    {
        return false;
    }

    left = reinterpret_cast<sdk::FSceneViewStateInterface*>(left_address);
    right = reinterpret_cast<sdk::FSceneViewStateInterface*>(right_address);
    return ghosting_is_valid_scene_state(left) &&
        ghosting_is_valid_scene_state(right) &&
        left != right;
}

std::optional<LegacyLocalPlayerViewStatesSnapshot> resolve_ue425_426_view_states(uintptr_t localplayer) {
    constexpr uintptr_t STOCK_VIEW_STATES_OFFSET = 0xA8;
    std::array<uintptr_t, 2> candidates{};
    size_t candidate_count{};

    const auto add_candidate = [&](uintptr_t candidate) {
        if (candidate == 0 || candidate < localplayer) {
            return;
        }

        for (size_t i = 0; i < candidate_count; ++i) {
            if (candidates[i] == candidate) {
                return;
            }
        }

        if (candidate_count < candidates.size()) {
            candidates[candidate_count++] = candidate;
        }
    };

    // UE4.25.4 and UE4.26.0 source place the private ViewStates TArray
    // immediately before the reflected ControllerId. Prefer that relationship
    // so licensee builds do not depend on the stock absolute class offset.
    try {
        auto* const local_player_object = reinterpret_cast<sdk::UObject*>(localplayer);
        const auto controller_id_data =
            reinterpret_cast<uintptr_t>(local_player_object->get_property_data(L"ControllerId"));
        if (controller_id_data >= localplayer + sizeof(GhostingRawArrayHeader) &&
            controller_id_data < localplayer + 0x1000)
        {
            add_candidate(controller_id_data - sizeof(GhostingRawArrayHeader));
        }
    } catch (...) {
    }

    // Shipping PDBs for the stock 4.25/4.25Plus/4.26 layout put ViewStates at
    // 0xA8. It remains a validated fallback when reflected property lookup is
    // unavailable, never an unchecked write target.
    add_candidate(localplayer + STOCK_VIEW_STATES_OFFSET);

    std::optional<LegacyLocalPlayerViewStatesSnapshot> resolved{};
    for (size_t i = 0; i < candidate_count; ++i) {
        LegacyLocalPlayerViewStatesSnapshot candidate{};
        if (!validate_ue425_426_view_states_array(candidates[i], candidate)) {
            continue;
        }

        if (resolved && resolved->header_address != candidate.header_address) {
            SPDLOG_WARN(
                "[NativeStereoFix][UE4.25/4.26] Refusing ambiguous LocalPlayer ViewStates candidates {:x} and {:x}",
                resolved->header_address,
                candidate.header_address);
            return std::nullopt;
        }

        resolved = candidate;
    }

    return resolved;
}

bool ghosting_object_array_contains(
    uintptr_t object,
    int32_t internal_index,
    bool validate_serial,
    int32_t expected_serial,
    int32_t* out_serial)
{
    auto* const object_array = sdk::FUObjectArray::get();
    if (object_array == nullptr) {
        return false;
    }

    __try {
        const auto object_count = object_array->get_object_count();
        if (object_count <= 0 || internal_index < 0 || internal_index >= object_count) {
            return false;
        }

        auto* const item = object_array->get_object(internal_index);
        if (item == nullptr) {
            return false;
        }

        const auto serial = item->get_serial_number();
        if (item->get_object() != reinterpret_cast<sdk::UObjectBase*>(object) ||
            (validate_serial && serial != expected_serial))
        {
            return false;
        }

        if (out_serial != nullptr) {
            *out_serial = serial;
        }

        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ghosting_object_vtable_matches(void* object, void** expected_vtable) {
    if (object == nullptr || expected_vtable == nullptr) {
        return false;
    }

    __try {
        return *reinterpret_cast<void***>(object) == expected_vtable;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ghosting_can_use_uobject_hook() {
    auto& object_hook = UObjectHook::get();
    return object_hook != nullptr &&
        object_hook->is_fully_hooked() &&
        !object_hook->is_disabled();
}

bool ghosting_is_live_uobject(
    sdk::UObject* object,
    GhostingUObjectValidationMode validation_mode = GhostingUObjectValidationMode::ObjectArray,
    const GhostingUObjectIdentity* expected_identity = nullptr,
    GhostingUObjectIdentity* out_identity = nullptr,
    bool validate_membership = true)
{
    const auto address = reinterpret_cast<uintptr_t>(object);
    if (address == 0) {
        return false;
    }

    uintptr_t vtable{};
    uintptr_t first_virtual{};
    uintptr_t object_class{};
    int32_t internal_index{-1};
    if (!safe_read_value(address, vtable) ||
        vtable == 0 ||
        !safe_read_value(vtable, first_virtual) ||
        first_virtual == 0 ||
        !is_executable_process_range(first_virtual, 1) ||
        !safe_read_value(address + sdk::UObjectBase::get_class_private_offset(), object_class) ||
        object_class == 0 ||
        !safe_read_value(address + sdk::UObjectBase::get_internal_index_offset(), internal_index))
    {
        return false;
    }

    if (expected_identity != nullptr &&
        (vtable != expected_identity->vtable ||
         object_class != expected_identity->object_class ||
         internal_index != expected_identity->internal_index))
    {
        return false;
    }

    int32_t serial{};
    if (validate_membership) {
        if (validation_mode == GhostingUObjectValidationMode::ObjectArray) {
            if (!ghosting_object_array_contains(
                    address,
                    internal_index,
                    expected_identity != nullptr,
                    expected_identity != nullptr ? expected_identity->serial : 0,
                    &serial))
            {
                return false;
            }
        } else {
            auto& object_hook = UObjectHook::get();
            if (!ghosting_can_use_uobject_hook() ||
                !object_hook->exists(reinterpret_cast<sdk::UObjectBase*>(object)))
            {
                return false;
            }
        }
    }

    if (out_identity != nullptr) {
        *out_identity = {
            .vtable = vtable,
            .object_class = object_class,
            .internal_index = internal_index,
            .serial = serial,
        };
    }

    return true;
}

bool ghosting_read_array_header(
    uintptr_t address,
    int32_t maximum_count,
    int32_t maximum_capacity,
    GhostingRawArrayHeader& out)
{
    GhostingRawArrayHeader header{};
    if (!safe_read_value(address, header) ||
        header.data == 0 ||
        (header.data & (alignof(void*) - 1)) != 0 ||
        header.count <= 0 ||
        header.count > maximum_count ||
        header.capacity < header.count ||
        header.capacity > maximum_capacity)
    {
        return false;
    }

    out = header;
    return true;
}

bool ghosting_read_object_property(
    sdk::UObject* object,
    std::wstring_view property,
    uintptr_t& out_slot,
    sdk::UObject*& out_value,
    GhostingUObjectValidationMode validation_mode)
{
    if (!ghosting_is_live_uobject(object, validation_mode)) {
        return false;
    }

    try {
        const auto property_data = reinterpret_cast<uintptr_t>(object->get_property_data(property));
        uintptr_t value{};
        if (property_data == 0 || !safe_read_value(property_data, value)) {
            return false;
        }

        out_slot = property_data;
        out_value = reinterpret_cast<sdk::UObject*>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool ghosting_resolve_view_state_slots(
    uintptr_t header_address,
    sdk::FSceneViewStateInterface* left_state,
    sdk::FSceneViewStateInterface* right_state,
    GhostingFixOwnerCandidate& out)
{
    GhostingRawArrayHeader header{};
    if (!ghosting_read_array_header(header_address, 8, 16, header)) {
        return false;
    }

    const bool prefers_modern_stride =
        is_ue_5_5_runtime() || is_ue_5_6_or_newer();
    const std::array<uint32_t, 2> strides = prefers_modern_stride
        ? std::array<uint32_t, 2>{0x38, 0x28}
        : std::array<uint32_t, 2>{0x28, 0x38};

    for (const auto stride : strides) {
        const auto storage_size = static_cast<size_t>(header.count) * stride;
        if (storage_size / stride != static_cast<size_t>(header.count) ||
            !is_readable_process_range(header.data, storage_size))
        {
            continue;
        }

        uintptr_t state_slots[2]{};
        uintptr_t expected_vtable{};
        bool valid_layout = true;

        for (int32_t i = 0; i < header.count; ++i) {
            const auto element = header.data + static_cast<uintptr_t>(i) * stride;
            uintptr_t vtable{};
            uintptr_t first_virtual{};
            uintptr_t state{};
            if (!safe_read_value(element, vtable) ||
                vtable == 0 ||
                !safe_read_value(vtable, first_virtual) ||
                first_virtual == 0 ||
                !is_executable_process_range(first_virtual, 1) ||
                !safe_read_value(element + sizeof(uintptr_t), state))
            {
                valid_layout = false;
                break;
            }

            if (expected_vtable == 0) {
                expected_vtable = vtable;
            } else if (vtable != expected_vtable) {
                valid_layout = false;
                break;
            }

            if (state != 0 &&
                !ghosting_is_valid_scene_state(reinterpret_cast<sdk::FSceneViewStateInterface*>(state)))
            {
                valid_layout = false;
                break;
            }

            if (state == reinterpret_cast<uintptr_t>(left_state)) {
                state_slots[0] = element + sizeof(uintptr_t);
            }
            if (state == reinterpret_cast<uintptr_t>(right_state)) {
                state_slots[1] = element + sizeof(uintptr_t);
            }
        }

        if (!valid_layout || state_slots[0] == 0 || state_slots[1] == 0 || state_slots[0] == state_slots[1]) {
            continue;
        }

        out.view_states_header = header_address;
        out.view_states_data = header.data;
        out.view_states_count = header.count;
        out.view_states_capacity = header.capacity;
        out.view_state_stride = stride;
        out.view_state_reference_vtable = expected_vtable;
        out.eye_state_slot[0] = state_slots[0];
        out.eye_state_slot[1] = state_slots[1];
        out.view_states_are_array = true;
        return true;
    }

    return false;
}

bool ghosting_resolve_direct_view_state_slots(
    uintptr_t local_player_address,
    uintptr_t controller_id_data,
    sdk::FSceneViewStateInterface* left_state,
    sdk::FSceneViewStateInterface* right_state,
    GhostingFixOwnerCandidate& out)
{
    if (controller_id_data <= local_player_address + sdk::UObjectBase::get_class_size()) {
        return false;
    }

    const auto lower_bound = std::max(
        local_player_address + sdk::UObjectBase::get_class_size(),
        controller_id_data > 0x180 ? controller_id_data - 0x180 : local_player_address);
    const auto first_candidate = (lower_bound + alignof(void*) - 1) & ~(alignof(void*) - 1);
    const std::array<uint32_t, 5> strides{0x28, 0x38, 0x20, 0x30, 0x40};

    for (const auto stride : strides) {
        for (auto first = first_candidate;
             first + stride + (2 * sizeof(uintptr_t)) <= controller_id_data;
             first += sizeof(uintptr_t))
        {
            const auto second = first + stride;
            uintptr_t first_vtable{};
            uintptr_t second_vtable{};
            uintptr_t first_virtual{};
            uintptr_t first_state{};
            uintptr_t second_state{};
            if (!safe_read_value(first, first_vtable) ||
                first_vtable == 0 ||
                !safe_read_value(second, second_vtable) ||
                first_vtable != second_vtable ||
                !safe_read_value(first_vtable, first_virtual) ||
                first_virtual == 0 ||
                !is_executable_process_range(first_virtual, 1) ||
                !safe_read_value(first + sizeof(uintptr_t), first_state) ||
                !safe_read_value(second + sizeof(uintptr_t), second_state))
            {
                continue;
            }

            const auto left = reinterpret_cast<uintptr_t>(left_state);
            const auto right = reinterpret_cast<uintptr_t>(right_state);
            const bool natural_order = first_state == left && second_state == right;
            const bool swapped_order = first_state == right && second_state == left;
            if (!natural_order && !swapped_order) {
                continue;
            }

            out.view_states_header = 0;
            out.view_states_data = first;
            out.view_states_count = 2;
            out.view_states_capacity = 2;
            out.view_state_stride = stride;
            out.view_state_reference_vtable = first_vtable;
            out.eye_state_slot[0] = natural_order
                ? first + sizeof(uintptr_t)
                : second + sizeof(uintptr_t);
            out.eye_state_slot[1] = natural_order
                ? second + sizeof(uintptr_t)
                : first + sizeof(uintptr_t);
            out.view_states_are_array = false;
            return true;
        }
    }

    return false;
}

bool ghosting_resolve_current_owner(
    sdk::FSceneViewStateInterface* left_state,
    sdk::FSceneViewStateInterface* right_state,
    GhostingFixOwnerCandidate& out,
    GhostingUObjectValidationMode validation_mode,
    GhostingOwnerResolveDiagnostic& diagnostic)
{
    diagnostic = {};
    if (!ghosting_is_valid_scene_state(left_state) ||
        !ghosting_is_valid_scene_state(right_state) ||
        left_state == right_state)
    {
        diagnostic.failure = GhostingOwnerResolveFailure::InvalidSceneStates;
        return false;
    }

    auto* const engine = reinterpret_cast<sdk::UObject*>(sdk::UEngine::get());
    if (engine == nullptr) {
        diagnostic.failure = GhostingOwnerResolveFailure::EngineUnavailable;
        return false;
    }

    GhostingUObjectIdentity engine_identity{};
    if (!ghosting_is_live_uobject(engine, validation_mode, nullptr, &engine_identity)) {
        diagnostic.failure = GhostingOwnerResolveFailure::EngineLifetime;
        return false;
    }

    uintptr_t game_instance_slot{};
    sdk::UObject* game_instance{};
    if (!ghosting_read_object_property(
            engine,
            L"GameInstance",
            game_instance_slot,
            game_instance,
            validation_mode))
    {
        diagnostic.failure = GhostingOwnerResolveFailure::GameInstanceProperty;
        return false;
    }

    GhostingUObjectIdentity game_instance_identity{};
    if (!ghosting_is_live_uobject(game_instance, validation_mode, nullptr, &game_instance_identity)) {
        diagnostic.failure = GhostingOwnerResolveFailure::GameInstanceLifetime;
        return false;
    }

    uintptr_t local_players_header{};
    try {
        local_players_header = reinterpret_cast<uintptr_t>(game_instance->get_property_data(L"LocalPlayers"));
    } catch (...) {
        diagnostic.failure = GhostingOwnerResolveFailure::LocalPlayersProperty;
        return false;
    }

    if (local_players_header == 0) {
        diagnostic.failure = GhostingOwnerResolveFailure::LocalPlayersProperty;
        return false;
    }

    GhostingRawArrayHeader local_players{};
    if (!ghosting_read_array_header(local_players_header, 8, 32, local_players) ||
        !is_readable_process_range(local_players.data, static_cast<size_t>(local_players.count) * sizeof(uintptr_t)))
    {
        diagnostic.failure = GhostingOwnerResolveFailure::LocalPlayersArray;
        return false;
    }

    diagnostic.local_player_count = local_players.count;
    diagnostic.failure = GhostingOwnerResolveFailure::LocalPlayerSlot;

    for (int32_t player_ordinal = 0; player_ordinal < local_players.count; ++player_ordinal) {
        diagnostic.local_player_index = player_ordinal;
        const auto local_player_slot =
            local_players.data + static_cast<uintptr_t>(player_ordinal) * sizeof(uintptr_t);
        uintptr_t local_player_address{};
        if (!safe_read_value(local_player_slot, local_player_address)) {
            continue;
        }

        auto* const local_player = reinterpret_cast<sdk::UObject*>(local_player_address);
        GhostingUObjectIdentity local_player_identity{};
        if (!ghosting_is_live_uobject(local_player, validation_mode, nullptr, &local_player_identity)) {
            diagnostic.failure = GhostingOwnerResolveFailure::LocalPlayerLifetime;
            continue;
        }

        uintptr_t controller_id_data{};
        uintptr_t viewport_override_data{};
        try {
            controller_id_data = reinterpret_cast<uintptr_t>(local_player->get_property_data(L"ControllerId"));
        } catch (...) {
            controller_id_data = 0;
        }
        try {
            viewport_override_data = reinterpret_cast<uintptr_t>(local_player->get_property_data(L"ViewportClientOverride"));
        } catch (...) {
            viewport_override_data = 0;
        }

        if (controller_id_data == 0) {
            diagnostic.failure = GhostingOwnerResolveFailure::ViewStateStorage;
            continue;
        }

        FixedCapacityList<uintptr_t, 20> view_state_headers{};
        const auto add_header = [&](uintptr_t address) {
            if (address < local_player_address + sdk::UObjectBase::get_class_size() ||
                address >= local_player_address + 0x800 ||
                (address & (alignof(void*) - 1)) != 0)
            {
                return;
            }

            for (const auto existing : view_state_headers) {
                if (existing == address) {
                    return;
                }
            }

            view_state_headers.try_push_back(address);
        };

        if (viewport_override_data >= sizeof(GhostingRawArrayHeader)) {
            add_header(viewport_override_data - sizeof(GhostingRawArrayHeader));
        }
        if (controller_id_data >= sizeof(GhostingRawArrayHeader)) {
            add_header(controller_id_data - sizeof(GhostingRawArrayHeader));

            // ViewStates is immediately before ControllerId in stock array layouts.
            // The bounded candidates remain exact-pair validated for licensee padding.
            for (uintptr_t distance = 0x18; distance <= 0x80; distance += sizeof(uintptr_t)) {
                if (controller_id_data >= distance) {
                    add_header(controller_id_data - distance);
                }
            }
        }

        GhostingFixOwnerCandidate candidate{};
        bool found_view_states = false;
        for (const auto header : view_state_headers) {
            candidate = {};
            if (ghosting_resolve_view_state_slots(header, left_state, right_state, candidate)) {
                found_view_states = true;
                break;
            }
        }

        // UE4.11-4.24 use adjacent ViewState/StereoViewState references instead
        // of the later TArray. Accept only two contiguous wrappers containing
        // this exact learned pair.
        if (!found_view_states) {
            candidate = {};
            found_view_states = ghosting_resolve_direct_view_state_slots(
                local_player_address,
                controller_id_data,
                left_state,
                right_state,
                candidate);
        }

        if (!found_view_states) {
            diagnostic.failure = GhostingOwnerResolveFailure::ViewStateStorage;
            continue;
        }

        uintptr_t viewport_client_slot{};
        sdk::UObject* viewport_client{};
        GhostingUObjectIdentity viewport_client_identity{};
        uintptr_t world_slot{};
        sdk::UObject* world{};
        GhostingUObjectIdentity world_identity{};
        bool saw_live_viewport = false;
        const auto try_viewport_client = [&](std::wstring_view property) {
            uintptr_t candidate_slot{};
            sdk::UObject* candidate_viewport{};
            GhostingUObjectIdentity candidate_viewport_identity{};
            if (!ghosting_read_object_property(
                    local_player,
                    property,
                    candidate_slot,
                    candidate_viewport,
                    validation_mode) ||
                !ghosting_is_live_uobject(
                    candidate_viewport,
                    validation_mode,
                    nullptr,
                    &candidate_viewport_identity))
            {
                return false;
            }

            saw_live_viewport = true;
            uintptr_t candidate_world_slot{};
            sdk::UObject* candidate_world{};
            GhostingUObjectIdentity candidate_world_identity{};
            if (!ghosting_read_object_property(
                    candidate_viewport,
                    L"World",
                    candidate_world_slot,
                    candidate_world,
                    validation_mode) ||
                !ghosting_is_live_uobject(
                    candidate_world,
                    validation_mode,
                    nullptr,
                    &candidate_world_identity))
            {
                return false;
            }

            viewport_client_slot = candidate_slot;
            viewport_client = candidate_viewport;
            viewport_client_identity = candidate_viewport_identity;
            world_slot = candidate_world_slot;
            world = candidate_world;
            world_identity = candidate_world_identity;
            return true;
        };

        if (!try_viewport_client(L"ViewportClientOverride") &&
            !try_viewport_client(L"ViewportClient"))
        {
            diagnostic.failure = saw_live_viewport
                ? GhostingOwnerResolveFailure::World
                : GhostingOwnerResolveFailure::ViewportClient;
            continue;
        }

        candidate.engine = engine;
        candidate.engine_vtable = engine_identity.vtable;
        candidate.engine_class = engine_identity.object_class;
        candidate.engine_index = engine_identity.internal_index;
        candidate.engine_serial = engine_identity.serial;
        candidate.game_instance_slot = game_instance_slot;
        candidate.game_instance = game_instance;
        candidate.game_instance_vtable = game_instance_identity.vtable;
        candidate.game_instance_class = game_instance_identity.object_class;
        candidate.game_instance_index = game_instance_identity.internal_index;
        candidate.game_instance_serial = game_instance_identity.serial;
        candidate.local_players_header = local_players_header;
        candidate.local_players_data = local_players.data;
        candidate.local_players_count = local_players.count;
        candidate.local_players_capacity = local_players.capacity;
        candidate.local_player_slot = local_player_slot;
        candidate.local_player = local_player;
        candidate.local_player_vtable = local_player_identity.vtable;
        candidate.local_player_class = local_player_identity.object_class;
        candidate.local_player_index = local_player_identity.internal_index;
        candidate.local_player_serial = local_player_identity.serial;
        candidate.viewport_client_slot = viewport_client_slot;
        candidate.viewport_client = viewport_client;
        candidate.viewport_client_vtable = viewport_client_identity.vtable;
        candidate.viewport_client_class = viewport_client_identity.object_class;
        candidate.viewport_client_index = viewport_client_identity.internal_index;
        candidate.viewport_client_serial = viewport_client_identity.serial;
        candidate.world_slot = world_slot;
        candidate.world = world;
        candidate.world_vtable = world_identity.vtable;
        candidate.world_class = world_identity.object_class;
        candidate.world_index = world_identity.internal_index;
        candidate.world_serial = world_identity.serial;
        candidate.uses_uobject_hook_validation =
            validation_mode == GhostingUObjectValidationMode::UObjectHook;
        out = candidate;
        diagnostic.failure = GhostingOwnerResolveFailure::None;
        return true;
    }

    return false;
}

bool avowed_is_live_uobject(uintptr_t object, uintptr_t* out_vtable = nullptr, uintptr_t* out_class = nullptr) {
    if (!avowed_is_current_game() || object == 0) {
        return false;
    }

    uintptr_t vtable{};
    if (!safe_read_value(object, vtable) || !looks_like_virtual_function_table(vtable)) {
        return false;
    }

    uintptr_t cls{};
    if (!safe_read_value(object + sdk::UObjectBase::get_class_private_offset(), cls) || cls == 0) {
        return false;
    }

    uint32_t internal_index{};
    if (!safe_read_value(object + sdk::UObjectBase::get_internal_index_offset(), internal_index)) {
        return false;
    }

    auto object_array = sdk::FUObjectArray::get();
    if (object_array == nullptr) {
        return false;
    }

    const auto object_count = object_array->get_object_count();
    if (object_count <= 0 || internal_index >= (uint32_t)object_count) {
        return false;
    }

    auto item = object_array->get_object((int32_t)internal_index);
    if (item == nullptr || !is_readable_process_range((uintptr_t)item, sizeof(sdk::FUObjectItem))) {
        return false;
    }

    uintptr_t item_object{};
    if (!safe_read_value((uintptr_t)item + sdk::FUObjectArray::get_item_object_offset(), item_object) || item_object != object) {
        return false;
    }

    if (out_vtable != nullptr) {
        *out_vtable = vtable;
    }

    if (out_class != nullptr) {
        *out_class = cls;
    }

    return true;
}

bool dune_is_live_uobject(uintptr_t object, uintptr_t* out_class = nullptr) {
    if (!dune_awakening_is_current_game() || object == 0) {
        return false;
    }

    uintptr_t vtable{};
    if (!safe_read_value(object, vtable) || !looks_like_virtual_function_table(vtable)) {
        return false;
    }

    uintptr_t cls{};
    if (!safe_read_value(object + sdk::UObjectBase::get_class_private_offset(), cls) || cls == 0) {
        return false;
    }

    uint32_t internal_index{};
    if (!safe_read_value(object + sdk::UObjectBase::get_internal_index_offset(), internal_index)) {
        return false;
    }

    auto* object_array = sdk::FUObjectArray::get();
    if (object_array == nullptr) {
        return false;
    }

    const auto object_count = object_array->get_object_count();
    if (object_count <= 0 || internal_index >= (uint32_t)object_count) {
        return false;
    }

    auto* item = object_array->get_object((int32_t)internal_index);
    if (item == nullptr || !is_readable_process_range((uintptr_t)item, sizeof(sdk::FUObjectItem))) {
        return false;
    }

    uintptr_t item_object{};
    if (!safe_read_value((uintptr_t)item + sdk::FUObjectArray::get_item_object_offset(), item_object) || item_object != object) {
        return false;
    }

    if (out_class != nullptr) {
        *out_class = cls;
    }

    return true;
}

struct DunePlayerState {
    sdk::UObjectBase* object{};
    sdk::UClass* object_class{};
    bool character_creation{};
    bool playable_world{};
    bool from_tracked_objects{};
    std::string full_name{};
};

DunePlayerState classify_dune_player_object(
    sdk::UObjectBase* object,
    sdk::UClass* player_character_class,
    sdk::UClass* character_creation_class,
    bool from_tracked_objects)
{
    DunePlayerState result{};
    uintptr_t object_class{};

    if (object == nullptr ||
        !dune_is_live_uobject(reinterpret_cast<uintptr_t>(object), &object_class) ||
        object_class == 0)
    {
        return result;
    }

    auto* const uclass = reinterpret_cast<sdk::UClass*>(object_class);
    if (player_character_class != nullptr && !uclass->is_a(player_character_class)) {
        return result;
    }

    try {
        result.full_name = utility::narrow(object->get_full_name());
    } catch (...) {
        return {};
    }

    if (result.full_name.empty() ||
        result.full_name.find("Default__") != std::string::npos)
    {
        return {};
    }

    result.object = object;
    result.object_class = uclass;
    result.from_tracked_objects = from_tracked_objects;
    result.character_creation =
        character_creation_class != nullptr &&
        uclass->is_a(character_creation_class);

    // Runtime actors include a map/level path. This excludes class defaults,
    // preview assets, and other persistent metadata tracked under the same
    // native DunePlayerCharacter base class.
    const bool is_runtime_actor =
        result.full_name.find("PersistentLevel.") != std::string::npos ||
        result.full_name.find("/Game/Dune/Maps/") != std::string::npos;
    result.playable_world = is_runtime_actor && !result.character_creation;
    return result;
}

DunePlayerState detect_dune_player_state() {
    static sdk::UClass* player_character_class = nullptr;
    static sdk::UClass* character_creation_class = nullptr;

    if (player_character_class == nullptr) {
        player_character_class =
            sdk::find_uobject<sdk::UClass>(L"Class /Script/DuneSandbox.DunePlayerCharacter", false);
    }

    if (character_creation_class == nullptr) {
        character_creation_class =
            sdk::find_uobject<sdk::UClass>(L"Class /Script/DuneSandbox.DuneCharacterCreationCharacter", false);
    }

    auto* const engine = sdk::UEngine::get();
    auto* const local_pawn = engine != nullptr ? engine->get_localpawn(0) : nullptr;
    auto state = classify_dune_player_object(
        reinterpret_cast<sdk::UObjectBase*>(local_pawn),
        player_character_class,
        character_creation_class,
        false);

    if (state.character_creation || state.playable_world) {
        return state;
    }

    // Dune keeps a Cartography showroom world visible to UEngine while the
    // actual NPE/gameplay pawn lives in another world. UObjectHook already
    // tracks derived instances by every superclass, so use that authoritative
    // set instead of sweeping GUObjectArray on the game thread.
    auto& object_hook = UObjectHook::get();
    if (object_hook == nullptr || object_hook->is_disabled() || player_character_class == nullptr) {
        return {};
    }

    DunePlayerState character_creation_state{};
    for (auto* const object : object_hook->get_objects_by_class(player_character_class)) {
        auto candidate = classify_dune_player_object(
            object,
            player_character_class,
            character_creation_class,
            true);

        if (candidate.playable_world) {
            return candidate;
        }

        if (candidate.character_creation && character_creation_state.object == nullptr) {
            character_creation_state = std::move(candidate);
        }
    }

    return character_creation_state;
}

std::optional<uintptr_t> locate_vtable_from_constructor_rip_references(uintptr_t constructor) {
    constexpr auto MAX_CONSTRUCTOR_SCAN_BYTES = 0x800;
    auto best_candidate = std::optional<uintptr_t>{};

    for (auto ip = constructor; ip < constructor + MAX_CONSTRUCTOR_SCAN_BYTES;) {
        const auto decoded = utility::decode_one((uint8_t*)ip);

        if (!decoded || decoded->Length == 0) {
            break;
        }

        if (decoded->OperandsCount >= 2 &&
            decoded->IsRipRelative &&
            (decoded->Instruction == ND_INS_LEA || decoded->Instruction == ND_INS_MOV) &&
            decoded->Operands[0].Type == ND_OP_REG &&
            decoded->Operands[1].Type == ND_OP_MEM)
        {
            const auto referenced_addr = utility::resolve_displacement(ip);

            if (referenced_addr && looks_like_virtual_function_table(*referenced_addr)) {
                SPDLOG_INFO("Found FFakeStereoRendering vtable candidate via constructor RIP reference at {:x} -> {:x}",
                            ip, *referenced_addr);
                best_candidate = *referenced_addr;
                break;
            }
        }

        if (std::string_view{decoded->Mnemonic}.starts_with("RET")) {
            break;
        }

        ip += decoded->Length;
    }

    return best_candidate;
}
}

// Scan through function instructions to detect usage of double
// floating point precision instructions.
bool is_using_double_precision(uintptr_t addr) {
    SPDLOG_INFO("Scanning function at {:x} for double precision usage", addr);

    bool result = false;

    utility::exhaustive_decode((uint8_t*)addr, 50, [&](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
        if (std::string_view{ix.Mnemonic}.starts_with("CALL")) {
            return utility::ExhaustionResult::STEP_OVER;
        }

        if (ix.Instruction == ND_INS_MOVSD && ix.Operands[0].Type == ND_OP_MEM && ix.Operands[1].Type == ND_OP_REG) {
            SPDLOG_INFO("[UE5 Detected] Detected Double precision MOVSD at {:x}", (uintptr_t)ip);
            result = true;
            return utility::ExhaustionResult::BREAK;
        }

        if (ix.Instruction == ND_INS_ADDSD) {
            SPDLOG_INFO("[UE5 Detected] Detected Double precision ADDSD at {:x}", (uintptr_t)ip);
            result = true;
            return utility::ExhaustionResult::BREAK;
        }

        return utility::ExhaustionResult::CONTINUE;
    });

    return result;
}

FFakeStereoRenderingHook::FFakeStereoRenderingHook() {
    g_hook = this;
    setup_options();
}

void FFakeStereoRenderingHook::on_frame() {
    // Engine tick hook is always installed — plugins (including dumper-mode
    // clients) need on_pre_engine_tick callbacks to submit game-thread work.
    attempt_hook_game_engine_tick();

    // Render-pipeline hooks are the crash vector on some UE4.26.x forks
    // (RoboQuest, Stellar Blade). Dumper mode skips them entirely so
    // reflection-only plugins can run without triggering the
    // FViewport::GetRenderTargetTexture PointerHook that tears down the
    // render thread. See DumperMode.hpp.
    if (uevr::is_dumper_mode()) {
        return;
    }

    attempt_hook_slate_thread();
    attempt_hook_fsceneview_constructor();

    // Ideally we want to do all hooking
    // from game engine tick. if it fails
    // we will fall back to doing it here.
    if (!m_hooked_game_engine_tick && m_attempted_hook_game_engine_tick) {
        attempt_hooking();
    }
}


void FFakeStereoRenderingHook::on_draw_ui() {
    ZoneScopedN(__FUNCTION__);

    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNode("Stereo Hook Options")) {
        m_asynchronous_scan->draw("Asynchronous Code Scanning");
        m_recreate_textures_on_reset->draw("Recreate Textures on Reset");
        m_frame_delay_compensation->draw("Frame Delay Compensation");
        m_use_fmalloc_scene_view_extensions->draw("Use FMalloc for ISceneViewExtensions");

        if (m_tracking_system_hook != nullptr) {
            m_tracking_system_hook->on_draw_ui();
        }

#if 0
        if (ImGui::Button("Spawn scene capture")) {
            get_render_target_manager()->create_scene_capture();
        }

        ImGui::SameLine();
        if (ImGui::Button("Destroy scene capture")) {
            get_render_target_manager()->destroy_scene_capture();
        }

        ImGui::SameLine();
        if (ImGui::Button("Create texture")) {
            get_render_target_manager()->create_scene_capture_texture();
        }

        ImGui::SameLine();
        if (ImGui::Button("Destroy texture")) {
            get_render_target_manager()->destroy_scene_capture();
        }

        bool status = false;

        if (get_render_target_manager()->get_scene_capture_utexture() != nullptr) {
            if (UObjectHook::get()->exists(get_render_target_manager()->get_scene_capture_utexture())) {
                status = true;
            }
        }
        ImGui::Text("Scene Capture Texture: %s", status ? "Exists" : "Does not exist");
#endif

        auto& data = m_viewport_rt_hook_data;
        std::scoped_lock _{data.retaddr_mutex};

        std::vector<uintptr_t> retaddrs{};
        std::vector<std::string> items{};
        for (auto& addr : data.seen_retaddrs) {
            items.push_back(fmt::format("{:x}", addr));
            retaddrs.push_back(addr);
        }
        
        std::vector<const char*> citems{};
        for (auto& item : items) {
            citems.push_back(item.c_str());
        }

        if (!items.empty()) {
            if (ImGui::BeginCombo("GetRenderTargetTexture Retaddrs", items[data.selected_retaddr].c_str())) {
                for (int n = 0; n < items.size(); n++) {
                    ImGui::PushID(n);
                    auto retaddr = retaddrs[n];
                    const bool is_selected = (data.selected_retaddr == n);

                    // Calculate the text size for the current item
                    const auto text_size = ImGui::CalcTextSize(items[n].c_str(), NULL, true);
                    const auto padding = ImGui::GetStyle().ItemSpacing.x;
                    const auto selectable_size = ImVec2{text_size.x + padding, text_size.y};

                    if (ImGui::Selectable(items[n].c_str(), is_selected, ImGuiSelectableFlags_None, selectable_size)) {
                        data.selected_retaddr = n;
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Call Original")) {
                        data.call_original_retaddrs.insert(retaddr);
                        data.redirected_retaddrs.erase(retaddr);
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Redirect")) {
                        data.redirected_retaddrs.insert(retaddr);
                        data.call_original_retaddrs.erase(retaddr);
                    }

                    ImGui::SameLine();
                    if (data.call_original_retaddrs.contains(retaddr)) {
                        ImGui::Text("[Calling Original]");
                    } else if (data.redirected_retaddrs.contains(retaddr)) {
                        ImGui::Text("[Redirected]");
                    } else {
                        ImGui::Text("[Default]");
                    }

                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }

        }

        ImGui::TreePop();
    }

    ImGui::Separator();
}

void FFakeStereoRenderingHook::attempt_hooking() {
    if (m_finished_hooking || m_tried_hooking) {
        return;
    }

    // TODO: see if this can be threaded; it might not be able to because of TLS or something
    if (!VR::get()->should_skip_uobjectarray_init()) {
        sdk::FName::get_constructor();
        sdk::FName::get_to_string();
        sdk::FUObjectArray::get();
    }

    if (!m_injected_stereo_at_runtime) {
        attempt_runtime_inject_stereo();
        m_injected_stereo_at_runtime = true;
    }
    
    m_hooked = hook();
}

namespace detail{
bool pre_find_engine_tick() {
    ZoneScopedN(__FUNCTION__);
    sdk::UGameEngine::get_tick_address(); // this takes a LONG time to find
    sdk::UGameEngine::get_initialize_hmd_device_address();
    return true;
}
}

void FFakeStereoRenderingHook::attempt_hook_game_engine_tick(uintptr_t return_address) {
    if (m_asynchronous_scan->value()) {
        static std::future<bool> future = std::async(std::launch::async, detail::pre_find_engine_tick);

        // Wait for the future to be valid before attempting to hook
        if (future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            future.get();
        } else if (future.valid()) {
            return;
        }
    }

    if (m_hooked_game_engine_tick) {
        return;
    }

    if (return_address == 0 && m_attempted_hook_game_engine_tick) {
        return;
    }
    
    SPDLOG_INFO("Attempting to hook UGameEngine::Tick!");

    m_attempted_hook_game_engine_tick = true;

    auto func = sdk::UGameEngine::get_tick_address();

    if (!func) {
        if (return_address == 0) {
            SPDLOG_ERROR("Cannot hook UGameEngine::Tick");
            return;
        }

        const auto engine_module = sdk::get_ue_module(L"Engine");
        static const auto negative_delta_time_strings = 
            utility::scan_strings(engine_module, L"Negative delta time!");
        
        if (negative_delta_time_strings.empty()) {
            SPDLOG_ERROR("Cannot hook UGameEngine::Tick (Negative delta time! not found)");
            return;
        }

        static std::vector<uintptr_t> negative_delta_time_funcs = [&]() {
            std::vector<uintptr_t> out{};

            for (auto str : negative_delta_time_strings) {
                const auto ref = utility::scan_displacement_reference(engine_module, str);

                if (!ref) {
                    continue;
                }
                //
                const auto func_start = utility::find_virtual_function_start(*ref);

                if (!func_start) {
                    continue;
                }

                SPDLOG_INFO("Negative delta time string function @ {:x}", *func_start);

                out.push_back(*func_start);
            }

            return out;
        }();

        const auto return_address_func = utility::find_virtual_function_start(return_address);

        if (!return_address_func) {
            SPDLOG_ERROR("Return address is not within a valid function!");
            return;
        }

        // Check if the return address is within one of the negative delta time functions.
        // If it is, then it's UGameEngine::Tick. Set func to the return_address_func.
        for (auto potential : negative_delta_time_funcs) {
            if (potential == *return_address_func) {
                SPDLOG_INFO("Found UGameEngine::Tick @ {:x}", *return_address_func);
                func = *return_address_func;
                break;
            }
        }

        if (!func) {
            SPDLOG_ERROR("Return address is not the correct function!");
            return;
        }
    }

    // TODO: move this to a better place
    m_tick_hook = safetyhook::create_inline((void*)*func, &engine_tick_hook, safetyhook::InlineHook::StartDisabled);

    if (!m_tick_hook) {
        SPDLOG_ERROR("Failed to hook UGameEngine::Tick!");
        return;
    }

    if (auto tick_hook_enable = m_tick_hook.enable(); !tick_hook_enable.has_value()) {
        SPDLOG_ERROR("Failed to enable UGameEngine::Tick hook! {}", (int)tick_hook_enable.error().type);
        return;
    }

    m_hooked_game_engine_tick = true;

    SPDLOG_INFO("Hooked UGameEngine::Tick!");
}

void* FFakeStereoRenderingHook::engine_tick_hook(sdk::UGameEngine* engine, float delta, bool idle) {
    ZoneScopedN("UGameEngine::Tick Hook");
    FrameMarkStart("UGameEngine::Tick");

    auto hook = g_hook;
    
    hook->m_in_engine_tick = true;

    utility::ScopeGuard _{[]() {
        g_hook->m_in_engine_tick = false;
        FrameMarkEnd("UGameEngine::Tick");
    }};
    
    static bool once = true;

    if (once) {
        SPDLOG_INFO("First time calling UGameEngine::Tick!");
        once = false;
    }

    if (!g_framework->is_game_data_intialized()) {
        // This allocates memory on the stack.
        static bool check_canary_once = true;
        volatile uint64_t shadow_space[64]{};

#ifdef NDEBUG
        if (check_canary_once) {
#endif
            std::memset((void*)shadow_space, 0, 64 * sizeof(uint64_t));
#ifdef NDEBUG
        }
#endif
        // We're using original here instead of call_unsafe to make sure the canaries are the first thing on the stack.
        void* result = hook->m_tick_hook.original<void* (*)(sdk::UGameEngine*, float, bool)>()(engine, delta, idle);

        // At least do some logic with the shadow space so it doesn't get optimized out for some reason.
        // But only do it once in release builds.
#ifdef NDEBUG
        if (check_canary_once) {
#endif
            for (size_t i = 0; i < 64; ++i) {
                if (shadow_space[i] != 0) {
                    SPDLOG_ERROR("[UGameEngine::Tick] Shadow space was overwritten! {:x} @ {}", shadow_space[i], i);
                }
            }

#ifdef NDEBUG
            check_canary_once = false;
        }
#endif

        return result;
    }

    // Dumper mode: skip render-pipeline hooks (see DumperMode.hpp). Engine
    // tick dispatch below still runs, so plugins receive on_pre_engine_tick.
    if (!uevr::is_dumper_mode()) {
        hook->attempt_hooking();
    }

    // Best place to run game thread jobs.
    GameThreadWorker::get().execute();

    if (hook->m_ignore_next_engine_tick) {
        hook->m_ignored_engine_delta = delta;
        hook->m_ignore_next_engine_tick = false;
        return nullptr;
    }

    // Dumper mode: skip the imgui-frame + engine-thread-enable logic. ImGui
    // needs a D3D device + swapchain that we never installed, and
    // enable_engine_thread is a VR-only optimization. The mod fan-out below
    // still runs, so plugins still get on_pre_engine_tick callbacks.
    if (!uevr::is_dumper_mode()) {
        g_framework->enable_engine_thread();
        g_framework->run_imgui_frame(false);
    }

    delta += hook->m_ignored_engine_delta;
    hook->m_ignored_engine_delta = 0.0f;

    if (hook->m_tracking_system_hook != nullptr) {
        hook->m_tracking_system_hook->on_pre_engine_tick(engine, delta);
    }

    const auto& mods = g_framework->get_mods()->get_mods();
    for (auto& mod : mods) {
        mod->on_pre_engine_tick(engine, delta);
    }

    void* result = nullptr;

    {
        // This allocates memory on the stack.
        static bool check_canary_once = true;
        volatile uint64_t shadow_space[64]{};

#ifdef NDEBUG
        if (check_canary_once) {
#endif
            std::memset((void*)shadow_space, 0, 64 * sizeof(uint64_t));
#ifdef NDEBUG
        }
#endif
        // We're using original here instead of call_unsafe to make sure the canaries are the first thing on the stack.
        result = hook->m_tick_hook.original<void* (*)(sdk::UGameEngine*, float, bool)>()(engine, delta, idle);

        // At least do some logic with the shadow space so it doesn't get optimized out for some reason.
        // But only do it once in release builds.
#ifdef NDEBUG
        if (check_canary_once) {
#endif
            for (size_t i = 0; i < 64; ++i) {
                if (shadow_space[i] != 0) {
                    SPDLOG_ERROR("[UGameEngine::Tick] Shadow space was overwritten! {:x} @ {}", shadow_space[i], i);
                }
            }

#ifdef NDEBUG
            check_canary_once = false;
        }
#endif
    }

    for (auto& mod : mods) {
        mod->on_post_engine_tick(engine, delta);
    }

    return result;
}

namespace detail{
bool pre_find_slate_thread() {
    sdk::slate::locate_draw_window_renderthread_fn(); // Can take a while to find
    sdk::slate::locate_draw_window_renderthread_fn_alternate();
    return true;
}
}

void FFakeStereoRenderingHook::attempt_hook_slate_thread(uintptr_t return_address, bool alternate) {
    if (m_asynchronous_scan->value()) {
        static std::future<bool> future = std::async(std::launch::async, detail::pre_find_slate_thread);

        // Wait for the future to be valid before attempting to hook
        if (future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            future.get();
        } else if (future.valid()) {
            return;
        }
    }

    if (m_hooked_slate_thread && !alternate) {
        return;
    }

    const auto attempted = alternate ? m_attempted_hook_slate_thread_alternate : m_attempted_hook_slate_thread;

    if (return_address == 0 && attempted) {
        return;
    }

    SPDLOG_INFO("Attempting to hook FSlateRHIRenderer::DrawWindow_RenderThread!");

    if (alternate) {
        SPDLOG_INFO("Using alternate method to hook FSlateRHIRenderer::DrawWindow_RenderThread!");
        m_attempted_hook_slate_thread_alternate = true;
    } else {
        m_attempted_hook_slate_thread = true;
    }

    auto func = alternate ? sdk::slate::locate_draw_window_renderthread_fn_alternate() : sdk::slate::locate_draw_window_renderthread_fn();

    if (!func && return_address == 0) {
        SPDLOG_ERROR("Cannot hook FSlateRHIRenderer::DrawWindow_RenderThread");
        return;
    }

    if (return_address != 0) {
        func = utility::find_function_start_with_call(return_address);

        if (!func) {
            SPDLOG_ERROR("Cannot hook FSlateRHIRenderer::DrawWindow_RenderThread with alternative return address method");
            m_hooked_slate_thread = true; // not actually true but just to stop spamming the scans
            return;
        }

        SPDLOG_INFO("Checking if the assembly listing for {:X} is really small", *func);

        // Check if the assembly listing for this function is really small. It shouldn't be really small.
        // This will happen on UE 5.5+ where RenderTexture_RenderThread is enqueued inside of a lambda.
        size_t distance_to_ret = 0;
        utility::exhaustive_decode((uint8_t*)*func, 1000, [&](utility::ExhaustionContext& ctx2) -> utility::ExhaustionResult {
            ++distance_to_ret;

            if (ctx2.instrux.BranchInfo.IsBranch && std::string_view{ctx2.instrux.Mnemonic}.starts_with("CALL")) {
                return utility::ExhaustionResult::STEP_OVER;
            }

            return utility::ExhaustionResult::CONTINUE;
        });

        if (distance_to_ret < 50) {
            SPDLOG_ERROR("FSlateRHIRenderer::DrawWindow_RenderThread function is too small! Distance to RET: {}", distance_to_ret);
            m_hooked_slate_thread = true; // not actually true but just to stop spamming the scans
            return;
        }

        SPDLOG_INFO("Found FSlateRHIRenderer::DrawWindow_RenderThread with alternative return address method: {:x}", *func);
    }

    m_slate_thread_hook = safetyhook::create_inline((void*)*func, &FFakeStereoRenderingHook::slate_draw_window_render_thread, safetyhook::InlineHook::StartDisabled);
    m_hooked_slate_thread = true;

    if (!m_slate_thread_hook) {
        SPDLOG_ERROR("Failed to hook FSlateRHIRenderer::DrawWindow_RenderThread!");
        return;
    }

    if (auto enable_result = m_slate_thread_hook.enable(); !enable_result.has_value()) {
        SPDLOG_ERROR("Failed to enable FSlateRHIRenderer::DrawWindow_RenderThread hook! {}", (int)enable_result.error().type);
        return;
    }

    SPDLOG_INFO("Hooked FSlateRHIRenderer::DrawWindow_RenderThread @ 0x{:x}!", *func);
}

namespace detail{
bool pre_find_fsceneview_constructor() {
    sdk::FSceneView::get_constructor_address(); // Can take a while to find
    return true;
}
}

void FFakeStereoRenderingHook::attempt_hook_fsceneview_constructor() {
    if (m_attempted_hook_fsceneview_constructor) {
        return;
    }
    
    // just try to find it before ghosting fix is even enabled
    if (m_asynchronous_scan->value()) {
        static std::future<bool> future = std::async(std::launch::async, detail::pre_find_fsceneview_constructor);

        // Wait for the future to be valid before attempting to hook
        if (future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            future.get();
        } else if (future.valid()) {
            return;
        }
    }

    auto& vr = VR::get();

    if (!vr->is_ghosting_fix_enabled() && !vr->is_splitscreen_compatibility_enabled() && !vr->is_sceneview_compatibility_enabled() && !vr->is_native_stereo_fix_enabled()) {
        return;
    }

    utility::ScopeGuard _{[&]() {
        m_attempted_hook_fsceneview_constructor = true;
    }};

    SPDLOG_INFO("Attempting to hook FSceneView::FSceneView constructor!");
    const auto constructor = sdk::FSceneView::get_constructor_address();

    if (!constructor) {
        SPDLOG_ERROR("Cannot hook FSceneView::FSceneView constructor");
        return;
    }

    g_hook->m_sceneview_data.constructor_hook = safetyhook::create_inline(*constructor, (uintptr_t)&sceneview_constructor, safetyhook::InlineHook::StartDisabled);

    if (!g_hook->m_sceneview_data.constructor_hook) {
        SPDLOG_ERROR("Failed to hook FSceneView::FSceneView constructor!");
        return;
    }

    if (auto enable_result = g_hook->m_sceneview_data.constructor_hook.enable(); !enable_result.has_value()) {
        SPDLOG_ERROR("Failed to enable FSceneView::FSceneView constructor hook! {}", (int)enable_result.error().type);
        return;
    }

    SPDLOG_INFO("Hooked FSceneView::FSceneView constructor!");
}

bool FFakeStereoRenderingHook::hook() {
    SPDLOG_INFO("Entering FFakeStereoRenderingHook::hook");

    m_tried_hooking = true;

    // Locking the hook monitor mutex stops our code from trying to re-hook DX11 and 12 after
    // Long pauses in code execution, due to us doing massive scans for code in this function.
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    const auto vtable = locate_fake_stereo_rendering_vtable();

    // This happens if games have intentionally removed the stereo initialization functions and stereo emulation classes.
    // So we need to manually create the stereo device.
    if (!vtable) {
        SPDLOG_ERROR("Failed to locate Fake Stereo Rendering VTable, attempting to perform nonstandard hook");

        auto check_file_version = [](uint32_t ms, uint32_t ls) {
            try {
                const auto full_path = utility::get_module_pathw(utility::get_executable());

                if (!full_path) {
                    SPDLOG_ERROR("Failed to get executable path, falling back");
                    return false;
                }

                const auto file_version_size = GetFileVersionInfoSizeW(full_path->c_str(), nullptr);

                if (file_version_size == 0) {
                    SPDLOG_ERROR("Failed to get file version info size, falling back");
                    return false;
                }

                std::vector<uint8_t> file_version_data(file_version_size);
                GetFileVersionInfoW(full_path->c_str(), 0, file_version_size, file_version_data.data());

                UINT size{};
                VS_FIXEDFILEINFO* fixed_file_info{};

                if (VerQueryValueA(file_version_data.data(), "\\", (LPVOID*)&fixed_file_info, &size) && fixed_file_info != nullptr) {
                    SPDLOG_INFO("MS: {:x}, LS: {:x}", fixed_file_info->dwFileVersionMS, fixed_file_info->dwFileVersionLS);

                    if (fixed_file_info->dwFileVersionMS == ms && fixed_file_info->dwFileVersionLS == ls) {
                        SPDLOG_INFO("Found matching executable, attempting to perform nonstandard hook");
                        return true;
                    } else {
                        SPDLOG_INFO("File does not match requested version, falling back");
                    }
                } else {
                    SPDLOG_ERROR("Failed to get file version info, falling back");
                }
            } catch(...) {
                SPDLOG_ERROR("Failed to get file version info, falling back");
            }

            return false;
        };

        const auto found_version = sdk::search_for_version(utility::get_executable());

        if (!found_version) {
            SPDLOG_WARN("Failed to find version in executable");
        }

        // Check for version 5.54.0.0
        if (check_file_version(0x50005, 0x40000)) {
            return nonstandard_create_stereo_device_hook_5_54();
        }

        // Check for version 4.27.2.0
        // 4.26 also works here
        if (check_file_version(0x4001B, 0x20000) || found_version.value_or(L"") == L"4.26") {
            return nonstandard_create_stereo_device_hook_4_27();
        }

        // Check for version 4.22.3.0
        if (check_file_version(0x40016, 0x30000)) {
            return nonstandard_create_stereo_device_hook_4_22();
        }

        // Check for version 4.18.3.0
        if (check_file_version(0x40012, 0x30000)) {
            return nonstandard_create_stereo_device_hook_4_18();
        }

        return nonstandard_create_stereo_device_hook();
    }

    return standard_fake_stereo_hook(*vtable);
}

bool FFakeStereoRenderingHook::standard_fake_stereo_hook(uintptr_t vtable) {
    ZoneScopedN(__FUNCTION__);
    SPDLOG_INFO("Performing standard fake stereo hook");

    const auto game = sdk::get_ue_module(L"Engine");
    std::array<uint8_t, 0x1000> og_vtable{};
    memcpy(og_vtable.data(), (void*)vtable, og_vtable.size()); // to perform tests on.

    const auto module_vtable_within = utility::get_module_within(vtable);

    // In 4.18 the destructor virtual doesn't exist or is at the very end of the vtable.
    const auto is_stereo_enabled_index = sdk::is_vfunc_pattern(*(uintptr_t*)vtable, "B0 01") ? 0 : 1;
    const auto is_stereo_enabled_func_ptr = &((uintptr_t*)vtable)[is_stereo_enabled_index];

    SPDLOG_INFO("IsStereoEnabled Index: {}", is_stereo_enabled_index);

    const auto stereo_view_offset_index = get_stereo_view_offset_index(vtable);

    if (!stereo_view_offset_index) {
        SPDLOG_ERROR("Failed to locate Stereo View Offset Index");
        return false;
    }

    // Some compiler optimizations cause 31 C0 (xor eax, eax) to be used.
    bool uses_33_c0 = false;

    for (size_t i = 0; i < 30; ++i) try {
        const auto fn = ((uintptr_t*)vtable)[i];

        if (fn == 0 || IsBadReadPtr((void*)fn, sizeof(void*))) {
            SPDLOG_WARN("Found null function pointer at index {}", i);
            break;
        }

        if (sdk::is_vfunc_pattern(fn, "33 C0")) {
            uses_33_c0 = true;
            SPDLOG_INFO("Found 33 C0 pattern at index {}", i);
            break;
        }
    } catch(...) {

    }

    const auto stereo_projection_matrix_index = *stereo_view_offset_index + 1;
    const auto is_4_18_or_lower = *stereo_view_offset_index <= 6;

    const auto& stereo_view_offset_func = ((uintptr_t*)vtable)[*stereo_view_offset_index];

    auto render_texture_render_thread_func = utility::find_virtual_function_from_string_ref(game, L"RenderTexture_RenderThread");

    // Seems more robust than simply just checking the vtable index.
    m_uses_old_rendertarget_manager = *stereo_view_offset_index <= 11 && !render_texture_render_thread_func;

    SPDLOG_INFO("Using old rendertarget manager: {}", m_uses_old_rendertarget_manager);

    if (!render_texture_render_thread_func) {
        // Fallback scan to checking for the first non-default virtual function (<= 4.18)
        SPDLOG_INFO("Failed to find RenderTexture_RenderThread, falling back to first non-default virtual function");

        for (auto i = 2; i < 10; ++i) {
            const auto func = ((uintptr_t*)vtable)[stereo_projection_matrix_index + i];

            // Some protectors can fool this check, so we also check for the vfunc pattern (emulates the code)
            if (!utility::is_stub_code((uint8_t*)func) && 
                !sdk::is_vfunc_pattern(func, "33 C0") &&
                !sdk::is_vfunc_pattern(func, "32 C0"))
            {
                render_texture_render_thread_func = func;
                break;
            }
        }

        if (!render_texture_render_thread_func) {
            SPDLOG_ERROR("Failed to find RenderTexture_RenderThread");
            return false;
        }
    }

    SPDLOG_INFO("RenderTexture_RenderThread: {:x}", (uintptr_t)*render_texture_render_thread_func);

    // Scan for the function pointer, it should be in the middle of the vtable.
    auto rendertexture_fn_vtable_middle = utility::scan_ptr(vtable + ((stereo_projection_matrix_index + 2) * sizeof(void*)), 50 * sizeof(void*), *render_texture_render_thread_func);

    if (!rendertexture_fn_vtable_middle) {
        SPDLOG_ERROR("Failed to find RenderTexture_RenderThread VTable Middle");
        return false;
    }

    auto rendertexture_fn_vtable_index = (*rendertexture_fn_vtable_middle - vtable) / sizeof(uintptr_t);
    SPDLOG_INFO("RenderTexture_RenderThread VTable Middle: {} {:x}", rendertexture_fn_vtable_index, (uintptr_t)*rendertexture_fn_vtable_middle);

    auto render_target_manager_vtable_index = rendertexture_fn_vtable_index + 1 + (2 * (size_t)is_4_18_or_lower);

    // verify first that the render target manager index is returning a null pointer
    // and if not, scan forward until we run into a vfunc that returns a null pointer
    auto get_render_target_manager_func_ptr = &((uintptr_t*)vtable)[render_target_manager_vtable_index];

    bool is_4_11 = false;

    //if (!sdk::is_vfunc_pattern(*(uintptr_t*)get_render_target_manager_func_ptr, "33 C0")) {
        //SPDLOG_INFO("Expected GetRenderTargetManager function at index {} does not return null, scanning forward for return nullptr.", render_target_manager_vtable_index);

        for (;;++render_target_manager_vtable_index) {
            get_render_target_manager_func_ptr = &((uintptr_t*)vtable)[render_target_manager_vtable_index];

            if (IsBadReadPtr(*(void**)get_render_target_manager_func_ptr, 1)) {
                SPDLOG_ERROR("Failed to find GetRenderTargetManager vtable index, a crash is imminent");
                return false;
            }

            if (sdk::is_vfunc_pattern(*(uintptr_t*)get_render_target_manager_func_ptr, "33 C0") || (!uses_33_c0 && sdk::is_vfunc_pattern(*(uintptr_t*)get_render_target_manager_func_ptr, "31 C0"))) {
                const auto distance_from_rendertexture_fn = render_target_manager_vtable_index - rendertexture_fn_vtable_index;

                // means it's 4.17 I think. 12 means 4.11.
                if (distance_from_rendertexture_fn == 10 || distance_from_rendertexture_fn == 11 || distance_from_rendertexture_fn == 12) {
                    is_4_11 = distance_from_rendertexture_fn == 12;
                    m_rendertarget_manager_embedded_in_stereo_device = true;
                    SPDLOG_INFO("Render target manager appears to be directly embedded in the stereo device vtable");
                } else {
                    // Now this may potentially be the correct index, but we're not quite done yet.
                    // On 4.19 (and possibly others), the index is 1 higher than it should be.
                    // We can tell by checking how many functions in front of this index return null.
                    // if there are two functions in front of this index that return null, we need to add 1 to the index.
                    SPDLOG_INFO("Found potential GetRenderTargetManager function at index {}", render_target_manager_vtable_index);
                    SPDLOG_INFO("Double checking GetRenderTargetManager index...");

                    int32_t count = 0;
                    for (auto i = render_target_manager_vtable_index + 1; i < render_target_manager_vtable_index + 5; ++i) {
                        const auto addr_of_func = (uintptr_t)&((uintptr_t*)vtable)[i];
                        const auto func = ((uintptr_t*)vtable)[i];

                        if (func == 0 || IsBadReadPtr((void*)func, 1)) {
                            break;
                        }

                        // Make sure we didn't cross over into another vtable's boundaries.
                        const auto module_within = utility::get_module_within(addr_of_func);

                        if (module_within && utility::scan_displacement_reference(*module_within, addr_of_func)) {
                            SPDLOG_INFO("Crossed over into another vtable's boundaries, aborting double check");
                            SPDLOG_INFO("Reached end of double check at index {}, {} appears to be the correct index.", i, render_target_manager_vtable_index);
                            break;
                        }

                        if (!sdk::is_vfunc_pattern(func, "33 C0") && !sdk::is_vfunc_pattern(func, "31 C0")) {
                            SPDLOG_INFO("Reached end of double check at index {}, {} appears to be the correct index.", i, render_target_manager_vtable_index);
                            break;
                        }

                        if (++count >= 2) {
                            ++render_target_manager_vtable_index;
                            get_render_target_manager_func_ptr = &((uintptr_t*)vtable)[render_target_manager_vtable_index];

                            SPDLOG_INFO("Adjusted GetRenderTargetManager index to {}", render_target_manager_vtable_index);
                            break;
                        }
                    }

                    SPDLOG_INFO("Distance: {}", distance_from_rendertexture_fn);
                }

                break;
            } else {
                try {
                    using GetRenderTargetManagerFn = IStereoRenderTargetManager* (*)(void*, void*, void*, void*, void*, void*, void*, void*);
                    const auto func = (GetRenderTargetManagerFn)(*get_render_target_manager_func_ptr);
    
                    // On UE5.5+ FFakeStereoRendering has a valid GetRenderTargetManager that doesn't return null.
                    if (!is_4_18_or_lower && func(og_vtable.data(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == (IStereoRenderTargetManager*)&og_vtable[sizeof(void*)]) {
                        m_uses_old_rendertarget_manager = false; // nope
                        SPDLOG_INFO("Found UE5.5+ variant of GetRenderTargetManager function at index {}", render_target_manager_vtable_index);
                        SPDLOG_INFO("GetRenderTargetManager function at index {} appears to be valid.", render_target_manager_vtable_index);
                        break;
                    }
                } catch(...) {
                    SPDLOG_WARN("Unknown exception while checking GetRenderTargetManager function at index {}", render_target_manager_vtable_index);
                }
            }
        }
    //} else {
        //SPDLOG_INFO("GetRenderTargetManager function at index {} appears to be valid.", render_target_manager_vtable_index);
    //}
    
    const auto get_stereo_layers_func_ptr = (uintptr_t)(get_render_target_manager_func_ptr + sizeof(void*));

    if (get_render_target_manager_func_ptr == 0) {
        SPDLOG_ERROR("Failed to find GetRenderTargetManager");
        return false;
    }

    if (get_stereo_layers_func_ptr == 0) {
        SPDLOG_ERROR("Failed to find GetStereoLayers");
        return false;
    }

    SPDLOG_INFO("GetRenderTargetManagerptr: {:x}", (uintptr_t)get_render_target_manager_func_ptr);
    SPDLOG_INFO("GetStereoLayersptr: {:x}", (uintptr_t)get_stereo_layers_func_ptr);

    const auto adjust_view_rect_distance = is_4_18_or_lower ? 2 : 3;
    const auto adjust_view_rect_index = *stereo_view_offset_index - adjust_view_rect_distance;

    SPDLOG_INFO("AdjustViewRect Index: {}", adjust_view_rect_index);
    
    auto calculate_stereo_projection_matrix_index = *stereo_view_offset_index + 1;

    // While generally most of the time the stereo projection matrix func is the next one after the stereo view offset func,
    // it's not always the case. We can scan for a call to the tanf function in one of the virtual functions to find it.
    for (auto i = 0; i < 10; ++i) {
        const auto potential_func = ((uintptr_t*)vtable)[calculate_stereo_projection_matrix_index + i];
        if (potential_func == 0 || IsBadReadPtr((void*)potential_func, 1) || utility::is_stub_code((uint8_t*)potential_func)) {
            continue;
        }

        auto ip = (uint8_t*)potential_func;
        if (*(uint8_t*)ip == 0xE9) {
            ip = (uint8_t*)utility::calculate_absolute(potential_func + 1);
            SPDLOG_INFO("Found JMP at {:x}, jumping to {:x}", (uintptr_t)potential_func, (uintptr_t)ip);
        }

        bool found = false;

        SPDLOG_INFO("Scanning {:x}...", (uintptr_t)ip);

        for (auto j = 0; j < 50; ++j) {
            INSTRUX ix{};

            const auto status = NdDecodeEx(&ix, (ND_UINT8*)ip, 1000, ND_CODE_64, ND_DATA_64);

            if (!ND_SUCCESS(status)) {
                SPDLOG_INFO("Decoding failed with error {:x}!", (uint32_t)status);
                break;
            }

            if (ix.Category == ND_CAT_RET || ix.InstructionBytes[0] == 0xE9) {
                SPDLOG_INFO("Encountered RET or JMP at {:x}, aborting scan", (uintptr_t)ip);
                break;
            }

            if (ix.InstructionBytes[0] == 0xE8) {
                auto called_func = (uintptr_t)(ip + ix.Length + (int32_t)ix.RelativeOffset);
                auto inner_ins = utility::decode_one((uint8_t*)called_func);

                SPDLOG_INFO("called {:x}", (uintptr_t)called_func);
                uintptr_t final_func = 0;

                // Fully resolve the pointer jmps until we reach another module.
                while (inner_ins && inner_ins->InstructionBytes[0] == 0xFF && inner_ins->InstructionBytes[1] == 0x25) {
                    const auto called_func_ptr = (uintptr_t*)(called_func + inner_ins->Length + (int32_t)inner_ins->Displacement);
                    const auto called_func_ptr_val = *called_func_ptr;

                    SPDLOG_INFO("called ptr {:x}", (uintptr_t)called_func_ptr_val);

                    inner_ins = utility::decode_one((uint8_t*)called_func_ptr_val);
                    final_func = called_func_ptr_val;
                    called_func = called_func_ptr_val;
                }

                // Check if this function is jmping into the "tanf" export in ucrtbase.dll
                if (final_func != 0) {
                    const auto module_within = utility::get_module_within(final_func);

                    if (module_within &&
                        (final_func == (uintptr_t)GetProcAddress(*module_within, "tanf") ||
                        final_func == (uintptr_t)GetProcAddress(*module_within, "tan"))) 
                    {
                        SPDLOG_INFO("Found CalculateStereoProjectionMatrix: {} {:x}", calculate_stereo_projection_matrix_index + i, potential_func);
                        calculate_stereo_projection_matrix_index += i;
                        found = true;
                        break;
                    } else {
                        SPDLOG_INFO("Function did not call tanf, skipping");
                    }
                } else {
                    SPDLOG_INFO("Failed to resolve inner pointer");
                }
            }

            ip += ix.Length;
        }

        if (found) {
            break;
        }
    }

    const auto init_canvas_index = calculate_stereo_projection_matrix_index + 1;

    const auto adjust_view_rect_func = ((uintptr_t*)vtable)[adjust_view_rect_index];
    const auto calculate_stereo_projection_matrix_func = ((uintptr_t*)vtable)[calculate_stereo_projection_matrix_index];
    const auto init_canvas_func_ptr = &((uintptr_t*)vtable)[init_canvas_index];
    // const auto render_texture_render_thread_func = ((uintptr_t*)*vtable)[*stereo_view_offset_index + 3];
    

    SPDLOG_INFO("AdjustViewRect: {:x}", (uintptr_t)adjust_view_rect_func);
    SPDLOG_INFO("CalculateStereoProjectionMatrix: {:x}", (uintptr_t)calculate_stereo_projection_matrix_func);
    SPDLOG_INFO("CalculateStereoViewOffset: {:x}", (uintptr_t)stereo_view_offset_func);
    SPDLOG_INFO("IsStereoEnabled: {:x}", (uintptr_t)*is_stereo_enabled_func_ptr);

    m_has_double_precision = is_using_double_precision(stereo_view_offset_func) || is_using_double_precision(calculate_stereo_projection_matrix_func);

    {
        m_adjust_view_rect_hook = safetyhook::create_inline((void*)adjust_view_rect_func, adjust_view_rect);
        m_calculate_stereo_view_offset_hook_inline = safetyhook::create_inline((void*)stereo_view_offset_func, calculate_stereo_view_offset);
        m_calculate_stereo_projection_matrix_hook = safetyhook::create_inline((void*)calculate_stereo_projection_matrix_func, calculate_stereo_projection_matrix);
    }
    
    if (!m_adjust_view_rect_hook) {
        SPDLOG_ERROR("Failed to create AdjustViewRect hook");
    }

    if (!m_calculate_stereo_view_offset_hook_inline) {
        SPDLOG_ERROR("Failed to create CalculateStereoViewOffset hook, falling back to pointer hook");
        m_calculate_stereo_view_offset_hook_ptr = std::make_unique<PointerHook>((void**)&stereo_view_offset_func, (void*)calculate_stereo_view_offset);
    }

    if (!m_calculate_stereo_projection_matrix_hook) {
        SPDLOG_ERROR("Failed to create CalculateStereoProjectionMatrix hook");
    }

    // This requires a pointer hook because the virtual just returns false
    // compiler optimization makes that function get re-used in a lot of places
    // so it's not feasible to just detour it, we need to replace the pointer in the vtable.
    if (!m_rendertarget_manager_embedded_in_stereo_device) {
        m_render_texture_render_thread_hook = safetyhook::create_inline((void*)*render_texture_render_thread_func, render_texture_render_thread);

        if (!m_render_texture_render_thread_hook) {
            SPDLOG_ERROR("Failed to create RenderTexture_RenderThread hook");
        }

        // Seems to exist in 4.18+
        m_get_render_target_manager_hook = std::make_unique<PointerHook>((void**)get_render_target_manager_func_ptr, (void*)&get_render_target_manager_hook);
    } else {
        // When the render target manager is embedded in the stereo device, it just means
        // that all of the virtuals are now part of FFakeStereoRendering
        // instead of being a part of IStereoRenderTargetManager and being returned via GetRenderTargetManager.
        // Only seen in 4.17 and below.
        SPDLOG_INFO("Performing hooks on embedded RenderTargetManager");

        // Scan forward from the alleged RenderTexture_RenderThread function to find the
        // real RenderTexture_RenderThread function, because it is different when the
        // render target manager is embedded in the stereo device.
        // When it's embedded, it seems like it's the first function right after
        // a set of functions that return false sequentially.
        bool prev_function_returned_false = false;

        for (auto i = rendertexture_fn_vtable_index + 1; i < 100; ++i) {
            const auto func = ((uintptr_t*)og_vtable.data())[i];

            if (func == 0 || IsBadReadPtr((void*)func, 3)) {
                SPDLOG_ERROR("Failed to find real RenderTexture_RenderThread");
                return false;
            }
            
            if (sdk::is_vfunc_pattern(func, "32 C0")) {
                prev_function_returned_false = true;
            } else {
                if (prev_function_returned_false) {
                    render_texture_render_thread_func = func;
                    rendertexture_fn_vtable_index = i;
                    m_render_texture_render_thread_hook = safetyhook::create_inline((void*)*render_texture_render_thread_func, render_texture_render_thread);
                    if (!m_render_texture_render_thread_hook) {
                        SPDLOG_ERROR("Failed to create RenderTexture_RenderThread hook");
                    }
                    SPDLOG_INFO("Real RenderTexture_RenderThread: {} {:x}", rendertexture_fn_vtable_index, (uintptr_t)*render_texture_render_thread_func);
                    break;
                }

                prev_function_returned_false = false;
            }
        }

        // Scan backwards from RenderTexture_RenderThread for the first virtual that just returns
        int32_t calculate_render_target_size_index = 0;

        for (auto i = rendertexture_fn_vtable_index - 1; i > 0; --i) {
            const auto func = ((uintptr_t*)og_vtable.data())[i];

            if (func == 0 || IsBadReadPtr((void*)func, 3)) {
                SPDLOG_ERROR("Failed to find calculate render target size index, falling back to hardcoded index");
                calculate_render_target_size_index = rendertexture_fn_vtable_index - 3;
                break;
            }

            if (sdk::is_vfunc_pattern(func, "C3") || sdk::is_vfunc_pattern(func, "C2 00 00")) {
                SPDLOG_INFO("Dynamically found CalculateRenderTargetSize index: {}", i);
                calculate_render_target_size_index = i;
                break;
            }
        }

        const auto calculate_render_target_size_func_ptr = &((uintptr_t*)vtable)[calculate_render_target_size_index];
        SPDLOG_INFO("CalculateRenderTargetSize index: {}", calculate_render_target_size_index);

        // To be seen if this one needs automated analysis
        const auto need_reallocate_viewport_render_target_index = calculate_render_target_size_index + 1;
        const auto need_reallocate_viewport_render_target_func_ptr = &((uintptr_t*)vtable)[need_reallocate_viewport_render_target_index];

        // To be seen if this one needs automated analysis
        const auto should_use_separate_render_target_index = calculate_render_target_size_index + 2;
        const auto should_use_separate_render_target_func_ptr = &((uintptr_t*)vtable)[should_use_separate_render_target_index];

        // Log a warning if NeedReallocateViewportRenderTarget or ShouldUseSeparateRenderTarget are not
        // functions that plainly return false, but do not fail entirely.
        bool need_reallocate_viewport_render_target_is_bad = false;
        bool should_use_separate_render_target_is_bad = false;

        if (!sdk::is_vfunc_pattern(*need_reallocate_viewport_render_target_func_ptr, "32 C0")) {
            SPDLOG_WARN("NeedReallocateViewportRenderTarget is not a function that returns false");
            need_reallocate_viewport_render_target_is_bad = true;
        }

        if (!sdk::is_vfunc_pattern(*should_use_separate_render_target_func_ptr, "32 C0")) {
            SPDLOG_WARN("ShouldUseSeparateRenderTarget is not a function that returns false");
            should_use_separate_render_target_is_bad = true;
        }

        SPDLOG_INFO("NeedReallocateViewportRenderTarget index: {}", need_reallocate_viewport_render_target_index);
        SPDLOG_INFO("ShouldUseSeparateRenderTarget index: {}", should_use_separate_render_target_index);

        // Scan forward from RenderTexture_RenderThread for the first virtual that returns false
        int32_t allocate_render_target_index = 0;

        for (auto i = rendertexture_fn_vtable_index + 1; i < 100; ++i) {
            const auto func = ((uintptr_t*)og_vtable.data())[i];

            if (func == 0 || IsBadReadPtr((void*)func, 3)) {
                SPDLOG_ERROR("Failed to find allocate render target index, falling back to hardcoded index");
                allocate_render_target_index = render_target_manager_vtable_index + 3;
                break;
            }

            if (sdk::is_vfunc_pattern(func, "32 C0")) {
                SPDLOG_INFO("Dynamically found AllocateRenderTarget index: {}", i);
                allocate_render_target_index = i;
                break;
            }
        }

        const auto allocate_render_target_func_ptr = &((uintptr_t*)vtable)[allocate_render_target_index];
        SPDLOG_INFO("AllocateRenderTarget index: {}", allocate_render_target_index);

        m_embedded_rtm.calculate_render_target_size_hook = 
            std::make_unique<PointerHook>((void**)calculate_render_target_size_func_ptr, +[](void* self, const sdk::FViewport& viewport, uint32_t& x, uint32_t& y) {
            #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
                SPDLOG_INFO("CalculateRenderTargetSize (embedded)");
            #else
                SPDLOG_INFO_ONCE("CalculateRenderTargetSize (embedded)");
            #endif

                return g_hook->get_render_target_manager()->calculate_render_target_size(viewport, x, y);
            }
        );

        m_embedded_rtm.allocate_render_target_texture_hook = 
            std::make_unique<PointerHook>((void**)allocate_render_target_func_ptr, +[](void* self, 
                uint32_t index, uint32_t w, uint32_t h, uint8_t format, uint32_t num_mips,
                ETextureCreateFlags lags, ETextureCreateFlags targetable_texture_flags, FTexture2DRHIRef& out_texture,
                FTexture2DRHIRef& out_shader_resource, uint32_t num_samples) -> bool {
            #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
                SPDLOG_INFO("AllocateRenderTargetTexture (embedded): {:x}", (uintptr_t)_ReturnAddress());
            #else
                SPDLOG_INFO_ONCE("AllocateRenderTargetTexture (embedded): {:x}", (uintptr_t)_ReturnAddress());
            #endif

                return g_hook->get_render_target_manager()->allocate_render_target_texture((uintptr_t)_ReturnAddress(), &out_texture, &out_shader_resource);
            }
        );
    
        m_embedded_rtm.should_use_separate_render_target_hook = 
            std::make_unique<PointerHook>((void**)should_use_separate_render_target_func_ptr, +[](void* self) -> bool {
            #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
                SPDLOG_INFO("ShouldUseSeparateRenderTarget (embedded): {:x}", (uintptr_t)_ReturnAddress());
            #else
                SPDLOG_INFO_ONCE("ShouldUseSeparateRenderTarget (embedded): {:x}", (uintptr_t)_ReturnAddress());
            #endif
            
                auto vr = VR::get();

                if (vr->is_extreme_compatibility_mode_enabled()) {
                    return false;
                }

                if (vr->is_hmd_active() && !vr->is_stereo_emulation_enabled()) {
                    g_hook->get_embedded_rtm().should_use_separate_rt_called = true;
                    return true;
                }

                return false;
            }
        );

        if (!need_reallocate_viewport_render_target_is_bad) {
            m_embedded_rtm.need_reallocate_viewport_render_target_hook = 
                std::make_unique<PointerHook>((void**)need_reallocate_viewport_render_target_func_ptr, +[](void* self, sdk::FViewport* viewport) -> bool {
                #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
                    SPDLOG_INFO("NeedReallocateViewportRenderTarget (embedded): {:x}", (uintptr_t)_ReturnAddress());
                #else
                    SPDLOG_INFO_ONCE("NeedReallocateViewportRenderTarget (embedded): {:x}", (uintptr_t)_ReturnAddress());
                #endif

                    if (g_hook->get_render_target_manager()->need_reallocate_view_target(*viewport)) {
                        g_hook->get_embedded_rtm().need_reallocate_viewport_render_target_called = true;
                        g_hook->get_embedded_rtm().last_time_needed_hmd_reallocate = std::chrono::steady_clock::now();
                        return true;
                    }

                    return false;
                }
            );
        }
    }
    
    m_is_stereo_enabled_hook = std::make_unique<PointerHook>((void**)is_stereo_enabled_func_ptr, (void*)&is_stereo_enabled);

    // scan for GetDesiredNumberOfViews function, we use this function to perform AFR if needed
    SPDLOG_INFO("Searching for GetDesiredNumberOfViews function...");
    std::optional<uint32_t> get_desired_number_of_views_index{};

    for (auto i = 1; i < 20; ++i) {
        auto func_ptr = &((uintptr_t*)vtable)[i];

        if (IsBadReadPtr((void*)*func_ptr, sizeof(void*))) {
            SPDLOG_INFO("Could not locate GetDesiredNumberOfViews function, this is okay, not really needed");
            break;
        }

        // pretty consistent patterns
        if (sdk::is_vfunc_pattern(*func_ptr, "0F B6 C2 FF C0 C3") ||
            sdk::is_vfunc_pattern(*func_ptr, "33 C0 84 D2 0F 95 C0 FF C0 C3") || 
            sdk::is_vfunc_pattern(*func_ptr, "84 D2 74 04 8B 41 ? C3 B8 01"))
        {
            SPDLOG_INFO("Found GetDesiredNumberOfViews function at index: {}", i);
            get_desired_number_of_views_index = i;
            m_get_desired_number_of_views_hook = std::make_unique<PointerHook>((void**)func_ptr, (void*)&get_desired_number_of_views_hook);
            break;
        }
    }

    // If double precision detected, it means it's >= UE 5.0.3
    if (m_has_double_precision && get_desired_number_of_views_index) {
        SPDLOG_INFO("Searching for GetViewPassForIndex function...");

        // Pretty simple, it's at +1, to be seen if this needs automation
        const auto get_view_pass_for_index_index = *get_desired_number_of_views_index + 1;

        auto func_ptr = &((uintptr_t*)vtable)[get_view_pass_for_index_index];

        if (IsBadReadPtr((void*)*func_ptr, sizeof(void*))) {
            SPDLOG_INFO("Could not locate GetViewPassForIndex function. A crash may occur.");
        } else {
            SPDLOG_INFO("Found GetViewPassForIndex function at index: {}", get_view_pass_for_index_index);
            m_get_view_pass_for_index_hook = std::make_unique<PointerHook>((void**)func_ptr, (void*)&get_view_pass_for_index_hook);
        }
    } else if (m_has_double_precision) {
        SPDLOG_INFO("Could not locate GetViewPassForIndex function because GetDesiredNumberOfViews function was not found. A crash may occur.");
    }

    SPDLOG_INFO("Leaving FFakeStereoRenderingHook::hook");

    const auto renderer_module = sdk::get_ue_module(L"Renderer");
    const auto backbuffer_format_cvar = sdk::find_cvar_by_description(L"Defines the default back buffer pixel format.", L"r.DefaultBackBufferPixelFormat", 4, renderer_module);
    m_pixel_format_cvar_found = backbuffer_format_cvar.has_value();

    // In 4.18 this doesn't exist. Not much we can do about that.
    if (backbuffer_format_cvar) {
        SPDLOG_INFO("Backbuffer Format CVar: {:x}", (uintptr_t)*backbuffer_format_cvar);
        *(int32_t*)(*(uintptr_t*)*backbuffer_format_cvar + 0) = 0;   // 8bit RGBA, which is what VR headsets support
        *(int32_t*)(*(uintptr_t*)*backbuffer_format_cvar + 0x4) = 0; // 8bit RGBA, which is what VR headsets support
    } else {
        SPDLOG_ERROR("Failed to find backbuffer format cvar, continuing anyways...");
    }

    // make a shadow copy of FFakeStereoRendering's vtable to get past weird compiler optimizations
    // that cause the hook to not work, reason being that the compiler will optimize
    // if the vtable pointer is equal to the original vtable pointer, and it will
    // not call the hook function, so we make a shadow copy of the vtable
    auto active_stereo_device = locate_active_stereo_rendering_device();
    
    // We need to manually insert a stereo device at this point if it's not already.
    // This is what the "nonstandard" hooks did, but those did not have access to FFakeStereoRendering's vtable.
    // All we need to do in this instance is get the engine offset to the stereo device, create a fake pointer with our own vtable,
    // and just overwrite the engine's (null) stereo device pointer with our fake one.
    // It is very rare that this should need to be done.
    if (!active_stereo_device) {
        SPDLOG_INFO("Attempting to create a stereo device without InitializeHMDDevice...");
        const auto device_offset = sdk::UEngine::get_stereo_rendering_device_offset();

        if (device_offset) {
            auto engine = sdk::UGameEngine::get();

            if (engine != nullptr) {
                m_fallback_device.vtable = (void*)vtable;
                *(uintptr_t*)((uintptr_t)engine + *device_offset) = (uintptr_t)&m_fallback_device;

                active_stereo_device = (uintptr_t)&m_fallback_device;
                s_stereo_rendering_device_offset = *device_offset; // Set it up if it's not already
            }
        } else {
            SPDLOG_ERROR("Could not create a new stereo device, VR may not work!");
        }
    }

    if (active_stereo_device) {
        SPDLOG_INFO("Found active stereo device: {:x}", (uintptr_t)*active_stereo_device);
        SPDLOG_INFO("Overwriting vtable...");

        static std::vector<uintptr_t> shadow_vtable{};
        auto& vtable = *(uintptr_t**)*active_stereo_device;

        for (auto i = 0; i < 100; i++) {
            shadow_vtable.push_back(vtable[i]);
        }

        vtable = shadow_vtable.data();
    } else {
        SPDLOG_INFO("Current stereo device is null, cannot overwrite vtable");
        patch_vtable_checks(); // fallback to patching vtable checks
    }

    setup_view_extensions();
    hook_game_viewport_client();

    m_finished_hooking = true;

    SPDLOG_INFO("Finished hooking FFakeStereoRendering!");

    return true;
}

bool FFakeStereoRenderingHook::nonstandard_create_stereo_device_hook() {
    // This may only work on one game for now, but it should be a good placeholder
    // for creating a stereo device for games that don't have one.
    // We can figure out how to make it work for other games when we run into one
    // that needs this same functionality.

    // The reason why this function is needed is because in the one game that
    // the FFakeStereoRenderingHook doesn't work through the standard method,
    // is because the VR pipeline seems to have been heavily modified,
    // and so the -emulatestereo command line argument doesn't work, and
    // the FFakeStereoRendering vtable does not seem to exist
    // However the StereoRenderingDevice within GEngine seems to still exist
    // so we can take advantage of that and create our own stereo device
    // the downside is it will be much more difficult to figure out the 
    // proper vtable indices for the functions we need to hook
    // and we will need to actually implement some of the functions
    SPDLOG_INFO("Attempting to create a stereo device for the game using nonstandard method");
    m_fallback_vtable.resize(30);

    // Give all of the functions placeholders.
    for (auto i = 0; i < m_fallback_vtable.size(); ++i) {
        m_fallback_vtable[i] = +[](FFakeStereoRendering* stereo) -> void* {
            return nullptr;
        };
    }

    // Actually implement the ones we care about now.
    auto idx = 0;
    //m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo) -> void { SPDLOG_INFO("Destructor called?");  }; // destructor.
    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo) -> bool { 
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoEnabled called: {:x}", (uintptr_t)_ReturnAddress());
#endif

        return g_hook->is_stereo_enabled(stereo); 
    }; // IsStereoEnabled
    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // IsStereoEnabledOnNextFrame
    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // EnableStereo

    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) { 
        return g_hook->adjust_view_rect(stereo, index, x, y, w, h);
    }; // AdjustViewRect


    ++idx; // idk waht this is.

    // in this version the index is passed...?
    /*m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo, uint32_t index, Vector2f* bounds) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetTextSafeRegionBounds called");
#endif

        bounds->x = 0.75f;
        bounds->y = 0.75f;

        return bounds;
    };*/ // GetTextSafeRegionBounds

    m_fallback_vtable[idx++] = 
    +[](FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        return g_hook->calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location);
    }; // CalculateStereoViewOffset

    
    idx++;

    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("CalculateStereoProjectionMatrix called: {:x} {} {:x}", (uintptr_t)_ReturnAddress(), view_index, (uintptr_t)out);
#endif

        if (!g_hook->m_has_double_precision) {
            (*out)[3][2] = 0.1f; // Need to pre-set the Z value to something, otherwise it will be 0.0f & probably break something.
        } else {
            auto dmat = (Matrix4x4d*)out;
            (*dmat)[3][2] = 0.1;
        }

        return g_hook->calculate_stereo_projection_matrix(stereo, out, view_index);
    }; // CalculateStereoProjectionMatrix

    m_fallback_vtable[idx++] = +[](FFakeStereoRendering* stereo, void* a2) {
        // do nothing
    }; // not sure what this one is. think it sets the FOV. Not present in newer UE4 versions.

    idx++; // just leave this one as a placeholder for now. Returns false.

    m_fallback_vtable[idx++] = 
    +[](FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list, FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) {
        return g_hook->render_texture_render_thread(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    };

    idx++; // just leave this one as a placeholder for now. Probably SetClippingPlanes.

    m_fallback_vtable[13] = +[](FFakeStereoRendering* stereo) { return g_hook->get_render_target_manager_hook(stereo); }; // GetRenderTargetManager
    //m_fallback_vtable[13] = +[](FFakeStereoRendering* stereo) { return nullptr; }; // GetRenderTargetManager

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot create stereo device!");
        return false;
    }

    //m_418_detected = true;
    m_special_detected = true;
    m_manually_constructed = true;
    m_fallback_device.vtable = m_fallback_vtable.data();

    auto stereo_rendering_device_offset = sdk::UEngine::get_stereo_rendering_device_offset();
    if (!stereo_rendering_device_offset) {
        stereo_rendering_device_offset = 0xAC8; // fallback for the engine this was originally made for.
    }

    *(uintptr_t*)((uintptr_t)engine + *stereo_rendering_device_offset) = (uintptr_t)&m_fallback_device; // TODO: Automatically find this offset.

    // So the view extension hook will work.
    s_stereo_rendering_device_offset = *stereo_rendering_device_offset;

    hook_game_viewport_client();
    setup_view_extensions();

    SPDLOG_INFO("Finished creating stereo device for the game using nonstandard method");

    m_finished_hooking = true;

    return true;
}

bool FFakeStereoRenderingHook::nonstandard_create_stereo_device_hook_5_54() {
    SPDLOG_INFO("Attempting to create a stereo device for the game using nonstandard method (5.5.4)");

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot create stereo device!");
        return false;
    }

    m_fallback_vtable.resize(30);

    // Give all of the functions placeholders.
    for (auto i = 0; i < m_fallback_vtable.size(); ++i) {
        m_fallback_vtable[i] = +[](FFakeStereoRendering* stereo) -> void* {
            return nullptr;
        };
    }

    // 5.5.4 vtable: all DeviceIs* functions are static, GetViewIndexForPass removed.
    constexpr auto DESTRUCTOR_INDEX = 0;
    constexpr auto IS_STEREO_ENABLED_INDEX = 1;
    constexpr auto IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX = 2;
    constexpr auto ENABLE_STEREO_INDEX = 3;

    constexpr auto GET_DESIRED_NUMBER_OF_VIEWS_INDEX = 4;
    constexpr auto GET_VIEW_PASS_FOR_INDEX_INDEX = 5;

    constexpr auto GET_LOD_VIEW_INDEX_INDEX = 6;
    constexpr auto IS_STANDALONE_STEREO_ONLY_DEVICE_INDEX = 7;

    constexpr auto ADJUST_VIEW_RECT_INDEX = 8;
    constexpr auto SET_FINAL_VIEW_RECT_INDEX = 9;
    constexpr auto GET_TEXT_SAFE_REGION_BOUNDS_INDEX = 10;
    constexpr auto CALCULATE_STEREO_VIEW_OFFSET_INDEX = 11;
    constexpr auto CALCULATE_STEREO_PROJECTION_MATRIX_INDEX = 12;
    constexpr auto INIT_CANVAS_FROM_VIEW_INDEX = 13;
    constexpr auto RENDER_TEXTURE_RENDER_THREAD_INDEX = 14;
    constexpr auto GET_RENDER_TARGET_MANAGER_INDEX = 15;

    auto stereo_rendering_device_offset = sdk::UEngine::get_stereo_rendering_device_offset();
    if (!stereo_rendering_device_offset) {
        SPDLOG_ERROR("Failed to find stereo rendering device offset for 5.54, cannot create stereo device!");
        return false;
    }

    m_fallback_vtable[DESTRUCTOR_INDEX] = +[](FFakeStereoRendering* stereo) -> void { SPDLOG_INFO("Destructor called?"); };
    m_fallback_vtable[IS_STEREO_ENABLED_INDEX] = +[](FFakeStereoRendering* stereo) -> bool {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoEnabled called: {:x}", (uintptr_t)_ReturnAddress());
#endif

        return g_hook->is_stereo_enabled(stereo);
    };

    m_fallback_vtable[IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); };
    m_fallback_vtable[ENABLE_STEREO_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); };

    m_fallback_vtable[GET_DESIRED_NUMBER_OF_VIEWS_INDEX] = +[](FFakeStereoRendering* stereo, bool stereo_enabled) -> int32_t {
        return g_hook->get_desired_number_of_views_hook(stereo, stereo_enabled);
    };

    m_fallback_vtable[GET_VIEW_PASS_FOR_INDEX_INDEX] = +[](FFakeStereoRendering* stereo, bool stereo_requested, const uint32_t view_index) -> EStereoscopicPass {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetViewPassForIndex called: {:x} {} ", (uintptr_t)_ReturnAddress(), view_index);
    #endif

        return g_hook->get_view_pass_for_index_hook(stereo, stereo_requested, view_index);
    };

    m_fallback_vtable[ADJUST_VIEW_RECT_INDEX] = +[](FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) {
        return g_hook->adjust_view_rect(stereo, index, x, y, w, h);
    };

    m_fallback_vtable[CALCULATE_STEREO_VIEW_OFFSET_INDEX] =
    +[](FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        return g_hook->calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location);
    };

    m_fallback_vtable[CALCULATE_STEREO_PROJECTION_MATRIX_INDEX] = +[](FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("CalculateStereoProjectionMatrix called: {:x} {} {:x}", (uintptr_t)_ReturnAddress(), view_index, (uintptr_t)out);
#endif

        if (!g_hook->m_has_double_precision) {
            (*out)[3][2] = 0.1f;
        } else {
            auto dmat = (Matrix4x4d*)out;
            (*dmat)[3][2] = 0.1;
        }

        return g_hook->calculate_stereo_projection_matrix(stereo, out, view_index);
    };

    m_fallback_vtable[RENDER_TEXTURE_RENDER_THREAD_INDEX] =
    +[](FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list, FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) {
        return g_hook->render_texture_render_thread(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    };

    m_fallback_vtable[GET_RENDER_TARGET_MANAGER_INDEX] = +[](FFakeStereoRendering* stereo) { return g_hook->get_render_target_manager_hook(stereo); };

    m_special_detected_5_54 = true;
    m_manually_constructed = true;
    m_fallback_device.vtable = m_fallback_vtable.data();
    m_has_double_precision = true;
    m_uses_old_rendertarget_manager = false;

    auto& current_device = *(uintptr_t*)((uintptr_t)engine + *stereo_rendering_device_offset);
    SPDLOG_INFO("Current device: {:x}", current_device);
    current_device = (uintptr_t)&m_fallback_device;

    s_stereo_rendering_device_offset = *stereo_rendering_device_offset;

    hook_game_viewport_client();
    setup_view_extensions();

    SPDLOG_INFO("Finished creating stereo device for the game using nonstandard method (5.5.4)");

    m_finished_hooking = true;

    return true;
}

bool FFakeStereoRenderingHook::nonstandard_create_stereo_device_hook_4_27() {
    SPDLOG_INFO("Attempting to create a stereo device for the game using nonstandard method (4.27)");

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot create stereo device!");
        return false;
    }

    m_fallback_vtable.resize(30);

    // Give all of the functions placeholders.
    for (auto i = 0; i < m_fallback_vtable.size(); ++i) {
        m_fallback_vtable[i] = +[](FFakeStereoRendering* stereo) -> void* {
            return nullptr;
        };
    }

    constexpr auto DESTRUCTOR_INDEX = 0;
    constexpr auto IS_STEREO_ENABLED_INDEX = 1;
    constexpr auto IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX = 2;
    constexpr auto ENABLE_STEREO_INDEX = 3;

    constexpr auto GET_DESIRED_NUMBER_OF_VIEWS_INDEX = 4;
    constexpr auto GET_VIEW_PASS_FOR_INDEX_INDEX = 5;
    constexpr auto GET_VIEW_INDEX_FOR_PASS_INDEX = 6;

    constexpr auto DEVICE_IS_STEREO_EYE_PASS_INDEX = 7;
    constexpr auto DEVICE_IS_STEREO_EYE_VIEW_INDEX = 8;
    constexpr auto DEVICE_IS_A_PRIMARY_PASS_INDEX = 9;
    constexpr auto DEVICE_IS_A_PRIMARY_VIEW_INDEX = 10;
    constexpr auto DEVICE_IS_A_SECONDARY_PASS_INDEX = 11;
    constexpr auto DEVICE_IS_A_SECONDARY_VIEW_INDEX = 12;
    constexpr auto DEVICE_IS_AN_ADDITIONAL_PASS_INDEX = 13; // not necessary...?
    constexpr auto DEVICE_IS_AN_ADDITIONAL_VIEW_INDEX = 14; // not necessary...?
    constexpr auto DEVICE_GET_LOD_VIEW_INDEX_INDEX = 15; // not necessary...?

    constexpr auto ADJUST_VIEW_RECT_INDEX = 16;
    constexpr auto CALCULATE_STEREO_VIEW_OFFSET_INDEX = 19;
    constexpr auto CALCULATE_STEREO_PROJECTION_MATRIX_INDEX = 20;
    constexpr auto RENDER_TEXTURE_RENDER_THREAD_INDEX = 22;
    constexpr auto GET_RENDER_TARGET_MANAGER_INDEX = 23;

    auto stereo_rendering_device_offset = sdk::UEngine::get_stereo_rendering_device_offset();
    if (!stereo_rendering_device_offset) {
        stereo_rendering_device_offset = 0xB18; // fallback for the engine this was originally made for.
    }

    static constexpr auto FSCENEVIEW_STEREO_PASS_OFFSET = 0xAF0;
    static auto get_stereo_pass = [](const sdk::FSceneView& view) -> EStereoscopicPass {
        return (EStereoscopicPass)*(uint8_t*)((uintptr_t)&view + FSCENEVIEW_STEREO_PASS_OFFSET);
    };

    // Actually implement the ones we care about now.
    m_fallback_vtable[DESTRUCTOR_INDEX] = +[](FFakeStereoRendering* stereo) -> void { SPDLOG_INFO("Destructor called?");  }; // destructor.
    m_fallback_vtable[IS_STEREO_ENABLED_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { 
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoEnabled called: {:x}", (uintptr_t)_ReturnAddress());
#endif

        return g_hook->is_stereo_enabled(stereo); 
    }; // IsStereoEnabled

    m_fallback_vtable[IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // IsStereoEnabledOnNextFrame
    m_fallback_vtable[ENABLE_STEREO_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // EnableStereo

    m_fallback_vtable[ADJUST_VIEW_RECT_INDEX] = +[](FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) { 
        return g_hook->adjust_view_rect(stereo, index, x, y, w, h);
    }; // AdjustViewRect

    m_fallback_vtable[CALCULATE_STEREO_VIEW_OFFSET_INDEX] = 
    +[](FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        return g_hook->calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location);
    }; // CalculateStereoViewOffset

    m_fallback_vtable[CALCULATE_STEREO_PROJECTION_MATRIX_INDEX] = +[](FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("CalculateStereoProjectionMatrix called: {:x} {} {:x}", (uintptr_t)_ReturnAddress(), view_index, (uintptr_t)out);
#endif

        if (!g_hook->m_has_double_precision) {
            (*out)[3][2] = 0.1f; // Need to pre-set the Z value to something, otherwise it will be 0.0f & probably break something.
        } else {
            auto dmat = (Matrix4x4d*)out;
            (*dmat)[3][2] = 0.1;
        }

        return g_hook->calculate_stereo_projection_matrix(stereo, out, view_index);
    }; // CalculateStereoProjectionMatrix

    m_fallback_vtable[RENDER_TEXTURE_RENDER_THREAD_INDEX] = 
    +[](FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list, FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) {
        return g_hook->render_texture_render_thread(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    };

    m_fallback_vtable[GET_RENDER_TARGET_MANAGER_INDEX] = +[](FFakeStereoRendering* stereo) { return g_hook->get_render_target_manager_hook(stereo); }; // GetRenderTargetManager

    m_fallback_vtable[GET_DESIRED_NUMBER_OF_VIEWS_INDEX] = +[](FFakeStereoRendering* stereo, bool stereo_enabled) -> int32_t { 
        return g_hook->get_desired_number_of_views_hook(stereo, stereo_enabled); 
    }; // GetDesiredNumberOfViews

    m_fallback_vtable[GET_VIEW_PASS_FOR_INDEX_INDEX] = +[](FFakeStereoRendering* stereo, bool stereo_requested, const uint32_t view_index) -> EStereoscopicPass {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetViewPassForIndex called: {:x} {} ", (uintptr_t)_ReturnAddress(), view_index);
    #endif

        return g_hook->get_view_pass_for_index_hook(stereo, stereo_requested, view_index);
    }; // GetViewPassForIndex

    m_fallback_vtable[GET_VIEW_INDEX_FOR_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> int32_t {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetViewIndexForPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        switch (pass) {
            case EStereoscopicPass::eSSP_FULL:
            case EStereoscopicPass::eSSP_PRIMARY:
                return 0;
            
            case EStereoscopicPass::eSSP_SECONDARY:
                return 1;
            
            default:
                SPDLOG_ERROR("Unknown pass: {}", (uint32_t)pass);
                return -1;
        };
    };

    m_fallback_vtable[DEVICE_IS_STEREO_EYE_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsStereoEyePass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        return pass != EStereoscopicPass::eSSP_FULL;
    }; // DeviceIsStereoEyePass

    m_fallback_vtable[DEVICE_IS_STEREO_EYE_VIEW_INDEX] = +[](FFakeStereoRendering* stereo, const sdk::FSceneView& view) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsStereoEyeView called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)get_stereo_pass(view));
    #endif

        return get_stereo_pass(view) != EStereoscopicPass::eSSP_FULL;
    }; // DeviceIsStereoEyePass

    m_fallback_vtable[DEVICE_IS_A_PRIMARY_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsAPrimaryPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        return pass == EStereoscopicPass::eSSP_FULL || pass == EStereoscopicPass::eSSP_PRIMARY;
    }; // DeviceIsAPrimaryPass

    m_fallback_vtable[DEVICE_IS_A_PRIMARY_VIEW_INDEX] = +[](FFakeStereoRendering* stereo, const sdk::FSceneView& view) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsAPrimaryView called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)get_stereo_pass(view));
    #endif

        return get_stereo_pass(view) == EStereoscopicPass::eSSP_FULL || get_stereo_pass(view) == EStereoscopicPass::eSSP_PRIMARY;
    }; // DeviceIsAPrimaryPass

    m_fallback_vtable[DEVICE_IS_A_SECONDARY_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsASecondaryPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        return !(pass == EStereoscopicPass::eSSP_FULL || pass == EStereoscopicPass::eSSP_PRIMARY);
    }; // DeviceIsASecondaryPass

    m_fallback_vtable[DEVICE_IS_A_SECONDARY_VIEW_INDEX] = +[](FFakeStereoRendering* stereo, const sdk::FSceneView& view) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("DeviceIsASecondaryView called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)get_stereo_pass(view));
    #endif

        return get_stereo_pass(view) > EStereoscopicPass::eSSP_PRIMARY;
    }; // DeviceIsASecondaryView

    m_special_detected_4_27 = true;
    m_manually_constructed = true;
    m_fallback_device.vtable = m_fallback_vtable.data();

    auto& current_device = *(uintptr_t*)((uintptr_t)engine + *stereo_rendering_device_offset);
    SPDLOG_INFO("Current device: {:x}", current_device);
    current_device = (uintptr_t)&m_fallback_device; // TODO: Automatically find this offset.

    // So the view extension hook will work.
    s_stereo_rendering_device_offset = *stereo_rendering_device_offset;

    hook_game_viewport_client();
    setup_view_extensions();

    SPDLOG_INFO("Finished creating stereo device for the game using nonstandard method");

    m_finished_hooking = true;

    return true;
}

bool FFakeStereoRenderingHook::nonstandard_create_stereo_device_hook_4_22() {
    SPDLOG_INFO("Attempting to create a stereo device for the game using nonstandard method (4.22)");

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot create stereo device!");
        return false;
    }

    m_fallback_vtable.resize(30);

    // Give all of the functions placeholders.
    for (auto i = 0; i < m_fallback_vtable.size(); ++i) {
        m_fallback_vtable[i] = +[](FFakeStereoRendering* stereo) -> void* {
            return nullptr;
        };
    }

    constexpr auto DESTRUCTOR_INDEX = 0;
    constexpr auto IS_STEREO_ENABLED_INDEX = 1;
    constexpr auto IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX = 2;
    constexpr auto ENABLE_STEREO_INDEX = 3;

    constexpr auto GET_DESIRED_NUMBER_OF_VIEWS_INDEX = 4;
    constexpr auto GET_VIEW_PASS_FOR_INDEX_INDEX = 5;
    constexpr auto GET_VIEW_INDEX_FOR_PASS_INDEX = 6;
    constexpr auto IS_STEREO_EYE_PASS_INDEX = 7;

    constexpr auto ADJUST_VIEW_RECT_INDEX = 8;
    constexpr auto CALCULATE_STEREO_VIEW_OFFSET_INDEX = ADJUST_VIEW_RECT_INDEX + 3;
    constexpr auto CALCULATE_STEREO_PROJECTION_MATRIX_INDEX = CALCULATE_STEREO_VIEW_OFFSET_INDEX + 1;
    constexpr auto RENDER_TEXTURE_RENDER_THREAD_INDEX = CALCULATE_STEREO_PROJECTION_MATRIX_INDEX + 2;
    constexpr auto GET_RENDER_TARGET_MANAGER_INDEX = RENDER_TEXTURE_RENDER_THREAD_INDEX + 1;

    auto stereo_rendering_device_offset = sdk::UEngine::get_stereo_rendering_device_offset();
    if (!stereo_rendering_device_offset) {
        stereo_rendering_device_offset = 0xAB8; // fallback for the engine this was originally made for.
    }

    // Actually implement the ones we care about now.
    m_fallback_vtable[DESTRUCTOR_INDEX] = +[](FFakeStereoRendering* stereo) -> void { SPDLOG_INFO("Destructor called?");  }; // destructor.
    m_fallback_vtable[IS_STEREO_ENABLED_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { 
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoEnabled called: {:x}", (uintptr_t)_ReturnAddress());
#endif

        return g_hook->is_stereo_enabled(stereo); 
    }; // IsStereoEnabled

    m_fallback_vtable[IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // IsStereoEnabledOnNextFrame
    m_fallback_vtable[ENABLE_STEREO_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // EnableStereo

    m_fallback_vtable[ADJUST_VIEW_RECT_INDEX] = +[](FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) { 
        return g_hook->adjust_view_rect(stereo, index, x, y, w, h);
    }; // AdjustViewRect

    m_fallback_vtable[CALCULATE_STEREO_VIEW_OFFSET_INDEX] = 
    +[](FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        return g_hook->calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location);
    }; // CalculateStereoViewOffset

    m_fallback_vtable[CALCULATE_STEREO_PROJECTION_MATRIX_INDEX] = +[](FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("CalculateStereoProjectionMatrix called: {:x} {} {:x}", (uintptr_t)_ReturnAddress(), view_index, (uintptr_t)out);
#endif

        if (!g_hook->m_has_double_precision) {
            (*out)[3][2] = 0.1f; // Need to pre-set the Z value to something, otherwise it will be 0.0f & probably break something.
        } else {
            auto dmat = (Matrix4x4d*)out;
            (*dmat)[3][2] = 0.1;
        }

        return g_hook->calculate_stereo_projection_matrix(stereo, out, view_index);
    }; // CalculateStereoProjectionMatrix

    m_fallback_vtable[RENDER_TEXTURE_RENDER_THREAD_INDEX] = 
    +[](FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list, FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) {
        return g_hook->render_texture_render_thread(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    };

    m_fallback_vtable[GET_RENDER_TARGET_MANAGER_INDEX] = +[](FFakeStereoRendering* stereo) { return g_hook->get_render_target_manager_hook(stereo); }; // GetRenderTargetManager

    m_fallback_vtable[GET_DESIRED_NUMBER_OF_VIEWS_INDEX] = +[](FFakeStereoRendering* stereo, bool stereo_enabled) -> int32_t { 
        return g_hook->get_desired_number_of_views_hook(stereo, stereo_enabled); 
    }; // GetDesiredNumberOfViews

    m_fallback_vtable[GET_VIEW_PASS_FOR_INDEX_INDEX] = +[](FFakeStereoRendering* stereo, bool stereo_requested, const uint32_t view_index) -> EStereoscopicPass {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetViewPassForIndex called: {:x} {} ", (uintptr_t)_ReturnAddress(), view_index);
    #endif

        return g_hook->get_view_pass_for_index_hook(stereo, stereo_requested, view_index);
    }; // GetViewPassForIndex

    m_fallback_vtable[GET_VIEW_INDEX_FOR_PASS_INDEX] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> int32_t {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("GetViewIndexForPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        switch (pass) {
            case EStereoscopicPass::eSSP_FULL:
            case EStereoscopicPass::eSSP_PRIMARY:
                return 0;
            
            case EStereoscopicPass::eSSP_SECONDARY:
                return 1;
            
            default:
                SPDLOG_ERROR("Unknown pass: {}", (uint32_t)pass);
                return -1;
        };
    };

    m_fallback_vtable[IS_STEREO_EYE_PASS_INDEX ] = +[](FFakeStereoRendering* stereo, const EStereoscopicPass pass) -> bool {
    #ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoPass called: {:x} {} ", (uintptr_t)_ReturnAddress(), (uint32_t)pass);
    #endif

        return pass != EStereoscopicPass::eSSP_FULL;
    };

    m_special_detected_4_22 = true;
    m_manually_constructed = true;
    m_fallback_device.vtable = m_fallback_vtable.data();

    auto& current_device = *(uintptr_t*)((uintptr_t)engine + *stereo_rendering_device_offset);
    SPDLOG_INFO("Current device: {:x}", current_device);
    current_device = (uintptr_t)&m_fallback_device; // TODO: Automatically find this offset.

    // So the view extension hook will work.
    s_stereo_rendering_device_offset = *stereo_rendering_device_offset;

    hook_game_viewport_client();
    setup_view_extensions();

    SPDLOG_INFO("Finished creating stereo device for the game using nonstandard method");

    m_finished_hooking = true;

    return true;
}

bool FFakeStereoRenderingHook::nonstandard_create_stereo_device_hook_4_18() {
    SPDLOG_INFO("Attempting to create a stereo device for the game using nonstandard method (4.18)");

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot create stereo device!");
        return false;
    }

    m_fallback_vtable.resize(30);

    // Give all of the functions placeholders.
    for (auto i = 0; i < m_fallback_vtable.size(); ++i) {
        m_fallback_vtable[i] = +[](FFakeStereoRendering* stereo) -> void* {
            return nullptr;
        };
    }

    constexpr auto IS_STEREO_ENABLED_INDEX = 0;
    constexpr auto IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX = 1;
    constexpr auto ENABLE_STEREO_INDEX = 2;

    constexpr auto ADJUST_VIEW_RECT_INDEX = 3;
    constexpr auto CALCULATE_STEREO_VIEW_OFFSET_INDEX = ADJUST_VIEW_RECT_INDEX + 2;
    constexpr auto CALCULATE_STEREO_PROJECTION_MATRIX_INDEX = CALCULATE_STEREO_VIEW_OFFSET_INDEX + 1;
    constexpr auto RENDER_TEXTURE_RENDER_THREAD_INDEX = CALCULATE_STEREO_PROJECTION_MATRIX_INDEX + 3;
    constexpr auto GET_RENDER_TARGET_MANAGER_INDEX = RENDER_TEXTURE_RENDER_THREAD_INDEX + 3;

    auto stereo_rendering_device_offset = sdk::UEngine::get_stereo_rendering_device_offset();
    if (!stereo_rendering_device_offset) {
        stereo_rendering_device_offset = 0xAE8; // fallback for the engine this was originally made for.
    }

    // Actually implement the ones we care about now.
    m_fallback_vtable[IS_STEREO_ENABLED_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { 
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("IsStereoEnabled called: {:x}", (uintptr_t)_ReturnAddress());
#endif

        return g_hook->is_stereo_enabled(stereo); 
    }; // IsStereoEnabled

    m_fallback_vtable[IS_STEREO_ENABLED_ON_NEXT_FRAME_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // IsStereoEnabledOnNextFrame
    m_fallback_vtable[ENABLE_STEREO_INDEX] = +[](FFakeStereoRendering* stereo) -> bool { return g_hook->is_stereo_enabled(stereo); }; // EnableStereo

    m_fallback_vtable[ADJUST_VIEW_RECT_INDEX] = +[](FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) { 
        return g_hook->adjust_view_rect(stereo, index, x, y, w, h);
    }; // AdjustViewRect

    m_fallback_vtable[CALCULATE_STEREO_VIEW_OFFSET_INDEX] = 
    +[](FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location) {
        return g_hook->calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location);
    }; // CalculateStereoViewOffset

    m_fallback_vtable[CALCULATE_STEREO_PROJECTION_MATRIX_INDEX] = +[](FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
        SPDLOG_INFO("CalculateStereoProjectionMatrix called: {:x} {} {:x}", (uintptr_t)_ReturnAddress(), view_index, (uintptr_t)out);
#endif

        if (!g_hook->m_has_double_precision) {
            (*out)[3][2] = 0.1f; // Need to pre-set the Z value to something, otherwise it will be 0.0f & probably break something.
        } else {
            auto dmat = (Matrix4x4d*)out;
            (*dmat)[3][2] = 0.1;
        }

        return g_hook->calculate_stereo_projection_matrix(stereo, out, view_index);
    }; // CalculateStereoProjectionMatrix

    m_fallback_vtable[RENDER_TEXTURE_RENDER_THREAD_INDEX] = 
    +[](FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list, FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) {
        return g_hook->render_texture_render_thread(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    };

    m_fallback_vtable[GET_RENDER_TARGET_MANAGER_INDEX] = +[](FFakeStereoRendering* stereo) { return g_hook->get_render_target_manager_hook(stereo); }; // GetRenderTargetManager

    m_special_detected_4_18 = true;
    m_uses_old_rendertarget_manager = true; // this engine has a funny render target manager.
    m_manually_constructed = true;
    m_fallback_device.vtable = m_fallback_vtable.data();

    auto& current_device = *(uintptr_t*)((uintptr_t)engine + *stereo_rendering_device_offset);
    SPDLOG_INFO("Current device: {:x}", current_device);
    current_device = (uintptr_t)&m_fallback_device; // TODO: Automatically find this offset.

    // So the view extension hook will work.
    s_stereo_rendering_device_offset = *stereo_rendering_device_offset;

    hook_game_viewport_client();
    setup_view_extensions();

    SPDLOG_INFO("Finished creating stereo device for the game using nonstandard method");

    m_finished_hooking = true;

    return true;
}

bool FFakeStereoRenderingHook::hook_game_viewport_client() try {
    SPDLOG_INFO("Attempting to hook UGameViewportClient::Draw...");

    // We need to cache the canvas index before we hook the draw function or else this doesn't work.
    sdk::FViewport::get_debug_canvas_index();
    auto game_viewport_client_draw = sdk::UGameViewportClient::get_draw_function();

    if (!game_viewport_client_draw) {
        SPDLOG_ERROR("Failed to find UGameViewportClient::Draw!");
        m_has_game_viewport_client_draw_hook = false;
        return false;
    }

    m_gameviewportclient_draw_hook = safetyhook::create_inline((void*)*game_viewport_client_draw, &game_viewport_client_draw_hook, safetyhook::InlineHook::StartDisabled);
    m_has_game_viewport_client_draw_hook = true;

    if (!m_gameviewportclient_draw_hook) {
        SPDLOG_ERROR("Failed to hook UGameViewportClient::Draw!");
        return false;
    }

    if (auto enable_result = m_gameviewportclient_draw_hook.enable(); !enable_result.has_value()) {
        SPDLOG_ERROR("Failed to enable UGameViewportClient::Draw hook!");
        return false;
    }

    return true;
} catch(std::exception& e) {
    SPDLOG_ERROR("Failed to hook UGameViewportClient: {}", e.what());
    return false;
} catch(...) {
    SPDLOG_ERROR("Failed to hook UGameViewportClient!");
    return false;
}

void* FFakeStereoRenderingHook::viewport_destructor_hook(void* viewport, void* a2, void* a3, void* a4) {
    ZoneScopedN(__FUNCTION__);

    SPDLOG_INFO("FViewport::~FViewport called: {:x}", (uintptr_t)_ReturnAddress());

    // Call the original destructor.
    auto call_orig = [&]() -> void* {
        ZoneScopedN("FViewport::~FViewport");
        auto res = g_hook->m_viewport_destructor_hook->get_original<decltype(&viewport_destructor_hook)>()(viewport, a2, a3, a4);
        g_hook->m_last_destroyed_viewport = viewport;

        return res;
    };

    if (!g_framework->is_game_data_intialized()) {
        return call_orig();
    }

    auto vr = VR::get();

    if (!vr->is_hmd_active()) {
        return call_orig();
    }

    static bool once = true;

    if (once) {
        SPDLOG_INFO("FViewport::Destructor called for the first time.");
        once = false;
    }

    return call_orig();
}

void FFakeStereoRenderingHook::viewport_draw_hook(void* viewport, bool should_present) {
    ZoneScopedN(__FUNCTION__);

    g_hook->m_last_viewport_vtable = *(void***)viewport;

    auto call_orig = [&]() {
        ZoneScopedN("FViewport::Draw");
        g_hook->m_viewport_draw_hook.call(viewport, should_present);
    };

    if (!g_framework->is_game_data_intialized()) {
        call_orig();
        return;
    }

    if (g_hook->m_viewport_destructor_hook == nullptr) {
        static bool already_tried = false;

        if (!already_tried) {
            already_tried = true;
            auto& vtable = *(void***)viewport;

            if (vtable != nullptr && vtable[0] != nullptr) {
                // Destructors usually have some kind of test reg8, 01 instruction within them.
                if (utility::find_pattern_in_path((uint8_t*)vtable[0], 0x100, false, "F6 ? 01")) {
                    SPDLOG_INFO("Found TEST mnemonic for FViewport destructor at {:x}", (uintptr_t)vtable[0]);
                    SPDLOG_INFO("Hooking FViewport::~FViewport at {:x}", (uintptr_t)vtable[0]);
                    g_hook->m_viewport_destructor_hook = std::make_unique<PointerHook>(&vtable[0], &viewport_destructor_hook);
                } else {
                    SPDLOG_ERROR("Failed to find FViewport destructor pattern at {:x}", (uintptr_t)vtable[0]);
                }
            }
        }
    }

    if (g_hook->m_ignore_next_viewport_draw) {
        g_hook->m_ignore_next_viewport_draw = false;
        return;
    }

    auto vr = VR::get();

    if (!vr->is_hmd_active()) {
        call_orig();
        return;
    }

    static bool once = true;

    if (once) {
        SPDLOG_INFO("FViewport::Draw called for the first time.");
        once = false;
    }

    call_orig();
}

// This function needs some more work for more rigorous filtering
// However it does its job on the relevant titles
// This is only used for the UI compatibility mode.
FRHITexture2D** FFakeStereoRenderingHook::viewport_get_render_target_texture_hook(sdk::FViewport* viewport) {
    const auto retaddr = (uintptr_t)_ReturnAddress();

    SPDLOG_INFO_ONCE("FViewport::GetRenderTargetTexture called!");
    const auto og = g_hook->m_viewport_get_render_target_texture_hook->get_original<decltype(&viewport_get_render_target_texture_hook)>();
    const auto& vr = VR::get();

    if (!vr->is_ahud_compatibility_enabled() || !vr->is_hmd_active() || g_hook->m_slate_draw_window_thread_id == 0) {
        return og(viewport);
    }

    auto& data = g_hook->m_viewport_rt_hook_data;

    {
        std::scoped_lock _{data.retaddr_mutex};
        utility::ScopeGuard guard{[&](){ data.seen_retaddrs.insert(retaddr); }};

        if (data.call_original_retaddrs.contains(retaddr)) {
            return og(viewport);
        }

        std::optional<size_t> func_start{};

        // ALWAYS check the retaddr for ViewFamilyTexture first and never skip it
        // This will fix the case where we run into some other texture initially.
        if (!data.seen_retaddrs.contains(retaddr)) {
            SPDLOG_INFO("FViewport::GetRenderTargetTexture called from {:x}", retaddr);

            func_start = utility::find_function_start(retaddr);

            if (!func_start) {
                func_start = retaddr;
            }

            // The function that has this string reference should ALWAYS get passed
            // back to the original function, this is the actual scene render target.
            // Everything else we will redirect to the UI render target.
            if (utility::find_string_reference_in_path(*func_start, L"ViewFamilyTexture", false) || utility::find_string_reference_in_path(*func_start, L"ViewFamilyTarget", false)) {
                SPDLOG_INFO("Found view family texture reference @ {:x}", retaddr);
                data.call_original_retaddrs.insert(retaddr);
                data.has_view_family_tex = true;
                return og(viewport);
            }

            // We should always allow the viewport when used in a post processing context to go through.
            // There's two because this function stops itself at 200 instructions
            // doing a second one from the retaddr allows us to go further.
            if (utility::find_string_reference_in_path(*func_start, L"FinalPostProcessColor", false) || utility::find_string_reference_in_path(retaddr, L"FinalPostProcessColor", false)) {
                SPDLOG_INFO("Found FinalPostProcessColor reference @ {:x}", retaddr);
                data.call_original_retaddrs.insert(retaddr);
                return og(viewport);
            }

            const auto next_fn_call = utility::scan_disasm(retaddr, 0x30, "E8 ? ? ? ?");

            if (next_fn_call) {
                const auto fn = utility::calculate_absolute(*next_fn_call + 1);

                // I don't know of any other way to check this. I'm not sure what this function is.
                // It seems like deep within a threaded or function for enqueueing a render command.
                if (utility::scan(fn, 0x50, "01 01 01 01") && utility::scan(fn, 0x50, "22 00 00 00")) {
                    SPDLOG_INFO("Found unknown screen space rendering call @ {:x}", retaddr);
                    data.redirected_retaddrs.insert(retaddr);
                }
            }

            // There are multiple other HAL references we can use too.
            static const auto hal_clear_solid_rectangle_fn = utility::find_function_from_string_ref(utility::get_executable(), "HAL::ClearSolidRectangle");
            static std::unordered_set<uintptr_t> scaleform_hal_vtable_functions{};

            const auto is_scaleform = hal_clear_solid_rectangle_fn.has_value();

            if (hal_clear_solid_rectangle_fn.has_value() && scaleform_hal_vtable_functions.empty()) try {
                scaleform_hal_vtable_functions.insert(*hal_clear_solid_rectangle_fn);

                SPDLOG_INFO("Found HAL::ClearSolidRectangle function @ {:x}", *hal_clear_solid_rectangle_fn);
                std::vector<uintptr_t> scaleform_hal_vtable_refs{};
                const auto module_size = utility::get_module_size(utility::get_executable()).value_or(0);
                const auto start = (uintptr_t)utility::get_executable();
                const auto end = (uintptr_t)utility::get_executable() + module_size;
                const auto hal_module = utility::get_module_within(*hal_clear_solid_rectangle_fn).value_or(nullptr);

                // There are multiple HAL vtable, so just collect all of them.
                for (auto i = start; i < end - 0x1000; i += sizeof(uintptr_t)) {
                    const auto remaining = end - i;
                    const auto function_ptr = utility::scan_ptr(i, remaining - 0x1000, *hal_clear_solid_rectangle_fn);

                    if (!function_ptr.has_value()) {
                        break;
                    }

                    i = *function_ptr;

                    SPDLOG_INFO("Found HAL::ClearSolidRectangle function pointer @ {:x}", *function_ptr);
                    for (auto j = 0; j < 100; ++j) {
                        const auto entry = *(uintptr_t*)(*function_ptr + (j * sizeof(uintptr_t)));

                        if (entry == 0 || IsBadReadPtr((void*)entry, sizeof(uintptr_t))) {
                            break;
                        }

                        const auto is_same_module = utility::get_module_within(entry).value_or(nullptr) == hal_module;

                        if (!is_same_module) {
                            break;
                        }

                        scaleform_hal_vtable_functions.insert(entry);
                    }
                }
            } catch(...) {
                SPDLOG_ERROR("Failed to find Scaleform HAL vtable functions!");
            }

            if (is_scaleform && !scaleform_hal_vtable_functions.empty()) try {
                // Walk the stack, get function starts and check if any are in the vtable
                constexpr auto max_stack_depth = 100;
                uintptr_t stack[max_stack_depth]{};

                const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);

                for (auto i = 0; i < depth; ++i) {
                    SPDLOG_INFO(" Stack[{}]: {:x}", i, stack[i]);
                }

                bool found = false;

                for (auto i = 1; i < std::min<uint16_t>(7, depth); ++i) {
                    const auto scaleform_func_start = utility::find_virtual_function_start(stack[i]);

                    if (!scaleform_func_start) {
                        continue;
                    }

                    if (scaleform_hal_vtable_functions.contains(*scaleform_func_start)) {
                        SPDLOG_INFO("Found Scaleform HAL vtable function reference @ {:x}", retaddr);
                        data.redirected_retaddrs.insert(retaddr);
                        found = true;
                        break;
                    }
                }
            } catch(...) {
                SPDLOG_ERROR("Failed to walk stack for scaleform vtable functions!");
            }
        }

        // Hacky way to allow the first texture to go through
        // For the games that are using something other than ViewFamilyTexture as the scene RT.
        if (!data.call_original_retaddrs.empty() && !data.redirected_retaddrs.contains(retaddr) && !data.has_view_family_tex) {
            return og(viewport);
        }

        if (!data.redirected_retaddrs.contains(retaddr) && !data.call_original_retaddrs.contains(retaddr)) {
            if (!func_start) {
                func_start = utility::find_function_start(retaddr);

                if (!func_start) {
                    func_start = retaddr;
                }
            }

            // Probably NOT...
            /*if (utility::find_string_reference_in_path(*func_start, L"r.RHICmdAsyncRHIThreadDispatch")) {
                SPDLOG_INFO("Found RHICmdAsyncRHIThreadDispatch reference @ {:x}", retaddr);
                call_original_retaddrs.insert(retaddr);
                return og(viewport);
            }*/

            // TODO? this needs some more rigorous filtering
            // some games are insane and have multiple "UnknownTexture" references...
            if (utility::find_string_reference_in_path(*func_start, L"UnknownTexture", false)) {
                SPDLOG_INFO("Found unknown texture reference @ {:x}", retaddr);
                data.call_original_retaddrs.insert(retaddr);
                return og(viewport);
            }

            SPDLOG_INFO("Redirecting FViewport::GetRenderTargetTexture call to UI render target @ {:x}", retaddr);
            data.redirected_retaddrs.insert(retaddr);
        }
    }

    // Finally redirect the call to the UI render target.
    auto& ui_target = g_hook->get_render_target_manager()->get_ui_target();

    if (ui_target != nullptr) {
        return &ui_target;
    }

    return og(viewport);
}

void FFakeStereoRenderingHook::game_viewport_client_draw_hook(sdk::UGameViewportClient* viewport_client, sdk::FViewport* viewport, sdk::FCanvas* canvas, void* a4) {
    ZoneScopedN(__FUNCTION__);

    // UI compatibility mode
    // Tries to redirect calls to GetRenderTargetTexture to point towards our UI
    // texture instead of the scene render target, if it's not the scene itself/the view family texture.
    // This usually isn't needed but sometimes there are bespoke changes to the rendering pipeline
    // or uses of the AHUD class that make it necessary.
    if (g_framework->is_game_data_intialized() && VR::get()->is_ahud_compatibility_enabled() && viewport != nullptr) {
        if (g_hook->m_viewport_get_render_target_texture_hook == nullptr) {
            SPDLOG_INFO("Hooking FViewport::GetRenderTargetTexture...");
            void** vp_vtable = *(void***)viewport;
            g_hook->m_viewport_get_render_target_texture_hook = std::make_unique<PointerHook>(&vp_vtable[1], &viewport_get_render_target_texture_hook);
            SPDLOG_INFO("Hooked FViewport::GetRenderTargetTexture!");
        }
    }

    auto call_orig = [=]() {
        ZoneScopedN("UGameViewportClient::Draw");
        g_hook->m_gameviewportclient_draw_hook.call(viewport_client, viewport, canvas, a4);
    };

    SPDLOG_INFO_ONCE("UGameViewportClient::Draw called for the first time.");

    if (!g_framework->is_game_data_intialized()) {
        call_orig();
        return;
    }

    g_hook->m_in_viewport_client_draw = true;
    g_hook->m_was_in_viewport_client_draw = false;
    g_hook->get_render_target_manager()->set_viewport(viewport);

    utility::ScopeGuard _{ 
        []() { 
            g_hook->m_in_viewport_client_draw = false;
            g_hook->m_was_in_viewport_client_draw = false;
        } 
    };

    auto vr = VR::get();

    if (!vr->is_hmd_active()) {
        call_orig();
        return;
    }

    static uint32_t hook_attempts = 0;
    static bool run_anyways = false;

    if (hook_attempts < 100 && !g_hook->m_hooked_game_engine_tick && g_hook->m_attempted_hook_game_engine_tick) {
        ZoneScopedN("UGameViewportClient::Draw (hook UGameEngine::Tick)");
        SPDLOG_INFO("Performing alternative UGameEngine::Tick hook for synced AFR.");

        ++hook_attempts;

        // Go up the stack and find the viewport draw function.
        constexpr auto max_stack_depth = 100;
        uintptr_t stack[max_stack_depth]{};

        const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);

        for (auto i = 0; i < depth; ++i) {
            SPDLOG_INFO("Stack[{}]: {:x}", i, stack[i]);
        }

        for (auto i = 3; i < depth; ++i) {
            const auto ret = stack[i];

            g_hook->attempt_hook_game_engine_tick(ret);

            if (g_hook->m_hooked_game_engine_tick) {
                SPDLOG_INFO("Successfully hooked UGameEngine::Tick for synced AFR.");
                break;
            }
        }
    } else {
        run_anyways = !g_hook->m_hooked_game_engine_tick;
    }

    const auto in_engine_tick = g_hook->m_in_engine_tick;

    if (run_anyways || in_engine_tick) {
        if (g_hook->m_has_view_extension_hook) {
            g_frame_count = vr->get_runtime()->internal_frame_count;
            vr->update_hmd_state(true, vr->get_runtime()->internal_frame_count + 1);
        } else {
            vr->update_hmd_state(false);
        }
    }

    const auto& mods = g_framework->get_mods()->get_mods();

    for (const auto& mod : mods) {
        mod->on_pre_viewport_client_draw(viewport_client, viewport, canvas);
    }

    call_orig();

    // Perform synced eye rendering (synced AFR)
    if (in_engine_tick && vr->is_using_synchronized_afr()) {
        static bool hooked_viewport_draw = false;

        // Hook for FViewport::Draw
        if (g_hook->m_hooked_game_engine_tick && !hooked_viewport_draw) {
            hooked_viewport_draw = true;

            // Go up the stack and find the viewport draw function.
            constexpr auto max_stack_depth = 100;
            uintptr_t stack[max_stack_depth]{};

            const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);
            if (depth >= 2) {
                // Log the stack functions
                for (auto i = 0; i < depth; ++i) {
                    SPDLOG_INFO("(Stack[{}]: {:x}", i, stack[i]);
                }

                auto try_hook_index = [&](uint32_t index) -> bool {
                    SPDLOG_INFO("Attempting to locate FViewport::Draw function @ stack[{}]", index);

                    const auto viewport_draw_middle = stack[index];
                    const auto viewport_draw = utility::find_function_start_with_call(viewport_draw_middle);

                    if (!viewport_draw) {
                        SPDLOG_ERROR("Failed to find viewport draw function @ {}", index);
                        return false;
                    }

                    SPDLOG_INFO("Found FViewport::Draw function at {:x}", (uintptr_t)*viewport_draw); 

                    g_hook->m_viewport_draw_hook = safetyhook::create_inline((void*)*viewport_draw, &viewport_draw_hook);

                    if (!g_hook->m_viewport_draw_hook) {
                        SPDLOG_ERROR("Failed to hook FViewport::Draw function!");
                        return false;
                    }

                    return true;
                };

                if (!try_hook_index(1)) {
                    // Fallback to index 3, on some UE4 games the viewport draw function is called from a different stack index.
                    if (!try_hook_index(2)) {
                        SPDLOG_ERROR("Failed to find viewport draw function! Cannot perform synced AFR!");
                    }
                }
            }
        }
    }

    // This is how synchronized AFR works. it forces a world draw
    // on the start of the next engine tick, before the world ticks again.
    // that will allow both views and the world to be drawn in sync with no artifacts.
    if (in_engine_tick && vr->is_using_synchronized_afr() && g_frame_count % 2 == 0) {
        GameThreadWorker::get().enqueue([=]() {
            if (g_hook->m_viewport_draw_hook && viewport != g_hook->m_last_destroyed_viewport) {
                __try {
                    if (*(void***)viewport != g_hook->m_last_viewport_vtable) {
                        SPDLOG_ERROR("FViewport::Draw called on a viewport with a different vtable! This is not expected!");
                        return;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    SPDLOG_ERROR("FViewport::Draw called with a bad viewport pointer! This is not expected!");
                    return;
                }

                const auto viewport_draw = (void (*)(void*, bool))g_hook->m_viewport_draw_hook.target();
                viewport_draw(viewport, true);

                auto& vr = VR::get();
                const auto method = vr->get_synced_sequential_method();
                
                if (method == VR::SyncedSequentialMethod::SKIP_TICK) {
                    g_hook->m_ignore_next_engine_tick = true;
                    //g_hook->m_ignore_next_viewport_draw = true;
                } else if (method == VR::SyncedSequentialMethod::SKIP_DRAW) {
                    g_hook->m_ignore_next_viewport_draw = true;
                }
            }
        });
    }

    for (const auto& mod : mods) {
        mod->on_post_viewport_client_draw(viewport_client, viewport, canvas);
    }
}

static std::array<uintptr_t, 50> g_view_extension_vtable{};
struct SceneViewExtensionAnalyzer;

// Analyzes all of the virtual functions for ISceneViewExtension
// We create the ISceneViewExtension ourselves and overwrite all of the virtual functions
// The class will count how many times each virtual is getting called
// and then when a threshold is reached, it finds the most called one
// the most called one is IsActiveThisFrame which we need to activate the ISceneViewExtension
struct SceneViewExtensionAnalyzer {
    template<int N>
    struct FillVtable {
        static void fill(std::array<uintptr_t, 50>& table);
        static void fill2(std::array<uintptr_t, 50>& table);
    };

    template<>
    struct FillVtable<-1> {
        static void fill(std::array<uintptr_t, 50>& table) {}
        static void fill2(std::array<uintptr_t, 50>& table) {}
    };

    struct AnalyzedFunction {
        uint32_t call_count{0};
        uint32_t frame_count_a2{0};
        uint32_t frame_count_a3{0};
        uint32_t frame_count_offset_a2{0};
        uint32_t frame_count_offset_a3{0};
        uint32_t times_frame_count_correct_a2{0};
        uint32_t times_frame_count_correct_a3{0};
        std::array<uint8_t, 0x100> a2_data{};
        std::array<uint8_t, 0x100> a3_data{};
    };

    static inline std::recursive_mutex dummy_mutex{};
    static inline uint32_t total_call_count{};
    static inline std::unordered_map<uint32_t, AnalyzedFunction> functions{};
    static inline bool has_found_is_active_this_frame_index{false};
    static inline bool has_found_begin_render_viewfamily{false};
    static inline bool index_0_called{false};
    
    static inline uint32_t is_active_this_frame_index{0};
    static inline uint32_t begin_render_viewfamily_index{0};
    static inline uint32_t pre_render_viewfamily_renderthread_index{0};
    static inline uint32_t frame_count_offset{0};

    template<int N>
    static bool analysis_dummy_stage1(ISceneViewExtension* extension, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
        if (N == 0) {
            index_0_called = true;
        }

        if (has_found_is_active_this_frame_index) {
            return false;
        }

        std::scoped_lock _{dummy_mutex};

        auto& func = functions[N];

        ++total_call_count;
        ++functions[N].call_count;

        if (total_call_count >= 50) {
            // Find the most called index, it's going to be IsActiveThisFrame
            uint32_t max_count = 0;
            uint32_t max_index = 0;

            for (const auto& func : functions) {
                const auto count = func.second.call_count;
                const auto index = func.first;

                if (count > max_count) {
                    max_count = count;
                    max_index = index;
                }
            }

            SPDLOG_INFO("[Stage 1] Found most called index to be {} with {} calls", max_index, max_count);

            functions.clear();
            FillVtable<g_view_extension_vtable.size() - 1>::fill2(g_view_extension_vtable);

            // Force the function to return true
            g_view_extension_vtable[max_index] = (uintptr_t)+[](ISceneViewExtension* ext) -> bool {
                return true;
            };

            has_found_is_active_this_frame_index = true;
            is_active_this_frame_index = max_index;
        } else {
            if (functions[N].call_count == 1) {
                SPDLOG_INFO("[Stage 1] ISceneViewExtension Index {} called for the first time!", N);
            }
        }

        return false;
    };

    template<int N>
    static bool analysis_dummy_stage2(ISceneViewExtension* extension, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
        if (has_found_begin_render_viewfamily) {
            return false;
        }

        if (N == 0) {
            index_0_called = true;
        }

        std::scoped_lock _{dummy_mutex};

        if (functions.contains(N)) {
            auto& func = functions[N];

            if (func.call_count++ == 0) {
                SPDLOG_INFO("[Stage 2] SceneViewExtension Index {} called for the first time!", N);
            }

            const auto& last_view_family_data_a2 = func.a2_data;
            const auto view_family_a2 = (uintptr_t)a2;

            if (a2 != 0 && !IsBadReadPtr((void*)a2, 0x100)) {
                for (auto i = 0x10; i < last_view_family_data_a2.size(); i += sizeof(uint32_t)) {
                    const auto a = *(uint32_t*)&last_view_family_data_a2[i];
                    const auto b = *(uint32_t*)&((uint8_t*)view_family_a2)[i];

                    if (b == a + 1 && a >= 10) { // rule out really low frame counts (this could be something else)
                        if (func.frame_count_a2 + 1 == b) {
                            SPDLOG_INFO("[A2] Function index {} Found frame count offset at {:x}, ({})", N, i, b);

                            func.frame_count_offset_a2 = i;
                            ++func.times_frame_count_correct_a2;

                            // func_next is one of the functions ahead of N and has the frame count in a3
                            AnalyzedFunction* func_next = nullptr;
                            uint32_t next_index = 0;

                            for (auto j = N + 1; j < g_view_extension_vtable.size(); j++) {
                                if (functions.contains(j)) {
                                    const auto& next = functions[j];

                                    if (next.times_frame_count_correct_a3 >= 10) {
                                        func_next = &functions[j];
                                        next_index = j;
                                        break;
                                    }
                                }
                            }

                            if (func_next != nullptr) {
                                if (func.times_frame_count_correct_a2 >= 50 && 
                                    func_next->times_frame_count_correct_a3 >= 50 && 
                                    func.frame_count_offset_a2 == func_next->frame_count_offset_a3 &&
                                    std::abs((int32_t)func.frame_count_a2 - (int32_t)func_next->frame_count_a3) <= 3) // In some games, the frame delta is really high but the same offset (so, it's wrong)
                                {
                                    SPDLOG_INFO("Found final frame count offset at {:x}", i);
                                    SPDLOG_INFO("Found BeginRenderViewFamily at index {}", N);
                                    SPDLOG_INFO("Found PreRenderViewFamily_RenderThread at index {}", next_index);
                                    has_found_begin_render_viewfamily = true;
                                    begin_render_viewfamily_index = N;
                                    pre_render_viewfamily_renderthread_index = next_index;

                                    frame_count_offset = i;
                                    sdk::FSceneViewFamily::set_frame_count_offset(frame_count_offset);

                                    setup_view_extension_hook();
                                    return false;
                                }   
                            }
                        }

                        func.frame_count_a2 = b;
                        break;
                    }
                }
            }

            const auto& last_view_family_data_a3 = func.a3_data;
            const auto view_family_a3 = (uintptr_t)a3;

            if (a3 != 0 && !IsBadReadPtr((void*)a3, 0x100)) {
                for (auto i = 0x10; i < last_view_family_data_a3.size(); i += sizeof(uint32_t)) {
                    const auto a = *(uint32_t*)&last_view_family_data_a3[i];
                    const auto b = *(uint32_t*)&((uint8_t*)view_family_a3)[i];

                    if (b == a + 1 && a >= 10) { // rule out really low frame counts (this could be something else)
                        if (func.frame_count_a3 + 1 == b) {
                            SPDLOG_INFO("[A3] Function index {} Found frame count offset at {:x} ({})", N, i, b);
                            ++func.times_frame_count_correct_a3;
                        }

                        func.frame_count_a3 = b;
                        func.frame_count_offset_a3 = i;
                        break;
                    }
                }
            }
        }

        if (a2 != 0 && !IsBadReadPtr((void*)a2, 0x100)) {
            memcpy(functions[N].a2_data.data(), (void*)a2, 0x100);
        }

        if (a3 != 0 && !IsBadReadPtr((void*)a3, 0x100)) {
            memcpy(functions[N].a3_data.data(), (void*)a3, 0x100);
        }

        return false;
    }

    static inline std::recursive_mutex vtable_mutex{};
    static inline std::unordered_map<sdk::FRHICommandBase_New*, void**> original_vtables{};
    static inline std::unordered_map<sdk::FRHICommandBase_New*, uint32_t> cmd_frame_counts{};

    // Meant to be called after analysis has been completed
    static void setup_view_extension_hook() {
        std::scoped_lock _{dummy_mutex};

        SPDLOG_INFO("Setting up BeginRenderViewFamily hook...");

        const auto setup_view_family_index = index_0_called ? 0 : 1;

        g_view_extension_vtable[setup_view_family_index] = (uintptr_t)&FFakeStereoRenderingHook::setup_view_family;
        g_view_extension_vtable[begin_render_viewfamily_index] = (uintptr_t)&FFakeStereoRenderingHook::begin_render_viewfamily;

        if (!index_0_called && (setup_view_family_index + 2) != begin_render_viewfamily_index) {
            g_view_extension_vtable[setup_view_family_index + 2] = (uintptr_t)&FFakeStereoRenderingHook::setup_viewpoint;
        }

        // PreRenderViewFamily_RenderThread
        g_view_extension_vtable[pre_render_viewfamily_renderthread_index] = (uintptr_t)&FFakeStereoRenderingHook::pre_render_viewfamily_renderthread;

        SPDLOG_INFO("Done setting up BeginRenderViewFamily hook!");
    }

    static inline std::unordered_set<int> tested_execute_indices{};
    static inline int correct_execute_index{0};
    static inline bool found_correct_execute{false};

    template<int N>
    static void* hooked_command_fn(sdk::FRHICommandBase_New* cmd, sdk::FRHICommandListBase* cmd_list, void* debug_context, void* r9, void* stack_1, void* stack_2, void* stack_3, void* stack_4, void* stack_5, void* stack_6, void* stack_7, void* stack_8) {
        std::scoped_lock _{vtable_mutex};
        //std::scoped_lock __{VR::get()->get_vr_mutex()};

        static bool once = true;

        if (once) {
            SPDLOG_INFO("[ISceneViewExtension] Successfully hijacked command list! {}", N);
        }

        const auto original_vtable = original_vtables[cmd];
        const auto original_func = original_vtable[N];

        const auto func = (decltype(hooked_command_fn<N>)*)original_func;
        const auto frame_count = cmd_frame_counts[cmd];

        if (once) {
            SPDLOG_INFO("[ISceneViewExtension] Command list frame count: {}", frame_count);
            SPDLOG_INFO("[ISceneViewExtension] Original vtable: {:x}", (uintptr_t)original_vtable);
            once = false;
        }

        if (!found_correct_execute && !tested_execute_indices.contains(N) && VR::get()->get_present_thread_id() != 0) {
            tested_execute_indices.insert(N);

            // N == 0 is a pretty safe heuristic
            // Otherwise if >= 1 gets called first, we can assume if the thread is the same
            // as the DXGI present thread, then it's the correct execute function
            if (N == 0 || GetCurrentThreadId() == VR::get()->get_present_thread_id()) {
                correct_execute_index = N;
                found_correct_execute = true;
                SPDLOG_INFO("[ISceneViewExtension] Found correct execute index: {}", N);
            }
        }

        auto& vr = VR::get();
        auto runtime = vr->get_runtime();

        auto call_orig = [=]() {
            const auto result = func(cmd, cmd_list, debug_context, r9, stack_1, stack_2, stack_3, stack_4, stack_5, stack_6, stack_7, stack_8);

            if (N == correct_execute_index) {
                runtime->enqueue_render_poses(frame_count);
            }

            return result;
        };

        if (N != correct_execute_index) {
            return call_orig();
        }

        // set the vtable back
        *(void**)cmd = original_vtable;
        original_vtables.erase(cmd);
        cmd_frame_counts.erase(cmd);

        RHIThreadWorker::get().execute();

        if (vr->get_synchronize_stage() == VR::SynchronizeStage::EARLY) {
            if (runtime->is_openxr()) {
                if (g_framework->get_renderer_type() == Framework::RendererType::D3D11) {
                    if (!runtime->got_first_sync || runtime->synchronize_frame(frame_count) != VRRuntime::Error::SUCCESS) {
                        return call_orig();
                    }  
                } else if (runtime->synchronize_frame(frame_count) != VRRuntime::Error::SUCCESS) {
                    return call_orig();
                }

                vr->get_openxr_runtime()->begin_frame();
            } else {
                if (runtime->synchronize_frame(frame_count) != VRRuntime::Error::SUCCESS) {
                    return call_orig();
                }
            }
        }

        return call_orig();
    }

    static void hook_new_rhi_command(sdk::FRHICommandBase_New* last_command, uint32_t frame_count) {
        std::scoped_lock __{vtable_mutex};

        auto runtime = VR::get()->get_runtime();
        runtime->on_pre_render_render_thread(frame_count);

        if (last_command == nullptr || *(void**)last_command == nullptr) {
            SPDLOG_INFO("Cannot hook command with no vtable, falling back to passing current frame count to runtime");
            runtime->enqueue_render_poses(frame_count);
            return;
        }

        // Whichever one gets called first is the winner winner chicken dinner
        static std::array<uintptr_t, 7> new_vtable{
            (uintptr_t)&hooked_command_fn<0>,
            (uintptr_t)&hooked_command_fn<1>,
            (uintptr_t)&hooked_command_fn<2>,
            (uintptr_t)&hooked_command_fn<3>,
            (uintptr_t)&hooked_command_fn<4>,
            (uintptr_t)&hooked_command_fn<5>,
            (uintptr_t)&hooked_command_fn<6>
        };

        cmd_frame_counts[last_command] = frame_count;

        if (original_vtables.contains(last_command) || *(void**)last_command == new_vtable.data()) {
            static auto last_log_time = std::chrono::high_resolution_clock::time_point{};
            const auto now = std::chrono::high_resolution_clock::now();
            
            if (now - last_log_time > std::chrono::seconds(1)) {
                SPDLOG_WARN("Something strange is going on, the vtable is already hooked, maybe previous frame was not rendered?");
                last_log_time = now;
            }

            return;
        }

        original_vtables[last_command] = *(void***)last_command;
        *(void***)last_command = (void**)new_vtable.data();
    }

    static void hook_old_rhi_command(sdk::FRHICommandBase_Old* last_command, uint32_t frame_count) {
        static std::recursive_mutex func_mutex{};
        static std::unordered_map<sdk::FRHICommandBase_Old*, sdk::FRHICommandBase_Old::Func> original_funcs{};
        static std::unordered_map<sdk::FRHICommandBase_Old*, uint32_t> cmd_frame_counts{};

        std::scoped_lock __{func_mutex};

        auto runtime = VR::get()->get_runtime();
        runtime->on_pre_render_render_thread(frame_count);

        cmd_frame_counts[last_command] = frame_count;

        if (original_funcs.contains(last_command)) {
            static auto last_log_time = std::chrono::high_resolution_clock::time_point{};
            const auto now = std::chrono::high_resolution_clock::now();
            
            if (now - last_log_time > std::chrono::seconds(1)) {
                SPDLOG_WARN("Something strange is going on, the function is already hooked, maybe previous frame was not rendered?");
                last_log_time = now;
            }

            return;
        }

        static auto func_override = (sdk::FRHICommandBase_Old::Func)+[](sdk::FRHICommandListBase* cmd_list, sdk::FRHICommandBase_Old* cmd) {
            std::scoped_lock _{func_mutex};
            //std::scoped_lock __{VR::get()->get_vr_mutex()};

            static bool once = true;

            if (once) {
                SPDLOG_INFO("[ISceneViewExtension] Successfully hijacked command list!");
                once = false;
            }

            auto& vr = VR::get();
            auto runtime = vr->get_runtime();

            const auto func = original_funcs[cmd];
            const auto frame_count = cmd_frame_counts[cmd];

            runtime->enqueue_render_poses(frame_count);
            runtime->on_pre_render_rhi_thread(frame_count);

            auto call_orig = [&]() {
                func(*cmd_list, cmd);
            };

            cmd->func = func;
            original_funcs.erase(cmd);
            cmd_frame_counts.erase(cmd);

            RHIThreadWorker::get().execute();

            if (vr->get_synchronize_stage() == VR::SynchronizeStage::EARLY) {
                if (runtime->is_openxr()) {
                    if (g_framework->get_renderer_type() == Framework::RendererType::D3D11) {
                        if (!runtime->got_first_sync || runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                            return call_orig();
                        }  
                    } else if (runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                        return call_orig();
                    }

                    vr->get_openxr_runtime()->begin_frame();
                } else {
                    if (runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                        return call_orig();
                    }
                }
            }

            return call_orig();
        };

        original_funcs[last_command] = last_command->func;
        last_command->func = func_override;
    }
};

template<int N>
void SceneViewExtensionAnalyzer::FillVtable<N>::fill(std::array<uintptr_t, 50>& table) {
    table[N] = (uintptr_t)&SceneViewExtensionAnalyzer::analysis_dummy_stage1<N>;
    FillVtable<N - 1>::fill(table);
}

template<int N>
void SceneViewExtensionAnalyzer::FillVtable<N>::fill2(std::array<uintptr_t, 50>& table) {
    table[N] = (uintptr_t)&SceneViewExtensionAnalyzer::analysis_dummy_stage2<N>;
    FillVtable<N - 1>::fill2(table);
}

// 4.25something to 4.27
// TODO: Add support for all versions via PDB dumps
constexpr auto INIT_OPTIONS_OFFSET = 0x50;

bool FFakeStereoRenderingHook::is_in_viewport_client_draw() const {
    return m_in_viewport_client_draw && GameThreadWorker::get().is_same_thread();
}

bool FFakeStereoRenderingHook::bind_ghosting_fix_owner(GhostingFixPair& pair, const char* log_label) {
    GhostingFixOwnerCandidate candidate{};
    GhostingOwnerResolveDiagnostic object_array_diagnostic{};
    GhostingOwnerResolveDiagnostic object_hook_diagnostic{};
    bool resolved = ghosting_resolve_current_owner(
        pair.eye_state[0],
        pair.eye_state[1],
        candidate,
        GhostingUObjectValidationMode::ObjectArray,
        object_array_diagnostic);

    const bool hook_fallback_available = ghosting_can_use_uobject_hook();
    if (!resolved && hook_fallback_available) {
        resolved = ghosting_resolve_current_owner(
            pair.eye_state[0],
            pair.eye_state[1],
            candidate,
            GhostingUObjectValidationMode::UObjectHook,
            object_hook_diagnostic);
    }

    if (!resolved) {
        if (!pair.logged_owner_unavailable) {
            pair.logged_owner_unavailable = true;
            SPDLOG_WARN(
                "[{}] Scene-state owner resolution failed closed "
                "direct_stage={} direct_player={}/{} hook_available={} hook_stage={} hook_player={}/{}",
                log_label,
                ghosting_owner_failure_name(object_array_diagnostic.failure),
                object_array_diagnostic.local_player_index,
                object_array_diagnostic.local_player_count,
                hook_fallback_available,
                hook_fallback_available
                    ? ghosting_owner_failure_name(object_hook_diagnostic.failure)
                    : "unavailable",
                object_hook_diagnostic.local_player_index,
                object_hook_diagnostic.local_player_count);
        }
        return false;
    }

    pair.owner = {
        .engine = candidate.engine,
        .engine_vtable = candidate.engine_vtable,
        .engine_class = candidate.engine_class,
        .engine_index = candidate.engine_index,
        .engine_serial = candidate.engine_serial,
        .game_instance_slot = candidate.game_instance_slot,
        .game_instance = candidate.game_instance,
        .game_instance_vtable = candidate.game_instance_vtable,
        .game_instance_class = candidate.game_instance_class,
        .game_instance_index = candidate.game_instance_index,
        .game_instance_serial = candidate.game_instance_serial,
        .local_players_header = candidate.local_players_header,
        .local_players_data = candidate.local_players_data,
        .local_players_count = candidate.local_players_count,
        .local_players_capacity = candidate.local_players_capacity,
        .local_player_slot = candidate.local_player_slot,
        .local_player = candidate.local_player,
        .local_player_vtable = candidate.local_player_vtable,
        .local_player_class = candidate.local_player_class,
        .local_player_index = candidate.local_player_index,
        .local_player_serial = candidate.local_player_serial,
        .view_states_header = candidate.view_states_header,
        .view_states_data = candidate.view_states_data,
        .view_states_count = candidate.view_states_count,
        .view_states_capacity = candidate.view_states_capacity,
        .view_state_stride = candidate.view_state_stride,
        .view_state_reference_vtable = candidate.view_state_reference_vtable,
        .eye_state_slot = {candidate.eye_state_slot[0], candidate.eye_state_slot[1]},
        .viewport_client_slot = candidate.viewport_client_slot,
        .viewport_client = candidate.viewport_client,
        .viewport_client_vtable = candidate.viewport_client_vtable,
        .viewport_client_class = candidate.viewport_client_class,
        .viewport_client_index = candidate.viewport_client_index,
        .viewport_client_serial = candidate.viewport_client_serial,
        .world_slot = candidate.world_slot,
        .world = candidate.world,
        .world_vtable = candidate.world_vtable,
        .world_class = candidate.world_class,
        .world_index = candidate.world_index,
        .world_serial = candidate.world_serial,
        .last_validated_frame = g_frame_count,
        .stable_frames = 1,
        .view_states_are_array = candidate.view_states_are_array,
        .uses_uobject_hook_validation = candidate.uses_uobject_hook_validation,
        .verified = true,
    };

    if (candidate.uses_uobject_hook_validation) {
        SPDLOG_WARN(
            "[{}] Bound exact LocalPlayer scene-state ownership through the authoritative UObjectHook set "
            "after direct FUObjectArray validation failed at stage={} owner={:x} storage={} stride=0x{:x}",
            log_label,
            ghosting_owner_failure_name(object_array_diagnostic.failure),
            reinterpret_cast<uintptr_t>(candidate.local_player),
            candidate.view_states_are_array ? "array" : "legacy pair",
            candidate.view_state_stride);
    }

    pair.logged_owner_unavailable = false;
    pair.logged_owner_stabilizing = false;
    pair.logged_owner_validation_failed = false;
    return true;
}

bool FFakeStereoRenderingHook::validate_ghosting_fix_owner(
    const GhostingFixPair& pair,
    const char** failure_stage)
{
    if (failure_stage != nullptr) {
        *failure_stage = nullptr;
    }

    const auto fail = [&](const char* stage) {
        if (failure_stage != nullptr) {
            *failure_stage = stage;
        }
        return false;
    };

    const auto& owner = pair.owner;
    if (!owner.verified ||
        !ghosting_is_valid_scene_state(pair.eye_state[0]) ||
        !ghosting_is_valid_scene_state(pair.eye_state[1]) ||
        pair.eye_state[0] == pair.eye_state[1])
    {
        return fail("scene states");
    }

    const auto validation_mode = owner.uses_uobject_hook_validation
        ? GhostingUObjectValidationMode::UObjectHook
        : GhostingUObjectValidationMode::ObjectArray;
    const bool validate_individual_membership = !owner.uses_uobject_hook_validation;

    if (owner.uses_uobject_hook_validation) {
        auto& object_hook = UObjectHook::get();
        const std::array<sdk::UObjectBase*, 5> objects{
            reinterpret_cast<sdk::UObjectBase*>(owner.engine),
            reinterpret_cast<sdk::UObjectBase*>(owner.game_instance),
            reinterpret_cast<sdk::UObjectBase*>(owner.local_player),
            reinterpret_cast<sdk::UObjectBase*>(owner.viewport_client),
            reinterpret_cast<sdk::UObjectBase*>(owner.world),
        };
        if (!ghosting_can_use_uobject_hook() ||
            !object_hook->all_exist(objects.data(), objects.size()))
        {
            return fail("UObjectHook membership");
        }
    }

    const GhostingUObjectIdentity engine_identity{
        owner.engine_vtable,
        owner.engine_class,
        owner.engine_index,
        owner.engine_serial,
    };
    const GhostingUObjectIdentity game_instance_identity{
        owner.game_instance_vtable,
        owner.game_instance_class,
        owner.game_instance_index,
        owner.game_instance_serial,
    };
    const GhostingUObjectIdentity local_player_identity{
        owner.local_player_vtable,
        owner.local_player_class,
        owner.local_player_index,
        owner.local_player_serial,
    };
    const GhostingUObjectIdentity viewport_client_identity{
        owner.viewport_client_vtable,
        owner.viewport_client_class,
        owner.viewport_client_index,
        owner.viewport_client_serial,
    };
    const GhostingUObjectIdentity world_identity{
        owner.world_vtable,
        owner.world_class,
        owner.world_index,
        owner.world_serial,
    };

    if (!ghosting_is_live_uobject(
            owner.engine,
            validation_mode,
            &engine_identity,
            nullptr,
            validate_individual_membership) ||
        !ghosting_is_live_uobject(
            owner.game_instance,
            validation_mode,
            &game_instance_identity,
            nullptr,
            validate_individual_membership) ||
        !ghosting_is_live_uobject(
            owner.local_player,
            validation_mode,
            &local_player_identity,
            nullptr,
            validate_individual_membership) ||
        !ghosting_is_live_uobject(
            owner.viewport_client,
            validation_mode,
            &viewport_client_identity,
            nullptr,
            validate_individual_membership) ||
        !ghosting_is_live_uobject(
            owner.world,
            validation_mode,
            &world_identity,
            nullptr,
            validate_individual_membership))
    {
        return fail("UObject identity");
    }

    uintptr_t current_game_instance{};
    uintptr_t current_local_player{};
    uintptr_t current_viewport_client{};
    uintptr_t current_world{};
    if (!safe_read_value(owner.game_instance_slot, current_game_instance) ||
        current_game_instance != reinterpret_cast<uintptr_t>(owner.game_instance) ||
        !safe_read_value(owner.local_player_slot, current_local_player) ||
        current_local_player != reinterpret_cast<uintptr_t>(owner.local_player) ||
        !safe_read_value(owner.viewport_client_slot, current_viewport_client) ||
        current_viewport_client != reinterpret_cast<uintptr_t>(owner.viewport_client) ||
        !safe_read_value(owner.world_slot, current_world) ||
        current_world != reinterpret_cast<uintptr_t>(owner.world))
    {
        return fail("owner pointer chain");
    }

    GhostingRawArrayHeader local_players{};
    if (!ghosting_read_array_header(owner.local_players_header, 8, 32, local_players) ||
        local_players.data != owner.local_players_data ||
        local_players.count != owner.local_players_count ||
        local_players.capacity != owner.local_players_capacity)
    {
        return fail("LocalPlayers array");
    }

    int32_t view_state_count = owner.view_states_count;
    if (owner.view_states_are_array) {
        GhostingRawArrayHeader view_states{};
        if (!ghosting_read_array_header(owner.view_states_header, 8, 16, view_states) ||
            view_states.data != owner.view_states_data ||
            view_states.count != owner.view_states_count ||
            view_states.capacity != owner.view_states_capacity)
        {
            return fail("ViewStates array");
        }
        view_state_count = view_states.count;
    } else if (owner.view_states_header != 0 ||
               owner.view_states_count != 2 ||
               owner.view_states_capacity != 2)
    {
        return fail("legacy view-state pair");
    }

    const auto storage_size = static_cast<size_t>(view_state_count) * owner.view_state_stride;
    if (owner.view_state_stride == 0 ||
        storage_size / owner.view_state_stride != static_cast<size_t>(view_state_count) ||
        !is_readable_process_range(owner.view_states_data, storage_size))
    {
        return fail("scene-state storage bounds");
    }

    uintptr_t current_eye_state[2]{};
    uintptr_t current_reference_vtable[2]{};
    if (!safe_read_value(owner.eye_state_slot[0], current_eye_state[0]) ||
        !safe_read_value(owner.eye_state_slot[1], current_eye_state[1]) ||
        !safe_read_value(owner.eye_state_slot[0] - sizeof(uintptr_t), current_reference_vtable[0]) ||
        !safe_read_value(owner.eye_state_slot[1] - sizeof(uintptr_t), current_reference_vtable[1]) ||
        current_reference_vtable[0] != owner.view_state_reference_vtable ||
        current_reference_vtable[1] != owner.view_state_reference_vtable ||
        current_eye_state[0] != reinterpret_cast<uintptr_t>(pair.eye_state[0]) ||
        current_eye_state[1] != reinterpret_cast<uintptr_t>(pair.eye_state[1]))
    {
        return fail("eye-state slots");
    }

    return true;
}

bool FFakeStereoRenderingHook::refresh_ghosting_fix_owner(GhostingFixPair& pair, const char* log_label) {
    if (!pair.owner.verified && !bind_ghosting_fix_owner(pair, log_label)) {
        return false;
    }

    // UObject GC and LocalPlayer mutation run on the game thread. A successful
    // validation remains authoritative for later views in this same engine frame.
    if (pair.owner.last_validated_frame == g_frame_count) {
        return true;
    }

    const char* failure_stage{};
    if (!validate_ghosting_fix_owner(pair, &failure_stage)) {
        if (!pair.logged_owner_validation_failed) {
            pair.logged_owner_validation_failed = true;
            SPDLOG_WARN(
                "[{}] Verified owner became invalid at stage={}; keeping remap fail-closed "
                "scene={:x} generation={} owner={:x}",
                log_label,
                failure_stage != nullptr ? failure_stage : "unknown",
                pair.scene,
                pair.generation,
                reinterpret_cast<uintptr_t>(pair.owner.local_player));
        }
        return false;
    }

    pair.logged_owner_validation_failed = false;
    pair.owner.last_validated_frame = g_frame_count;
    if (pair.owner.stable_frames < std::numeric_limits<uint32_t>::max()) {
        ++pair.owner.stable_frames;
    }

    return true;
}

namespace {
// FSceneViewInitOptions::ViewFamily is reached through a SCANNED offset. When
// that scan lands wrong the "family" is whatever bytes live there, and reading
// its scene interface dereferences a non-canonical address — which Windows
// reports as EXCEPTION_ACCESS_VIOLATION "reading address 0xffffffffffffffff",
// inside the engine's own FSceneView constructor (Hellblade 2 on inject;
// FANTASY LIFE i likewise).
//
// Probe both reads under SEH and report absence rather than dying: every
// consumer downstream already has a path for a null family/scene. Its own
// function because sceneview_constructor holds objects that need unwinding, and
// __try cannot coexist with those in one frame.
__declspec(noinline) bool try_read_view_family_and_scene(sdk::FSceneViewInitOptions* init_options,
                                                         sdk::FSceneViewFamily** out_family,
                                                         sdk::FSceneInterface** out_scene) {
    *out_family = nullptr;
    *out_scene = nullptr;

    __try {
        auto* family = init_options->get_view_family();

        if (family != nullptr) {
            *out_scene = family->get_scene_interface();
        }

        *out_family = family;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out_family = nullptr;
        *out_scene = nullptr;
        return false;
    }
}
}

// FSceneView constructor hook
sdk::FSceneView* FFakeStereoRenderingHook::sceneview_constructor(sdk::FSceneView* view, sdk::FSceneViewInitOptions* init_options, void* a3, void* a4) {
    SPDLOG_INFO_ONCE("Called FSceneView constructor for the first time");

    auto& vr = VR::get();

    if (!g_hook->is_in_viewport_client_draw() || !vr->is_hmd_active()) {
        return g_hook->m_sceneview_data.constructor_hook.unsafe_call<sdk::FSceneView*>(view, init_options, a3, a4);
    }

    if (g_hook->m_analyzing_view_extensions || !g_hook->m_has_view_extensions_installed) {
        SPDLOG_INFO_ONCE("FSceneView constructor was called before view extensions were installed, aborting");
        return g_hook->m_sceneview_data.constructor_hook.unsafe_call<sdk::FSceneView*>(view, init_options, a3, a4);
    }

    std::scoped_lock ___{g_hook->m_sceneview_data.mtx};

    const auto retaddr = (uintptr_t)_ReturnAddress();

    if (!g_hook->m_sceneview_data.seen_retaddrs.contains(retaddr)) {
        g_hook->m_sceneview_data.seen_retaddrs.insert(retaddr);
        SPDLOG_INFO("FSceneView constructor called from {:x}", retaddr);
    }

    sdk::FSceneViewInitOptionsBase::update_offsets(init_options);

    if (auto view_family = init_options->get_view_family(); view_family != nullptr) {
        sdk::FSceneViewFamily::update_offsets(view_family, nullptr);
    }

    const auto is_ue5 = g_hook->has_double_precision();
    auto init_options_ue5 = (sdk::FSceneViewInitOptionsUE5*)init_options;

    const auto init_options_scene_state = init_options->get_scene_state();
    auto* native_effective_scene_state = init_options_scene_state;
    const auto init_options_original_stereo_pass = init_options->get_stereo_pass();
    const auto init_options_player_index = init_options->get_player_index();
    sdk::FSceneViewFamily* init_options_view_family = nullptr;
    sdk::FSceneInterface* init_options_scene = nullptr;
    if (!try_read_view_family_and_scene(init_options, &init_options_view_family, &init_options_scene)) {
        SPDLOG_WARN_ONCE("[SceneView] Reading FSceneViewInitOptions::ViewFamily faulted — the scanned offset is "
                         "wrong for this build. Continuing without the family; stereo may degrade to mono.");
    }
    const bool split_fiction_haze_context =
        g_split_fiction_haze_view_build.active &&
        split_fiction_is_current_game() &&
        is_ue_5_4_runtime() &&
        vr->is_splitscreen_compatibility_enabled();
    int32_t split_screen_metadata_player_index = init_options_player_index.value_or(-1);
    uint32_t split_screen_metadata_stereo_pass = init_options_original_stereo_pass;
    bool split_fiction_haze_metadata_active = false;
    bool restore_init_options_after_constructor = false;

    utility::ScopeGuard restore_init_options_guard{[&]() {
        if (!restore_init_options_after_constructor) {
            return;
        }

        init_options->set_scene_state(init_options_scene_state);
        init_options->set_stereo_pass(init_options_original_stereo_pass);
        if (init_options_player_index.has_value()) {
            init_options->set_player_index(*init_options_player_index);
        }
    }};

    if (init_options_scene_state != nullptr) {
        if (is_ue5) {
            auto& vio_entry = g_hook->m_sceneview_data.view_init_options_ue5[init_options_scene_state];
            memcpy(&vio_entry, init_options, sizeof(sdk::FSceneViewInitOptionsUE5));
        } else {
            auto& vio_entry = g_hook->m_sceneview_data.view_init_options_ue4[init_options_scene_state];
            memcpy(&vio_entry, init_options, sizeof(sdk::FSceneViewInitOptionsUE4));
        }
    }

    auto& known_scene_states = g_hook->m_sceneview_data.known_scene_states;
    auto& last_frame_count = g_hook->m_sceneview_data.last_frame_count;
    auto& last_index = g_hook->m_sceneview_data.last_index;

    if (last_frame_count != g_frame_count || last_index > 1) {
        last_index = 0;
    }

    last_frame_count = g_frame_count;

    const auto true_index = vr->is_using_afr() ? (g_frame_count + last_index) % 2 : last_index;

    if (vr->is_splitscreen_compatibility_enabled() || vr->is_sceneview_compatibility_enabled()) {
        int32_t w = vr->get_hmd_width();
        int32_t h = vr->get_hmd_height();

        int32_t x = 0;
        int32_t y = 0;

        if (!vr->is_using_afr() && true_index == 1 && !vr->is_native_stereo_fix_enabled()) {
            x += w;
        }

        FIntRect view_rect{x, y, x + w, y + h};

        vr->get_runtime()->update_matrices(0.1f, 10000.0f);

        const auto proj_mat = vr->get_projection_matrix((VRRuntime::Eye)(true_index));

        auto& init_options_view_origin = is_ue5 ? *(glm::vec3*)&init_options_ue5->view_origin : init_options->view_origin;
        auto& init_options_view_rect = is_ue5 ? init_options_ue5->view_rect : init_options->view_rect;
        auto& init_options_constrained_view_rect = is_ue5 ? init_options_ue5->constrained_view_rect : init_options->constrained_view_rect;
        auto& init_options_projection_matrix = init_options->projection_matrix;
        auto& init_options_projection_matrix_ue5 = init_options_ue5->projection_matrix;

        auto& init_options_view_rotation_matrix = init_options->view_rotation_matrix;
        auto& init_options_view_rotation_matrix_ue5 = init_options_ue5->view_rotation_matrix;

        const auto conversion_mat = glm::mat4 {
            0, 0, 1, 0,
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 0, 1
        };

        const auto conversion_mat_inverse = glm::inverse(conversion_mat);

        // We need to "undo" the operations done to create the rotation matrix so we can get the original angle
        // const auto view_rot_mat = conversion_mat * make_inverse_rot_matrix(euler); <-- this is the result of the conversion
        glm::vec3 euler{};

        if (is_ue5) {
            euler = utility::math::ue_euler_from_rotation_matrix(glm::inverse(conversion_mat_inverse * glm::mat4{init_options_view_rotation_matrix_ue5}));
        } else {
            euler = utility::math::ue_euler_from_rotation_matrix(glm::inverse(conversion_mat_inverse * init_options_view_rotation_matrix));
        }

        auto euler_d = glm::vec<3, double>{euler};
        auto euler_pointer = is_ue5 ? (Rotator<float>*)&euler_d : (Rotator<float>*)&euler;

        g_hook->calculate_stereo_view_offset_(true_index + 1, euler_pointer, 100.0f, &init_options_view_origin);

        if (is_ue5) {
            euler = euler_d;
        }

        const auto view_rot_mat = conversion_mat * utility::math::ue_inverse_rotation_matrix(euler);

        *(FIntRect*)&init_options_view_rect = view_rect;
        *(FIntRect*)&init_options_constrained_view_rect = view_rect;

        if (is_ue5) {
            init_options_view_rotation_matrix_ue5 = view_rot_mat;

            if (!vr->is_using_2d_screen()) {
                init_options_projection_matrix_ue5 = proj_mat;
            }
        } else {
            init_options_view_rotation_matrix = view_rot_mat;

            if (!vr->is_using_2d_screen()) {
                init_options_projection_matrix = proj_mat;
            }
        }
    }

    const auto init_options_stereo_pass = init_options->get_stereo_pass();

    std::optional<uint32_t> views_original_count{};

    if (vr->is_native_stereo_fix_enabled() && vr->is_native_stereo_fix_same_pass_enabled() && init_options_stereo_pass > EStereoscopicPass::eSSP_PRIMARY) {
        if (g_hook->get_render_target_manager()->get_scene_capture_render_target() != nullptr) {
            init_options->set_stereo_pass(EStereoscopicPass::eSSP_PRIMARY);

            auto view_family = init_options->get_view_family();
            auto views = view_family != nullptr ? view_family->get_views() : nullptr;

            if (views != nullptr) {
                // Hide the fact that we have multiple views from the FSceneView constructor.
                // At least 1 view causes special stereo logic to run in the constructor.
                // Notably I've seen more than 1 view causing crashes on UE5 with the native stereo fix without doing this.
                views_original_count = views->count;
                views->count = 0;
            }
        }
    }

    bool new_scene_state_inserted_this_frame = false;

    if (init_options_scene_state != nullptr && !g_hook->m_sceneview_data.known_scene_states.contains(init_options_scene_state)) {
        SPDLOG_INFO("Inserting new scene state {:x}", (uintptr_t)init_options_scene_state);
        known_scene_states.insert(init_options_scene_state);
        new_scene_state_inserted_this_frame = true;
    } else if (init_options_scene_state == nullptr) {
        SPDLOG_ERROR_ONCE("Scene state passed to FSceneView constructor is null");

        if ((int32_t)init_options_stereo_pass < 0) {
            SPDLOG_ERROR_ONCE("Stereo pass is negative");
        }
    }

    if (init_options_scene_state != nullptr && !new_scene_state_inserted_this_frame && vr->is_ghosting_fix_enabled() && !known_scene_states.empty() && vr->is_using_afr() && true_index == 1) {
        init_options->set_stereo_pass(EStereoscopicPass::eSSP_PRIMARY);
        auto& eye_pair = g_hook->m_sceneview_data.m_ghosting_fix_pair;
        if (eye_pair.eye_state[0] == init_options_scene_state) {
            eye_pair.last_seen_frame = g_frame_count;
        } else if (eye_pair.eye_state == nullptr || g_frame_count - eye_pair.last_seen_frame > 90) {
            eye_pair.eye_state[0] = init_options_scene_state;
            eye_pair.eye_state[1] = nullptr;
        }
        if (eye_pair.eye_state[0] == init_options_scene_state && eye_pair.eye_state[1]) {
            init_options->set_scene_state(eye_pair.eye_state[1]);
        }
        if (eye_pair.eye_state[0] == init_options_scene_state && !eye_pair.eye_state[1]) {
            // Set the scene state to the one that isn't the current one
            for (auto scene_state : known_scene_states) {
                if (scene_state != init_options_scene_state) {
                    SPDLOG_INFO_ONCE("Setting scene state to {:x}", (uintptr_t)scene_state);
                    init_options->set_scene_state(scene_state);
                    eye_pair.eye_state[1] = scene_state;
                    break;
                }
            }
        }
    }

    last_index++;

    auto result = g_hook->m_sceneview_data.constructor_hook.unsafe_call<sdk::FSceneView*>(view, init_options, a3, a4);

    // Reset the view count back to what it was.
    if (views_original_count.has_value()) {
        auto view_family = init_options->get_view_family();
        auto views = nsf_resolve_verified_views(view_family);

        if (views != nullptr) {
            views->count = views_original_count.value();
        }
    }

    return result;
}

void FFakeStereoRenderingHook::setup_view_family(ISceneViewExtension* extension, sdk::FSceneViewFamily& view_family) {
    ZoneScopedN("SetupViewFamily");

    static bool once = true;

    if (once) {
        SPDLOG_INFO("Called SetupViewFamily for the first time");
        once = false;
    }

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    auto& vr = VR::get();

    if (!vr->is_hmd_active()) {
        return;
    }

    //vr->update_hmd_state(true, vr->get_runtime()->internal_frame_count + 1);
}

void FFakeStereoRenderingHook::setup_viewpoint(ISceneViewExtension* extension, void* player_controller, void* view_info) {
    ZoneScopedN("SetupViewPoint");
    SPDLOG_INFO_ONCE("Called SetupViewPoint for the first time");

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    auto& vr = VR::get();

    if (!vr->is_ghosting_fix_enabled() || g_hook->m_fixed_localplayer_view_count) {
        return;
    }

    // Using this as a way to get to the localplayer
    static bool attempted_hook{false};

    // Fix localplayer view count
    if (!attempted_hook) {
        SPDLOG_INFO("Attempting to find caller of ISceneViewExtension::SetupViewPoint");

        attempted_hook = true;
        const auto return_address = (uintptr_t)_ReturnAddress();
        const auto caller = utility::find_virtual_function_start(return_address);

        if (!caller) {
            SPDLOG_ERROR("Failed to find caller of ISceneViewExtension::SetupViewPoint");
            return;
        }

        // No need to StartDisabled on this because we're on the same thread.
        g_hook->m_localplayer_get_viewpoint_hook = safetyhook::create_inline(*caller, (uintptr_t)&localplayer_setup_viewpoint);
        
        if (!g_hook->m_localplayer_get_viewpoint_hook) {
            SPDLOG_ERROR("Failed to hook ISceneViewExtension::SetupViewPoint");
            return;
        }

        SPDLOG_INFO("Hooked ISceneViewExtension::SetupViewPoint");
    }
}

void FFakeStereoRenderingHook::localplayer_setup_viewpoint(void* localplayer, void* view_info, void* pass) {
    ZoneScopedN("LocalPlayerSetupViewPoint");
    SPDLOG_INFO_ONCE("Called LocalPlayerSetupViewPoint for the first time");

    if (!g_hook->m_fixed_localplayer_view_count) {
        static bool attempted = false;

        if (!attempted) {
            attempted = true;

            if (localplayer != nullptr && !IsBadReadPtr(localplayer, sizeof(void*))) try {
                g_hook->post_init_properties((uintptr_t)localplayer);
            } catch(...) {
                SPDLOG_ERROR("[LocalPlayerSetupViewPoint] Failed to post init properties");
            }
        }
    }

    g_hook->m_localplayer_get_viewpoint_hook.call<void>(localplayer, view_info, pass);
}

// The hooked function is NOT always the 3-argument singular overload. The
// pre-UE5.7 resolver hooks whatever DIRECTLY called the view extension, which in
// modular builds is the wider renderer entry (Returnal:
// Returnal-Renderer-Win64-Shipping.dll). Re-calling that with only three
// arguments constructs the scene renderer with whatever garbage is left in
// r9/[rsp+0x28] — which AVs inside the engine's own view extensions a few frames
// after the NSF flow engages. Accept and forward both trailing arguments; on the
// older 3-argument shape the callee simply ignores them and passing them costs
// nothing.
static void nsf_brvf_member_handler(void* render_module, sdk::FCanvas* canvas,
                                    sdk::FSceneViewFamily* view_family_candidate,
                                    void* trailing_ptr_arg, uintptr_t trailing_flag_arg) {
    FFakeStereoRenderingHook::begin_render_viewfamily_real(
        render_module, canvas, view_family_candidate, trailing_ptr_arg, trailing_flag_arg);
}

void FFakeStereoRenderingHook::begin_render_viewfamily_real(void* render_module, sdk::FCanvas* canvas, sdk::FSceneViewFamily* view_family_candidate,
                                                            void* trailing_ptr_arg, uintptr_t trailing_flag_arg) {
    ZoneScopedN("BeginRenderViewFamilyReal");

    SPDLOG_INFO_ONCE("Called BeginRenderViewFamilyReal for the first time");

    if (!g_framework->is_game_data_intialized()) {
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate, trailing_ptr_arg, trailing_flag_arg);
        return;
    }

    auto& vr = VR::get();
    auto rtm = g_hook->get_render_target_manager();

    if (!vr->is_hmd_active() || !vr->is_native_stereo_fix_enabled()) {
        rtm->destroy_scene_capture();

        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }

    struct TArrayViewViewFamily {
        sdk::FSceneViewFamily** data;
        uint32_t count;
    };

    const auto uses_tarrayview = sdk::FSceneViewFamily::has_vtable() && *(void**)view_family_candidate != sdk::FSceneViewFamily::get_vtable_ptr();
    const auto ue5_view_family_array = (TArrayViewViewFamily*)view_family_candidate;

    if (uses_tarrayview && ue5_view_family_array->data == nullptr) {
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }

    // UE5 passes an TArrayView of ViewFamily pointers instead of a single ViewFamily
    sdk::FSceneViewFamily* view_family = uses_tarrayview ? ue5_view_family_array->data[0] : view_family_candidate;

    auto views_ptr = view_family->get_views();
    if (views_ptr == nullptr) {
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }

    auto& views = *views_ptr;
    const auto prev_count = views.count;

    const auto rt = rtm->get_scene_capture_utexture();
    const auto rtrsrc = rt != nullptr ? (sdk::FTextureRenderTargetResource*)rt->get_resource() : nullptr;
    const auto rtfrt = rtrsrc != nullptr ? rtrsrc->as_render_target() : nullptr;

    if (rtfrt == nullptr) {
        // This is fine to call constantly because we use an in-flight render target
        // that gets unset after the texture is fully created. This function exits early otherwise.
        rtm->create_scene_capture();
        views.count = 1;
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        views.count = prev_count;
        return;
    }

    auto view_family_target = view_family->get_render_target();

    if (view_family_target == nullptr) {
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);
        return;
    }

    bool wants_swap = false;
    if (views.count > 1) {
        views.count = 1;
        wants_swap = true;

        auto runtime = vr->get_runtime();
        const auto frame_count = runtime->internal_frame_count;

        // We need to clone the VR state from last frame to this frame
        if (runtime->is_openxr()) {
            auto openxr = (runtimes::OpenXR*)runtime;
            std::scoped_lock __{ openxr->sync_assignment_mtx };

            const auto last_frame = (frame_count) % runtimes::OpenXR::QUEUE_SIZE;
            const auto now_frame = (frame_count + 1) % runtimes::OpenXR::QUEUE_SIZE;
            openxr->pipeline_states[now_frame] = openxr->pipeline_states[last_frame];
            openxr->pipeline_states[now_frame].frame_count = now_frame;
        } else {
            auto openvr = (runtimes::OpenVR*)runtime;
            std::unique_lock __{ openvr->pose_mtx };

            const auto last_frame = (frame_count) % openvr->pose_queue.size();
            const auto now_frame = (frame_count + 1) % openvr->pose_queue.size();
            openvr->pose_queue[now_frame] = openvr->pose_queue[last_frame];
        }

        /*auto init_options = (sdk::FSceneViewInitOptions*)((uintptr_t)view_family.views.data[0] + INIT_OPTIONS_OFFSET);
        init_options->stereo_pass = 0;

        auto init_options2 = (sdk::FSceneViewInitOptions*)((uintptr_t)view_family.views.data[1] + INIT_OPTIONS_OFFSET);
        init_options2->stereo_pass = 0;

        std::array<uint8_t, 0x500> init_options_copy{};
        std::array<uint8_t, 0x500> init_options_copy2{};

        memcpy(init_options_copy.data(), init_options, 0x500);
        view_family.views.data[0]->constructor((sdk::FSceneViewInitOptions*)init_options_copy.data()); // Triggers our hook as well

        memcpy(init_options_copy2.data(), init_options2, 0x500);
        view_family.views.data[1]->constructor((sdk::FSceneViewInitOptions*)init_options_copy2.data());*/
    }

    g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);

    if (wants_swap) {
        // Swap out the existing render target for our custom one
        // Also, the entire point of swapping the render target
        // instead of "just" re-using the existing one is that doing that causes a 90% FPS drop
        // because the engine is still working on the old render target
        const auto original_target = view_family_target;

        view_family->set_render_target(rtfrt);

        auto scene = (sdk::FScene*)view_family->get_scene_interface();

        if (scene != nullptr) {
            // We decrement the frame count because it fixes motion vectors in the right eye.
            scene->decrement_frame_count();
        }
        
        std::swap(views[0], views[1]);

        // Call it again
        g_hook->m_render_module_begin_render_viewfamily_hook.unsafe_call<void>(render_module, canvas, view_family_candidate);

        std::swap(views[0], views[1]);

        view_family->set_render_target(original_target);
    }

    views.count = prev_count;
}

void FFakeStereoRenderingHook::begin_render_viewfamily(ISceneViewExtension* extension, sdk::FSceneViewFamily& view_family) {
    ZoneScopedN("BeginRenderViewFamily");

    SPDLOG_INFO_ONCE("Called BeginRenderViewFamily for the first time");

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    sdk::FSceneViewFamily::update_offsets(&view_family, g_hook->get_render_target_manager()->get_viewport());
    auto si = view_family.get_scene_interface();

    if (si != nullptr) {
        sdk::FScene::update_offsets((sdk::FScene*)si);
    }

    if (!g_hook->has_engine_tick_hook()) {
        // Alternative place of running game thread work.
        GameThreadWorker::get().execute();
    }

    auto& vr = VR::get();

    if (!vr->is_hmd_active()) {
        return;
    }

    const auto frame_count = *(uint32_t*)((uintptr_t)&view_family + SceneViewExtensionAnalyzer::frame_count_offset);
    auto views_ptr = view_family.get_views();

    //vr->update_hmd_state(true, frame_count);
    auto runtime = vr->get_runtime();
    runtime->internal_frame_count = frame_count;
    runtime->on_pre_render_game_thread(frame_count);

    // This is a HACKHACKHACK to get splitscreen working on around 4.20 to 4.27 something
    // This is completely borked on UE5
    // We can probably do it better inside the sceneview constructor hook, but that needs to be handled with care
    if (vr->is_splitscreen_compatibility_enabled() && views_ptr != nullptr) {
        auto& views = *views_ptr;
        
        // B = dst, A = src
        static auto copy_init_options_from = [](const sdk::FSceneView& a, sdk::FSceneView& b) {
            std::scoped_lock _{g_hook->m_sceneview_data.mtx};
            auto init_options_a = (sdk::FSceneViewInitOptions*)((uintptr_t)&a + INIT_OPTIONS_OFFSET);
            auto init_options_b = (sdk::FSceneViewInitOptions*)((uintptr_t)&b + INIT_OPTIONS_OFFSET);

            auto& cached_init_options = g_hook->m_sceneview_data.view_init_options_ue4;

            if (auto it = cached_init_options.find(init_options_a->scene_view_state); it != cached_init_options.end()) {
                const auto& vio_entry = it->second;
                //memcpy(init_options_b, &vio_entry, sizeof(sdk::FSceneViewInitOptionsUE4));
                init_options_b->view_origin = vio_entry.view_origin;
                init_options_b->view_rotation_matrix = vio_entry.view_rotation_matrix;
                *(FIntRect*)&init_options_b->view_rect = *(FIntRect*)&vio_entry.view_rect;
                *(FIntRect*)&init_options_b->constrained_view_rect = *(FIntRect*)&vio_entry.constrained_view_rect;
                init_options_b->projection_matrix = vio_entry.projection_matrix;
                return;
            }

            // Otherwise just do this crap
            init_options_b->view_origin = init_options_a->view_origin;
            init_options_b->view_rotation_matrix = init_options_a->view_rotation_matrix;
            *(FIntRect*)&init_options_b->view_rect = *(FIntRect*)&init_options_a->view_rect;
            *(FIntRect*)&init_options_b->constrained_view_rect = *(FIntRect*)&init_options_a->constrained_view_rect;
            init_options_b->projection_matrix = init_options_a->projection_matrix;
        };

        auto do_splitscreen = [&](int32_t view_index) {
            int32_t w = vr->get_hmd_width();
            int32_t h = vr->get_hmd_height();

            int32_t x = 0;
            int32_t y = 0;

            const auto true_index = vr->is_using_afr() ? (frame_count + 1) % 2 : view_index;

            if (!vr->is_using_afr() && true_index == 1) {
                x += w;
            }

            auto view = views.data[view_index % views.count];

            FIntRect view_rect{x, y, x + w, y + h};

            auto& vr = VR::get();

            VR::get()->get_runtime()->update_matrices(0.1f, 10000.0f);

            const auto proj_mat = VR::get()->get_projection_matrix((VRRuntime::Eye)(true_index));

            std::array<uint8_t, 0x500> init_options_copy{};

            auto init_options = (sdk::FSceneViewInitOptions*)((uintptr_t)view + INIT_OPTIONS_OFFSET);

            auto& init_options_view_origin = init_options->view_origin;
            auto& init_options_view_rotation_matrix = init_options->view_rotation_matrix;
            auto& init_options_view_rect = *(FIntRect*)&init_options->view_rect;
            auto& init_options_constrained_view_rect = *(FIntRect*)&init_options->constrained_view_rect;
            auto& init_options_projection_matrix = init_options->projection_matrix;
            auto& init_options_stereo_pass = init_options->stereo_pass;

            // ADDENDUM: The sceneview constructor hook handles the rotation logic now.
            /*const auto conversion_mat = glm::mat4 {
                0, 0, 1, 0,
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 0, 1
            };

            const auto conversion_mat_inverse = glm::inverse(conversion_mat);*/

            // We need to "undo" the operations done to create the rotation matrix so we can get the original angle
            // const auto view_rot_mat = conversion_mat * make_inverse_rot_matrix(euler); <-- this is the result of the conversion
            //auto euler = utility::math::ue_euler_from_rotation_matrix(glm::inverse(conversion_mat_inverse * init_options_view_rotation_matrix));
            //g_hook->calculate_stereo_view_offset_(true_index + 1, (Rotator<float>*)&euler, 100.0f, &init_options_view_origin);
            //const auto view_rot_mat = conversion_mat * utility::math::ue_inverse_rotation_matrix(euler);
            //init_options_view_rotation_matrix = view_rot_mat;

            init_options_view_rect = view_rect;
            init_options_constrained_view_rect = view_rect;
            init_options_projection_matrix = proj_mat;

            memcpy(init_options_copy.data(), init_options, 0x500);
            view->constructor((sdk::FSceneViewInitOptions*)init_options_copy.data()); // Triggers our hook as well
        };

        const auto requested_index = vr->get_requested_splitscreen_index();
        const auto final_index = std::min<uint32_t>(views.count - 1, requested_index);
        const auto other_index = final_index != 0 ? 0 : 1;

        if (final_index > 0) {
            if (views.count > 1) {
                copy_init_options_from(*views.data[final_index], *views.data[other_index]);
            }

            if (!vr->is_using_afr()) {
                if (views.count > 1) {
                    do_splitscreen(other_index);
                } else {
                    do_splitscreen(0);
                }
            } else {
                do_splitscreen(0);
            }
        }
    }

    // If we couldn't find GetDesiredNumberOfViews, we need to set the view count to 1 as a workaround
    // TODO: Check if this can cause a memory leak, I don't know who is resonsible
    // for destroying the views in the array
    // This check might seem kind of arbitrary, but sometimes (rarely) the offset
    // for the views can be wrong so if the count is some sane number
    // then we can assume that the offset is correct
    if (vr->is_using_afr() && views_ptr != nullptr && views_ptr->count >= 2 && views_ptr->count <= 4) {
        SPDLOG_INFO_ONCE("Setting view count to 1 (from {})", views_ptr->count);
        views_ptr->count = 1;
    }


    using BeginRenderViewFamilyRealFn = void(*)(void*, sdk::FCanvas*, sdk::FSceneViewFamily*);
    static BeginRenderViewFamilyRealFn begin_rendering_view_family_real_fn = nullptr;
    static bool already_tried = false;
    if (begin_rendering_view_family_real_fn == nullptr && !already_tried && vr->is_native_stereo_fix_enabled()) {
        already_tried = true;

        // Get callstack
        constexpr auto max_stack_depth = 100;
        uintptr_t stack[max_stack_depth]{};

        const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);
        uintptr_t mid = 0;

        for (int i = 1; i < depth; i++) {
            SPDLOG_INFO(" {:x}", (uintptr_t)stack[i]);
            mid = stack[i];
            break;
        }

        if (mid != 0) {
            // unwind first to find the actual function start.
            // find_virtual_function_start doesnt use unwind, (it subtracts by 1)
            // it can find false positives or nothing at all.
            const auto unwind = utility::find_function_start_unwind(mid);
            const auto candidate = utility::find_virtual_function_start(unwind ? *unwind : mid);

            if (candidate) {
                begin_rendering_view_family_real_fn = (BeginRenderViewFamilyRealFn)*candidate;

                if (begin_rendering_view_family_real_fn != nullptr) {
                    SPDLOG_INFO("Found BeginRenderingViewFamily real function at {:x}", (uintptr_t)begin_rendering_view_family_real_fn);

                    g_hook->m_render_module_begin_render_viewfamily_hook = safetyhook::create_inline((uintptr_t)begin_rendering_view_family_real_fn, (uintptr_t)&begin_render_viewfamily_real);

                    if (g_hook->m_render_module_begin_render_viewfamily_hook) {
                        SPDLOG_INFO("Hooked BeginRenderingViewFamily real function");
                    } else {
                        SPDLOG_ERROR("Failed to hook BeginRenderingViewFamily real function");
                    }
                } else {
                    SPDLOG_ERROR("Failed to find BeginRenderingViewFamily real function");
                }
            } else {
                SPDLOG_ERROR("Failed to find BeginRenderingViewFamily real function");
            }
        }
    }
}

void FFakeStereoRenderingHook::pre_render_viewfamily_renderthread(ISceneViewExtension* extension, sdk::FRHICommandListBase* cmd_list, sdk::FSceneViewFamily& view_family) {
    ZoneScopedN("PreRenderViewFamily_RenderThread");

    utility::ScopeGuard _{[]() {
        RenderThreadWorker::get().execute();
    }};
    
    SPDLOG_INFO_ONCE("Called PreRenderViewFamily_RenderThread for the first time");
    
    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    auto& vr = VR::get();

    if (!vr->is_hmd_active()) {
        return;
    }

    static size_t execution_count{0};

    // This should 100% only get executed if the headset is on, because
    // FFakeStereoRenderingHook::render_texture_render_thread is the first fallback for hooking
    // And we don't want to miss that unintentionally
    if (g_hook->m_attempted_hook_slate_thread && !g_hook->m_slate_thread_hook && !g_hook->m_attempted_hook_slate_thread_alternate && execution_count++ >= 50) {
        SPDLOG_INFO("DrawWindow_RenderThread was not hooked after {} render calls, trying alternative hook", execution_count);

        g_hook->attempt_hook_slate_thread(0, true);
    }

    if (vr->is_stereo_emulation_enabled()) {
        return;
    }

    const auto frame_count = *(uint32_t*)((uintptr_t)&view_family + SceneViewExtensionAnalyzer::frame_count_offset);
    static uint32_t last_frame = 0;

    // We only want to run this logic on the first "frame" (left eye) passed through here
    // When using Native Stereo Fix
    if (vr->is_native_stereo_fix_enabled() && frame_count == last_frame) {
        return;
    }

    last_frame = frame_count;

    static bool is_ue5_rdg_builder = false;
    static uint32_t ue5_command_offset = 0;
    static bool analyzed_root_already = false;
    static bool is_old_command_base = false;

    if (is_ue5_rdg_builder) {
        cmd_list = *(sdk::FRHICommandListBase**)((uintptr_t)cmd_list + ue5_command_offset);
    }

    const auto compensation = g_hook->get_frame_delay_compensation();

    // Using slate's draw window hook is the safest way to do this without
    // false positives on the command list in this function
    // otherwise we can attempt to use the command list here and hook it
    // in the slate hook, a guaranteed proper command list is passed to the function
    // so we can use that to hook the command list
    // The main inspiration for this is UE5.0.3 because it passes an FRDGBuilder
    // which *does* contain the command list in it, but for whatever reason I can't
    // seem to hook it properly, so I'm using the slate hook instead
    // ADDENDUM: For now, I'm only using the slate hook for UE5.0.3.
    // But I'll use it as a fallback as well for when the command list appears to be empty
    // Reason being the slate hook doesn't appear to run every frame, so it's not a perfect solution
    auto enqueue_poses_on_slate_thread = [&]() {
        g_hook->get_slate_thread_worker()->enqueue([=](FRHICommandListImmediate* command_list) {
            static bool once_slate = true;

            if (once_slate) {
                SPDLOG_INFO("Called enqueued function on the Slate thread for the first time! Frame count: {}", frame_count);
                once_slate = false;
            }

            static size_t actual_offset = 0;
            auto l = (sdk::FRHICommandListBase*)((uintptr_t)command_list + actual_offset);
            const auto is_ue5 = g_hook->has_double_precision();

            if (l != nullptr && l->root != nullptr && ((uintptr_t)l->root & (sizeof(void*) - 1)) == 0) {
                auto new_root = (sdk::FRHICommandBase_New*)l->root;
                if (!analyzed_root_already) try {
                    // so all of this might seem really overkill but
                    // it's a good way to detect whether we have an FMemStack at the top of the command list
                    // which we need to skip on UE5.5+
                    if (utility::get_module_within(*(void**)l->root).value_or(nullptr) == nullptr || 
                        IsBadReadPtr(*(void**)l->root, sizeof(void*)) || 
                        utility::get_module_within(**(void***)l->root).value_or(nullptr) == nullptr ||
                        (!IsBadReadPtr(new_root->next, sizeof(void*)) && (utility::get_module_within(*(void**)new_root->next).value_or(nullptr) == nullptr || utility::get_module_within(**(void***)new_root->next).value_or(nullptr) == nullptr))
                    )
                {
                        if (is_ue5) {
                            // UE5 is NOT an old command list, we need to bruteforce the offset
                            // Start at 0x10 because that's usually where the pointers in FMemStack end.
                            for (size_t i = 0x10; i < 0x50; i += sizeof(void*)) try {
                                const auto cur_l = (sdk::FRHICommandListBase*)((uintptr_t)command_list + i);
                                if (utility::get_module_within(*(void**)cur_l->root).value_or(nullptr) != nullptr) {
                                    actual_offset = i;
                                    l = cur_l;
                                    SPDLOG_INFO("Found UE5.5+ command list at offset 0x{:x}", i);
                                    break;
                                }
                            } catch(...) {

                            }
                        } else {
                            SPDLOG_INFO("Old FRHICommandBase detected");
                            is_old_command_base = true;
                        }
                    } else {
                        SPDLOG_INFO("New FRHICommandBase detected");
                    }

                    analyzed_root_already = true;
                } catch(...) {
                    SPDLOG_ERROR("Failed to analyze FRHICommandBase");
                    analyzed_root_already = true;
                }

                if (!is_old_command_base) {
                    SceneViewExtensionAnalyzer::hook_new_rhi_command((sdk::FRHICommandBase_New*)l->root, frame_count + compensation);
                } else {
                    SceneViewExtensionAnalyzer::hook_old_rhi_command((sdk::FRHICommandBase_Old*)l->root, frame_count + compensation);
                }
            } else {
                // welp
                vr->get_runtime()->enqueue_render_poses(frame_count + compensation);
            }
        });
    };

    // okay well I think this evaluates to false all the time
    // but apparently it has been working for a LONG TIME so I'm not going to touch this until after release
    // (the else statement still handles everything... fine?)
    const auto has_good_root = 
        cmd_list != nullptr &&
        ((uintptr_t)cmd_list & 1 == 0) &&
        cmd_list->root != nullptr &&
        ((uintptr_t)cmd_list->root & 1 == 0);

    // Hijack the top command in the command list so we can enqueue the render poses on the RHI thread
    if (has_good_root) {
        SPDLOG_INFO_ONCE("Command list root is good");

        if (!analyzed_root_already) try {
            auto root = cmd_list->root;

            auto analyze_for_ue5 = [&]() {
                // Find the real command list.
                is_ue5_rdg_builder = true;
                const auto rdg_builder = (uintptr_t)cmd_list;

                for (auto i = 0x10; i <= 0x100; i += sizeof(void*)) try {
                    const auto value = *(uintptr_t*)(rdg_builder + i);

                    if (value == 0 || IsBadReadPtr((void*)value, sizeof(void*))) {
                        continue;
                    }

                    if (utility::get_module_within((void*)value).has_value()) {
                        continue;
                    }

                    const auto value_deref = *(uintptr_t*)value;

                    if (value_deref == 0 || IsBadReadPtr((void*)value_deref, sizeof(void*))) {
                        continue;
                    }

                    if (utility::get_module_within((void*)value_deref).has_value()) {
                        continue;
                    }

                    const auto root_vtable = *(uintptr_t*)value_deref;

                    if (root_vtable == 0 || IsBadReadPtr((void*)root_vtable, sizeof(void*))) {
                        continue;
                    }

                    if (!utility::get_module_within((void*)root_vtable).has_value()) {
                        continue;
                    }

                    // Check that there is a valid function in the vtable
                    const auto first_function = *(uintptr_t*)root_vtable;

                    if (first_function == 0 || IsBadReadPtr((void*)first_function, sizeof(void*))) {
                        continue;
                    }

                    if (!utility::get_module_within((void*)first_function).has_value()) {
                        continue;
                    }

                    SPDLOG_INFO("Possible UE5 command list found at offset 0x{:x}", i);
                    ue5_command_offset = i;
                    cmd_list = (sdk::FRHICommandListBase*)value;
                    break;
                } catch(...) {
                    spdlog::error("Exception occurred while analyzing UE5 command list");
                }
            };

            // If we read the pointer at the start of the root and it's not a module, then it's the old FRHICommandBase
            // this is because all vtables reside within a module
            if (utility::get_module_within(*(void**)root).value_or(nullptr) == nullptr) {
                // UE5
                if (g_hook->has_double_precision()) {
                    analyze_for_ue5();

                    if (ue5_command_offset == 0) {
                        SPDLOG_ERROR("Failed to find UE5 command list, trying again next frame");
                        return;
                    }
                } else {
                    SPDLOG_INFO("Old FRHICommandBase detected");
                    is_old_command_base = true;
                }
            } else {
                SPDLOG_INFO("New FRHICommandBase detected");
            }

            analyzed_root_already = true;
        } catch(...) {
            SPDLOG_ERROR("Failed to analyze root command");
            analyzed_root_already = true;
        }

        if (g_hook->get_render_target_manager()->is_ue_5_0_3() && g_hook->has_slate_hook()) {
            enqueue_poses_on_slate_thread();
        } else try {
            if (!is_old_command_base) {
                SceneViewExtensionAnalyzer::hook_new_rhi_command((sdk::FRHICommandBase_New*)cmd_list->root, frame_count + compensation);
            } else {
                SceneViewExtensionAnalyzer::hook_old_rhi_command((sdk::FRHICommandBase_Old*)cmd_list->root, frame_count + compensation);
            }
        } catch(...) {
            SPDLOG_INFO_ONCE("Failed to hook command list, falling back to Slate thread hook");

            if (g_hook->has_slate_hook()) {
                enqueue_poses_on_slate_thread();
            } else {
                vr->get_runtime()->enqueue_render_poses(frame_count + compensation);
            }
        }
    } else {
        SPDLOG_INFO_ONCE("Bad root or command list, falling back to Slate thread hook");

        // welp v2
        if (g_hook->has_slate_hook()) {
            enqueue_poses_on_slate_thread();
        } else {
            vr->get_runtime()->enqueue_render_poses(frame_count + compensation);
        }
    }
}

bool FFakeStereoRenderingHook::setup_view_extensions() try {
    SPDLOG_INFO("Attempting to set up view extensions...");

    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to get engine pointer! Cannot set up view extensions!");
        return false;
    }

    const auto active_stereo_device = locate_active_stereo_rendering_device();

    if (!active_stereo_device || !s_stereo_rendering_device_offset) {
        SPDLOG_ERROR("Failed to locate active stereo rendering device!");
        return false;
    }

    // This is a proof of concept at the moment for newer UE versions
    // older versions may not work or crash.
    // TODO: Figure out older versions.
    constexpr auto weak_ptr_size = sizeof(TWeakPtr<void*>);
    static const auto potential_hmd_device_offset = s_stereo_rendering_device_offset + weak_ptr_size;
    static const uintptr_t potential_hmd_device = (uintptr_t)engine + potential_hmd_device_offset;
    static const uintptr_t potential_view_extensions = (uintptr_t)engine + s_stereo_rendering_device_offset + (weak_ptr_size * 2); // 2 to skip over the XRSystem

    // This can happen if the game left a VR plugin in it
    // Usually this isn't an issue, but some games can leave a valid HMDDevice or XRSystem laying around for whatever reason
    // If this isn't cleaned up, the game will crash because it tries to gather view extensions from the existing device
    // and the view extensions it gathered will cause a crash when calling them. also the HMD device itself can cause a crash, it's not actually initialized.
    if (*(void**)potential_hmd_device != nullptr) {
        // Double check that we're actually replacing a pointer and not an integer or something
        if (!IsBadReadPtr(*(void**)potential_hmd_device, sizeof(void*))) {
            SPDLOG_INFO("Found an existing HMDDevice or XRSystem, nullifying it...");
            static std::vector<uintptr_t> replacement_vtable{};

            for (auto i = 0; i < 200; ++i) {
                replacement_vtable.push_back((uintptr_t)+[]() { return nullptr; });
            }

            //**(void***)potential_hmd_device = replacement_vtable.data();
            *(void**)potential_hmd_device = nullptr;
            m_fixed_localplayer_view_count = true; // If this is already allocated, then there's already a second view for us to use
        }

        if (!IsBadReadPtr(*(void**)(potential_hmd_device + sizeof(void*)), sizeof(void*))) {
            *(void**)(potential_hmd_device + sizeof(void*)) = nullptr;
        }
    }

    m_tracking_system_hook = std::make_unique<IXRTrackingSystemHook>(this, potential_hmd_device_offset);
    m_components.push_back(m_tracking_system_hook.get());

    // Add a vectored exception handler that catches attempted dereferences of a null XRSystem or HMDDevice
    // The exception handler will then patch out the instructions causing the crash and continue execution
    AddVectoredExceptionHandler(1, [](PEXCEPTION_POINTERS exception) -> LONG {
        static std::vector<Patch::Ptr> xrsystem_patches{};
        static std::unordered_set<uintptr_t> ignored_addresses{};

        if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            const auto exception_address = exception->ContextRecord->Rip;

            if (ignored_addresses.contains(exception_address)) {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            ignored_addresses.insert(exception_address);

            if (exception_address == 0) {
                SPDLOG_INFO("[Exception Handler] Exception address is null");
                return EXCEPTION_CONTINUE_SEARCH;
            }

            if (IsBadReadPtr((void*)exception_address, sizeof(void*))) {
                SPDLOG_INFO("[Exception Handler] Bad read pointer at {:x}", exception_address);
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const auto decoded = utility::decode_one((uint8_t*)exception_address);

            if (!decoded) {
                SPDLOG_ERROR("[Exception Handler] Failed to decode instruction at {:x}", exception_address);
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const auto& op2 = decoded->Operands[1];

            if (decoded->OperandsCount != 2 || 
                 op2.Type != ND_OP_MEM      || 
                !op2.Info.Memory.HasBase)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            SPDLOG_INFO("Encountered attempted dereference of null pointer at {:x}", exception_address);

            // Get the start of the previous instruction
            const auto previous_instruction = utility::resolve_instruction(exception_address - 1);

            if (!previous_instruction) {
                SPDLOG_ERROR("Could not resolve previous instruction at {:x}", exception_address - 1);
                return EXCEPTION_CONTINUE_SEARCH;
            }

            if (previous_instruction->instrux.Operands[0].Type != ND_OP_REG ||
                previous_instruction->instrux.Operands[0].Info.Register.Reg != op2.Info.Memory.Base)
            {
                SPDLOG_ERROR("Previous instruction does not use the same register as the dereference");
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const auto prev_op2 = previous_instruction->instrux.Operands[1];

            if (previous_instruction->instrux.OperandsCount < 2 ||
                prev_op2.Type != ND_OP_MEM ||
                !prev_op2.Info.Memory.HasBase)
            {
                SPDLOG_ERROR("Previous instruction is not a memory dereference");
                return EXCEPTION_CONTINUE_SEARCH;
            }

            if (!prev_op2.Info.Memory.HasDisp) {
                SPDLOG_ERROR("Previous instruction does not have a displacement");
                return EXCEPTION_CONTINUE_SEARCH;
            }

            if (prev_op2.Info.Memory.Disp != potential_hmd_device_offset) {
                SPDLOG_ERROR("Previous instruction is not the XRSystem or HMDDevice dereference");
                return EXCEPTION_CONTINUE_SEARCH;
            }

            SPDLOG_INFO("Found the dereference of the XRSystem or HMDDevice at {:x}", previous_instruction->addr);

            // Patch the initial instruction that caused the crash
            SPDLOG_INFO("Creating first patch...");

            std::vector<int16_t> first_patch{};

            for (auto i = 0; i < decoded->Length; ++i) {
                first_patch.push_back(0x90);
            }

            xrsystem_patches.push_back(Patch::create(exception_address, first_patch));

            const auto next_instruction_addr = exception_address + decoded->Length;
            const auto next_instruction = utility::decode_one((uint8_t*)next_instruction_addr);

            if (!next_instruction) {
                SPDLOG_ERROR("Could not decode next instruction at {:x}", exception_address + decoded->Length);
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            if (!std::string_view{next_instruction->Mnemonic}.starts_with("CALL")) {
                SPDLOG_ERROR("Next instruction is not a call, continuing anyways since we patched the dereference");
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // Patch the next instruction if it's a call
            SPDLOG_INFO("Creating second patch...");

            std::vector<int16_t> second_patch{};

            for (auto i = 0; i < next_instruction->Length; ++i) {
                second_patch.push_back(0x90);
            }

            xrsystem_patches.push_back(Patch::create(next_instruction_addr, second_patch));

            SPDLOG_INFO("Finished creating patches, continuing execution. Hopefully we don't crash...");
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        return EXCEPTION_CONTINUE_SEARCH;
    });

    // The TWeakPtr version is for >= 4.11 UE versions
    TWeakPtr<FSceneViewExtensions>& view_extensions_tweakptr = 
        *(TWeakPtr<FSceneViewExtensions>*)potential_view_extensions;

    // This means it's an old version of UE
    // so the view extensions are a TArray and not a TWeakPtr<TArray>
    if (!m_rendertarget_manager_embedded_in_stereo_device) {
        if (view_extensions_tweakptr.reference == nullptr) {
            view_extensions_tweakptr.allocate_naive(m_use_fmalloc_scene_view_extensions->value());
        }
    }

    FSceneViewExtensions& view_extensions = m_rendertarget_manager_embedded_in_stereo_device ?  
                                            *(FSceneViewExtensions*)potential_view_extensions : *view_extensions_tweakptr.reference;

    SPDLOG_INFO("Current ext ptr: {:x}", (uintptr_t)view_extensions.extensions.data);
    SPDLOG_INFO("Current ext count: {}", view_extensions.extensions.count);
    SPDLOG_INFO("Current ext capacity: {}", view_extensions.extensions.capacity);

    // Verifications on the current memory of the FSceneViewExtensions, because pre-4.10 (?) the view extensions array did not actually exist
    if (m_rendertarget_manager_embedded_in_stereo_device) {
        SPDLOG_INFO("Performing verifications on the current memory of the FSceneViewExtensions...");

        const auto& current_view_extensions_ptr_value = view_extensions.extensions;

        // Check if current value is non zero and points to invalid memory
        if (current_view_extensions_ptr_value.data != nullptr && IsBadReadPtr((void*)current_view_extensions_ptr_value.data, sizeof(void*))) {
            SPDLOG_ERROR("Usual view extensions pointer is non-zero but points to invalid memory! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }

        // Check if count is greater than capacity, which is not possible
        if ((uint32_t)current_view_extensions_ptr_value.count > (uint32_t)current_view_extensions_ptr_value.capacity) {
            SPDLOG_ERROR("Usual view extensions count is greater than capacity! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }

        // Check if count or capacity is negative, which is not possible
        if ((int32_t)current_view_extensions_ptr_value.count < 0 || (int32_t)current_view_extensions_ptr_value.capacity < 0) {
            SPDLOG_ERROR("Usual view extensions count or capacity is negative! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }
        
        // Check if the memory at count treated as a pointer points to valid memory, which is not possible
        const auto count_as_ptr = *(void**)&current_view_extensions_ptr_value.count;
        if (count_as_ptr != nullptr && !IsBadReadPtr(count_as_ptr, sizeof(void*))) {
            SPDLOG_ERROR("Usual view extensions count is actually a pointer to valid memory! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }

        // Check if the data pointer is null but capacity is greater than 0, which is not possible
        if (current_view_extensions_ptr_value.data == nullptr && current_view_extensions_ptr_value.capacity > 0) {
            SPDLOG_INFO("Usual view extensions data pointer is null but capacity is greater than 0! Cannot set up view extensions!");
            SPDLOG_INFO("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
        }

        // Check if the data pointer is non-null but the capacity is 0, which is not possible
        if (current_view_extensions_ptr_value.data != nullptr && current_view_extensions_ptr_value.capacity == 0) {
            SPDLOG_ERROR("Usual view extensions data pointer is non-null but capacity is 0! Cannot set up view extensions!");
            SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
            return false;
        }

        // Check if any current entries in the array within the count are invalid, which is not possible
        if (current_view_extensions_ptr_value.data != nullptr) {
            for (auto i = 0; i < current_view_extensions_ptr_value.count; ++i) {
                const auto ext = current_view_extensions_ptr_value.data[i].reference;

                if (IsBadReadPtr((void*)ext, sizeof(void*))) {
                    SPDLOG_ERROR("Usual view extensions array contains an invalid entry! Cannot set up view extensions!");
                    SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
                    return false;
                }

                const auto ext_vtable = *(void**)ext;

                if (IsBadReadPtr((void*)ext_vtable, sizeof(void*))) {
                    SPDLOG_ERROR("Usual view extensions array contains an entry with an invalid vtable! Cannot set up view extensions!");
                    SPDLOG_ERROR("This may mean that the UE version is very old and this method of hooking the view extensions is not supported.");
                    return false;
                }
            }
        }
    }

    // Allocate a completely new array if the current one is null or empty
    if (view_extensions.extensions.data == nullptr || view_extensions.extensions.data[0].reference == nullptr || view_extensions.extensions.count == 0) {
        SPDLOG_INFO("Allocating new view extensions array...");

        auto& exts = view_extensions.extensions;

        // Allocate a bunch more than necessary to prevent crashes when the engine tries to add new entries
        const auto new_capacity = 32;

        if (!m_use_fmalloc_scene_view_extensions->value()) {
            exts.data = new TWeakPtr<ISceneViewExtension>[new_capacity]{};
        } else {
            if (auto fmalloc = sdk::FMalloc::get(); fmalloc != nullptr) {
                exts.data = (TWeakPtr<ISceneViewExtension>*)fmalloc->malloc(new_capacity * sizeof(TWeakPtr<ISceneViewExtension>));
                for (auto i = 0; i < new_capacity; ++i) {
                    new (&exts.data[i]) TWeakPtr<ISceneViewExtension>();
                }
            } else {
                SPDLOG_ERROR("Failed to get FMalloc! Cannot allocate new view extensions array! Falling back to default allocation method...");
                exts.data = new TWeakPtr<ISceneViewExtension>[new_capacity]{};
            }
        }

        exts.count = 0;
        exts.capacity = new_capacity;

        ZeroMemory(exts.data, sizeof(TWeakPtr<ISceneViewExtension>) * new_capacity);
        exts.data[exts.count++].allocate_naive(m_use_fmalloc_scene_view_extensions->value());
    } else if (view_extensions.extensions.data != nullptr && view_extensions.extensions.count <= view_extensions.extensions.capacity) {
        auto& exts = view_extensions.extensions;

        // TODO: Use FMemory::Realloc (or whatever its called) instead of new/delete cuz game crashes when reallocating/closing the game
        if (exts.count == exts.capacity) {
            SPDLOG_INFO("Extending view extensions array...");

            const auto new_capacity = exts.capacity * 4;
            const auto old_capacity = exts.capacity;

            TWeakPtr<ISceneViewExtension>* new_exts = nullptr;

            if (!m_use_fmalloc_scene_view_extensions->value()) {
                new_exts = new TWeakPtr<ISceneViewExtension>[new_capacity];
            } else {
                if (auto fmalloc = sdk::FMalloc::get(); fmalloc != nullptr) {
                    new_exts = (TWeakPtr<ISceneViewExtension>*)fmalloc->malloc(new_capacity * sizeof(TWeakPtr<ISceneViewExtension>));
                    for (auto i = 0; i < new_capacity; ++i) {
                        new (&new_exts[i]) TWeakPtr<ISceneViewExtension>();
                    }
                } else {
                    SPDLOG_ERROR("Failed to get FMalloc! Cannot allocate new view extensions array! Falling back to default allocation method...");
                    new_exts = new TWeakPtr<ISceneViewExtension>[new_capacity];
                }
            }

            ZeroMemory(new_exts, sizeof(TWeakPtr<ISceneViewExtension>) * new_capacity);
            memcpy(new_exts, exts.data, sizeof(TWeakPtr<ISceneViewExtension>) * old_capacity);

            // dont delete it cuz its owned by the games allocator... for now
            //delete[] exts.data;

            exts.data = new_exts;
            exts.capacity = new_capacity;
        } else {
            SPDLOG_INFO("Allocating new view extension entry onto existing array...");
        }

        exts.data[exts.count++].allocate_naive(m_use_fmalloc_scene_view_extensions->value());
    } else {
        SPDLOG_INFO("None of the previous conditions were met, so we're not allocating a new view extensions array");
    }

    if (view_extensions.extensions.count > 0 && view_extensions.extensions.data != nullptr) {
        // Replace the vtable of the first entry
        auto& entry = view_extensions.extensions.data[view_extensions.extensions.count-1];

        if (entry.reference == nullptr) {
            SPDLOG_ERROR("Failed to get first view extension entry!");
            return false;
        }

        auto& vtable = *(uintptr_t**)entry.reference;

        g_hook->m_analyze_view_extensions_start_time = std::chrono::high_resolution_clock::now();
        g_hook->m_analyzing_view_extensions = true;

        if (!m_rendertarget_manager_embedded_in_stereo_device) {
            SceneViewExtensionAnalyzer::FillVtable<g_view_extension_vtable.size()-1>::fill(g_view_extension_vtable);
        } else {
            // Skip straight to stage 2.
            SPDLOG_INFO("Skipping view extension stage 1...");
            SceneViewExtensionAnalyzer::FillVtable<g_view_extension_vtable.size()-1>::fill2(g_view_extension_vtable);
        }

        // Will get called when the view extensions are finally hooked.
        RenderThreadWorker::get().enqueue([this]() {
            this->m_analyzing_view_extensions = false;
            this->m_has_view_extensions_installed = true;
        });

        // overwrite the vtable
        vtable = g_view_extension_vtable.data();
        m_has_view_extension_hook = true;
    } else {
        // TODO: Allocate a new one.
        m_has_view_extension_hook = false;

        SPDLOG_INFO("Failed to set up view extensions! (not yet implemented to allocate a new one)");
    }

    return true;
} catch(...) {
    SPDLOG_ERROR("Unknown exception while setting up view extensions!");
    return false;
}

std::optional<uintptr_t> FFakeStereoRenderingHook::locate_fake_stereo_rendering_constructor() {
    static std::optional<uintptr_t> cached_result{};

    if (cached_result) {
        return cached_result;
    }

    const auto engine_dll = sdk::get_ue_module(L"Engine");

    auto fake_stereo_rendering_constructor = utility::find_function_from_string_ref(engine_dll, L"r.StereoEmulationHeight");

    if (!fake_stereo_rendering_constructor) {
        fake_stereo_rendering_constructor = utility::find_function_from_string_ref(engine_dll, L"r.StereoEmulationFOV");

        if (!fake_stereo_rendering_constructor) {
            SPDLOG_ERROR("Failed to find FFakeStereoRendering constructor");
            return std::nullopt;
        }
    }

    if (!fake_stereo_rendering_constructor) {
        SPDLOG_ERROR("Failed to find FFakeStereoRendering constructor");
        return std::nullopt;
    }

    SPDLOG_INFO("FFakeStereoRendering constructor: {:x}", (uintptr_t)*fake_stereo_rendering_constructor);
    cached_result = *fake_stereo_rendering_constructor;

    return *fake_stereo_rendering_constructor;
}

std::optional<uintptr_t> FFakeStereoRenderingHook::locate_fake_stereo_rendering_vtable() {
    static std::optional<uintptr_t> cached_result{};

    if (cached_result) {
        return cached_result;
    }

    if (g_hook->m_manually_constructed) {
        cached_result = *(uintptr_t*)((uintptr_t)sdk::UGameEngine::get() + s_stereo_rendering_device_offset);
        return cached_result;
    }

    const auto fake_stereo_rendering_constructor = locate_fake_stereo_rendering_constructor();

    if (!fake_stereo_rendering_constructor) {
        // If this happened, then that's bad news, the UE version is probably extremely old
        // so we have to use this fallback method.
        SPDLOG_INFO("Failed to locate FFakeStereoRendering constructor, using fallback method");
        const auto initialize_hmd_device = sdk::UEngine::get_initialize_hmd_device_address();

        if (!initialize_hmd_device) {
            SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable via fallback method");
            return std::nullopt;
        }

        // To be seen if this needs to be adjusted. At first glance it doesn't look very reliable.
        // maybe perform emulation or something in the future?
        const auto instruction = utility::scan_disasm(*initialize_hmd_device, 100, "48 8D 05 ? ? ? ?");

        if (!instruction) {
            SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable via fallback method (2)");
            return std::nullopt;
        }

        const auto result = utility::calculate_absolute(*instruction + 3);

        if (!result) {
            SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable via fallback method (3)");
            return std::nullopt;
        }

        SPDLOG_INFO("FFakeStereoRendering VTable: {:x}", (uintptr_t)result);
        cached_result = result;

        return result;
    }

    const auto vtable_ref = utility::scan(*fake_stereo_rendering_constructor, 100, "48 8D 05 ? ? ? ?");

    if (!vtable_ref) {
        SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable Reference");
        return std::nullopt;
    }

    const auto vtable = utility::calculate_absolute(*vtable_ref + 3);

    if (!vtable) {
        SPDLOG_ERROR("Failed to find FFakeStereoRendering VTable");
        return std::nullopt;
    }

    SPDLOG_INFO("FFakeStereoRendering VTable: {:x}", (uintptr_t)vtable);
    cached_result = vtable;

    return vtable;
}

std::optional<uintptr_t> FFakeStereoRenderingHook::locate_active_stereo_rendering_device() {
    auto engine = (uintptr_t)sdk::UEngine::get();

    if (engine == 0) {
        SPDLOG_ERROR("GEngine does not appear to be instantiated, cannot verify stereo rendering device is setup.");
        return std::nullopt;
    }

    SPDLOG_INFO("Checking engine pointers for StereoRenderingDevice...");
    auto fake_stereo_device_vtable = locate_fake_stereo_rendering_vtable();

    if (!fake_stereo_device_vtable) {
        SPDLOG_ERROR("Failed to locate fake stereo rendering device vtable, cannot verify stereo rendering device is setup.");
        return std::nullopt;
    }

    if (s_stereo_rendering_device_offset != 0) {
        const auto result = *(uintptr_t*)(engine + s_stereo_rendering_device_offset);

        if (result == 0) {
            return std::nullopt;
        }

        return result;
    }

    for (auto i = 0; i < 0x2000; i += sizeof(void*)) {
        const auto addr_of_ptr = engine + i;

        if (IsBadReadPtr((void*)addr_of_ptr, sizeof(void*))) {
            SPDLOG_INFO("Reached end of engine pointers at offset {:x}", i);
            break;
        }

        const auto ptr = *(uintptr_t*)addr_of_ptr;

        if (ptr == 0 || IsBadReadPtr((void*)ptr, sizeof(void*))) {
            continue;
        }

        auto potential_vtable = *(uintptr_t*)ptr;

        if (potential_vtable == *fake_stereo_device_vtable) {
            SPDLOG_INFO("Found fake stereo rendering device at offset {:x} -> {:x}", i, ptr);
            s_stereo_rendering_device_offset = i;
            return ptr;
        }
    }

    SPDLOG_ERROR("Failed to find stereo rendering device");
    return std::nullopt;
}

std::optional<uint32_t> FFakeStereoRenderingHook::get_stereo_view_offset_index(uintptr_t vtable) {
    for (auto i = 0; i < 30; ++i) {
        auto func = ((uintptr_t*)vtable)[i];

        if (func == 0 || IsBadReadPtr((void*)func, sizeof(void*))) {
            continue;
        }

        // Resolve jmps if needed.
        while (*(uint8_t*)func == 0xE9) {
            SPDLOG_INFO("VFunc at index {} contains a jmp, resolving...", i);
            func = utility::calculate_absolute(func + 1);
        }

        bool found = false;
        uint32_t xmm_register_usage_count = 0;

        // We do an exhaustive decode (disassemble all possible code paths) that correctly follows the control flow
        // because some games are obfuscated and do huge jumps across gaps of junk code.
        // so we can't just linearly scan forward as the disassembler will fail at some point.
        utility::exhaustive_decode((uint8_t*)func, 50, [&](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
            if (found) {
                return utility::ExhaustionResult::BREAK;
            }

            if (ix.BranchInfo.IsBranch && !ix.BranchInfo.IsConditional && std::string_view{ix.Mnemonic}.starts_with("CALL")) {
                return utility::ExhaustionResult::STEP_OVER;
            }

            char txt[ND_MIN_BUF_SIZE]{};
            NdToText(&ix, 0, sizeof(txt), txt);

            if (std::string_view{txt}.find("xmm") != std::string_view::npos && ++xmm_register_usage_count >= 10) {
                found = true;
                return utility::ExhaustionResult::BREAK;
            }

            return utility::ExhaustionResult::CONTINUE;
        });

        if (found) {
            SPDLOG_INFO("Found Stereo View Offset Index: {}", i);
            return i;
        }
    }

    return std::nullopt;
}

// DISCLAIMER: I've only seen this in one game so far...
// So, there's some kind of compiler optimization for inlined virtuals
// that checks whether the vtable pointer matches the base FFakeStereoRendering class.
// if it matches, it just calls an inlined version of the function.
// otherwise it actually calls the function within the vtable.
bool FFakeStereoRenderingHook::patch_vtable_checks() {
    SPDLOG_INFO("Attempting to patch inlined vtable checks...");

    const auto fake_stereo_rendering_constructor = locate_fake_stereo_rendering_constructor();
    const auto fake_stereo_rendering_vtable = locate_fake_stereo_rendering_vtable();

    if (!fake_stereo_rendering_constructor || !fake_stereo_rendering_vtable) {
        SPDLOG_ERROR("Cannot patch vtables, constructor or vtable not found!");
        return false;
    }

    const auto vtable_module_within = utility::get_module_within(*fake_stereo_rendering_vtable);
    const auto module_size = utility::get_module_size(*vtable_module_within);
    const auto module_end = (uintptr_t)*vtable_module_within + *module_size;

    SPDLOG_INFO("{:x} {:x} {:x}", *fake_stereo_rendering_vtable, (uintptr_t)*vtable_module_within, *module_size);

    for (auto ref = utility::scan_displacement_reference(*vtable_module_within, *fake_stereo_rendering_vtable); 
        ref.has_value();
        ref = utility::scan_displacement_reference((uintptr_t)*ref + 4, (module_end - *ref) - sizeof(void*), *fake_stereo_rendering_vtable)) 
    {
        const auto distance_from_constructor = *ref - *fake_stereo_rendering_constructor;

        // We don't want to mess with the one within the constructor.
        if (distance_from_constructor < 0x100) {
            SPDLOG_INFO("Skipping vtable reference within constructor");
            continue;
        }

        // Change the bytes to be some random number
        // this causes the vtable check to fail and will call the function within the vtable.
        DWORD old{};
        VirtualProtect((void*)*ref, 4, PAGE_EXECUTE_READWRITE, &old);
        *(uint32_t*)*ref = 0x12345678;
        VirtualProtect((void*)*ref, 4, old, &old);
        SPDLOG_INFO("Patched vtable check at {:x}", (uintptr_t)*ref);
    }

    SPDLOG_INFO("Finished patching inlined vtable checks.");
    return true;
}

bool FFakeStereoRenderingHook::attempt_runtime_inject_stereo() {
    // This attempts to create a new StereoRenderingDevice in the GEngine
    // if it doesn't already exist via using -emulatestereo.
    auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("Failed to locate GEngine, cannot inject stereo rendering device at runtime.");
        return false;
    }

    static auto enable_stereo_emulation_cvar = sdk::vr::get_enable_stereo_emulation_cvar();

    if (!locate_active_stereo_rendering_device()) {
        SPDLOG_INFO("Calling InitializeHMDDevice...");

        //utility::ThreadSuspender _{};

        engine->initialize_hmd_device();

        SPDLOG_INFO("Called InitializeHMDDevice.");

        if (!locate_active_stereo_rendering_device()) {
            SPDLOG_INFO("Previous call to InitializeHMDDevice did not setup the stereo rendering device, attempting to call again...");

            auto patch_emulate_stereo_flag = []() {
                //SPDLOG_ERROR("Failed to locate r.EnableStereoEmulation cvar, next call may fail.");
                SPDLOG_INFO("r.EnableStereoEmulation cvar not found, using fallback method of forcing -emulatestereo flag.");
                
                const auto emulate_stereo_string_ref = sdk::UGameEngine::get_emulatestereo_string_ref_address();

                if (emulate_stereo_string_ref) {
                    const auto resolved = utility::resolve_instruction(*emulate_stereo_string_ref);

                    if (resolved) {
                        // Scan forward for a call instruction, this call checks the command line for "emulatestereo".
                        const auto call = utility::scan_disasm(resolved->addr, 20, "E8 ? ? ? ?");

                        if (call) {
                            // Patch the instruction to mov al, 1
                            SPDLOG_INFO("Patching instruction at {:x} to mov al, 1", (uintptr_t)*call);
                            static auto patch = Patch::create(*call, { 0xB0, 0x01, 0x90, 0x90, 0x90 });
                        }
                    }
                }
            };

            // We don't call this before because the cvar will not be set up
            // until it's referenced once. after we set this we need to call the function again.
            if (enable_stereo_emulation_cvar) {
                try {
                    enable_stereo_emulation_cvar->set<int>(1);
                } catch(...) {
                    SPDLOG_ERROR("Access violation occurred when writing to r.EnableStereoEmulation, the address may be incorrect!");
                    patch_emulate_stereo_flag();
                }
            } else {
                //SPDLOG_ERROR("Failed to locate r.EnableStereoEmulation cvar, next call may fail.");
                patch_emulate_stereo_flag();
            }

            SPDLOG_INFO("Calling InitializeHMDDevice... AGAIN");

            engine->initialize_hmd_device();

            SPDLOG_INFO("Called InitializeHMDDevice again.");
        }

        if (locate_active_stereo_rendering_device()) {
            SPDLOG_INFO("Stereo rendering device setup successfully.");
        } else {
            SPDLOG_ERROR("Failed to setup stereo rendering device.");
            return false;
        }
    } else {
        SPDLOG_INFO("Not necessary to call InitializeHMDDevice, stereo rendering device is already setup.");
        m_fixed_localplayer_view_count = true; // Everything was set up beforehand, we don't need to do anything, so just set it to true.
    }

    return true;
}

bool FFakeStereoRenderingHook::is_stereo_enabled(FFakeStereoRendering* stereo) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("is stereo enabled called!");
#else
    SPDLOG_INFO_ONCE("is stereo enabled called!");
#endif

    // wait!!!
    if (!g_framework->is_game_data_intialized()) {
        return false;
    }

    if (g_hook->m_sceneview_data.inside_post_init_properties) {
        g_hook->set_should_recreate_textures(true);
        return true;
    }
    
    /*if (g_hook->m_analyzing_view_extensions) {
        const auto now = std::chrono::high_resolution_clock::now();

        if (now - g_hook->m_analyze_view_extensions_start_time > std::chrono::seconds(15)) {
            SPDLOG_INFO("Timed out waiting for view extensions to be analyzed.");
            g_hook->m_analyzing_view_extensions = false;
        }

        return false;
    }*/

    static std::atomic<bool> last_state = false;
    auto hook = g_hook;

    // The best way to enable stereo rendering without causing crashes
    // while also allowing the desktop view to initially display
    // if the HMD is not on at the start. It only allows
    // stereo to be enabled if it starts from the first call to IsStereoEnabled inside UGameViewportClient::Draw.
    if (hook->m_has_game_viewport_client_draw_hook) {
        if (GameThreadWorker::get().is_same_thread()) {
            if (hook->m_in_viewport_client_draw && !hook->m_was_in_viewport_client_draw) {
                const auto is_hmd_active = VR::get()->is_hmd_active();

                if (!last_state && is_hmd_active) {
                    VR::get()->wait_for_present();
                    hook->set_should_recreate_textures(true);
                }

                last_state = is_hmd_active;
            }

            hook->m_was_in_viewport_client_draw = hook->m_in_viewport_client_draw;
        }

        return last_state;
    }

    static uint32_t count = 0;

    // Forcefully return true the first few times to let stuff initialize.
    if (count < 50) {
        if (count == 0) {
            hook->set_should_recreate_textures(true);
        }

        ++count;
        last_state = true;
        return true;
    }

    const auto result = !VR::get()->get_runtime()->got_first_sync || VR::get()->is_hmd_active();

    if (result && !last_state) {
        hook->set_should_recreate_textures(true);
    }

    last_state = result;

    return result;
}

void FFakeStereoRenderingHook::adjust_view_rect(FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("adjust view rect called! {}", index);
    SPDLOG_INFO(" x: {}, y: {}, w: {}, h: {}", *x, *y, *w, *h);
#else
    SPDLOG_INFO_ONCE("adjust view rect called! {}", index);
    SPDLOG_INFO_ONCE(" x: {}, y: {}, w: {}, h: {}", *x, *y, *w, *h);
#endif

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    static bool index_starts_from_one = true;

    if (index == 2) {
        index_starts_from_one = true;
    } else if (index == 0) {
        index_starts_from_one = false;
    }

    // The purpose of this is to prevent the game from crashing in IDirect3D12CommandList::Close
    // Because the game will try to copy a texture region that is out of bounds.
    if (g_hook->m_skip_next_adjust_view_rect) {
        *x = 0;
        *y = 0;
        *w = std::min<uint32_t>(VR::get()->get_hmd_width(), *w);
        *h = std::min<uint32_t>(VR::get()->get_hmd_height(), *h);
        g_hook->m_skip_next_adjust_view_rect = false;
        g_hook->m_skip_next_adjust_view_rect_count = 1;
        return;
    }

    if (g_hook->m_skip_next_adjust_view_rect_count > 0) {
        *x = 0;
        *y = 0;
        *w = std::min<uint32_t>(VR::get()->get_hmd_width(), *w);
        *h = std::min<uint32_t>(VR::get()->get_hmd_height(), *h);
        --g_hook->m_skip_next_adjust_view_rect_count;
        return;
    }

    if (VR::get()->is_stereo_emulation_enabled()) {
        *w *= 2;
    } else {
        *w = VR::get()->get_hmd_width() * 2;
        *h = VR::get()->get_hmd_height();
    }


    *w = *w / 2;

    const auto true_index = index_starts_from_one ? ((index + 1) % 2) : (index % 2);

    if (!VR::get()->is_native_stereo_fix_enabled()) {
        *x += *w * true_index;
    }
}

__forceinline void FFakeStereoRenderingHook::calculate_stereo_view_offset(
    FFakeStereoRendering* stereo, const int32_t view_index, Rotator<float>* view_rotation, 
    const float world_to_meters, Vector3f* view_location)
{
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("calculate stereo view offset called! {}", view_index);
#else
    SPDLOG_INFO_ONCE("calculate stereo view offset called! {}", view_index);
#endif

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    auto vr = VR::get();
    //std::scoped_lock _{vr->get_vr_mutex()};

    static bool index_starts_from_one = true;
    static bool index_was_ever_two = false;
    static bool index_was_ever_negative = false;

    if (view_index == -1) {
        index_was_ever_negative = true;
        SPDLOG_INFO_ONCE("calculate stereo view offset called with view index -1 (INDEX_NONE), ignoring.");
        return;
    }

    // This is eSSP_FULL, we don't care. It will cause the view to become monoscopic if we do anything.
    if (index_was_ever_two && view_index == 0) {
        SPDLOG_INFO_ONCE("calculate stereo view offset called with view index 0 after 2, ignoring.");
        return;
    }

    vr->set_world_to_meters(world_to_meters);

    if (view_index == 2) {
        index_starts_from_one = true;
        index_was_ever_two = true;
    } else if (view_index == 0 && !index_was_ever_two) {
        index_starts_from_one = false;
    }

    const auto is_full_pass = view_index == 0 && !index_was_ever_two && !index_was_ever_negative && !g_hook->m_has_double_precision;

    auto true_index = index_starts_from_one ? ((view_index + 1) % 2) : (view_index % 2);
    const auto has_double_precision = g_hook->m_has_double_precision;
    const auto rot_d = (Rotator<double>*)view_rotation;

    if (vr->is_using_afr() && !is_full_pass) {
        true_index = g_frame_count % 2;

        // Flat3D: alternate the eye per DRAW, not per engine frame number.
        // Some titles (Hogwarts) keep the frame number for the forced synced-
        // sequential draw — raw %2 then renders the SAME eye twice per pair,
        // collapsing the stereo baseline (squished depth). A stalled frame
        // number takes the complement of the previous draw's eye; the
        // projection hook consumes m_afr_draw_index for the same draw.
        if (vr->is_using_flat3d()) {
            int idx = (int)(g_frame_count % 2);
            if (g_hook->m_afr_draw_frame == (int64_t)g_frame_count && g_hook->m_afr_draw_index >= 0) {
                idx = g_hook->m_afr_draw_index ^ 1;
                if (auto* f = vr->get_flat3d_runtime(); f != nullptr) {
                    f->pair_stall_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
            g_hook->m_afr_draw_frame = (int64_t)g_frame_count;
            g_hook->m_afr_draw_index = idx;
            true_index = idx;
        }

        // Plain-AFR rotation latch: freeze one HMD pose across the AFR pair so
        // the eyes don't shear apart under head motion. HMD-only — under
        // Flat3D the rotation IS the game camera: latching makes the right eye
        // lag a frame, and any special pass that enters here on even parity
        // (map/portrait scene captures are typically top-down) poisons the
        // latch and pins the right eye looking at the ground.
        if (!vr->is_using_synchronized_afr() && !vr->is_using_afw() && !vr->is_using_flat3d()) {
            if (g_hook->m_has_double_precision) {
                if (true_index == 1) {
                    *rot_d = g_hook->m_last_afr_rotation_double;
                } else {
                    g_hook->m_last_afr_rotation_double = *rot_d;
                }
            } else {
                if (true_index == 1) {
                    *view_rotation = g_hook->m_last_afr_rotation;
                } else {
                    g_hook->m_last_afr_rotation = *view_rotation;
                }
            }
        }
    }

    // Synced Sequential pair detector (Flat3D): the forced second draw of a
    // pair runs on unticked game state, so its RAW camera (view location +
    // rotation as passed in, before any eye offset / OpenTrack mutation) is
    // bit-identical to the first draw's. Push one match record per eye draw;
    // the present path consumes them FIFO to publish complete same-state
    // pairs (build_flat3d_frame_params). No de-dup and no other heuristics:
    // when this signal is absent or ambiguous the consumer falls back to
    // NAIVE per-present publishing — a wrongly-aligned pair lock shows a
    // cross-state pair on EVERY present, which is strictly worse than naive
    // (matched pair every other present).
    if (!is_full_pass && vr->is_using_flat3d() && vr->is_using_synchronized_afr()) {
        if (auto* f = vr->get_flat3d_runtime(); f != nullptr) {
            uint8_t sig[48]{};
            const size_t comp = has_double_precision ? sizeof(double) : sizeof(float);
            memcpy(sig, view_location, comp * 3);
            memcpy(sig + comp * 3, view_rotation, comp * 3);
            const size_t len = comp * 6;
            const bool match = f->prev_cam_sig_len == len && memcmp(f->prev_cam_sig, sig, len) == 0;
            memcpy(f->prev_cam_sig, sig, len);
            f->prev_cam_sig_len = len;

            std::scoped_lock _{f->pair_mtx};
            f->pair_push_count++;
            if (match) {
                f->pair_match_count++;
            }
            const uint8_t flags = (match ? 1u : 0u) |
                ((g_hook->m_afr_draw_index >= 0 ? ((uint8_t)g_hook->m_afr_draw_index & 1u) : 0u) << 1);
            f->pair_second_fifo.push_back({(uint32_t)g_frame_count, flags});
            if (f->pair_second_fifo.size() > 8) {
                // Draw->present correspondence lost (draws outpacing presents):
                // records no longer describe the frames being presented. Reset
                // to naive rather than consume stale, misaligning values.
                f->pair_second_fifo.clear();
            }
        }
    }

    if ((true_index == 0 || vr->is_using_afw()) && !is_full_pass) {
        if (has_double_precision) {
            g_hook->m_last_pre_rotation_double = *rot_d;
        } else {
            g_hook->m_last_pre_rotation = *view_rotation;
        }

        //vr->wait_for_present();
        
        if (!g_hook->m_has_view_extension_hook && !g_hook->m_has_game_viewport_client_draw_hook) {
            vr->update_hmd_state();
        }
    }

    /*if (view_index % 2 == 1 && VR::get()->get_synchronize_stage() == VR::SynchronizeStage::EARLY) {
        std::scoped_lock _{ vr->get_runtime()->render_mtx };
        SPDLOG_INFO("SYNCING!!!");
        //vr->get_runtime()->synchronize_frame();
        vr->update_hmd_state();
    }*/

    // if we were unable to hook UGameEngine::Tick, we can run our game thread jobs here instead.
    if (!is_full_pass && !g_hook->m_has_view_extension_hook && g_hook->m_attempted_hook_game_engine_tick && !g_hook->m_hooked_game_engine_tick) {
        GameThreadWorker::get().execute();
    }

    if (vr->is_sceneview_compatibility_enabled() && !g_hook->m_inside_manual_view_offset) {
        return;
    }

    const auto& mods = g_framework->get_mods()->get_mods();

    if (!is_full_pass) {
        for (auto& mod : mods) {
            mod->on_early_calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location, g_hook->m_has_double_precision);
        }

        for (auto& mod : mods) {
            mod->on_pre_calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location, g_hook->m_has_double_precision);
        }
    }

    const auto view_d = (Vector3d*)view_location;

    // world to view
    const auto view_mat = !has_double_precision ? 
        glm::yawPitchRoll(
            glm::radians(view_rotation->yaw),
            glm::radians(view_rotation->pitch),
            glm::radians(view_rotation->roll)) : 
        glm::yawPitchRoll(
            glm::radians((float)rot_d->yaw),
            glm::radians((float)rot_d->pitch),
            glm::radians((float)rot_d->roll));

    // view to world
    const auto view_mat_inverse = !has_double_precision ? 
        glm::yawPitchRoll(
            glm::radians(-view_rotation->yaw),
            glm::radians(view_rotation->pitch),
            glm::radians(-view_rotation->roll)) : 
        glm::yawPitchRoll(
            glm::radians(-(float)rot_d->yaw),
            glm::radians((float)rot_d->pitch),
            glm::radians(-(float)rot_d->roll));

    const auto view_quat_inverse = glm::quat {
        view_mat_inverse
    };

    const auto view_quat = glm::quat {
        view_mat
    };

    const auto quat_converter = glm::quat{Matrix4x4f {
        0, 0, -1, 0,
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 0, 1
    }};

    auto vqi_norm = glm::normalize(view_quat_inverse);

    // Decoupled Pitch
    if (vr->is_decoupled_pitch_enabled()) {
        vr->set_pre_flattened_rotation(vqi_norm);
        vqi_norm = utility::math::flatten(vqi_norm);
    }

    const auto camera_forward_offset = vr->get_camera_forward_offset();
    const auto camera_right_offset = vr->get_camera_right_offset();
    const auto camera_up_offset = vr->get_camera_up_offset();
    const auto camera_forward = quat_converter * (vqi_norm * glm::vec3{0, 0, camera_forward_offset});
    const auto camera_right = quat_converter * (vqi_norm * glm::vec3{-camera_right_offset, 0, 0});
    const auto camera_up = quat_converter * (vqi_norm * glm::vec3{0, -camera_up_offset, 0});

    const auto world_scale = world_to_meters * vr->get_world_scale();

    if (has_double_precision) {
        *view_d += camera_forward;
        *view_d += camera_right;
        *view_d += camera_up;
    } else {
        *view_location += camera_forward;
        *view_location += camera_right;
        *view_location += camera_up;
    }

    // Flat 3D monitor mode reuses the 2D-screen neutralization: skip head
    // translation and rotation overwrite (no HMD), keep eye separation.
    const auto is_2d_screen = vr->is_using_2d_screen() || vr->is_using_flat3d();

    const auto rotation_offset = vr->get_rotation_offset();
    const auto current_hmd_rotation = glm::normalize(rotation_offset * glm::quat{vr->get_rotation(0)});
    const auto current_eye_rotation_offset = glm::normalize(glm::quat{vr->get_eye_transform(true_index)});
    const auto other_eye_rotation_offset = glm::normalize(glm::quat{vr->get_eye_transform((true_index + 1) % 2)});

    const auto new_rotation = glm::normalize(vqi_norm * current_hmd_rotation * current_eye_rotation_offset);
    const auto new_rotation_other = glm::normalize(vqi_norm * current_hmd_rotation * other_eye_rotation_offset);
    const auto eye_offset = glm::vec3{vr->get_eye_offset((VRRuntime::Eye)(true_index))};
    const auto eye_offset_other = glm::vec3{vr->get_eye_offset((VRRuntime::Eye)((true_index + 1) % 2))};

    const auto standing_delta = vr->get_position(0) - vr->get_standing_origin();
    const auto standing_delta_flat = glm::vec3{standing_delta.x, 0, standing_delta.z};

    const auto pos = glm::vec3{rotation_offset * standing_delta};
    const auto pos_flat = glm::vec3{rotation_offset * standing_delta_flat};

    const auto head_offset = quat_converter * (vqi_norm * (pos * world_scale));
    const auto head_offset_flat = quat_converter * (vqi_norm * (pos_flat * world_scale));
    const auto eye_separation = quat_converter * (glm::normalize(new_rotation) * (eye_offset * world_scale));
    const auto eye_separation_other = quat_converter * (glm::normalize(new_rotation_other) * (eye_offset_other * world_scale));

    // Don't apply any headset transformations
    // if we have stereo emulation mode enabled
    // it is only for debugging purposes
    if (!vr->is_stereo_emulation_enabled()) {
        if (!has_double_precision) {
            if (!is_2d_screen) {
                *view_location -= head_offset;
            }

            *view_location -= eye_separation;
        } else {
            if (!is_2d_screen) {
                *view_d -= head_offset;
            }

            *view_d -= eye_separation;
        }

        if (!is_2d_screen) {
            const auto euler = glm::degrees(utility::math::euler_angles_from_steamvr(new_rotation));

            if (!has_double_precision) {
                view_rotation->pitch = euler.x;
                view_rotation->yaw = euler.y;
                view_rotation->roll = euler.z;
            } else {
                rot_d->pitch = euler.x;
                rot_d->yaw = euler.y;
                rot_d->roll = euler.z;
            }
        }

        // Flat 3D + OpenTrack head tracking (v2): additive TrackIR-style look
        // (head rotation added on top of the game's own camera) and
        // head-coupled parallax (camera-local translation). Pure no-op when
        // OpenTrack is off — the game keeps its rotation and only the fixed
        // eye separation applies (as above).
        if (vr->is_using_flat3d()) {
            auto* f = vr->get_flat3d_runtime();

            if (f != nullptr && f->opentrack_active.load()) {
                const float rs = vr->get_flat3d_opentrack_rot_scale();
                const float ps = vr->get_flat3d_opentrack_pos_scale();

                // Read the current (game) view yaw to build a camera-local
                // basis for the positional shift.
                const float base_yaw_deg = has_double_precision ? (float)rot_d->yaw : view_rotation->yaw;
                const float add_pitch = glm::degrees(f->head_pitch.load()) * rs;
                const float add_yaw   = glm::degrees(f->head_yaw.load())   * rs;
                const float add_roll  = glm::degrees(f->head_roll.load())  * rs;

                if (!has_double_precision) {
                    view_rotation->pitch += add_pitch;
                    view_rotation->yaw   += add_yaw;
                    view_rotation->roll  += add_roll;
                } else {
                    rot_d->pitch += add_pitch;
                    rot_d->yaw   += add_yaw;
                    rot_d->roll  += add_roll;
                }

                // Positional parallax: translate the eye origin by head x/y/z
                // in the game camera's local frame (yaw-only basis, UE X=fwd,
                // Y=right, Z=up), scaled to world units.
                const float yaw_rad = glm::radians(base_yaw_deg);
                const float cy = std::cos(yaw_rad);
                const float sy = std::sin(yaw_rad);
                const float hx = f->head_x.load() * ps * world_scale; // right
                const float hy = f->head_y.load() * ps * world_scale; // up
                const float hz = f->head_z.load() * ps * world_scale; // forward

                // forward = (cy, sy, 0), right = (sy, -cy, 0), up = (0,0,1)
                const glm::vec3 head_shift{
                    hz * cy + hx * sy,
                    hz * sy - hx * cy,
                    hy};

                if (!has_double_precision) {
                    *view_location -= head_shift;
                } else {
                    *view_d -= head_shift;
                }
            }
        }

        // Roomscale movement
        // only do it on the right eye pass
        // if we did it on the left, there would be eye desyncs when the right eye is rendered
        if ((true_index == 1 || vr->is_using_afw()) && (vr->is_roomscale_enabled() || vr->is_aim_pawn_control_rotation_enabled())) {
            const auto world = sdk::UEngine::get()->get_world();

            if (const auto controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0); controller != nullptr) {
                const auto pawn = controller->get_acknowledged_pawn();

                static bool was_pawn_rotation_enabled = false;

                if (pawn != nullptr && vr->is_aim_pawn_control_rotation_enabled()) {
                    auto camera_component = (sdk::UObject*)pawn->get_camera_component();

                    if (camera_component != nullptr && camera_component->get_class() != nullptr) {
                        static const auto boolprop = (sdk::FBoolProperty*)camera_component->get_class()->find_property(L"bUsePawnControlRotation");

                        if (boolprop != nullptr) {
                            boolprop->set_value_in_object(camera_component, true);
                            was_pawn_rotation_enabled = true;
                        }
                    }
                } else if (pawn != nullptr && was_pawn_rotation_enabled) {
                    auto camera_component = (sdk::UObject*)pawn->get_camera_component();

                    if (camera_component != nullptr && camera_component->get_class() != nullptr) {
                        static const auto boolprop = (sdk::FBoolProperty*)camera_component->get_class()->find_property(L"bUsePawnControlRotation");

                        if (boolprop != nullptr) {
                            boolprop->set_value_in_object(camera_component, false);
                            was_pawn_rotation_enabled = false;
                        }
                    }
                }

                if (pawn != nullptr && vr->is_roomscale_enabled()) {
                    const auto pawn_pos = pawn->get_actor_location();
                    const auto new_pos = pawn_pos - head_offset_flat;

                    // Roomscale sweep option allows the actor to affect the world
                    // like push doors open, and prevent them from clipping through walls
                    pawn->set_actor_location(new_pos, vr->is_roomscale_sweep_enabled(), false);

                    // Recenter the standing origin
                    auto current_standing_origin = vr->get_standing_origin();
                    const auto hmd_pos = vr->get_position(0);
                    // dont touch the Y axis
                    current_standing_origin.x = hmd_pos.x;
                    current_standing_origin.z = hmd_pos.z;
                    vr->set_standing_origin(current_standing_origin);
                }
            }
        }

        // Process snapturn    
        vr->process_snapturn();
    }

    if (!is_full_pass) {
        for (auto& mod : mods) {
            mod->on_post_calculate_stereo_view_offset(stereo, view_index, view_rotation, world_to_meters, view_location, g_hook->m_has_double_precision);
        }

        if (true_index == 0 || vr->is_using_afw()) {
            if (has_double_precision) {
                g_hook->m_last_rotation_double = *rot_d;
            } else {
                g_hook->m_last_rotation = *view_rotation;
            }
        }

        // Modify Player Control Rotation
        if ((true_index == 1 || vr->is_using_afw()) && vr->is_aim_modify_player_control_rotation_enabled() && vr->is_any_aim_method_active()) {
            if (g_hook->m_tracking_system_hook != nullptr) {
                g_hook->m_tracking_system_hook->manual_update_control_rotation();
            }
        }

        auto view_location_other = has_double_precision ? Vector3f() : *view_location;
        auto view_d_other = has_double_precision ? *view_d : Vector3d();

        if (has_double_precision) {
            view_d_other += eye_separation;
            view_d_other -= eye_separation_other;
        } else {
            view_location_other += eye_separation;
            view_location_other -= eye_separation_other;
        }

        auto view_rotation_other = has_double_precision ? Rotator<float>() : *view_rotation;
        auto rot_d_other = has_double_precision ? *rot_d : Rotator<double>();

        if (!is_2d_screen) {
            const auto euler = glm::degrees(utility::math::euler_angles_from_steamvr(new_rotation_other));

            if (!has_double_precision) {
                view_rotation_other.pitch = euler.x;
                view_rotation_other.yaw = euler.y;
                view_rotation_other.roll = euler.z;
            } else {
                rot_d_other.pitch = euler.x;
                rot_d_other.yaw = euler.y;
                rot_d_other.roll = euler.z;
            }
        }

        const auto view_to_world = !has_double_precision ? 
            glm::yawPitchRoll(
                glm::radians(-view_rotation->yaw),
                glm::radians(view_rotation->pitch),
                glm::radians(-view_rotation->roll)) : 
            glm::yawPitchRoll(
                glm::radians(-(float)rot_d->yaw),
                glm::radians((float)rot_d->pitch),
                glm::radians(-(float)rot_d->roll));
        const auto view_to_world_other = !has_double_precision ? 
            glm::yawPitchRoll(
                glm::radians(-view_rotation_other.yaw),
                glm::radians(view_rotation_other.pitch),
                glm::radians(-view_rotation_other.roll)) : 
            glm::yawPitchRoll(
                glm::radians(-(float)rot_d_other.yaw),
                glm::radians((float)rot_d_other.pitch),
                glm::radians(-(float)rot_d_other.roll));
        glm::vec3 cam_pos;
        glm::vec3 cam_pos_other;
        if (!has_double_precision) {
            cam_pos = (*view_location) / vr->get_world_to_meters();
            cam_pos_other = view_location_other / vr->get_world_to_meters();
            cam_pos = glm::vec3(cam_pos.y, cam_pos.z, -cam_pos.x);
            cam_pos_other = glm::vec3(cam_pos_other.y, cam_pos_other.z, -cam_pos_other.x);
        } else {
            glm::dvec3 cam_pos_d = (*view_d) / double(vr->get_world_to_meters());
            glm::dvec3 cam_pos_d_other = view_d_other / double(vr->get_world_to_meters());
            cam_pos = glm::vec3(cam_pos_d.y, cam_pos_d.z, -cam_pos_d.x);
            cam_pos_other = glm::vec3(cam_pos_d_other.y, cam_pos_d_other.z, -cam_pos_d_other.x);
        }

        auto distance = glm::distance(vr->view_matrix_origin_offset, cam_pos);
        if (distance > 100.0) {
            vr->view_matrix_origin_offset = cam_pos;
        }

        cam_pos -= vr->view_matrix_origin_offset;
        cam_pos_other -= vr->view_matrix_origin_offset;

        glm::mat4 view_inv_matrix = glm::translate(glm::mat4(1.0f), cam_pos) * view_to_world;
        glm::mat4 view_inv_matrix_other = glm::translate(glm::mat4(1.0f), cam_pos_other) * view_to_world_other;
        vr->render_view_inv_matrix[true_index][2] = vr->render_view_inv_matrix[true_index][1];
        vr->render_view_inv_matrix[true_index][1] = vr->render_view_inv_matrix[true_index][0];
        vr->render_view_inv_matrix[true_index][0].curr = view_inv_matrix;
        vr->render_view_inv_matrix[true_index][0].other = view_inv_matrix_other;
        vr->last_update_matrix_frame_count[true_index] = g_frame_count;
    }

#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("Finished calculating stereo view offset!");
#else
    SPDLOG_INFO_ONCE("Finished calculating stereo view offset!");
#endif
}

__forceinline Matrix4x4f* FFakeStereoRenderingHook::calculate_stereo_projection_matrix(FFakeStereoRendering* stereo, Matrix4x4f* out, const int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("calculate stereo projection matrix called! {} from {:x}", view_index, (uintptr_t)_ReturnAddress() - (uintptr_t)utility::get_module_within((uintptr_t)_ReturnAddress()).value_or(nullptr));
#else
    SPDLOG_INFO_ONCE("calculate stereo projection matrix called! {} from {:x}", view_index, (uintptr_t)_ReturnAddress() - (uintptr_t)utility::get_module_within((uintptr_t)_ReturnAddress()).value_or(nullptr));
#endif

    auto& vr = VR::get();

    // Only call PostInitProperties if ghosting fix enabled or native stereo is being used.
    // Also, if we don't have a hook on GetDesiredNumberOfViews, we need to call PostInitProperties
    //if (!vr->is_using_afr() || vr->is_ghosting_fix_enabled() || !g_hook->m_get_desired_number_of_views_hook) {
    if (!vr->should_skip_post_init_properties()) {
        if (!g_hook->m_fixed_localplayer_view_count) {
            if (!g_hook->m_calculate_stereo_projection_matrix_post_hook) {
                const auto return_address = (uintptr_t)_ReturnAddress();
                SPDLOG_INFO("Inserting midhook after CalculateStereoProjectionMatrix... @ {:x}", return_address);

                constexpr auto max_stack_depth = 100;
                uintptr_t stack[max_stack_depth]{};

                const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);

                for (int i = 0; i < depth; i++) {
                    g_hook->m_projection_matrix_stack.push_back(stack[i]);
                    SPDLOG_INFO(" {:x}", (uintptr_t)stack[i]);
                }

                g_hook->m_calculate_stereo_projection_matrix_post_hook = safetyhook::create_mid((void*)return_address, &FFakeStereoRenderingHook::post_calculate_stereo_projection_matrix);

                if (!g_hook->m_calculate_stereo_projection_matrix_post_hook) {
                    SPDLOG_ERROR("Failed to insert midhook after CalculateStereoProjectionMatrix!");
                }
            }
        } else if (g_hook->m_calculate_stereo_projection_matrix_post_hook) {
            SPDLOG_INFO("Removing midhook after CalculateStereoProjectionMatrix, job is done...");
            g_hook->m_calculate_stereo_projection_matrix_post_hook = {};
            g_hook->m_get_projection_data_pre_hook = {};
        }   
    }

    if (!g_framework->is_game_data_intialized()) {
        if (g_hook->m_calculate_stereo_projection_matrix_hook) {
            return g_hook->m_calculate_stereo_projection_matrix_hook.call<Matrix4x4f*>(stereo, out, view_index);
        }

        return out;
    }

    static bool index_starts_from_one = true;
    static bool index_was_ever_two = false;

    // This is eSSP_FULL, we don't care. It will cause the view to become monoscopic if we do anything.
    // or maybe we should, this could be used for WorldToScreen.
    /*if (index_was_ever_two && view_index == 0) {
        SPDLOG_INFO_ONCE("Index was ever two, and now it's zero. This is eSSP_FULL, we don't care. It will cause the view to become monoscopic if we do anything.");
        return out;
    }*/

    if (view_index == 2) {
        index_starts_from_one = true;
        index_was_ever_two = true;
    } else if (view_index == 0) {
        index_starts_from_one = false;
    }

    // Can happen if we hooked this differently.
    if (g_hook->m_calculate_stereo_projection_matrix_hook) {
        g_hook->m_calculate_stereo_projection_matrix_hook.call<Matrix4x4f*>(stereo, out, view_index);
    } else {
        if (g_hook->m_has_double_precision) {
            (*out)[3][2] = sdk::globals::get_near_clipping_plane();
        } else {
            (*(Matrix4x4d*)out)[3][2] = (double)sdk::globals::get_near_clipping_plane();
        }
    }

    // Flat 3D monitor mode: game-FoV perspective + off-axis shear at [2][0].
    // Shear delta = dir * (sep/2 / convergence) * P00 — the classic
    // parallel-cameras + asymmetric-frustum method (VRto3D / perfect_dark_3D):
    // geometry at z == convergence lands at zero disparity, nearer pops out.
    //
    // The matrix produced by the "original" call above is NOT the game
    // camera's projection — the hooked function is UE's built-in
    // FFakeStereoRenderingDevice (hardcoded ~126° FoV, 640x480 aspect), and
    // when no direct hook exists `out` is untouched garbage. So the game's
    // real FoV is sampled from APlayerCameraManager (game thread) and the
    // projection is rebuilt with the engine's own reversed-Z infinite-far
    // construction (same shape as the 2d-screen path below).
    if (vr->is_using_flat3d() && out != nullptr) {
        auto true_index = index_starts_from_one ? ((view_index + 1) % 2) : (view_index % 2);

        if (vr->is_using_afr()) {
            // Use the per-draw eye the view-offset hook chose (it alternates
            // even when the engine keeps its frame number for the forced
            // synced draw — see m_afr_draw_index). Raw %2 here would put the
            // same shear on both halves of such a pair.
            true_index = g_hook->m_afr_draw_index >= 0 ? g_hook->m_afr_draw_index : (int)(g_frame_count % 2);
        }

        auto* flat3d = vr->get_flat3d_runtime();

        // Once per frame (this is called once per eye).
        static uint32_t fov_sample_frame = 0xFFFFFFFF;
        if (fov_sample_frame != (uint32_t)g_frame_count) {
            fov_sample_frame = (uint32_t)g_frame_count;
            flat3d->game_fov_deg.store(vr->sample_flat3d_game_fov(flat3d->game_fov_deg.load()));
            flat3d->game_fov_is_vertical.store(vr->sample_flat3d_fov_is_vertical(flat3d->game_fov_is_vertical.load()));
            flat3d->game_wants_cursor.store(vr->sample_flat3d_show_cursor(flat3d->game_wants_cursor.load()));
            flat3d->game_paused.store(vr->sample_flat3d_game_paused(flat3d->game_paused.load()));
            vr->sample_flat3d_camera_and_publish_anchors();
        }

        const float half_fov = glm::radians(flat3d->game_fov_deg.load()) * 0.5f;
        const float rt_w = (float)flat3d->get_width();
        const float rt_h = (float)flat3d->get_height();
        const float aspect = rt_h > 0.0f && rt_w > 0.0f ? (rt_w / rt_h) : (16.0f / 9.0f);

        float tan_half_h = glm::tan(half_fov);

        // The sampled angle may be the VERTICAL FoV (UE's MaintainYFOV / portrait
        // constraint — see VR::sample_flat3d_fov_is_vertical). Widen it to the
        // horizontal one through the per-eye aspect, which reproduces UE's own
        // XAxisMultiplier = H/W exactly: xs = (H/W)/tan == 1/(tan * aspect).
        // Without this the frustum is far too narrow and the scene renders
        // heavily zoomed in.
        if (flat3d->game_fov_is_vertical.load()) {
            tan_half_h *= aspect;
        }

        // User FoV scale (see VR::flat3d_fov_multiplier). Applied ON TOP of the
        // game's live FoV sampled above, so the game keeps driving the camera
        // (ADS zoom, cine cameras) and this only widens/narrows it. >1 widens the
        // frustum, which zooms the rendered scene OUT. Logged on change only —
        // this is a normal setting, not a per-frame event.
        if (const float fov_mult = vr->flat3d_fov_multiplier(); fov_mult != 1.0f) {
            tan_half_h *= fov_mult;

            static float last_logged_fov_mult = 1.0f;

            if (fov_mult != last_logged_fov_mult) {
                last_logged_fov_mult = fov_mult;
                SPDLOG_INFO("[Flat3D][fov] FoV multiplier {:.3f}: hfov {:.2f} -> {:.2f} deg",
                    fov_mult, glm::degrees(2.0f * half_fov), glm::degrees(2.0f * std::atan(tan_half_h)));
            }
        }

        // Our-side near-plane resolver (SDK dummy -> r.SetNearClipPlane / UE
        // default) so the UESDK submodule stays pristine. See VR::flat3d_effective_nearz.
        const float near_z = vr->flat3d_effective_nearz();

        // Effective separation must match the eye translation the view path
        // applies (eyes[] x world_to_meters x world_scale); convergence stays
        // in game meters, so only world_scale enters the ratio.
        const float sep = flat3d->separation_m.load() * vr->get_world_scale(); // meters
        const float conv = std::max(flat3d->convergence_m.load(), 0.001f);     // meters
        const float o = sep * 0.5f / conv; // frustum shear offset (tangent units)

        // Symmetric-projection compat — driven by the same Compatibility page
        // setting the HMD paths use (Horizontal Projection = Symmetrical in
        // OpenVR/OpenXR.cpp: widen the tangents to a symmetric superset,
        // compensate by cropping — here the compositor crops via the scene
        // shift+scale instead of submit view_bounds). The flat3d frustum is
        // already vertically symmetric and horizontally mirrored between the
        // eyes, so the Vertical override and Mirrored are inherently no-ops.
        const bool symmetric = vr->get_horizontal_projection_override() == VR::HORIZONTAL_PROJECTION_OVERRIDE::HORIZONTAL_SYMMETRIC;

        // Horizontal scale: original tangent, widened by |o| when symmetric.
        const float xs_tan = symmetric ? (tan_half_h + o) : tan_half_h;
        const float xs = xs_tan > 0.0f ? (1.0f / xs_tan) : 1.0f;
        // Vertical FoV is unchanged in either mode.
        const float ys = (tan_half_h > 0.0f ? (1.0f / tan_half_h) : 1.0f) * aspect;

        // Sign: in UE's projection convention (z forward, w = z) the LEFT eye
        // shear is NEGATIVE. Proof via the HMD chain this mode mirrors:
        // VRto3D returns left-eye tangents {l = -t+o, r = t+o} with
        // o = +sep/2/conv, and OpenVR.cpp's get_mat maps raw tangents into
        // [2][0] = (l'+r')/(l'-r') with l' = -l, r' = -r, giving -o*P00.
        const float dir = (true_index == 0) ? -1.0f : 1.0f;                    // left -, right +
        const float shear = symmetric ? 0.0f : dir * o * xs;

        vr->m_nearz = near_z;
        flat3d->update_matrices(near_z, 10000.0f);

        // AFW warp camera data: this branch returns before the HMD path's
        // render_projection_matrix write below, which used to leave the matrix
        // zero — update_camera_data() then fed the frame-warp plugin garbage
        // view->clip matrices under Flat3D. Mirror that write here with the
        // same conversion (UE z-forward/w=z clip -> RH w=-z, near plane scaled
        // to meters via -1/world_to_meters, off-center terms sign-flipped).
        {
            const auto wtm = vr->get_world_to_meters();
            const float near_m = wtm != 0.0f ? (-near_z / wtm) : -near_z;
            Matrix4x4f warp_proj{
                xs, 0.0f, 0.0f, 0.0f,
                0.0f, ys, 0.0f, 0.0f,
                -shear, 0.0f, -1.0f, -1.0f,
                0.0f, 0.0f, near_m, 0.0f
            };
            vr->render_projection_matrix[true_index].curr = warp_proj;
            warp_proj[2][0] = shear; // the other eye mirrors the shear
            vr->render_projection_matrix[true_index].other = warp_proj;
        }

        if (!g_hook->m_has_double_precision) {
            *out = Matrix4x4f {
                xs, 0.0f, 0.0f, 0.0f,
                0.0f, ys, 0.0f, 0.0f,
                shear, 0.0f, 0.0f, 1.0f,
                0.0f, 0.0f, near_z, 0.0f
            };

            flat3d->set_game_projection((uint32_t)true_index, *out, tan_half_h, 1.0f / ys, near_z);
        } else {
            auto& dm = *(Matrix4x4d*)out;
            dm = Matrix4x4d {
                (double)xs, 0.0, 0.0, 0.0,
                0.0, (double)ys, 0.0, 0.0,
                (double)shear, 0.0, 0.0, 1.0,
                0.0, 0.0, (double)near_z, 0.0
            };

            flat3d->set_game_projection((uint32_t)true_index, Matrix4x4f{dm}, tan_half_h, 1.0f / ys, near_z);
        }

        return out;
    }

    if (medium_one_based_projection_pass && (view_index < 1 || view_index > 2)) {
        return out;
    }

    if (VR::get()->is_using_2d_screen()) {
        float fov = 90.0f; // todo, get from FMinimalViewInfo

        const float width = VR::get()->get_hmd_width();
        const float height = VR::get()->get_hmd_height();
        const float half_fov = glm::radians(fov) / 2.0f;
        const float xs = 1.0f / glm::tan(half_fov);
        const float ys = width / glm::tan(half_fov) / height;
        const float near_z = sdk::globals::get_near_clipping_plane();

        auto true_index = index_starts_from_one ? ((view_index + 1) % 2) : (view_index % 2);
        if (g_hook->m_has_double_precision) {
            (*(Matrix4x4d*)out) = Matrix4x4d {
                xs, 0.0, 0.0, 0.0,
                0.0, ys, 0.0, 0.0,
                0.0, 0.0, 0.0, 1.0,
                0.0, 0.0, near_z, 0.0
            };
            vr->render_projection_matrix[true_index].curr = Matrix4x4f(*(Matrix4x4d*)out);
            vr->render_projection_matrix[true_index].other = Matrix4x4f(*(Matrix4x4d*)out);
        } else {
            *out = Matrix4x4f {
                xs, 0.0f, 0.0f, 0.0f,
                0.0f, ys, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f,
                0.0f, 0.0f, near_z, 0.0f
            };
            vr->render_projection_matrix[true_index].curr = *out;
            vr->render_projection_matrix[true_index].other = *out;
        }

        return out;
    }

    // SPDLOG_INFO("NearZ: {}", old_znear);

    if (out != nullptr) {
        auto true_index = index_starts_from_one ? ((view_index + 1) % 2) : (view_index % 2);
    
        if (vr->is_using_afr()) {
            true_index = g_frame_count % 2;
        }

        auto& double_matrix = *(Matrix4x4d*)out;

        if (!g_hook->m_has_double_precision) {
            float old_znear = (*out)[3][2];
            VR::get()->m_nearz = old_znear;            
            VR::get()->get_runtime()->update_matrices(old_znear, 10000.0f);
        } else {
            double old_znear = (double_matrix)[3][2];
            VR::get()->m_nearz = (float)old_znear;
            VR::get()->get_runtime()->update_matrices((float)old_znear, 10000.0f);
        }

        if (!g_hook->m_has_double_precision) {
            *out = VR::get()->get_projection_matrix((VRRuntime::Eye)(true_index));
        } else {
            const auto fmat = VR::get()->get_projection_matrix((VRRuntime::Eye)(true_index));
            double_matrix = fmat;
        }
        if (true_index >= 0 && true_index <= 1) {
            auto other_index = (true_index + 1) % 2;
            auto world_to_meters = vr->get_world_to_meters();
            vr->render_projection_matrix[true_index].curr = vr->get_projection_matrix((VRRuntime::Eye)(true_index));
            vr->render_projection_matrix[true_index].curr[2][0] *= -1.0f;
            vr->render_projection_matrix[true_index].curr[2][1] *= -1.0f;
            vr->render_projection_matrix[true_index].curr[2][2] = -1.0f;
            vr->render_projection_matrix[true_index].curr[2][3] = -1.0f;
            vr->render_projection_matrix[true_index].curr[3][2] *= -1.0f / world_to_meters;
            vr->render_projection_matrix[true_index].other = vr->get_projection_matrix((VRRuntime::Eye)(other_index));
            vr->render_projection_matrix[true_index].other[2][0] *= -1.0f;
            vr->render_projection_matrix[true_index].other[2][1] *= -1.0f;
            vr->render_projection_matrix[true_index].other[2][2] = -1.0f;
            vr->render_projection_matrix[true_index].other[2][3] = -1.0f;
            vr->render_projection_matrix[true_index].other[3][2] *= -1.0f / world_to_meters;
        }
    } else {
        SPDLOG_ERROR("CalculateStereoProjectionMatrix returned nullptr!");
    }

#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("Finished calculating stereo projection matrix!");
#else
    SPDLOG_INFO_ONCE("Finished calculating stereo projection matrix!");
#endif
    
    return out;
}

__forceinline void FFakeStereoRenderingHook::render_texture_render_thread(FFakeStereoRendering* stereo, FRHICommandListImmediate* rhi_command_list,
    FRHITexture2D* backbuffer, FRHITexture2D* src_texture, double window_size) 
{
    if (!g_framework->is_game_data_intialized()) {
        return;
    }

#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("render texture render thread called!");
#else
    SPDLOG_INFO_ONCE("render texture render thread called!");
#endif


    if (!g_hook->is_slate_hooked() && g_hook->has_attempted_to_hook_slate()) {
        SPDLOG_INFO("Attempting to hook SlateRHIRenderer::DrawWindow_RenderThread using RenderTexture_RenderThread return address...");
        const auto return_address = (uintptr_t)_ReturnAddress();
        SPDLOG_INFO(" Return address: {:x}", return_address);
        g_hook->attempt_hook_slate_thread(return_address);
    }

    g_hook->get_slate_thread_worker()->execute(rhi_command_list);

    /*const auto return_address = (uintptr_t)_ReturnAddress();
    const auto slate_cvar_usage_location = sdk::vr::get_slate_draw_to_vr_render_target_usage_location();

    if (slate_cvar_usage_location) {
        const auto distance_from_usage = (intptr_t)(return_address - *slate_cvar_usage_location);

        if (distance_from_usage <= 0x200) {
            //SPDLOG_INFO("Ret: {:x} Distance: {:x}", return_address, distance_from_usage);

            auto& d3d11_vr = VR::get()->m_d3d11;
            auto& hook = g_framework->get_d3d11_hook();
            auto device = hook->get_device();
            ComPtr<ID3D11DeviceContext> context{};

            device->GetImmediateContext(&context);
            context->CopyResource(d3d11_vr.get_test_tex().Get(), (ID3D11Resource*)src_texture->get_native_resource());
            context->Flush();
        }
    }*/

    //g_hook->m_rtm.set_render_target(src_texture);

    /*if (g_hook->m_rtm.get_scene_target() != src_texture) {
        g_hook->m_rtm.set_render_target(src_texture);
    }*/

    // SPDLOG_INFO("{:x}", (uintptr_t)src_texture->GetNativeResource());

    // maybe the window size is actually a pointer we will find out later.
    /*if (g_hook->m_render_texture_render_thread_hook) {
        g_hook->m_render_texture_render_thread_hook->call<void*>(stereo, rhi_command_list, backbuffer, src_texture, window_size);
    }*/
}

void FFakeStereoRenderingHook::init_canvas(FFakeStereoRendering* stereo, sdk::FSceneView* view, UCanvas* canvas) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("init canvas called!");
#else
    SPDLOG_INFO_ONCE("init canvas called!");
#endif

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    // Since the FSceneView and UCanvas structures will probably vary wildly
    // in terms of field offsets and size, we will need to dynamically scan
    // from the return address of this function to find the ViewProjectionMatrix offset.
    // in the FSceneView and also the UCanvas.
    // it happens in the else block of the conditional statement that calls this function
    static uint32_t fsceneview_viewproj_offset = 0;
    static uint32_t ucanvas_viewproj_offset = 0;

    if (fsceneview_viewproj_offset == 0 || ucanvas_viewproj_offset == 0) {
        SPDLOG_INFO("Searching for FSceneView and UCanvas offsets...");
        SPDLOG_INFO("Canvas: {:x}", (uintptr_t)canvas);

        const auto return_address = (uintptr_t)_ReturnAddress();
        const auto containing_function = utility::find_function_start(return_address);

        SPDLOG_INFO("Found containing function at {:x}", *containing_function);

        auto find_offsets = [](uintptr_t start, uintptr_t end) -> bool {
            for (auto ip = (uintptr_t)start; ip < end + 0x100;) {
                const auto ix = utility::decode_one((uint8_t*)ip);

                if (!ix) {
                    SPDLOG_ERROR("Failed to decode instruction at {:x}", ip);
                    break;
                }

                // The initial instructions look something like this
                /*
                0F 28 86 C0 03 00 00                          movaps  xmm0, xmmword ptr [rsi+3C0h]
                41 0F 11 87 80 02 00 00                       movups  xmmword ptr [r15+280h], xmm0
                */
                if (std::string_view{ix->Mnemonic} == "MOVAPS" && ix->Operands[1].Type == ND_OP_MEM) {
                    const auto next = utility::decode_one((uint8_t*)(ip + ix->Length));

                    if (next) {
                        if (std::string_view{next->Mnemonic} == "MOVUPS" && next->Operands[0].Type == ND_OP_MEM) {
                            fsceneview_viewproj_offset = ix->Operands[1].Info.Memory.Disp;
                            ucanvas_viewproj_offset = next->Operands[0].Info.Memory.Disp;
                            
                            SPDLOG_INFO("Found at {:x}", ip);
                            SPDLOG_INFO("Found FSceneView ViewProjectionMatrix offset: {:x}", fsceneview_viewproj_offset);
                            SPDLOG_INFO("Found UCanvas ViewProjectionMatrix offset: {:x}", ucanvas_viewproj_offset);
                            return true;
                            break;
                        }
                    }
                }

                ip += ix->Length;
            }

            return false;
        };

        if (!find_offsets(*containing_function, return_address)) {
            // If we still didn't find it at this stage, re-scan from the previous function from the previous function call instead.
            const auto potential_func = utility::calculate_absolute(return_address - 4);
            if (!find_offsets(potential_func, potential_func + 0x100)) {
                SPDLOG_ERROR("Failed to find offsets!");
                return;
            }
        }
    }

    //*(Matrix4x4f*)((uintptr_t)view + fsceneview_viewproj_offset) = VR::get()->get_projection_matrix(VRRuntime::Eye::LEFT);
    *(Matrix4x4f*)((uintptr_t)canvas + ucanvas_viewproj_offset) = *(Matrix4x4f*)((uintptr_t)view + fsceneview_viewproj_offset);
}

uint32_t FFakeStereoRenderingHook::get_desired_number_of_views_hook(FFakeStereoRendering* stereo, bool is_stereo_enabled) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("get desired number of views hook called!");
#else
    SPDLOG_INFO_ONCE("get desired number of views hook called!");
#endif

    auto& vr = VR::get();

    if (g_hook->m_sceneview_data.inside_post_init_properties) {
        return 2;
    }

    if (!is_stereo_enabled || (vr->is_using_afr() && !vr->is_splitscreen_compatibility_enabled())) {
        // We need to know about the second scene state to fix ghosting, so set the view count to 2
        // after we know about it, we can continue returning 1.
        if (is_stereo_enabled && vr->is_ghosting_fix_enabled() && vr->is_using_afr() &&
            g_hook->m_sceneview_data.known_scene_states.size() < 2 && g_hook->m_fixed_localplayer_view_count &&
            !!g_hook->m_sceneview_data.constructor_hook && g_hook->m_has_view_extensions_installed)
        {
            // Only works correctly if view extensions are installed, so we can reset the view count to 1 without crashing
            return 2;
        }

        return 1;
    }

    if (vr->is_native_stereo_fix_enabled()) {
        auto rtm = g_hook->get_render_target_manager();
        if ((rtm->get_scene_capture_render_target() == nullptr || !g_hook->m_sceneview_data.constructor_hook || !g_hook->m_render_module_begin_render_viewfamily_hook)) {
            if (rtm->get_scene_capture_utexture() == nullptr) {
                rtm->create_scene_capture();
            }

            return 1; // wait for the scene capture render target to be set and FSceneView constructor to be hooked
        }
    }

    return 2;
}

// Only really necessary for 5.0.3 because for some reason negative view index gets passed into it
// but 5.0.3 doesn't account for this and thinks it's a secondary pass
// so the purpose of the hook (mostly) is to make those return eSSP_FULL to fix a crash
EStereoscopicPass FFakeStereoRenderingHook::get_view_pass_for_index_hook(FFakeStereoRendering* stereo, bool stereo_requested, int32_t view_index) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("get view pass for index hook called! {} {}", stereo_requested, view_index);
#else
    SPDLOG_INFO_ONCE("get view pass for index hook called! {} {}", stereo_requested, view_index);
#endif

    // On 5.0.3 this check is not here, it was only added in 5.1
    // So we need to imitate it here to prevent a crash
    if (!stereo_requested || view_index < 0) {
        return EStereoscopicPass::eSSP_FULL;
    }

    return view_index % 2 == 0 ? EStereoscopicPass::eSSP_PRIMARY : EStereoscopicPass::eSSP_SECONDARY;
}

IStereoRenderTargetManager* FFakeStereoRenderingHook::get_render_target_manager_hook(FFakeStereoRendering* stereo) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("get render target manager hook called!");
#else
    SPDLOG_INFO_ONCE("get render target manager hook called!");
#endif

    if (!g_framework->is_game_data_intialized()) {
        return nullptr;
    }

    auto vr = VR::get();

    if (vr->is_stereo_emulation_enabled() || vr->is_extreme_compatibility_mode_enabled()) {
        return nullptr;
    }

    if (!vr->get_runtime()->got_first_poses || vr->is_hmd_active()) {
        if (g_hook->m_uses_old_rendertarget_manager) {
            return (IStereoRenderTargetManager*)&g_hook->m_rtm_418;
        }

        if (g_hook->m_special_detected) {
            return (IStereoRenderTargetManager*)&g_hook->m_rtm_special;
        }

        return &g_hook->m_rtm;
    }

    return nullptr;
}

IStereoLayers* FFakeStereoRenderingHook::get_stereo_layers_hook(FFakeStereoRendering* stereo) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("get stereo layers hook called!");
#else
    SPDLOG_INFO_ONCE("get stereo layers hook called!");
#endif

    if (!g_framework->is_game_data_intialized()) {
        return nullptr;
    }

    if (!VR::get()->get_runtime()->got_first_poses || VR::get()->is_hmd_active()) {
        /*static uint8_t fake_data[0x100]{};

        if (*(uintptr_t*)&fake_data == 0) {
            *(uintptr_t*)&fake_data = (uintptr_t)utility::get_executable() + 0x3D13420; // test
        }

        //return &g_hook->m_sl;
        return (IStereoLayers*)&fake_data;*/
    }

    return nullptr;
}

void FFakeStereoRenderingHook::post_calculate_stereo_projection_matrix(safetyhook::Context& ctx) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("post calculate stereo projection matrix called!");
#else
    SPDLOG_INFO_ONCE("post calculate stereo projection matrix called!");
#endif

    if (g_hook->m_fixed_localplayer_view_count || g_hook->m_hooked_alternative_localplayer_scan) {
        return;
    }

    auto vfunc = utility::find_virtual_function_start(g_hook->m_calculate_stereo_projection_matrix_post_hook.target_address());

    if (!vfunc) {
        // attempt to hook GetProjectionData instead to get the localplayer
        SPDLOG_INFO("Failed to find virtual function start for CalculateStereoProjectionMatrix, attempting to hook GetProjectionData instead...");

        if (!g_hook->m_projection_matrix_stack.empty() && g_hook->m_projection_matrix_stack.size() >= 3) {
            const auto post_get_projection_data = g_hook->m_projection_matrix_stack[2];

            const auto get_projection_data_candidate_1 = utility::find_function_start_with_call(post_get_projection_data);
            const auto get_projection_data_candidate_2 = utility::find_virtual_function_start(post_get_projection_data);

            // Select whichever one is closest to post_get_projection_data
            std::optional<uintptr_t> get_projection_data{};

            if (get_projection_data_candidate_1 && get_projection_data_candidate_2) {
                const auto candidate_1_distance = std::abs((int64_t)post_get_projection_data - (int64_t)*get_projection_data_candidate_1);
                const auto candidate_2_distance = std::abs((int64_t)post_get_projection_data - (int64_t)*get_projection_data_candidate_2);

                if (candidate_1_distance < candidate_2_distance) {
                    get_projection_data = get_projection_data_candidate_1;
                } else {
                    get_projection_data = get_projection_data_candidate_2;
                }
            } else if (get_projection_data_candidate_1) {
                get_projection_data = get_projection_data_candidate_1;
            } else if (get_projection_data_candidate_2) {
                get_projection_data = get_projection_data_candidate_2;
            } else {
                // emergency fallback
                SPDLOG_INFO("Failed to find GetProjectionData, falling back to emergency fallback (this may not work)");
                get_projection_data = utility::find_function_start(post_get_projection_data);
            }

            if (get_projection_data) {
                SPDLOG_INFO("Successfully found GetProjectionData at {:x}", *get_projection_data);

                g_hook->m_hooked_alternative_localplayer_scan = true;

                g_hook->m_get_projection_data_pre_hook = safetyhook::create_mid((void*)*get_projection_data, &FFakeStereoRenderingHook::pre_get_projection_data);
                g_hook->m_projection_matrix_stack.clear();

                if (g_hook->m_get_projection_data_pre_hook) {
                    SPDLOG_INFO("Successfully hooked GetProjectionData");
                    return;
                } else {
                    SPDLOG_ERROR("Failed to hook GetProjectionData");
                }
            } else {
                SPDLOG_ERROR("Failed to find GetProjectionData!");
            }
        }
    }

    if (!vfunc) {
        SPDLOG_INFO("Could not find function via normal means, scanning for int3s...");

        const auto ref = utility::scan_reverse(g_hook->m_calculate_stereo_projection_matrix_post_hook.target_address(), 0x2000, "CC CC CC");

        if (ref) {
            vfunc = *ref + 3;
        }

        if (!vfunc) {
            g_hook->m_fixed_localplayer_view_count = true;
            SPDLOG_ERROR("Failed to find virtual function start for post calculate_stereo_projection_matrix!");
            return;
        }
    }

    // Scan forward until we find an assignment of the RCX register into a storage register.
    std::unordered_map<uint32_t, uintptr_t*> register_to_context {
        { NDR_RBX, &ctx.rbx },
        { NDR_RCX, &ctx.rcx },
        { NDR_RDX, &ctx.rdx },
        { NDR_RSI, &ctx.rsi },
        { NDR_RDI, &ctx.rdi },
        { NDR_RBP, &ctx.rbp },
        { NDR_RSP, &ctx.rsp },
        { NDR_R8, &ctx.r8 },
        { NDR_R9, &ctx.r9 },
        { NDR_R10, &ctx.r10 },
        { NDR_R11, &ctx.r11 },
        { NDR_R12, &ctx.r12 },
        { NDR_R13, &ctx.r13 },
        { NDR_R14, &ctx.r14 },
        { NDR_R15, &ctx.r15 },
    };

    INSTRUX ix{};
    std::optional<uint32_t> found_register{};
    auto ip = (uint8_t*)vfunc.value_or(0);

    while (true) {
        const auto status = NdDecodeEx(&ix, (ND_UINT8*)ip, 1000, ND_CODE_64, ND_DATA_64);

        if (!ND_SUCCESS(status)) {
            SPDLOG_INFO("Decoding failed with error {:x}!", (uint32_t)status);
            break;
        }

        if (ix.Instruction == ND_INS_MOV && ix.Operands[0].Type == ND_OP_REG && ix.Operands[1].Type == ND_OP_REG && ix.Operands[1].Info.Register.Reg == NDR_RCX) {
            SPDLOG_INFO("Found assignment of RCX to storage register at {:x} ({})!", (uintptr_t)ip, ix.Operands[0].Info.Register.Reg);
            found_register = ix.Operands[0].Info.Register.Reg;
            break;
        }

        ip += ix.Length;
    }

    if (!found_register) {
        g_hook->m_fixed_localplayer_view_count = true;
        SPDLOG_ERROR("Failed to find assignment of RCX to storage register!");
        return;
    }

    const auto localplayer = *register_to_context[found_register.value_or(0)];
    SPDLOG_INFO("Local player: {:x}", localplayer);

    if (localplayer == 0) {
        g_hook->m_fixed_localplayer_view_count = true;
        SPDLOG_ERROR("Failed to find local player, cannot call PostInitProperties!");
        return;
    }

    g_hook->post_init_properties(localplayer);
}

void FFakeStereoRenderingHook::pre_get_projection_data(safetyhook::Context& ctx) {
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("pre get projection data called!");
#else
    SPDLOG_INFO_ONCE("pre get projection data called!");
#endif

    if (g_hook->m_fixed_localplayer_view_count) {
        return;
    }

    const auto localplayer = ctx.rcx;
    SPDLOG_INFO("Local player: {:x}", localplayer);

    if (localplayer == 0) {
        g_hook->m_fixed_localplayer_view_count = true;
        SPDLOG_ERROR("Failed to find local player, cannot call PostInitProperties!");
        return;
    }

    g_hook->post_init_properties(localplayer);
}

void FFakeStereoRenderingHook::post_init_properties(uintptr_t localplayer) {
    SPDLOG_INFO("Searching for PostInitProperties virtual function...");

    std::optional<uint32_t> idx{};
    const auto engine = sdk::UEngine::get_lvalue();

    if (engine == nullptr) {
        SPDLOG_ERROR("Cannot proceed without engine!");
        return;
    }

    const auto vtable = *(uintptr_t**)localplayer;

    if (vtable == nullptr || IsBadReadPtr((void*)vtable, sizeof(void*))) {
        SPDLOG_ERROR("Cannot proceed, vtable for so-called \"local player\" is invalid!");
        return;
    }

    INSTRUX ix{};

    for (auto i = 1; i < 25; ++i) {
        if (idx) {
            break;
        }

        SPDLOG_INFO("Analyzing index {}...", i);

        const auto vfunc = vtable[i];

        if (vfunc == 0 || IsBadReadPtr((void*)vfunc, 1)) {
            SPDLOG_ERROR("Encountered invalid vfunc at index {}!", i);
            break;
        }

        SPDLOG_INFO("Scanning vfunc at index {} ({:x})...", i, vfunc);

        utility::exhaustive_decode((uint8_t*)vfunc, 25, [&](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
            if (idx) {
                return utility::ExhaustionResult::BREAK;
            }

            if (const auto disp = utility::resolve_displacement(ip); disp) {
                // the second expression catches UE dynamic/debug builds
                if (*disp == (uintptr_t)engine || 
                    (!IsBadReadPtr((void*)*disp, sizeof(void*)) && *(uintptr_t*)*disp == (uintptr_t)*engine)) 
                {
                    SPDLOG_INFO("Found PostInitProperties at {} {:x}!", i, (uintptr_t)vfunc);
                    idx = i;
                    return utility::ExhaustionResult::BREAK;
                }
            }

            return utility::ExhaustionResult::CONTINUE;
        });
    }

    if (!idx) {
        SPDLOG_ERROR("Failed to find PostInitProperties virtual function! A crash may occur!");
    }

    // Now call PostInitProperties.
    // The purpose of this is setting up the view for the other eye.
    // Just creating the StereoRenderingDevice does not automatically do it, so we have to do it manually.
    // Usually the game just calls this function near startup after calling InitializeHMDDevice.
    if (idx) {
        SPDLOG_INFO("Calling PostInitProperties on local player!");

        // Get PEB and set debugger present
        auto peb = (PEB*)__readgsqword(0x60);

        const auto old = peb->BeingDebugged;
        peb->BeingDebugged = true;

        // If the exception count exceeds a certain amount, we need to un-nop the function call because it was supposed to return a pointer.
        static auto exception_count = 0;
        static std::vector<Patch::Ptr> patches{};
        static std::vector<uintptr_t> patch_locations{};

        static std::vector<Patch::Ptr> assert_patches{};
        const void (*post_init_properties)(uintptr_t) = (*(decltype(post_init_properties)**)localplayer)[*idx];

        // Scan through all of the branches of PostInitProperties to find any assertions
        // The assertion we're looking for is easily identified by a string that it loads in RCX, named "!Reference"
        // If we dont do this, there's a possibility that the game will crash at some point or cause some sort of corruption
        utility::exhaustive_decode((uint8_t*)post_init_properties, 100, [](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
            if (ix.Operands[1].Type == ND_OP_MEM) {
                const auto referenced_addr = utility::resolve_displacement(ip);

                if (referenced_addr) try {
                    if (std::string_view{(const char*)*referenced_addr}.starts_with("!Reference")) {
                        // Scan forward and patch out the first call or jmp we run into
                        utility::exhaustive_decode((uint8_t*)ip, 10, [](INSTRUX& ix, uintptr_t ip) -> utility::ExhaustionResult {
                            if (*(uint8_t*)ip == 0xE8) {
                                SPDLOG_INFO("Patching assertion at {:x}!", ip);
                                assert_patches.push_back(Patch::create(ip, { 0x90, 0x90, 0x90, 0x90, 0x90 }));
                                return utility::ExhaustionResult::BREAK;
                            }

                            if (*(uint8_t*)ip == 0xE9) {
                                SPDLOG_INFO("Patching assertion at {:x}!", ip);
                                assert_patches.push_back(Patch::create(ip, { 0xC3 }));
                                return utility::ExhaustionResult::BREAK;
                            }

                            if (std::string_view{ix.Mnemonic}.starts_with("CALL")) {
                                SPDLOG_INFO("Patching assertion at {:x}!", ip);
                                std::vector<int16_t> nop{};
                                for (auto i = 0; i < ix.Length; ++i) {
                                    nop.push_back(0x90);
                                }

                                assert_patches.push_back(Patch::create(ip, nop));
                                return utility::ExhaustionResult::BREAK;
                            }

                            return utility::ExhaustionResult::CONTINUE;
                        });
                    }
                } catch(...) {

                }
            }

            return utility::ExhaustionResult::CONTINUE;
        });

        // set up a handler to skip int3 assertions
        // we do this because debug builds assert when the views are already setup.
        const auto seh_handler = [](PEXCEPTION_POINTERS info) -> LONG {
            ++exception_count;

            if (info->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
                SPDLOG_INFO("Skipping int3 breakpoint at {:x}!", info->ContextRecord->Rip);
                const auto insn = utility::decode_one((uint8_t*)info->ContextRecord->Rip);

                if (insn) {
                    SPDLOG_INFO("Skipping {} bytes!", insn->Length);
                    info->ContextRecord->Rip += insn->Length;

                    // Nop out the next function call.
                    // It logs and does some other stuff and causes a crash later on.
                    // To be seen if this will cause any issues, does not appear to (on 4.9 debug builds)
                    const auto call = utility::scan_disasm((uintptr_t)info->ContextRecord->Rip, 20, "E8 ? ? ? ?");

                    if (call) {
                        patch_locations.push_back(*call);
                        patches.emplace_back(Patch::create(*call, {0x90, 0x90, 0x90, 0x90, 0x90}));
                    }

                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            SPDLOG_INFO("Encountered exception {:x} at {:x}!", info->ExceptionRecord->ExceptionCode, info->ContextRecord->Rip);

            // This happens if we removed a call that shouldn't have been removed.
            if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !patches.empty()) {
                SPDLOG_WARN("Access violation at {:x}! Removing patch at {:x}!", info->ContextRecord->Rip, patch_locations.back());

                exception_count = 0;
                info->ContextRecord->Rip = patch_locations.back();
                patches.pop_back();
                patch_locations.pop_back();
            } else {
                const auto insn = utility::decode_one((uint8_t*)info->ContextRecord->Rip);

                if (insn) {
                    info->ContextRecord->Rip += insn->Length;
                } else {
                    info->ContextRecord->Rip += 1;
                }
            }

            // yolo? idk xd
            return EXCEPTION_CONTINUE_EXECUTION;
        };

        const auto exception_handler = AddVectoredExceptionHandler(1, seh_handler);

        m_sceneview_data.inside_post_init_properties = true;
        post_init_properties(localplayer);
        m_sceneview_data.inside_post_init_properties = false;

        SPDLOG_INFO("PostInitProperties called!");

        // remove the handler
        RemoveVectoredExceptionHandler(exception_handler);
        peb->BeingDebugged = old;
    }

    g_hook->m_sceneview_data.known_scene_states.clear();
    g_hook->m_fixed_localplayer_view_count = true;
}

void* FFakeStereoRenderingHook::slate_draw_window_render_thread(void* renderer, void* a2, void* a3, 
                                                                void* a4, void* params, void* unk1, void* unk2) 
{
#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("SlateRHIRenderer::DrawWindow_RenderThread called!");
#else
    SPDLOG_INFO_ONCE("SlateRHIRenderer::DrawWindow_RenderThread called!");
#endif

    if (!g_framework->is_game_data_intialized() || a2 == nullptr) {
        return g_hook->m_slate_thread_hook.call<void*>(renderer, a2, a3, a4, params, unk1, unk2);
    }

    auto viewport_info = (sdk::FViewportInfo*)a3;
    sdk::ISlateViewport* slate_viewport = nullptr; // UE5.5+
    void** a4_ptr = (void**)a4;

    static bool a4_is_ue_5_5_variant = [&]() -> bool {
        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Checking if a4 is UE 5.5 variant...");

        __try {
            if (a4_ptr[0] == renderer) {
                SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] a4 is UE 5.5 variant!");
                return true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            SPDLOG_WARN("Exception occurred while checking if a4 is UE 5.5 variant!");
        }

        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] a4 is not UE 5.5 variant!");

        return false;
    }();

    if (!a4_is_ue_5_5_variant) {
        // How are we going to fix this on UE5.5?
        g_hook->get_slate_thread_worker()->execute((FRHICommandListImmediate*)a2);
    } else {
        const auto window = (uintptr_t)a4_ptr[2];

        static std::optional<size_t> viewport_offset = [&]() -> std::optional<size_t> {
            std::optional<size_t> result{};
            const auto module_within = utility::get_module_within(g_hook->m_slate_thread_hook.target_address());

            // Temporarily unhook the DrawWindow_RenderThread hook because we need to emulate the function
            // We could use the trampoline but bdshemu is picky about whether RIP is
            // within the "shellcode" or not (e.g. within the module bounds)
            // and so, the hook must be temporarily unhooked
            if (!module_within) {
                SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Failed to get module within for target address!");
                return result;
            }

            SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Module within: {:x}", (uintptr_t)*module_within);

            if (!g_hook->m_slate_thread_hook.disable().has_value()) {
                SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Failed to disable slate thread hook!");
                return result;
            }

            utility::ScopeGuard guard{[&]() {
                SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Re-enabling slate thread hook!");
                if (!g_hook->m_slate_thread_hook.enable().has_value()) {
                    SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Failed to re-enable slate thread hook!");
                }
            }};

            utility::ShemuContext ctx{*module_within};
            ctx.ctx->Registers.RegRip = (ND_UINT64)g_hook->m_slate_thread_hook.target_address();
            ctx.ctx->Registers.RegRcx = (ND_UINT64)renderer;
            ctx.ctx->Registers.RegRdx = (ND_UINT64)a2;
            ctx.ctx->Registers.RegR8 = (ND_UINT64)a3;
            ctx.ctx->Registers.RegR9 = (ND_UINT64)a4;
            ctx.ctx->MemThreshold = 1000;

            uint32_t window_getter_callstack_level = 0;
            std::span<uint8_t> window_bounds{(uint8_t*)window, (uint8_t*)window + 0x1000};

            utility::emulate(*module_within, ctx.ctx->Registers.RegRip, 1000, ctx, [&](const utility::ShemuContextExtended& ctx) -> utility::ExhaustionResult {
                SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Emulating instruction: {:x} ({:X})", ctx.ctx->ctx->Registers.RegRip, ctx.ctx->ctx->Registers.RegRip - (uintptr_t)*module_within);

                auto is_within_stack = [&](uintptr_t addr) -> bool {
                    return addr >= ctx.ctx->ctx->StackBase && addr < ctx.ctx->ctx->StackBase + ctx.ctx->ctx->StackSize;
                };

                // Allow writes to go through if we are inside the window getter.
                // The downside is this might unintentionally increase the reference count of the window
                // but it's necessary for the window getter to not give us a nullptr.
                if (ctx.next.writes_to_memory && window_getter_callstack_level == 0) {
                    bool allow_write = false;

                    // However, if it writes to the stack, allow it through.
                    for (size_t i = 0; i < ctx.next.ix.OperandsCount; ++i) {
                        const auto& op = ctx.next.ix.Operands[i];
                        
                        if (op.Type == ND_OP_MEM && op.Access.Write) {
                            const auto base_reg = op.Info.Memory.HasBase ? ((uint64_t*)&ctx.ctx->ctx->Registers.RegRax)[op.Info.Memory.Base] : 0;
                            const auto index_reg = op.Info.Memory.HasIndex ? ((uint64_t*)&ctx.ctx->ctx->Registers.RegRax)[op.Info.Memory.Index] : 0;
                            const auto addr = base_reg + index_reg * op.Info.Memory.Scale + op.Info.Memory.Disp;

                            if (is_within_stack(addr)) {
                                SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Allowing write to stack at {:x}!", addr);
                                allow_write = true;
                                break;
                            }
                        }
                    }

                    if (!allow_write) {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Instruction writes to memory but we're not inside the window getter, skipping! ({:x})", ctx.ctx->ctx->Registers.RegRip);
                        return utility::ExhaustionResult::STEP_OVER;
                    }
                }

                if (std::string_view{ctx.next.ix.Mnemonic}.starts_with("CALL")) {
                    if (window_getter_callstack_level > 0) {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Allowing call inside window getter function, continuing!");
                        ++window_getter_callstack_level;
                        return utility::ExhaustionResult::CONTINUE;
                    }

                    const auto rcx_within_bounds = (uint8_t*)ctx.ctx->ctx->Registers.RegRcx >= window_bounds.data() && (uint8_t*)ctx.ctx->ctx->Registers.RegRcx < window_bounds.data() + window_bounds.size();
                    const auto rdx_within_bounds = (uint8_t*)ctx.ctx->ctx->Registers.RegRdx >= window_bounds.data() && (uint8_t*)ctx.ctx->ctx->Registers.RegRdx < window_bounds.data() + window_bounds.size();

                    // Check if RCX != window first. We don't want to skip over the call if it is set to it.
                    // There are inlined and non-inlined versions of this function which is why we need to check this.
                    if (!rcx_within_bounds && !rdx_within_bounds) {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Skipping call (not within window bounds)!");
                        return utility::ExhaustionResult::STEP_OVER;
                    }

                    SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Allowing call to 0x{:x}, RCX or RDX matches window {:x}!", ctx.next.ix.Operands[0].Info.Register.Reg, window);
                    SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] RCX: {:x}, RDX: {:x}", ctx.ctx->ctx->Registers.RegRcx, ctx.ctx->ctx->Registers.RegRdx);
                    ++window_getter_callstack_level;
                    return utility::ExhaustionResult::CONTINUE;
                }

                // Check if we hit a ret and are inside the window getter function.
                if (ctx.next.ix.Instruction == ND_INS_RETN) {
                    if (window_getter_callstack_level > 0) { 
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Hit ret inside window getter function, continuing!");
                        --window_getter_callstack_level;
                        return utility::ExhaustionResult::CONTINUE;
                    }
                }

                // We're looking for a mov reg, [reg+offset] instruction
                // where reg contains the pointer to the window
                // and offset is the offset to the viewport.
                const auto& cctx = ctx.ctx->ctx;
                const auto& ix = ctx.next.ix;

                // Debug stuff
#if 0
                SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Instruction: {:x} ({})", ctx.ctx->ctx->Registers.RegRip, ix.Mnemonic);

                for (uint32_t i = 0; i < ix.OperandsCount; ++i) {
                    const auto& op = ix.Operands[i];

                    if (op.Type == ND_OP_REG) {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Operand {} is register: {}", i, op.Info.Register.Reg);
                    } else if (op.Type == ND_OP_MEM) {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Operand {} is memory: [base: {}, index: {}, scale: {}, disp: {:x}]", i,
                            op.Info.Memory.HasBase ? op.Info.Memory.Base : 0,
                            op.Info.Memory.HasIndex ? op.Info.Memory.Index : 0,
                            op.Info.Memory.Scale,
                            op.Info.Memory.HasDisp ? op.Info.Memory.Disp : 0);
                    } else {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Operand {} is of type {}", i, static_cast<uint32_t>(op.Type));
                    }
                }

                SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] RDI: {:x}", ctx.ctx->ctx->Registers.RegRdi);
                SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] RDX: {:x}", ctx.ctx->ctx->Registers.RegRdx);
#endif

                if (ix.Instruction == ND_INS_MOV && ix.Operands[0].Type == ND_OP_REG && ix.Operands[1].Type == ND_OP_MEM &&
                    ix.Operands[1].Info.Memory.HasBase && ix.Operands[1].Info.Memory.HasDisp)
                {
                    uintptr_t* reg = (uintptr_t*)&((uint64_t*)&cctx->Registers.RegRax)[ix.Operands[1].Info.Memory.Base];
                    SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Found memory operand with base register {:x} and displacement {:x}!", (uintptr_t)reg, ix.Operands[1].Info.Memory.Disp);

                    // Instead of checking the window, we check if the register is within the bounds of the window's memory.
                    // This should allow us to catch all sorts of compiler optimizations.
                    if ((uint8_t*)*reg >= window_bounds.data() && (uint8_t*)*reg < window_bounds.data() + window_bounds.size()) try {
                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Base register {:x} is within window bounds, checking offset...", (uintptr_t)reg);

                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Found window pointer at {:x}!", (uintptr_t)reg);
                        auto offset = ix.Operands[1].Info.Memory.Disp;
                        const auto value = *(uintptr_t***)((uintptr_t)*reg + offset);

                        if (value == nullptr || IsBadReadPtr((void*)value, sizeof(void*))) {
                            SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Skipping invalid offset at {:x}!", (uintptr_t)value);
                            return utility::ExhaustionResult::CONTINUE;
                        }

                        if (*value == nullptr || IsBadReadPtr((void*)*value, sizeof(void*))) {
                            SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Skipping invalid vtable at {:x}!", (uintptr_t)*value);
                            return utility::ExhaustionResult::CONTINUE;
                        }

                        if (!utility::get_module_within(*value).has_value() || !utility::get_module_within((*value)[0]).has_value()) {
                            SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Skipping invalid module at {:x}!", (uintptr_t)*value);
                            return utility::ExhaustionResult::CONTINUE;
                        }

                        const auto behind_value = *(uintptr_t***)((uintptr_t)*reg + offset - sizeof(void*));

                        if (behind_value != nullptr && !IsBadReadPtr((void*)behind_value, sizeof(void*)) &&
                            *behind_value != nullptr && !IsBadReadPtr((void*)*behind_value, sizeof(void*)) &&
                            utility::get_module_within(*behind_value).has_value() && utility::get_module_within((*behind_value)[0]).has_value())
                        {
                            SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Adjusting offset by sizeof(void*)!");
                            offset -= sizeof(void*);
                        }

                        result = (*reg + offset) - (uintptr_t)window;

                        SPDLOG_INFO("[SlateRHIRenderer::DrawWindow_RenderThread] Found viewport offset at {:x}!", *result);
                        return utility::ExhaustionResult::BREAK;
                    } catch (...) {
                        SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Exception while checking offset!");
                    }
                }

                return utility::ExhaustionResult::CONTINUE;
            });

            if (!result) {
                SPDLOG_ERROR("[SlateRHIRenderer::DrawWindow_RenderThread] Failed to find viewport offset!");
            }

            return result;
        }();

        if (viewport_offset) {
            slate_viewport = *(sdk::ISlateViewport**)((uintptr_t)window + *viewport_offset);
        }
    }

    const auto& mods = g_framework->get_mods()->get_mods();

    for (auto& mod : mods) {
        mod->on_pre_slate_draw_window(renderer, a2, viewport_info);
    }

    g_hook->m_inside_slate_draw_window = true;
    g_hook->m_slate_draw_window_thread_id = GetCurrentThreadId();

    auto call_orig = [&]() {
        auto ret = g_hook->m_slate_thread_hook.call<void*>(renderer, a2, a3, a4, params, unk1, unk2);

        for (auto& mod : mods) {
            mod->on_post_slate_draw_window(renderer, a2, viewport_info);
        }

        g_hook->m_inside_slate_draw_window = false;

        return ret;
    };


    auto vr = VR::get();

    if (!vr->is_hmd_active() || vr->is_stereo_emulation_enabled()) {
        return call_orig();
    }

    const auto ui_target = g_hook->get_render_target_manager()->get_ui_target();

    if (ui_target == nullptr) {
        SPDLOG_INFO_EVERY_N_SEC(1, "No UI target, skipping!");
        return call_orig();
    }

    sdk::FSlateResource* slate_resource = nullptr;

    if (slate_viewport != nullptr) {
        slate_resource = slate_viewport->GetViewportRenderTargetTexture();
    } else {
        const auto viewport_rt_provider = viewport_info->get_rt_provider(g_hook->get_render_target_manager()->get_render_target());

        if (viewport_rt_provider == nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(1, "No viewport RT provider, skipping!");
            return call_orig();
        }
    
        slate_resource = viewport_rt_provider->get_viewport_render_target_texture();
    }

    if (slate_resource == nullptr) {
        SPDLOG_INFO_EVERY_N_SEC(1, "No slate resource, skipping!");
        return call_orig();
    }
    
    // Replace the texture with one we have control over.
    // This isolates the UI to render on our own texture separate from the scene.
    const auto old_texture = slate_resource->get_mutable_resource();
    slate_resource->get_mutable_resource() = ui_target;

    // To be seen if we need to resort to a MidHook on this function if the parameters
    // are wildly different between UE versions.
    const auto ret = g_hook->m_slate_thread_hook.call<void*>(renderer, a2, a3, a4, params, unk1, unk2);

    // Restore the old texture.
    slate_resource->get_mutable_resource() = old_texture;

    for (auto& mod : mods) {
        mod->on_post_slate_draw_window(renderer, a2, viewport_info);
    }
    
    // After this we copy over the texture and clear it in the present hook. doing it here just seems to crash sometimes.
    SPDLOG_INFO_ONCE("SlateRHIRenderer::DrawWindow_RenderThread finished!");

    return ret;
}

// INTERNAL USE ONLY!!!!
__declspec(noinline) void VRRenderTargetManager::CalculateRenderTargetSize(const sdk::FViewport& Viewport, uint32_t& InOutSizeX, uint32_t& InOutSizeY) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager::CalculateRenderTargetSize called!");

    m_last_calculate_render_size_return_address = (uintptr_t)_ReturnAddress();

    VRRenderTargetManager_Base::calculate_render_target_size(Viewport, InOutSizeX, InOutSizeY);
}

__declspec(noinline) bool VRRenderTargetManager::NeedReAllocateDepthTexture(const void* DepthTarget) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager::NeedReAllocateDepthTexture called!");

    m_last_needs_reallocate_depth_texture_return_address = (uintptr_t)_ReturnAddress();

    if (this->depth_analysis_passed) {
        return VRRenderTargetManager_Base::need_reallocate_depth_texture(DepthTarget);
    }

    return false;
}

__declspec(noinline) bool VRRenderTargetManager::NeedReAllocateShadingRateTexture(const void* ShadingRateTarget) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager::NeedReAllocateShadingRateTexture called!");

    const auto return_address = (uintptr_t)_ReturnAddress();
    const auto diff = return_address - m_last_calculate_render_size_return_address;

    if (diff <= 0x50) {
        // We need to switch the FFakeStereoRenderingHook's render target manager
        // to the old one NOW or we will crash. Reason being what was actually called
        // is the GetNumberOfBufferedFrames function, not NeedReAllocateShadingRateTexture.
        SPDLOG_INFO("Switching to old render target manager! Incorrect function called!");
        //g_hook->switch_to_old_rendertarget_manager();

        // Do a switcharoo on the vtable of this object to the old one because we will crash if we don't.
        // I've decided against actually switching the entire object over in favor of just vtable
        // swapping for now even though it's kind of a hack.
        const auto fake_object = std::make_unique<VRRenderTargetManager_418>();
        *(void**)this = *(void**)fake_object.get();

        return true; // The return value should actually be 1, so just return true.
    }

    return false;
}

void VRRenderTargetManager_Base::update_viewport(bool use_separate_rt, const sdk::FViewport& vp, class SViewport* vp_widget) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager_Base::update_viewport called! {} {:x} {:x}", use_separate_rt, (uintptr_t)&vp, (uintptr_t)vp_widget);

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    //SPDLOG_INFO("Widget: {:x}", (uintptr_t)ViewportWidget);
}

void VRRenderTargetManager_Base::calculate_render_target_size(const sdk::FViewport& viewport, uint32_t& x, uint32_t& y) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager_Base::calculate_render_target_size called!");

#ifdef FFAKE_STEREO_RENDERING_LOG_ALL_CALLS
    SPDLOG_INFO("calculate render target size called!");
#endif

    if (!g_framework->is_game_data_intialized()) {
        return;
    }

    SPDLOG_INFO("RenderTargetSize Before: {}x{}", x, y);

    // See VR::flat3d_single_view_target.
    const auto single_view = VR::get()->flat3d_single_view_target();

    x = VR::get()->get_hmd_width() * (single_view ? 1 : 2);
    y = VR::get()->get_hmd_height();

    if (single_view) {
        SPDLOG_INFO_ONCE("[Flat3D] SINGLE-view render target {}x{}: one view per frame owns the WHOLE surface, "
                         "so a double-wide would leave it covering only the left half", x, y);
    }

    SPDLOG_DEBUG("RenderTargetSize After: {}x{}", x, y);
}

bool VRRenderTargetManager_Base::need_reallocate_view_target(const sdk::FViewport& Viewport) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager_Base::need_reallocate_view_target called!");

    if (!g_framework->is_game_data_intialized()) {
        return false;
    }

    if (!m_attempted_find_force_separate_rt) try {
        m_attempted_find_force_separate_rt = true;

        // Go up the stack until we find something that isn't in our module.
        const auto our_module = g_framework->get_framework_module();
        constexpr auto max_stack_depth = 100;
        uintptr_t stack[max_stack_depth]{};

        const auto depth = RtlCaptureStackBackTrace(0, max_stack_depth, (void**)&stack, nullptr);

        std::optional<uintptr_t> ret_addr{};
        std::optional<HMODULE> module_within{};

        for (auto i = 0; i < depth; ++i) {
            SPDLOG_INFO("Stack[{}]: {:x}", i, stack[i]);

            module_within = utility::get_module_within(stack[i]);

            if (!module_within) {
                continue;
            }

            if (*module_within != our_module) {
                ret_addr = stack[i];
                break;
            }
        }

        // Emulate from the return address and find a memory write
        // this should contain the offset to the force separate rt bool.
        if (ret_addr) {
            SPDLOG_INFO("Found return address: {:x}", *ret_addr);

            utility::ShemuContext ctx{*module_within};
            ctx.ctx->Registers.RegRip = *ret_addr;
            ctx.ctx->Registers.RegRax = 1; // As if we're returning true from this function.

            utility::emulate(*module_within, *ret_addr, 100, ctx, [this](const utility::ShemuContextExtended& ctx) -> utility::ExhaustionResult {
                SPDLOG_INFO("Emulating instruction: {:x}", ctx.ctx->ctx->Registers.RegRip);

                if (ctx.next.writes_to_memory) {
                    const auto& ix = ctx.next.ix;
                    if (ix.Instruction == ND_INS_MOV && ix.Operands[0].Type == ND_OP_MEM && ix.Operands[1].Type == ND_OP_REG) {
                        // We're looking for a mov [reg1+N], reg2
                        const auto& op0 = ix.Operands[0];

                        // Needs a register
                        if (!op0.Info.Memory.HasBase || op0.Info.Memory.IsRipRel) {
                            return utility::ExhaustionResult::STEP_OVER;
                        }

                        // Needs a displacement
                        if (!op0.Info.Memory.HasDisp) {
                            return utility::ExhaustionResult::STEP_OVER;
                        }

                        // We don't want a stack based register
                        if (op0.Info.Memory.Base == NDR_RSP || op0.Info.Memory.Base == NDR_RBP) {
                            return utility::ExhaustionResult::STEP_OVER;
                        }

                        if (op0.Info.Memory.Disp > 0 && op0.Info.Memory.Disp < 0x2000) {
                            m_viewport_force_separate_rt_offset = op0.Info.Memory.Disp;
                            SPDLOG_INFO("Found force separate rt offset: {:x}", *m_viewport_force_separate_rt_offset);
                            return utility::ExhaustionResult::BREAK;
                        }
                    }

                    SPDLOG_INFO("Stepping over...");

                    return utility::ExhaustionResult::STEP_OVER;
                }

                if (std::string_view{ctx.next.ix.Mnemonic}.starts_with("CALL")) {
                    // We need to break out of this, we should've found the offset before the call.
                    SPDLOG_ERROR("Failed to find force separate rt offset! Encountered call at {:x}", ctx.ctx->ctx->Registers.RegRip);
                    return utility::ExhaustionResult::BREAK;
                }

                return utility::ExhaustionResult::CONTINUE;
            });
        }
    } catch(...) { // if we dont find it, it's fine, not very many games require it.
        SPDLOG_ERROR("Failed to find force separate rt offset! (Exception)");
    }

    const auto w = VR::get()->get_hmd_width();
    const auto h = VR::get()->get_hmd_height();

    if (w != this->last_width || h != this->last_height || g_hook->should_recreate_textures()) {
        SPDLOG_INFO("Reallocating view target! {} {} -> {} {}", this->last_width, this->last_height, w, h);

        this->last_width = w;
        this->last_height = h;
        this->wants_depth_reallocate = true;
        this->destroy_scene_capture();
        g_hook->set_should_recreate_textures(false);
        return true;
    }

    return false;
}

bool VRRenderTargetManager_Base::need_reallocate_depth_texture(const void* DepthTarget) {
    SPDLOG_INFO_ONCE("VRRenderTargetManager_Base::need_reallocate_depth_texture called!");

    if (!g_framework->is_game_data_intialized()) {
        return false;
    }

    if (this->wants_depth_reallocate) {
        SPDLOG_INFO("Reallocating depth texture!");

        this->wants_depth_reallocate = false;
        return true;
    }

    return false;
}

// Size for the redirected UI (Slate) render target. Normally the backbuffer
// size — but under the 3D Display native-output override the swapchain is
// held at the display's native size while the ENGINE believes (and draws
// Slate at) its own requested resolution; the UI target must match that
// belief or the UI renders cropped into a corner of the larger texture.
static auto get_ui_texture_size() {
    auto size = g_framework->is_dx11() ? g_framework->get_d3d11_rt_size() : g_framework->get_d3d12_rt_size();

    if (VR::get()->is_using_flat3d()) {
        uint32_t believed_w = 0;
        uint32_t believed_h = 0;

        if (g_framework->is_dx11()) {
            if (const auto& hook = g_framework->get_d3d11_hook(); hook != nullptr) {
                believed_w = hook->get_engine_believed_width();
                believed_h = hook->get_engine_believed_height();
            }
        } else {
            if (const auto& hook = g_framework->get_d3d12_hook(); hook != nullptr) {
                believed_w = hook->get_engine_believed_width();
                believed_h = hook->get_engine_believed_height();
            }
        }

        if (believed_w != 0 && believed_h != 0) {
            size.x = (float)believed_w;
            size.y = (float)believed_h;
        }
    }

    return size;
}

void VRRenderTargetManager_Base::pre_texture_hook_callback(safetyhook::Context& ctx, bool from_second) {
    SPDLOG_INFO("PreTextureHook called! {}", ctx.r8);

    // maybe do some work later to bruteforce the registers/offsets for these
    // a la emulation or something more rudimentary
    // since it always seems to access a global right before, which
    // refers to the current pixel format, which we can overwrite (which may not be safe)
    // so we could just follow how the global is being written to registers or the stack
    // and then just overwrite the registers/stack with our own values
    auto rtm = g_hook->get_render_target_manager();

    if (!rtm->allocate_texture_called) {
        SPDLOG_ERROR("AllocateTexture not called yet! (PreTextureHook)");
        return;
    }

    if (!g_hook->has_pixel_format_cvar()) {
        if (g_hook->get_render_target_manager()->is_pre_texture_call_e8) {
            //ctx.r8 = 2; // PF_B8G8R8A8 // decided not to actually set it here, we need to double check when it's actually called
        } else if (!rtm->is_using_texture_desc) {
            *((uint8_t*)ctx.rsp + 0x28) = 2; // PF_B8G8R8A8
        }
    }

    // Now we are going to attempt to JIT a function that will call the original function
    // using the context we have. This will call it twice, but allow us to
    // have control over one of the textures it generates. We need
    // the other generated texture as a UI render target to be used in FFakeStereoRenderingHook::slate_draw_window_render_thread.
    // This will allow the original game UI to be rendered in world space without resorting to WidgetComponent.
    // One can argue that this may be an overengineered alternative to "just" calling FDynamicRHI::CreateTexture2D
    // but that function is very hard to pattern scan for, and we already have it here, so why not use it?
    using namespace asmjit;
    using namespace asmjit::x86;

    SPDLOG_INFO("Attempting to JIT a function to call the original function!");

    auto& insn_bytes = !from_second ? rtm->texture_create_insn_bytes : rtm->texture_create_insn_bytes2;

    const auto ix = utility::decode_one(insn_bytes.data(), insn_bytes.size());

    if (!ix) {
        SPDLOG_ERROR("Failed to decode instruction!");
        return;
    }
    
    // We can't do it to the normal E8 call because the code is not in the same area
    // so RIP relative calls are not possible through the emulator. will just have to
    // resolve those manually through disassembly.
    uintptr_t func_ptr = 0;

    if (!g_hook->get_render_target_manager()->is_pre_texture_call_e8) {
        // Set up the emulator. We will use it to emulate the function call.
        // All we need from it is where the function call lands, so we can call it for real.
        auto emu_ctx = utility::ShemuContext(
            (uintptr_t)insn_bytes.data(),
            insn_bytes.size());

        SPDLOG_INFO("Insn bytes size: {}", insn_bytes.size());
        for (size_t i = 0; i < insn_bytes.size(); ++i) {
            SPDLOG_INFO("Byte[{}]: {:x}", i, insn_bytes[i]);
        }

        emu_ctx.ctx->Registers.RegRcx = ctx.rcx;
        emu_ctx.ctx->Registers.RegRdx = ctx.rdx;
        emu_ctx.ctx->Registers.RegR8 = ctx.r8;
        emu_ctx.ctx->Registers.RegR9 = ctx.r9;
        emu_ctx.ctx->Registers.RegRbx = ctx.rbx;
        emu_ctx.ctx->Registers.RegRax = ctx.rax;
        emu_ctx.ctx->Registers.RegRdi = ctx.rdi;
        emu_ctx.ctx->Registers.RegRsi = ctx.rsi;
        emu_ctx.ctx->Registers.RegR10 = ctx.r10;
        emu_ctx.ctx->Registers.RegR11 = ctx.r11;
        emu_ctx.ctx->Registers.RegR12 = ctx.r12;
        emu_ctx.ctx->Registers.RegR13 = ctx.r13;
        emu_ctx.ctx->Registers.RegR14 = ctx.r14;
        emu_ctx.ctx->Registers.RegR15 = ctx.r15;

        // if disasm is call [rsp+N] we need to set RSP to the actual stack
        // otherwise emulation will fail.
        // conversely, if we set RSP when it's NOT using RSP in the register
        // it will also fail.
        if (ix->Operands[0].Type == ND_OP_MEM && ix->Operands[0].Info.Memory.HasBase &&
            ix->Operands[0].Info.Memory.Base == NDR_RSP)
        {
            emu_ctx.ctx->Registers.RegRsp = ctx.rsp;
            emu_ctx.ctx->Stack = (ND_UINT8*)ctx.rsp;
            emu_ctx.ctx->StackBase = ctx.rsp;
            SPDLOG_INFO("Setting RSP to {:x} for emulation!", ctx.rsp);
        } else {
            SPDLOG_INFO("Not setting RSP for emulation!");
        }

        emu_ctx.ctx->MemThreshold = 1;

        if (emu_ctx.emulate((uintptr_t)insn_bytes.data(), 1) != SHEMU_SUCCESS) {
            SPDLOG_ERROR("Failed to emulate instruction!: {} RIP: {:x}", emu_ctx.status, emu_ctx.ctx->Registers.RegRip);
            return;
        }
    
        SPDLOG_INFO("Emu landed at {:x}", emu_ctx.ctx->Registers.RegRip);
        func_ptr = emu_ctx.ctx->Registers.RegRip;

        if (func_ptr == 0) {
            SPDLOG_ERROR("Function pointer is null after emulation!");
            return;
        }
    } else {
        const auto target = g_hook->get_render_target_manager()->pre_texture_hook.target_address();
        func_ptr = target + 5 + *(int32_t*)&insn_bytes.data()[1];
    }

    SPDLOG_INFO("Function pointer: {:x}", func_ptr);

    /*CodeHolder code{};
    JitRuntime runtime{};
    code.init(runtime.environment());

    Assembler a{&code};
    
    static auto cloned_stack = std::make_unique<std::array<uint8_t, 0x3000>>();
    static auto cloned_registers = std::make_unique<std::array<uint8_t, 0x1000>>();

    auto aligned_stack = ((uintptr_t)&(*cloned_stack)[0x2000]);
    aligned_stack += (-(intptr_t)aligned_stack) & (40 - 1);

    memcpy((void*)aligned_stack, (void*)(ctx.rsp), 0x1000);

    static auto stack_ptr = std::make_unique<uintptr_t>();
    static auto post_register_storage = std::make_unique<uintptr_t>();

    // Store the original stack pointer.
    a.movabs(rax, (void*)stack_ptr.get());
    a.mov(ptr(rax), rsp);

    // Push all of the original registers onto the stack.
    a.movabs(rsp, (void*)&(*cloned_registers)[0x500]);
    //a.mov(rsp, rax);

    a.push(rcx);
    a.push(rdx);
    a.push(r8);
    a.push(r9);
    a.push(r10);
    a.push(r11);
    a.push(r12);
    a.push(r13);
    a.push(r14);
    a.push(r15);
    a.push(rbx);
    a.push(rbp);
    a.push(rsi);
    a.push(rdi);
    a.pushfq();

    a.mov(rax, (void*)post_register_storage.get());
    a.mov(ptr(rax), rsp);

    a.movabs(rsp, aligned_stack);


    a.mov(rdx, rcx); // func param
    a.movabs(rcx, ctx.rcx);
    //a.movabs(rdx, ctx.rdx);
    a.movabs(r8, ctx.r8);
    //a.movabs(r9, ctx.r9);
    const auto size = get_ui_texture_size();
    a.mov(r9, (uint32_t)size.x);
    // move w into first stack argument
    a.mov(dword_ptr(rsp, 0x20), (uint32_t)size.y);
    a.movabs(r10, ctx.r10);
    a.movabs(r11, ctx.r11);
    a.movabs(r12, ctx.r12);
    a.movabs(r13, ctx.r13);
    a.movabs(r14, ctx.r14);
    a.movabs(r15, ctx.r15);
    a.movabs(rax, ctx.rax);
    a.movabs(rbx, ctx.rbx);
    a.movabs(rbp, ctx.rbp);
    a.movabs(rsi, ctx.rsi);
    a.movabs(rdi, ctx.rdi);

    // Correct the stack pointers inside the stack we cloned
    // to point to areas within the cloned stack if they were
    // pointing to the original stack.
    for (auto stack_var = 0; stack_var < 0x1000; stack_var += sizeof(void*)) {
        auto stack_var_ptr = (uintptr_t*)(aligned_stack + stack_var);

        if (*stack_var_ptr >= ctx.rsp && *stack_var_ptr < ctx.rsp + 0x1000) {
            SPDLOG_INFO("Correcting stack var at 0x{:x}", stack_var);
            *stack_var_ptr = aligned_stack + (*stack_var_ptr - ctx.rsp);
        }
    }

    auto correct_register = [&](auto& reg) {
        if (reg >= ctx.rsp && reg < ctx.rsp + 0x1000) {
            SPDLOG_INFO("Correcting Register");
            reg = aligned_stack + (reg - ctx.rsp);
        }

    };
    for (auto insn_byte : g_hook->get_render_target_manager()->texture_create_insn_bytes) {
        a.db(insn_byte);
    }

    a.mov(rsp, post_register_storage.get());
    a.mov(rsp, ptr(rsp));
    //a.mov(rsp, rcx);

    // Pop all of the original registers off of the stack.
    a.popfq();
    a.pop(rdi);
    a.pop(rsi);
    a.pop(rbp);
    a.pop(rbx);
    a.pop(r15);
    a.pop(r14);
    a.pop(r13);
    a.pop(r12);
    a.pop(r11);
    a.pop(r10);
    a.pop(r9);
    a.pop(r8);
    a.pop(rdx);
    a.pop(rcx);

    //a.pop(rsp); // Restore the original stack pointer.
    a.movabs(rsp, (void*)stack_ptr.get());
    a.mov(rsp, ptr(rsp));

    a.ret();

    uintptr_t code_addr{};
    runtime.add(&code_addr, &code);

    SPDLOG_INFO("JITed address: {:x}", code_addr);

    //MessageBox(0, "debug now", "debug", 0);

    void (*func)(void* rdx) = (decltype(func))code_addr;

    static FTexture2DRHIRef out{};
    out.texture = nullptr;
    func(&out);*/

    auto call_with_context = [&](uintptr_t func, FTexture2DRHIRef& out) {
        CodeHolder code{};
        JitRuntime runtime{};
        code.init(runtime.environment());

        Assembler a{&code};

        auto post_align_label = a.newLabel();

        a.push(rbx);

        a.mov(rcx, ctx.rcx);
        
        if (!g_hook->get_render_target_manager()->is_pre_texture_call_e8) {
            a.movabs(rdx, (uintptr_t)&out);
        } else {
            a.mov(rdx, ctx.rdx);
        }

        a.mov(r8, ctx.r8);

        const auto size = get_ui_texture_size();
        a.mov(r9, (uint32_t)size.x);

        a.sub(rsp, 0x100);
        a.mov(rbx, 0x100);
        a.test(rsp, sizeof(void*));
        a.jz(post_align_label);

        a.sub(rsp, 8);
        a.mov(rbx, 0x108);
        a.bind(post_align_label);

        a.mov(ptr(rsp, 0x20), (uint32_t)size.y);

        for (auto i = 0x28; i < 0x90; i += sizeof(void*)) {
            a.mov(rax, *(uintptr_t*)(ctx.rsp + i));
            a.mov(ptr(rsp, i), rax);
        }

        a.mov(rax, (void*)func);
        a.call(rax);

        a.add(rsp, rbx);
        a.pop(rbx);

        a.ret();

        uintptr_t code_addr{};
        runtime.add(&code_addr, &code);
        void (*jitted_func)() = (decltype(jitted_func))code_addr;

        jitted_func();
    };

    static FTexture2DRHIRef out{};
    static FTexture2DRHIRef shader_out{};

    const auto size = get_ui_texture_size();
    const auto stack_args = (uintptr_t*)(ctx.rsp + 0x20);

    SPDLOG_INFO("About to call the original!");
    
    if (!rtm->is_pre_texture_call_e8) {
        SPDLOG_INFO("Calling register version of texture create");

        if (rtm->is_using_texture_desc && rtm->is_version_greq_5_1) {
            if (ctx.r9 == 0 || IsBadReadPtr((void*)ctx.r9, sizeof(void*))) {
                SPDLOG_INFO("Possible UE 5.0.3 detected, not 5.1 or above");
                rtm->is_using_texture_desc = false;
                rtm->is_version_5_0_3 = true;
                rtm->is_version_greq_5_1;
            }
        }

        if (rtm->is_using_texture_desc && rtm->is_version_greq_5_1) {
            SPDLOG_INFO("Calling UE5 texture desc version of texture create");

            void (*func)(
                uintptr_t rhi,
                FTexture2DRHIRef* out,
                uintptr_t command_list,
                uintptr_t desc,
                uintptr_t stack_0, // Stack dummies in-case this is the wrong function
                uintptr_t stack_1,
                uintptr_t stack_2,
                uintptr_t stack_3,
                uintptr_t stack_4,
                uintptr_t stack_5,
                uintptr_t stack_6,
                uintptr_t stack_7,
                uintptr_t stack_8) = (decltype(func))func_ptr;

            // Scan for the render target width and height in the desc
            // and replace it with the desktop resolution (This is for the UI texture)
            const auto scan_x = VR::get()->get_hmd_width() * 2;
            const auto scan_y = VR::get()->get_hmd_height();

            std::optional<int32_t> width_offset{};
            std::optional<int32_t> height_offset{};

            int32_t old_width{};
            int32_t old_height{};

            for (auto i = 0; i < 0x100; ++i) {
                auto& x = *(int32_t*)(ctx.r9 + i);
                auto& y = *(int32_t*)(ctx.r9 + i + 4);

                if (x == scan_x && y == scan_y) {
                    SPDLOG_INFO("UE5: Found render target width and height at offset: {:x}", i);

                    width_offset = i;
                    height_offset = i + 4;

                    old_width = x;
                    old_height = y;

                    x = size.x;
                    y = size.y;

                    uint8_t* format = (uint8_t*)(ctx.r9 + width_offset.value() + 15);

                    // some games have 10 bit format
                    if (*format == 18) {
                        *format = 2; // PF_B8G8R8A8
                    }

                    break;
                }
            }

            func(ctx.rcx, &out, ctx.r8, ctx.r9,
                stack_args[0], stack_args[1], 
                stack_args[2], stack_args[3],
                stack_args[4],
                stack_args[5], stack_args[6],
                stack_args[7], stack_args[8]
            );

            if (width_offset && height_offset) {
                auto& x = *(int32_t*)(ctx.r9 + *width_offset);
                auto& y = *(int32_t*)(ctx.r9 + *height_offset);

                x = old_width;
                y = old_height;
            }

            if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.rdx;
            }
        } else if (rtm->is_using_texture_desc) { // extremely rare.
            SPDLOG_INFO("Calling UE4 texture desc version of texture create");

            void (*func)(
                uintptr_t rhi,
                uintptr_t desc,
                TRefCountPtr<IPooledRenderTarget>* out,
                uintptr_t name // wchar_t*
            ) = (decltype(func))func_ptr;

            // Scan for the render target width and height in the desc
            // and replace it with the desktop resolution (This is for the UI texture)
            const auto scan_x = VR::get()->get_hmd_width() * 2;
            const auto scan_y = VR::get()->get_hmd_height();

            std::optional<int32_t> width_offset{};
            std::optional<int32_t> height_offset{};

            int32_t old_width{};
            int32_t old_height{};

            for (auto i = 0; i < 0x100; ++i) {
                auto& x = *(int32_t*)(ctx.rdx + i);
                auto& y = *(int32_t*)(ctx.rdx + i + 4);

                if (x == scan_x && y == scan_y) {
                    SPDLOG_INFO("UE4: Found render target width and height at offset: {:x}", i);

                    width_offset = i;
                    height_offset = i + 4;

                    old_width = x;
                    old_height = y;

                    x = size.x;
                    y = size.y;
                    break;
                }
            }

            static TRefCountPtr<IPooledRenderTarget> real_out{};

            func(ctx.rcx, ctx.rdx, &real_out, ctx.r9);

            if (real_out.reference != nullptr) {
                const auto& tex = real_out.reference->item.texture;
                const auto& shader = real_out.reference->item.srt;
                out.texture = tex.texture;
                shader_out.texture = shader.texture;
            }

            if (width_offset && height_offset) {
                auto& x = *(int32_t*)(ctx.rdx + *width_offset);
                auto& y = *(int32_t*)(ctx.rdx + *height_offset);

                x = old_width;
                y = old_height;
            }

            if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.r8;
            }
        } else { // most common version.
            SPDLOG_INFO("Calling common version of texture create (several arguments)");

            void (*func)(
                uintptr_t rhi,
                FTexture2DRHIRef* out,
                uintptr_t command_list,
                uintptr_t w,
                uintptr_t h,
                uintptr_t format,
                uintptr_t mips,
                uintptr_t samples,
                uintptr_t flags,
                uintptr_t create_info,
                uintptr_t additional,
                uintptr_t additional2) = (decltype(func))func_ptr;

            func(ctx.rcx, &out, ctx.r8, size.x, size.y, 2, 
                stack_args[2], stack_args[3], stack_args[4], 
                stack_args[5], stack_args[6], stack_args[7]);

            if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.rdx;
            }
        }

        rtm->ui_target = out.texture;
    } else {
        SPDLOG_INFO("Calling E8 version of texture create");
        
        // check if RCX is near the stack pointer
        // if it is then it's a different form of E8 call that takes the texture in the first parameter.
        if (ctx.rcx != 0 && std::abs((int64_t)ctx.rcx - (int64_t)ctx.rsp) <= 0x300) {
            SPDLOG_INFO("Weird form of E8 call detected...");

            // RDX check is to make sure RDX is a pointer and not something like the width which would be a relatively small integer
            if (rtm->is_using_texture_desc && rtm->is_version_greq_5_1 && ctx.rdx >= 65535) {
                SPDLOG_INFO("Calling UE5 texture desc version of texture create");

                void (*func)(
                    FTexture2DRHIRef* out,
                    uintptr_t desc,
                    uintptr_t r8,
                    uintptr_t r9
                ) = (decltype(func))func_ptr;

                // Scan for the render target width and height in the desc
                // and replace it with the desktop resolution (This is for the UI texture)
                const auto scan_x = VR::get()->get_hmd_width() * 2;
                const auto scan_y = VR::get()->get_hmd_height();

                std::optional<int32_t> width_offset{};
                std::optional<int32_t> height_offset{};

                int32_t old_width{};
                int32_t old_height{};

                for (auto i = 0; i < 0x100; ++i) {
                    auto& x = *(int32_t*)(ctx.rdx + i);
                    auto& y = *(int32_t*)(ctx.rdx + i + 4);

                    if (x == scan_x && y == scan_y) {
                        SPDLOG_INFO("UE5: Found render target width and height at offset: {:x}", i);

                        width_offset = i;
                        height_offset = i + 4;

                        old_width = x;
                        old_height = y;

                        x = size.x;
                        y = size.y;
                        break;
                    }
                }

                func(&out, ctx.rdx, ctx.r8, ctx.r9);

                if (width_offset && height_offset) {
                    auto& x = *(int32_t*)(ctx.rdx + *width_offset);
                    auto& y = *(int32_t*)(ctx.rdx + *height_offset);

                    x = old_width;
                    y = old_height;
                }

                if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                    SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                    rtm->texture_hook_ref = (FTexture2DRHIRef*)ctx.rcx;
                }
            } else {
                // Format
                ctx.r9 = 2; // PF_B8G8R8A8

                void (*func)(
                    FTexture2DRHIRef* out,
                    uint32_t w,
                    uint32_t h,
                    uint8_t format,
                    uintptr_t mips,
                    uintptr_t samples,
                    uintptr_t flags,
                    uintptr_t a7,
                    uintptr_t a8,
                    uintptr_t a9,
                    uintptr_t additional,
                    uintptr_t additional2) = (decltype(func))func_ptr;

                func(&out, (uint32_t)size.x, (uint32_t)size.y, 2,
                    stack_args[0], stack_args[1], 
                    stack_args[2], stack_args[3],
                    stack_args[4],
                    stack_args[7], stack_args[8], stack_args[9]);
            }
        } else {
            ctx.r8 = 2; // PF_B8G8R8A8

            std::optional<int> previous_stack_found_index{};
            std::optional<int> previous_stack_repeating_index{};

            std::optional<int> texture_argument_index{};
            std::optional<int> shader_argument_index{};

            for (auto i = 0; i < 10; ++i) {
                const auto stack_ptr = stack_args[i];

                if (std::abs((int64_t)stack_ptr - (int64_t)ctx.rsp) <= 0x300) {
                    if (previous_stack_found_index && *previous_stack_found_index == i - 1) {
                        previous_stack_repeating_index = i;
                    }

                    previous_stack_found_index = i;
                    SPDLOG_INFO("Stack pointer found at arg index {} ({} stack)", i + 4, i);
                } else if (previous_stack_repeating_index && *previous_stack_repeating_index == i - 1) {
                    texture_argument_index = i - 2;
                    shader_argument_index = i - 1;
                    SPDLOG_INFO("Texture argument may be at index {} ({} stack)", *texture_argument_index + 4, *texture_argument_index);
                    SPDLOG_INFO("Shader argument may be at index {} ({} stack)", *shader_argument_index + 4, *shader_argument_index);
                    break;
                }
            }

            if (!texture_argument_index && !shader_argument_index) {
                // operate on a wild guess (hardcoded function signature)
                SPDLOG_INFO("Calling E8 version of texture create with hardcoded function signature");

                void (*func)(
                    uint32_t w,
                    uint32_t h,
                    uint8_t format,
                    uintptr_t mips,
                    uintptr_t samples,
                    uintptr_t flags,
                    uintptr_t a7,
                    uintptr_t a8,
                    uintptr_t a9,
                    FTexture2DRHIRef* out,
                    FTexture2DRHIRef* shader_out,
                    uintptr_t additional,
                    uintptr_t additional2) = (decltype(func))func_ptr;

                func((uint32_t)size.x, (uint32_t)size.y, 2, ctx.r9,
                    stack_args[0], stack_args[1], 
                    stack_args[2], stack_args[3],
                    stack_args[4],
                    &out, &shader_out,
                    stack_args[7], stack_args[8]);
            } else {
                // dynamically generate the function call
                SPDLOG_INFO("Calling E8 version of texture create with dynamically generated function signature");

                void (*func)(
                    uint32_t w,
                    uint32_t h,
                    uint8_t format,
                    uintptr_t mips,
                    uintptr_t stack_0,
                    uintptr_t stack_1,
                    uintptr_t stack_2,
                    uintptr_t stack_3,
                    uintptr_t stack_4,
                    uintptr_t stack_5,
                    uintptr_t stack_6,
                    uintptr_t stack_7,
                    uintptr_t stack_8) = (decltype(func))func_ptr;

                std::array<uintptr_t, 9> cloned_stack{};
                for (auto i = 0; i < 9; ++i) {
                    cloned_stack[i] = stack_args[i];
                }

                cloned_stack[*texture_argument_index] = (uintptr_t)&out;
                cloned_stack[*shader_argument_index] = (uintptr_t)&shader_out;

                func((uint32_t)size.x, (uint32_t)size.y, 2, ctx.r9,
                    cloned_stack[0], cloned_stack[1], 
                    cloned_stack[2], cloned_stack[3],
                    cloned_stack[4],
                    cloned_stack[5], cloned_stack[6],
                    cloned_stack[7], cloned_stack[8]);

                if (rtm->texture_hook_ref == nullptr || rtm->texture_hook_ref->texture == nullptr) {
                    SPDLOG_INFO("Had to set texture hook ref in pre texture hook!");
                    rtm->texture_hook_ref = (FTexture2DRHIRef*)stack_args[*texture_argument_index];
                }
            }
        }

        rtm->ui_target = out.texture;
    }

    if (out.texture == nullptr) {
        SPDLOG_ERROR("Failed to create UI texture!");
    } else {
        SPDLOG_INFO("Created UI texture at {:x}", (uintptr_t)out.texture);
    }

    //call_with_context((uintptr_t)func, out);

    SPDLOG_INFO("Called the original function!");

    // Cause stuff like the VR ui texture to get recreated.
    VR::get()->reinitialize_renderer();
}

void VRRenderTargetManager_Base::texture_hook_callback(safetyhook::Context& ctx, bool from_second) {
    auto rtm = g_hook->get_render_target_manager();

    SPDLOG_INFO("Post texture hook called!");
    SPDLOG_INFO(" Ref: {:x}", (uintptr_t)rtm->texture_hook_ref);

    if (!rtm->allocate_texture_called) {
        g_hook->set_should_recreate_textures(true);
        rtm->render_target = nullptr;
        rtm->ui_target = nullptr;
        rtm->texture_hook_ref = nullptr;

        SPDLOG_INFO("[Post texture hook] Allocate texture was not called, skipping...");
        return;
    }

    rtm->allocate_texture_called = false;

    // very rare...
    if (rtm->is_using_texture_desc && !rtm->is_version_greq_5_1) {
        const auto pooled_rt_container = (TRefCountPtr<IPooledRenderTarget>*)rtm->texture_hook_ref;

        if (pooled_rt_container != nullptr && pooled_rt_container->reference != nullptr) {
            rtm->texture_hook_ref = &pooled_rt_container->reference->item.texture;
        }
    }

    FRHITexture2D* texture = nullptr;

    if (rtm->texture_hook_ref != nullptr) {
        texture = rtm->texture_hook_ref->texture;

        // happens?
        if (texture == nullptr) {
            SPDLOG_INFO(" Texture is null, trying to get it from RAX...");

            const auto ref = (FTexture2DRHIRef*)ctx.rax;

            if (!IsBadReadPtr(ref, sizeof(void*)) && !IsBadReadPtr(ref->texture, sizeof(void*))) {
                texture = ref->texture;
            } else {
                SPDLOG_ERROR(" RAX is bad! Can't get texture!");
            }
        }

        if (texture != nullptr) {
            SPDLOG_INFO(" Resulting texture: {:x}", (uintptr_t)texture);
            SPDLOG_INFO(" Real resource: {:x}", (uintptr_t)texture->get_native_resource());
            
            FRHITexture2D::set_vtable(*(void**)texture);
        } else {
            SPDLOG_INFO(" Texture is still null!");
        }
    }

    SPDLOG_INFO(" last texture index: {}", rtm->last_texture_index);

    rtm->render_target = texture;
    //rtm->ui_target = texture;
    rtm->texture_hook_ref = nullptr;
    ++rtm->last_texture_index;
}

void VRRenderTargetManager_Base::destroy_scene_capture() try {
    if (this->scene_capture_actor != nullptr && this->in_flight_target == nullptr) {
        SPDLOG_INFO("Destroying scene capture!");

        if (this->scene_capture_actor.valid()) {
            this->scene_capture_actor->destroy_actor();
        }
    }

    if (this->in_flight_target == nullptr) {
        this->scene_capture_actor = nullptr;
        this->scene_capture_component = nullptr;
        this->scene_capture_target = nullptr;

        RHIThreadWorker::get().enqueue([this]() -> void {
            this->scene_capture_target_rhi_thread = nullptr;
        });
    }
} catch (const std::exception& e) {
    SPDLOG_ERROR("[VRRenderTargetManager] Exception in destroy_scene_capture: {}", e.what());
    this->scene_capture_target = nullptr;
    this->scene_capture_actor = nullptr;
    this->scene_capture_component = nullptr;
    
    RHIThreadWorker::get().enqueue([this]() -> void {
        this->scene_capture_target_rhi_thread = nullptr;
    });
} catch (...) {
    SPDLOG_ERROR("[VRRenderTargetManager] Unknown exception in destroy_scene_capture!");
}

FRHITexture2D* VRRenderTargetManager_Base::get_scene_capture_render_target() {
    if (this->in_flight_target != nullptr) {
        return nullptr;
    }

    const auto is_same_as_rhi_thread = RHIThreadWorker::get().is_same_thread();
    const auto& sct = is_same_as_rhi_thread ? this->scene_capture_target_rhi_thread : this->scene_capture_target;

    if (sct != nullptr) try {
        // I REALLY don't want to lock a mutex in a hot path so let's hope that our exception handler catches everything.
        if (!sct.valid()) {
            SPDLOG_WARN("[VRRenderTargetManager] Scene capture target is not a UTexture! Texture probably deleted on level change!");
            
            if (is_same_as_rhi_thread) {
                this->scene_capture_target_rhi_thread = nullptr;
            }

            return nullptr;
        }

        auto rsrc = (sdk::FTextureRenderTargetResource*)sct->get_resource();
        auto rsrc_frt = rsrc != nullptr ? rsrc->as_render_target() : nullptr;

        if (rsrc_frt != nullptr) {  
            auto tex_ref = rsrc_frt->get_render_target_texture();
            if (tex_ref != nullptr) {
                return *tex_ref;
            }
        }
    } catch (...) {
        SPDLOG_ERROR("[VRRenderTargetManager] Exception in get_scene_capture_render_target! Texture probably deleted on level change!");

        if (is_same_as_rhi_thread) {
            this->scene_capture_target_rhi_thread = nullptr;
        }
    }

    return nullptr;
}

sdk::UTexture* VRRenderTargetManager_Base::get_scene_capture_utexture() {
    if (this->in_flight_target != nullptr) {
        return nullptr;
    }

    const auto& utex = this->scene_capture_target;

    if (utex != nullptr) try {
        if (utex.valid()) {
            return (sdk::UTexture*)utex;
        }

        SPDLOG_WARN("[VRRenderTargetManager] Scene capture target is not a UTexture! Texture probably deleted on level change!");

        GameThreadWorker::get().enqueue([this]() -> void {
            this->in_flight_target = nullptr;
            this->destroy_scene_capture();
        });
    } catch (...) {
        SPDLOG_ERROR("[VRRenderTargetManager] Exception in get_scene_capture_utexture! Texture probably deleted on level change!");

        GameThreadWorker::get().enqueue([this]() -> void {
            this->in_flight_target = nullptr;
            this->destroy_scene_capture();
        });
    }

    return nullptr;
}

bool VRRenderTargetManager_Base::create_scene_capture() try {
    if (this->in_flight_target != nullptr) {
        return false;
    }

    // This is necessary for offset calculations to succeed.
    if (FRHITexture2D::get_vtable() == nullptr) {
        SPDLOG_WARN("[VRRenderTargetManager] FRHITexture2D vtable is null, waiting for it to be set!");
        return false;
    }

    destroy_scene_capture();

    SPDLOG_INFO("Creating scene capture!");

    auto kismet_rendering = sdk::UKismetRenderingLibrary::get();

    if (kismet_rendering == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get UKismetRenderingLibrary!");
        return false;
    }

    static auto scene_capture_c = sdk::USceneCaptureComponent2D::static_class();

    if (scene_capture_c == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get USceneCaptureComponent2D class!");
        return false;
    }

    auto ugs = sdk::UGameplayStatics::get();

    if (ugs == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get UGameplayStatics!");
        return false;
    }

    auto engine = sdk::UGameEngine::get();

    if (engine == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get UGameEngine!");
        return false;
    }

    auto world = engine->get_world();

    if (world == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get UWorld!");
        return false;
    }

    static auto actor_c = sdk::AActor::static_class();

    if (actor_c == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to get AActor class!");
        return false;
    }

    this->scene_capture_actor = ugs->spawn_actor(world, actor_c, glm::vec3{0, 0, 0});

    if (this->scene_capture_actor == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to spawn actor!");
        return false;
    }

    this->scene_capture_component = (sdk::USceneCaptureComponent2D*)this->scene_capture_actor->add_component_by_class(scene_capture_c, false);

    if (this->scene_capture_component == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to add scene capture component!");
        return false;
    }

    const float clear_color[4] {0.0f, 0.0f, 0.0f, 1.0f};
    auto tgt_raw = kismet_rendering->create_render_target_2d(world, VR::get()->get_hmd_width(), VR::get()->get_hmd_height(), 2, clear_color, false);

    if (tgt_raw == nullptr) {
        SPDLOG_ERROR("[VRRenderTargetManager] Failed to create texture!");
        return false;
    }

    sdk::UObjectReference tgt{tgt_raw};

    SPDLOG_INFO("[VRRenderTargetManager] Created texture target: {:x}", (uintptr_t)tgt.get());
    this->scene_capture_actor->finish_add_component(this->scene_capture_component);

    this->scene_capture_component->set_texture_target(tgt);

    // We don't actually want this to tick.
    // We are just using the property as a convenient way to keep the texture alive without crashing.
    this->scene_capture_component->set_visibility(false);
    if (auto capture_every_frame = scene_capture_c->find_property(L"bCaptureEveryFrame"); capture_every_frame != nullptr) {
        *capture_every_frame->get_data<bool>(this->scene_capture_component) = false;
    }

    static bool already_updated{false};
    static std::array<uintptr_t, 100> original_frender_target_vtable{};
    static auto gamma_increase_fn = +[](const sdk::FRenderTarget* frt) -> float {
        auto rtm = g_hook->get_render_target_manager();
        auto viewport = rtm != nullptr ? rtm->get_viewport() : nullptr;

        if (viewport != nullptr) {
            return viewport->get_display_gamma();
        }

        return 2.2f;
    };

    static auto hook_frt = [](sdk::FRenderTarget* frt) {
        if (frt == nullptr) {
            SPDLOG_WARN("[FRenderTarget] FRenderTarget is null! Can't hook!");
            return;
        }

        SPDLOG_INFO("[FRenderTarget] Hooking FRenderTarget!");

        auto& vtable = *(void**)frt;
        memcpy(original_frender_target_vtable.data(), vtable, original_frender_target_vtable.size() * sizeof(uintptr_t));

        if (auto display_gamma_index = sdk::FRenderTarget::get_display_gamma_index(); display_gamma_index != 0) {
            original_frender_target_vtable[*display_gamma_index] = (uintptr_t)gamma_increase_fn;
            vtable = original_frender_target_vtable.data();
            SPDLOG_INFO("[FRenderTarget] Hooked FRenderTarget!");
        } else {
            SPDLOG_WARN("[FRenderTarget] Gamma index not found, can't hook!");
        }
    };

    static const auto utex_c = sdk::UTexture::static_class();

    // Enqueue offset lookup on the render thread because that's when the resource is actually created.
    if (!already_updated) {
        this->in_flight_target = tgt;

        // Repeats every render loop for 5 seconds, times out if the texture is not created.
        RenderThreadWorker::ConditionalJobFunc render_thread_conditional_task = [this, tgt]() -> bool {
            try {
                if (!tgt.valid()) {
                    SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                    GameThreadWorker::get().enqueue([this]() -> void {
                        this->in_flight_target = nullptr;
                        destroy_scene_capture();
                    });
                    return true;
                }
    
                if (sdk::UTexture::update_render_resource_offset_texture2d(tgt)) {
                    SPDLOG_INFO("Successfully updated render resource offset for scene capture target!");
    
                    if (auto rsrc = (sdk::FTextureRenderTargetResource*)tgt->get_resource(); rsrc != nullptr) {
                        const bool success = sdk::FTextureRenderTargetResource::update_render_target_vtable_offset(rsrc);
                        const auto frt = success ? rsrc->as_render_target() : nullptr;
    
                        if (frt != nullptr) {
                            sdk::FRenderTarget::update_offsets(frt);

                            if (frt->get_render_target_texture() == nullptr || *frt->get_render_target_texture() == nullptr) {
                                SPDLOG_WARN("Waiting for render target texture to be valid...");
                                return false;
                            }
    
                            hook_frt(frt);
    
                            RHIThreadWorker::get().enqueue([this, tgt]() -> void {
                                if (!tgt.valid()) {
                                    SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                                    this->scene_capture_target_rhi_thread = nullptr;
                                    return;
                                }

                                this->scene_capture_target_rhi_thread = tgt;
                            });
                            
                            GameThreadWorker::get().enqueue([this, tgt]() -> void {
                                if (!tgt.valid()) {
                                    SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                                    this->in_flight_target = nullptr;
                                    destroy_scene_capture();
                                    return;
                                }
                                
                                this->scene_capture_target = tgt;
                                this->in_flight_target = nullptr;
    
                                SPDLOG_INFO("Scene capture texture created!");
                            });
    
                            already_updated = true;
        
                            return true;
                        }
    
                        SPDLOG_WARN("Waiting for render target to be valid...");
    
                        return false; // Keep waiting until it works.
                    }
                } else {
                    SPDLOG_ERROR("Failed to update render resource offset for scene capture target!");
                }
            } catch (const std::exception& e) {
                SPDLOG_ERROR("[VRRenderTargetManager] Exception in create_scene_capture (offset lookup): {}", e.what());
                GameThreadWorker::get().enqueue([this]() -> void {
                    this->in_flight_target = nullptr;
                    destroy_scene_capture();
                });
                return true;
            } catch (...) {
                SPDLOG_ERROR("[VRRenderTargetManager] Unknown exception in create_scene_capture (offset lookup)!");
                GameThreadWorker::get().enqueue([this]() -> void {
                    this->in_flight_target = nullptr;
                    destroy_scene_capture();
                });
                return true;
            }

            return false;
        };

        RenderThreadWorker::ConditionalJobTimeoutFunc render_thread_on_timeout = [this]() {
            SPDLOG_ERROR("Timed out waiting for scene capture texture to be created!");
            GameThreadWorker::get().enqueue([this]() -> void {
                this->in_flight_target = nullptr;
                destroy_scene_capture();
            });
        };

        RenderThreadWorker::get().enqueue_conditional(render_thread_conditional_task, render_thread_on_timeout, std::chrono::seconds(2));
    
        SPDLOG_INFO("Waiting for scene capture texture to be created...");
    } else {
        this->in_flight_target = tgt;

        RenderThreadWorker::ConditionalJobFunc render_thread_conditional_task = [this, tgt]() -> bool {
            try {
                if (!tgt.valid()) {
                    SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                    GameThreadWorker::get().enqueue([this]() -> void {
                        this->in_flight_target = nullptr;
                        destroy_scene_capture();
                    });
    
                    return true;
                }
    
                auto rsrc = (sdk::FTextureRenderTargetResource*)tgt->get_resource();
                auto frt = rsrc != nullptr ? rsrc->as_render_target() : nullptr;
                auto frttex = frt != nullptr ? frt->get_render_target_texture() : nullptr;
    
                // Wait until FRenderTarget is not null.
                if (frt == nullptr || frttex == nullptr || *frttex == nullptr) {
                    SPDLOG_WARN("Waiting for render target to be valid...");
                    return false;
                }
    
                hook_frt(frt);
    
                RHIThreadWorker::get().enqueue([this, tgt]() -> void {
                    if (!tgt.valid()) {
                        SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                        this->scene_capture_target_rhi_thread = nullptr;
                        return;
                    }

                    this->scene_capture_target_rhi_thread = tgt;
                });
    
                GameThreadWorker::get().enqueue([this, tgt]() -> void {
                    if (!tgt.valid()) {
                        SPDLOG_ERROR("Scene capture target was destroyed between threads!");
                        this->in_flight_target = nullptr;
                        destroy_scene_capture();
                        return;
                    }
    
                    this->in_flight_target = nullptr;
                    this->scene_capture_target = tgt;
    
                    SPDLOG_INFO("Scene capture texture fully created!");
                });
    
                return true;
            } catch (const std::exception& e) {
                SPDLOG_ERROR("[VRRenderTargetManager] Exception in create_scene_capture: {}", e.what());
                GameThreadWorker::get().enqueue([this]() -> void {
                    this->in_flight_target = nullptr;
                    destroy_scene_capture();
                });
                return true;
            } catch (...) {
                SPDLOG_ERROR("[VRRenderTargetManager] Unknown exception in create_scene_capture!");
                GameThreadWorker::get().enqueue([this]() -> void {
                    this->in_flight_target = nullptr;
                    destroy_scene_capture();
                });
                return true;
            }
        };

        RenderThreadWorker::ConditionalJobTimeoutFunc render_thread_on_timeout = [this]() {
            SPDLOG_ERROR("Timed out waiting for scene capture texture to be created!");
            GameThreadWorker::get().enqueue([this]() -> void {
                this->in_flight_target = nullptr;
                destroy_scene_capture();
            });
        };

        RenderThreadWorker::get().enqueue_conditional(render_thread_conditional_task, render_thread_on_timeout, std::chrono::seconds(2));

        SPDLOG_INFO("Waiting for scene capture texture to be created...");
    }

    return true;
} catch (const std::exception& e) {
    SPDLOG_ERROR("[VRRenderTargetManager] Exception in create_scene_capture: {}", e.what());
    return false;
} catch (...) {
    SPDLOG_ERROR("[VRRenderTargetManager] Unknown exception in create_scene_capture!");
    return false;
}

// This is a very special fix for cases where engine modifications
// can add a second call to UpdateViewportRHI right before the place we expect it to get called
// The fact that they get called back-to-back over and over causes huge performance problems
// because the viewport texture keeps getting recreated over and over.
// This hook attempts to only allow the last call to UpdateViewportRHI inside of EnqueueBeginRenderFrame to do anything
// Usually there's only one call to UpdateViewportRHI inside of EnqueueBeginRenderFrame, but (very rarely) there can be two.
__declspec(noinline) void FFakeStereoRenderingHook::update_viewport_rhi_hook(void* viewport, size_t destroyed, size_t new_size_x, size_t new_size_y, size_t new_window_mode, size_t preferred_pixel_format) {
    auto call_orig = [&]() {
        g_hook->m_update_viewport_rhi_hook->get_original<void(*)(void*, size_t, size_t, size_t, size_t, size_t)>()(viewport, destroyed, new_size_x, new_size_y, new_window_mode, preferred_pixel_format);
    };

    SPDLOG_INFO_ONCE("UpdateViewportRHI (embedded): {:x}", (uintptr_t)_ReturnAddress());

    const auto hmd_active = VR::get()->is_hmd_active();
    static bool modified_use_separate_rt = false;

    if (!hmd_active) {
        if (modified_use_separate_rt) {
            modified_use_separate_rt = false;

            const auto rtm = g_hook->get_render_target_manager();

            if (rtm != nullptr) {
                if (const auto offset = rtm->get_viewport_force_separate_rt_offset()) {
                    SPDLOG_INFO_ONCE("Resetting bUseSeparateRenderTarget to false!");
                    auto& use_separate_rt = *(bool*)((uintptr_t)viewport + (*offset - 1));
                    use_separate_rt = false;
                }
            }
        }

        call_orig();
        return;
    }

    struct FunctionInfo {
        std::vector<uintptr_t> return_addrs{}; // in order of call
        size_t count{0};
    };

    static std::mutex mtx{};
    static std::unordered_map<uintptr_t, uintptr_t> functions_within{};
    static std::unordered_map<uintptr_t, FunctionInfo> function_infos{};

    {
        std::scoped_lock _{mtx};

        const auto return_addr = (uintptr_t)_ReturnAddress();
        auto function_within = functions_within.find(return_addr);

        if (function_within == functions_within.end()) {
            const auto result = utility::find_virtual_function_start(return_addr);

            if (result) {
                functions_within[return_addr] = *result;
            } else {
                functions_within[return_addr] = 0;
            }

            function_within = functions_within.find(return_addr);

            if (function_within->second != 0) {
                ++function_infos[function_within->second].count;
            }

            function_infos[function_within->second].return_addrs.push_back(return_addr);

            SPDLOG_INFO("Added new call of UpdateViewportRHI to function {:x} (count: {})", function_within->second, function_infos[function_within->second].count);
        }

        if (function_within->second == 0) {
            SPDLOG_INFO_ONCE("Could not find vfunc start for call of UpdateViewportRHI, calling original.");
            call_orig();
            return;
        }

        const auto& function_info = function_infos[function_within->second]; 

        // We only care about corrections where UpdateViewportRHI is called more than once in the same function.
        if (function_info.count <= 1 || function_info.return_addrs.empty()) {
            call_orig();
            return;
        }

        if (!g_hook->m_rendertarget_manager_embedded_in_stereo_device) {
            const auto rtm = g_hook->get_render_target_manager();

            if (rtm != nullptr) {
                if (const auto offset = rtm->get_viewport_force_separate_rt_offset()) {
                    auto& should_force_separate_rt = *(bool*)((uintptr_t)viewport + *offset);
                    auto& use_separate_rt = *(bool*)((uintptr_t)viewport + (*offset - 1));

                    if (!should_force_separate_rt) {
                        SPDLOG_INFO_ONCE("UpdateViewportRHI was called without should_force_separate_rt being set to true, skipping.");
                        should_force_separate_rt = true;
                        use_separate_rt = true;
                        modified_use_separate_rt = true;
                        return; // NO!!!!!!!!!!!!!!!!!!!
                    }
                }
            }
        } else {      
            // We only want the last function to be called.
            // We don't need to call the original here because it will get called by the last function.
            // if we call the original here, it will cause performance issues.
            if (function_info.return_addrs.back() != return_addr) {
                return;
            }
        }
    }

    
    if (!g_hook->m_rendertarget_manager_embedded_in_stereo_device) {
        call_orig();
        return;
    }

    auto& rtm = g_hook->get_embedded_rtm();

    static std::chrono::steady_clock::time_point last_time_hmd_active{};
    static bool hmd_was_active = false;
    bool should_call_orig = false;

    if (hmd_active && !hmd_was_active) {
        last_time_hmd_active = std::chrono::steady_clock::now();
        hmd_was_active = true;
    } else if (!hmd_active) {
        hmd_was_active = false;
        should_call_orig = true;
    }

    if (hmd_active) {
        should_call_orig = std::chrono::steady_clock::now() - last_time_hmd_active <= std::chrono::milliseconds(2000);
        //should_call_orig = should_call_orig || (std::chrono::steady_clock::now() - rtm.last_time_needed_hmd_reallocate <= std::chrono::milliseconds(2000));
    }

    if (should_call_orig) {
        rtm.should_use_separate_rt_called = false;
        rtm.need_reallocate_viewport_render_target_called = false;
        call_orig();
        return;
    }

    if (!rtm.should_use_separate_rt_called) {
        SPDLOG_INFO_ONCE("Skipping UpdateViewportRHI (embedded) because ShouldUseSeparateRenderTarget() was not called!");
        return; // Do not call at all.
    }

    if (!rtm.need_reallocate_viewport_render_target_called) {
        const auto need_reallocate = g_hook->get_render_target_manager()->need_reallocate_view_target(*(sdk::FViewport*)viewport);

        if (!need_reallocate) {
            SPDLOG_INFO_ONCE("Skipping UpdateViewportRHI (embedded) because NeedReallocateViewportRenderTarget() was not called and we don't need to reallocate anyway!");
            rtm.should_use_separate_rt_called = false;
            return; // Do not call at all.
        }

        SPDLOG_INFO_ONCE("We need to reallocate the viewport render target even though NeedReallocateViewportRenderTarget() was not called!");
        //rtm.last_time_needed_hmd_reallocate = std::chrono::steady_clock::now();
    }

    call_orig();
    rtm.should_use_separate_rt_called = false;
    rtm.need_reallocate_viewport_render_target_called = false;
}

void FFakeStereoRenderingHook::attempt_hook_update_viewport_rhi(uintptr_t return_address) {
    if (/*!m_rendertarget_manager_embedded_in_stereo_device ||*/ m_special_detected || m_attempted_hook_update_viewport_rhi) {
        return;
    }

    m_attempted_hook_update_viewport_rhi = true;

    if (m_update_viewport_rhi_hook == nullptr) {
        SPDLOG_INFO("Attempting to hook UpdateViewportRHI...");

        const auto init_dynamic_rhi = utility::find_virtual_function_start(return_address);

        if (init_dynamic_rhi) {
            SPDLOG_INFO("Found InitDynamicRHI: {:x}", *init_dynamic_rhi);

            const auto init_dynamic_rhi_ptr = utility::scan_ptr(*utility::get_module_within(*init_dynamic_rhi), *init_dynamic_rhi);
            if (!init_dynamic_rhi_ptr) {
                SPDLOG_ERROR("Failed to find InitDynamicRHI pointer!");
                return;
            }

            const auto update_viewport_rhi_ptr = *init_dynamic_rhi_ptr - (sizeof(void*) * 2);

            if (*(void**)update_viewport_rhi_ptr == nullptr || IsBadReadPtr(*(void**)update_viewport_rhi_ptr, sizeof(void*))) {
                SPDLOG_ERROR("Failed to find UpdateViewportRHI!");
                return;
            }

            // Make sure this is no displacement reference to this. This can mean we accidentally found the vtable for IViewportRenderTargetProvider
            // The vfunc pointer should be in the middle of the vtable, not the start.
            if (utility::scan_displacement_reference(*utility::get_module_within(*init_dynamic_rhi), update_viewport_rhi_ptr)) {
                SPDLOG_ERROR("Found displacement reference to UpdateViewportRHI, this is probably the vtable for IViewportRenderTargetProvider, aborting!");
                return;
            }

            m_update_viewport_rhi_hook = std::make_unique<PointerHook>((void**)update_viewport_rhi_ptr, &update_viewport_rhi_hook);
        } else {
            SPDLOG_ERROR("Failed to find InitDynamicRHI, cannot hook UpdateViewportRHI!");
        }
    }
}

bool VRRenderTargetManager_Base::allocate_render_target_texture(uintptr_t return_address, FTexture2DRHIRef* tex, FTexture2DRHIRef* shader_resource) {
    this->texture_hook_ref = tex;
    this->shader_resource_hook_ref = shader_resource;
    this->allocate_texture_called = true;

    if (!this->set_up_texture_hook) {
        ZoneScopedN("VRRenderTargetManager_Base::allocate_render_target_texture initialization");
        SPDLOG_INFO("AllocateRenderTargetTexture retaddr: {:x}", return_address);

        g_hook->attempt_hook_update_viewport_rhi(return_address);

        SPDLOG_INFO("Scanning for call instr...");

        bool next_call_is_not_the_right_one = false;

        auto is_string_nearby = [](uintptr_t addr, std::wstring_view str) {
            const auto addr_module = utility::get_module_within(addr);
            if (!addr_module) {
                return false;
            }

            const auto module_size = utility::get_module_size(*addr_module);
            const auto module_end = (uintptr_t)*addr_module + *module_size - 0x1000;

            // Find all possible strings, not just the first one
            for (auto str_addr = utility::scan_string(*addr_module, str.data(), true); 
                str_addr.has_value(); 
                str_addr = utility::scan_string(*str_addr + 1, (module_end - (*str_addr + 1)), str.data(), true)) 
            {
                // Scan for ALL references to this string
                for (auto string_ref = utility::scan_displacement_reference(*addr_module, (uintptr_t)*str_addr);
                    string_ref.has_value();
                    string_ref = utility::scan_displacement_reference(*string_ref + 1, (module_end - (*string_ref + 1)), (uintptr_t)*str_addr))
                {
                    const auto string_ref_func_start = utility::find_function_start((uintptr_t)*string_ref);
                    const auto return_addr_func_start = utility::find_function_start(addr);

                    SPDLOG_INFO("String ref func start: {:x}", (uintptr_t)*string_ref_func_start);
                    SPDLOG_INFO("Return addr func start: {:x}", (uintptr_t)*return_addr_func_start);

                    if (string_ref_func_start && return_addr_func_start && *string_ref_func_start == *return_addr_func_start) {
                        return true;
                    }
                }
            }

            return false;
        };

        // This string is present in UE5 (>= 5.1) and used when using texture descriptors to create textures.
        // that means this is UE5 and the function will take a texture descriptor instead of a bunch of arguments.
        if (is_string_nearby(return_address, L"BufferedRT")) {
            SPDLOG_INFO("Found string ref for BufferedRT, this is UE5!");
            this->is_using_texture_desc = true;
            this->is_version_greq_5_1 = true;
        }

        // Present in a specific game or game(s), somewhere around 4.8-4.12 (?)
        // indicates that texture descriptors are being used.
        if (is_string_nearby(return_address, L"SceneViewBuffer")) {
            SPDLOG_INFO("Found string ref for SceneViewBuffer, texture descriptors are being used!");
            this->is_using_texture_desc = true;
            this->is_version_greq_5_1 = false;

            next_call_is_not_the_right_one = true; // not seen a case where this isn't true (yet)
        }

        // Now, we need to emulate from where AllocateRenderTargetTexture returns from
        // we will set RAX to false, to get the control flow correct
        // and then keep emulating until we hit the call we want
        // Previously, we were using just straight linear disassembly to do this, and it mostly worked
        // but in one game, there was an unconditional branch after the call instead of flowing
        // directly into the next instruction.
        auto emu = utility::ShemuContext{*utility::get_module_within(return_address)};

        emu.ctx->Registers.RegRax = 0;
        emu.ctx->Registers.RegRip = (ND_UINT64)return_address;
        emu.ctx->MemThreshold = 100;

        const std::vector<std::string> bad_patterns_before_call = {
            "B2 32", // mov dl, 32h, (seen in UE5 debug/dev builds)
            "B2 2A", // mov dl, 2Ah, (seen in UE4.23 debug/dev builds)
            "B2 2B", // mov dl, 2Bh, (seen in UE4.25 debug/dev builds)
            "BA 2F 00 00 00", // mov edx, 2Fh (seen in UE5 debug/dev builds)
            "F6 85 ? ? ? ? 05", // test byte ptr [rbp+?], 5 (seen in UE5 debug/dev builds)
        };

        while(true) {
            if (emu.ctx->InstructionsCount > 200) {
                SPDLOG_WARN("Emulated too many instructions without finding the call, aborting!");
                break;
            }

            const auto ip = emu.ctx->Registers.RegRip;
            const auto bytes = (uint8_t*)ip;
            const auto decoded = utility::decode_one((uint8_t*)ip);

            if (ip != 0) {
                for (const auto& pattern : bad_patterns_before_call) {
                    if (utility::scan(ip, 100, pattern).value_or(0) == ip) {
                        SPDLOG_INFO("Found bad pattern before call, skipping next call: {:x} ({})", ip, pattern);
                        next_call_is_not_the_right_one = true;
                        break;
                    }
                }
            }
            
            if (!next_call_is_not_the_right_one) try {
                const auto addr = utility::resolve_displacement(ip);

                if (addr && !IsBadReadPtr((void*)*addr, 12)) {
                    if (std::wstring_view{(const wchar_t*)*addr}.starts_with(L"BufferedRT")) {
                        this->is_using_texture_desc = true;
                        this->is_version_greq_5_1 = true;

                        SPDLOG_INFO("Found usage of string \"BufferedRT\" while analyzing AllocateRenderTargetTexture!");
                    } else if (std::string_view{(const char*)*addr}.starts_with("IsInRenderingThread") && std::string_view{decoded->Mnemonic}.starts_with("LEA") && decoded->Operands[0].Type == ND_OP_REG && decoded->Operands[0].Info.Register.Reg == NDR_RCX) {
                        SPDLOG_INFO("Found usage of string \"IsInRenderingThread\" while analyzing AllocateRenderTargetTexture, skipping next call!");
                        next_call_is_not_the_right_one = true;
                    }
                }
            } catch(...) {

            }

            // make sure we are not emulating any instructions that write to memory
            // so we can just set the IP to the next instruction
            if (decoded) {
                const auto is_call = std::string_view{decoded->Mnemonic}.starts_with("CALL");

                if (decoded->MemoryAccess & ND_ACCESS_ANY_WRITE || is_call) {
                    // We are looking for the call instruction
                    // This instruction calls RHICreateTargetableShaderResource2D(TexSizeX, TexSizeY, SceneTargetFormat, 1, TexCreate_None,
                    // TexCreate_RenderTargetable, false, CreateInfo, BufferedRTRHI, BufferedSRVRHI); Which sets up the BufferedRTRHI and
                    // BufferedSRVRHI variables.
                    if (is_call && !next_call_is_not_the_right_one && bytes[0] == 0xE8) try {
                        // Analyze some of the instructions inside the call first
                        // If it has a mov eax, 0x800, then returns, we can skip this function
                        const auto fn = utility::calculate_absolute(ip + 1);
                        SPDLOG_INFO("Analyzing call at {:x} to {:x}", ip, fn);

                        if (auto result = utility::scan(fn, 10, "41 B8 30 00 00 00"); result.has_value() && *result == fn) {
                            SPDLOG_INFO("First instruction is a mov r8d, 30h, skipping this call!");
                            next_call_is_not_the_right_one = true;
                        } else if (auto result = utility::scan(fn, 50, "B8 00 08 00 00 C3"); result.has_value()) {
                            SPDLOG_INFO("First few instructions are a mov eax, 800h, ret, skipping this call!");
                            next_call_is_not_the_right_one = true;
                        } else if (this->is_version_greq_5_1) { // Limiting the scope of this to newer UE5 versions so we don't potentially break older versions
                            const auto module_fn_within = utility::get_module_within(fn);
                            const auto next_insn = (uint8_t*)(ip + decoded->Length);

                            // Seen on UE5.3.2 development builds
                            if (auto result = utility::scan_disasm(fn, 15, "BD 01 00 00 00"); result.has_value()) {
                                // This string is not unicode
                                if (utility::find_string_reference_in_path(fn, "InGPUMask != 0", false).has_value()) {
                                    SPDLOG_INFO("Found InGPUMask != 0 string within the function and mov ebp, 1, skipping this call!");
                                    next_call_is_not_the_right_one = true;
                                }
                            } else if (next_insn[0] == 0x84 && next_insn[1] == 0xC0) { // test al, al
                                if (auto ref = utility::find_string_reference_in_path((uintptr_t)next_insn, "IsInRenderingThread()", false); ref.has_value()) {
                                    if (ref->addr > (uintptr_t)next_insn && ref->addr - (uintptr_t)next_insn < 30) {
                                        SPDLOG_INFO("Found IsInRenderingThread() instead of the function we want, skipping this call!");
                                        next_call_is_not_the_right_one = true;
                                    }
                                }
                            } else if (utility::find_pattern_in_path((uint8_t*)fn, 30, true, "66 41 C7 40 34 00 FF")) {
                                SPDLOG_INFO("Found 66 41 C7 40 34 00 FF pattern within the function, skipping this call!");
                                next_call_is_not_the_right_one = true;
                            } else {
                                // Check how many instructions are in the call. If there's <= 30 AND there's no call/jmp in it, this is not the right one
                                size_t insn_count = 0;
                                bool encountered_branch = false;
                                utility::exhaustive_decode((uint8_t*)fn, 200, [&](const utility::ExhaustionContext& ctx) -> utility::ExhaustionResult {
                                    if (std::string_view{ctx.instrux.Mnemonic}.starts_with("CALL") || std::string_view{ctx.instrux.Mnemonic}.starts_with("JMP")) {
                                        encountered_branch = true;
                                        return utility::ExhaustionResult::BREAK;
                                    }

                                    return utility::ExhaustionResult::CONTINUE;
                                });

                                if (insn_count <= 30 && !encountered_branch) {
                                    SPDLOG_INFO("Function at {:x} only has {} instructions and no calls/branches, skipping this call!", fn, insn_count);
                                    next_call_is_not_the_right_one = true;
                                }
                            }
                        }
                    } catch(...) {
                        SPDLOG_INFO("Failed to analyze call at {:x}", ip);
                    }

                    if (is_call && !next_call_is_not_the_right_one && bytes[0] == 0xFF && bytes[1] == 0x15) {
                        // well this definitely is not the right one, indirect calls have never called the function we wanted (I think)
                        SPDLOG_INFO("Found indirect call @ {:x}, skipping", ip);
                        next_call_is_not_the_right_one = true;
                    }

                    if (is_call && !next_call_is_not_the_right_one) {
                        const auto post_call = (uintptr_t)ip + decoded->Length;
                        SPDLOG_INFO("AllocateRenderTargetTexture post_call: {:x}, rel {:x}", post_call, post_call - (uintptr_t)*utility::get_module_within((void*)post_call));

                        if (*(uint8_t*)ip == 0xE8) {
                            SPDLOG_INFO("E8 call found!");
                            this->is_pre_texture_call_e8 = true;
                        } else {
                            SPDLOG_INFO("E8 call not found, assuming register call!");
                        }

                        // So we can call the original texture create function again.
                        this->texture_create_insn_bytes.resize(decoded->Length);
                        memcpy(this->texture_create_insn_bytes.data(), (void*)ip, decoded->Length);

                        if (this->is_version_greq_5_1 && !this->is_pre_texture_call_e8 && bytes[-7] == 0x48 && bytes[-6] == 0x8B && bytes[-5] == 0x0D && bytes[0] == 0xFF && bytes[1] == 0x94) {
                            // Scan forward for a similar one and also hook that
                            auto second_call = utility::scan((uintptr_t)ip + decoded->Length, 0x60, "48 8B 0D ? ? ? ? FF 94 ? ? ? ? ?");

                            if (second_call) {
                                // So we can call the original texture create function again.
                                this->texture_create_insn_bytes2.resize(decoded->Length);
                                memcpy(this->texture_create_insn_bytes2.data(), (void*)(*second_call + 7), decoded->Length);

                                SPDLOG_INFO("Found second call at {:x}", *second_call);
                                auto post_second_call = *second_call + 7 + decoded->Length;
                                //auto texture_hook_result = safetyhook::MidHook::create((void*)post_second_call, &VRRenderTargetManager::texture_hook_callback);
                                auto texture_hook_result = safetyhook::MidHook::create((void*)post_second_call, +[](safetyhook::Context& ctx) -> void {
                                    VRRenderTargetManager::texture_hook_callback(ctx, true);
                                });

                                if (!texture_hook_result.has_value()) {
                                    const auto e = texture_hook_result.error();

                                    if (e.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
                                        SPDLOG_ERROR("Failed to create post second texture hook: BAD_ALLOCATION: {}", (uint8_t)e.allocator_error);
                                    } else {
                                        SPDLOG_ERROR("Failed to create post second texture hook: BAD_INLINE_HOOK: {}", (uint8_t)e.inline_hook_error.type);
                                    }
                                } else {
                                    this->texture_hook2 = std::move(texture_hook_result.value());
                                    SPDLOG_INFO("Successfully created second texture hook!");
                                }

                                auto pre_second_call = *second_call + 7;
                                //auto pre_texure_hook_result = safetyhook::MidHook::create((void*)pre_second_call, &VRRenderTargetManager::pre_texture_hook_callback);
                                auto pre_texure_hook_result = safetyhook::MidHook::create((void*)pre_second_call, +[](safetyhook::Context& ctx) -> void {
                                    VRRenderTargetManager::pre_texture_hook_callback(ctx, true);
                                });

                                if (!pre_texure_hook_result.has_value()) {
                                    const auto e = pre_texure_hook_result.error();

                                    if (e.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
                                        SPDLOG_ERROR("Failed to create pre second texture hook: BAD_ALLOCATION: {}", (uint8_t)e.allocator_error);
                                    } else {
                                        SPDLOG_ERROR("Failed to create pre second texture hook: BAD_INLINE_HOOK: {}", (uint8_t)e.inline_hook_error.type);
                                    }
                                } else {
                                    this->pre_texture_hook2 = std::move(pre_texure_hook_result.value());
                                    SPDLOG_INFO("Successfully created second pre texture hook!");
                                }
                            } else {
                                SPDLOG_INFO("Second call not detected! Continuing...");
                            }
                        }

                        //auto texture_hook_result = safetyhook::MidHook::create((void*)post_call, &VRRenderTargetManager::texture_hook_callback);
                        auto texture_hook_result = safetyhook::MidHook::create((void*)post_call, +[](safetyhook::Context& ctx) -> void {
                            VRRenderTargetManager::texture_hook_callback(ctx, false);
                        });

                        if (!texture_hook_result.has_value()) {
                            const auto e = texture_hook_result.error();

                            if (e.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
                                SPDLOG_ERROR("Failed to create post texture hook: BAD_ALLOCATION: {}", (uint8_t)e.allocator_error);
                            } else {
                                SPDLOG_ERROR("Failed to create post texture hook: BAD_INLINE_HOOK: {}", (uint8_t)e.inline_hook_error.type);
                            }
                        } else {
                            this->texture_hook = std::move(texture_hook_result.value());
                        }

                        //auto pre_texure_hook_result = safetyhook::MidHook::create((void*)ip, &VRRenderTargetManager::pre_texture_hook_callback);
                        auto pre_texure_hook_result = safetyhook::MidHook::create((void*)ip, +[](safetyhook::Context& ctx) -> void {
                            VRRenderTargetManager::pre_texture_hook_callback(ctx, false);
                        });

                        if (!pre_texure_hook_result.has_value()) {
                            const auto e = pre_texure_hook_result.error();

                            if (e.type == safetyhook::MidHook::Error::BAD_ALLOCATION) {
                                SPDLOG_ERROR("Failed to create pre texture hook: BAD_ALLOCATION: {}", (uint8_t)e.allocator_error);
                            } else {
                                SPDLOG_ERROR("Failed to create pre texture hook: BAD_INLINE_HOOK: {}", (uint8_t)e.inline_hook_error.type);
                            }
                        } else {
                            this->pre_texture_hook = std::move(pre_texure_hook_result.value());
                        }
                        this->set_up_texture_hook = true;

                        return false;
                    }

                    SPDLOG_INFO("Skipping write to memory instruction at {:x} ({:x} bytes, landing at {:x})", ip, decoded->Length, ip + decoded->Length);
                    emu.ctx->Registers.RegRip += decoded->Length;
                    emu.ctx->Instruction = *decoded; // pseudo-emulate the instruction
                    ++emu.ctx->InstructionsCount;

                    if (is_call) {
                        next_call_is_not_the_right_one = false;
                    }
                } else if (emu.emulate() != SHEMU_SUCCESS) { // only emulate the non-memory write instructions
                    SPDLOG_INFO("Emulation failed at {:x} ({:x} bytes, landing at {:x})", ip, decoded->Length, ip + decoded->Length);
                    // instead of just adding it onto the RegRip, we need to use the ip we had previously from the decode
                    // because the emulator can move the instruction pointer after emulate() is called
                    emu.ctx->Registers.RegRip = ip + decoded->Length;
                    continue;
                }
            } else {
                break;
            }
        }

        SPDLOG_ERROR("Failed to find call instruction!");
    }

    return false;
}

bool VRRenderTargetManager::AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips,
    ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture,
    FTexture2DRHIRef& OutShaderResourceTexture, uint32_t NumSamples) {
    // So, what's happening here is instead of using this method
    // to actually create our textures, we are going to
    // get the return address, scan forward for the next call instruction
    // and insert a midhook after the next call instruction.
    // The purpose of this is to get the texture that is being created
    // by the engine itself after we return false from this function.
    // When we return false from this function, it indicates
    // to the engine that we are letting the engine itself
    // create the texture, rather than us creating it ourselves.
    // This should allow maximum compatibility across engine versions.
    /*const auto dynamic_rhi = *(uintptr_t*)((uintptr_t)sdk::get_ue_module(L"Engine") + 0x3309C50);
    const auto command_list = (uintptr_t)sdk::get_ue_module(L"Engine") + 0x330AE70;
    struct {
        void* bulk_data{nullptr};
        void* rsrc_array{nullptr};

        struct {
            uint32_t color_binding{1};
            float color[4]{};
        } clear_value_binding;

        uint32_t gpu_mask{1};
        bool without_native_rsrc{false};
        const TCHAR* debug_name{"BufferedRT"};
        uint32_t extended_data{};
    } create_info;

    const void (*RHICreateTexture2D_RenderThread)(
        uintptr_t rhi,
        FTexture2DRHIRef* out,
        uintptr_t command_list,
        uint32_t w,
        uint32_t h,
        uint8_t format,
        uint32_t mips,
        uint32_t samples,
        ETextureCreateFlags flags,
        void* create_info) = (*(decltype(RHICreateTexture2D_RenderThread)**)dynamic_rhi)[178];

    *(uint64_t*)&TargetableTextureFlags |= (uint64_t)ETextureCreateFlags::ShaderResource | (uint64_t)Flags;
    RHICreateTexture2D_RenderThread(dynamic_rhi, &OutTargetableTexture, command_list, SizeX, SizeY, 2, NumMips, NumSamples, TargetableTextureFlags, &create_info);

    const auto size = get_ui_texture_size();
    RHICreateTexture2D_RenderThread(dynamic_rhi, &OutShaderResourceTexture, command_list, (uint32_t)size.x, (uint32_t)size.y, 2, NumMips, NumSamples, TargetableTextureFlags, &create_info);

    this->render_target = OutTargetableTexture.texture;
    this->ui_target = OutShaderResourceTexture.texture;

    OutShaderResourceTexture.texture = OutTargetableTexture.texture;*/

    m_last_allocate_render_target_return_address = (uintptr_t)_ReturnAddress();
    SPDLOG_INFO("AllocateRenderTargetTexture called from: {:x}", m_last_allocate_render_target_return_address - (uintptr_t)*utility::get_module_within((void*)m_last_allocate_render_target_return_address));

    // So, if CalculateRenderTargetSize was *never* called before this function
    // that means we have the virtual index of this function wrong, and we must swap the vtable out.
    // also, if this function was called very close to NeedReallocateDepthTexture, that also means
    // the virtual index is wrong, and we must swap the vtable out.
    const auto is_incorrect_vtable = 
        m_last_calculate_render_size_return_address == 0 ||
        m_last_allocate_render_target_return_address - m_last_needs_reallocate_depth_texture_return_address <= 0x200;

    if (is_incorrect_vtable) {
        // oh no this is the wrong vtable!!!! we need to fix it  nOW!!!
        SPDLOG_INFO("AllocateRenderTargetTexture called instead of AllocateDepthTexture! Fixing...");
        SPDLOG_INFO("Switching to old render target manager! Incorrect function called!");
        //g_hook->switch_to_old_rendertarget_manager();

        // Do a switcharoo on the vtable of this object to the old one because we will crash if we don't.
        // I've decided against actually switching the entire object over in favor of just vtable
        // swapping for now even though it's kind of a hack.
        const auto fake_object = std::make_unique<VRRenderTargetManager_418>();
        *(void**)this = *(void**)fake_object.get();

        return false;
    }

    this->depth_analysis_passed = true;

    return this->allocate_render_target_texture((uintptr_t)_ReturnAddress(), &OutTargetableTexture, &OutShaderResourceTexture);

    //return true;
}

bool VRRenderTargetManager_418::AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips, uint32_t Flags,
        uint32_t TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture, FTexture2DRHIRef& OutShaderResourceTexture,
        uint32_t NumSamples) 
{
    return this->allocate_render_target_texture((uintptr_t)_ReturnAddress(), &OutTargetableTexture, &OutShaderResourceTexture);
}

bool VRRenderTargetManager_Special::AllocateRenderTargetTexture(uint32_t Index, uint32_t SizeX, uint32_t SizeY, uint8_t Format, uint32_t NumMips,
    ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, FTexture2DRHIRef& OutTargetableTexture,
    FTexture2DRHIRef& OutShaderResourceTexture, uint32_t NumSamples) 
{
    return this->allocate_render_target_texture((uintptr_t)_ReturnAddress(), &OutTargetableTexture, &OutShaderResourceTexture);
}
