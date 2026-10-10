/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/xbox.h"
#include "xenia/hid/kinect/kinect_input_driver.h"

DEFINE_bool(allow_nui_initialization, true,
            "Enable NUI initialization\n"
            " Only set true when testing kinect games. Certain games may\n"
            " require avatar implementation.",
            "Kernel");

namespace xe {
namespace kernel {
namespace xam {

static xe::hid::kinect::KinectInputDriver* kd() {
  return xe::hid::kinect::KinectInputDriver::instance();
}
// https://web.cs.ucdavis.edu/~okreylos/ResDev/Kinect/MainPage.html

struct X_NUI_DEVICE_STATUS {
  /* Notes:
     - for one side func of XamNuiGetDeviceStatus
       - if some data addressis less than zero then unk1 = it
       - else another func is called and its return can set unk1 = c0051200 or
     some value involving DetroitDeviceRequest
       - next PsCamDeviceRequest is called and if its return is less than zero
     then X_NUI_DEVICE_STATUS = return of PsCamDeviceRequest
       - else it equals an unknown local_1c
       - finally McaDeviceRequest is called and if its return is less than zero
     then unk2 = return of McaDeviceRequest
       - else it equals an unknown local_14
     - status can be set to X_NUI_DEVICE_STATUS[3] | 0x44 or | 0x40
  */
  xe::be<uint32_t> unk0;
  xe::be<uint32_t> unk1;
  xe::be<uint32_t> unk2;
  xe::be<uint32_t> status;
  xe::be<uint32_t> unk4;
  xe::be<uint32_t> unk5;
};
static_assert(sizeof(X_NUI_DEVICE_STATUS) == 24, "Size matters");

// Get
dword_result_t XamNuiGetDeviceStatus_entry(
    pointer_t<X_NUI_DEVICE_STATUS> status_ptr) {
  /* Notes:
     - it does return a value that is not always used
     - returns values are X_ERROR_SUCCESS, 0xC0050006, and others
     - 1) On func start *status_ptr = 0, status_ptr->unk1 = 0, status_ptr->unk2
     = 0, and status_ptr->status = 0
     - 2) calls XamXStudioRequest(6,&var <- = 0);
     - if return is greater than -1 && var & 0x80000000 != 0 then set
     status_ptr->unk1 = 0xC000009D, status_ptr->unk2 = 0xC000009D, and
     status_ptr->status = status_ptr[3] = 0x20
     - lots of branching functions after
  */

  status_ptr.Zero();
  if (!cvars::allow_nui_initialization) {
    return 0xC0050006;
  }
  // Initialize on first call — games poll GetDeviceStatus before anything
  // else and expect the device to become ready.
  if (kd() && !kd()->is_initialized()) {
    kd()->NuiInitialize(0x08);  // NUI_INITIALIZE_FLAG_USES_SKELETON
  }
  if (kd() && kd()->is_initialized()) {
    // status = 0x45 means device ready + sensor connected + motor connected
    status_ptr->status = 0x45u;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiGetDeviceStatus, kNone, kStub);

dword_result_t XamUserNuiGetUserIndex_entry(unknown_t unk, lpdword_t index) {
  XELOGI("XamUserNuiGetUserIndex called");
  if (index) { *index = 0; }
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndex, kNone, kStub);

dword_result_t XamUserNuiGetUserIndexForSignin_entry(lpdword_t index) {
  XELOGI("XamUserNuiGetUserIndexForSignin called");
  if (index) { *index = 0; }
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndexForSignin, kNone, kImplemented);

dword_result_t XamUserNuiGetUserIndexForBind_entry(lpdword_t index) {
  XELOGI("XamUserNuiGetUserIndexForBind called");
  if (index) { *index = 0; }
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndexForBind, kNone, kStub);

dword_result_t XamNuiGetDepthCalibration_entry(lpdword_t unk1) {
  /* Notes:
     - Possible returns X_STATUS_NO_SUCH_FILE, and 0x10000000
  */
  return X_STATUS_NO_SUCH_FILE;
}
DECLARE_XAM_EXPORT1(XamNuiGetDepthCalibration, kNone, kStub);

// Skeleton
qword_result_t XamNuiSkeletonGetBestSkeletonIndex_entry(int_t unk) {
  if (cvars::allow_nui_initialization && kd() && kd()->is_initialized()) {
    return 0;
  }
  return 0xffffffffffffffff;
}
DECLARE_XAM_EXPORT1(XamNuiSkeletonGetBestSkeletonIndex, kNone, kStub);

dword_result_t XamNuiEnableChatMic_entry(dword_t enable) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiEnableChatMic, kNone, kStub);

dword_result_t XamNuiStoreDepthCalibration_entry(lpvoid_t unk1, dword_t unk2) {
  return X_STATUS_NO_SUCH_FILE;
}
DECLARE_XAM_EXPORT1(XamNuiStoreDepthCalibration, kNone, kStub);

dword_result_t XamUserNuiIsBiometricEnabled_entry(dword_t user_index) {
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamUserNuiIsBiometricEnabled, kNone, kStub);

dword_result_t XamNuiIdentityGetQualityFlagsMessage_entry(lpvoid_t unk1, dword_t unk2) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityGetQualityFlagsMessage, kNone, kStub);

dword_result_t XamNuiCameraTiltSetCallback_entry(lpvoid_t callback, lpvoid_t context) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltSetCallback, kNone, kStub);

dword_result_t XamNuiIdentityGetQualityFlags_entry(dword_t unk1, lpdword_t out_flags) {
  if (out_flags) {
    *out_flags = 0;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityGetQualityFlags, kNone, kStub);

dword_result_t XamNuiHudGetEngagedEnrollmentIndex_entry(unknown_t unk) {
  return 0;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetEngagedEnrollmentIndex, kNone, kStub);

/* XamNuiCamera Notes
   - most require message calls to xam in 0x0002Bxxx area
*/

dword_result_t XamNuiCameraTiltGetStatus_entry(lpvoid_t unk) {
  if (unk) {
    std::memset(kernel_memory()->TranslateVirtual(unk.guest_address()), 0, 0x50);
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltGetStatus, kNone, kStub);

dword_result_t XamNuiCameraElevationGetAngle_entry(lpqword_t unk1,
                                                   lpdword_t unk2) {
  /* Notes:
     - Xam 12611 does not show what unk1 is used for (Ghidra)
  */
  uint32_t tilt_status[] = {0x58745373, 0x50};  // (XtSs)? & bytes to copy
  X_STATUS result = XamNuiCameraTiltGetStatus_entry(tilt_status);
  if (XSUCCEEDED(result)) {
    // operation here
    // *unk1 = output1
    // *unk2 = output2
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationGetAngle, kNone, kStub);

dword_result_t XamNuiCameraGetTiltControllerType_entry() {
  return 1; // 1 = Motor tilt controller connected
}
DECLARE_XAM_EXPORT1(XamNuiCameraGetTiltControllerType, kNone, kStub);

dword_result_t XamNuiCameraSetFlags_entry(qword_t unk1, dword_t unk2) {
  /* Notes:
     - if XamNuiCameraGetTiltControllerType returns 1 then operation is done
     - else 0xffffffff8007048f
  */
  X_STATUS result = X_E_DEVICE_NOT_CONNECTED;
  int Controller_Type = XamNuiCameraGetTiltControllerType_entry();

  if (Controller_Type == 1) {
    uint32_t tilt_status[] = {0x58745373, 0x50};  // (XtSs)? & bytes to copy
    result = XamNuiCameraTiltGetStatus_entry(tilt_status);
    if (XSUCCEEDED(result)) {
      // op here
      // result =
    }
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamNuiCameraSetFlags, kNone, kStub);

dword_result_t XamIsNuiUIActive_entry() {
  return kernel_state()->xam_state()->is_xam_dialog_present_.load();
}
DECLARE_XAM_EXPORT1(XamIsNuiUIActive, kNone, kImplemented);

dword_result_t XamNuiIsDeviceReady_entry() {
  if (!cvars::allow_nui_initialization) return 0;
  return (kd() && kd()->is_initialized()) ? 1u : 0u;
}
DECLARE_XAM_EXPORT1(XamNuiIsDeviceReady, kNone, kImplemented);

dword_result_t XamIsNuiAutomationEnabled_entry(unknown_t unk1, unknown_t unk2) {
  /* Notes:
     - XamIsNuiAutomationEnabled = XamIsNatalPlaybackEnabled
     - Always returns X_E_SUCCESS? Maybe check later versions
     - Recieves param but never interacts with them
     - No Operations
  */
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT2(XamIsNuiAutomationEnabled, kNone, kStub, kHighFrequency);

dword_result_t XamIsNatalPlaybackEnabled_entry(unknown_t unk1, unknown_t unk2) {
  /* Notes:
     - XamIsNuiAutomationEnabled = XamIsNatalPlaybackEnabled
     - Always returns X_E_SUCCESS? Maybe check later versions
     - Recieves param but never interacts with them
     - No Operations
  */
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT2(XamIsNatalPlaybackEnabled, kNone, kStub, kHighFrequency);

dword_result_t XamNuiIsChatMicEnabled_entry() {
  /* Notes:
     - calls a second function with a param of uint local_20 [4];
     - Second function calls ExGetXConfigSetting(7,9,local_30,0x1c,local_40);
     - Result is sent to *local_20[0] = ^
     - Once sent back to XamNuiIsChatMicEnabled it looks for byte that
     correlates to NUI mic setting
     - return uVar2 = (~(ulonglong)local_20[0] << 0x20) >> 0x23 & 1;
     - unless the second function returns something -1 or less then
     XamNuiIsChatMicEnabled 1
  */
  return false;
}
DECLARE_XAM_EXPORT1(XamNuiIsChatMicEnabled, kNone, kImplemented);

/* HUD Notes:
   - XamNuiHudGetEngagedTrackingID, XamNuiHudIsEnabled,
   XamNuiHudSetEngagedTrackingID, XamNuiHudInterpretFrame, and
   XamNuiHudGetEngagedEnrollmentIndex all utilize the same data address
*/
uint32_t nui_unknown_1 = 0;
uint32_t engaged_tracking_id = 0;
char nui_unknown_2 = '\0';

dword_result_t XamNuiHudSetEngagedTrackingID_entry(dword_t id) {
  engaged_tracking_id = id;
  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiHudSetEngagedTrackingID, kNone, kImplemented);
 
qword_result_t XamNuiHudGetEngagedTrackingID_entry() {
  return engaged_tracking_id;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetEngagedTrackingID, kNone, kImplemented);
 
dword_result_t XamNuiHudIsEnabled_entry() {
  return XamNuiIsDeviceReady_entry();
}
DECLARE_XAM_EXPORT1(XamNuiHudIsEnabled, kNone, kImplemented);

dword_result_t XamNuiHudGetInitializeFlags_entry() {
  /* HUD_Flags Notes:
     - set by 0x2B003
     - set to 0 by unnamed func alongside version_id
     - known values:
       - 0x40000000
       - 0x200
  */
  return 0;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetInitializeFlags, kNone, kImplemented);

void XamNuiHudGetVersions_entry(lpqword_t unk1, lpqword_t unk2) {
  /* version_id Notes:
     - set by 0x2B003
     - set to 0 by unnamed func alongside HUD_Flags
  */
  if (unk1) {
    *unk1 = 0;
  }
  if (unk2) {
    *unk2 = 0;
  }
}
DECLARE_XAM_EXPORT1(XamNuiHudGetVersions, kNone, kImplemented);

/* XamNuiIdentity Notes:
   - most require message calls to xam in 0x0002Cxxx area
*/
uint64_t NUI_Session_Id = 0;

qword_result_t XamNuiIdentityGetSessionId_entry() {
  if (NUI_Session_Id == 0) {
    // xboxkrnl::XeCryptRandom_entry(NUI_Session_Id, 8);
    NUI_Session_Id = 0xDEADF00DDEADF00D;
  }
  return NUI_Session_Id;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityGetSessionId, kNone, kImplemented);

dword_result_t XamNuiIdentityEnrollForSignIn_entry(dword_t unk1, qword_t unk2,
                                                   qword_t unk3, dword_t unk4) {
  /* Notes:
     - Decompiler issues so double check
  */
  if (XamNuiHudIsEnabled_entry() == false) {
    return X_E_FAIL;
  }
  // buffer [2]
  // buffer[0] = unk
  // var = unk4
  // return func(0xfe,0x2c010,buffer,0xc,unk3);
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityEnrollForSignIn, kNone, kStub);

dword_result_t XamNuiIdentityAbort_entry(dword_t unk) {
  if (XamNuiHudIsEnabled_entry() == false) {
    return X_E_FAIL;
  }
  // buffer [4]
  // buffer[0] = unk
  // return func(0xfe,0x2c00e,buffer,4,0)
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityAbort, kNone, kStub);

// Other
dword_result_t XamUserNuiEnableBiometric_entry(dword_t user_index,
                                               int_t enable) {
  return X_E_INVALIDARG;
}
DECLARE_XAM_EXPORT1(XamUserNuiEnableBiometric, kNone, kStub);

void XamNuiPlayerEngagementUpdate_entry(qword_t unk1, unknown_t unk2,
                                        lpunknown_t unk3) {
  /* Notes:
     - Only calls a second function with the params unk3, 0, and 0x1c in that
     order
  */
}
DECLARE_XAM_EXPORT1(XamNuiPlayerEngagementUpdate, kNone, kStub);

dword_result_t XamXStudioRequest_entry(dword_t cmd, lpvoid_t p_in_out) {
  if (cmd == 6 && p_in_out) {
    auto* out = kernel_state()->memory()->TranslateVirtual<uint32_t*>(
        p_in_out.guest_address());
    if (cvars::allow_nui_initialization) {
      // Report Kinect as present (no error bits).
      *out = 0;
    } else {
      // Report Kinect as NOT present (bit 31 set) so callers that check this
      // before XamNuiGetDeviceStatus don't spin-wait.
      *out = xe::byte_swap(uint32_t(0x80000000));
    }
    return X_ERROR_SUCCESS;
  }
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT2(XamXStudioRequest, kNone, kStub, kHighFrequency);

// --- Camera Elevation & Motor Control ---
dword_result_t XamNuiCameraRememberFloor_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamNuiCameraRememberFloor, kNone, kStub);

dword_result_t XamNuiCameraElevationSetAngle_entry(dword_t angle) {
  if (kd()) { return kd()->NuiCameraSetElevation(static_cast<int32_t>(angle)); }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationSetAngle, kNone, kStub);

dword_result_t XamNuiCameraElevationAutoTilt_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamNuiCameraElevationAutoTilt, kNone, kStub);

dword_result_t XamNuiCameraElevationStopMovement_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamNuiCameraElevationStopMovement, kNone, kStub);

dword_result_t XamNuiCameraElevationSetCallback_entry(lpvoid_t callback, lpvoid_t context) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamNuiCameraElevationSetCallback, kNone, kStub);

dword_result_t XamNuiCameraElevationReverseAutoTilt_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamNuiCameraElevationReverseAutoTilt, kNone, kStub);

dword_result_t XamNuiCameraAdjustTilt_entry(dword_t angle) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamNuiCameraAdjustTilt, kNone, kStub);

dword_result_t XamNuiCameraTiltReportStatus_entry(lpvoid_t status) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamNuiCameraTiltReportStatus, kNone, kStub);

dword_result_t XamNuiNatalCameraUpdateStarting_entry(unknown_t unk) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamNuiNatalCameraUpdateStarting, kNone, kStub);

