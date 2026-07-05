param(
    [int]$Port = 8787
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$LogSuffix = if ($Port -eq 8787) { "" } else { ".$Port" }
$OutLog = Join-Path $Root "proxy$LogSuffix.out.log"
$ErrLog = Join-Path $Root "proxy$LogSuffix.err.log"
$StartScript = Join-Path $Root "tools\start_lexin_proxy.ps1"

Write-Host "LeXin live proxy log"
Write-Host "Root: $Root"
Write-Host "Port: $Port"
Write-Host ""

$oldProcs = Get-CimInstance Win32_Process |
    Where-Object { $_.Name -like "node*" -and $_.CommandLine -like "*lexin_proxy.js*" }
foreach ($proc in $oldProcs) {
    Write-Host "Stopping old LeXin proxy process PID $($proc.ProcessId) for fresh live logs..."
    Stop-Process -Id $proc.ProcessId -Force
}
if ($oldProcs) {
    Start-Sleep -Milliseconds 800
}

& $StartScript -Port $Port
if (-not $?) {
    exit 1
}

Write-Host ""
Write-Host "Live logs are below. Press Ctrl+C to stop viewing logs."
Write-Host "The proxy process keeps running in the background unless you stop it separately."
Write-Host ""

if (-not (Test-Path $OutLog)) {
    New-Item -ItemType File -Path $OutLog -Force | Out-Null
}
if (-not (Test-Path $ErrLog)) {
    New-Item -ItemType File -Path $ErrLog -Force | Out-Null
}

$outJob = Start-Job -ScriptBlock {
    param($Path)
    Get-Content -Path $Path -Tail 80 -Wait | ForEach-Object {
        "[OUT] $_"
    }
} -ArgumentList $OutLog

$errJob = Start-Job -ScriptBlock {
    param($Path)
    Get-Content -Path $Path -Tail 30 -Wait | ForEach-Object {
        if ($_ -ne "") {
            "[ERR] $_"
        }
    }
} -ArgumentList $ErrLog

try {
    while ($true) {
        Receive-Job -Job $outJob, $errJob
        Start-Sleep -Milliseconds 200
    }
} finally {
    Stop-Job -Job $outJob, $errJob -ErrorAction SilentlyContinue
    Remove-Job -Job $outJob, $errJob -ErrorAction SilentlyContinue
}
