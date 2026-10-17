# Development and runtime architecture

This guide covers player startup, the host-to-bot boundary, capture modes, and how to rebuild the ActionScript bridge.

## Runtime components

The player has five main parts:

- `xenomit_flash.dll` is the C++ host. It owns its own single-threaded apartment (STA) thread, which runs the Win32 window, the Flash ActiveX instance and the message loop. Its C ABI is in `include/xenomit_flash.h`.
- `xenomit_flash_client.exe` is a thin out-of-process wrapper. It reads the stdin bootstrap, starts the DLL with the named LiveFrame mapping and the control pipe, and waits.
- `Flash.ocx` renders the game movie inside a system ATL ActiveX container.
- `DobotBridge.as` is injected into the movie. It exposes reflection calls, direct actions and game observations through `ExternalInterface`.
- The sibling `xenomit-game-api` crate does one of two things. It loads the DLL in-process (`FlashInProcClient`), or it speaks the pipe protocol to the exe (`FlashClient`, `FlashNativeClient`).

All Flash and COM calls run on the host's STA thread. Exported functions and the pipe worker queue owned tasks to that thread (`src/task_queue.cpp`) and wait with a bound; they never call COM directly. Frame-bound calls enter a bounded FIFO that runs up to four calls, within 4 ms, per `ENTER_FRAME`. The bridge pushes each result back with `xenomitResult`. The host returns the ActionScript result or error; it never treats submission as success.

## Starting the player

### From xenomit-bot-gui

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

For a local movie, pass its path with `--movie`. Common optional arguments are `--url`, `--width`, `--height`, `--backend cached|native`, `--native-discovery auto|anchor|pattern|parity`, `--flash`, `--hidden`, and `--hidden-bootstrap draw`. The default OCX path is `Flash.ocx` beside the executable and DLL. Also accepted: `--max-fps 0-100`, `--local-proxy <port>`, `--clear-cache <substring>`, `--game-ports <list>`. The player rejects startup when `--movie` is absent.

### Host initialization

1. Parse options and, for `--launch-stdin`, read the six bootstrap records (exe), or receive them in `XfConfig` (`xf_start`). The host then runs the remaining steps on its own STA thread.
2. Resolve the OCX path; by default it is `Flash.ocx` beside `xenomit_flash.dll`.
3. Call `OleInitialize` on the host thread to initialize its COM STA, and install the Detours hooks.
4. Load the OCX with `LoadLibraryExW`, get `DllGetClassObject`, and instantiate Shockwave Flash through its class factory. This registration-free activation does not require global OCX registration.
5. Create a Win32 top-level window and attach the COM object to the system ATL ActiveX container.
6. Apply the page base URL, FlashVars, quality, and window mode.
7. Download the movie with WinINet, or read a local SWF. Downloads are capped at 64 MiB.
8. Inject the compiled bridge and stage the patched SWF in the system temporary directory. The original movie is not overwritten. Startup stops if bridge injection fails.
9. Start playback, create the pipe, and enter the Windows message loop.

The host and OCX must both be x64. Keep them side by side when using the default OCX path.

## In-process host and InProcessPlayer parity

DarkBot's InProcessPlayer.dll hosts Flash inside the bot's JVM and is driven through JNI. The in-process mode uses the same model with a C ABI for Rust. `xf_start` creates the host's STA thread and returns once the bridged movie is playing. After that:

- Every export can be called from any thread.
- `xf_status` and `xf_live_frame` never touch the Flash thread.
- Sync calls wait on a bounded completion. With `XF_ASYNC`, a call returns a ticket that `xf_take_result` collects.

Measured locally with the bridge movie at 60 fps, from Rust (`examples/flash_inproc_smoke.rs` in xenomit-game-api):

| Operation | Pipe (old exe) | In-process |
| --- | --- | --- |
| status | 0.08 ms median, 10.6 ms p95 | 0.004 ms |
| status while another thread runs frame calls | 18.9 ms median | 0.005 ms |
| frame call (FINVOKE) | 19.1 ms median | 17.0 ms (one frame) |
| immediate call (INVOKE) | n/a | 0.7 ms |

Frame calls stay bound to the game frame in both modes. The gain is that status, LiveFrame and other calls no longer queue behind them.

| Area | InProcessPlayer | This host |
| --- | --- | --- |
| Process model | DLL in the JVM, JNI | DLL in the bot process, C ABI; exe kept as an isolated fallback |
| Dispatch | Variant tasks, ring queue, 1 s sync engine | Owned tasks, bounded queue, sync waits or async tickets |
| Frame results | Future completed by the worker | `xenomitResult` push; `resultAS` polling is gone |
| Direct actions | `moveShip`, `selectEntity`, `collectBox`, `useItem`, `refine`, `sendNotification`, `callMethod(addr, index)` | `xf_move`, `xf_select`, `xf_collect`, `xf_use_item`, `xf_refine`, `xf_notify` through the game's public managers; `xf_caps` checks signatures. No raw method-index invocation. |
| Memory | Raw read/write/replace/query exposed to the bot | Guarded `xf_read` and `xf_query_bytes` only; hot state is the LiveFrame; no writes |
| Input | Mouse via SendMessage, `keyClick`, `sendText`, `pasteText`, `setUserInput` | `xf_mouse`, `xf_key`, `xf_text`, `xf_paste`, `xf_set_user_input` |
| Window | Position, size, client size, minimum size, minimized, visible, transparency, FPS, volume, quality | Same setters; FPS is clamped to 100 |
| Flash config | Virtual `mms.cfg` and `settings.sol` through file hooks; GPU exports | Same, with our own payloads (no allow list, because the root movie is a local file); GPU exports on the exe |
| Network | Read counters, port 5001 socket, blocking, local proxy, cache purge | Same, plus ages, byte counts, `WSARecv` and `closesocket` |
| Lifecycle | `reload`, CPU, memory, `emptyWorkingSet` | `xf_reload`, status CPU and working set, `xf_trim`, restartable `xf_stop`/`xf_start` |
| Not adopted | License checks, string obfuscation, retry loop, system Flash fallback | Pinned OCX, exit codes, system ATL container |

