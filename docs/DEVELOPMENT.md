# Development and runtime architecture

This guide covers player startup, the host-to-bot boundary, capture modes, and how to rebuild the ActionScript bridge.

## Runtime components

The player has four main parts:

- The C++ host owns the Win32 window, Flash ActiveX instance, local named pipe, and single-threaded apartment (STA) message loop.
- `Flash.ocx` renders the game movie inside a system ATL ActiveX container.
- `DobotBridge.as` is injected into the movie. It exposes reflection calls and game observations through `ExternalInterface`.
- The sibling `xenomit-game-api` adapter speaks the pipe protocol and translates replies into `GameClient` snapshots and commands.

All Flash and COM calls run on the player UI thread. The pipe worker performs bounded I/O and posts requests to that thread; it never calls COM directly. Frame-bound calls enter a bounded FIFO and execute one at a time on `ENTER_FRAME`. The host returns the ActionScript result or error rather than treating submission as success.

## Starting the player

### From xenomit-bot

The normal startup path is:

1. The bot authenticates and obtains the preloader URL, page instance URL, SID, FlashVars, and stage size.
2. It starts `xenomit_flash_client.exe` with `--launch-stdin` and the selected capture backend.
3. It writes six newline-delimited UTF-8 records to stdin: movie URL, page base URL, SID, FlashVars, width, and height. Session values stay out of the process command line.
4. Unless `XENOMIT_FLASH_KEEP_VISIBLE=1`, the bot passes `--hidden --hidden-bootstrap draw`. The host draws the ActiveX control to an offscreen bitmap until the hero and map are ready, then stops the timer.
5. The bot connects to `\\.\pipe\xenomit_flash_<pid>`.

The bot resolves the executable from `XENOMIT_FLASH_CLIENT_EXE` when set. Otherwise it checks beside the bot executable and then the sibling `xenomit-flash-client/build` directory. `XENOMIT_FLASH_OCX_PATH` is passed as the player's `--flash` override.

The bot adds `autoStartEnabled=1` and `display2d=2` to FlashVars. If either supplied stage dimension is at most 100, it uses 1280 by 800.

### Standalone launch

For a real authenticated session, use the same `--launch-stdin` contract from a trusted parent process. The player reads all six records before initializing the OCX. Keep the SID and FlashVars out of command-line arguments, checked-in scripts, and logs.

For a local movie, pass its path with `--movie`. Common optional arguments are `--url`, `--width`, `--height`, `--backend cached|native`, `--flash`, `--hidden`, and `--hidden-bootstrap draw`. The default OCX path is `Flash.ocx` beside the executable. The player rejects startup when `--movie` is absent.

### Host initialization

1. Parse options and, for `--launch-stdin`, read the six bootstrap records.
2. Resolve the OCX path; by default it is `Flash.ocx` in the executable directory.
3. Call `OleInitialize` on the main thread to initialize its COM STA.
4. Load the OCX with `LoadLibraryExW`, get `DllGetClassObject`, and instantiate Shockwave Flash through its class factory. This registration-free activation does not require global OCX registration.
5. Create a Win32 top-level window and attach the COM object to the system ATL ActiveX container.
6. Apply the page base URL, FlashVars, quality, and window mode.
7. Download the movie with WinINet, or read a local SWF. Downloads are capped at 64 MiB.
8. Inject the compiled bridge and stage the patched SWF in the system temporary directory. The original movie is not overwritten. Startup stops if bridge injection fails.
9. Start playback, create the pipe, and enter the Windows message loop.

The host and OCX must both be x64. Keep them side by side when using the default OCX path.

## Build the host

From the repository root, run:

~~~console
just build
~~~

The Justfile is the normal entry point. On Windows it runs recipes with PowerShell 7 (`pwsh`) and invokes `scripts/build.ps1`; on Linux and macOS it uses Bash and invokes the matching `.sh` scripts. The host build is Windows-only because the player uses Win32, COM/ATL, MSVC's Flash type-library import, and the 64-bit Flash ActiveX runtime. The Unix `build.sh` entry point explains this limitation. The Windows script uses the x64 MSVC compiler with C++17, optimization, Windows SDK libraries, and the bundled miniz/LZMA sources. It verifies the SHA-256 of `runtime/Flash.ocx` and copies the OCX beside the executable in `build/`.

