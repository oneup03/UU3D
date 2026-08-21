#pragma once

#define NOMINMAX

#include <memory>
#include <string>

#include <sdk/Math.hpp>

#include "vr/runtimes/OpenVR.hpp"
#include "vr/runtimes/OpenXR.hpp"
#include "vr/runtimes/Flat3D.hpp"

#include "vr/D3D11Component.hpp"
#include "vr/D3D12Component.hpp"
#include "vr/OverlayComponent.hpp"

#include "vr/FFakeStereoRenderingHook.hpp"
#include "vr/RenderTargetPoolHook.hpp"
#include "vr/CVarManager.hpp"

#include "Mod.hpp"

#undef max
#include <tracy/Tracy.hpp>

#include "PDAFWPlugin.h"

#include "vr/UpscaleHelper.hpp"

class VR : public Mod {
public:
    CameraData cameraData[2];
    CameraDataMVCorrection cameraDataForMV[2];
    D3D12RendererAPI* d3d12Renderer = nullptr;
    // AFW init is retried until it succeeds: the plugin device + raw D3D12 hooks
    // once (m_framewarp_device_initialized), and the DLSS/NGX hooks once nvngx.dll
    // has loaded (m_ngx_hooks_installed — it loads lazily when the game first uses
    // DLSS, which can be well after the VR mod initializes).
    bool m_framewarp_device_initialized = false;
    bool m_ngx_hooks_installed = false;

    // Plugin-free capture of the DLSS input depth for the Flat3D "DLSS Depth"
    // source: an owned per-eye copy made with our OWN D3D12 (game device +
    // CopyResource on the game's command list), independent of PDAFWPlugin.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dlss_depth[2]{};
    std::mutex m_dlss_depth_mutex{};

    ID3D12Resource* rawDepthTex = NULL;
    ID3D12Resource* rawMotionVectorsTex = NULL;

    TextureDesc rawMVDesc[2];

    TextureDesc rawVelocityDesc[2]{{}, {}};

    TextureDesc uiBufferDesc{};
    TextureDesc depthDesc[2]{{}, {}};
    TextureDesc motionVectorsDesc[2]{{}, {}};

    UINT renderSize[2] = {0, 0};
    UINT finalSize[2] = {1, 1};
    float mvScale[2] = {1.0, 1.0};
    float jitterOffset[2] = {1.0, 1.0};

    int afw_since_inject_frame_count = 0;
    int last_dlss_frame_count = 0;
    int dlss_continue_frame_count = 0;

    bool is_afw_last_frame = false;
    int afw_switching_skip_frames = 0;
    int afw_resolution_change_skip_frames = 0;

    bool mDebug1 = false;
    bool mDebug2 = false;
    bool mDebug3 = false;
    int mDebug5 = 0;

    std::map<NVSDK_NGX_Handle*, NVSDK_NGX_Feature> vrNoneDLSSHandleMap;

    struct MatrixPair {
        Matrix4x4f curr;
        Matrix4x4f other;
    };
    MatrixPair render_view_inv_matrix[2][3]{};
    MatrixPair render_projection_matrix[2]{};
    int last_update_matrix_frame_count[2] = {0, 0};

    int last_update_camera_data_frame_count = 0;
    void update_camera_data(int frame_count);

    int get_render_frame_count() { return m_render_frame_count; };
    int get_vr_frame_count() { return m_frame_count; };

    bool is_enable_sharpening() { return m_enable_sharpening->value(); };
    float get_sharpness() { return m_sharpness->value(); };

    float get_ignore_motion_threshold() { return m_ignore_motion_threshold->value(); };

    bool is_left_eye() { return m_frame_count % 2 == m_left_eye_interval; };
    uint8_t get_left_eye_interval() const { return m_left_eye_interval; };

    bool is_use_uint64() { return m_use_uint64->value(); };
    bool is_fix_object_motion_vector() { return m_fix_object_motion_vector->value(); };
    float get_fix_object_motion_range() { return m_fix_object_motion_range->value(); };

    ShadingRate get_framewarp_shading_rate() { 
        auto value = m_framewarp_shading_rate->value();
        auto shading_rate = ShadingRate::ShadingRate_1X1;
        if (value == 0) {
            shading_rate = ShadingRate::ShadingRate_1X1;
        } else if (value == 1) {
            shading_rate = ShadingRate::ShadingRate_1X2;
        } else if (value == 2) {
            shading_rate = ShadingRate::ShadingRate_1X4;
        } else if (value == 3) {
            shading_rate = ShadingRate::ShadingRate_2X1;
        } else if (value == 4) {
            shading_rate = ShadingRate::ShadingRate_2X2;
        } else if (value == 5) {
            shading_rate = ShadingRate::ShadingRate_2X4;
        } else if (value == 6) {
            shading_rate = ShadingRate::ShadingRate_4X1;
        } else if (value == 7) {
            shading_rate = ShadingRate::ShadingRate_4X2;
        } else if (value == 8) {
            shading_rate = ShadingRate::ShadingRate_4X4;
        } 
        return shading_rate;
    }

    bool is_using_ultra_responsive() { return m_ultra_responsive->value(); };
    bool is_fix_moving_object_brightness_flickering() { return m_fix_moving_object_brightness_flickering->value(); };

    bool is_dlss_for_n_frame(int count) { return dlss_continue_frame_count > count; };
    bool is_no_dlss() { return (m_render_frame_count - last_dlss_frame_count) > 10; };
    bool is_never_dlss() { return (m_render_frame_count - last_dlss_frame_count) > 10 && last_dlss_frame_count == 0; };

    bool is_renderdoc = false;

public:
    enum RenderingMethod {
        NATIVE_STEREO = 0,
        SYNCHRONIZED = 1,
        ALTERNATING = 2,
        ALTERNATE_FRAMEWARP = 3,
    };

    enum SynchronizeStage {
        EARLY = 0,
        LATE = 1,
        VERY_LATE = 2,
    };

    enum SyncedSequentialMethod {
        SKIP_TICK = 0,
        SKIP_DRAW = 1,
    };

    enum AimMethod : int32_t {
        GAME,
        HEAD,
        RIGHT_CONTROLLER,
        LEFT_CONTROLLER,
        TWO_HANDED_RIGHT,
        TWO_HANDED_LEFT,
    };

    enum DPadMethod : int32_t {
        RIGHT_TOUCH,
        LEFT_TOUCH,
        LEFT_JOYSTICK,
        RIGHT_JOYSTICK,
        GESTURE_HEAD,
        GESTURE_HEAD_RIGHT,
    };

    enum HORIZONTAL_PROJECTION_OVERRIDE : int32_t {
        HORIZONTAL_DEFAULT,
        HORIZONTAL_SYMMETRIC,
        HORIZONTAL_MIRROR
    };

    enum VERTICAL_PROJECTION_OVERRIDE : int32_t {
        VERTICAL_DEFAULT,
        VERTICAL_SYMMETRIC,
        VERTICAL_MATCHED
    };

    static const inline std::string s_action_pose = "/actions/default/in/Pose";
    static const inline std::string s_action_grip_pose = "/actions/default/in/GripPose";
    static const inline std::string s_action_trigger = "/actions/default/in/Trigger";
    static const inline std::string s_action_grip = "/actions/default/in/Grip";
    static const inline std::string s_action_joystick = "/actions/default/in/Joystick";
    static const inline std::string s_action_joystick_click = "/actions/default/in/JoystickClick";

    static const inline std::string s_action_a_button_left = "/actions/default/in/AButtonLeft";
    static const inline std::string s_action_b_button_left = "/actions/default/in/BButtonLeft";
    static const inline std::string s_action_a_button_touch_left = "/actions/default/in/AButtonTouchLeft";
    static const inline std::string s_action_b_button_touch_left = "/actions/default/in/BButtonTouchLeft";

    static const inline std::string s_action_a_button_right = "/actions/default/in/AButtonRight";
    static const inline std::string s_action_b_button_right = "/actions/default/in/BButtonRight";
    static const inline std::string s_action_a_button_touch_right = "/actions/default/in/AButtonTouchRight";
    static const inline std::string s_action_b_button_touch_right = "/actions/default/in/BButtonTouchRight";

    static const inline std::string s_action_dpad_up = "/actions/default/in/DPad_Up";
    static const inline std::string s_action_dpad_right = "/actions/default/in/DPad_Right";
    static const inline std::string s_action_dpad_down = "/actions/default/in/DPad_Down";
    static const inline std::string s_action_dpad_left = "/actions/default/in/DPad_Left";
    static const inline std::string s_action_system_button = "/actions/default/in/SystemButton";
    static const inline std::string s_action_thumbrest_touch_left = "/actions/default/in/ThumbrestTouchLeft";
    static const inline std::string s_action_thumbrest_touch_right = "/actions/default/in/ThumbrestTouchRight";

public:
    static std::shared_ptr<VR>& get();

    std::string_view get_name() const override { return "VR"; }

    std::optional<std::string> clean_initialize();
    std::optional<std::string> on_initialize_d3d_thread() {
        return clean_initialize();
    }

    // AFW (Async Frame Warp) plugin + DLSS/NGX hook init. Idempotent and retryable:
    // runs for every runtime (incl. Flat3D) and re-attempts the NGX hooks each frame
    // until nvngx.dll is present. Safe when the plugin is the no-op dummy (InitDevice
    // returns null -> AFW stays disabled instead of crashing).
    void init_framewarp_module();

    // Snapshot the DLSS input depth into m_dlss_depth[eye] using our own D3D12
    // (no plugin), recorded onto the game's command list. Called from the NGX
    // EvaluateFeature hook when the Flat3D "DLSS Depth" source is selected.
    void capture_dlss_depth_copy(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* depth, int eye);
    // The owned copy for a given eye (AddRef'd under the lock), or null. In
    // D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE.
    Microsoft::WRL::ComPtr<ID3D12Resource> get_dlss_depth_copy(int eye);

    std::vector<SidebarEntryInfo> get_sidebar_entries() override {
        return {
            {"Runtime", false},
            {"Unreal", false},
            {"Input", false},
            {"Camera", false},
            {"Keybinds", false},
            {"3D Display", false},
            {"Console/CVars", true},
            {"Compatibility", true},
            {"Debug", true},
        };
    }

    // texture bounds to tell OpenVR which parts of the submitted texture to render (default - use the whole texture).
    // Will be modified to accommodate forced symmetrical eye projection
    vr::VRTextureBounds_t m_right_bounds{0.0f, 0.0f, 1.0f, 1.0f};
    vr::VRTextureBounds_t m_left_bounds{0.0f, 0.0f, 1.0f, 1.0f};

    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;
    
    void on_draw_ui() override;
    void on_draw_sidebar_entry(std::string_view name) override;
    void on_pre_imgui_frame() override;

    void handle_keybinds();
    void on_frame() override;

    void on_present() override;
    void on_post_present() override;

    void on_device_reset() override {
        get_runtime()->on_device_reset();

        if (m_fake_stereo_hook != nullptr) {
            m_fake_stereo_hook->on_device_reset();
        }

        // Drop our owned DLSS depth copies; they're lazily re-created on the next
        // capture (also self-heals on a resolution change via the size check).
        {
            std::scoped_lock lock(m_dlss_depth_mutex);
            m_dlss_depth[0].Reset();
            m_dlss_depth[1].Reset();
        }

        if (m_is_d3d12) {
            m_d3d12.on_reset(this);
        } else {
            m_d3d11.on_reset(this);
        }
    }

