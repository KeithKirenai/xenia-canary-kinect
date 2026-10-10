/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xam_app.h"

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/hid/kinect/kinect_input_driver.h"
#include "xenia/base/memory.h"
#include "xenia/kernel/xthread.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xsemaphore.h"

static xe::hid::kinect::KinectInputDriver* kd() {
  return xe::hid::kinect::KinectInputDriver::instance();
}

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
  uint32_t frame_ptr = static_cast<uint32_t>(context->r[5]);
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
      uint32_t enabled = xe::load_and_swap<uint32_t>(buffer);
      XELOGD("XEnableGuestSignin: {}", enabled ? "true" : "false");
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
      if (buffer_ptr) {
        auto* out = memory_->TranslateVirtual<uint32_t*>(buffer_ptr);
        if (out) {
          XELOGI("XamApp: 0x2B001 raw buffer content: {:08X} {:08X} {:08X} {:08X} "
                 "{:08X} {:08X} {:08X} {:08X}",
                 xe::byte_swap(out[0]), xe::byte_swap(out[1]),
                 xe::byte_swap(out[2]), xe::byte_swap(out[3]),
                 xe::byte_swap(out[4]), xe::byte_swap(out[5]),
                 xe::byte_swap(out[6]), xe::byte_swap(out[7]));

          uint32_t original_handle = xe::byte_swap(out[0]);
          uint32_t status_ptr = xe::byte_swap(out[1]);
          uint32_t hr_ptr = xe::byte_swap(out[2]);

          if (kd() && !kd()->is_initialized()) {
            kd()->NuiInitialize(0x08);
          }

          // Allocate guest memory for mock NUI device on first call:
          if (!nui_device_addr_) {
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
                  // INuiSensor method mappings:
                  // Slot 3: NuiSetFrameEndEvent
                  // Slot 4: NuiImageStreamOpen
                  xe::store_and_swap<uint32_t>(&vtable_data[4], stream_open_trampoline);
                  // Slot 16: NuiSkeletonGetNextFrame (INuiSensor official index 16)
                  xe::store_and_swap<uint32_t>(&vtable_data[16], skeleton_frame_trampoline);
                  // Slot 18: alternate index for NuiSkeletonGetNextFrame
                  xe::store_and_swap<uint32_t>(&vtable_data[18], skeleton_frame_trampoline);
                  // Slot 23: NuiStatus (INuiSensor index 23)
                  xe::store_and_swap<uint32_t>(&vtable_data[23], status_trampoline);
                  // Slot 27: alternate NuiStatus
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

          // Signal and register the notification event:
          if (original_handle && original_handle < 0x7FFFFFFF) {
            auto object =
                kernel_state_->object_table()->LookupObject<XObject>(original_handle);
            if (!object) {
              auto ev = object_ref<XEvent>(new XEvent(kernel_state_));
              ev->Initialize(true, false);
              kernel_state_->object_table()->RestoreHandle(original_handle, ev.get());
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
                       original_handle);
              } else if (object->type() == XObject::Type::Semaphore) {
                auto sem = static_cast<xe::kernel::XSemaphore*>(object.get());
                std::ignore = sem->ReleaseSemaphore(1, nullptr);
              }
            }
          }

          // If status_ptr points to a guest NUI C++ state object, unblock its ready flag:
          if (status_ptr >= 0x10000 && status_ptr < 0xFF000000) {
            auto* vtable_slot = memory_->TranslateVirtual<uint32_t*>(status_ptr);
            if (vtable_slot) {
              uint32_t vtable_val = xe::load_and_swap<uint32_t>(vtable_slot);
              XELOGI("XamApp: 0x2B001 NUI object +0x00 vtable={:08X}", vtable_val);
              if (vtable_val == 0) {
                xe::memory::PageAccess rp_old;
                xe::memory::Protect(vtable_slot, 4, xe::memory::PageAccess::kReadWrite, &rp_old);
                xe::store_and_swap<uint32_t>(vtable_slot, nui_device_addr_);
                xe::memory::Protect(vtable_slot, 4, rp_old);
                XELOGI("XamApp: 0x2B001 wrote device {:08X} directly to status_ptr {:08X}",
                       nui_device_addr_, status_ptr);
              }
            }

            // Dump memory of status_ptr:
            auto* raw_mem = memory_->TranslateVirtual<uint32_t*>(status_ptr);
            if (raw_mem) {
              XELOGI("XamApp: 0x2B001 dump of status_ptr {:08X}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                     status_ptr,
                     xe::byte_swap(raw_mem[0]), xe::byte_swap(raw_mem[1]),
                     xe::byte_swap(raw_mem[2]), xe::byte_swap(raw_mem[3]),
                     xe::byte_swap(raw_mem[4]), xe::byte_swap(raw_mem[5]),
                     xe::byte_swap(raw_mem[6]), xe::byte_swap(raw_mem[7]));
            }

            // Write mock device into any null slot within the first 0x80 bytes of status_ptr:
            for (uint32_t off = 0; off <= 0x80; off += 4) {
              auto* slot = memory_->TranslateVirtual<uint32_t*>(status_ptr + off);
              if (slot) {
                uint32_t current_val = xe::load_and_swap<uint32_t>(slot);
                if (current_val == 0) {
                  xe::memory::PageAccess rp_old;
                  xe::memory::Protect(slot, 4, xe::memory::PageAccess::kReadWrite, &rp_old);
                  xe::store_and_swap<uint32_t>(slot, nui_device_addr_);
                  xe::memory::Protect(slot, 4, rp_old);
                  XELOGI("XamApp: 0x2B001 wrote device {:08X} to status_ptr+0x{:02X}",
                         nui_device_addr_, off);
                }
              }
            }
          }

          // Only write mock device to out[1] if out[1] was initially null (i.e. expected as output):
          if (status_ptr == 0) {
            xe::store_and_swap<uint32_t>(&out[1], nui_device_addr_);
          }

          // If hr_ptr is provided, write 0 (S_OK) so the caller's HRESULT check passes:
          if (hr_ptr >= 0x10000 && hr_ptr < 0xFF000000) {
            auto* hr_slot = memory_->TranslateVirtual<uint32_t*>(hr_ptr);
            if (hr_slot) {
              xe::memory::PageAccess hp_old;
              xe::memory::Protect(hr_slot, 4, xe::memory::PageAccess::kReadWrite, &hp_old);
              xe::store_and_swap<uint32_t>(hr_slot, 0);
              xe::memory::Protect(hr_slot, 4, hp_old);
            }
          }

          XELOGI("XamApp: 0x2B001 done: event={:08X} nui_obj={:08X} device={:08X} hr_ptr={:08X}",
                 original_handle, status_ptr, nui_device_addr_, hr_ptr);
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
    case 0x0002C009: {
      // Registers (non-zero) or removes (zero) a Kinect identity callback.
      if (!buffer_ptr) {
        return X_E_INVALIDARG;
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