A local package consists of `build/xenomit_flash_client.exe` and its matching `build/Flash.ocx`. The runtime binary and build products are not checked into Git. Keep `generated/bridge_abc.inc` and the `third_party/` compression sources because they are host build inputs.

## Modify and rebuild the bridge

The bridge source is `bridge/DobotBridge.as`. It is not the movie's document class: the C++ injector inserts its compiled DoABC payload and a small sprite on the first frame.

The bridge registers callbacks for health checks, reflection invocation, inspection, cached state reads, backend selection, and capture. It resolves game classes as the preloader loads them and schedules Flash-thread work through `ENTER_FRAME`. The host invokes callbacks with `ShockwaveFlash.CallFunction` using an XML request/reply envelope.

Normal host builds consume `generated/bridge_abc.inc`; they do not need Java, Flex, or Python. To change the ActionScript bridge:

1. Edit `bridge/DobotBridge.as`.
2. Install a Flex SDK containing `lib/mxmlc.jar` and `frameworks/libs/player/32.0/playerglobal.swc`, plus Java and Python 3.
3. Recompile and extract the ABC payload:

   ~~~console
   # Windows
   just bridge-flex 'C:\path\to\flex'

   # Linux or macOS
   just bridge-flex '/path/to/flex'
   ~~~

4. Review `generated/bridge_abc.inc`, then rebuild the host:

   ~~~console
   just build
   ~~~

The `bridge` recipe uses the default Flex SDK path; `bridge-flex` accepts an explicit SDK path. The PowerShell and Bash implementations compile `DobotBridge.as` to `generated/DobotBridge.swf`; `bridge/extract_abc.py` extracts its DoABC payload into `generated/bridge_abc.inc`. On Unix, the default SDK path is `${TMPDIR:-/tmp}/dobot-flex/flex`; override it with `FLEX_SDK_PATH` or pass it to `just bridge-flex`. The SWF is an intermediate ignored by Git; the include is required by the host build. The injector accepts FWS, CWS, and ZWS movies and inserts the bridge before the first displayed frame.

If a callback name, argument, return value, or capture schema changes, update the C++ caller in `src/player.cpp` or `src/as_bridge.cpp` and the sibling `xenomit-game-api` adapter in the same change.

## Capture modes

The bot passes `--backend cached` or `--backend native`.

- Cached mode asks the ActionScript bridge for snapshots and avoids version-specific AVM offsets.
- Native mode schedules version-specific AVM reads from `ENTER_FRAME` and publishes the version 2 shared-memory frame. It requires the supported runtime fingerprint and game class/method profile; unsupported profiles fail closed.
- Navigation, portals, catalog, and inventory continue through supplementary ActionScript observations in both modes.

In cached mode, entity capture normally runs every 150 ms, navigation refreshes about every 300 ms, and inventory refreshes every second. Native capture runs on the Flash thread; the pipe worker reads only the host's published state. Changing the backend resets stale capture state without restarting the game process.

## Local controller pipe

The host creates one local message-mode pipe per process: `\\.\pipe\xenomit_flash_<pid>`. It bounds message sizes and I/O waits, rejects remote clients, and keeps one pipe instance alive across connections.

| Request | Purpose |
| --- | --- |
| `STATUS` | Readiness and capture health. |
| `BRIDGE` | Confirm the ActionScript bridge responds. |
| `STATE` | Read a cached state stage. |
| `INSPECT` and `INVOKE` | Inspect or call a game object through ActionScript reflection. |
| `FINVOKE` | Queue a call for `ENTER_FRAME` and return its result. |
| `REFERENCE` | Request a full ActionScript reference snapshot for controller-side parity diagnostics. |
| `BACKEND` | Select cached or native capture. |
| `CAPTURE`, `WINDOW`, `QUALITY` | Change capture, visibility, or render quality. |

Arguments are tab-delimited; structured call arguments use JSON. This is an internal bot/client contract. Update the host and sibling adapter together when changing it.

## Output layout

- `runtime/Flash.ocx` is the local OCX input.
- `generated/bridge_abc.inc` is the required compiled ActionScript payload.
- `build/xenomit_flash_client.exe` and `build/Flash.ocx` are the local player package.
