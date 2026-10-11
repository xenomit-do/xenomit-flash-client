#!/usr/bin/env bash
set -euo pipefail

cat >&2 <<'EOF'
The Flash OCX host can only be built on Windows. It depends on Win32, COM/ATL,
the MSVC `#import` support for Flash.ocx, and the 64-bit Flash ActiveX runtime.

Use `just build` from Windows with Visual Studio 2022 Build Tools installed.
On Unix, `just bridge` can compile the ActionScript bridge when Java, Python 3,
and a Flex SDK are available.
EOF
exit 2
