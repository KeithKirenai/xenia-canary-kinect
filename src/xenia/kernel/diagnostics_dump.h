/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

// Diagnostics helpers shared between the kernel and XAM layers.
//
// Currently: snapshotting the title's decompressed image so NUI runtime code can
// be disassembled offline (XEX images are compressed on disc; Xenia decompresses
// them into guest RAM where the emulator can read them).

#pragma once

#include <cstdint>

namespace xe {
namespace kernel {

// Dumps [base, base + size) of guest memory to `filename` as a flat binary.
// Unmapped pages are written as zeroes. Diagnostic only -- not for production
// use, since it walks the whole range page by page.
void DumpGuestImageForDiagnostics(const char* filename, uint32_t base,
                                  uint32_t size);

}  // namespace kernel
}  // namespace xe