dword_result_t XamNuiNatalCameraUpdateComplete_entry(unknown_t unk) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamNuiNatalCameraUpdateComplete, kNone, kStub);

// --- Identity & Binding ---
dword_result_t XamUserNuiBind_entry(dword_t user_index, dword_t tracking_id) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamUserNuiBind, kNone, kStub);

dword_result_t XamUserNuiUnbind_entry(dword_t user_index) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamUserNuiUnbind, kNone, kStub);

dword_result_t XamUserNuiGetEnrollmentIndex_entry(dword_t user_index, lpdword_t index) {
  if (index) { *index = 0; }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetEnrollmentIndex, kNone, kStub);

dword_result_t XamNuiIdentityIdentifyWithBiometric_entry(lpvoid_t unk1, lpvoid_t unk2) { return X_E_FAIL; }
DECLARE_XAM_EXPORT1(XamNuiIdentityIdentifyWithBiometric, kNone, kStub);

// --- Gestures, HUD, & Automation ---
dword_result_t XamNuiGetSystemGestureControl_entry(lpvoid_t control) { return X_E_FAIL; }
DECLARE_XAM_EXPORT1(XamNuiGetSystemGestureControl, kNone, kStub);

dword_result_t XamNuiHudInterpretFrame_entry(lpvoid_t frame) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamNuiHudInterpretFrame, kNone, kStub);

