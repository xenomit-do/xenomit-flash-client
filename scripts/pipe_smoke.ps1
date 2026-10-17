# Drive a running xenomit_flash_client.exe over its control pipe.
#   .\scripts\pipe_smoke.ps1 -ProcessId 1234                 # status, bridge, caps
#   .\scripts\pipe_smoke.ps1 -ProcessId 1234 -Command 'FPS 30'
#   .\scripts\pipe_smoke.ps1 -ProcessId 1234 -Latency 50     # FINVOKE round trips
# Never pass session data here; this script only sends control commands.
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [string[]]$Command = @('VERSION', 'STATUS', 'BRIDGE', 'CAPS'),
    [int]$Latency = 0
)
$ErrorActionPreference = 'Stop'

function Invoke-Pipe([string]$Text) {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', "xenomit_flash_$ProcessId", [System.IO.Pipes.PipeDirection]::InOut)
    try {
        $pipe.Connect(5000)
        $pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        $pipe.Write($bytes, 0, $bytes.Length)
        $buffer = New-Object byte[] 1048576
        $reply = New-Object System.IO.MemoryStream
        do {
            $read = $pipe.Read($buffer, 0, $buffer.Length)
            $reply.Write($buffer, 0, $read)
        } while (!$pipe.IsMessageComplete)
        $pipe.WriteByte(65) # receipt
        return [System.Text.Encoding]::UTF8.GetString($reply.ToArray()).TrimEnd("`n")
    } finally {
        $pipe.Dispose()
    }
}

foreach ($item in $Command) {
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $reply = Invoke-Pipe $item
    $watch.Stop()
    '{0,-40} {1,8:N2} ms  {2}' -f $item, $watch.Elapsed.TotalMilliseconds, $reply
}

if ($Latency -gt 0) {
    $call = "FINVOKE`txenomit`tclientFeatures`t[]"
    $times = @()
    for ($i = 0; $i -lt $Latency; $i++) {
        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        $null = Invoke-Pipe $call
        $watch.Stop()
        $times += $watch.Elapsed.TotalMilliseconds
    }
    $sorted = $times | Sort-Object
    'FINVOKE round trip over {0} calls: median {1:N2} ms, p95 {2:N2} ms' -f $Latency, $sorted[[int]($sorted.Count / 2)], $sorted[[int][Math]::Floor($sorted.Count * 0.95)]
}
