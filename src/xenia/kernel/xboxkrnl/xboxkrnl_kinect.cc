/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// PsCam / Mca / Detroit -- Xbox 360 Kinect kernel device request interfaces.

#include <cstdio>
#include <string>
#include <algorithm>

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// ---------------------------------------------------------------------------
// PsCam / Mca / Detroit -- Kinect device request interfaces
// ---------------------------------------------------------------------------

static uint32_t ps_cam_call_count = 0;

static uint32_t GetPsCamRequestCode(Memory* mem, uint32_t arg2) {
  if (arg2 >= 0x10000 && arg2 < 0xF0000000) {
    auto* ctx = mem->TranslateVirtual<uint32_t*>(arg2);
    if (ctx) {
      return xe::load_and_swap<uint32_t>(ctx + 2);
    }
  }
  return 0;
}

dword_result_t PsCamDeviceRequest_entry(
    dword_t arg0, dword_t arg1, dword_t arg2,
    dword_t arg3, dword_t arg4, dword_t arg5) {
  ps_cam_call_count++;

  auto* mem = kernel_state()->memory();
  uint32_t request_code = GetPsCamRequestCode(mem, arg2.value());

  XELOGI("PsCamDeviceRequest [#{}]: arg0={:08X}, arg1={:08X}, arg2={:08X}, arg3={:08X}, arg4={:08X}, arg5={:08X}, req=0x{:X}",
         ps_cam_call_count, arg0.value(), arg1.value(), arg2.value(),
         arg3.value(), arg4.value(), arg5.value(), request_code);

  // Dump the output buffer contents for first few calls
  if (arg0.value() >= 0x10000 && arg0.value() < 0xF0000000 && ps_cam_call_count <= 16) {
    auto* out = mem->TranslateVirtual<uint8_t*>(arg0.value());
    if (out) {
      uint32_t buf_size = 0xBD;
      if (arg1.value() > arg0.value() && (arg1.value() - arg0.value()) < 0x1000) {
        buf_size = arg1.value() - arg0.value();
      }
      XELOGI("PsCamDeviceRequest: buf[{:02X}]={:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
             "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
             "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
             buf_size,
             out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7],
             out[8], out[9], out[10], out[11], out[12], out[13], out[14], out[15],
             out[16], out[17], out[18], out[19], out[20], out[21], out[22], out[23]);
    }
  }

  // Write device state into the kernel write region (first 20 bytes)
  if (arg0.value() >= 0x10000 && arg0.value() < 0xF0000000) {
    auto* out = mem->TranslateVirtual<uint8_t*>(arg0.value());
    if (out) {
      // Bytes 0x00-0x13 are the kernel write region:
      //   0x00: device handle / status (non-zero = device present)
      //   0x04: sub-status (0 = no error)
      //   0x08: device version (1 = Kinect v1)
      //   0x0C: capabilities bitmask (depth|video|audio)
      //   0x10: max skeleton slots (6)
      xe::store_and_swap<uint32_t>(out + 0x00, 0x00000001);  // device handle
      xe::store_and_swap<uint32_t>(out + 0x04, 0x00000000);  // status = OK
      xe::store_and_swap<uint32_t>(out + 0x08, 0x00000001);  // version = Kinect v1
      xe::store_and_swap<uint32_t>(out + 0x0C, 0x00000007);  // caps: depth|video|audio
      xe::store_and_swap<uint32_t>(out + 0x10, 0x00000006);  // max skeletons
    }
  }

  // arg3 status field — write 0 (S_OK)
  if (arg3.value() >= 0x10000 && arg3.value() < 0xF0000000) {
    auto* status = mem->TranslateVirtual<uint32_t*>(arg3.value());
    if (status) {
      xe::store_and_swap<uint32_t>(status, 0);
    }
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(PsCamDeviceRequest, kNone, kStub);

dword_result_t McaDeviceRequest_entry(
    dword_t request_code, lpvoid_t input_buffer, dword_t input_length,
    lpvoid_t output_buffer, dword_t output_length, lpdword_t bytes_returned) {
  XELOGI("McaDeviceRequest: code={}, in_len={}, out_len={}", request_code.value(), input_length.value(), output_length.value());

  auto* mem = kernel_state()->memory();

  if (input_buffer && input_length.value() > 0 && input_length.value() < 0x1000) {
    auto* in = mem->TranslateVirtual<uint8_t*>(input_buffer.guest_address());
    if (in) {
      uint32_t len = std::min<uint32_t>(input_length.value(), 32);
      std::string hex;
      for (uint32_t i = 0; i < len; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X ", in[i]);
        hex += buf;
      }
      XELOGI("McaDeviceRequest: input buffer: {}", hex);
    }
  }

  if (bytes_returned) {
    *bytes_returned = 0;
  }
  if (output_buffer && output_length) {
    auto* dst = kernel_state()->memory()->TranslateVirtual(output_buffer.guest_address());
    if (dst) {
      std::memset(dst, 0, std::min<uint32_t>(output_length.value(), 4));
      if (output_length.value() >= 4) {
        xe::store_and_swap<uint32_t>(dst, 1);
        if (bytes_returned) {
          *bytes_returned = 4;
        }
      }
    }
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(McaDeviceRequest, kNone, kStub);

dword_result_t DetroitDeviceRequest_entry(
    dword_t request_code, lpvoid_t input_buffer, dword_t input_length,
    lpvoid_t output_buffer, dword_t output_length, lpdword_t bytes_returned) {
  XELOGI("DetroitDeviceRequest: code={}, in_len={}, out_len={}", request_code.value(), input_length.value(), output_length.value());

  auto* mem = kernel_state()->memory();

  if (input_buffer && input_length.value() > 0 && input_length.value() < 0x1000) {
    auto* in = mem->TranslateVirtual<uint8_t*>(input_buffer.guest_address());
    if (in) {
      uint32_t len = std::min<uint32_t>(input_length.value(), 32);
      std::string hex;
      for (uint32_t i = 0; i < len; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X ", in[i]);
        hex += buf;
      }
      XELOGI("DetroitDeviceRequest: input buffer: {}", hex);
    }
  }

  if (bytes_returned) {
    *bytes_returned = 0;
  }
  if (output_buffer && output_length) {
    auto* dst = kernel_state()->memory()->TranslateVirtual(output_buffer.guest_address());
    if (dst) {
      std::memset(dst, 0, std::min<uint32_t>(output_length.value(), 4));
      if (output_length.value() >= 4) {
        xe::store_and_swap<uint32_t>(dst, 1);
        if (bytes_returned) {
          *bytes_returned = 4;
        }
      }
    }
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(DetroitDeviceRequest, kNone, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