dword_result_t XamNuiHudEnableInputFilter_entry(dword_t enable) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamNuiHudEnableInputFilter, kNone, kStub);

dword_result_t ControlpackNuiCursorSetTrackingId_entry(dword_t id) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(ControlpackNuiCursorSetTrackingId, kNone, kStub);

dword_result_t ControlPackSideNavControlGetNuiHandle_entry(lpvoid_t handle) { return X_E_FAIL; }
DECLARE_XAM_EXPORT1(ControlPackSideNavControlGetNuiHandle, kNone, kStub);

dword_result_t XamEnableNuiAutomation_entry(dword_t enable) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamEnableNuiAutomation, kNone, kStub);

dword_result_t XamEnableNatalPlayback_entry(dword_t enable) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamEnableNatalPlayback, kNone, kStub);

// --- Hardware Details ---
dword_result_t XamNuiGetDeviceSerialNumber_entry(lpstring_t serial_out, dword_t max_len) {
  if (serial_out) { std::strncpy(serial_out, "KINECTEMULATOR123", max_len); }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiGetDeviceSerialNumber, kNone, kStub);

dword_result_t XamNuiGetLoadedDepthCalibration_entry(lpvoid_t out_calib) { return X_STATUS_NO_SUCH_FILE; }
DECLARE_XAM_EXPORT1(XamNuiGetLoadedDepthCalibration, kNone, kStub);

