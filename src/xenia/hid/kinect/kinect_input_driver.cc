/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary Authors. All rights reserved.                  *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 * Based on the experimental, in-progress Kinect HLE implementation by Weronika Saturday (@weronika-saturday).
 */

#include "xenia/hid/kinect/kinect_input_driver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/diagnostics_dump.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xsemaphore.h"
#include "xenia/kernel/xtimer.h"

// Platform-specific dynamic loading.
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#define XE_DLOPEN(name) reinterpret_cast<void*>(LoadLibraryA(name))
#define XE_DLSYM(mod, sym) \
  reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(mod), sym))
#define XE_DLCLOSE(mod) FreeLibrary(reinterpret_cast<HMODULE>(mod))
#else
#include <dlfcn.h>
#define XE_DLOPEN(name) dlopen(name, RTLD_LAZY | RTLD_LOCAL)
#define XE_DLSYM(mod, sym) dlsym(mod, sym)
#define XE_DLCLOSE(mod) dlclose(mod)
#endif

// Kinect Backend Selector config variable.
DEFINE_string(kinect_backend, "any",
              "Kinect skeleton tracking backend. Options: [any, synthetic, kinect_v1, mediapipe]",
              "HID");
DEFINE_bool(kinect_force_sync_objects, false,
            "Signal guest events/semaphores/timers the NUI workers wait on. "
            "UNSOUND: those objects belong to guest synchronisation and waking "
            "a thread without its work item can corrupt its scheduling. Off by "
            "default; only the registered skeleton frame event is signalled.",
            "HID");
// Diagnostic: instead of writing real skeleton data into the guest's shared
// frame buffer, write a sentinel at every dword that encodes its own offset
// (0x5A000000 | offset). A guest memory dump taken afterwards then shows every
// place the title copied out of our buffer, which is the only reliable way to
// confirm the frame layout it expects at that address.
DEFINE_bool(nui_nui_layout_probe, false,
            "Write self-describing sentinels into the shared skeleton frame "
            "buffer and dump guest memory so the offsets the title reads can be "
            "recovered. Diagnostic only; the title cannot parse sentinels.",
            "HID");
// Diagnostic: fill the shared frame buffer with sentinels once, then strip all
// access from its page. If the title ever loads from that address the guest
// takes a page fault, which proves whether it reads the buffer at all -- the
// sentinels alone cannot show this, because a title that reads fields and
// discards them leaves no trace in a memory dump.
DEFINE_bool(nui_nui_guard_page, false,
            "After writing to the shared skeleton frame buffer, remove all "
            "access from its page to detect whether the title ever reads it. "
            "Diagnostic only; a fault means the title does read it.",
            "HID");
// Diagnostic: dump the guest *heap* some seconds after the NUI runtime is up.
// The title's NUI pipeline is push-driven: a producer posts 32-bit work items
// into a 4-entry ring in the worker object and then signals its event. The
// enqueue routine (guest 0x82C48EB0) has no direct branch-link callers and its
// address appears nowhere in the 0x82000000-0x83FFFFFF image, so the pointer to
// it must be published at runtime on the heap -- which is why the static image
// dump taken at XamNuiGetDeviceStatus can never show it. This dumps the heap.
DEFINE_int32(nui_heap_dump_after_s, 0,
             "If > 0, dump guest heap snapshots this many seconds after the "
             "driver poll thread starts. 0 disables (default).",
             "HID");

