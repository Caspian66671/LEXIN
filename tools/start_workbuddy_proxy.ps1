param(
    [int]$Port = 8787
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$ProxyScript = Join-Path $Root "tools\workbuddy_proxy.js"
$OutLog = Join-Path $Root "proxy.out.log"
$ErrLog = Join-Path $Root "proxy.err.log"

function Test-Proxy {
    try {
        $health = Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/health" -TimeoutSec 2
        return ($health.Content.Trim() -eq "OK")
    } catch {
        return $false
    }
}

if (Test-Proxy) {
    Write-Host "WorkBuddy proxy already running on port $Port."
} else {
    Write-Host "Starting WorkBuddy proxy on port $Port..."
    Start-Process -WindowStyle Hidden `
        -FilePath "node.exe" `
        -ArgumentList @($ProxyScript) `
        -WorkingDirectory $Root `
        -RedirectStandardOutput $OutLog `
        -RedirectStandardError $ErrLog

    Start-Sleep -Milliseconds 900
}

if (-not (Test-Proxy)) {
    Write-Host "Proxy did not start. Check proxy.err.log."
    if (Test-Path $ErrLog) {
        Get-Content $ErrLog -Tail 20
    }
    exit 1
}

Write-Host "Proxy OK."
Write-Host ""
Write-Host "Weather:"
Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/weather" -TimeoutSec 8 |
    Select-Object -ExpandProperty Content
Write-Host ""
Write-Host "Time:"
Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/time" -TimeoutSec 3 |
    Select-Object -ExpandProperty Content