dword_result_t XamNuiGetTrueColorInfo_entry(lpvoid_t out_info) { return X_E_FAIL; }
DECLARE_XAM_EXPORT1(XamNuiGetTrueColorInfo, kNone, kStub);

dword_result_t XamNuiGetCameraIntrinsics_entry(lpvoid_t out_intrinsics) { return X_E_FAIL; }
DECLARE_XAM_EXPORT1(XamNuiGetCameraIntrinsics, kNone, kStub);

dword_result_t XamNuiGetFanRate_entry(lpdword_t out_rate) {
  if (out_rate) { *out_rate = 50; } // 50% fan speed
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiGetFanRate, kNone, kStub);

dword_result_t XamNuiGetSupportString_entry(lpstring_t out_str, dword_t max_len) {
  if (out_str) { std::strncpy(out_str, "Kinect Support Stub Active", max_len); }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiGetSupportString, kNone, kStub);

// --- Audio & Misc ---
dword_result_t XamNatalDeviceAudioCalibrate_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamNatalDeviceAudioCalibrate, kNone, kStub);

dword_result_t XamNuiSetForceDeviceOff_entry(dword_t force_off) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamNuiSetForceDeviceOff, kNone, kStub);

dword_result_t XamNuiSkeletonScoreUpdate_entry(lpvoid_t unk) { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamNuiSkeletonScoreUpdate, kNone, kStub);

