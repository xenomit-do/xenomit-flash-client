param([string]$FlexSdkPath = (Join-Path $env:TEMP 'dobot-flex\flex'))

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Sdk = $FlexSdkPath
$Player = Join-Path $Sdk 'frameworks\libs\player\32.0\playerglobal.swc'
$Source = Join-Path $RepoRoot 'bridge\DobotBridge.as'
$Swf = Join-Path $RepoRoot 'generated\DobotBridge.swf'
$Include = Join-Path $RepoRoot 'generated\bridge_abc.inc'
if (!(Test-Path (Join-Path $Sdk 'lib\mxmlc.jar'))) { throw "Flex SDK not found at $Sdk" }
if (!(Test-Path $Player)) { throw "playerglobal32_0.swc not found at $Player" }

$FlexLib = Join-Path $Sdk 'frameworks'
cmd /d /s /c "java -jar `"$Sdk\lib\mxmlc.jar`" +flexlib=`"$FlexLib`" -compiler.fonts.local-fonts-snapshot= -compiler.theme= -load-config= -compiler.library-path=`"$Player`" -target-player=32.0 -warnings=false -output `"$Swf`" `"$Source`""
if ($LASTEXITCODE -ne 0) { throw "mxmlc failed with exit code $LASTEXITCODE" }

& python (Join-Path $RepoRoot 'bridge\extract_abc.py') $Swf $Include
if ($LASTEXITCODE -ne 0) { throw "ABC extraction failed" }



