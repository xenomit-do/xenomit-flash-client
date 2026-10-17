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
$Detours = Join-Path $RepoRoot 'third_party\detours'
$Include = Join-Path $RepoRoot 'include'
$Src = Join-Path $RepoRoot 'src'
$Build = Join-Path $RepoRoot 'build'
$Intermediates = Join-Path $Build 'intermediate'
New-Item -ItemType Directory -Force $Build | Out-Null
if (Test-Path -LiteralPath $Intermediates -PathType Leaf) { throw 'Build intermediate path must be a directory' }
New-Item -ItemType Directory -Force $Intermediates | Out-Null
$ExecutablePath = Join-Path $Build $OutputName
$DllPath = Join-Path $Build 'xenomit_flash.dll'
$ImportLib = Join-Path $Build 'xenomit_flash.lib'

# The host core is xenomit_flash.dll (C ABI in include\xenomit_flash.h). The
# executable is a thin out-of-process host over the same DLL.
$HostSources = @('host.cpp', 'task_queue.cpp', 'exports.cpp', 'pipe_server.cpp', 'input.cpp', 'hooks.cpp',
    'swf_bridge.cpp', 'as_bridge.cpp', 'native_state.cpp') | ForEach-Object { "`"$Src\$_`"" }
$ThirdParty = @("`"$Miniz\miniz.c`"", "`"$Miniz\miniz_tdef.c`"", "`"$Miniz\miniz_tinfl.c`"", "`"$Lzma\LzmaDec.c`"",
    "`"$Detours\detours.cpp`"", "`"$Detours\modules.cpp`"", "`"$Detours\disasm.cpp`"")
$Libs = 'user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib wininet.lib urlmon.lib ws2_32.lib winmm.lib advapi32.lib psapi.lib'
$Common = "/nologo /EHsc /std:c++17 /O2 /DUNICODE /D_UNICODE /I `"$Include`""
$Dll = "cl $Common /W4 /LD /DXENOMIT_FLASH_BUILD /I `"$FlashDir`" /I `"$RepoRoot\generated`" /I `"$Miniz`" /I `"$Lzma`" /I `"$Detours`" $($HostSources -join ' ') $($ThirdParty -join ' ') /Fe:`"$DllPath`" /link /IMPLIB:`"$ImportLib`" $Libs"
$Exe = "cl $Common /W4 `"$Src\exe_main.cpp`" /Fe:`"$ExecutablePath`" /link `"$ImportLib`" shell32.lib"
$Command = "`"$VsDev`" -arch=x64 -host_arch=x64 && $Dll && $Exe"
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
Copy-Item -LiteralPath (Join-Path $Include 'xenomit_flash.h') -Destination (Join-Path $Build 'xenomit_flash.h') -Force
$ResolvedBuild = [System.IO.Path]::GetFullPath($Build).TrimEnd([char[]]@('\','/')) + [System.IO.Path]::DirectorySeparatorChar
$ResolvedIntermediates = [System.IO.Path]::GetFullPath($Intermediates)
if (!$ResolvedIntermediates.StartsWith($ResolvedBuild, [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing to remove intermediates outside the build directory' }
$IntermediateItem = Get-Item -LiteralPath $ResolvedIntermediates -Force
if ($IntermediateItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { throw 'Refusing to remove a reparse point used as the intermediate directory' }
Remove-Item -LiteralPath $ResolvedIntermediates -Recurse -Force
Write-Host "Built $DllPath"
Write-Host "Built $ExecutablePath"
