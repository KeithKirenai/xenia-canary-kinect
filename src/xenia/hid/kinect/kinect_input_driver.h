/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary Authors. All rights reserved.                  *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * Based on the experimental, in-progress Kinect HLE implementation by Weronika Saturday (@weronika-saturday).
 *
 * Kinect / NUI HID input driver — cross-platform implementation.
 *
 * Platform              Real hardware backend
 * ──────────────────────────────────────────────────────────────────────────
 * Windows               Kinect10.dll  (Kinect for Windows SDK 1.8)
 *                       Loaded dynamically; graceful fallback if absent.
 *
 * Linux / macOS         OpenNI2 + NiTE2 + freenect OpenNI2 bridge
 *                       libOpenNI2.so / libNiTE2.so loaded dynamically.
 *                       freenect bridge (libFreenectDriver.so) exposes the
 *                       sensor to OpenNI2 without requiring any proprietary
 *                       kernel driver — the standard libusb udev rules suffice.
 *
 * All platforms         Synthetic T-pose skeleton at 30 fps when no real
 *                       hardware / SDK is available.
 *
 * NiTE2 joint mapping vs Xbox 360 NUI joint mapping
 * ──────────────────────────────────────────────────
 * Both use 15/20 joints respectively. NiTE2 tracks 15 joints; we map them
 * to the 20-joint Xbox layout and mark the 5 unmapped joints as INFERRED
 * (interpolated from their neighbours).
 *
 * Joint               NiTE2 index   Xbox index
 * HEAD                0             3
 * NECK                1             2  (SHOULDER_CENTER)
 * LEFT_SHOULDER       2             4
 * RIGHT_SHOULDER      3             8
 * LEFT_ELBOW          4             5
 * RIGHT_ELBOW         5             9
 * LEFT_HAND           6             7  (skip WRIST → INFERRED)
 * RIGHT_HAND          7             11 (skip WRIST → INFERRED)
 * TORSO               8             1  (SPINE)
 * LEFT_HIP            9             12
 * RIGHT_HIP           10            16
 * LEFT_KNEE           11            13
 * RIGHT_KNEE          12            17
 * LEFT_FOOT           13            14 (skip ANKLE → INFERRED)
 * RIGHT_FOOT          14            18 (skip ANKLE → INFERRED)
 *
 * Unmapped Xbox joints filled by linear interpolation:
 *   HIP_CENTER       = midpoint(LEFT_HIP, RIGHT_HIP)
 *   WRIST_LEFT       = lerp(ELBOW_LEFT, HAND_LEFT, 0.5)
 *   WRIST_RIGHT      = lerp(ELBOW_RIGHT, HAND_RIGHT, 0.5)
 *   ANKLE_LEFT       = lerp(KNEE_LEFT, FOOT_LEFT, 0.5)
 *   ANKLE_RIGHT      = lerp(KNEE_RIGHT, FOOT_RIGHT, 0.5)
 * *****************************************************************************
 */

#ifndef XENIA_HID_KINECT_KINECT_INPUT_DRIVER_H_
#define XENIA_HID_KINECT_KINECT_INPUT_DRIVER_H_

#include <atomic>
#include <mutex>
#include <thread>

#include "xenia/hid/input_driver.h"
#include "xenia/ui/window_listener.h"