namespace xe {
namespace hid {
namespace kinect {

// ---------------------------------------------------------------------------
// T-pose offsets relative to hip-center (metres, skeleton space).
// ---------------------------------------------------------------------------
static constexpr float kTPose[kNuiSkeletonPositionCount][3] = {
    {0.00f, 0.00f, 0.00f},    // HIP_CENTER
    {0.00f, 0.20f, 0.00f},    // SPINE
    {0.00f, 0.40f, 0.00f},    // SHOULDER_CENTER
    {0.00f, 0.60f, 0.00f},    // HEAD
    {-0.22f, 0.40f, 0.00f},   // SHOULDER_LEFT
    {-0.45f, 0.40f, 0.00f},   // ELBOW_LEFT
    {-0.57f, 0.40f, 0.00f},   // WRIST_LEFT   (interpolated)
    {-0.76f, 0.40f, 0.00f},   // HAND_LEFT
    {0.22f, 0.40f, 0.00f},    // SHOULDER_RIGHT
    {0.45f, 0.40f, 0.00f},    // ELBOW_RIGHT
    {0.57f, 0.40f, 0.00f},    // WRIST_RIGHT  (interpolated)
    {0.76f, 0.40f, 0.00f},    // HAND_RIGHT
    {-0.12f, 0.00f, 0.00f},   // HIP_LEFT
    {-0.12f, -0.42f, 0.00f},  // KNEE_LEFT
    {-0.12f, -0.61f, 0.00f},  // ANKLE_LEFT   (interpolated)
    {-0.12f, -0.90f, 0.08f},  // FOOT_LEFT
    {0.12f, 0.00f, 0.00f},    // HIP_RIGHT
    {0.12f, -0.42f, 0.00f},   // KNEE_RIGHT
    {0.12f, -0.61f, 0.00f},   // ANKLE_RIGHT  (interpolated)
    {0.12f, -0.90f, 0.08f},   // FOOT_RIGHT
};

// NiTE2 joint index → Xbox 360 joint index mapping.
static constexpr int kNiTE2ToXbox[15] = {
    NUI_JOINT_HEAD,             // NiTE JOINT_HEAD
    NUI_JOINT_SHOULDER_CENTER,  // NiTE JOINT_NECK
    NUI_JOINT_SHOULDER_LEFT,    // NiTE JOINT_LEFT_SHOULDER
    NUI_JOINT_SHOULDER_RIGHT,   // NiTE JOINT_RIGHT_SHOULDER
    NUI_JOINT_ELBOW_LEFT,       // NiTE JOINT_LEFT_ELBOW
    NUI_JOINT_ELBOW_RIGHT,      // NiTE JOINT_RIGHT_ELBOW
    NUI_JOINT_HAND_LEFT,        // NiTE JOINT_LEFT_HAND
    NUI_JOINT_HAND_RIGHT,       // NiTE JOINT_RIGHT_HAND
    NUI_JOINT_SPINE,            // NiTE JOINT_TORSO
    NUI_JOINT_HIP_LEFT,         // NiTE JOINT_LEFT_HIP
    NUI_JOINT_HIP_RIGHT,        // NiTE JOINT_RIGHT_HIP
    NUI_JOINT_KNEE_LEFT,        // NiTE JOINT_LEFT_KNEE
    NUI_JOINT_KNEE_RIGHT,       // NiTE JOINT_RIGHT_KNEE
    NUI_JOINT_FOOT_LEFT,        // NiTE JOINT_LEFT_FOOT
    NUI_JOINT_FOOT_RIGHT,       // NiTE JOINT_FOOT_RIGHT
};

// Static singleton — set in Setup(), cleared in destructor.
KinectInputDriver* KinectInputDriver::instance_ = nullptr;

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

KinectInputDriver::KinectInputDriver(xe::ui::Window* window,
                                     size_t window_z_order)
    : InputDriver(window, window_z_order) {
  if (window) {
    window->AddInputListener(&window_input_listener_, window_z_order);
  }
}

KinectInputDriver::~KinectInputDriver() {
  if (window()) {
    window()->RemoveInputListener(&window_input_listener_);
  }
  instance_ = nullptr;
  thread_running_ = false;
  if (poll_thread_.joinable()) poll_thread_.join();
  NuiShutdown();
  UnloadWindowsSDK();
  UnloadOpenNI2();
  UnloadMediaPipe();
  UnloadLibFreenect();
}

void KinectInputDriver::WindowInputListenerImpl::OnMouseMove(
    xe::ui::MouseEvent& e) {
  auto* win = driver_->window();
  if (!win) return;
  uint32_t w = win->GetActualLogicalWidth();
  uint32_t h = win->GetActualLogicalHeight();
  if (w > 0 && h > 0) {
    float nx = (static_cast<float>(e.x()) / static_cast<float>(w)) * 2.0f - 1.0f;
    float ny = 1.0f - (static_cast<float>(e.y()) / static_cast<float>(h)) * 2.0f;
    driver_->virtual_mouse_x_.store(std::clamp(nx, -1.0f, 1.0f));
    driver_->virtual_mouse_y_.store(std::clamp(ny, -1.0f, 1.0f));
    driver_->virtual_mouse_active_.store(true);
  }
}

void KinectInputDriver::WindowInputListenerImpl::OnMouseDown(
    xe::ui::MouseEvent& e) {
  if (e.button() == xe::ui::MouseEvent::Button::kLeft) {
    driver_->virtual_mouse_lbutton_.store(true);
    driver_->virtual_mouse_active_.store(true);
  } else if (e.button() == xe::ui::MouseEvent::Button::kRight) {
    driver_->virtual_mouse_rbutton_.store(true);
    driver_->virtual_mouse_active_.store(true);
  }
}

void KinectInputDriver::WindowInputListenerImpl::OnMouseUp(
    xe::ui::MouseEvent& e) {
  if (e.button() == xe::ui::MouseEvent::Button::kLeft) {
    driver_->virtual_mouse_lbutton_.store(false);
  } else if (e.button() == xe::ui::MouseEvent::Button::kRight) {
    driver_->virtual_mouse_rbutton_.store(false);
  }
}

// ---------------------------------------------------------------------------
// InputDriver interface — all gamepad queries unsupported
// ---------------------------------------------------------------------------

X_STATUS KinectInputDriver::Setup() {
  instance_ = this;
  std::string backend_opt = cvars::kinect_backend;

  if (backend_opt == "kinect_v1") {
#if XE_PLATFORM_WIN32
    if (TryLoadWindowsSDK()) {
      backend_ = Backend::WindowsSDK;
      XELOGI("KinectInputDriver: Windows SDK (Kinect10.dll) loaded.");
      return X_STATUS_SUCCESS;
    }
#endif
    if (TryLoadLibFreenect()) {
      backend_ = Backend::LibFreenect;
      XELOGI("KinectInputDriver: libfreenect driver loaded.");
      return X_STATUS_SUCCESS;
    }
    if (TryLoadOpenNI2()) {
      backend_ = Backend::OpenNI2;
      XELOGI("KinectInputDriver: OpenNI2 + NiTE2 backend loaded.");
      return X_STATUS_SUCCESS;
    }
    XELOGW("KinectInputDriver: kinect_v1 requested but no physical hardware SDK could be loaded.");
    return X_STATUS_UNSUCCESSFUL;
  }

  if (backend_opt == "libfreenect") {
    if (TryLoadLibFreenect()) {
      backend_ = Backend::LibFreenect;
      XELOGI("KinectInputDriver: libfreenect driver loaded.");
      return X_STATUS_SUCCESS;
    }
    XELOGW("KinectInputDriver: libfreenect requested but failed to load.");
    return X_STATUS_UNSUCCESSFUL;
  }

  if (backend_opt == "mediapipe") {
    if (TryLoadMediaPipe()) {
      backend_ = Backend::MediaPipe;
      XELOGI("KinectInputDriver: MediaPipe backend loaded.");
      return X_STATUS_SUCCESS;
    }
    XELOGW("KinectInputDriver: mediapipe requested but failed to load.");
    return X_STATUS_UNSUCCESSFUL;
  }

  if (backend_opt == "synthetic") {
    backend_ = Backend::Synthetic;
    XELOGI("KinectInputDriver: Using synthetic skeleton.");
    return X_STATUS_SUCCESS;
  }

  // "any" - try all in order.
#if XE_PLATFORM_WIN32
  if (TryLoadWindowsSDK()) {
    backend_ = Backend::WindowsSDK;
    XELOGI("KinectInputDriver: Windows SDK (Kinect10.dll) loaded.");
    return X_STATUS_SUCCESS;
  }
#endif
  if (TryLoadLibFreenect()) {
    backend_ = Backend::LibFreenect;
    XELOGI("KinectInputDriver: libfreenect driver loaded.");
    return X_STATUS_SUCCESS;
  }
  if (TryLoadOpenNI2()) {
    backend_ = Backend::OpenNI2;
    XELOGI("KinectInputDriver: OpenNI2 + NiTE2 backend loaded.");
    return X_STATUS_SUCCESS;
  }
  if (TryLoadMediaPipe()) {
    backend_ = Backend::MediaPipe;
    XELOGI("KinectInputDriver: MediaPipe backend loaded.");
    return X_STATUS_SUCCESS;
  }

  backend_ = Backend::Synthetic;
  XELOGI("KinectInputDriver: No real backend found — using synthetic skeleton.");
  return X_STATUS_SUCCESS;
}

X_RESULT KinectInputDriver::GetCapabilities(uint32_t, uint32_t,
                                            X_INPUT_CAPABILITIES*) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
X_RESULT KinectInputDriver::GetState(uint32_t, X_INPUT_STATE*) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
X_RESULT KinectInputDriver::SetState(uint32_t, X_INPUT_VIBRATION*) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
X_RESULT KinectInputDriver::GetKeystroke(uint32_t, uint32_t,
                                         X_INPUT_KEYSTROKE*) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

// ---------------------------------------------------------------------------
// NUI API
// ---------------------------------------------------------------------------

X_RESULT KinectInputDriver::NuiInitialize(uint32_t flags) {
  if (initialized_) return X_ERROR_SUCCESS;

  if (backend_ == Backend::WindowsSDK) {
    using PfnInit = int(__stdcall*)(uint32_t);
    int hr = reinterpret_cast<PfnInit>(win_NuiInitialize_)(flags);
    if (hr != 0) {
      XELOGW(
          "KinectInputDriver: Kinect10.dll NuiInitialize → {:08X}, "
          "falling back to synthetic.",
          static_cast<uint32_t>(hr));
      backend_ = Backend::Synthetic;
    } else {
      if (win_NuiSkeletonTrackingEnable_) {
        using PfnTrack = int(__stdcall*)(void*, uint32_t);
        int hr2 = reinterpret_cast<PfnTrack>(win_NuiSkeletonTrackingEnable_)(
            nullptr, 0);
        if (hr2 != 0) {
          XELOGW(
              "KinectInputDriver: NuiSkeletonTrackingEnable → {:08X} "
              "(non-fatal)",
              static_cast<uint32_t>(hr2));
        }
      }
    }
  }

  if (backend_ == Backend::OpenNI2) {
    // OpenNI2 device opened inside TryLoadOpenNI2().
  }

  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    BuildSyntheticFrame(&frame_latest_);
    new_frame_ = true;
  }

