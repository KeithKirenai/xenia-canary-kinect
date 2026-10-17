# Xenia Kinect Development Guidelines

## Config and Flag Management
- **Rule**: Xenia Canary saves/overwrites its configuration file (`xenia-canary.config.toml`) in the executable directory upon clean exit.
- **Action**: When modifying default configurations for development builds (e.g., enabling NUI initialization by default):
  1. Modify the `DEFINE_bool` or `DEFINE_string` default values directly in the source code.
  2. Proactively delete or regenerate existing `.config.toml` files in the build output folder to ensure updated defaults are written.

## Kinect (NUI) HLE Emulation Dependencies
- **Rule**: Guest NUI initialization is split between user-mode XAM stubs (`xam_nui.cc`) and kernel-mode device requests (`xboxkrnl_ldi.cc` / `xboxkrnl_io.cc`).
- **Action**: If a guest game freezes during startup or path resolution:
  - Check the log for `NullDevice::ResolvePath` or dismount/mount calls (`IoDismountVolume`, `IoDismountVolumeByFileHandle`).
  - Verify if low-level device control calls (`PsCamDeviceRequest`, `McaDeviceRequest`, `DetroitDeviceRequest`) are being queried with custom request codes.

## Visual OCR Diagnostics & Boot Lifecycle
- **Rule**: Game logs do not always expose whether the title is displaying calibration menus or awaiting a skeleton in the play space. Ground truth must be verified using lightweight per-second visual OCR sampling.
- **Boot Sequence & Timeline (Kinect Adventures)**:
  - `T+000s - T+010s`: Engine init & Microsoft Studios splash.
  - `T+011s - T+020s`: Middleware logos & asset packaging.
  - `T+021s - T+046s`: Calibration guidelines ("STAND UP TO PLAY!", "CLEAR YOUR PLAY AREA!", "FACE THE SENSOR!", "GIVE EACH OTHER ELBOW ROOM!").
  - `T+047s - T+052s`: Setup confirmation ("GOOD!").
  - `T+053s+`: Play space detection ("IS ANYONE THERE? PLEASE STAND IN THE SENSOR'S VIEW, OR CONNECT A CONTROLLER...").
- **Stylized Text OCR Requirements**:
  - Direct RGB OCR often misses stylized game fonts and colored banners.
  - Use a multi-pass pipeline:
    1. Viewport crop (exclude window titlebar and top menu bar, `crop((0, 60, w, h))`).
    2. Grayscale conversion (`img.convert('L')`).
    3. Inverted luminance (`ImageOps.invert`).
    4. Dynamic autocontrast (`ImageOps.autocontrast`).
    5. High-contrast enhancement (`ImageEnhance.Contrast(gray).enhance(2.5)`).

## Message Contracts: Respect `buffer_length` (CRITICAL)
- **Rule**: A XAM-app message only carries `buffer_length` bytes. Read and write
  **nothing** beyond it. Reading "the 8 dwords" of a 4-byte message does not
  yield 8 arguments -- it yields the caller's adjacent stack locals.
- **Observed failure**: the `0x2B001` handler dumped and wrote 8 dwords
  unconditionally. Kinect Adventures declares `buffer_length = 4`, so:
  - `out[2]` came back as `0x82A55BF8` / `0x82B05544` -- these are the callers'
    **saved link registers**, matching each call's return address exactly.
  - `out[1]` was `r1+0x54`, a live local in the calling frame.
  The handler then fabricated a ~28 KB "NUI state object" at `out[1]+0x18`,
  `+0x70` and `+0x6CF0` (gated by a `vtable == 0x100 || vtable in .text`
  address-shape heuristic) and stored `S_OK` at `out[2]` -- **over the
  instruction the CPU was about to return to**.
- **Action**: gate every out-slot on the declared length
  (`declared_dwords = buffer_length / 4`). Treat a value read from outside the
  declared length as stack garbage, never as a contract. Same family of mistake
  as fabricating a write target from an address heuristic.

## Page Access Protection for Genuine Guest Writes
- **Rule**: When you *do* have a legitimate guest pointer from a message,
  writing it may require explicit page access protection.
- **Action**: wrap with `xe::memory::Protect(slot, 4, xe::memory::PageAccess::kReadWrite, &old_prot)`.
  This is about access protection only -- it is **not** justification for
  writing to an address you inferred rather than one the title supplied.

## Log Volume Discipline (CRITICAL)
- **Rule**: The Release build ships with `log_level = 2`, so **`XELOGI` IS EMITTED** while `XELOGD` is compiled out. Logging a `kHighFrequency` kernel export at `XELOGI` floods the log and starves the emulator.
- **Observed Failure**: An unconditional `XELOGI` in `NtWaitForSingleObjectEx_entry` combined with guest thread **TID 0x1A polling handle `F8000134` with `timeout=0`** (a zero-timeout spin loop) produced **~26,800 spin lines per 2 MB** and a **318 MB log**. The game never got past the Microsoft Studios splash. No exception was involved — it was pure host-side I/O starvation.
- **Action**:
  1. Never put `XELOGI` on a per-call path of a `kHighFrequency` export (`NtWaitForSingleObjectEx`, `KeWaitForSingleObject`, `KeWaitForMultipleObjects`). Use `XELOGD`.
  2. Deduplicate repeated registration/handoff logic per `(tid, handle)` so a spin loop does not re-run object lookups and `Set()` on every iteration.
  3. **Inspecting logs**: never `Get-Content` a whole log. Use ripgrep with a `limit` (`Grep` tool), or seek to a byte offset near EOF and read only the final few MB. The log routinely reaches hundreds of MB.
  4. Before a diagnostic run, archive/remove the previous `xenia.log` so counts and sizes are unambiguous.

