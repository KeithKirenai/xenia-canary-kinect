/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// LDI -- LZX Decompression Interface (xboxkrnl.exe exports).

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/lzx.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// ---------------------------------------------------------------------------
// LDI -- LZX Decompression Interface
// ---------------------------------------------------------------------------

dword_result_t LDICreateDecompression_entry(dword_t cb_data_block_max,
                                            lpvoid_t pv_configuration,
                                            lpvoid_t pfn_ma, lpvoid_t pfn_mf,
                                            lpdword_t pcb_src_used,
                                            lpdword_t ph_decompression) {
  XELOGI("LDICreateDecompression: block_max={}", cb_data_block_max.value());
  if (ph_decompression) {
    *ph_decompression = 0xDEDE;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(LDICreateDecompression, kNone, kImplemented);

dword_result_t LDIDecompress_entry(dword_t h_decompression, lpvoid_t pb_dst,
                                   dword_t cb_dst, lpvoid_t pb_src,
                                   lpdword_t pcb_src_used) {
  if (h_decompression != 0xDEDE) {
    return 0x80000008;  // E_INVALIDARG
  }
  auto* src_data =
      kernel_state()->memory()->TranslateVirtual(pb_src.guest_address());
  auto* dst_data =
      kernel_state()->memory()->TranslateVirtual(pb_dst.guest_address());

  if (!src_data || !dst_data) {
    return 0x80000008;
  }

  int result =
      lzx_decompress(src_data, cb_dst, dst_data, cb_dst, 0x8000, nullptr, 0);
  if (result < 0) {
    XELOGE("LDIDecompress: lzx_decompress failed with code {}", result);
    return X_E_FAIL;
  }

  if (pcb_src_used) {
    *pcb_src_used = cb_dst.value();
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDecompress, kNone, kImplemented);

dword_result_t LDIDestroyDecompression_entry(dword_t h_decompression) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDestroyDecompression, kNone, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Ldi);