  initialized_ = true;
  thread_running_ = true;
  poll_thread_ = std::thread([this] { PollThread(); });

  XELOGI("KinectInputDriver: NuiInitialize OK (flags={:08X}, backend={})",
         flags,
         backend_ == Backend::WindowsSDK ? "WindowsSDK"
         : backend_ == Backend::OpenNI2  ? "OpenNI2"
         : backend_ == Backend::MediaPipe ? "MediaPipe"
                                         : "Synthetic");
  return X_ERROR_SUCCESS;
}

void KinectInputDriver::NuiShutdown() {
  if (!initialized_) return;
  thread_running_ = false;
  if (poll_thread_.joinable()) poll_thread_.join();

  if (backend_ == Backend::WindowsSDK && win_NuiShutdown_) {
    using PfnShut = void(__stdcall*)();
    reinterpret_cast<PfnShut>(win_NuiShutdown_)();
  }

  initialized_ = false;
}

void KinectInputDriver::SetSkeletonFrameEvent(xe::kernel::XEvent* event) {
  skeleton_xevent_ = event;
  XELOGI("KinectInputDriver: SetSkeletonFrameEvent event={:p}",
         static_cast<void*>(event));
}

void KinectInputDriver::AddSyncEvent(xe::kernel::XEvent* event) {
  if (!event) return;
  std::lock_guard<std::mutex> lock(sync_events_mutex_);
  for (auto* ev : sync_events_) {
    if (ev == event) return;
  }
  sync_events_.push_back(event);
  XELOGI("KinectInputDriver: AddSyncEvent event={:p} (total: {})",
         static_cast<void*>(event), sync_events_.size());
}

void KinectInputDriver::AddNuiWakeEvent(xe::kernel::XEvent* event) {
  if (!event) return;
  std::lock_guard<std::mutex> lock(sync_events_mutex_);
  for (auto* ev : nui_wake_events_) {
    if (ev == event) return;
  }
  nui_wake_events_.push_back(event);
  XELOGI("KinectInputDriver: AddNuiWakeEvent event={:p} handle={:08X} (total: {})",
         static_cast<void*>(event), event->handle(), nui_wake_events_.size());
}

void KinectInputDriver::AddSyncSemaphore(xe::kernel::XSemaphore* sem) {
  if (!sem) return;
  std::lock_guard<std::mutex> lock(sync_events_mutex_);
  for (auto* s : sync_semaphores_) {
    if (s == sem) return;
  }
  sync_semaphores_.push_back(sem);
  XELOGI("KinectInputDriver: AddSyncSemaphore semaphore={:p} (total: {})",
         static_cast<void*>(sem), sync_semaphores_.size());
}

void KinectInputDriver::AddSyncTimer(xe::kernel::XTimer* timer) {
  if (!timer) return;
  std::lock_guard<std::mutex> lock(sync_events_mutex_);
  for (auto* t : sync_timers_) {
    if (t == timer) return;
  }
  sync_timers_.push_back(timer);
  XELOGI("KinectInputDriver: AddSyncTimer timer={:p} (total: {})",
         static_cast<void*>(timer), sync_timers_.size());
}

X_RESULT KinectInputDriver::NuiSkeletonGetNextFrame(
    uint32_t wait_ms, X_NUI_SKELETON_FRAME* out_frame) {
  if (!initialized_) return X_ERROR_DEVICE_NOT_CONNECTED;

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      if (new_frame_) {
        // Note: the latest frame is *not* consumed here. This is a polling
        // API (several consumers -- the COM trampoline, XamApp diagnostics --
        // and the game's own render loop read the same sensor), and a
        // single-consumer "new frame" flag made whoever lost the race see a
        // zeroed frame instead of the current one.
        std::memcpy(out_frame, &frame_latest_, sizeof(*out_frame));
        return X_ERROR_SUCCESS;
      }
    }
    if (!wait_ms || std::chrono::steady_clock::now() >= deadline)
      return X_ERROR_FUNCTION_FAILED;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

X_RESULT KinectInputDriver::NuiCameraSetElevation(int32_t degrees) {
  if (backend_ == Backend::WindowsSDK && win_NuiCameraSetElevation_) {
    using Pfn = int(__stdcall*)(int32_t);
    reinterpret_cast<Pfn>(win_NuiCameraSetElevation_)(degrees);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT KinectInputDriver::NuiCameraGetElevation(int32_t* out) {
  if (!out) return X_ERROR_INVALID_PARAMETER;
  if (backend_ == Backend::WindowsSDK && win_NuiCameraGetElevation_) {
    using Pfn = int(__stdcall*)(int32_t*);
    reinterpret_cast<Pfn>(win_NuiCameraGetElevation_)(out);
    return X_ERROR_SUCCESS;
  }
  *out = 0;
  return X_ERROR_SUCCESS;
}

X_RESULT KinectInputDriver::NuiImageStreamOpen(
    uint32_t image_type, uint32_t resolution, uint32_t flags,
    uint32_t frame_limit, void* next_frame_event, void** out_stream_handle) {
  if (!out_stream_handle) return X_ERROR_INVALID_PARAMETER;

  if (backend_ == Backend::WindowsSDK && win_NuiImageStreamOpen_) {
    using Pfn = int(__stdcall*)(uint32_t, uint32_t, uint32_t, uint32_t, void*,
                                void**);
    int hr = reinterpret_cast<Pfn>(win_NuiImageStreamOpen_)(
        image_type, resolution, flags, frame_limit, next_frame_event,
        out_stream_handle);
    if (hr == 0 && *out_stream_handle) {
      XELOGI("KinectInputDriver: NuiImageStreamOpen success, handle={:p}",
             *out_stream_handle);
      return X_ERROR_SUCCESS;
    }
  }

  // Provide synthetic stream handle if real sensor is unavailable
  *out_stream_handle = reinterpret_cast<void*>(static_cast<uintptr_t>(0xCAFE0000 | (image_type & 0xFFFF)));
  XELOGI("KinectInputDriver: NuiImageStreamOpen synthetic stream handle {:p} (type={})",
         *out_stream_handle, image_type);
  return X_ERROR_SUCCESS;
}

X_RESULT KinectInputDriver::NuiImageStreamGetNextFrame(
    void* stream_handle, uint32_t wait_ms, void** out_image_frame) {
  if (!out_image_frame) return X_ERROR_INVALID_PARAMETER;

  if (backend_ == Backend::WindowsSDK && win_NuiImageStreamGetNextFrame_ &&
      stream_handle && (reinterpret_cast<uintptr_t>(stream_handle) & 0xFFFF0000) != 0xCAFE0000) {
    using Pfn = int(__stdcall*)(void*, uint32_t, void**);
    int hr = reinterpret_cast<Pfn>(win_NuiImageStreamGetNextFrame_)(
        stream_handle, wait_ms, out_image_frame);
    if (hr == 0) {
      return X_ERROR_SUCCESS;
    }
  }

  *out_image_frame = nullptr;
  return X_ERROR_FUNCTION_FAILED;
}

X_RESULT KinectInputDriver::NuiImageStreamReleaseFrame(void* stream_handle,
                                                      void* image_frame) {
  if (backend_ == Backend::WindowsSDK && win_NuiImageStreamReleaseFrame_ &&
      stream_handle && (reinterpret_cast<uintptr_t>(stream_handle) & 0xFFFF0000) != 0xCAFE0000) {
    using Pfn = int(__stdcall*)(void*, void*);
    reinterpret_cast<Pfn>(win_NuiImageStreamReleaseFrame_)(stream_handle,
                                                           image_frame);
  }
  return X_ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Synthetic skeleton
// ---------------------------------------------------------------------------

void KinectInputDriver::BuildSyntheticFrame(X_NUI_SKELETON_FRAME* f) {
  std::memset(f, 0, sizeof(*f));
  using namespace std::chrono;
  f->liTimeStamp =
      duration_cast<microseconds>(steady_clock::now().time_since_epoch())
          .count();
  f->dwFrameNumber = frame_number_++;
  f->vNormalToGravity = {0.f, 1.f, 0.f, 0.f};
  f->vFloorClipPlane = {0.f, 1.f, 0.f, 0.f};

  bool use_mouse = virtual_mouse_active_.load();
  float mx = virtual_mouse_x_.load();
  float my = virtual_mouse_y_.load();
  bool lbtn = virtual_mouse_lbutton_.load();
  bool rbtn = virtual_mouse_rbutton_.load();

  float wave = std::sin(f->liTimeStamp / 1e6 * 1.5) * 0.15f;

  auto& s = f->SkeletonData[0];
  s.eTrackingState = X_NUI_SKELETON_TRACKED;
  s.dwTrackingID = 1;
  s.dwUserIndex = 0;
  s.Position = {0.f, 0.f, 2.f, 1.f};

  for (uint32_t j = 0; j < kNuiSkeletonPositionCount; ++j) {
    float x = kTPose[j][0], y = kTPose[j][1], z = kTPose[j][2];

    if (use_mouse) {
      if (j == NUI_JOINT_HAND_RIGHT) {
        x = mx * 0.85f;
        y = my * 0.65f;
        z = lbtn ? -0.35f : 0.0f;  // Push/slash forward on click
      } else if (j == NUI_JOINT_WRIST_RIGHT) {
        x = mx * 0.70f;
        y = my * 0.52f;
        z = (lbtn ? -0.35f : 0.0f) * 0.8f;
      } else if (j == NUI_JOINT_ELBOW_RIGHT) {
        x = 0.22f + (mx * 0.85f - 0.22f) * 0.5f;
        y = 0.40f + (my * 0.65f - 0.40f) * 0.5f;
        z = (lbtn ? -0.35f : 0.0f) * 0.5f;
      } else if (j == NUI_JOINT_HAND_LEFT) {
        if (rbtn) {
          x = -mx * 0.85f;
          y = my * 0.65f;
          z = -0.35f;
        } else {
          y += wave;
        }
      } else if (j == NUI_JOINT_WRIST_LEFT) {
        if (rbtn) {
          x = -mx * 0.70f;
          y = my * 0.52f;
        } else {
          y += wave;
        }
      } else if (j == NUI_JOINT_ELBOW_LEFT) {
        if (rbtn) {
          x = -0.22f + (-mx * 0.85f + 0.22f) * 0.5f;
          y = 0.40f + (my * 0.65f - 0.40f) * 0.5f;
        }
      }
    } else {
      if (j == NUI_JOINT_HAND_LEFT || j == NUI_JOINT_WRIST_LEFT) y += wave;
      if (j == NUI_JOINT_HAND_RIGHT || j == NUI_JOINT_WRIST_RIGHT) y -= wave;
    }

    s.SkeletonPositions[j] = {x, y, 2.f + z, 1.f};
    s.eSkeletonPositionTrackingState[j] = X_NUI_SKELETON_POSITION_TRACKED;
  }
  for (uint32_t i = 1; i < kNuiSkeletonCount; ++i)
    f->SkeletonData[i].eTrackingState = X_NUI_SKELETON_NOT_TRACKED;
}

// ---------------------------------------------------------------------------
// Poll thread (30 fps)
// ---------------------------------------------------------------------------

void KinectInputDriver::PollThread() {
  xe::threading::set_name("KinectPoll");
  constexpr auto kPeriod = std::chrono::milliseconds(1000 / 30);

  // Force-signalling guest events/semaphores/timers that the guest never asked
  // us to signal is unsound. Off by default; only the registered skeleton frame
  // event is signalled.
  const bool force_sync_objects = cvars::kinect_force_sync_objects;

  // Diagnostic: count produced frames and how often we actually signal the
  // guest. A title that waits on a skeleton frame event will hang forever if
  // this counter stops advancing.
  uint64_t frames_produced = 0;
  uint64_t guest_signals = 0;
  auto last_report = std::chrono::steady_clock::now();

  // One-shot guest heap snapshots, taken once the title's NUI runtime has had
  // time to allocate its worker object and publish the push callback. The
  // producer callback (guest 0x82C48EB0) is not referenced anywhere in the
  // title image, so it can only be found on the heap after startup.
  const int32_t heap_dump_after_s = cvars::nui_heap_dump_after_s;
  auto poll_started = std::chrono::steady_clock::now();
  bool heap_dumped = false;

  while (thread_running_) {
    auto t0 = std::chrono::steady_clock::now();

    if (heap_dump_after_s > 0 && !heap_dumped) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          t0 - poll_started).count();
      if (elapsed >= heap_dump_after_s) {
        heap_dumped = true;
        XELOGI("KinectInputDriver: heap dump at t+{}s", elapsed);
        // Ask the memory manager where the guest's heaps actually live rather
        // than guessing a window: the first attempt scanned 0x80000000-0x82000000
        // and 0x84000000-0x86000000 and found nothing.
        auto* kss = xe::kernel::kernel_state();
        if (kss) {
          xe::Memory* guest_mem = kss->memory();
          if (guest_mem) {
            guest_mem->DumpMap();
          }
        }
        // DumpMap() shows the guest heap is 0x00000000-0x3FFFFFFF (1 GB at zero),
        // but the NUI worker's thread parameter landed at 0x4004E998, just above
        // it. Snapshot both windows.
        ::xe::kernel::DumpGuestImageForDiagnostics("guest_heap_low.bin",
                                                   0x00000000u, 0x2000000u);
        ::xe::kernel::DumpGuestImageForDiagnostics("guest_heap_high.bin",
                                                   0x40000000u, 0x1000000u);
      }
    }

    switch (backend_) {
      case Backend::WindowsSDK:
        PollWindowsSDK();
        break;
      case Backend::OpenNI2:
        PollOpenNI2();
        break;
      case Backend::LibFreenect:
        PollLibFreenect();
        break;
      case Backend::MediaPipe:
        PollMediaPipe();
        break;
      default:
        std::lock_guard<std::mutex> lock(frame_mutex_);
        BuildSyntheticFrame(&frame_latest_);
        new_frame_ = true;
        break;
    }

    // Signal the game's skeleton frame event if registered.
    if (new_frame_) {
      auto write_to_guest_dst = [&](uint8_t* dst) {
        if (!dst) return;
        uint64_t ts = static_cast<uint64_t>(frame_latest_.liTimeStamp);
        xe::store_and_swap<uint64_t>(reinterpret_cast<uint64_t*>(dst + 0), ts);
        xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(dst + 8), frame_latest_.dwFrameNumber);
        xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(dst + 12), frame_latest_.dwFlags);

        auto write_float = [](uint8_t* d, float v) {
          uint32_t bits;
          std::memcpy(&bits, &v, sizeof(float));
          xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(d), bits);
        };
        write_float(dst + 16, frame_latest_.vFloorClipPlane.x);
        write_float(dst + 20, frame_latest_.vFloorClipPlane.y);
        write_float(dst + 24, frame_latest_.vFloorClipPlane.z);
        write_float(dst + 28, frame_latest_.vFloorClipPlane.w);

        write_float(dst + 32, frame_latest_.vNormalToGravity.x);
        write_float(dst + 36, frame_latest_.vNormalToGravity.y);
        write_float(dst + 40, frame_latest_.vNormalToGravity.z);
        write_float(dst + 44, frame_latest_.vNormalToGravity.w);

        constexpr uint32_t kSkelDataSize = 436;
        constexpr uint32_t kSkelDataOffset = 48;
        if (cvars::nui_nui_layout_probe || cvars::nui_nui_guard_page) {
          for (uint32_t o = 0; o < 0x800; ++o) {
            xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(dst + o * 4),
                                          0x5A000000u | o);
          }
          return;
        }
        constexpr uint32_t kMaxSkeletonsForGuestFrame = 4;
        for (uint32_t s = 0; s < kMaxSkeletonsForGuestFrame; ++s) {
          const auto& src = frame_latest_.SkeletonData[s];
          uint8_t* sd = dst + kSkelDataOffset + s * kSkelDataSize;

          xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(sd + 0), static_cast<uint32_t>(src.eTrackingState));
          xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(sd + 4), src.dwTrackingID);
          xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(sd + 8), src.dwEnrollmentIndex);
          xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(sd + 12), src.dwUserIndex);

          write_float(sd + 16, src.Position.x);
          write_float(sd + 20, src.Position.y);
          write_float(sd + 24, src.Position.z);
          write_float(sd + 28, src.Position.w);

          for (uint32_t j = 0; j < kNuiSkeletonPositionCount; ++j) {
            uint8_t* jp = sd + 32 + j * 16;
            write_float(jp + 0, src.SkeletonPositions[j].x);
            write_float(jp + 4, src.SkeletonPositions[j].y);
            write_float(jp + 8, src.SkeletonPositions[j].z);
            write_float(jp + 12, src.SkeletonPositions[j].w);
          }

          for (uint32_t j = 0; j < kNuiSkeletonPositionCount; ++j) {
            xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(sd + 352 + j * 4), static_cast<uint32_t>(src.eSkeletonPositionTrackingState[j]));
          }

          xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(sd + 432), src.dwQualityFlags);
        }
      };

      uint8_t* dst1 = shared_guest_dst_.load();
      if (dst1) {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        xe::memory::PageAccess old_prot;
        const bool protected_ok = xe::memory::Protect(
            dst1, 0x1000, xe::memory::PageAccess::kReadWrite, &old_prot);
        write_to_guest_dst(dst1);
        if (protected_ok) {
          xe::memory::Protect(dst1, 0x1000, old_prot, nullptr);
        }
        if (cvars::nui_nui_guard_page) {
          static std::atomic<bool> guarded{false};
          if (!guarded.exchange(true)) {
            const bool ok = xe::memory::Protect(
                dst1, 0x1000, xe::memory::PageAccess::kNoAccess, nullptr);
            XELOGI(
                "KinectInputDriver: guard page installed on shared frame "
                "buffer host_ptr={:p} -> {}",
                static_cast<void*>(dst1), ok);
          }
        }
      }

      if (skeleton_xevent_) {
        skeleton_xevent_->Set(0, false);
        ++guest_signals;
      }

      // REMOVED: the fabricated "title internal NUI device circular queue"
      // write. It targeted a hardcoded guest address (0x83896510) with an
      // invented layout -- offsets +0x00, +0x58, +0xA40 and a queue slot at
      // +0xA70 -- none of which were ever observed in the guest. Three
      // reasons it could not work and one reason it was actively harmful:
      //   1. The whole region reads back as zero in a runtime dump.
      //   2. The title is never handed a pointer to it, so it cannot read it.
      //   3. Skeleton frames only reach a title that negotiated a buffer.
      //   4. Writing 0x4000 bytes of guessed structure into live guest memory
      //      every 33 ms is precisely the corruption that froze the title on
      //      2026-10-13. Do not reintroduce a write target derived from an
      //      address heuristic; deliver frames through a contract the title
      //      established itself.
      //
      // write_to_guest_dst() above still serves the registered shared frame
      // buffer, which the title did hand us.

      if (cvars::nui_nui_layout_probe) {
        // After the sentinel has been sitting in the buffer long enough for the
        // title to consume it, snapshot guest memory so we can search for the
        // sentinel values and learn which offsets the title actually reads.
        // Two snapshots: the first catches the title mid-consumption (it may
        // fault on a sentinel tracking state and die before the second), the
        // second catches anything it keeps in long-lived state.
        static std::atomic<uint64_t> probe_frames{0};
        const uint64_t probe_n = probe_frames.fetch_add(1);
        if (probe_n == 3) {
          ::xe::kernel::DumpGuestImageForDiagnostics("guest_probe_a.bin",
                                                    0x82000000u, 0x2000000u);
        } else if (probe_n == 60) {
          ::xe::kernel::DumpGuestImageForDiagnostics("guest_probe_b.bin",
                                                    0x82000000u, 0x2000000u);
        }
      }

      // Wake the title's own NUI queue. Titles using the XAM NUI service park
      // their worker threads on an event they create themselves *after* the
      // 0x2B001 bind, so there is no handle to register up front. Those objects
      // are auto-reset (KeResetEvent right after the wait in the title's NUI
      // worker) and carry work items that only appear once the game enqueues
      // them, so waking them every frame is safe: an empty queue just returns
      // immediately, and the title drives the actual data through the shared
      // frame buffer instead.
      for (auto* ev : nui_wake_events_) {
        if (ev) {
          ev->Set(0, false);
        }
      }

      // Force-signalling arbitrary guest events/semaphores/timers every frame
      // is unsound: those objects belong to the guest's own synchronisation and
      // signalling them behind its back can corrupt its scheduling (a thread
      // may wake, find its work item missing, and re-wait). Only the skeleton
      // frame event above -- which the guest explicitly registered with us via
      // SetSkeletonFrameEvent -- is legitimate. The rest stays opt-in.
      if (force_sync_objects) {
        std::lock_guard<std::mutex> lock(sync_events_mutex_);
        for (auto* ev : sync_events_) {
          if (ev) {
            ev->Set(0, false);
          }
        }
        for (auto* sem : sync_semaphores_) {
          if (sem) {
            std::ignore = sem->ReleaseSemaphore(1, nullptr);
          }
        }
        for (auto* tim : sync_timers_) {
          if (tim) {
            std::ignore = tim->SetTimer(0, 0, 0, 0, false);
          }
        }
      }
    }
    ++frames_produced;

    // Report every 5s so a stalled producer is obvious in the log.
    auto now = std::chrono::steady_clock::now();
    if (now - last_report > std::chrono::seconds(5)) {
      last_report = now;
      XELOGI(
          "KinectInputDriver: frames={} guest_signals={} dst1={:08X} dst2={:08X} "
          "skel_event={} tracked_state={}",
          frames_produced, guest_signals,
          static_cast<uint32_t>(
              reinterpret_cast<uintptr_t>(shared_guest_dst_.load())),
          static_cast<uint32_t>(
              reinterpret_cast<uintptr_t>(shared_guest_dst2_.load())),
          skeleton_xevent_ ? 1 : 0,
          static_cast<int>(frame_latest_.SkeletonData[0].eTrackingState));
    }

    auto elapsed = std::chrono::steady_clock::now() - t0;
    if (elapsed < kPeriod) std::this_thread::sleep_for(kPeriod - elapsed);
  }
}

