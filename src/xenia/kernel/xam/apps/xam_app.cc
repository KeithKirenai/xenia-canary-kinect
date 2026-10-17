/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xam_app.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/kernel_module.h"
#include "xenia/kernel/diagnostics_dump.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/hid/kinect/kinect_input_driver.h"
#include "xenia/base/memory.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/user_module.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xsemaphore.h"

static xe::hid::kinect::KinectInputDriver* kd() {
  return xe::hid::kinect::KinectInputDriver::instance();
}

DEFINE_bool(nui_clone_stream_template, false,
            "On a follow-up 0x2B001, copy the NUI object template captured from "
            "the first call into the fresh object instead of skipping it. The "
            "kernel is expected to populate that struct and the call is "
            "ignored today.",
            "XAM");

DEFINE_int32(nui_notify_hardware_delay, 10,
           "Seconds after the 0x2B001 bind before broadcasting "
           "kXNotificationSystemNUIHardwareStatusChanged. The title polls "
           "XamNuiGetDeviceStatus exactly once and then parks its NUI worker "
           "threads on an event, so it needs a status-change notification to "
           "start the skeleton stream. Broadcasting straight from the 0x2B001 "
           "handler wedged the title on the splash, hence the delay. -1 "
           "disables.",
           "XAM");
DEFINE_int32(nui_notify_hardware_state, 1,
           "device_state value passed to the NUI hardware status notification.",
           "XAM");

DEFINE_bool(nui_mock_device, true,
            "Fabricate a mock INuiSensor COM device for XAM message 0x2B001. "
            "Set false to reproduce upstream (no-Kinect) behaviour.",
            "XAM");

DEFINE_bool(nui_nui_write_shared_frame_buffer, false,
            "Write skeleton frames directly into the guest buffer passed in "
            "out[7] of XAM message 0x2B001. EXPERIMENTAL, OFF by default: the "
            "page is writable and mapped, but writing there makes Kinect "
            "Adventures hang, so the frame layout the title expects at that "
            "address is not yet confirmed. Only enable for debugging.",
            "XAM");

static void NuiCOMStub_QueryInterface(xe::cpu::ppc::PPCContext* context) {
  uint32_t ppvObject = static_cast<uint32_t>(context->r[5]);
  if (ppvObject) {
    auto* ppv_val = context->TranslateVirtual<uint32_t*>(ppvObject);
    if (ppv_val) {
      xe::store_and_swap<uint32_t>(ppv_val, static_cast<uint32_t>(context->r[3]));
      XELOGI("NuiCOMStub_QueryInterface: wrote interface ptr {:08X} to ppvObject {:08X}",
             static_cast<uint32_t>(context->r[3]), ppvObject);
    }
  }
  context->r[3] = 0; // S_OK
}

static int g_nui_com_call_count = 0;
static void NuiCOMStub_Success(xe::cpu::ppc::PPCContext* context) {
  uint32_t this_ptr = static_cast<uint32_t>(context->r[3]);
  uint32_t method_id = g_nui_com_call_count++;
  if (method_id < 20) {
    XELOGI("NuiCOMStub_Success: call #{:d} this={:08X} r3={:08X} r4={:08X} r5={:08X}",
           method_id, this_ptr,
           static_cast<uint32_t>(context->r[3]),
           static_cast<uint32_t>(context->r[4]),
           static_cast<uint32_t>(context->r[5]));
  }
  context->r[3] = 0; // S_OK / 0
}

static void NuiCOMStub_NuiStatus(xe::cpu::ppc::PPCContext* context) {
  // INuiSensor::NuiStatus() -> S_OK (0) if initialized
  context->r[3] = 0; // S_OK
}

static void NuiCOMStub_ImageStreamOpen(xe::cpu::ppc::PPCContext* context) {
  // r3 = this, r4 = eImageType, r5 = eResolution, r6 = flags, r7 = frameLimit, r8 = hNextFrameEvent, r9 = phStreamHandle
  uint32_t phStreamHandle = static_cast<uint32_t>(context->r[9]);
  if (phStreamHandle) {
    auto* out = context->TranslateVirtual<uint32_t*>(phStreamHandle);
    if (out) {
      uint32_t fake_handle = 0xCAFE0000 | (static_cast<uint32_t>(context->r[4]) & 0xFFFF);
      xe::store_and_swap<uint32_t>(out, fake_handle);
      XELOGI("NuiCOMStub_ImageStreamOpen: returned stream handle {:08X} to {:08X}",
             fake_handle, phStreamHandle);
    }
  }
  context->r[3] = 0; // S_OK
}