    bool on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) override;
    void on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) override;
    void on_xinput_set_state(uint32_t* retval, uint32_t user_index, XINPUT_VIBRATION* vibration) override;
    void update_imgui_state_from_xinput_state(XINPUT_STATE& state, bool is_vr_controller);

    void on_pre_engine_tick(sdk::UGameEngine* engine, float delta) override;
    void on_pre_calculate_stereo_view_offset(void* stereo_device, const int32_t view_index, Rotator<float>* view_rotation, 
                                             const float world_to_meters, Vector3f* view_location, bool is_double) override;
    void on_pre_viewport_client_draw(void* viewport_client, void* viewport, void* canvas) override;

    void update_hmd_state(bool from_view_extensions = false, uint32_t frame_count = 0);
    void update_action_states();
    void update_dpad_gestures();

    void reinitialize_renderer() {
        if (m_is_d3d12) {
            m_d3d12.force_reset();
        } else {
            m_d3d11.force_reset();
        }
    }


    Vector4f get_position(uint32_t index, bool grip = true)  const;
    Vector4f get_velocity(uint32_t index)  const;
    Vector4f get_angular_velocity(uint32_t index)  const;
    Matrix4x4f get_hmd_rotation(uint32_t frame_count) const;
    Matrix4x4f get_hmd_transform(uint32_t frame_count) const;
    Matrix4x4f get_rotation(uint32_t index, bool grip = true)  const;
    Matrix4x4f get_transform(uint32_t index, bool grip = true) const;
    vr::HmdMatrix34_t get_raw_transform(uint32_t index) const;

    Vector4f get_grip_position(uint32_t index) const {
        return get_position(index, true);
    }

    Vector4f get_aim_position(uint32_t index) const {
        return get_position(index, false);
    }

    Matrix4x4f get_grip_rotation(uint32_t index) const {
        return get_rotation(index, true);
    }

    Matrix4x4f get_aim_rotation(uint32_t index) const {
        return get_rotation(index, false);
    }

    Matrix4x4f get_grip_transform(uint32_t hand_index) const;
    Matrix4x4f get_aim_transform(uint32_t hand_index) const;

    Vector4f get_eye_offset(VRRuntime::Eye eye) const;
    Vector4f get_current_offset();
    
    Matrix4x4f get_eye_transform(uint32_t index);
    Matrix4x4f get_current_eye_transform(bool flip = false);
    Matrix4x4f get_projection_matrix(VRRuntime::Eye eye, bool flip = false);
    Matrix4x4f get_current_projection_matrix(bool flip = false);

    bool is_action_active(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle) const;

    bool is_action_active_any_joystick(vr::VRActionHandle_t action) const {
        if (is_action_active(action, m_left_joystick)) {
            return true;
        }

        if (is_action_active(action, m_right_joystick)) {
            return true;
        }

        return false;
    }
    Vector2f get_joystick_axis(vr::VRInputValueHandle_t handle) const;

    vr::VRActionHandle_t get_action_handle(std::string_view action_path) {
        if (auto it = m_action_handles.find(action_path.data()); it != m_action_handles.end()) {
            return it->second;
        }

        return vr::k_ulInvalidActionHandle;
    }

    Vector2f get_left_stick_axis() const;
    Vector2f get_right_stick_axis() const;

    void trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle);
    
    float get_standing_height();
    Vector4f get_standing_origin();
    void set_standing_origin(const Vector4f& origin);

    glm::quat get_rotation_offset();
    void set_rotation_offset(const glm::quat& offset);
    void recenter_view();
    void recenter_horizon();


    template<typename T = VRRuntime>
    T* get_runtime() const {
        return (T*)m_runtime.get();
    }

    runtimes::OpenXR* get_openxr_runtime() const {
        return m_openxr.get();
    }

    runtimes::OpenVR* get_openvr_runtime() const {
        return m_openvr.get();
    }

    runtimes::Flat3D* get_flat3d_runtime() const {
        return m_flat3d.get();
    }

    bool is_using_flat3d() const {
        return m_runtime != nullptr && m_runtime->is_flat3d();
    }

    // Queues a 3D screenshot of the next composited stereo frame (Ctrl+F12 /
    // the menu-header button drawn by Framework).
    void request_flat3d_screenshot();

    float get_flat3d_opentrack_rot_scale() const { return m_flat3d_opentrack_rot_scale->value(); }
    float get_flat3d_opentrack_pos_scale() const { return m_flat3d_opentrack_pos_scale->value(); }

    bool is_hmd_active() const {
        if (m_disable_vr) {
            return false;
        }

        auto runtime = get_runtime();

        if (runtime == nullptr) {
            return false;
        }

        return runtime->ready() || (m_stereo_emulation_mode && runtime->loaded);
    }

    auto get_hmd() const {
        return m_openvr->hmd;
    }

    auto& get_openvr_poses() const {
        return m_openvr->render_poses;
    }

    auto& get_overlay_component() {
        return m_overlay_component;
    }

    uint32_t get_hmd_width() const;
    uint32_t get_hmd_height() const;

    const auto& get_eyes() const {
        return get_runtime()->eyes;
    }

    auto get_frame_count() const {
        return m_frame_count;
    }

    auto& get_controllers() const {
        return m_controllers;
    }

    bool is_using_controllers() const {
        return m_controller_test_mode || (m_controllers_allowed->value() &&
        is_hmd_active() && !m_controllers.empty() && (std::chrono::steady_clock::now() - m_last_controller_update) <= std::chrono::seconds((int32_t)m_motion_controls_inactivity_timer->value()));
    }

    bool is_using_controllers_within(std::chrono::seconds seconds) const {
        return m_controllers_allowed->value() && is_hmd_active() && !m_controllers.empty() && (std::chrono::steady_clock::now() - m_last_controller_update) <= seconds;
    }

    int get_hmd_index() const {
        return 0;
    }

    int get_left_controller_index() const {
        const auto wants_swap = m_swap_controllers->value();

        if (m_runtime->is_openxr()) {
            return wants_swap ? 2 : 1;
        } else if (m_runtime->is_openvr()) {
            return !m_controllers.empty() ? (wants_swap ? m_controllers[1] : m_controllers[0]) : -1;
        }

        return -1;
    }

    int get_right_controller_index() const {
        const auto wants_swap = m_swap_controllers->value();

        if (m_runtime->is_openxr()) {
            return wants_swap ? 1 : 2;
        } else if (m_runtime->is_openvr()) {
            return !m_controllers.empty() ? (wants_swap ? m_controllers[0] : m_controllers[1]) : -1;
        }

        return -1;
    }

    auto get_left_joystick() const {
        if (!m_swap_controllers->value()) {
            return m_left_joystick;
        }

        return m_right_joystick;
    }

    auto get_right_joystick() const {
        if (!m_swap_controllers->value()) {
            return m_right_joystick;
        }

        return m_left_joystick;
    }

    bool is_gui_enabled() const {
        return m_enable_gui->value();
    }

    auto get_camera_forward_offset() const {
        return m_camera_forward_offset->value();
    }

    auto get_camera_right_offset() const {
        return m_camera_right_offset->value();
    }

    auto get_camera_up_offset() const {
        return m_camera_up_offset->value();
    }

    auto get_world_scale() const {
        return m_world_scale->value();
    }

    auto is_stereo_emulation_enabled() const {
        return m_stereo_emulation_mode;
    }

    void reset_present_event() {
        ResetEvent(m_present_finished_event);
    }

    void wait_for_present() {
        if (!m_wait_for_present) {
            return;
        }

        if (m_frame_count <= m_game_frame_count) {
            //return;
        }

        if (WaitForSingleObject(m_present_finished_event, 11) == WAIT_TIMEOUT) {
            //timed_out = true;
        }

        m_game_frame_count = m_frame_count;
        //ResetEvent(m_present_finished_event);
    }

    auto& get_vr_mutex() {
        return m_openvr_mtx;
    }

    bool is_using_afr() const {
        return m_rendering_method->value() == RenderingMethod::ALTERNATING || 
               m_rendering_method->value() == RenderingMethod::SYNCHRONIZED ||
               m_rendering_method->value() == RenderingMethod::ALTERNATE_FRAMEWARP ||
               m_extreme_compat_mode->value() == true;
    }

    bool is_using_synchronized_afr() const {
        return m_rendering_method->value() == RenderingMethod::SYNCHRONIZED ||
               (m_extreme_compat_mode->value() && m_rendering_method->value() == RenderingMethod::NATIVE_STEREO) ||
               (m_rendering_method->value() == RenderingMethod::ALTERNATE_FRAMEWARP &&
                   (afw_since_inject_frame_count < 90 || afw_resolution_change_skip_frames > 0 || is_using_2d_screen()));
    }

    bool is_using_afw() {
        return m_rendering_method->value() == RenderingMethod::ALTERNATE_FRAMEWARP && afw_since_inject_frame_count >= 90 &&
               afw_switching_skip_frames == 0 && afw_resolution_change_skip_frames == 0 && g_framework->is_dx12() && !is_using_2d_screen();
    }

    bool is_using_afw_without_api_check() {
        return m_rendering_method->value() == RenderingMethod::ALTERNATE_FRAMEWARP && afw_since_inject_frame_count >= 90 &&
               afw_switching_skip_frames == 0 && afw_resolution_change_skip_frames == 0;
    }


    SynchronizeStage get_synchronize_stage() {
        return (SynchronizeStage) m_sync_mode->value();
    }

    SyncedSequentialMethod get_synced_sequential_method() const {
        return (SyncedSequentialMethod)m_synced_afr_method->value();
    }

    uint32_t get_lowest_xinput_index() const {
        return m_lowest_xinput_user_index;
    }

    auto& get_render_target_pool_hook() const {
        return m_render_target_pool_hook;
    }

    void set_world_to_meters(float value) {
        m_world_to_meters = value;
    }

    float get_world_to_meters() const {
        return m_world_to_meters * m_world_scale->value();
    }

    float get_depth_scale() const {
        return m_depth_scale->value();
    }

    // Resolve the effective near clipping plane for the Flat3D stereo/depth
    // path. Wraps the SDK's GNearClippingPlane scan so the UESDK submodule can
    // stay pristine: on obfuscated/inlined builds where the scan fails (e.g.
    // Expedition 33) the SDK returns its 1.0 dummy, so we substitute the game's
    // r.SetNearClipPlane cvar (if it set one) or UE's own default of 10.
    // An explicit Custom Z Near override is always honored verbatim.
    float flat3d_effective_nearz();

    bool is_depth_enabled() const {
        return m_enable_depth->value();
    }

    // Flat3D scene-depth source for the Adaptive Crosshair / HUD-depth /
    // Auto-Convergence features.
    enum Flat3DDepthSource : int32_t {
        FLAT3D_DEPTH_PER_DRAW = 0,     // API-level per-draw capture (safe default)
        FLAT3D_DEPTH_ENGINE_POOL = 1,  // UE render-target pool SceneDepthZ (engine hook)
        FLAT3D_DEPTH_DSV_OBSERVER = 2, // D3D12Hook DSV/barrier observer snapshot (no engine hook)
        FLAT3D_DEPTH_DLSS = 3,         // our own copy of the game's DLSS input depth (any mode)
    };

    int32_t flat3d_depth_source() const {
        const auto v = m_flat3d_depth_source->value();

        // Legacy promotion: configs saved before the combo existed used the
        // Flat3D_UseEngineDepth toggle. Honor it while the combo still sits at
        // its default; the UI mirrors the toggle on any explicit combo change.
        if (v == FLAT3D_DEPTH_PER_DRAW && m_flat3d_use_engine_depth->value()) {
            return FLAT3D_DEPTH_ENGINE_POOL;
        }

        return v;
    }

    bool flat3d_use_engine_depth() const {
        return flat3d_depth_source() == FLAT3D_DEPTH_ENGINE_POOL;
    }

    // Multiplier applied to the game's live FoV when the render frustum is
    // rebuilt. 1.0 = use the game camera's FoV unchanged.
    float flat3d_fov_multiplier() const {
        const auto v = m_flat3d_fov_multiplier->value();
        return (std::isfinite(v) && v > 0.05f) ? v : 1.0f;
    }

    // 3D render resolution as a fraction of native, applied through
    // r.ScreenPercentage (see flat3d_apply_screen_percentage) — it does NOT size
    // the stereo target. 0 = Auto: leave the engine's screen percentage alone.
    float flat3d_render_scale() const {
        const auto v = (size_t)m_flat3d_render_scale->value();
        return v < s_flat3d_render_scale_values.size() ? s_flat3d_render_scale_values[v] : 0.0f;
    }

    // Raw Flat3D output mode (vrmod::flat3d::Flat3DOutputMode). Returned as int
    // to avoid pulling the compositor header into VR.hpp; callers classify it.
    int32_t flat3d_output_mode_value() const {
        return (int32_t)m_flat3d_output_mode->value();
    }

    // The 3D output always holds on the PRIMARY display (3D users keep the 3D
    // panel primary); this makes the native-output hold deterministic.
    bool flat3d_output_on_primary() const {
        return true;
    }

    // True when we detected a real full-SbS double-wide panel (drives Native
    // Render + Upscale automatically).
    bool flat3d_is_full_sbs() const {
        return m_flat3d_full_sbs.load(std::memory_order_acquire);
    }


    // The DSV-observer depth source is D3D12-only. It installs no engine hook
    // (unlike the pool path) and sees depth allocated at any time (unlike the
    // per-draw path).
    bool flat3d_wants_dsv_depth() const {
        return flat3d_depth_source() == FLAT3D_DEPTH_DSV_OBSERVER && m_is_d3d12;
    }

    // The "DLSS Depth" source snapshots the game's DLSS input depth into our own
    // per-eye copy (m_dlss_depth[], via capture_dlss_depth_copy in the NGX hook) —
    // the exact render-res scene depth the game feeds DLSS. Plugin-free and works
    // in any rendering method (not just AFW). D3D12-only; needs DLSS active in-game.
    bool flat3d_wants_dlss_depth() const {
        return flat3d_depth_source() == FLAT3D_DEPTH_DLSS && m_is_d3d12;
    }

    bool is_decoupled_pitch_enabled() const {
        return m_decoupled_pitch->value();
    }

    bool is_decoupled_pitch_ui_adjust_enabled() const {
        return m_decoupled_pitch_ui_adjust->value();
    }

    void set_decoupled_pitch(bool value) {
        m_decoupled_pitch->value() = value;
    }

    void set_aim_allowed(bool value) {
        m_aim_temp_disabled = !value;
    }

    bool is_aim_allowed() const {
        return !m_aim_temp_disabled;
    }

    AimMethod get_aim_method() const {
        if (m_aim_temp_disabled) {
            return AimMethod::GAME;
        }

        return (AimMethod)m_aim_method->value();
    }

    void set_aim_method(AimMethod method) {
        if ((size_t)method >= s_aim_method_names.size()) {
            method = AimMethod::GAME;
        }

        m_aim_method->value() = method;
    }

    AimMethod get_movement_orientation() const {
        return (AimMethod)m_movement_orientation->value();
    }

    float get_aim_speed() const {
        return m_aim_speed->value();
    }
    
    bool is_aim_multiplayer_support_enabled() const {
        return m_aim_multiplayer_support->value();
    }

    bool is_aim_pawn_control_rotation_enabled() const {
        return m_aim_use_pawn_control_rotation->value();
    }

    bool is_aim_modify_player_control_rotation_enabled() const {
        return m_aim_modify_player_control_rotation->value();
    }

    bool is_aim_interpolation_enabled() const {
        return m_aim_interp->value();
    }
    
    bool is_any_aim_method_active() const {
        return m_aim_method->value() > AimMethod::GAME && !m_aim_temp_disabled;
    }

    bool is_headlocked_aim_enabled() const {
        return m_aim_method->value() == AimMethod::HEAD && !m_aim_temp_disabled;
    }

    bool is_controller_aim_enabled() const {
        const auto value = m_aim_method->value();
        return !m_aim_temp_disabled && (value == AimMethod::LEFT_CONTROLLER || value == AimMethod::RIGHT_CONTROLLER || value == AimMethod::TWO_HANDED_LEFT || value == AimMethod::TWO_HANDED_RIGHT);
    }

    bool is_controller_movement_enabled() const {
        const auto value = m_movement_orientation->value();
        return value == AimMethod::LEFT_CONTROLLER || value == AimMethod::RIGHT_CONTROLLER || value == AimMethod::TWO_HANDED_LEFT || value == AimMethod::TWO_HANDED_RIGHT;
    }

    bool wants_blueprint_load() const {
        return m_load_blueprint_code->value();
    }

    bool is_splitscreen_compatibility_enabled() const {
        return m_splitscreen_compatibility_mode->value();
    }

    uint32_t get_requested_splitscreen_index() const {
        return m_splitscreen_view_index->value();
    }

    // Flat3D single-view render target. In AFR/Synced/AFW the engine renders ONE
    // view per frame, and it sizes that view's VIEWPORT from the whole stereo
    // target: with a 2W surface it spans 2W while only the left half is covered,
    // so we read the left portion of a frame cut at the halfway line (widening
    // the FoV rescales inside the cut instead of revealing the right side —
    // Fantasy Life i). Advertising a single-eye surface makes viewport == eye
    // rect. NOT for Native Stereo Fix: that still builds two views (the second is
    // redirected to the scene capture) and a half-width surface renders nothing.
    // Pair with 3D Render Resolution = 100%, or the surface still disagrees with
    // the scale the engine lays the scene out at.
    // One view per frame owns the WHOLE stereo surface (Alternating, Synced
    // Sequential, AFW, and Extreme Compatibility, which reads one backbuffer at
    // a time) — so a double-wide target leaves the engine's viewport spanning 2W
    // with only the left half covered. Native Stereo renders both views into the
    // double-wide and must keep it.
    //
    // The Native Stereo Fix checkbox is deliberately NOT consulted: NSF only
    // does anything in Native Stereo, so testing it here would silently disable
    // single-view for an AFR run just because the box was left ticked.
    bool flat3d_single_view_target() const {
        return m_compatibility_single_view_render_target->value() &&
               is_using_flat3d() && is_using_afr();
    }

    // Drives the screen percentage from the 3D Render Resolution combo.
    void flat3d_apply_screen_percentage();

    // Which stage the render resolution drives: 0 = primary (r.ScreenPercentage,
    // the upscaler's own input resolution), 1 = secondary (a separate pass after
    // the temporal upscale, so it stacks with DLSS instead of fighting it),
    // 2 = shrink our stereo render target directly (legacy).
    int32_t flat3d_render_scale_stage() const {
        return (int32_t)m_flat3d_render_scale_stage->value();
    }

    // Legacy stage: size the stereo target by the percentage instead of asking
    // the engine to render less. Kept because some titles honour neither screen
    // percentage cvar (Jedi Survivor), but it can leave the frame a top-left
    // corner of a full-size render when the engine sizes scene work from its
    // viewport rather than from the target we hand it.
    bool flat3d_render_scale_sizes_target() const {
        return flat3d_render_scale_stage() == 2 && flat3d_render_scale() > 0.0f;
    }

    bool is_sceneview_compatibility_enabled() const {
        return m_sceneview_compatibility_mode->value();
    }

    bool is_native_stereo_fix_enabled() const {
        return m_native_stereo_fix->value() && !is_using_afr();
    }

    bool is_native_stereo_fix_same_pass_enabled() const {
        return m_native_stereo_fix_same_pass->value();
    }

    bool is_ahud_compatibility_enabled() const {
        return m_compatibility_ahud->value();
    }

    bool is_ghosting_fix_enabled() const {
        return m_ghosting_fix->value();
    }

    auto& get_fake_stereo_hook() {
        return m_fake_stereo_hook;
    }

    void set_pre_flattened_rotation(const glm::quat& rot) {
        std::unique_lock _{m_decoupled_pitch_data.mtx};
        m_decoupled_pitch_data.pre_flattened_rotation = rot;
    }

    auto get_pre_flattened_rotation() const {
        std::shared_lock _{m_decoupled_pitch_data.mtx};
        return m_decoupled_pitch_data.pre_flattened_rotation;
    }

    bool is_using_2d_screen() const {
        // Mutually exclusive with 3D Display mode (which reuses its view-path
        // neutralization but owns the projection and presentation).
        return m_2d_screen_mode->value() && !is_using_flat3d();
    }

    bool is_roomscale_enabled() const {
        return m_roomscale_movement->value() && !m_aim_temp_disabled;
    }

    bool is_roomscale_sweep_enabled() const {
        return m_roomscale_sweep->value();
    }

    bool is_dpad_shifting_enabled() const {
        return m_dpad_shifting->value();
    }

    DPadMethod get_dpad_method() const {
        return (DPadMethod)m_dpad_shifting_method->value();
    }

    bool is_snapturn_enabled() const {
        return m_snapturn->value();
    }

    void set_snapturn_enabled(bool value) {
        m_snapturn->value() = value;
    }

    float get_snapturn_js_deadzone() const {
        return m_snapturn_joystick_deadzone->value();
    }

    int get_snapturn_angle() const {
        return m_snapturn_angle->value();
    }

    float get_controller_pitch_offset() const {
        return m_controller_pitch_offset->value();
    }

    bool should_skip_post_init_properties() const {
        return m_compatibility_skip_pip->value();
    }
    
    bool should_skip_uobjectarray_init() const {
        return m_compatibility_skip_uobjectarray_init->value();
    }

    bool is_extreme_compatibility_mode_enabled() const {
        return m_extreme_compat_mode->value();
    }

    auto get_horizontal_projection_override() const {
        return m_horizontal_projection_override->value();
    }

    auto get_vertical_projection_override() const {
        return m_vertical_projection_override->value();
    }

    // World-marker HUD: post-hook body for the projection UFunction hooks
    // (the raw callback is a free function in VR_Flat3D.cpp; this needs
    // member access). ufunction is an sdk::UFunction*.
    void flat3d_marker_hook_post(void* ufunction, void* params);

    bool should_grow_rectangle_for_projection_cropping() const {
        return m_grow_rectangle_for_projection_cropping->value();
    }

    vrmod::D3D11Component& d3d11() {
        return m_d3d11;
    }

    vrmod::D3D12Component& d3d12() {
        return m_d3d12;
    }

    uint32_t get_present_thread_id() const {
        return m_present_thread_id;
    }