## Kernel Export Registration Gotcha (`!!` in the import table)
- **Rule**: Declaring `DECLARE_XBOXKRNL_EXPORT1(...)` is **not enough**. Each export group must also have
  (a) `DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(<Group>)` at the end of its `.cc`, and
  (b) an `XE_MODULE_EXPORT_GROUP(xboxkrnl, <Group>)` line in `xboxkrnl_module_export_groups.inc`.
- **Symptom**: the kernel import dump prints `!!` next to the name (meaning `is_implemented()` is false)
  and the guest floods `undefined extern call to <addr> <Name>`.
- **Action**: check both files before concluding an export is broken. `xboxkrnl_kinect.cc` was missing
  both, so `PsCamDeviceRequest` / `McaDeviceRequest` / `DetroitDeviceRequest` were never bound
  (~950k undefined calls, 56 MB log) even though the handlers were fully written.
  Also check the tag passed to the `DECLARE_*` macro: `kStub` marks an export unimplemented.

## Guest Memory Writes from XAM Stubs
- **Rule**: `kernel_memory()->TranslateVirtual(addr)` returns **nullptr** for unmapped guest addresses.
  Never feed its result straight into `memset`/`memcpy` -- with the garbage arguments guests pass to
  stubs (e.g. `XamReadBiometricData` called with `buffer_size=0x416B0000`) that is a null-pointer write
  inside `VCRUNTIME140.dll`, which surfaces as an "Unhandled Exception in Xenia".
- **Action**: null-check the translation, sanity-clamp the size, and return an error status instead of
  writing. Applies to `xam_nui.cc` and any other XAM stub taking guest pointers.

## Distinguishing a Crash from a Deadlock
- **Rule**: Xenia's crash handler pops a **modal `MessageBoxA`**, so the process stays alive and frozen
  and a naive test loop hangs forever.
- **Action**: `monitor_game_1s_ocr.py` runs a watchdog thread that polls every 0.25s for a
  `#32770` dialog titled `Unhandled Exception in Xenia`, logs the exception, closes it and kills the
  process. Classification:
  - Modal dialog + `CRITICAL: Unhandled Exception` in the log = **crash**.
  - Process alive, no dialog, log tail shows only `KeDelayExecutionThread` / `KeWaitForSingleObject`
    = **guest deadlock** (threads parked, nobody runnable).

## Deadlock Diagnosis: the Wait Probe
- **Tool**: `src/xenia/kernel/wait_probe.{h,cc}`. Records every kernel object a guest thread blocks on
  plus every object that gets signalled, then dumps a summary. Armed automatically by
  `kernel_wait_probe` (default true); dump cadence via `kernel_wait_probe_dump_ticks` (20ms ticks).
- **Reading the summary**:
  ```
  obj=F800002C waits=310 signals=0 lr=82C48DB4..82C48E60 timeout=4294667296 thread_count=1
  ```
  `waits` climbing while `signals` stays `0` is the deadlock signature: threads wait on an event
  **nobody ever signals**. Other objects in the same dump with `signals>0` are healthy.
- **Action**: cross-reference the blocking object's handle against the distinct `KeSetEvent`/`NtSetEvent`
  targets in the log (`SetEvent: obj=...`). If the blocked object never appears there, the *signaller*
  code never runs -- the real question is why that path is not reached.
- **Note**: this supersedes the earlier "no runnable threads" heuristic, which never fired because a
  thread spinning in `KeDelayExecutionThread` counts as running. Always dump unconditionally instead.
  `XThread::is_running()` is **not** a reliable "is blocked" predicate either -- it stays true while a
  thread is parked inside a host wait (28/28 threads reported running while 24 were blocked). Use the
  wait-probe data, or dump GPRs: any register in `0x82000000-0x84000000` is a guest code address and
  reveals where a thread is parked (`r30` is the frame/LR chain on PPC).

## Kinect Boot Stall (Kinect Adventures) -- RESOLVED by bisect (2026-10-13)
- **Root cause**: `xam_app.cc` message `0x2B001` registered the guest pointers `out[5]`/`out[7]`
  as "shared skeleton buffers" via a heuristic ("writable pointer >= 0x83000000") and the
  `KinectInputDriver` poll thread then wrote ~2.6 KB of `X_NUI_SKELETON_FRAME` data into them
  every 33 ms. Those addresses are **not** frame buffers -- `out[5]` was observed to be a code
  pointer (`0x8213D660`) -- so the writes were corrupting live guest memory and the title froze
  on the Microsoft splash. A variant run also exited the process cleanly at T+23s.