Detours hooks are installed once, before the OCX loads, and cover OS APIs only:

- `CreateFileW`, `ReadFile`, `GetFileSize(Ex)` and `CloseHandle` serve the virtual Flash configuration.
- `InternetReadFile`, `InternetConnectW`, `HttpOpenRequestW` and `InternetOpenW` provide counters, blocking patterns and the local proxy.
- Winsock `connect`, `recv`, `WSARecv` and `closesocket` track the game socket on `game_ports` (default 5001).

STATUS and `xf_status` report `vfs_hits`, `net_reads`, `game_socket`, `game_recv`, `game_recv_bytes`, `game_recv_age_ms` and `blocked_requests`.

## Build the host

From the repository root, run:

~~~console
just build
~~~

The Justfile is the normal entry point. On Windows it runs recipes with PowerShell 7 (`pwsh`) and invokes `scripts/build.ps1`; on Linux and macOS it uses Bash and invokes the matching `.sh` scripts. The host build is Windows-only because the player uses Win32, COM/ATL, MSVC's Flash type-library import, and the 64-bit Flash ActiveX runtime. The Unix `build.sh` entry point explains this limitation. The Windows script uses the x64 MSVC compiler with C++17, optimization, Windows SDK libraries, and the bundled miniz, LZMA and Detours sources. It links `xenomit_flash.dll` (with its import library) and then the thin `xenomit_flash_client.exe` against it. It verifies the SHA-256 of `runtime/Flash.ocx` and copies the OCX and the C header into `build/`.

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

### Native offset profile

Native mode reads AVM slots at offsets that change with every game client build. They are not written by hand: `xenomit-tooling-offset-updater` resolves them from the deployed `main.swf` and writes `generated/game_offsets.h` (constants), `generated/game_offsets.json` (the profile the next run diffs against), `docs/OFFSETS_CHANGELOG.md` and `docs/offset-reports/`. Offsets that remain literal in `src/native_state.cpp` are Flash runtime internals pinned by the OCX hash.

Native capture is refused unless both hold:

- `Flash.ocx` matches the pinned runtime.
- The launch's `gameclientHash` FlashVar equals `kGameClientHash`.

A refused `--backend native` launch exits with code 2. A refused `BACKEND native` command returns an error naming both builds. Select cached capture, or regenerate the header and rebuild.

### Root discovery

The host gives the bridge a random per-session nonce through the `xenomitAnchor` callback. The bridge keeps one `NativeAnchor` object holding the nonce (`tag0`/`tag1`) and `ScreenManager.getInstance()` (`screen`). The host scans for the nonce, then requires all of the following:

- The object's class name is `NativeAnchor`.
- `screen` is a `ScreenManager`.
- The generated method fingerprints match.

The legacy alternative is the MainClientApplication value pattern from the header. `--native-discovery` selects:

| Value | Behaviour |
| --- | --- |
| `auto` (default) | anchor; pattern only after a full scan finds no anchor |
| `anchor` | anchor only |
| `pattern` | legacy pattern only |
| `parity` | both required; every capture checks they resolve the same ScreenManager and fails the frame on mismatch |

`STATUS` reports `native_root`, `parity_checks` and `parity_mismatches`. Run parity across login, map changes, death/revive and relogin before retiring the pattern.

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
| `VERSION` | Host and C ABI version. |
| `MOVE x y`, `STOP`, `SELECT id [1]`, `DESELECT`, `COLLECT id [x y]`, `USE item [1]`, `REFINE ore amount`, `NOTIFY<TAB>name<TAB>json` | Direct actions in the next frame. |
| `CAPS` | Which direct actions resolved against the running game. |
| `ASYNC <call>`, `RESULT <ticket>` | Queue a frame call and collect its result later. |
| `KEY vk [down\|up\|click]`, `MOUSE move\|down\|up\|click x y`, `TEXT <text>`, `PASTE <text>`, `INPUT 0\|1` | Input to the Flash window; `INPUT 0` drops real input. |
| `WINDOW pos\|size\|client\|minclient x y`, `WINDOW minimize\|alpha n`, `FPS n`, `VOLUME n` | Window and rendering control. |
| `BLOCK<TAB>pattern...`, `BLOCK clear`, `CACHE <substring>` | Request blocking and URL-cache purge. |
| `RELOAD`, `TRIM` | Reload the movie; trim the working set. |

Arguments are tab- or space-delimited as shown; structured call arguments use JSON. The pipe is a text front end over the same exports (`xf_exchange` runs it in-process). This is an internal bot/client contract. Update the host and sibling adapter together when changing it. `scripts/pipe_smoke.ps1` drives a running exe host.

## Output layout

- `runtime/Flash.ocx` is the local OCX input.
- `generated/bridge_abc.inc` is the required compiled ActionScript payload.
- `build/xenomit_flash.dll`, `xenomit_flash.lib`, `xenomit_flash.h`, `xenomit_flash_client.exe` and `Flash.ocx` are the local player package.