private:
    Vector4f get_position_unsafe(uint32_t index) const;
    Vector4f get_velocity_unsafe(uint32_t index) const;
    Vector4f get_angular_velocity_unsafe(uint32_t index) const;

private:
    std::optional<std::string> initialize_openvr();
    std::optional<std::string> initialize_openvr_input();
    std::optional<std::string> initialize_openxr();
    std::optional<std::string> initialize_openxr_input();
    std::optional<std::string> initialize_openxr_swapchains();

    // Flat 3D monitor mode (implemented in VR_Flat3D.cpp)
    std::optional<std::string> initialize_flat3d();
    void on_draw_sidebar_flat3d();
    void handle_flat3d_keybinds();
    void update_flat3d_params(); // per-frame effective separation/convergence
    void update_flat3d_ensure_windowed(); // Katanga: force the game into a normal window
    // Samples the game camera's live FoV (deg, horizontal) via
    // APlayerCameraManager::GetFOVAngle. Game thread only (ProcessEvent);
    // returns fallback_deg when no world/camera is available yet.
    float sample_flat3d_game_fov(float fallback_deg);
    // Reads APlayerController::bShowMouseCursor (drives the stereo cursor).
    // Game thread only; returns fallback when no world/controller yet.
    bool sample_flat3d_show_cursor(bool fallback);
    bool sample_flat3d_fov_is_vertical(bool fallback);
    std::optional<bool> camera_component_fov_is_vertical();

    // Screen-percentage state: the value we last wrote (0 = we are not managing
    // it, so Auto leaves whatever the game does alone) and the stage we wrote it
    // to, so a stage switch can hand the previous one back first.
    int32_t m_flat3d_screen_percentage_applied{0};
    int32_t m_flat3d_screen_percentage_mode{0};
    bool sample_flat3d_game_paused(bool fallback);
    // Samples the camera position/forward and publishes the HUD marker
    // anchors collected this frame. Game thread only.
    void sample_flat3d_camera_and_publish_anchors();
    // Installs the ProjectWorldLocationToScreen / ProjectWorldToScreen
    // UFunction hooks that feed the world-marker HUD mode. Idempotent.
    void setup_flat3d_marker_hooks();
    // Per-frame parameter block for the D3D compositors (UI/crosshair shifts,
    // HDR colorspace comes from the component side).
    vrmod::flat3d::Flat3DFrameParams build_flat3d_frame_params(uint32_t eye_w, uint32_t eye_h);

    bool detect_controllers();
    bool is_any_action_down();

    std::optional<std::string> reinitialize_openvr() {
        spdlog::info("Reinitializing OpenVR");
        std::scoped_lock _{m_openvr_mtx};

        m_runtime.reset();
        m_runtime = std::make_shared<VRRuntime>();
        m_openvr.reset();

        // Reinitialize openvr input, hopefully this fixes the issue
        m_controllers.clear();
        m_controllers_set.clear();

        auto e = initialize_openvr();

        if (e) {
            spdlog::error("Failed to reinitialize OpenVR: {}", *e);
        }

        return e;
    }

    std::optional<std::string> reinitialize_openxr() {
        spdlog::info("Reinitializing OpenXR");
        std::scoped_lock _{m_openvr_mtx};

        if (m_is_d3d12) {
            m_d3d12.openxr().destroy_swapchains();
        } else {
            m_d3d11.openxr().destroy_swapchains();
        }

        m_openxr.reset();
        m_runtime.reset();
        m_runtime = std::make_shared<VRRuntime>();
        
        m_controllers.clear();
        m_controllers_set.clear();

        auto e = initialize_openxr();

        if (e) {
            spdlog::error("Failed to reinitialize OpenXR: {}", *e);
        }

        return e;
    }

    float m_nearz{ 0.1f };
    float m_farz{ 3000.0f };
    float m_world_to_meters{1.0f}; // Placeholder, it gets set later in a hook

    std::unique_ptr<FFakeStereoRenderingHook> m_fake_stereo_hook{ std::make_unique<FFakeStereoRenderingHook>() };
    std::unique_ptr<RenderTargetPoolHook> m_render_target_pool_hook{ std::make_unique<RenderTargetPoolHook>() };
    std::unique_ptr<CVarManager> m_cvar_manager{ std::make_unique<CVarManager>() };

    void add_components_vr() {
        m_components = {
            m_fake_stereo_hook.get(),
            m_render_target_pool_hook.get(),
            m_cvar_manager.get(),
            &m_overlay_component
        };
    }

    std::shared_ptr<VRRuntime> m_runtime{std::make_shared<VRRuntime>()}; // will point to the real runtime if it exists
    std::shared_ptr<runtimes::OpenVR> m_openvr{std::make_shared<runtimes::OpenVR>()};
    std::shared_ptr<runtimes::OpenXR> m_openxr{std::make_shared<runtimes::OpenXR>()};
    std::shared_ptr<runtimes::Flat3D> m_flat3d{std::make_shared<runtimes::Flat3D>()};

    mutable TracyLockable(std::recursive_mutex, m_openvr_mtx);
    mutable TracyLockable(std::recursive_mutex, m_reinitialize_mtx);
    mutable TracyLockable(std::recursive_mutex, m_actions_mtx);
    mutable std::shared_mutex m_rotation_mtx{};

    std::vector<int32_t> m_controllers{};
    std::unordered_set<int32_t> m_controllers_set{};

    glm::vec3 m_overlay_rotation{-1.550f, 0.0f, -1.330f};
    glm::vec4 m_overlay_position{0.0f, 0.06f, -0.07f, 1.0f};
    
    Vector4f m_standing_origin{ 0.0f, 1.5f, 0.0f, 0.0f };
    glm::quat m_rotation_offset{ glm::identity<glm::quat>() };

    HANDLE m_present_finished_event{CreateEvent(nullptr, TRUE, FALSE, nullptr)};

    Vector4f m_raw_projections[2]{};

    vrmod::D3D11Component m_d3d11{};
    vrmod::D3D12Component m_d3d12{};
    vrmod::OverlayComponent m_overlay_component;
    bool m_disable_overlay{false};

    // Action set handles
    vr::VRActionSetHandle_t m_action_set{};
    vr::VRActiveActionSet_t m_active_action_set{};

    // Action handles
    vr::VRActionHandle_t m_action_pose{ };
    vr::VRActionHandle_t m_action_trigger{ };
    vr::VRActionHandle_t m_action_grip{ };
    vr::VRActionHandle_t m_action_grip_pose{ };
    vr::VRActionHandle_t m_action_joystick{};
    vr::VRActionHandle_t m_action_joystick_click{};

    vr::VRActionHandle_t m_action_a_button_right{};
    vr::VRActionHandle_t m_action_a_button_touch_right{};
    vr::VRActionHandle_t m_action_b_button_right{};
    vr::VRActionHandle_t m_action_b_button_touch_right{};

    vr::VRActionHandle_t m_action_a_button_left{};
    vr::VRActionHandle_t m_action_a_button_touch_left{};
    vr::VRActionHandle_t m_action_b_button_left{};
    vr::VRActionHandle_t m_action_b_button_touch_left{};

    vr::VRActionHandle_t m_action_dpad_up{};
    vr::VRActionHandle_t m_action_dpad_right{};
    vr::VRActionHandle_t m_action_dpad_down{};
    vr::VRActionHandle_t m_action_dpad_left{};

    vr::VRActionHandle_t m_action_system_button{};
    vr::VRActionHandle_t m_action_haptic{};
    vr::VRActionHandle_t m_action_thumbrest_touch_left{};
    vr::VRActionHandle_t m_action_thumbrest_touch_right{};

    std::unordered_map<std::string, std::reference_wrapper<vr::VRActionHandle_t>> m_action_handles {
        { s_action_pose, m_action_pose },
        { s_action_grip_pose, m_action_grip_pose },
        { s_action_trigger, m_action_trigger },
        { s_action_grip, m_action_grip },
        { s_action_joystick, m_action_joystick },
        { s_action_joystick_click, m_action_joystick_click },

        { s_action_a_button_left, m_action_a_button_left },
        { s_action_b_button_left, m_action_b_button_left },
        { s_action_a_button_touch_left, m_action_a_button_touch_left },
        { s_action_b_button_touch_left, m_action_b_button_touch_left },

        { s_action_a_button_right, m_action_a_button_right },
        { s_action_b_button_right, m_action_b_button_right },
        { s_action_a_button_touch_right, m_action_a_button_touch_right },
        { s_action_b_button_touch_right, m_action_b_button_touch_right },

        { s_action_dpad_up, m_action_dpad_up },
        { s_action_dpad_right, m_action_dpad_right },
        { s_action_dpad_down, m_action_dpad_down },
        { s_action_dpad_left, m_action_dpad_left },

        { s_action_system_button, m_action_system_button },
        { s_action_thumbrest_touch_left, m_action_thumbrest_touch_left },
        { s_action_thumbrest_touch_right, m_action_thumbrest_touch_right },

        // Out
        { "/actions/default/out/Haptic", m_action_haptic },
    };

    // Input sources
    vr::VRInputValueHandle_t m_left_joystick{};
    vr::VRInputValueHandle_t m_right_joystick{};

    std::chrono::steady_clock::time_point m_last_controller_update{};
    std::chrono::steady_clock::time_point m_last_xinput_update{};
    std::chrono::steady_clock::time_point m_last_xinput_spoof_sent{};
    std::chrono::steady_clock::time_point m_last_xinput_l3_r3_menu_open{};
    std::chrono::steady_clock::time_point m_last_interaction_display{};
    std::chrono::steady_clock::time_point m_last_engine_tick{};

    uint32_t m_lowest_xinput_user_index{};

    std::chrono::nanoseconds m_last_input_delay{};
    std::chrono::nanoseconds m_avg_input_delay{};

    static const inline std::vector<std::string> s_rendering_method_names {
        "Native Stereo",
        "Synchronized Sequential",
        "Alternating/AFR",
        "Alternate Frame Warping",
    };

    static const inline std::vector<std::string> s_sync_mode_names{
        "Early",
        "Late",
        "Very Late",
    };

    static const inline std::vector<std::string> s_synced_afr_method_names {
        "Skip Tick",
        "Skip Draw",
    };

    static const inline std::vector<std::string> s_aim_method_names {
        "Game",
        "Head/HMD",
        "Right Controller",
        "Left Controller",
        "Two Handed (Right)",
        "Two Handed (Left)",
    };

    static const inline std::vector<std::string> s_dpad_method_names {
        "Right Thumbrest + Left Joystick",
        "Left Thumbrest + Right Joystick",
        "Left Joystick (Disables Standard Joystick Input)",
        "Right Joystick (Disables Standard Joystick Input)",
        "Gesture (Head) + Left Joystick",
        "Gesture (Head) + Right Joystick",
    };

    static const inline std::vector<std::string> s_horizontal_projection_override_names{
        "Raw / default",
        "Symmetrical",
        "Mirrored",
    };

    static const inline std::vector<std::string> s_vertical_projection_override_names{
        "Raw / default",
        "Symmetrical",
        "Matched",
    };

    enum DesktopMirrorMode : int32_t {
        DESKTOP_MIRROR_FULL = 0,
        DESKTOP_MIRROR_SCENE_ONLY = 1,
    };

    static const inline std::vector<std::string> s_desktop_mirror_mode_names{
        "Full",
        "Scene Only",
    };

    enum Subnautica2NativeWaterMode : int32_t {
        SUBNAUTICA2_NATIVE_WATER_SAFE_REFLECTIONS = 0,
        SUBNAUTICA2_NATIVE_WATER_NO_REFLECTIONS = 1,
        SUBNAUTICA2_NATIVE_WATER_DISABLE_SINGLE_LAYER = 2,
    };

    static const inline std::vector<std::string> s_subnautica2_native_water_mode_names{
        "Native Water Safe Reflections",
        "Native Water No Reflections",
        "Disable SingleLayerWater Fallback",
    };

    const ModCombo::Ptr m_rendering_method{ ModCombo::create(generate_name("RenderingMethod"), s_rendering_method_names, RenderingMethod::NATIVE_STEREO) };
    const ModCombo::Ptr m_synced_afr_method{ ModCombo::create(generate_name("SyncedSequentialMethod"), s_synced_afr_method_names, 1) };
    // Ours (Desktop Spectator View Mode); the enum + names above survived the
    // cherry-pick but this member landed in a hunk that took the AFW side.
    const ModCombo::Ptr m_desktop_mirror_mode{ ModCombo::create(generate_name("DesktopSpectatorViewMode"), s_desktop_mirror_mode_names, DESKTOP_MIRROR_FULL) };
    const ModToggle::Ptr m_extreme_compat_mode{ ModToggle::create(generate_name("ExtremeCompatibilityMode"), false, true) };
    const ModToggle::Ptr m_uncap_framerate{ ModToggle::create(generate_name("UncapFramerate"), true) };
    const ModToggle::Ptr m_disable_blur_widgets{ ModToggle::create(generate_name("DisableBlurWidgets"), true) };
    const ModToggle::Ptr m_disable_hdr_compositing{ ModToggle::create(generate_name("DisableHDRCompositing"), true, true) };
    const ModToggle::Ptr m_disable_hzbocclusion{ ModToggle::create(generate_name("DisableHZBOcclusion"), true, true) };
    const ModToggle::Ptr m_disable_instance_culling{ ModToggle::create(generate_name("DisableInstanceCulling"), true, true) };
    const ModToggle::Ptr m_desktop_fix{ ModToggle::create(generate_name("DesktopRecordingFix_V2"), true) };
    const ModToggle::Ptr m_enable_gui{ ModToggle::create(generate_name("EnableGUI"), true) };
    const ModToggle::Ptr m_enable_depth{ ModToggle::create(generate_name("PassDepthToRuntime"), false, true) };
    const ModToggle::Ptr m_decoupled_pitch{ ModToggle::create(generate_name("DecoupledPitch"), false) };
    const ModToggle::Ptr m_decoupled_pitch_ui_adjust{ ModToggle::create(generate_name("DecoupledPitchUIAdjust"), true) };
    const ModToggle::Ptr m_load_blueprint_code{ ModToggle::create(generate_name("LoadBlueprintCode"), false, true) };
    const ModToggle::Ptr m_2d_screen_mode{ ModToggle::create(generate_name("2DScreenMode"), false) };
    const ModToggle::Ptr m_roomscale_movement{ ModToggle::create(generate_name("RoomscaleMovement"), false) };
    const ModToggle::Ptr m_roomscale_sweep{ ModToggle::create(generate_name("RoomscaleMovementSweep"), true) };
    const ModToggle::Ptr m_swap_controllers{ ModToggle::create(generate_name("SwapControllerInputs"), false) };
    const ModCombo::Ptr m_horizontal_projection_override{ModCombo::create(generate_name("HorizontalProjectionOverride"), s_horizontal_projection_override_names)};
    const ModCombo::Ptr m_vertical_projection_override{ModCombo::create(generate_name("VerticalProjectionOverride"), s_vertical_projection_override_names)};
    const ModToggle::Ptr m_grow_rectangle_for_projection_cropping{ModToggle::create(generate_name("GrowRectangleForProjectionCropping"), false)};
    const ModCombo::Ptr m_sync_mode{ ModCombo::create(generate_name("SynchronizationMode"), s_sync_mode_names, 2) };

    const ModToggle::Ptr m_use_uint64{ModToggle::create(generate_name("AFW_UseUINT64"), false)};
    const ModToggle::Ptr m_clear_before_framewarp{ModToggle::create(generate_name("AFW_ClearBeforeFramewarp"), false)};
    const ModToggle::Ptr m_fix_object_motion_vector{ModToggle::create(generate_name("AFW_FixObjectMotionVector"), true)};
    const ModSlider::Ptr m_fix_object_motion_range{ModSlider::create(generate_name("AFW_FixObjectMotionRange"), 0.0f, 10.0f, 3.0f)};
    const ModToggle::Ptr m_ultra_responsive{ModToggle::create(generate_name("AFW_UltraResponsive"), false)};
    const ModToggle::Ptr m_fix_moving_object_brightness_flickering{ModToggle::create(generate_name("AFW_FixMovingObjectBrightnessFlickering"), false)};
    const ModToggle::Ptr m_enable_sharpening{ModToggle::create(generate_name("AFW_EnableSharpening"), false)};
    const ModSlider::Ptr m_sharpness{ModSlider::create(generate_name("AFW_Sharpness"), 0.0f, 1.0f, 0.6f)};
    const ModToggle::Ptr m_framewarp_debug{ModToggle::create(generate_name("AFW_FramewarpDebug"), false)};
    const ModSlider::Ptr m_ignore_motion_threshold{ModSlider::create(generate_name("AFW_IgnoreMotionThreshold"), 0.1f, 100.0f, 2.5f)};
    const ModCombo::Ptr m_framewarp_mode{ModCombo::create(generate_name("AFW_FramewarpMode"),
        {
            "None",
            "AlternateEyeWarping",
            "PreviousFrameWarping",
            "CombinedWarping"
        },
        (int)FrameWarpMode::CombinedWarping)
    };
    const ModCombo::Ptr m_framewarp_shading_rate{ModCombo::create(generate_name("AFW_ShadingRate"),
        {
            "1 X 1 (Full Resolution)",
            "1 X 2 (Full Width x Half Height)",
            "1 X 4 (Full Width x Quarter Height)",
            "2 X 1 (Half Width x Full Height)",
            "2 X 2 (Half Width x Half Height)",
            "2 X 4 (Half Width x Quarter Height)",
            "4 X 1 (Quarter Width x Full Height)",
            "4 X 2 (Quarter Width x Half Height)",
            "4 X 4 (Quarter Width x Quarter Height)"
        },
        (int)0)
    };

    // Snap turn settings and globals
    void gamepad_snapturn(XINPUT_STATE& state);
    void process_snapturn();
    
    const ModToggle::Ptr m_snapturn{ ModToggle::create(generate_name("SnapTurn"), false) };
    const ModSlider::Ptr m_snapturn_joystick_deadzone{ ModSlider::create(generate_name("SnapturnJoystickDeadzone"), 0.01f, 0.99f, 0.2f) };
    const ModInt32::Ptr m_snapturn_angle{ ModSliderInt32::create(generate_name("SnapturnTurnAngle"), 1, 359, 45) };
    bool m_snapturn_on_frame{false};
    bool m_snapturn_left{false};
    bool m_was_snapturn_run_on_input{false};

    const ModSlider::Ptr m_controller_pitch_offset{ ModSlider::create(generate_name("ControllerPitchOffset"), -90.0f, 90.0f, 0.0f) };

    // Aim method and movement orientation are not the same thing, but they can both have the same options
    const ModCombo::Ptr m_aim_method{ ModCombo::create(generate_name("AimMethod"), s_aim_method_names, AimMethod::GAME) };
    const ModCombo::Ptr m_movement_orientation{ ModCombo::create(generate_name("MovementOrientation"), s_aim_method_names, AimMethod::GAME) };
    AimMethod m_previous_aim_method{ AimMethod::GAME };
    const ModToggle::Ptr m_aim_use_pawn_control_rotation{ ModToggle::create(generate_name("AimUsePawnControlRotation"), false) };
    const ModToggle::Ptr m_aim_modify_player_control_rotation{ ModToggle::create(generate_name("AimModifyPlayerControlRotation"), false) };
    const ModToggle::Ptr m_aim_multiplayer_support{ ModToggle::create(generate_name("AimMPSupport"), false) };
    const ModToggle::Ptr m_aim_interp{ ModToggle::create(generate_name("AimInterp"), true, true) };
    const ModSlider::Ptr m_aim_speed{ ModSlider::create(generate_name("AimSpeed"), 0.01f, 25.0f, 15.0f) };
    const ModToggle::Ptr m_dpad_shifting{ ModToggle::create(generate_name("DPadShifting"), true) };
    const ModCombo::Ptr m_dpad_shifting_method{ ModCombo::create(generate_name("DPadShiftingMethod"), s_dpad_method_names, DPadMethod::RIGHT_TOUCH) };
    
    struct DPadGestureState {
        std::recursive_mutex mtx{};
        enum Direction : uint8_t {
            NONE,
            UP = 1 << 0,
            RIGHT = 1 << 1,
            DOWN = 1 << 2,
            LEFT = 1 << 3,
        };
        uint8_t direction{NONE};
    } m_dpad_gesture_state{};

    //const ModToggle::Ptr m_headlocked_aim{ ModToggle::create(generate_name("HeadLockedAim"), false) };
    //const ModToggle::Ptr m_headlocked_aim_controller_based{ ModToggle::create(generate_name("HeadLockedAimControllerBased"), false) };
    const ModSlider::Ptr m_motion_controls_inactivity_timer{ ModSlider::create(generate_name("MotionControlsInactivityTimer"), 30.0f, 100.0f, 30.0f) };
    const ModSlider::Ptr m_joystick_deadzone{ ModSlider::create(generate_name("JoystickDeadzone"), 0.01f, 0.9f, 0.2f) };
    const ModSlider::Ptr m_camera_forward_offset{ ModSlider::create(generate_name("CameraForwardOffset"), -4000.0f, 4000.0f, 0.0f) };
    const ModSlider::Ptr m_camera_right_offset{ ModSlider::create(generate_name("CameraRightOffset"), -4000.0f, 4000.0f, 0.0f) };
    const ModSlider::Ptr m_camera_up_offset{ ModSlider::create(generate_name("CameraUpOffset"), -4000.0f, 4000.0f, 0.0f) };
    const ModSlider::Ptr m_camera_fov_distance_multiplier{ ModSlider::create(generate_name("CameraFOVDistanceMultiplier"), 0.00f, 1000.0f, 0.0f) };
    const ModSlider::Ptr m_world_scale{ ModSlider::create(generate_name("WorldScale"), 0.01f, 10.0f, 1.0f) };
    const ModSlider::Ptr m_depth_scale{ ModSlider::create(generate_name("DepthScale"), 0.01f, 1.0f, 1.0f) };

    const ModToggle::Ptr m_ghosting_fix{ ModToggle::create(generate_name("GhostingFix"), true) };
    const ModToggle::Ptr m_native_stereo_fix{ ModToggle::create(generate_name("NativeStereoFix"), false) };
    const ModToggle::Ptr m_native_stereo_fix_same_pass{ ModToggle::create(generate_name("NativeStereoFixSamePass"), true) };

    const ModSlider::Ptr m_custom_z_near{ ModSlider::create(generate_name("CustomZNear"), 0.001f, 100.0f, 0.01f, true) };
    const ModToggle::Ptr m_custom_z_near_enabled{ ModToggle::create(generate_name("EnableCustomZNear"), false, true) };

    const ModToggle::Ptr m_splitscreen_compatibility_mode{ ModToggle::create(generate_name("Compatibility_SplitScreen"), false, true) };
    const ModInt32::Ptr m_splitscreen_view_index{ ModInt32::create(generate_name("SplitscreenViewIndex"), 0, true) };

    const ModToggle::Ptr m_sceneview_compatibility_mode{ ModToggle::create(generate_name("Compatibility_SceneView"), false, true) };
    const ModToggle::Ptr m_compatibility_single_view_render_target{ ModToggle::create(generate_name("Compatibility_SingleViewRenderTarget"), false, true) };

    const ModToggle::Ptr m_compatibility_skip_pip{ ModToggle::create(generate_name("Compatibility_SkipPostInitProperties"), false, true) };
    const ModToggle::Ptr m_compatibility_skip_uobjectarray_init{ ModToggle::create(generate_name("Compatibility_SkipUObjectArrayInit"), false, true) };

    const ModToggle::Ptr m_compatibility_ahud{ ModToggle::create(generate_name("Compatibility_AHUD"), false, true) };

    // Keybinds
    const ModKey::Ptr m_keybind_recenter{ ModKey::create(generate_name("RecenterViewKey")) };
    const ModKey::Ptr m_keybind_recenter_horizon{ ModKey::create(generate_name("RecenterHorizonKey")) };
    const ModKey::Ptr m_keybind_set_standing_origin{ ModKey::create(generate_name("ResetStandingOriginKey")) };

    const ModKey::Ptr m_keybind_load_camera_0{ ModKey::create(generate_name("LoadCamera0Key")) };
    const ModKey::Ptr m_keybind_load_camera_1{ ModKey::create(generate_name("LoadCamera1Key")) };
    const ModKey::Ptr m_keybind_load_camera_2{ ModKey::create(generate_name("LoadCamera2Key")) };

    const ModKey::Ptr m_keybind_toggle_2d_screen{ ModKey::create(generate_name("Toggle2DScreenKey")) };
    const ModKey::Ptr m_keybind_disable_vr{ ModKey::create(generate_name("DisableVRKey")) };
    bool m_disable_vr{false}; // definitely should not be persistent

    const ModKey::Ptr m_keybind_toggle_gui{ ModKey::create(generate_name("ToggleSlateGUIKey")) };
    
    const ModString::Ptr m_requested_runtime_name{ ModString::create("Frontend_RequestedRuntime", "unset") };

    const ModToggle::Ptr m_lerp_camera_pitch{ ModToggle::create(generate_name("LerpCameraPitch"), false) };
    const ModToggle::Ptr m_lerp_camera_yaw{ ModToggle::create(generate_name("LerpCameraYaw"), false) };
    const ModToggle::Ptr m_lerp_camera_roll{ ModToggle::create(generate_name("LerpCameraRoll"), false) };
    const ModSlider::Ptr m_lerp_camera_speed{ ModSlider::create(generate_name("LerpCameraSpeed"), 0.01f, 10.0f, 1.0f) };

    // ------------------------------------------------------------------------
    // Flat 3D monitor mode (FLAT3D runtime) settings. Kept as one contiguous
    // block for upstream-rebase friendliness. Depth/Convergence/ReferenceFoV
    // form one calibrated triple: depth & convergence are defined AT the
    // reference FoV; separation is auto-scaled by tan(gameFov/2)/tan(refFov/2)
    // every frame (always on).
    static const inline std::vector<std::string> s_flat3d_output_mode_names{
        "Side by Side",
        "Top and Bottom",
        "Row Interlaced",
        "Column Interlaced",
        "Checkerboard",
        "LeiaSR (SR display)",
        "Frame Packed 720p60",
        "Frame Packed 1080p24",
        "Frame Packed 1080p60",
        "Dual Display",
        "Dual Display (Flip)",
        "Katanga (VR shared texture)",
        "Anaglyph Red/Cyan",
        "Anaglyph Red/Cyan (Dubois)",
        "Anaglyph Red/Cyan (Deghosted)",
        "Anaglyph Red/Cyan (Compromise)",
        "Anaglyph Green/Magenta",
        "Anaglyph Green/Magenta (Dubois)",
        "Anaglyph Green/Magenta (Deghosted)",
        "Anaglyph Blue/Amber",
    };
    static const inline std::vector<std::string> s_flat3d_crosshair_mode_names{
        "Off",
        "Game Crosshair",
        "Laser Sight",
    };
    static const inline std::vector<std::string> s_flat3d_vsync_names{
        "Use In-Game Setting",
        "Force On",
        "No-Tear Fast",
    };
    static const inline std::vector<std::string> s_flat3d_hud_depth_names{
        "Flat (GUI Depth)",
        "Depth-Adaptive (Auto-Classified)",
        "World Markers (Auto-Detected)",
    };

    static const inline std::vector<std::string> s_flat3d_cursor_mode_names{
        "None",
        "GUI Depth",
        "Geometry (Under Cursor)",
    };

    // Full-SbS UI aspect handling. Only engages when the game's UI target is a
    // different aspect than the per-eye slice (the double-wide 32:9 case); on a
    // normal 16:9 / half-SbS output every mode is a no-op.
    //   Crop    - cover: sample the central per-eye-aspect slice, fill the eye
    //             (correct for games that CONSTRAIN their HUD centrally, e.g. SMT5V)
    //   Fit     - contain: scale the whole UI to fit, letterboxed (shows the FULL
    //             menu at correct aspect for games that SPREAD it full-width)
    //   Stretch - no crop: sample 1:1, accept the squish (full coverage)
    static const inline std::vector<std::string> s_flat3d_ui_aspect_names{
        "Crop to Center",
        "Fit (Letterbox)",
        "Stretch to Fill",
    };


    // Per-eye 3D render resolution as a fraction of the native output size.
    // Auto preserves the game's own requested resolution (pre-rewrite) — but a
    // game that boots already at native (or persists native after our
    // borderless/native hold) never issues a sub-native request, so Auto
    // degrades to native-per-eye. The explicit fractions override everything.
    static const inline std::vector<std::string> s_flat3d_render_scale_names{
        "Auto (In-Game Resolution)",
        "100% of Display",
        "75% of Display",
        "67% of Display",
        "50% of Display",
        "33% of Display",
    };
    static constexpr inline std::array<float, 6> s_flat3d_render_scale_values{
        0.0f, 1.0f, 0.75f, 2.0f / 3.0f, 0.5f, 1.0f / 3.0f,
    };

    const ModCombo::Ptr m_flat3d_output_mode{ ModCombo::create(generate_name("Flat3D_OutputMode"), s_flat3d_output_mode_names) };
    const ModCombo::Ptr m_flat3d_render_scale{ ModCombo::create(generate_name("Flat3D_RenderResolution"), s_flat3d_render_scale_names, 0) };
    static const inline std::vector<std::string> s_flat3d_render_scale_stage_names{
        "Primary (r.ScreenPercentage)",
        "Secondary (stacks with DLSS/TSR)",
        "Stereo Render Target (legacy)",
    };
    const ModCombo::Ptr m_flat3d_render_scale_stage{ ModCombo::create(generate_name("Flat3D_RenderResolutionStage"), s_flat3d_render_scale_stage_names, 0) };
    // Auto-detected each frame in the native-output block: true when the output
    // is a real full-SbS double-wide panel (SbS mode on a ~32:9 display). Drives
    // Native Render + Upscale automatically (spoof the engine to render native
    // per-eye + top-left UI crop); no user toggle. False on 16:9 / half-SbS.
    std::atomic<bool> m_flat3d_full_sbs{ false };
    const ModToggle::Ptr m_flat3d_eye_swap{ ModToggle::create(generate_name("Flat3D_EyeSwap"), false) };
    // Present-interval override. The 2x mode disables vsync AND caps the game
    // at twice the display refresh so AFR/Synced Sequential update each eye
    // at the full refresh rate.
    // Default: No-Tear Fast (2) — on flip-model swapchains (all DX12, i.e. the
    // common case) it is tear-free AND presents both AFR eye frames per
    // refresh (t.MaxFPS auto-caps at 2x refresh under AFR, 1x under native).
    // Force On stays for DX11 blit-model exclusive fullscreen, which tears at
    // interval 0 no matter the flags.
    const ModCombo::Ptr m_flat3d_vsync{ ModCombo::create(generate_name("Flat3D_VSyncOverride"), s_flat3d_vsync_names, 2) };
    // HDR swapchains (PQ/scRGB) wash out the SDR-defined 3D output modes and
    // color correction — ask the engine to switch HDR output off while on.
    const ModToggle::Ptr m_flat3d_force_sdr{ ModToggle::create(generate_name("Flat3D_ForceSDR"), true) };
    // Scales the Flat3D render FoV. The projection is rebuilt each frame from the
    // GAME's live FoV (APlayerCameraManager::GetFOVAngle, so ADS zoom and cine
    // cameras still drive it); this multiplies that, it does not replace it.
    // 1.0 = untouched. Scales the HORIZONTAL tangent, and the vertical follows
    // through the per-eye aspect, so it is a symmetric zoom with no stretch.
    // NOTE: it is a tangent scale, not a degree scale — at a 90 deg hFoV, 2.0
    // gives ~127 deg, not 180.
    // Which axis APlayerCameraManager::GetFOVAngle refers to. Auto reads UE's own
    // EAspectRatioAxisConstraint; the forced options are the escape hatch for
    // games that override it per camera component.
    static const inline std::vector<std::string> s_flat3d_fov_axis_names{
        "Auto (engine constraint)",
        "Horizontal",
        "Vertical",
    };
    const ModCombo::Ptr m_flat3d_fov_axis{ ModCombo::create(generate_name("Flat3D_FOVAxis"), s_flat3d_fov_axis_names, 0) };
    const ModSlider::Ptr m_flat3d_fov_multiplier{ ModSlider::create(generate_name("Flat3D_FOVMultiplier"), 0.5f, 3.0f, 1.0f) };
    const ModSlider::Ptr m_flat3d_depth{ ModSlider::create(generate_name("Flat3D_Depth"), 0.0f, 0.5f, 0.1f) };
    const ModSlider::Ptr m_flat3d_convergence{ ModSlider::create(generate_name("Flat3D_Convergence"), 0.001f, 5.0f, 1.0f) };
    const ModSlider::Ptr m_flat3d_reference_fov{ ModSlider::create(generate_name("Flat3D_ReferenceFOV"), 40.0f, 140.0f, 90.0f) };
    const ModToggle::Ptr m_flat3d_autoconv_enabled{ ModToggle::create(generate_name("Flat3D_AutoConvergence"), false) };
    const ModSlider::Ptr m_flat3d_autoconv_target_disparity{ ModSlider::create(generate_name("Flat3D_AutoConvTargetDisparity"), 0.001f, 0.03f, 0.005f) };
    const ModSlider::Ptr m_flat3d_autoconv_smoothing{ ModSlider::create(generate_name("Flat3D_AutoConvSmoothing"), 0.005f, 0.25f, 0.08f) };
    // Floor for the AUTO convergence pull-in only (the manual slider is not
    // clamped by this) — stops single close-flyby objects from dragging the
    // screen plane absurdly close.
    const ModSlider::Ptr m_flat3d_autoconv_min_conv{ ModSlider::create(generate_name("Flat3D_AutoConvMinConvergence"), 0.05f, 10.0f, 0.5f) };
    const ModToggle::Ptr m_flat3d_autoconv_logging{ ModToggle::create(generate_name("Flat3D_AutoConvLogging"), false, true) };
    const ModCombo::Ptr m_flat3d_crosshair_mode{ ModCombo::create(generate_name("Flat3D_CrosshairMode"), s_flat3d_crosshair_mode_names, 1) };
    const ModToggle::Ptr m_flat3d_crosshair_adaptive{ ModToggle::create(generate_name("Flat3D_CrosshairAdaptive"), true) };
    const ModSlider::Ptr m_flat3d_crosshair_region_radius{ ModSlider::create(generate_name("Flat3D_CrosshairRegionRadius"), 0.01f, 0.15f, 0.04f) };
    // Vertical center of the game-crosshair region in output UV (0 = top, 0.5 = screen center, 1 = bottom).
    const ModSlider::Ptr m_flat3d_crosshair_region_center_y{ ModSlider::create(generate_name("Flat3D_CrosshairRegionCenterY"), 0.0f, 1.0f, 0.5f) };
    const ModInt32::Ptr m_flat3d_crosshair_size{ ModSliderInt32::create(generate_name("Flat3D_CrosshairSize"), 4, 64, 8) };
    const ModInt32::Ptr m_flat3d_crosshair_color{ ModInt32::create(generate_name("Flat3D_CrosshairColorARGB"), (int32_t)0xFFFFFFFF, true) };
    const ModSlider::Ptr m_flat3d_crosshair_static_depth{ ModSlider::create(generate_name("Flat3D_CrosshairStaticDepth"), 0.1f, 100.0f, 2.0f) };
    // Replaces the system cursor with a per-eye cursor at GUI depth while
    // the game shows a mouse cursor (the hardware cursor is composited flat
    // by the OS — one copy at screen depth — which breaks in stereo).
    const ModCombo::Ptr m_flat3d_cursor_mode{ ModCombo::create(generate_name("Flat3D_CursorMode"), s_flat3d_cursor_mode_names, 0) };
    const ModInt32::Ptr m_flat3d_cursor_size{ ModSliderInt32::create(generate_name("Flat3D_CursorSize"), 8, 96, 32) };
    // HUD depth mode: flat plane at GUI depth, per-pixel depth-adaptive
    // (SceneDepthZ sampled below each pixel), or world-marker anchors from
    // hooked engine projection calls. Non-flat modes fall back to flat while
    // the game shows a mouse cursor (menus).
    const ModCombo::Ptr m_flat3d_hud_depth_mode{ ModCombo::create(generate_name("Flat3D_HUDDepthMode"), s_flat3d_hud_depth_names) };
    const ModSlider::Ptr m_flat3d_hud_marker_radius{ ModSlider::create(generate_name("Flat3D_HUDMarkerRadius"), 0.02f, 0.15f, 0.06f) };
    // Depth-adaptive: how far around a classified tile the depth shift
    // extends (fraction of screen width) — covers the whole icon + text.
    const ModSlider::Ptr m_flat3d_hud_icon_radius{ ModSlider::create(generate_name("Flat3D_HUDIconRadius"), 0.015f, 0.125f, 0.035f) };
    // Depth-adaptive: extend the icon-region dilation VERTICALLY, the way a
    // marker's leader-line stem hangs, so a thin stem inherits the icon's
    // classification + depth without the sideways bleed a bigger symmetric
    // radius causes. Signed fraction of screen width (converted to tiles like
    // the radius): >0 stem hangs DOWN, <0 hangs UP, 0 = off.
    const ModSlider::Ptr m_flat3d_hud_stem_reach{ ModSlider::create(generate_name("Flat3D_HUDStemReach"), -0.25f, 0.25f, 0.0f) };
    // DELIBERATELY absent from m_options, so it resets to off every session:
    // a debug overlay should not persist into a normal play session. Note that
    // absence from m_options is the ONLY way to express "do not save" - the
    // trailing `true` here is advanced_option, which affects UI visibility and
    // nothing else - so a deliberate omission is indistinguishable from the
    // accidental kind that makes a setting silently revert on every injection.
    // Leave it out on purpose; don't "fix" it.
    const ModToggle::Ptr m_flat3d_hud_debug{ ModToggle::create(generate_name("Flat3D_HUDDepthDebug"), false, true) };
    // Color-gated UI alpha: zero the redirected UI's alpha where it has ~no
    // color. Rescues the "UI Invert Alpha 0.5" workaround (P3R battles): that
    // collapses all alpha to a flat 0.5, tinting the scene through the EMPTY
    // UI regions; drawn UI has color, empty screen doesn't. 0 = off.
    const ModSlider::Ptr m_flat3d_ui_color_gate{ ModSlider::create(generate_name("Flat3D_UIColorGate"), 0.0f, 0.25f, 0.0f) };
    // Scene-depth source for the Adaptive Crosshair / HUD-depth features. OFF
    // (default) uses the safe API-level per-draw GameDepthCapture. ON reads the
    // engine's own render-target pool (SceneDepthZ by name), which finds a depth
    // buffer allocated once at load — invisible to the API path (e.g. SMT5V) —
    // but installs an inline FindFreeElement hook that crashes a few titles
    // (Jedi: Survivor), so it is opt-in per game.
    const ModToggle::Ptr m_flat3d_use_engine_depth{ ModToggle::create(generate_name("Flat3D_UseEngineDepth"), false) };
    static const inline std::vector<std::string> s_flat3d_depth_source_names {
        "Per-Draw Capture (Default)",
        "Engine Pool (SceneDepthZ)",
        "DSV Observer (D3D12)",
        "DLSS Depth",
    };
    // Successor to Flat3D_UseEngineDepth (kept above for config back-compat):
    // adds the hook-free D3D12 DSV-observer snapshot as a third source.
    const ModCombo::Ptr m_flat3d_depth_source{ ModCombo::create(generate_name("Flat3D_DepthSource"), s_flat3d_depth_source_names, FLAT3D_DEPTH_PER_DRAW) };
    // Mode-1 (Depth-Adaptive HUD) false-positive rejection — keeps animated but
    // screen-fixed HUD (minimap radar, gauges) and permanent panels from being
    // mistaken for world-tracking UI. See the classify shader in Flat3DShaders.
    const ModSlider::Ptr m_flat3d_hud_trans_gate{ ModSlider::create(generate_name("Flat3D_HUDTransGate"), 0.02f, 0.30f, 0.06f) };
    const ModSlider::Ptr m_flat3d_hud_rot_gate{ ModSlider::create(generate_name("Flat3D_HUDRotGate"), 0.01f, 0.20f, 0.05f) };
    const ModSlider::Ptr m_flat3d_hud_trans_floor{ ModSlider::create(generate_name("Flat3D_HUDTransFloor"), 0.0005f, 0.02f, 0.0015f) };
    const ModToggle::Ptr m_flat3d_hud_occlude_panels{ ModToggle::create(generate_name("Flat3D_HUDOccludePanels"), true) }; // #2
    const ModSlider::Ptr m_flat3d_hud_occ_gate{ ModSlider::create(generate_name("Flat3D_HUDOccGate"), 0.5f, 0.99f, 0.85f) };
    const ModInt32::Ptr m_flat3d_hud_panel_halo{ ModSliderInt32::create(generate_name("Flat3D_HUDPanelHalo"), 0, 6, 2) };
    // Central marker safe-zone: occupancy suppression is disabled inside this
    // box (half-extents in output UV from screen center) so world markers, which
    // live centrally, are never flattened when they linger. Panels sit outside it.
    const ModSlider::Ptr m_flat3d_hud_occ_safe_hw{ ModSlider::create(generate_name("Flat3D_HUDOccSafeHW"), 0.0f, 0.5f, 0.35f) };
    const ModSlider::Ptr m_flat3d_hud_occ_safe_hh{ ModSlider::create(generate_name("Flat3D_HUDOccSafeHH"), 0.0f, 0.5f, 0.35f) };
    // #5 Areal-fill reject: a large contiguous UI fill (menu backdrop / blurred
    // scrim) is never a world-marker. Coverage is measured over a +/-radius tile
    // block of the current UI; at/above the gate the tile is forced flat. Spatial
    // and immediate (no occupancy lag), and it works dead-center, unlike the
    // occupancy safe-zone which protects a central backdrop.
    const ModToggle::Ptr m_flat3d_hud_reject_fills{ ModToggle::create(generate_name("Flat3D_HUDRejectFills"), true) };
    const ModInt32::Ptr m_flat3d_hud_fill_radius{ ModSliderInt32::create(generate_name("Flat3D_HUDFillRadius"), 1, 16, 6) };
    const ModSlider::Ptr m_flat3d_hud_fill_gate{ ModSlider::create(generate_name("Flat3D_HUDFillGate"), 0.3f, 0.95f, 0.6f) };
    // #4 Up to 4 rectangular exclusion zones (always-flat HUD). cx/cy = center in
    // output UV, hw/hh = half extents; a zone with hw or hh == 0 is disabled.
    const std::array<ModSlider::Ptr, 4> m_flat3d_hud_excl_cx{
        ModSlider::create(generate_name("Flat3D_HUDExcl0CX"), 0.0f, 1.0f, 0.0f),
        ModSlider::create(generate_name("Flat3D_HUDExcl1CX"), 0.0f, 1.0f, 0.0f),
        ModSlider::create(generate_name("Flat3D_HUDExcl2CX"), 0.0f, 1.0f, 0.0f),
        ModSlider::create(generate_name("Flat3D_HUDExcl3CX"), 0.0f, 1.0f, 0.0f) };
    const std::array<ModSlider::Ptr, 4> m_flat3d_hud_excl_cy{
        ModSlider::create(generate_name("Flat3D_HUDExcl0CY"), 0.0f, 1.0f, 0.0f),
        ModSlider::create(generate_name("Flat3D_HUDExcl1CY"), 0.0f, 1.0f, 0.0f),
        ModSlider::create(generate_name("Flat3D_HUDExcl2CY"), 0.0f, 1.0f, 0.0f),
        ModSlider::create(generate_name("Flat3D_HUDExcl3CY"), 0.0f, 1.0f, 0.0f) };
    const std::array<ModSlider::Ptr, 4> m_flat3d_hud_excl_hw{
        ModSlider::create(generate_name("Flat3D_HUDExcl0HW"), 0.0f, 0.5f, 0.0f),
        ModSlider::create(generate_name("Flat3D_HUDExcl1HW"), 0.0f, 0.5f, 0.0f),
        ModSlider::create(generate_name("Flat3D_HUDExcl2HW"), 0.0f, 0.5f, 0.0f),
        ModSlider::create(generate_name("Flat3D_HUDExcl3HW"), 0.0f, 0.5f, 0.0f) };
    const std::array<ModSlider::Ptr, 4> m_flat3d_hud_excl_hh{
        ModSlider::create(generate_name("Flat3D_HUDExcl0HH"), 0.0f, 0.5f, 0.0f),
        ModSlider::create(generate_name("Flat3D_HUDExcl1HH"), 0.0f, 0.5f, 0.0f),
        ModSlider::create(generate_name("Flat3D_HUDExcl2HH"), 0.0f, 0.5f, 0.0f),
        ModSlider::create(generate_name("Flat3D_HUDExcl3HH"), 0.0f, 0.5f, 0.0f) };
    // Full-screen-GUI detector: flatten the HUD/crosshair depth once the
    // redirected UI covers at least this % of the screen (cursor-independent —
    // catches controller-driven menus, map/inventory screens). 0 disables the
    // coverage signal; the mouse-cursor and game-paused signals still apply.
    const ModInt32::Ptr m_flat3d_fullscreen_coverage{ ModSliderInt32::create(generate_name("Flat3D_FullscreenUICoveragePct"), 0, 100, 60) };
    // GUI / UEVR-menu depth as a MULTIPLE of the convergence distance:
    // 1.0 = exactly at the screen plane (zero shift — never clips at any
    // convergence), < 1 pops out, > 1 sits behind. Relative-to-convergence
    // keeps the overlays' disparity (and the cover-zoom that eats their
    // edges) from blowing up when convergence moves (slider or auto).
    const ModSlider::Ptr m_flat3d_gui_depth{ ModSlider::create(generate_name("Flat3D_GUIDepthFactor"), 0.25f, 8.0f, 1.0f) };
    const ModSlider::Ptr m_flat3d_menu_depth{ ModSlider::create(generate_name("Flat3D_MenuDepthFactor"), 0.25f, 4.0f, 1.0f) };
    const ModSlider::Ptr m_flat3d_hdr_paper_white{ ModSlider::create(generate_name("Flat3D_HDRPaperWhiteNits"), 80.0f, 400.0f, 200.0f) };
    // Diagnostic: enable the D3D12 debug layer's InfoQueue and log its validation
    // messages (resource-barrier state mismatches, etc.) to the UEVR log. Off by
    // default; requires the debug layer to be present on the game's device (early
    // injection with UEVR_D3D12_DEBUG=1, or dxcpl.exe "Force On"). Used to pin the
    // SceneDepthZ barrier mismatch behind the depth-feature crashes.
    const ModToggle::Ptr m_flat3d_d3d12_debug_layer{ ModToggle::create(generate_name("Flat3D_D3D12DebugLayer"), false, true) };

    // Ghost reduction: range compression in the compose shader to reduce visible
    // stereo crosstalk. 1.0 == off (exact no-op). This replaced the VRto3D-style
    // lift/gamma/gain colour correction, which existed to fight ghosting and
    // needed eleven sliders to do what this does with one.
    const ModSlider::Ptr m_flat3d_ghost_contrast{ ModSlider::create(generate_name("Flat3D_GhostContrast"), 0.5f, 1.0f, 1.0f) };
    // Black lift - the other way to give a display's own crosstalk cancellation
    // room to work. Cancellation clips at the bottom, so lifting the floor
    // targets that directly and costs black level instead of contrast. 0.0 == off.
    const ModSlider::Ptr m_flat3d_ghost_lift{ ModSlider::create(generate_name("Flat3D_GhostLift"), 0.0f, 0.25f, 0.0f) };

    // OpenTrack head tracking (v2). UDP receiver feeds a small head-coupled
    // perspective offset; separate look (yaw/pitch coupling) + parallax gains.
    const ModToggle::Ptr m_flat3d_opentrack_enabled{ ModToggle::create(generate_name("Flat3D_OpenTrack"), false) };
    const ModInt32::Ptr m_flat3d_opentrack_port{ ModInt32::create(generate_name("Flat3D_OpenTrackPort"), 4242) };
    const ModSlider::Ptr m_flat3d_opentrack_pos_scale{ ModSlider::create(generate_name("Flat3D_OpenTrackPosScale"), 0.0f, 4.0f, 1.0f) };
    const ModSlider::Ptr m_flat3d_opentrack_rot_scale{ ModSlider::create(generate_name("Flat3D_OpenTrackRotScale"), 0.0f, 2.0f, 1.0f) };

    // Keybinds matching VRto3D's hotkeys/step sizes (Ctrl+F3/F4 depth, Ctrl+F5/F6 convergence).
    // ------------------------------------------------------------------------

    std::chrono::high_resolution_clock::time_point m_last_lerp_update{};

    struct DecoupledPitchData {
        mutable std::shared_mutex mtx{};
        glm::quat pre_flattened_rotation{};
    } m_decoupled_pitch_data{};

    struct CameraFreeze {
        glm::vec3 position{};
        glm::vec3 rotation{}; // euler
        bool position_frozen{false};
        bool rotation_frozen{false};

        bool position_wants_freeze{false};
        bool rotation_wants_freeze{false};
    } m_camera_freeze{};

    struct CameraLerp {
        glm::vec3 last_position{};
        glm::vec3 last_rotation{};
    } m_camera_lerp{};

    struct CameraData {
        glm::vec3 offset{};
        float world_scale{1.0f};
        bool decoupled_pitch{false};
        bool decoupled_pitch_ui_adjust{true};
    };
    std::array<CameraData, 3> m_camera_datas{};
    void save_cameras();
    void load_cameras();
    void load_camera(int index);
    void save_camera(int index);

