# Xenomit Flash Client

Windows x64 C++ host for runtime/Flash.ocx. Build with just build. Rebuild the ActionScript bridge with just bridge only after changing bridge/DobotBridge.as; keep generated/bridge_abc.inc in source. See README.md and docs/DEVELOPMENT.md for setup and runtime details.

- All Flash/COM access belongs to the STA window thread. The pipe thread reads
  only cached state and posts bounded, heap-owned requests. Keep timeouts,
  cancellation/draining, the single-byte reply receipt and payload limits.
- FINVOKE executes through the bounded ENTER_FRAME FIFO and returns real AS3
  results/errors. Do not infer success from submission or retry ambiguous calls.
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
- Runtime overrides use --flash / XENOMIT_FLASH_OCX_PATH.

- Native capture (`--backend native`) reads AVM offsets only on the Flash STA
  thread, scheduled by a tiny ENTER_FRAME ExternalInterface notification. Never
  read VM objects concurrently or patch JIT/function pointers. Native capture
  is gated by the runtime SHA and validated game class/method profile.
- Keep cached capture (`--backend cached`) available. Native hot state uses
  version 2 LiveFrame shared memory; supplementary managers/NOA and command
  dispatch retain the existing bridge. Missing/stale geometry, corrupt vectors,
  invalid pointers or unsupported profiles must fail closed. Never rejuvenate
  the last valid mapping when a decode fails.
- `REFERENCE` is an explicit slow diagnostic for live parity; production native
  polling must never issue it or STATE.