- **Bisect proof**: committed HEAD `97de7aacd` boots fine (FACE THE SENSOR at ~T+22s, GOOD!,
  then "IS ANYBODY THERE?"). Re-applying the WIP tree file-by-file: only `xam_app.cc` broke
  boot; within `xam_app.cc`, removing the skeleton-buffer registration (keeping the mock device,
  vtable slots 16/23 additions, ready_flag writes, `nui_mock_device` flag, and dropping the
  NUI notification broadcasts) boots fine.
- **Fix**: never fabricate write targets from address-shape heuristics. Skeleton frames are
  delivered only through the `NuiSkeletonGetNextFrame` COM trampoline.
- **Discarded diagnosis**: the earlier "producer/consumer stall on event `F800002C`" theory was a
  symptom artifact of the corrupted guest state, not an emulator scheduling bug. The heavy
  instrumentation used for it (wait probe, per-call wait logging, periodic GPR/stack dumps in
  `kernel_state.cc`/`xboxkrnl_threading.cc`) was reverted; snapshots exist in git stash `wip-kinect-deadlock`.
- **Note**: the OCR harness must match the emulator window by **process name**, not title substring;
  a title match also captures terminals/IDEs whose title contains "Xenia" (the repo path).

## Kinect Player Detection & Presence Gates (2026-10-15 analysis, CORRECTED 2026-10-16)
> **Read this before acting on the list below.** The reverse engineering here is
> accurate, but the *conclusion* drawn from it was wrong: none of these gates
> execute, because the title never reaches them. Treat this as a map of code
> that exists, not of code that is blocking.

- **Problem**: Game boots cleanly to "IS ANYBODY THERE? ..." at T+51s, then idles.
- **DISPROVEN (2026-10-16)**: the claim that `XamNuiIdentityGetSessionId` returning
  0 causes an *infinite retry loop* at `0x82D30674`. The branch target `0x82D305C0`
  is an **error path**: it calls a logger with a string at `0x82818180` and then
  branches to the epilogue at `0x82D30718`. It does not loop. (The string itself is
  encrypted in the XEX, so it cannot be read directly.)
- **DISPROVEN (2026-10-16)**: the claim that these biometric gates are the blocker.
  Runtime evidence from a fresh build (`xenia_canary.exe` newer than `xam_nui.cc`,
  so the log reflects current source):
  ```
  XamNuiGetDeviceStatus called (n=1, tid=00000006)   <- exactly once
  XamUserNuiGetUserIndex                            <- ZERO calls
  ```
  `XamUserNuiGetUserIndex_entry` logs unconditionally and the name appears in the
  log only inside the import dump, so `0x82D306B8` never executes -- and therefore
  neither does the session-id check above it at `0x82D3065C`.
- **Verified code facts** (addresses confirmed against the guest image):
  1. `0x82D3065C` `bl XamNuiIdentityGetSessionId`; `0x82D3066C` `ldx r10,r11,r10`
     loads a 64-bit expected session id from `ctx+0x249A8`; `0x82D30670`
     `cmpld cr6,r3,r10`; `0x82D30674` `bne cr6,0x82D305C0` (error path, not a loop).
  2. `0x82D306B8` `bl XamUserNuiGetUserIndex`; `0x82D306BC` `cmpwi r3,0`;
     `0x82D306C0` `bge` -> success. Same shape at `0x82D31E38`.
  3. `0x82B76178`..`0x82B761B0`: the game calls `XamUserGetSigninInfo(0,&info)` and at
     `0x82B76188` requires `signin_state == 2`; otherwise it writes `-1` to
     `0x4BC(r31)`, discarding the player every tick. `user_profile.h` now returns
     `SignInState::SignedInToLive`.
  4. `XamNuiGetDeviceStatus`: `0x82A5F8F0` checks bit `0x02` (sensor ready -> Tracking
     Mode); `0x82B04324` checks bit `0x04` (hardware error -> `XamShowNuiHardwareRequiredUI`).
     `status = 0x47` satisfies both.
- **The actual blocker**: the title imports **zero** `Nui*` skeleton functions
  (`tools/xrefs.py --all-nui` shows 0 call sites for every `INuiSensor` method), so
  skeletons cannot arrive over COM. The driver produces frames
  (`frames=2251 guest_signals=2191`) and signals the registered wake events, but
  **nothing consumes them**. The live channels are `XamXStudioRequest`
  (10 call sites, incl. the `cmd=0x1502` frame query at `0x82D35720`) and the XAM
  NUI service message `0x2B001`.
- **`0x2B001` real contract** (verified at guest caller `0x831AEF60`):
  `XMsgSystemProcessCall(0xFE, 0x2B001, r1+0x50, 4)` -- one dword naming an
  `XObject` to bind. The first call passes `0x0001D4C0`; the second passes `0` and is
  a **deregister** taken on a failure path, not a second stream registration.
  Neither call site reads the buffer back.
- **Never fabricate a guest NUI structure from an address-shape heuristic.** The
  handler used to gate on `vtable == 0x100 || vtable in .text` and then write a
  ~28 KB invented state object into the title's live stack. Removed 2026-10-16.