public:
    VR() {
        m_options = {
            *m_flat3d_output_mode,
            *m_flat3d_render_scale,
            // m_options is what config_save writes and config_load reads. A
            // ModValue that is declared and drawn but missing here still works
            // for the session and silently reverts to its default on the next
            // injection, which reads as a save bug rather than a missing entry.
            *m_flat3d_render_scale_stage,
            *m_flat3d_eye_swap,
            *m_flat3d_vsync,
            *m_flat3d_force_sdr,
            *m_flat3d_fov_multiplier,
            *m_flat3d_fov_axis,
            *m_flat3d_depth,
            *m_flat3d_convergence,
            *m_flat3d_reference_fov,
            *m_flat3d_autoconv_enabled,
            *m_flat3d_autoconv_target_disparity,
            *m_flat3d_autoconv_smoothing,
            *m_flat3d_autoconv_min_conv,
            *m_flat3d_autoconv_logging,
            *m_flat3d_crosshair_mode,
            *m_flat3d_crosshair_adaptive,
            *m_flat3d_crosshair_region_radius,
            *m_flat3d_crosshair_region_center_y,
            *m_flat3d_crosshair_size,
            *m_flat3d_crosshair_color,
            *m_flat3d_crosshair_static_depth,
            *m_flat3d_hud_depth_mode,
            *m_flat3d_hud_marker_radius,
            *m_flat3d_hud_icon_radius,
            *m_flat3d_hud_stem_reach,
            *m_flat3d_ui_color_gate,
            *m_flat3d_use_engine_depth,
            *m_flat3d_depth_source,
            *m_flat3d_hud_trans_gate,
            *m_flat3d_hud_rot_gate,
            *m_flat3d_hud_trans_floor,
            *m_flat3d_hud_occlude_panels,
            *m_flat3d_hud_occ_gate,
            *m_flat3d_hud_panel_halo,
            *m_flat3d_hud_occ_safe_hw,
            *m_flat3d_hud_occ_safe_hh,
            *m_flat3d_hud_reject_fills,
            *m_flat3d_hud_fill_radius,
            *m_flat3d_hud_fill_gate,
            *m_flat3d_hud_excl_cx[0], *m_flat3d_hud_excl_cy[0], *m_flat3d_hud_excl_hw[0], *m_flat3d_hud_excl_hh[0],
            *m_flat3d_hud_excl_cx[1], *m_flat3d_hud_excl_cy[1], *m_flat3d_hud_excl_hw[1], *m_flat3d_hud_excl_hh[1],
            *m_flat3d_hud_excl_cx[2], *m_flat3d_hud_excl_cy[2], *m_flat3d_hud_excl_hw[2], *m_flat3d_hud_excl_hh[2],
            *m_flat3d_hud_excl_cx[3], *m_flat3d_hud_excl_cy[3], *m_flat3d_hud_excl_hw[3], *m_flat3d_hud_excl_hh[3],
            *m_flat3d_fullscreen_coverage,
            *m_flat3d_gui_depth,
            *m_flat3d_menu_depth,
            *m_flat3d_cursor_mode,
            *m_flat3d_cursor_size,
            *m_flat3d_hdr_paper_white,
            *m_flat3d_d3d12_debug_layer,
            *m_flat3d_ghost_contrast,
            *m_flat3d_ghost_lift,
            *m_flat3d_opentrack_enabled,
            *m_flat3d_opentrack_port,
            *m_flat3d_opentrack_pos_scale,
            *m_flat3d_opentrack_rot_scale,
            *m_rendering_method,
            *m_synced_afr_method,
            *m_extreme_compat_mode,
            *m_uncap_framerate,
            *m_disable_hdr_compositing,
            *m_disable_hzbocclusion,
            *m_disable_instance_culling,
            *m_desktop_fix,
            *m_enable_gui,
            *m_enable_depth,
            *m_decoupled_pitch,
            *m_decoupled_pitch_ui_adjust,
            *m_load_blueprint_code,
            *m_2d_screen_mode,
            *m_roomscale_movement,
            *m_roomscale_sweep,
            *m_swap_controllers,
            *m_horizontal_projection_override,
            *m_vertical_projection_override,
            *m_grow_rectangle_for_projection_cropping,
            *m_snapturn,
            *m_snapturn_joystick_deadzone,
            *m_snapturn_angle,
            *m_controller_pitch_offset,
            *m_aim_method,
            *m_movement_orientation,
            *m_aim_use_pawn_control_rotation,
            *m_aim_modify_player_control_rotation,
            *m_aim_multiplayer_support,
            *m_aim_speed,
            *m_aim_interp,
            *m_dpad_shifting,
            *m_dpad_shifting_method,
            *m_motion_controls_inactivity_timer,
            *m_joystick_deadzone,
            *m_camera_forward_offset,
            *m_camera_right_offset,
            *m_camera_up_offset,
            *m_world_scale,
            *m_depth_scale,
            *m_custom_z_near,
            *m_custom_z_near_enabled,
            *m_ghosting_fix,
            *m_native_stereo_fix,
            *m_native_stereo_fix_same_pass,
            *m_splitscreen_compatibility_mode,
            *m_splitscreen_view_index,
            *m_compatibility_skip_pip,
            *m_compatibility_skip_uobjectarray_init,
            *m_compatibility_ahud,
            *m_sceneview_compatibility_mode,
            *m_compatibility_single_view_render_target,
            *m_keybind_recenter,
            *m_keybind_recenter_horizon,
            *m_keybind_set_standing_origin,
            *m_keybind_load_camera_0,
            *m_keybind_load_camera_1,
            *m_keybind_load_camera_2,
            *m_keybind_toggle_2d_screen,
            *m_keybind_disable_vr,
            *m_keybind_toggle_gui,
            *m_requested_runtime_name,
            *m_show_fps,
            *m_show_statistics,
            *m_controllers_allowed,
            *m_lerp_camera_pitch,
            *m_lerp_camera_yaw,
            *m_lerp_camera_roll,
            *m_lerp_camera_speed,
            *m_sync_mode,
            *m_framewarp_mode,
            *m_fix_object_motion_vector,
            *m_fix_object_motion_range,
            *m_ultra_responsive,
            *m_fix_moving_object_brightness_flickering,
            *m_enable_sharpening,
            *m_sharpness,
            *m_framewarp_shading_rate
        };

        add_components_vr();
    }

