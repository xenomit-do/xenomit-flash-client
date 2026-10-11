param([string]$OutputName = 'xenomit_flash_client.exe')
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$VsDev = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
if (!(Test-Path $VsDev)) { throw 'Visual Studio 2022 Build Tools x64 required' }
$FlashDir = Join-Path $RepoRoot 'runtime'
$Ocx = Join-Path $FlashDir 'Flash.ocx'
if (!(Test-Path $Ocx)) { throw 'Copy the expected Flash.ocx into runtime first' }
if ((Get-FileHash -LiteralPath $Ocx -Algorithm SHA256).Hash -ne 'B0475512D5AD449DFA7D21B5A2645CB1711D2CE98B42C9FE000B5FBC0F8231BD') { throw 'Flash.ocx does not match the supported runtime' }
$Miniz = Join-Path $RepoRoot 'third_party\miniz'
$Lzma = Join-Path $RepoRoot 'third_party\lzma'
$Build = Join-Path $RepoRoot 'build'
$Intermediates = Join-Path $Build 'intermediate'
New-Item -ItemType Directory -Force $Build | Out-Null
if (Test-Path -LiteralPath $Intermediates -PathType Leaf) { throw 'Build intermediate path must be a directory' }
New-Item -ItemType Directory -Force $Intermediates | Out-Null
$ExecutablePath = Join-Path $Build $OutputName
$Command = "`"$VsDev`" -arch=x64 -host_arch=x64 && cl /nologo /EHsc /std:c++17 /O2 /W4 /DUNICODE /D_UNICODE /I `"$FlashDir`" /I `"$RepoRoot\generated`" /I `"$Miniz`" /I `"$Lzma`" `"$RepoRoot\src\player.cpp`" `"$RepoRoot\src\swf_bridge.cpp`" `"$RepoRoot\src\as_bridge.cpp`" `"$RepoRoot\src\native_state.cpp`" `"$Miniz\miniz.c`" `"$Miniz\miniz_tdef.c`" `"$Miniz\miniz_tinfl.c`" `"$Lzma\LzmaDec.c`" /Fe:`"$ExecutablePath`" /link user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib wininet.lib urlmon.lib ws2_32.lib winmm.lib advapi32.lib"
Push-Location $Intermediates
try {
    cmd /d /s /c $Command
    $CompileExitCode = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($CompileExitCode -ne 0) { throw "MSVC failed: $CompileExitCode" }
$InstalledOcx = Join-Path $Build 'Flash.ocx'
if (!(Test-Path -LiteralPath $InstalledOcx) -or (Get-FileHash -LiteralPath $InstalledOcx -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $Ocx -Algorithm SHA256).Hash) {
    Copy-Item -LiteralPath $Ocx -Destination $InstalledOcx -Force
}
$ResolvedBuild = [System.IO.Path]::GetFullPath($Build).TrimEnd([char[]]@('\','/')) + [System.IO.Path]::DirectorySeparatorChar
$ResolvedIntermediates = [System.IO.Path]::GetFullPath($Intermediates)
if (!$ResolvedIntermediates.StartsWith($ResolvedBuild, [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing to remove intermediates outside the build directory' }
$IntermediateItem = Get-Item -LiteralPath $ResolvedIntermediates -Force
if ($IntermediateItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { throw 'Refusing to remove a reparse point used as the intermediate directory' }
Remove-Item -LiteralPath $ResolvedIntermediates -Recurse -Force
Write-Host "Built $ExecutablePath"