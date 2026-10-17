---
name: kinect_development
description: Guides building, running, and debugging Kinect (NUI) features in Xenia Canary.
---

# Kinect Development Cheatsheet

## Building Xenia Canary with Kinect Support
To compile the emulator on Windows with MSVC:
```powershell
$env:VULKAN_SDK = "C:\VulkanSDK\1.4.363.0"
.\xb.ps1 build --target xenia-app --config release
```

### Faster iteration: skip the cmake re-configure
`xb.ps1 build` runs `cmake configure` on **every** invocation. For tight edit/build loops
add `--no_premake` to reuse the already-configured tree. Measured on an up-to-date tree:

| Command | No-op build |
|---|---|
| `.\xb.ps1 build --target xenia-app --config release` | ~3.9s |
| `.\xb.ps1 build --target xenia-app --config release --no_premake` | ~1.9s |

Only drop `--no_premake` when you change `CMakeLists.txt` or add/remove source files.
`CMAKE_UNITY_BUILD` is not enabled in this project; turning it on cuts compile/link overhead
further on clean builds but can surface ODR/name collisions, so test it separately.

## Offline RE toolchain (`tools/`) -- prefer this over guessing

A full Kinect Adventures run costs 60-90 s. These three scripts answer most
questions in seconds, with no emulator run at all, using the decompressed guest
image (`guest_nui_dump.bin`, written once from `XamNuiGetDeviceStatus`).

```powershell
# 1. name -> guest thunk map, parsed from Xenia's import dump (needs one run's log)
python tools\thunks.py build\bin\Windows\Release\xenia.log > thunks.txt

# 2. who calls a given kernel/XAM export?  (--all-nui, --stubs, --all also work)
python tools\xrefs.py guest_nui_dump.bin build\bin\Windows\Release\xenia.log --all-nui
python tools\xrefs.py guest_nui_dump.bin build\bin\Windows\Release\xenia.log XamUserNuiGetUserIndex

# 3. alignment-exact disassembly
python tools\xex.py guest_nui_dump.bin dis 0x82D3065C 40
```

**Do not disassemble a raw XEX with r2 directly.** Both loading it as a PE and
using `-B 0 -m <base>` silently decode the guest a few bytes off -- the real
`bl XamNuiIdentityGetSessionId` at `0x82D3065C` renders as `lwz r6, 0x48(r8)`.
`tools/xex.py` carves the window to a scratch file and maps it at the exact
vaddr, which is unambiguous. Cross-check any surprising disassembly against the
raw bytes before trusting it.

## Never delete `build/bin/Windows/Release/content/`

It is the cache of files Xenia has extracted from the disc. In disc-image mode
Xenia does **not** recreate it, and `ExtractDeviceFile` then throws
`Path Not Found`, killing the title when it asks for `update:\Database.xmplr`.
Symptom is a `<Nested Exception Encountered>` well after boot, on the main
thread. To restore:

```powershell
New-Item -ItemType Directory -Force -Path build\bin\Windows\Release\content
```

## Visual Boot Diagnostics (`monitor_game_1s_ocr.py`)
Runs Xenia against Kinect Adventures and OCRs the window every 3s for 75s.
A background watchdog thread polls every 0.25s for the modal
`Unhandled Exception in Xenia` dialog (class `#32770`), logs the exception block,
closes it and kills the process -- Xenia's crash handler uses a blocking
`MessageBoxA`, so without this the run hangs forever.
**A frozen game with a live process and no dialog is a guest deadlock, not a crash** --
check the log tail for idle threads parked in `KeDelayExecutionThread` / `KeWaitForSingleObject`.