// --- Missing NUI Show UI exports (needed by Fruit Ninja Kinect) ---
dword_result_t XamShowNuiGamerCardUIForXUID_entry(dword_t user_index, qword_t xuid) {
  XELOGI("XamShowNuiGamerCardUIForXUID(user={}, xuid={:016X}) stub", user_index.value(), xuid.value());
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiGamerCardUIForXUID, kNone, kStub);

dword_result_t XamShowNuiAchievementsUI_entry(dword_t user_index) {
  XELOGI("XamShowNuiAchievementsUI(user={}) stub", user_index.value());
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiAchievementsUI, kNone, kStub);

dword_result_t XamShowNuiMarketplaceUI_entry(dword_t user_index, dword_t flags) {
  XELOGI("XamShowNuiMarketplaceUI(user={}, flags={:08X}) stub", user_index.value(), flags.value());
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiMarketplaceUI, kNone, kStub);

dword_result_t XamShowNuiDeviceSelectorUI_entry(dword_t user_index, dword_t content_type,
                                                 dword_t content_flags, qword_t total_requested,
                                                 lpdword_t device_id_ptr, lpvoid_t overlapped) {
  XELOGI("XamShowNuiDeviceSelectorUI(user={}) stub", user_index.value());
  if (device_id_ptr) { *device_id_ptr = 1; }  // fake device
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiDeviceSelectorUI, kNone, kStub);

