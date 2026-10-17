/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/diagnostics_dump.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

DEFINE_bool(dbg_break_on_guest_breakpoint, false,
            "Break into the host debugger when the guest calls DbgBreakPoint. "
            "Off by default: with no kernel debugger attached, retail "
            "consoles ignore it and continue.",
            "Kernel");

namespace xe {
namespace kernel {
namespace xboxkrnl {

// One-shot dump of the title's image so offline disassembly works on
// compressed XEXs, which Xenia decompresses in guest RAM. Isolated in a
// no-unwind function because it uses SEH to survive unmapped pages
// (TranslateVirtual hands back a bad host pointer for those).
static void __declspec(noinline) DumpGuestImageNoUnwind(uint32_t code_base,
                                                        uint32_t code_size,
                                                        const char* filename) {
  auto* mem = kernel_state()->memory();
  FILE* f = fopen(filename, "wb");
  if (!f) {
    return;
  }
  uint8_t* buf = (uint8_t*)_alloca(0x1000);
  uint32_t pages_written = 0;
  for (uint32_t off = 0; off < code_size; off += 0x1000) {
    auto* p = mem->TranslateVirtual<uint8_t*>(code_base + off);
    memset(buf, 0, 0x1000);
    if (p) {
      __try {
        memcpy(buf, p, 0x1000);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
    fwrite(buf, 1, 0x1000, f);
    ++pages_written;
  }
  fflush(f);
  fclose(f);
  XELOGI("Guest image dump: {} pages ({} bytes) written to {}", pages_written,
         code_size, filename);
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {

// Exposed for other kernel/xam call sites that need a snapshot of the title's
// decompressed image (e.g. the NUI service handshake in xam_nui.cc).
void DumpGuestImageForDiagnostics(const char* filename, uint32_t base,
                                  uint32_t size) {
  xboxkrnl::DumpGuestImageNoUnwind(base, size, filename);
}

using ::xe::kernel::DumpGuestImageForDiagnostics;

}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {
namespace xboxkrnl {

using ::xe::kernel::DumpGuestImageForDiagnostics;

static bool dumped_code = false;

void DbgBreakPoint_entry() {
  if (cvars::dbg_break_on_guest_breakpoint) {
    xe::debugging::Break();
    return;
  }
  // No kernel debugger attached: log and continue, like retail hardware.
  auto thread = XThread::GetCurrentThread();
  uint32_t lr = 0;
  if (thread && thread->thread_state() && thread->thread_state()->context()) {
    lr = static_cast<uint32_t>(thread->thread_state()->context()->lr);
  }
  XELOGI("DbgBreakPoint: guest break ignored (tid={:08X} lr={:08X})",
         thread ? thread->thread_id() : 0, lr);
  if (lr) {
    auto* code = kernel_state()->memory()->TranslateVirtual<uint8_t*>(lr - 0x1C);
    if (code) {
      std::string words;
      for (int i = 0; i < 16; ++i) {
        words += fmt::format("{:08X} ", xe::load_and_swap<uint32_t>(code + i * 4));
      }
      XELOGI("DbgBreakPoint code @{:010X}: {}", lr - 0x1C, words);
    }
    auto* errstr = kernel_state()->memory()->TranslateVirtual<const char*>(0x8207FF44);
    if (errstr) {
      XELOGI("DbgBreakPoint error string @0x8207FF44: '{}'", errstr);
    }
  }
  if (!dumped_code) {
    dumped_code = true;
    DumpGuestImageNoUnwind(0x82000000, 0x400000, "guest_nuitest_dump.bin");
  }
}
DECLARE_XBOXKRNL_EXPORT2(DbgBreakPoint, kDebug, kStub, kImportant);

// https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx
typedef struct {
  xe::be<uint32_t> type;
  xe::be<uint32_t> name_ptr;
  xe::be<uint32_t> thread_id;
  xe::be<uint32_t> flags;
} X_THREADNAME_INFO;
static_assert_size(X_THREADNAME_INFO, 0x10);

void HandleSetThreadName(pointer_t<X_EXCEPTION_RECORD> record) {
  // SetThreadName. FFS.
  // https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx

  // TODO(benvanik): check record->number_parameters to make sure it's a
  // correct size.
  auto thread_info =
      reinterpret_cast<X_THREADNAME_INFO*>(&record->exception_information[0]);

  assert_true(thread_info->type == 0x1000);

  if (!thread_info->name_ptr) {
    XELOGD("SetThreadName called with null name_ptr");
    return;
  }

  // 4D5307D6 (and its demo) has a bug where it ends up passing freed memory for
  // the name, so at the point of SetThreadName it's filled with junk.

  // TODO(gibbed): cvar for thread name encoding for conversion, some games use
  // SJIS and there's no way to automatically know this.
  auto name = std::string(
      kernel_memory()->TranslateVirtual<const char*>(thread_info->name_ptr));
  std::ranges::replace_if(name, [](auto c) { return c < 32 || c > 127; }, '?');

  object_ref<XThread> thread;
  if (thread_info->thread_id == -1) {
    // Current thread.
    thread = retain_object(XThread::GetCurrentThread());
  } else {
    // Lookup thread by ID.
    thread = kernel_state()->GetThreadByID(thread_info->thread_id);
  }

  if (thread) {
    XELOGD("SetThreadName({}, {})", thread->thread_id(), name);
    thread->set_name(name);
  }

  // TODO(benvanik): unwinding required here?
}

typedef struct {
  xe::be<int32_t> mdisp;
  xe::be<int32_t> pdisp;
  xe::be<int32_t> vdisp;
} x_PMD;

typedef struct {
  xe::be<uint32_t> properties;
  xe::be<uint32_t> type_ptr;
  x_PMD this_displacement;
  xe::be<int32_t> size_or_offset;
  xe::be<uint32_t> copy_function_ptr;
} x_s__CatchableType;

typedef struct {
  xe::be<int32_t> number_catchable_types;
  xe::be<uint32_t> catchable_type_ptrs[1];
} x_s__CatchableTypeArray;

typedef struct {
  xe::be<uint32_t> attributes;
  xe::be<uint32_t> unwind_ptr;
  xe::be<uint32_t> forward_compat_ptr;
  xe::be<uint32_t> catchable_type_array_ptr;
} x_s__ThrowInfo;

void HandleCppException(pointer_t<X_EXCEPTION_RECORD> record) {
  // C++ exception.
  // https://blogs.msdn.com/b/oldnewthing/archive/2010/07/30/10044061.aspx
  // http://www.drdobbs.com/visual-c-exception-handling-instrumentat/184416600
  // http://www.openrce.org/articles/full_view/21

  assert_true(record->number_parameters == 3);
  assert_true(record->exception_information[0] == 0x19930520);

  auto thrown_ptr = record->exception_information[1];
  auto thrown = kernel_memory()->TranslateVirtual(thrown_ptr);
  auto vftable_ptr = *reinterpret_cast<xe::be<uint32_t>*>(thrown);

  auto throw_info_ptr = record->exception_information[2];
  auto throw_info =
      kernel_memory()->TranslateVirtual<x_s__ThrowInfo*>(throw_info_ptr);
  auto catchable_types =
      kernel_memory()->TranslateVirtual<x_s__CatchableTypeArray*>(
          throw_info->catchable_type_array_ptr);

  // xe::debugging::Break();
  XELOGE("Guest attempted to throw a C++ exception!");
}

void RtlRaiseException_entry(pointer_t<X_EXCEPTION_RECORD> record) {
  switch (record->code) {
    case 0x406D1388: {
      HandleSetThreadName(record);
      return;
    }
    case 0xE06D7363: {
      HandleCppException(record);
      return;
    }
  }

  // TODO(benvanik): unwinding.
  // This is going to suck.
  // xe::debugging::Break();

  // RtlRaiseException definitely wasn't a noreturn function, we can return
  // safe-ish
  XELOGE("Guest attempted to trigger a breakpoint!");
}
DECLARE_XBOXKRNL_EXPORT2(RtlRaiseException, kDebug, kStub, kImportant);

void KeBugCheckEx_entry(dword_t code, dword_t param1, dword_t param2,
                        dword_t param3, dword_t param4) {
  auto msg =
      fmt::format("*** STOP: 0x{:08X} (0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X})",
                  static_cast<uint32_t>(code), static_cast<uint32_t>(param1),
                  static_cast<uint32_t>(param2), static_cast<uint32_t>(param3),
                  static_cast<uint32_t>(param4));
  XELOGE("{}", msg);
  fflush(stdout);

  if (xe::debugging::IsDebuggerAttached()) {
    xe::debugging::Break();
  }

  // Show crash dialog and suspend the guest thread instead of killing the
  // host process.
  auto current_thread = kernel::XThread::GetCurrentThread();
  const auto* emulator = kernel_state()->emulator();
  auto* display_window = emulator->display_window();
  auto* imgui_drawer = emulator->imgui_drawer();
  if (display_window && imgui_drawer) {
    auto dlg_msg = fmt::format(
        "The guest kernel has crashed (KeBugCheck).\n\n{}\n\n"
        "The faulting thread has been suspended.",
        msg);
    display_window->app_context().CallInUIThreadSynchronous(
        [imgui_drawer, &dlg_msg]() {
          xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer,
                                              "Guest Kernel Crash", dlg_msg);
        });
  }

  if (current_thread) {
    current_thread->Suspend(nullptr);
  }
}
DECLARE_XBOXKRNL_EXPORT2(KeBugCheckEx, kDebug, kStub, kImportant);

void KeBugCheck_entry(dword_t code) { KeBugCheckEx_entry(code, 0, 0, 0, 0); }
DECLARE_XBOXKRNL_EXPORT2(KeBugCheck, kDebug, kImplemented, kImportant);

dword_result_t EtxProducerRegister_entry(lpvoid_t p1, lpvoid_t p2, lpvoid_t p3,
                                          dword_t p4, lpvoid_t p5, lpvoid_t p6) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(EtxProducerRegister, kDebug, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Debug);