// ===========================================================================
// Windows SDK backend
// ===========================================================================

bool KinectInputDriver::TryLoadWindowsSDK() {
#if XE_PLATFORM_WIN32
  void* mod = XE_DLOPEN("Kinect10.dll");
  if (!mod) return false;

  auto bind_required = [&](const char* sym, void** dst) -> bool {
    *dst = XE_DLSYM(mod, sym);
    if (!*dst) {
      XELOGW("KinectInputDriver: required symbol {} not found in Kinect10.dll",
             sym);
      XE_DLCLOSE(mod);
      win_module_ = nullptr;
      return false;
    }
    return true;
  };
  auto bind_optional = [&](const char* sym, void** dst) {
    *dst = XE_DLSYM(mod, sym);
    if (!*dst) {
      XELOGW("KinectInputDriver: optional symbol {} not found in Kinect10.dll",
             sym);
    }
  };

  win_module_ = mod;
  if (!bind_required("NuiInitialize", &win_NuiInitialize_)) return false;
  if (!bind_required("NuiShutdown", &win_NuiShutdown_)) return false;
  if (!bind_required("NuiSkeletonGetNextFrame", &win_NuiSkeletonGetNextFrame_))
    return false;
  bind_optional("NuiSkeletonTrackingEnable", &win_NuiSkeletonTrackingEnable_);
  bind_optional("NuiCameraElevationSetAngle", &win_NuiCameraSetElevation_);
  bind_optional("NuiCameraElevationGetAngle", &win_NuiCameraGetElevation_);
  bind_optional("NuiImageStreamOpen", &win_NuiImageStreamOpen_);
  bind_optional("NuiImageStreamGetNextFrame", &win_NuiImageStreamGetNextFrame_);
  bind_optional("NuiImageStreamReleaseFrame", &win_NuiImageStreamReleaseFrame_);
  XELOGI("KinectInputDriver: Kinect10.dll loaded successfully.");
  return true;
#else
  return false;
#endif
}