static void NuiCOMStub_SkeletonGetNextFrame(xe::cpu::ppc::PPCContext* context) {
  // r3 = this, r4 = dwMillisecondsToWait, r5 = pSkeletonFrame
  static std::atomic<uint64_t> skel_calls{0};
  uint64_t n = skel_calls.fetch_add(1);
  uint32_t frame_ptr = static_cast<uint32_t>(context->r[5]);
  if (n < 5 || n % 300 == 299) {
    XELOGI("NuiCOMStub_SkeletonGetNextFrame #{}: this={:08X} wait={} frame_ptr={:08X}",
           n + 1, static_cast<uint32_t>(context->r[3]),
           static_cast<uint32_t>(context->r[4]), frame_ptr);
  }
  {
    // Diagnostic: dump the first skeleton entry right after serialization so
    // guest-side decoding mismatches are visible in the host log.
    static std::atomic<uint64_t> dump_count{0};
    uint64_t d = dump_count.fetch_add(1);
    if (d < 3 || d == 600) {
      xe::hid::kinect::X_NUI_SKELETON_FRAME probe{};
      if (kd() && kd()->is_initialized()) {
        std::ignore = kd()->NuiSkeletonGetNextFrame(0, &probe);
      }
      XELOGI("NuiSkeletonGetNextFrame dump #{}: frame={} state0={} id0={} pos=({:f},{:f},{:f})",
             d + 1, probe.dwFrameNumber,
             static_cast<int>(probe.SkeletonData[0].eTrackingState),
             probe.SkeletonData[0].dwTrackingID,
             probe.SkeletonData[0].Position.x, probe.SkeletonData[0].Position.y,
             probe.SkeletonData[0].Position.z);
    }
  }
  if (frame_ptr && kd() && kd()->is_initialized()) {
    xe::hid::kinect::X_NUI_SKELETON_FRAME host_frame{};
    if (XSUCCEEDED(kd()->NuiSkeletonGetNextFrame(0, &host_frame))) {
      auto* dst = context->TranslateVirtual<uint8_t*>(frame_ptr);
      if (dst) {
        uint64_t ts = static_cast<uint64_t>(host_frame.liTimeStamp);
        xe::store_and_swap<uint64_t>(reinterpret_cast<uint64_t*>(dst + 0), ts);
        xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(dst + 8), host_frame.dwFrameNumber);
        xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(dst + 12), host_frame.dwFlags);

        auto write_float = [](uint8_t* d, float v) {
          uint32_t bits;
          std::memcpy(&bits, &v, sizeof(float));
          xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(d), bits);
        };
        write_float(dst + 16, host_frame.vFloorClipPlane.x);
        write_float(dst + 20, host_frame.vFloorClipPlane.y);
        write_float(dst + 24, host_frame.vFloorClipPlane.z);
        write_float(dst + 28, host_frame.vFloorClipPlane.w);

        write_float(dst + 32, host_frame.vNormalToGravity.x);
        write_float(dst + 36, host_frame.vNormalToGravity.y);
        write_float(dst + 40, host_frame.vNormalToGravity.z);
        write_float(dst + 44, host_frame.vNormalToGravity.w);

        constexpr uint32_t kSkelDataSize = 436;
        constexpr uint32_t kSkelDataOffset = 48;
        for (uint32_t s = 0; s < xe::hid::kinect::kNuiSkeletonCount; ++s) {
          const auto& src = host_frame.SkeletonData[s];
          uint8_t* sd = dst + kSkelDataOffset + s * kSkelDataSize;

          xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(sd + 0), static_cast<uint32_t>(src.eTrackingState));
          xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(sd + 4), src.dwTrackingID);
          xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(sd + 8), src.dwEnrollmentIndex);
          xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(sd + 12), src.dwUserIndex);

          write_float(sd + 16, src.Position.x);
          write_float(sd + 20, src.Position.y);
          write_float(sd + 24, src.Position.z);
          write_float(sd + 28, src.Position.w);

          for (uint32_t j = 0; j < xe::hid::kinect::kNuiSkeletonPositionCount; ++j) {
            uint8_t* jp = sd + 32 + j * 16;
            write_float(jp + 0, src.SkeletonPositions[j].x);
            write_float(jp + 4, src.SkeletonPositions[j].y);
            write_float(jp + 8, src.SkeletonPositions[j].z);
            write_float(jp + 12, src.SkeletonPositions[j].w);
          }

          for (uint32_t j = 0; j < xe::hid::kinect::kNuiSkeletonPositionCount; ++j) {
            xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(sd + 352 + j * 4), static_cast<uint32_t>(src.eSkeletonPositionTrackingState[j]));
          }

          xe::store_and_swap<uint32_t>(reinterpret_cast<uint32_t*>(sd + 432), src.dwQualityFlags);
        }
        context->r[3] = 0; // S_OK
        return;
      }
    }
  }
  context->r[3] = 0; // S_OK
}

/* Notes:
   - Messages ids that start with 0x00021xxx are UI calls
   - Messages ids that start with 0x00023xxx are used for the user profile
   - Messages ids that start with 0x0002Bxxx are used by the Kinect device
   usually for camera related functions
   - Messages ids that start with 0x0002Cxxx are used by the XamNuiIdentity
   functions
*/

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

XamApp::XamApp(KernelState* kernel_state) : App(kernel_state, 0xFE) {}

