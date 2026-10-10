/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xlivebase_app.h"

#include "xenia/base/logging.h"
#include "xenia/hid/kinect/kinect_input_driver.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

static xe::hid::kinect::KinectInputDriver* kd() {
  return xe::hid::kinect::KinectInputDriver::instance();
}

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state)
    : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message,
                                            uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x0005008C: {
      // Called on startup of blades dashboard v1888 to v2858
      XELOGD("XLiveBaseUnk5008C, unimplemented");
      return X_E_FAIL;
    }
    case 0x00050094: {
      // Called on startup of blades dashboard v4532 to v4552
      XELOGD("XLiveBaseUnk50094, unimplemented");
      return X_E_FAIL;
    }
    case 0x00058003: {
      /* Notes:
         - Called on startup of dashboard (netplay build)
         - used by other internet funtions to check if online (e.g.
         XamGetLiveHiveValueA)
         - Return is Saved elsewhere and used here
      */
      XELOGD("XLiveBaseLogonGetHR, implemented in netplay");
      return 0x001510F1;  // X_ONLINE_S_LOGON_DISCONNECTED
    }
    case 0x00058004: {
      /* Notes:
         - Called on startup, seems to just return a bool in the buffer.
         - It is Saved elsewhere and used here
         - Fruit Ninja uses 0x58004 with a 112-byte buffer for NUI session setup.
       */
      if (buffer_length >= 112) {
        if (kd() && !kd()->is_initialized()) {
          kd()->NuiInitialize(0x08);
        }
        auto* out = memory_->TranslateVirtual<uint32_t*>(buffer_ptr);
        if (out && buffer_length >= 16) {
          XELOGI("XLiveBaseApp: 0x58004 NUI session setup: {:08X} {:08X} {:08X} {:08X} (buf_len={:08X})",
                 xe::byte_swap(out[0]), xe::byte_swap(out[1]),
                 xe::byte_swap(out[2]), xe::byte_swap(out[3]),
                 buffer_length);
        } else {
          XELOGI("XLiveBaseApp: 0x58004 NUI session setup (buf={:08X}, len={:08X})",
                 buffer_ptr, buffer_length);
        }
        return X_E_SUCCESS;
      }

      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      xe::store_and_swap<uint32_t>(buffer, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      // Buffer only set when online
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetNatType({:08X})", buffer_ptr);
      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      XELOGD("XLiveBaseOnlineGetServiceInfo({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x00058020: {
      // 0x00058004 is called right before this.
      // We should create a XamEnumerate-able empty list here, but I'm not
      // sure of the format.
      // buffer_length seems to be the same ptr sent to 0x00058004.
      XELOGD("CXLiveFriends::Enumerate({:08X}, {:08X}) unimplemented",
             buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058023: {
      XELOGD(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058035: {
      // NUI frame/data request (Fruit Ninja).
      // ~40-byte buffer.
      auto* out = memory_->TranslateVirtual<uint32_t*>(buffer_ptr);
      if (out) {
        XELOGI("XLiveBaseApp: 0x58035 NUI frame request: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} (buf_len={:08X})",
               xe::byte_swap(out[0]), xe::byte_swap(out[1]),
               xe::byte_swap(out[2]), xe::byte_swap(out[3]),
               xe::byte_swap(out[4]), xe::byte_swap(out[5]),
               xe::byte_swap(out[6]), xe::byte_swap(out[7]),
               xe::byte_swap(out[8]), buffer_length);
      }
      if (kd() && kd()->is_initialized() && buffer_length >= 40) {
        xe::store_and_swap<uint32_t>(&out[0], 0x000000fe); // Session handle
        xe::store_and_swap<uint32_t>(&out[1], 0);          // Success status

        xe::hid::kinect::X_NUI_SKELETON_FRAME frame{};
        if (XSUCCEEDED(kd()->NuiSkeletonGetNextFrame(0, &frame))) {
          xe::store_and_swap<uint32_t>(&out[2], static_cast<uint32_t>(frame.liTimeStamp));
          xe::store_and_swap<uint32_t>(&out[3], frame.dwFrameNumber);
          xe::store_and_swap<uint32_t>(&out[4], static_cast<uint32_t>(frame.SkeletonData[0].eTrackingState));
          xe::store_and_swap<uint32_t>(&out[5], frame.SkeletonData[0].dwTrackingID);
        }
      }
      return X_E_SUCCESS;
    }
    case 0x00058037: {
      XELOGD("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      XELOGD("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