void KinectInputDriver::UnloadWindowsSDK() {
  if (win_module_) {
    XE_DLCLOSE(win_module_);
    win_module_ = nullptr;
  }
  win_NuiInitialize_ = win_NuiShutdown_ = win_NuiSkeletonGetNextFrame_ =
      win_NuiSkeletonTrackingEnable_ = win_NuiCameraSetElevation_ =
          win_NuiCameraGetElevation_ = win_NuiImageStreamOpen_ =
              win_NuiImageStreamGetNextFrame_ =
                  win_NuiImageStreamReleaseFrame_ = nullptr;
}

void KinectInputDriver::PollWindowsSDK() {
#if XE_PLATFORM_WIN32
  static constexpr size_t kWinFrameSize = 3872;
  static constexpr size_t kWinSkelOffset = 48;
  static constexpr size_t kWinSkelSize = 644;

  uint8_t raw[kWinFrameSize] = {};
  using PfnGet = int(__stdcall*)(uint32_t, void*);
  if (reinterpret_cast<PfnGet>(win_NuiSkeletonGetNextFrame_)(15, raw) != 0) {
    // If real sensor has not delivered a frame yet or is warming up, fallback to synthetic
    std::lock_guard<std::mutex> lock(frame_mutex_);
    BuildSyntheticFrame(&frame_latest_);
    new_frame_ = true;
    return;
  }

  std::lock_guard<std::mutex> lock(frame_mutex_);
  std::memset(&frame_latest_, 0, sizeof(frame_latest_));
  std::memcpy(&frame_latest_.liTimeStamp, raw, 8);
  std::memcpy(&frame_latest_.dwFrameNumber, raw + 8, 4);
  std::memcpy(&frame_latest_.dwFlags, raw + 12, 4);
  std::memcpy(&frame_latest_.vFloorClipPlane, raw + 16, 16);
  std::memcpy(&frame_latest_.vNormalToGravity, raw + 32, 16);

  bool has_physical_tracked = false;
  for (uint32_t s = 0; s < kNuiSkeletonCount; ++s) {
    const uint8_t* src = raw + kWinSkelOffset + s * kWinSkelSize;
    auto& dst = frame_latest_.SkeletonData[s];
    std::memcpy(&dst.eTrackingState, src, 4);
    std::memcpy(&dst.dwTrackingID, src + 4, 4);
    std::memcpy(&dst.dwUserIndex, src + 12, 4);
    std::memcpy(&dst.Position, src + 16, 16);
    std::memcpy(dst.SkeletonPositions, src + 32,
                sizeof(X_VECTOR4) * kNuiSkeletonPositionCount);
    for (uint32_t j = 0; j < kNuiSkeletonPositionCount; ++j)
      std::memcpy(&dst.eSkeletonPositionTrackingState[j], src + 352 + j * 4, 4);
    std::memcpy(&dst.dwQualityFlags, src + 432, 4);
    if (dst.eTrackingState == X_NUI_SKELETON_TRACKED) {
      has_physical_tracked = true;
    }
  }

  // If physical hardware does not see a standing person right now, seamlessly fall back to synthetic:
  if (!has_physical_tracked) {
    BuildSyntheticFrame(&frame_latest_);
  }

  new_frame_ = true;
#endif
}