X_HRESULT XamApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x0002000E: {
      X_ENUMERATE_PARAM* data_ptr =
          reinterpret_cast<X_ENUMERATE_PARAM*>(buffer);

      XELOGD(
          "XEnumerateCrossTitle({:04X}, {:04X}, {:04X}, {:04X}, {}, {}, "
          "{:04X})",
          data_ptr->user_index.get(), data_ptr->flags.get(),
          data_ptr->private_enum_structure_ptr.get(),
          data_ptr->buffer_ptr.get(), data_ptr->buffer_size.get(),
          data_ptr->items_requested.get(), data_ptr->items_returned_ptr.get());

      if (!data_ptr->buffer_ptr || !data_ptr->private_enum_structure_ptr) {
        return X_E_INVALIDARG;
      }

      auto enum_struct =
          memory_->TranslateVirtual<X_KENUMERATOR_CONTENT_AGGREGATE*>(
              data_ptr->private_enum_structure_ptr);

      auto e = kernel_state_->object_table()->LookupObject<XEnumerator>(
          enum_struct->handle);

      if (!e) {
        return X_E_INVALIDARG;
      }

      assert_true(enum_struct->magic == kXObjSignature);
      assert_true(data_ptr->buffer_size == sizeof(XCONTENT_DATA_INTERNAL));

      XCONTENT_DATA_INTERNAL* content_data_ptr =
          memory_->TranslateVirtual<XCONTENT_DATA_INTERNAL*>(
              data_ptr->buffer_ptr);

      uint32_t item_count = 0;
      std::memset(content_data_ptr, 0, data_ptr->buffer_size);

      X_RESULT result =
          e->WriteItems(reinterpret_cast<uint8_t*>(content_data_ptr),
                        data_ptr->buffer_size, &item_count);

      result = X_HRESULT_FROM_WIN32(result);

      xe::be<uint32_t>* items_returned_ptr =
          memory_->TranslateVirtual<xe::be<uint32_t>*>(
              data_ptr->items_returned_ptr);

      *items_returned_ptr = item_count;

      return result;
    }
    case 0x00020021: {
      struct XContentQueryVolumeDeviceType {
        char root_name[64];
        xe::be<uint32_t> is_title_process;
        xe::be<DeviceType> device_type_ptr;
        xe::be<uint32_t> overlapped_ptr;
      }* data = reinterpret_cast<XContentQueryVolumeDeviceType*>(buffer);
      assert_true(buffer_length == sizeof(XContentQueryVolumeDeviceType));

      std::string target;
      if (!kernel_state_->file_system()->FindSymbolicLink(
              std::string(data->root_name) + ':', target)) {
        return X_E_INVALIDARG;
      }

      // Only apply this check to XContent packages
      if (!target.starts_with("\\Device\\Package_")) {
        return X_E_INVALIDARG;
      }

      xe::be<DeviceType>* device_type_ptr =
          memory_->TranslateVirtual<xe::be<DeviceType>*>(
              static_cast<uint32_t>(data->device_type_ptr.get()));

      switch (kernel_state_->deployment_type_) {
        case XDeploymentType::kDownload:
        case XDeploymentType::kInstalledToHDD: {
          *device_type_ptr = DeviceType::HDD;
        } break;
        case XDeploymentType::kOpticalDisc: {
          *device_type_ptr = DeviceType::ODD;
        } break;
        default: {
          *device_type_ptr = DeviceType::Invalid;
        } break;
      }

      XELOGD("XContentQueryVolumeDeviceType('{}', {:08X}, {:08X}, {:08X})",
             data->root_name,
             static_cast<uint32_t>(data->is_title_process.get()),
             static_cast<uint32_t>(data->device_type_ptr.get()),
             static_cast<uint32_t>(data->overlapped_ptr.get()));

      return X_E_SUCCESS;
    }
    case 0x00021012: {
      // XamEnableGuestSignin.
      //
      // NOTE: this used to hardcode three guessed event handles (0xF80001AC /
      // 0xF80001B4 / 0xF80001E0) and force-set them, on the assumption they were
      // NUI sync events. That is unsound -- it signals guest objects nobody asked
      // us to signal -- and it ran unconditionally. Removing it lets the guest's
      // own signalling run untouched, which is what upstream does.
      uint32_t enabled = xe::load_and_swap<uint32_t>(buffer);
      XELOGD("XamApp: XamEnableGuestSignin: {}", enabled ? "true" : "false");
      return X_E_SUCCESS;
    }
    case 0x00022005: {
      struct XTITLE_GET_DEPLOYMENT_TYPE {
        xe::be<uint32_t> deployment_type_ptr;
        xe::be<uint32_t> overlapped_ptr;
      }* data = reinterpret_cast<XTITLE_GET_DEPLOYMENT_TYPE*>(buffer);
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XTITLE_GET_DEPLOYMENT_TYPE));
      auto deployment_type =
          memory_->TranslateVirtual<uint32_t*>(data->deployment_type_ptr);
      *deployment_type = static_cast<uint32_t>(kernel_state_->deployment_type_);
      XELOGD("XTitleGetDeploymentType({:08X}, {:08X}",
             data->deployment_type_ptr.get(), data->overlapped_ptr.get());
      return X_E_SUCCESS;
    }
    case 0x0002B001: {
      // NUI device initialization via XMsgInProcessCall.
      {
        // Diagnostic: the sender is the guest's XAM module, not the title, and
        // it is what decides whether skeleton tracking ever gets enabled. Log
        // the return address so we can disassemble the code that builds this
        // message and read the real contract instead of guessing at the
        // argument layout.
        auto* t = XThread::GetCurrentThread();
        uint32_t lr = 0;
        if (t && t->thread_state() && t->thread_state()->context()) {
          lr = static_cast<uint32_t>(t->thread_state()->context()->lr);
        }
        static std::atomic<uint64_t> arg_dumps{0};
        if (arg_dumps.fetch_add(1) < 2) {
          XELOGI("XamApp: 0x2B001 sender tid={:08X} lr={:08X}", t ? t->thread_id() : 0, lr);
          if (lr > 0x82000000 && lr < 0x84000000) {
            uint32_t base = lr & ~static_cast<uint32_t>(0xFFFFF);
            ::xe::kernel::DumpGuestImageForDiagnostics("guest_xam_msg.bin",
                                                      base, 0x200000u);
          }
          // XAM keeps its stream descriptors in its own address space
          // (0x7018xxxx). out[4] points at one of them and out[4] of a
          // follow-up call points at the next entry of the same list, so
          // dumping that page shows the whole table: how many streams exist,
          // which one is the skeleton stream, and what it expects back.
          ::xe::kernel::DumpGuestImageForDiagnostics("guest_xam_desc.bin",
                                                    0x70180000u, 0x10000u);
          // XAM's own code lives outside the title image (no reference to
          // 0x2B001 exists anywhere in 0x82000000-0x83FFFFFF), so dump the
          // high guest region to locate the code that builds this message and
          // read the real contract.
          // XAM's own code lives outside the title image (no reference to
          // 0x2B001 exists anywhere in 0x82000000-0x85FFFFFF), so ask the
          // module where its sections actually are and dump them. Reading the
          // contract from XAM's NUI code is the only way left to learn how
          // frames are supposed to reach the title.
          auto xam_mod = kernel_state_->GetKernelModule("xam.xex");
          XELOGI("XamModule lookup -> {}", xam_mod ? "found" : "NULL");
          if (xam_mod) {
            static const char* kSections[] = {".text", "_TEXT", ".rdata",
                                              ".data"};
            for (const char* sec : kSections) {
              uint32_t data_addr = 0;
              uint32_t data_size = 0;
              if (xam_mod->GetSection(sec, &data_addr, &data_size) &&
                  data_size > 0) {
                XELOGI("XamSection {}: {:08X} size {:08X}", sec, data_addr,
                       data_size);
                if (data_addr >= 0x80000000 && data_addr < 0x90000000 &&
                    data_size <= 0x2000000) {
                  ::xe::kernel::DumpGuestImageForDiagnostics(
                      "guest_xam_hi.bin", data_addr, data_size);
                }
              }
            }
          }
        }
      }
      if (buffer_ptr && buffer_length >= 4) {
        // How many dwords the title actually declared. NEVER read or write
        // past this: see the contract note below.
        const uint32_t declared_dwords = buffer_length / 4;
        auto* out = memory_->TranslateVirtual<uint32_t*>(buffer_ptr);
        if (out) {
          // 0x2B001 carries a SINGLE dword in the shipping title. Verified from
          // the guest caller at 0x831AEF60:
          //     stw  r3, 0x50(r1)          ; the one argument
          //     lis  r4, 2 / ori r4,0xb001 ; msg = 0x2B001
          //     li   r3, 0xFE              ; app = XAM
          //     bl   XMsgSystemProcessCall(0xFE, 0x2B001, r1+0x50, 4)
          // Kinect Adventures declares buffer_length = 4 and passes r3 = an
          // XObject handle (0x0001D4C0 on the first call, 0 on the second --
          // and the 0 case is a *deregister* taken on a failure path, not a
          // second stream registration). Neither call site reads the buffer
          // back.
          //
          // This handler used to dump and write 8 dwords unconditionally. Only
          // out[0] is the message; out[1..7] are the caller's *stack locals*.
          // That is why out[2] kept coming out as a link register (0x82A55BF8,
          // 0x82B05544) and out[1] as an address inside the live frame -- and
          // why the fabricated "NUI state object" writes and the S_OK store
          // were corrupting the title's stack and the very instruction it
          // returns to. Respect the declared length from here on.
          XELOGI("XamApp: 0x2B001 arg={:08X} (declared buffer_len={}, {} dword(s))",
                 xe::byte_swap(out[0]), buffer_length, declared_dwords);
          if (declared_dwords > 1) {
            XELOGI("XamApp: 0x2B001 extended buffer: {:08X} {:08X} {:08X} {:08X}",
                   xe::byte_swap(out[0]), xe::byte_swap(out[1]),
                   xe::byte_swap(out[2]), xe::byte_swap(out[3]));
          }

          uint32_t arg_val = xe::byte_swap(out[0]);

          if (kd() && !kd()->is_initialized() && cvars::nui_mock_device) {
            kd()->NuiInitialize(0x08);
          }

          // Allocate guest memory for mock NUI device on first call:
          if (!nui_device_addr_ && cvars::nui_mock_device) {
            nui_device_addr_ = memory_->SystemHeapAlloc(4);
            nui_vtable_addr_ = memory_->SystemHeapAlloc(256);

            if (nui_device_addr_ && nui_vtable_addr_) {
              // Register COM stubs once:
              if (!nui_stubs_registered_) {
                uint32_t qi_trampoline =
                    kernel_state_->kernel_trampoline_group()->NewLongtermTrampoline(
                        NuiCOMStub_QueryInterface);
                uint32_t success_trampoline =
                    kernel_state_->kernel_trampoline_group()->NewLongtermTrampoline(
                        NuiCOMStub_Success);
                uint32_t status_trampoline =
                    kernel_state_->kernel_trampoline_group()->NewLongtermTrampoline(
                        NuiCOMStub_NuiStatus);
                uint32_t stream_open_trampoline =
                    kernel_state_->kernel_trampoline_group()->NewLongtermTrampoline(
                        NuiCOMStub_ImageStreamOpen);
                uint32_t skeleton_frame_trampoline =
                    kernel_state_->kernel_trampoline_group()->NewLongtermTrampoline(
                        NuiCOMStub_SkeletonGetNextFrame);

                auto* vtable_data =
                    memory_->TranslateVirtual<uint32_t*>(nui_vtable_addr_);
                if (vtable_data) {
                  xe::store_and_swap<uint32_t>(&vtable_data[0], qi_trampoline);
                  for (int i = 1; i < 64; i++) {
                    xe::store_and_swap<uint32_t>(&vtable_data[i],
                                                 success_trampoline);
                  }
                  // Slot 4: NuiImageStreamOpen
                  xe::store_and_swap<uint32_t>(&vtable_data[4], stream_open_trampoline);
                  // Slot 16: NuiSkeletonGetNextFrame (standard INuiSensor interface index)
                  xe::store_and_swap<uint32_t>(&vtable_data[16], skeleton_frame_trampoline);
                  // Slot 18: NuiSkeletonGetNextFrame (alternate interface index)
                  xe::store_and_swap<uint32_t>(&vtable_data[18], skeleton_frame_trampoline);
                  // Slot 23: NuiStatus (standard INuiSensor interface index)
                  xe::store_and_swap<uint32_t>(&vtable_data[23], status_trampoline);
                  // Slot 27: NuiStatus (alternate interface index)
                  xe::store_and_swap<uint32_t>(&vtable_data[27], status_trampoline);
                }

                auto* device_data =
                    memory_->TranslateVirtual<uint32_t*>(nui_device_addr_);
                if (device_data) {
                  xe::store_and_swap<uint32_t>(device_data, nui_vtable_addr_);
                }
                nui_stubs_registered_ = true;
              }
              XELOGI("XamApp: 0x2B001 mock device allocated: device={:08X}, vtable={:08X}",
                     nui_device_addr_, nui_vtable_addr_);
            } else {
              XELOGE("XamApp: 0x2B001 SystemHeapAlloc FAILED (device={:08X}, vtable={:08X})",
                     nui_device_addr_, nui_vtable_addr_);
              nui_device_addr_ = 0;
              nui_vtable_addr_ = 0;
            }
          }

          // If the argument looks like an Event handle (e.g. 0x0001D4C0):
          if (arg_val > 0 && arg_val < 0x00020000) {
            auto object =
                kernel_state_->object_table()->LookupObject<XObject>(arg_val);
            if (!object) {
              auto ev = object_ref<XEvent>(new XEvent(kernel_state_));
              ev->Initialize(true, false);
              kernel_state_->object_table()->RestoreHandle(arg_val, ev.get());
              object = ev;
            }
            if (object) {
              if (object->type() == XObject::Type::Event) {
                auto ev = static_cast<xe::kernel::XEvent*>(object.get());
                ev->Set(0, false);
                if (kd()) {
                  kd()->SetSkeletonFrameEvent(ev);
                }
                XELOGI("XamApp: 0x2B001 signaled and registered Event handle {:08X}",
                       arg_val);
              } else if (object->type() == XObject::Type::Semaphore) {
                auto sem = static_cast<xe::kernel::XSemaphore*>(object.get());
                std::ignore = sem->ReleaseSemaphore(1, nullptr);
              }
            }
          }

          // Periodic hardware status broadcaster to notify title when sensor is ready:
          {
            static std::atomic<bool> notifying{false};
            if (kd() && !notifying.exchange(true)) {
              std::thread([this]() {
                for (int tick = 0; tick < 45; ++tick) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                  if (cvars::nui_notify_hardware_delay >= 0 &&
                      tick == cvars::nui_notify_hardware_delay) {
                    kernel_state_->BroadcastNotification(
                        kXNotificationSystemNUIHardwareStatusChanged,
                        static_cast<uint32_t>(cvars::nui_notify_hardware_state));
                    XELOGI(
                        "XamApp: broadcast NUIHardwareStatusChanged state={} "
                        "at t+{}s",
                        cvars::nui_notify_hardware_state, tick);
                  }
                }
              }).detach();
            }
          }

          // out[1]/out[2] are only meaningful when the title actually declared
          // them. The 32-byte test harness passes a real struct and relies on
          // both; Kinect Adventures declares 4 bytes and the "values" there are
          // its stack, so touching them corrupts live guest state.
          const bool extended = declared_dwords >= 3;
          uint32_t status_ptr = extended ? xe::byte_swap(out[1]) : 0;
          uint32_t hr_ptr = extended ? xe::byte_swap(out[2]) : 0;

          // A device out-slot: if out[1] is an empty pointer slot, publish the
          // mock INuiSensor there. Only reachable when the title declared the
          // slot, so this cannot fire for the 4-byte shipping contract.
          if (extended && status_ptr >= 0x10000 && status_ptr < 0xFF000000) {
            auto* vtable_slot = memory_->TranslateVirtual<uint32_t*>(status_ptr);
            if (vtable_slot) {
              uint32_t vtable_val = xe::load_and_swap<uint32_t>(vtable_slot);
              XELOGI("XamApp: 0x2B001 device slot {:08X} current={:08X}",
                     status_ptr, vtable_val);
              if (vtable_val == 0) {
                xe::memory::PageAccess rp_old;
                xe::memory::Protect(vtable_slot, 4, xe::memory::PageAccess::kReadWrite, &rp_old);
                xe::store_and_swap<uint32_t>(vtable_slot, nui_device_addr_);
                xe::memory::Protect(vtable_slot, 4, rp_old);
                XELOGI("XamApp: 0x2B001 published device {:08X} to out[1] {:08X}",
                       nui_device_addr_, status_ptr);
              }
            }
          }

          // REMOVED: the "NUI C++ state object" writes at out[1]+0x18,
          // +0x70 and +0x6CF0, plus forcing bytes[0..5], bytes[9] and the
          // +0x10 mask to 1. For Kinect Adventures out[1] is the caller's
          // stack local (r1+0x54), so this fabricated a ~28 KB state machine
          // inside the live frame and adjacent heap, gated by an address-shape
          // heuristic ("vtable looks like 0x100 or is in .text"). The title
          // never observed any of it. With out[1] now gated on a declared
          // 32-byte buffer this path is unreachable for the 4-byte contract,
          // and deleting it removes the last stack corruption on this message.
          //
          // Also gone: registering a "shared skeleton buffer" at
          // status_ptr + 0x17D00 and a "master device queue object" at the
          // hardcoded address 0x83896510 (into which the driver wrote an
          // invented +0x00/+0x58/+0xA40 layout every frame). Both were
          // derived from stack garbage, and both are the "fabricate a write
          // target from an address heuristic" mistake that froze the title on
          // 2026-10-13. Skeleton data must enter a title through a contract
          // the title itself established, not a structure we invented.

          // HRESULT out-slot, only when the title declared one. For the
          // 4-byte shipping contract hr_ptr stays 0 and we write nothing --
          // previously this stored S_OK over 0x82A55BF8, the caller's link
          // register, i.e. straight into its own code.
          if (hr_ptr >= 0x10000 && hr_ptr < 0xFF000000) {
            auto* hr_slot = memory_->TranslateVirtual<uint32_t*>(hr_ptr);
            if (hr_slot) {
              xe::memory::PageAccess hp_old;
              xe::memory::Protect(hr_slot, 4, xe::memory::PageAccess::kReadWrite, &hp_old);
              xe::store_and_swap<uint32_t>(hr_slot, 0);
              xe::memory::Protect(hr_slot, 4, hp_old);
              XELOGI("XamApp: 0x2B001 wrote S_OK to out[2] {:08X}", hr_ptr);
            }
          }

          XELOGI("XamApp: 0x2B001 done: arg={:08X} extended={} device={:08X}",
                 arg_val, extended ? 1 : 0, nui_device_addr_);
        }
      }
      return X_E_SUCCESS;
    }
    case 0x0002B002: {
      // NUI skeleton tracking enable / configure.
      // The game sends this after 0x2B001/0x2B004 to start skeleton tracking.
      auto* out = memory_->TranslateVirtual<uint32_t*>(buffer_ptr);
      if (out && buffer_length >= 4) {
        XELOGI("XamApp: 0x2B002 raw buffer: {:08X} {:08X} {:08X} {:08X}",
               xe::byte_swap(out[0]), xe::byte_swap(out[1]),
               xe::byte_swap(out[2]), xe::byte_swap(out[3]));
      }
      // Ensure KinectInputDriver is initialized for skeleton tracking.
      if (kd() && !kd()->is_initialized()) {
        kd()->NuiInitialize(0x08);
      }
      XELOGI("XamApp: 0x2B002 skeleton tracking enable → S_OK");
      return X_E_SUCCESS;
    }
    case 0x0002B003: {
      // Games used in:
      // 4D5309C9
      // It only receives buffer
      struct {
        xe::be<uint64_t> unk1;
        xe::be<uint64_t> unk2;
        xe::be<uint64_t> unk3;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
 
      XELOGI("XamUnk2B003({:016X}, {:016X}, {:016X}), unimplemented",
             args->unk1.get(), args->unk2.get(), args->unk3.get());
      return X_E_SUCCESS;
    }
    case 0x0002B004: {
      // NUI subsystem startup -- attempt device open.
      auto* driver = kd();
      if (!driver) {
        XELOGW("XamApp: 0x2B004 no KinectInputDriver");
        return X_E_FAIL;
      }
      if (!driver->is_initialized()) {
        // NUI_INITIALIZE_FLAG_USES_SKELETON
        X_RESULT result = driver->NuiInitialize(0x08);
        if (result != X_ERROR_SUCCESS) {
          XELOGE("XamApp: 0x2B004 NuiInitialize failed ({:08X})", result);
          return X_E_FAIL;
        }
      }
      XELOGI("XamApp: 0x2B004 NUI device ready");
      return X_E_SUCCESS;
    }
    case 0x0002B005: {
      return X_E_SUCCESS;
    }
    case 0x00021028: {
      return X_E_SUCCESS;
    }
    case 0x00021030: {
      return X_E_SUCCESS;
    }
    case 0x0002C001: {
      // NUI identity notification / status event.
      // Buffer contains: [code (4 bytes), param (8 bytes), ...]
      auto* raw = memory_->TranslateVirtual<uint32_t*>(buffer_ptr);
      if (raw && buffer_length >= 4) {
        uint32_t code = xe::load_and_swap<uint32_t>(&raw[0]);
        uint32_t p1 = (buffer_length >= 8) ? xe::load_and_swap<uint32_t>(&raw[1]) : 0;
        uint32_t p2 = (buffer_length >= 12) ? xe::load_and_swap<uint32_t>(&raw[2]) : 0;
        XELOGI("XamApp: 0x2C001 NUI event code={:08X} (p1={:08X}, p2={:08X}, len={})",
               code, p1, p2, buffer_length);
      } else {
        XELOGI("XamApp: 0x2C001 NUI event (buf={:08X}, len={})", buffer_ptr, buffer_length);
      }
      return X_E_SUCCESS;
    }
    case 0x0002C009: {
      // Registers (non-zero) or removes (zero) a Kinect identity callback.
      if (!buffer_ptr) {
        return X_E_INVALIDARG;
      }
      uint32_t callback = xe::load_and_swap<uint32_t>(buffer);
      // Dump the full 32-byte message so we can see the callback context
      // (real XAM invokes this with a context pointer + status).
      if (memory_) {
        auto* raw = memory_->TranslateVirtual<uint32_t*>(buffer_ptr);
        if (raw) {
          XELOGI(
              "XamApp: 0x2C009 buffer: cb={:08X} ctx={:08X} w2={:08X} w3={:08X} "
              "w4={:08X} w5={:08X} w6={:08X} w7={:08X}",
              callback, xe::load_and_swap<uint32_t>(&raw[1]),
              xe::load_and_swap<uint32_t>(&raw[2]),
              xe::load_and_swap<uint32_t>(&raw[3]),
              xe::load_and_swap<uint32_t>(&raw[4]),
              xe::load_and_swap<uint32_t>(&raw[5]),
              xe::load_and_swap<uint32_t>(&raw[6]),
              xe::load_and_swap<uint32_t>(&raw[7]));
        }
      }
      XELOGI("XamApp: 0x2C009 Kinect identity callback {:08X}",
             xe::load_and_swap<uint32_t>(buffer));
      return X_E_SUCCESS;
    }
    case 0x0002C00C: {
      // NUI device configuration (Fruit Ninja).
      // 6 parameters: 4+8+4+4+4+4 = 28 bytes.
      auto* out = memory_->TranslateVirtual<uint32_t*>(buffer_ptr);
      if (out && buffer_length >= 28) {
        XELOGI("XamApp: 0x2C00C NUI device config: {:08X} {:08X}{:08X} {:08X} {:08X} {:08X} {:08X}",
               xe::byte_swap(out[0]), xe::byte_swap(out[1]), xe::byte_swap(out[2]),
               xe::byte_swap(out[3]), xe::byte_swap(out[4]),
               xe::byte_swap(out[5]), xe::byte_swap(out[6]));
      } else {
        XELOGI("XamApp: 0x2C00C NUI device config (buf={:08X}, len={:08X})",
               buffer_ptr, buffer_length);
      }
      return X_E_SUCCESS;
    }
    case 0x0002C00D: {
      // NUI device configuration (Fruit Ninja).
      // 4 parameters: 4+8+4+4 = 20 bytes.
      auto* out = memory_->TranslateVirtual<uint32_t*>(buffer_ptr);
      if (out && buffer_length >= 20) {
        XELOGI("XamApp: 0x2C00D NUI device config: {:08X} {:08X}{:08X} {:08X} {:08X}",
               xe::byte_swap(out[0]), xe::byte_swap(out[1]), xe::byte_swap(out[2]),
               xe::byte_swap(out[3]), xe::byte_swap(out[4]));
      } else {
        XELOGI("XamApp: 0x2C00D NUI device config (buf={:08X}, len={:08X})",
               buffer_ptr, buffer_length);
      }
      return X_E_SUCCESS;
    }
    case 0x00058004: {
      // NUI session setup (Fruit Ninja).
      // 112-byte buffer with session/stream configuration.
      // Ensure KinectInputDriver is initialized.
      if (kd() && !kd()->is_initialized()) {
        kd()->NuiInitialize(0x08);
      }
      auto* out = memory_->TranslateVirtual<uint32_t*>(buffer_ptr);
      if (out && buffer_length >= 16) {
        XELOGI("XamApp: 0x58004 NUI session setup: {:08X} {:08X} {:08X} {:08X} (buf_len={:08X})",
               xe::byte_swap(out[0]), xe::byte_swap(out[1]),
               xe::byte_swap(out[2]), xe::byte_swap(out[3]),
               buffer_length);
      } else {
        XELOGI("XamApp: 0x58004 NUI session setup (buf={:08X}, len={:08X})",
               buffer_ptr, buffer_length);
      }
      return X_E_SUCCESS;
    }
    case 0x00058035: {
      // NUI frame/data request (Fruit Ninja).
      // ~40-byte buffer. Caller checks for E_INVALIDARG (0x80070057)
      // and ERROR_INSUFFICIENT_BUFFER (0x8007007A).
      // Fill buffer with skeleton frame data if possible.
      auto* out = memory_->TranslateVirtual<uint32_t*>(buffer_ptr);
      if (out) {
        XELOGI("XamApp: 0x58035 NUI frame request: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} (buf_len={:08X})",
               xe::byte_swap(out[0]), xe::byte_swap(out[1]),
               xe::byte_swap(out[2]), xe::byte_swap(out[3]),
               xe::byte_swap(out[4]), xe::byte_swap(out[5]),
               xe::byte_swap(out[6]), xe::byte_swap(out[7]),
               xe::byte_swap(out[8]), buffer_length);
      }
      // Attempt to fill with skeleton frame data if KinectInputDriver is ready.
      if (kd() && kd()->is_initialized() && buffer_length >= 40) {
        // Write session handle and success status.
        xe::store_and_swap<uint32_t>(&out[0], 0x000000fe);
        xe::store_and_swap<uint32_t>(&out[1], 0);

        xe::hid::kinect::X_NUI_SKELETON_FRAME frame{};
        if (XSUCCEEDED(kd()->NuiSkeletonGetNextFrame(0, &frame))) {
          // Field 2: timestamp lower 32-bits
          xe::store_and_swap<uint32_t>(&out[2], static_cast<uint32_t>(frame.liTimeStamp));
          // Field 3: frame number
          xe::store_and_swap<uint32_t>(&out[3], frame.dwFrameNumber);
          // Field 4: tracking state of primary user
          xe::store_and_swap<uint32_t>(&out[4], static_cast<uint32_t>(frame.SkeletonData[0].eTrackingState));
          // Field 5: tracking ID
          xe::store_and_swap<uint32_t>(&out[5], frame.SkeletonData[0].dwTrackingID);
        }
      }
      return X_E_SUCCESS;
    }
    // Causes dashboard to correctly process language/region change. It does not
    // contain any buffer.
    case 0x8000000D: {
      const bool is_pc_enabled =
          (kernel_state_->xconfig()->ReadSetting<uint8_t>(
               XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_FLAGS) &
           X_PC_FLAGS::PCEnabled) != 0;

      return is_pc_enabled ? X_E_ACCESS_DENIED : X_E_SUCCESS;
    }
    // Biometric (face/fingerprint) data read request. Kinect Adventures sends
    // this during startup. There is no biometric hardware on the host, so
    // report "success, zero bytes available" rather than failing -- the game
    // otherwise stalls. The buffer is only touched when the caller actually
    // supplied a valid one; the arguments here come from the guest unchecked.
    // Biometric (face/fingerprint) data read request. Kinect Adventures sends
    // this during startup. There is no biometric hardware on the host, so
    // report "success, zero bytes available" rather than failing -- the game
    // otherwise stalls. The buffer is only touched when the caller actually
    // supplied a valid one; the arguments here come from the guest unchecked.
    case 0x0002A004:  // Biometric session context init.
    case 0x0002A005:  // Biometric session/query open.
    case 0x0002A006:  // Biometric session/query close.
    case 0x0002A007:  // Biometric enumerate/read (24-byte records).
    case 0x0002A008: {
      static uint32_t bio_msg_count = 0;
      if (++bio_msg_count <= 16) {
        // Log with the actual message id so sequences stay readable.
        XELOGI("XamApp: biometric request msg={:06X} buf={:08X} len={}",
               message, buffer_ptr, buffer_length);
      }
      if (buffer && buffer_length && buffer_length <= 0x1000) {
        std::memset(buffer, 0, buffer_length);
      }
      return X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XAM message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
