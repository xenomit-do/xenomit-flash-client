[windows]
set shell := ["pwsh.exe", "-NoLogo", "-NoProfile", "-Command"]

[unix]
set shell := ["bash", "-uc"]

default: build

[windows]
build output_name="xenomit_flash_client.exe":
    & ".\scripts\build.ps1" -OutputName "{{output_name}}"

[unix]
build output_name="xenomit_flash_client.exe":
    bash ./scripts/build.sh "{{output_name}}"

[windows]
bridge:
    & ".\scripts\build_bridge.ps1"

[unix]
bridge:
    bash ./scripts/build_bridge.sh

[windows]
bridge-flex sdk_path:
    & ".\scripts\build_bridge.ps1" -FlexSdkPath "{{sdk_path}}"

[unix]
bridge-flex sdk_path:
    bash ./scripts/build_bridge.sh "{{sdk_path}}"