// ===========================================================================
// OpenNI2 + NiTE2 backend
// ===========================================================================

namespace nite2_offsets {
constexpr size_t kFrameStatus = 0;
constexpr size_t kFrameUserCount = 16;
constexpr size_t kFrameUsers = 20;
constexpr size_t kUserSize = 256;
constexpr size_t kUserIdOffset = 0;
constexpr size_t kUserStateOffset = 4;
constexpr size_t kUserSkeleton = 20;
constexpr size_t kSkeletonState = 0;
constexpr size_t kSkeletonJoints = 4;
constexpr size_t kJointSize = 40;
constexpr size_t kJointPosOffset = 4;
constexpr size_t kJointConfOffset = 16;
constexpr int kSkeletonTracked = 2;
}  // namespace nite2_offsets

bool KinectInputDriver::TryLoadOpenNI2() {
#if XE_PLATFORM_WIN32
  const char* oni_name = "OpenNI2.dll";
  const char* nite_name = "NiTE2.dll";
#elif XE_PLATFORM_LINUX
  const char* oni_name = "libOpenNI2.so";
  const char* nite_name = "libNiTE2.so";
#else
  const char* oni_name = "libOpenNI2.dylib";
  const char* nite_name = "libNiTE2.dylib";
#endif

  void* oni_mod = XE_DLOPEN(oni_name);
  if (!oni_mod) return false;

  void* nite_mod = XE_DLOPEN(nite_name);
  if (!nite_mod) {
    XE_DLCLOSE(oni_mod);
    return false;
  }

  oni_module_ = oni_mod;
  nite_module_ = nite_mod;

  pfn_oniInitialize_ = XE_DLSYM(oni_mod, "oniInitialize");
  pfn_oniShutdown_ = XE_DLSYM(oni_mod, "oniShutdown");

  pfn_niteInitialize_ = XE_DLSYM(nite_mod, "niteInitialize");
  pfn_niteShutdown_ = XE_DLSYM(nite_mod, "niteShutdown");
  pfn_niteUserTrackerCreate_ = XE_DLSYM(nite_mod, "niteUserTrackerCreate");
  pfn_niteUserTrackerDestroy_ = XE_DLSYM(nite_mod, "niteUserTrackerDestroy");
  pfn_niteUserTrackerReadFrame_ = XE_DLSYM(nite_mod, "niteUserTrackerReadFrame");
  pfn_niteUserTrackerFrameRelease_ = XE_DLSYM(nite_mod, "niteUserTrackerFrameRelease");

  if (!pfn_oniInitialize_ || !pfn_niteInitialize_ ||
      !pfn_niteUserTrackerCreate_ || !pfn_niteUserTrackerReadFrame_) {
    UnloadOpenNI2();
    return false;
  }

  using PfnOniInit = int (*)(int api_version);
  int oni_status = reinterpret_cast<PfnOniInit>(pfn_oniInitialize_)(2);
  if (oni_status != 0) {
    UnloadOpenNI2();
    return false;
  }

  using PfnNiteInit = int (*)();
  int nite_status = reinterpret_cast<PfnNiteInit>(pfn_niteInitialize_)();
  if (nite_status != 0) {
    using PfnOniShut = void (*)();
    reinterpret_cast<PfnOniShut>(pfn_oniShutdown_)();
    UnloadOpenNI2();
    return false;
  }

  using PfnTrackerCreate = int (*)(uint32_t* out_handle);
  uint32_t tracker_handle = 0;
  int tc_status = reinterpret_cast<PfnTrackerCreate>(pfn_niteUserTrackerCreate_)(&tracker_handle);
  if (tc_status != 0) {
    using PfnNiteShut = void (*)();
    reinterpret_cast<PfnNiteShut>(pfn_niteShutdown_)();
    using PfnOniShut = void (*)();
    reinterpret_cast<PfnOniShut>(pfn_oniShutdown_)();
    UnloadOpenNI2();
    return false;
  }

  nite_tracker_ = reinterpret_cast<void*>(static_cast<uintptr_t>(tracker_handle));
  return true;
}