private:
    bool m_stereo_emulation_mode{false}; // not a good config option, just for debugging
    bool m_wait_for_present{true};
    const ModToggle::Ptr m_controllers_allowed{ ModToggle::create(generate_name("ControllersAllowed"), true) };
    bool m_controller_test_mode{false};
    
    const ModToggle::Ptr m_show_fps{ ModToggle::create(generate_name("ShowFPSOverlay"), false) };
    bool m_show_fps_state{false};

    const ModToggle::Ptr m_show_statistics{ ModToggle::create(generate_name("ShowStatsOverlay"), false) };
    bool m_show_statistics_state{false};

    void update_statistics_overlay(sdk::UGameEngine* engine);

    int m_game_frame_count{};
    int m_frame_count{};
    int m_render_frame_count{};
    int m_last_frame_count{-1};
    int m_left_eye_frame_count{0};
    int m_right_eye_frame_count{0};

    bool m_submitted{false};

    // == 1 or == 0
    uint8_t m_left_eye_interval{0};
    uint8_t m_right_eye_interval{1};

    bool m_first_config_load{true};
    bool m_first_submit{true};
    bool m_is_d3d12{false};
    bool m_backbuffer_inconsistency{false};
    bool m_init_finished{false};
    bool m_has_hw_scheduling{false}; // hardware accelerated GPU scheduling
    bool m_spoofed_gamepad_connection{false};
    bool m_aim_temp_disabled{false};

    struct {
        bool draw{false};
        bool was_moving_left{false};
        bool was_moving_right{false};
        uint8_t page{0};
        uint8_t num_pages{3};
    } m_rt_modifier{};

    bool m_disable_projection_matrix_override{ false };
    bool m_disable_view_matrix_override{false};
    bool m_disable_backbuffer_size_override{false};

    uint32_t m_present_thread_id{};

    struct XInputContext {
        struct PadContext {
            using Func = std::function<void(const XINPUT_STATE&, bool is_vr_controller)>;
            std::optional<Func> update{};
            XINPUT_STATE state{};
        };

        PadContext gamepad{};
        PadContext vr_controller{};
        
        TracyLockable(std::recursive_mutex, mtx);

        struct VRState {
            class StickState {
            public:
                bool was_pressed(bool current_state) {
                    if (!current_state) {
                        is_pressed = false;
                        return false;
                    }

                    const auto now = std::chrono::steady_clock::now();
                    if (is_pressed && now - initial_press > std::chrono::milliseconds(500)) {
                        return true;
                    }

                    if (!is_pressed) {
                        initial_press = now;
                        is_pressed = true;
                        return true;
                    }

                    return false;
                } 
            
            private:
                std::chrono::steady_clock::time_point initial_press{};
                bool is_pressed{false};
            };

            StickState left_stick_up{};
            StickState left_stick_down{};
            StickState left_stick_left{};
            StickState left_stick_right{};
        } vr;

        void enqueue(bool is_vr_controller, const XINPUT_STATE& in_state, PadContext::Func func) {
            ZoneScopedN(__FUNCTION__);

            std::scoped_lock _{mtx};
            if (is_vr_controller) {
                vr_controller.update = func;
                vr_controller.state = in_state;
            } else {
                gamepad.update = func;
                gamepad.state = in_state;
            }
        }

        void update() {
            ZoneScopedN(__FUNCTION__);

            std::scoped_lock _{mtx};

            if (vr_controller.update) {
                (*vr_controller.update)(vr_controller.state, true);
                vr_controller.update.reset();
            }

            if (gamepad.update) {
                (*gamepad.update)(gamepad.state, false);
                gamepad.update.reset();
            }
        }

        bool headlocked_begin_held{false};
        bool menu_longpress_begin_held{false};
        std::chrono::steady_clock::time_point headlocked_begin{};
        std::chrono::steady_clock::time_point menu_longpress_begin{};
    } m_xinput_context{};

    static std::string actions_json;
    static std::string binding_rift_json;
    static std::string bindings_oculus_touch_json;
    static std::string binding_vive;
    static std::string bindings_vive_controller;
    static std::string bindings_knuckles;

    const std::unordered_map<std::string, std::string> m_binding_files {
        { "actions.json", actions_json },
        { "binding_rift.json", binding_rift_json },
        { "bindings_oculus_touch.json", bindings_oculus_touch_json },
        { "binding_vive.json", binding_vive },
        { "bindings_vive_controller.json", bindings_vive_controller },
        { "bindings_knuckles.json", bindings_knuckles }
    };

    friend class vrmod::D3D11Component;
    friend class vrmod::D3D12Component;
    friend class vrmod::OverlayComponent;
    friend class FFakeStereoRenderingHook;
};
