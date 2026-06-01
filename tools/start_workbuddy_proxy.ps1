param(
    [int]$Port = 8787
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ProxyScript = Join-Path $Root "tools\workbuddy_proxy.js"
$LogSuffix = if ($Port -eq 8787) { "" } else { ".$Port" }
$OutLog = Join-Path $Root "proxy$LogSuffix.out.log"
$ErrLog = Join-Path $Root "proxy$LogSuffix.err.log"

function Initialize-LogFile {
    param(
        [string]$Path,
        [string[]]$Value,
        [string]$Kind
    )
    try {
        Set-Content -Encoding UTF8 -Path $Path -Value $Value
        return $Path
    } catch {
        $Fallback = Join-Path $Root "proxy.$Port.$PID.$Kind.log"
        Set-Content -Encoding UTF8 -Path $Fallback -Value $Value
        Write-Host "Log file is busy, using $Fallback"
        return $Fallback
    }
}

function Get-NodeVersionText {
    param([string]$NodePath)
    try {
        return (& $NodePath --version)
    } catch {
        return "unknown"
    }
}

function Test-Proxy {
    try {
        $health = Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/health" -TimeoutSec 2
        return ($health.Content.Trim().StartsWith("OK"))
    } catch {
        return $false
    }
}

function Test-EnterpriseProxy {
    try {
        $insight = Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/insight" -TimeoutSec 4
        return ($insight.Content -like "*MODEL:*" -and $insight.Content -like "*INSIGHT:*")
    } catch {
        return $false
    }
}

function Stop-WorkBuddyProxy {
    $stopped = $false
    $procs = Get-CimInstance Win32_Process |
        Where-Object { $_.Name -like "node*" -and $_.CommandLine -like "*workbuddy_proxy.js*" }
    foreach ($proc in $procs) {
        Stop-Process -Id $proc.ProcessId -Force
        Write-Host "Stopped old WorkBuddy proxy process PID $($proc.ProcessId)."
        $stopped = $true
    }
    return $stopped
}

$NeedsStart = $true
if (Test-Proxy) {
    if (Test-EnterpriseProxy) {
        Write-Host "WorkBuddy enterprise proxy already running on port $Port."
        $NeedsStart = $false
    } else {
        Write-Host "Old WorkBuddy proxy detected on port $Port. Restarting for enterprise insight..."
        if (-not (Stop-WorkBuddyProxy)) {
            Write-Host "No WorkBuddy node process was found to stop. Port $Port may be used by another program."
        }
        for ($i = 0; $i -lt 10 -and (Test-Proxy); $i++) {
            Start-Sleep -Milliseconds 300
        }
    }
}

if ($NeedsStart) {
    if (-not (Test-Path $ProxyScript)) {
        throw "Proxy script not found: $ProxyScript"
    }

    $NodeCommand = Get-Command "node.exe" -ErrorAction SilentlyContinue
    if ($null -eq $NodeCommand) {
        Write-Host "Node.js was not found."
        Write-Host "Install Node.js 18 or newer, then run start_demo.bat again."
        exit 1
    }

    $NodePath = $NodeCommand.Source
    $NodeVersion = Get-NodeVersionText $NodePath
    $NodeArgs = "`"$ProxyScript`" --port $Port"

    $OutLog = Initialize-LogFile -Path $OutLog -Kind "out" -Value @(
        "WorkBuddy proxy startup",
        "Root: $Root",
        "Node: $NodePath",
        "Node version: $NodeVersion",
        "Script: $ProxyScript",
        "Port: $Port"
    )
    $ErrLog = Initialize-LogFile -Path $ErrLog -Kind "err" -Value @("")

    Write-Host "Starting WorkBuddy proxy on port $Port..."
    Write-Host "Node.js: $NodeVersion"
    Start-Process -WindowStyle Hidden `
        -FilePath $NodePath `
        -ArgumentList $NodeArgs `
        -WorkingDirectory $Root `
        -RedirectStandardOutput $OutLog `
        -RedirectStandardError $ErrLog

    for ($i = 0; $i -lt 12 -and -not (Test-Proxy); $i++) {
        Start-Sleep -Milliseconds 500
    }
}

if (-not (Test-Proxy)) {
    Write-Host "Proxy did not start. Check proxy.err.log."
    Write-Host "Common causes: Node.js is missing, port $Port is occupied, or antivirus blocked node.exe."
    if (Test-Path $ErrLog) {
        Get-Content $ErrLog -Tail 20
    }
    exit 1
}

if (-not (Test-EnterpriseProxy)) {
    Write-Host "Proxy is running, but /insight is not available. Check proxy.err.log."
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
Write-Host ""
Write-Host "Enterprise insight:"
Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/insight" -TimeoutSec 12 |
    Select-Object -ExpandProperty Content