void KinectInputDriver::UnloadOpenNI2() {
  if (nite_tracker_ && pfn_niteUserTrackerDestroy_) {
    using PfnDestroy = int (*)(uint32_t handle);
    reinterpret_cast<PfnDestroy>(pfn_niteUserTrackerDestroy_)(
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(nite_tracker_)));
    nite_tracker_ = nullptr;
  }
  if (pfn_niteShutdown_) {
    using Pfn = void (*)();
    reinterpret_cast<Pfn>(pfn_niteShutdown_)();
  }
  if (pfn_oniShutdown_) {
    using Pfn = void (*)();
    reinterpret_cast<Pfn>(pfn_oniShutdown_)();
  }
  if (nite_module_) {
    XE_DLCLOSE(nite_module_);
    nite_module_ = nullptr;
  }
  if (oni_module_) {
    XE_DLCLOSE(oni_module_);
    oni_module_ = nullptr;
  }
  pfn_oniInitialize_ = pfn_oniShutdown_ = nullptr;
  pfn_niteInitialize_ = pfn_niteShutdown_ = pfn_niteUserTrackerCreate_ =
      pfn_niteUserTrackerDestroy_ = pfn_niteUserTrackerReadFrame_ =
          pfn_niteUserTrackerFrameRelease_ = nullptr;
}

void KinectInputDriver::PollOpenNI2() {
  if (!pfn_niteUserTrackerReadFrame_) return;

  uint32_t tracker_handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(nite_tracker_));
  void* frame_ref = nullptr;
  using PfnRead = int (*)(uint32_t handle, void** out_frame);
  int status = reinterpret_cast<PfnRead>(pfn_niteUserTrackerReadFrame_)(tracker_handle, &frame_ref);
  if (status != 0 || !frame_ref) return;

  const uint8_t* fb = reinterpret_cast<const uint8_t*>(frame_ref);
  int frame_status;
  std::memcpy(&frame_status, fb + nite2_offsets::kFrameStatus, 4);
  if (frame_status != 0) goto release;

  {
    int user_count = 0;
    std::memcpy(&user_count, fb + nite2_offsets::kFrameUserCount, 4);
    user_count = std::min(user_count, static_cast<int>(kNuiSkeletonCount));

    X_NUI_SKELETON_FRAME new_frame{};
    using namespace std::chrono;
    new_frame.liTimeStamp =
        duration_cast<microseconds>(steady_clock::now().time_since_epoch())
            .count();
    new_frame.dwFrameNumber = frame_number_++;
    new_frame.vNormalToGravity = {0.f, 1.f, 0.f, 0.f};
    new_frame.vFloorClipPlane = {0.f, 1.f, 0.f, 0.f};

    const uint8_t* users_base = fb + nite2_offsets::kFrameUsers;
    int tracked_count = 0;

    for (int u = 0;
         u < user_count && tracked_count < static_cast<int>(kNuiSkeletonCount);
         ++u) {
      const uint8_t* user = users_base + u * nite2_offsets::kUserSize;

      int user_state = 0;
      std::memcpy(&user_state, user + nite2_offsets::kUserStateOffset, 4);

      const uint8_t* skel_ptr = user + nite2_offsets::kUserSkeleton;
      int skel_state = 0;
      std::memcpy(&skel_state, skel_ptr + nite2_offsets::kSkeletonState, 4);
      if (skel_state != nite2_offsets::kSkeletonTracked) continue;

      float positions[15][3] = {};
      float confidence[15] = {};
      const uint8_t* joints_ptr = skel_ptr + nite2_offsets::kSkeletonJoints;

      for (int j = 0; j < 15; ++j) {
        const uint8_t* joint = joints_ptr + j * nite2_offsets::kJointSize;
        std::memcpy(positions[j], joint + nite2_offsets::kJointPosOffset, 12);
        std::memcpy(&confidence[j], joint + nite2_offsets::kJointConfOffset, 4);
        positions[j][0] *= 0.001f;
        positions[j][1] *= 0.001f;
        positions[j][2] *= 0.001f;
      }

      int user_id = 0;
      std::memcpy(&user_id, user + nite2_offsets::kUserIdOffset, 4);

      MapNiTE2Skeleton(positions, confidence, static_cast<uint32_t>(user_id),
                       static_cast<uint32_t>(tracked_count),
                       &new_frame.SkeletonData[tracked_count]);
      ++tracked_count;
    }

    for (int s = tracked_count; s < static_cast<int>(kNuiSkeletonCount); ++s)
      new_frame.SkeletonData[s].eTrackingState = X_NUI_SKELETON_NOT_TRACKED;

    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      frame_latest_ = new_frame;
      new_frame_ = true;
    }
  }