namespace xe {
namespace kernel {
class XEvent;
class XSemaphore;
class XTimer;
}  // namespace kernel
namespace hid {
namespace kinect {

// ---------------------------------------------------------------------------
// Xbox 360 NUI data structures (host byte order internally).
// Callers (xam_nui.cc) byte-swap when writing to guest memory.
// ---------------------------------------------------------------------------

constexpr uint32_t kNuiSkeletonCount = 6;
constexpr uint32_t kNuiSkeletonPositionCount = 20;

enum X_NUI_SKELETON_TRACKING_STATE : uint32_t {
  X_NUI_SKELETON_NOT_TRACKED = 0,
  X_NUI_SKELETON_POSITION_ONLY = 1,
  X_NUI_SKELETON_TRACKED = 2,
};

enum X_NUI_SKELETON_POSITION_TRACKING_STATE : uint32_t {
  X_NUI_SKELETON_POSITION_NOT_TRACKED = 0,
  X_NUI_SKELETON_POSITION_INFERRED = 1,
  X_NUI_SKELETON_POSITION_TRACKED = 2,
};

// Joint indices — standard Xbox 360 NUI skeleton layout.
enum X_NUI_SKELETON_POSITION_INDEX : uint32_t {
  NUI_JOINT_HIP_CENTER = 0,
  NUI_JOINT_SPINE,
  NUI_JOINT_SHOULDER_CENTER,
  NUI_JOINT_HEAD,
  NUI_JOINT_SHOULDER_LEFT,
  NUI_JOINT_ELBOW_LEFT,
  NUI_JOINT_WRIST_LEFT,
  NUI_JOINT_HAND_LEFT,
  NUI_JOINT_SHOULDER_RIGHT,
  NUI_JOINT_ELBOW_RIGHT,
  NUI_JOINT_WRIST_RIGHT,
  NUI_JOINT_HAND_RIGHT,
  NUI_JOINT_HIP_LEFT,
  NUI_JOINT_KNEE_LEFT,
  NUI_JOINT_ANKLE_LEFT,
  NUI_JOINT_FOOT_LEFT,
  NUI_JOINT_HIP_RIGHT,
  NUI_JOINT_KNEE_RIGHT,
  NUI_JOINT_ANKLE_RIGHT,
  NUI_JOINT_FOOT_RIGHT,
};

struct X_VECTOR4 {
  float x, y, z, w;
};

struct X_NUI_SKELETON_DATA {
  X_NUI_SKELETON_TRACKING_STATE eTrackingState;
  uint32_t dwTrackingID;
  uint32_t dwEnrollmentIndex;
  uint32_t dwUserIndex;
  X_VECTOR4 Position;
  X_VECTOR4 SkeletonPositions[kNuiSkeletonPositionCount];
  X_NUI_SKELETON_POSITION_TRACKING_STATE
  eSkeletonPositionTrackingState[kNuiSkeletonPositionCount];
  uint32_t dwQualityFlags;
};

struct X_NUI_SKELETON_FRAME {
  int64_t liTimeStamp;
  uint32_t dwFrameNumber;
  uint32_t dwFlags;
  X_VECTOR4 vFloorClipPlane;
  X_VECTOR4 vNormalToGravity;
  X_NUI_SKELETON_DATA SkeletonData[kNuiSkeletonCount];
};

// ---------------------------------------------------------------------------
// KinectInputDriver
// ---------------------------------------------------------------------------
class KinectInputDriver final : public InputDriver {
 public:
  explicit KinectInputDriver(xe::ui::Window* window, size_t window_z_order);
  ~KinectInputDriver() override;

  X_STATUS Setup() override;

  // Global singleton accessor — set in Setup(), cleared in destructor.
  // Used by xam_nui.cc to route XamNui* calls to the active driver.
  static KinectInputDriver* instance() { return instance_; }

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
  InputType GetInputType() const override { return InputType::Other; }

  // NUI API — called from xam_nui.cc.
  X_RESULT NuiInitialize(uint32_t flags);
  void NuiShutdown();
  X_RESULT NuiSkeletonGetNextFrame(uint32_t wait_ms,
                                   X_NUI_SKELETON_FRAME* out_frame);
  X_RESULT NuiCameraSetElevation(int32_t degrees);
  X_RESULT NuiCameraGetElevation(int32_t* out_degrees);

  // Video / Depth Image Streams
  X_RESULT NuiImageStreamOpen(uint32_t image_type, uint32_t resolution,
                              uint32_t flags, uint32_t frame_limit,
                              void* next_frame_event, void** out_stream_handle);
  X_RESULT NuiImageStreamGetNextFrame(void* stream_handle, uint32_t wait_ms,
                                      void** out_image_frame);
  X_RESULT NuiImageStreamReleaseFrame(void* stream_handle, void* image_frame);