dword_result_t XamShowNuiDirtyDiscErrorUI_entry(dword_t user_index) {
  XELOGI("XamShowNuiDirtyDiscErrorUI(user={}) stub", user_index.value());
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiDirtyDiscErrorUI, kNone, kStub);

// --- XamCache stubs (Fruit Ninja imports these for NUI identity caching) ---
dword_result_t XamCacheOpenFile_entry(dword_t flags, lpstring_t path,
                                       lpdword_t handle_out, lpvoid_t overlapped) {
  XELOGI("XamCacheOpenFile('{}', flags={:08X}) stub", path ? path.value() : "<null>", flags.value());
  if (handle_out) { *handle_out = 0xCAFE0001; }  // fake handle
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamCacheOpenFile, kNone, kStub);

dword_result_t XamCacheCloseFile_entry(dword_t handle) {
  XELOGI("XamCacheCloseFile(handle={:08X}) stub", handle.value());
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamCacheCloseFile, kNone, kStub);

dword_result_t XamCacheReset_entry(dword_t flags) {
  XELOGI("XamCacheReset(flags={:08X}) stub", flags.value());
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamCacheReset, kNone, kStub);

// --- Biometric data stubs ---
dword_result_t XamReadBiometricData_entry(dword_t user_index, lpvoid_t buffer, dword_t buffer_size) {
  XELOGI("XamReadBiometricData(user={}, size={}) stub", user_index.value(), buffer_size.value());
  if (buffer && buffer_size) {
    std::memset(kernel_memory()->TranslateVirtual(buffer.guest_address()), 0, buffer_size);
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamReadBiometricData, kNone, kStub);

dword_result_t XamWriteBiometricData_entry(dword_t user_index, lpvoid_t buffer, dword_t buffer_size) {
  XELOGI("XamWriteBiometricData(user={}, size={}) stub", user_index.value(), buffer_size.value());
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamWriteBiometricData, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(NUI);
