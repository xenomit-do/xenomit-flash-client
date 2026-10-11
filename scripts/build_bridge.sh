#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
sdk_path="${1:-${FLEX_SDK_PATH:-${TMPDIR:-/tmp}/dobot-flex/flex}}"
flex_jar="$sdk_path/lib/mxmlc.jar"
playerglobal="$sdk_path/frameworks/libs/player/32.0/playerglobal.swc"
source="$repo_root/bridge/DobotBridge.as"
swf="$repo_root/generated/DobotBridge.swf"
include="$repo_root/generated/bridge_abc.inc"

if [[ ! -f "$flex_jar" ]]; then
  printf 'Flex SDK compiler not found: %s\n' "$flex_jar" >&2
  printf 'Pass its root directory with `just bridge-flex <sdk-path>` or set FLEX_SDK_PATH.\n' >&2
  exit 1
fi

if [[ ! -f "$playerglobal" ]]; then
  printf 'Flash Player 32 playerglobal not found: %s\n' "$playerglobal" >&2
  exit 1
fi

if ! command -v java >/dev/null 2>&1; then
  printf 'Java is required to compile the ActionScript bridge.\n' >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  printf 'Python 3 is required to extract the compiled bridge payload.\n' >&2
  exit 1
fi

mkdir -p "$repo_root/generated"
java -jar "$flex_jar" \
  "+flexlib=$sdk_path/frameworks" \
  -compiler.fonts.local-fonts-snapshot= \
  -compiler.theme= \
  -load-config= \
  "-compiler.library-path=$playerglobal" \
  -target-player=32.0 \
  -warnings=false \
  -output "$swf" \
  "$source"

python3 "$repo_root/bridge/extract_abc.py" "$swf" "$include"
printf 'Built bridge payload: %s\n' "$include"