  bool is_initialized() const { return initialized_.load(); }

  // Skeleton frame event — PollThread signals this when new data is ready.
  // The game creates an XEvent and waits on it; we signal it at 30fps.
  void SetSkeletonFrameEvent(xe::kernel::XEvent* event);
  void AddSyncEvent(xe::kernel::XEvent* event);
  // Registers an event the title created itself for its NUI worker queue.
  // Woken on every produced frame so the title's NUI workers can drain it.
  void AddNuiWakeEvent(xe::kernel::XEvent* event);
  void AddSyncSemaphore(xe::kernel::XSemaphore* sem);
  void AddSyncTimer(xe::kernel::XTimer* timer);

  // Set guest shared memory address where frames should be written directly
  void SetSharedSkeletonBuffer(uint8_t* guest_dst) { shared_guest_dst_ = guest_dst; }
  void SetSharedSkeletonBuffer2(uint8_t* guest_dst) { shared_guest_dst2_ = guest_dst; }
  // Note: there is deliberately no SetNuiDeviceObject(). The "device object" it
  // fed was a hardcoded guest address (0x83896510) paired with an invented
  // layout, and writing it corrupted live guest memory every frame. See the
  // removal note in kinect_input_driver.cc.

 private:
  // Synthetic skeleton (always available, no hardware needed).
  void BuildSyntheticFrame(X_NUI_SKELETON_FRAME* frame);

  // Background poll thread at 30 fps.
  void PollThread();

  // ── Windows backend (Kinect10.dll) ──────────────────────────────────────
  bool TryLoadWindowsSDK();
  void UnloadWindowsSDK();
  void PollWindowsSDK();

  // ── Linux/macOS backend (OpenNI2 + NiTE2) ───────────────────────────────
  bool TryLoadOpenNI2();
  void UnloadOpenNI2();
  void PollOpenNI2();

  // ── MediaPipe backend ───────────────────────────────────────────────────
  bool TryLoadMediaPipe();
  void UnloadMediaPipe();
  void PollMediaPipe();

  // ── libfreenect backend ───────────────────────────────────────────────────
  bool TryLoadLibFreenect();
  void UnloadLibFreenect();
  void PollLibFreenect();

  // Map a NiTE2 skeleton (raw float[15][3] positions + confidence[15]) into
  // our X_NUI_SKELETON_DATA, filling interpolated joints.
  void MapNiTE2Skeleton(const float positions[][3], const float confidence[],
                        uint32_t tracking_id, uint32_t user_index,
                        X_NUI_SKELETON_DATA* out);

  // ── State ────────────────────────────────────────────────────────────────
  std::atomic<bool> initialized_{false};
  std::atomic<bool> thread_running_{false};
  std::thread poll_thread_;
  std::mutex frame_mutex_;
  X_NUI_SKELETON_FRAME frame_latest_{};
  uint32_t frame_number_{0};
  bool new_frame_{false};
  uint32_t skeleton_event_handle_{0};  // XEvent handle to signal on new frame
  // Host event to signal
  xe::kernel::XEvent* skeleton_xevent_{nullptr};
  std::vector<xe::kernel::XEvent*> sync_events_;

  // Events the title creates itself for its NUI worker queue (after the
  // 0x2B001 bind, so they cannot be registered from a message handler). These
  // are woken every produced frame.
  std::vector<xe::kernel::XEvent*> nui_wake_events_;
  std::vector<xe::kernel::XSemaphore*> sync_semaphores_;
  std::vector<xe::kernel::XTimer*> sync_timers_;
  std::mutex sync_events_mutex_;
  std::atomic<uint8_t*> shared_guest_dst_{nullptr};
  std::atomic<uint8_t*> shared_guest_dst2_{nullptr};

  enum class Backend {
    None,
    Synthetic,
    WindowsSDK,
    OpenNI2,
    MediaPipe,
    LibFreenect
  } backend_{Backend::None};