release:
  if (pfn_niteUserTrackerFrameRelease_) {
    using PfnRelease = int (*)(uint32_t handle, void* frame);
    reinterpret_cast<PfnRelease>(pfn_niteUserTrackerFrameRelease_)(tracker_handle, frame_ref);
  }
}

void KinectInputDriver::MapNiTE2Skeleton(const float positions[][3],
                                         const float confidence[],
                                         uint32_t tracking_id,
                                         uint32_t user_index,
                                         X_NUI_SKELETON_DATA* out) {
  std::memset(out, 0, sizeof(*out));
  out->eTrackingState = X_NUI_SKELETON_TRACKED;
  out->dwTrackingID = tracking_id;
  out->dwUserIndex = user_index;

  for (int n = 0; n < 15; ++n) {
    int xb = kNiTE2ToXbox[n];
    out->SkeletonPositions[xb] = {positions[n][0], positions[n][1],
                                  positions[n][2], 1.f};
    out->eSkeletonPositionTrackingState[xb] =
        confidence[n] > 0.5f ? X_NUI_SKELETON_POSITION_TRACKED
                             : X_NUI_SKELETON_POSITION_INFERRED;
  }

  auto lerp_joint = [&](int dst, int a, int b) {
    auto& pa = out->SkeletonPositions[a];
    auto& pb = out->SkeletonPositions[b];
    out->SkeletonPositions[dst] = {(pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f,
                                   (pa.z + pb.z) * 0.5f, 1.f};
    out->eSkeletonPositionTrackingState[dst] = X_NUI_SKELETON_POSITION_INFERRED;
  };

  lerp_joint(NUI_JOINT_HIP_CENTER, NUI_JOINT_HIP_LEFT, NUI_JOINT_HIP_RIGHT);
  out->Position = out->SkeletonPositions[NUI_JOINT_HIP_CENTER];

  lerp_joint(NUI_JOINT_WRIST_LEFT, NUI_JOINT_ELBOW_LEFT, NUI_JOINT_HAND_LEFT);
  lerp_joint(NUI_JOINT_WRIST_RIGHT, NUI_JOINT_ELBOW_RIGHT, NUI_JOINT_HAND_RIGHT);
  lerp_joint(NUI_JOINT_ANKLE_LEFT, NUI_JOINT_KNEE_LEFT, NUI_JOINT_FOOT_LEFT);
  lerp_joint(NUI_JOINT_ANKLE_RIGHT, NUI_JOINT_KNEE_RIGHT, NUI_JOINT_FOOT_RIGHT);
}

// ===========================================================================
// MediaPipe placeholder backend
// ===========================================================================

bool KinectInputDriver::TryLoadMediaPipe() {
  // Stub for future webcam MediaPipe integration.
  return false;
}

void KinectInputDriver::UnloadMediaPipe() {}

void KinectInputDriver::PollMediaPipe() {
  // Build synthetic frame if MediaPipe starts up, or write OpenCV frame grabber.
  std::lock_guard<std::mutex> lock(frame_mutex_);
  BuildSyntheticFrame(&frame_latest_);
  new_frame_ = true;
}

// ===========================================================================
// libfreenect dynamic wrapper backend
// ===========================================================================

bool KinectInputDriver::TryLoadLibFreenect() {
#if XE_PLATFORM_WIN32
  fn_module_ = XE_DLOPEN("libfreenect.dll");
#else
  fn_module_ = XE_DLOPEN("libfreenect.so");
#endif
  if (!fn_module_) {
    return false;
  }
  fn_init_ = XE_DLSYM(fn_module_, "freenect_init");
  fn_shutdown_ = XE_DLSYM(fn_module_, "freenect_shutdown");
  fn_select_subdevices_ = XE_DLSYM(fn_module_, "freenect_select_subdevices");
  fn_open_device_ = XE_DLSYM(fn_module_, "freenect_open_device");
  fn_close_device_ = XE_DLSYM(fn_module_, "freenect_close_device");
  fn_set_video_callback_ = XE_DLSYM(fn_module_, "freenect_set_video_callback");
  fn_set_video_mode_ = XE_DLSYM(fn_module_, "freenect_set_video_mode");
  fn_start_video_ = XE_DLSYM(fn_module_, "freenect_start_video");
  fn_stop_video_ = XE_DLSYM(fn_module_, "freenect_stop_video");
  fn_set_depth_callback_ = XE_DLSYM(fn_module_, "freenect_set_depth_callback");
  fn_set_depth_mode_ = XE_DLSYM(fn_module_, "freenect_set_depth_mode");
  fn_start_depth_ = XE_DLSYM(fn_module_, "freenect_start_depth");
  fn_stop_depth_ = XE_DLSYM(fn_module_, "freenect_stop_depth");
  fn_set_tilt_degs_ = XE_DLSYM(fn_module_, "freenect_set_tilt_degs");
  fn_update_tilt_state_ = XE_DLSYM(fn_module_, "freenect_update_tilt_state");
  fn_get_tilt_state_ = XE_DLSYM(fn_module_, "freenect_get_tilt_state");
  fn_get_mks_accel_ = XE_DLSYM(fn_module_, "freenect_get_mks_accel");

  if (!fn_init_ || !fn_shutdown_ || !fn_open_device_ || !fn_close_device_) {
    XE_DLCLOSE(fn_module_);
    fn_module_ = nullptr;
    return false;
  }
  return true;
}

void KinectInputDriver::UnloadLibFreenect() {
  if (fn_module_) {
    XE_DLCLOSE(fn_module_);
    fn_module_ = nullptr;
  }
}

void KinectInputDriver::PollLibFreenect() {
  // libfreenect event polling loop or depth frame mapping goes here.
  std::lock_guard<std::mutex> lock(frame_mutex_);
  BuildSyntheticFrame(&frame_latest_);
  new_frame_ = true;
}

}  // namespace kinect
}  // namespace hid
}  // namespace xe
