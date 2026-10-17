<!-- xenomit-tooling-guidelines:managed -->

Windows x64 C++ Flash.ocx host. The core is xenomit_flash.dll with the C ABI in
include/xenomit_flash.h, loaded in-process by xenomit-game-api (the InProcessPlayer
model). xenomit_flash_client.exe is a thin out-of-process host over the same DLL
that keeps the stdin bootstrap and the named pipe. Build with just build.
Rebuild the ActionScript bridge with just bridge only after changing
bridge/DobotBridge.as; keep generated/bridge_abc.inc in source. See README.md
and docs/DEVELOPMENT.md for setup and runtime details.

- All Flash/COM access belongs to the host's STA thread. Exports and the pipe
  thread only read cached state or queue owned tasks (src/task_queue.cpp) and
  wait with a bound. Keep timeouts, cancellation/draining, the pipe's
  single-byte reply receipt and payload limits. No C++ exception may cross the
  ABI; DllMain stays empty.
- The C ABI is versioned (XF_API_VERSION). Any struct or signature change bumps
  it and updates xenomit-game-api src/bridge/flash_ffi.rs in the same change;
  keep the layout static_asserts in exports.cpp and the Rust layout tests equal.
- Frame calls (FINVOKE, direct actions, game commands) execute in the bounded
  ENTER_FRAME FIFO (at most 4 calls or 4 ms per frame) and return real AS3
  results/errors, pushed back by xenomitResult. Do not infer success from
  submission, an async ticket or a timeout, and never retry ambiguous calls.
- Direct actions (xenomit direct/caps in the bridge) call the same public game
  manager paths the controller used through reflection. No raw AVM method
  invocation, method indices or JIT/function-pointer patching.
- Detours hooks are limited to operating-system APIs (file, WinINet, Winsock)
  and are installed once before the OCX loads. Keep the virtual mms.cfg and
  settings.sol payloads our own; never copy third-party binaries.
- Injected input is sent with SendMessage on the STA to the Flash control
  window; xf_set_user_input only filters queued (real) input.
- One host per process. xf_stop allows a later xf_start; the native LiveFrame
  allocation is never freed because in-process readers keep its address.
- Entity capture runs every 150 ms, navigation every 300 ms on separate frames.
  The map catalog is static; item state refreshes once per second and only
  transmits when changed. Native window mode is the default. Map changes refresh
  navigation before entity publication. Preserve original
  capture timestamps and reject stale navigation/snapshots.
- Internal `packed_v1` entity rows carry their dictionary. The sibling
  xenomit-game-api adapter expands them. Bump the codec version for structural
  changes, validate malformed input and rebuild the API/bot along with the host.
- Use public game properties and normal manager/UI request paths. Free revival
  must use an actual visible, free, affordable, cooldown-free base repair offer.
  Missing observations fail closed. Do not invent state or paid repair choices.
- Keep runtime/.ocx and build artifacts out of Git. The supported runtime hash
  is pinned in scripts/build.ps1. Use authenticated bootstrap
  over stdin; never put sessions in repository code, diagnostics or command args.

- Default bot launch uses --hidden --hidden-bootstrap draw: OleDraw on the STA
  into an offscreen bitmap until map/hero readiness. Stop the timer and release
  its resources once ready; backend changes must not restart it. Preserve HWND
  and rendering.
- Runtime overrides use --flash / XENOMIT_FLASH_OCX_PATH (XfConfig.ocx_path
  in-process); xenomit-game-api finds the DLL through XENOMIT_FLASH_DLL.

- Native capture (`--backend native`) reads AVM offsets only on the Flash STA
  thread, scheduled by a tiny ENTER_FRAME ExternalInterface notification. Never
  read VM objects concurrently or patch JIT/function pointers. Native capture
  is gated by the runtime SHA and validated game class/method profile.
- Game offsets, method fingerprints and the legacy pattern come only from
  generated/game_offsets.h (xenomit-tooling-offset-updater). Never hand-edit
  it or add game literals to native_state.cpp; add a spec entry instead.
  Native mode requires the running gameclientHash FlashVar to equal
  kGameClientHash. Discovery prefers the bridge's NativeAnchor (own object:
  per-session nonce + ScreenManager.getInstance()); keep its three fields
  and their order. `--native-discovery parity` must stay available.
- Keep cached capture (`--backend cached`) available. Native hot state uses
  the version 2 LiveFrame (named mapping for the exe, host memory exposed by
  xf_live_frame in-process); supplementary managers/NOA and command
  dispatch retain the existing bridge. Missing/stale geometry, corrupt vectors,
  invalid pointers or unsupported profiles must fail closed. Never rejuvenate
  the last valid mapping when a decode fails.
- `REFERENCE` is an explicit slow diagnostic for live parity; production native
  polling must never issue it or STATE.