  // Windows SDK function pointers (void* to avoid including NuiApi.h).
  void* win_module_{nullptr};
  void* win_NuiInitialize_{nullptr};
  void* win_NuiShutdown_{nullptr};
  void* win_NuiSkeletonGetNextFrame_{nullptr};
  void* win_NuiSkeletonTrackingEnable_{nullptr};
  void* win_NuiCameraSetElevation_{nullptr};
  void* win_NuiCameraGetElevation_{nullptr};
  void* win_NuiImageStreamOpen_{nullptr};
  void* win_NuiImageStreamGetNextFrame_{nullptr};
  void* win_NuiImageStreamReleaseFrame_{nullptr};

  // OpenNI2 / NiTE2 handles and function pointers.
  // We store opaque void* handles so the header stays SDK-free.
  void* oni_module_{nullptr};    // libOpenNI2.so / OpenNI2.dll
  void* nite_module_{nullptr};   // libNiTE2.so   / NiTE2.dll
  void* oni_device_{nullptr};    // openni::Device* (heap-allocated)
  void* nite_tracker_{nullptr};  // nite::UserTracker* (heap-allocated)

  // OpenNI2 function pointers used for init/shutdown only.
  void* pfn_oniInitialize_{nullptr};
  void* pfn_oniShutdown_{nullptr};

  // NiTE2 function pointers (C wrappers generated in .cc).
  void* pfn_niteInitialize_{nullptr};
  void* pfn_niteShutdown_{nullptr};
  void* pfn_niteUserTrackerCreate_{nullptr};
  void* pfn_niteUserTrackerDestroy_{nullptr};
  void* pfn_niteUserTrackerReadFrame_{nullptr};
  void* pfn_niteUserTrackerFrameRelease_{nullptr};

  // libfreenect handles and function pointers.
  void* fn_module_{nullptr};  // libfreenect.dll / libfreenect.so
  void* fn_ctx_{nullptr};     // freenect_context*
  void* fn_dev_{nullptr};     // freenect_device*
  void* fn_init_{nullptr};
  void* fn_shutdown_{nullptr};
  void* fn_select_subdevices_{nullptr};
  void* fn_open_device_{nullptr};
  void* fn_close_device_{nullptr};
  void* fn_set_video_callback_{nullptr};
  void* fn_set_video_mode_{nullptr};
  void* fn_start_video_{nullptr};
  void* fn_stop_video_{nullptr};
  void* fn_set_depth_callback_{nullptr};
  void* fn_set_depth_mode_{nullptr};
  void* fn_start_depth_{nullptr};
  void* fn_stop_depth_{nullptr};
  void* fn_set_tilt_degs_{nullptr};
  void* fn_update_tilt_state_{nullptr};
  void* fn_get_tilt_state_{nullptr};
  void* fn_get_mks_accel_{nullptr};

  // ── Virtual Input (Mouse / Gamepad -> Skeleton) ─────────────────────────
  class WindowInputListenerImpl : public xe::ui::WindowInputListener {
   public:
    explicit WindowInputListenerImpl(KinectInputDriver* driver) : driver_(driver) {}
    void OnMouseMove(xe::ui::MouseEvent& e) override;
    void OnMouseDown(xe::ui::MouseEvent& e) override;
    void OnMouseUp(xe::ui::MouseEvent& e) override;

   private:
    KinectInputDriver* driver_;
  };
  WindowInputListenerImpl window_input_listener_{this};

  std::atomic<float> virtual_mouse_x_{0.f};
  std::atomic<float> virtual_mouse_y_{0.f};
  std::atomic<bool> virtual_mouse_active_{false};
  std::atomic<bool> virtual_mouse_lbutton_{false};
  std::atomic<bool> virtual_mouse_rbutton_{false};

  static KinectInputDriver* instance_;
};

}  // namespace kinect
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_KINECT_KINECT_INPUT_DRIVER_H_
