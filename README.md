# Xenomit Flash Client

A Windows x64 C++17 host for the Flash ActiveX control. The host loads the game movie, injects the controller bridge, and exposes a local named-pipe interface to `xenomit-bot`.

## Requirements

- Just installed and available on PATH. The Justfile selects PowerShell 7 on Windows and Bash on Unix.
- Windows x64 and Visual Studio 2022 Build Tools with the x64 C++ toolchain and Windows SDK to build the player host. The build script expects the standard Build Tools installation path.
- Place the supported 64-bit runtime at `runtime/Flash.ocx` before building. This local binary is excluded from Git. The build verifies its SHA-256 and copies it beside the executable.
- Java, Python 3, and a Flex SDK with Player 32 `playerglobal.swc` to rebuild the ActionScript bridge. Bridge builds are supported on Windows, Linux, and macOS.

## Build

Run from the repository root:

~~~console
just build
~~~

The optimized executable is written to `build/xenomit_flash_client.exe`; the matching `Flash.ocx` is copied to `build/Flash.ocx`. Keep these files together when running or packaging the player. The build compiles the C++ host plus the bundled miniz and LZMA sources. Normal host builds do not need Java, Flex, or Python.

The player host itself is Windows-only: it uses Win32, COM/ATL, MSVC's Flash type-library import, and the 64-bit Flash ActiveX runtime. On Unix, `just build` reports this platform limitation; bridge compilation remains available with `just bridge` or `just bridge-flex <sdk-path>`.


## Run from xenomit-bot

1. Build the player.
2. Start `xenomit-bot` and select **Flash (OCX)** on the sign-in page.
3. Sign in normally. The bot starts the player, sends the authenticated game bootstrap over stdin, and connects to the player's named pipe.

The bot searches for `xenomit_flash_client.exe` beside its own executable, then in the sibling `xenomit-flash-client/build` directory. Set `XENOMIT_FLASH_CLIENT_EXE` to select another player executable. Set `XENOMIT_FLASH_OCX_PATH` to override the OCX; otherwise the player loads `Flash.ocx` from its own directory.

The bot starts Flash hidden and uses an offscreen draw loop until the game is ready. Set `XENOMIT_FLASH_KEEP_VISIBLE=1` to start it visibly. The bot's Show/Hide client control remains available.

## Player startup contract

For an authenticated session, a trusted parent process starts the player with `--launch-stdin`. The player reads six UTF-8 newline-delimited records before initializing Flash:

1. Movie or preloader URL.
2. Page base URL.
3. Session ID.
4. FlashVars as ampersand-joined `key=value` pairs.
5. Stage width.
6. Stage height.

The bot supplies these records automatically. Do not place session IDs or FlashVars in command-line arguments, checked-in scripts, or logs. A standalone launcher should use the same stdin contract.

Example process arguments, without session data:

~~~powershell
.\build\xenomit_flash_client.exe --launch-stdin --backend native
~~~

For a local SWF, pass its path with `--movie <path>`. The window is visible by default; `--hidden` starts it hidden. The optional `--flash <path>` argument overrides the default OCX beside the executable.

## Source map

- `src/` contains the Win32/COM host, SWF injector, bridge transport, and native state reader.
- `bridge/DobotBridge.as` is the ActionScript bridge injected into each movie.
- `generated/bridge_abc.inc` is the compiled bridge payload used by normal C++ builds.
- `third_party/` contains compression sources required to read and rewrite SWFs.
- `scripts/` contains the host and bridge build scripts.
- `runtime/` and `build/` are local runtime input and build output directories ignored by Git.

Run `just bridge` after changing the ActionScript source. See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for the startup sequence, bridge internals, capture modes, pipe protocol, and bridge rebuild steps.
