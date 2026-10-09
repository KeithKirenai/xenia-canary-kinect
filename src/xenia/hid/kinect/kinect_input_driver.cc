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
#include "xenia/kernel/xevent.h"

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

X_RESULT KinectInputDriver::NuiSkeletonGetNextFrame(
    uint32_t wait_ms, X_NUI_SKELETON_FRAME* out_frame) {
  if (!initialized_) return X_ERROR_DEVICE_NOT_CONNECTED;

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      if (new_frame_) {
        std::memcpy(out_frame, &frame_latest_, sizeof(*out_frame));
        new_frame_ = false;
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

  while (thread_running_) {
    auto t0 = std::chrono::steady_clock::now();

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
    if (new_frame_ && skeleton_xevent_) {
      skeleton_xevent_->Set(0, false);
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
  bind_optional("NuiCameraSetElevation", &win_NuiCameraSetElevation_);
  bind_optional("NuiCameraGetElevation", &win_NuiCameraGetElevation_);
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
          win_NuiCameraGetElevation_ = nullptr;
}

void KinectInputDriver::PollWindowsSDK() {
#if XE_PLATFORM_WIN32
  static constexpr size_t kWinFrameSize = 3872;
  static constexpr size_t kWinSkelOffset = 48;
  static constexpr size_t kWinSkelSize = 644;

  uint8_t raw[kWinFrameSize] = {};
  using PfnGet = int(__stdcall*)(uint32_t, void*);
  if (reinterpret_cast<PfnGet>(win_NuiSkeletonGetNextFrame_)(0, raw) != 0)
    return;

  std::lock_guard<std::mutex> lock(frame_mutex_);
  std::memset(&frame_latest_, 0, sizeof(frame_latest_));
  std::memcpy(&frame_latest_.liTimeStamp, raw, 8);
  std::memcpy(&frame_latest_.dwFrameNumber, raw + 8, 4);
  std::memcpy(&frame_latest_.dwFlags, raw + 12, 4);
  std::memcpy(&frame_latest_.vFloorClipPlane, raw + 16, 16);
  std::memcpy(&frame_latest_.vNormalToGravity, raw + 32, 16);

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
