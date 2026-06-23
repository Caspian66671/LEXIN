param(
    [int]$Port = 8787
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ProxyScript = Join-Path $Root "tools\lexin_proxy.js"
$DeepSeekConfig = Join-Path $Root "deepseek_config.ps1"
$LogSuffix = if ($Port -eq 8787) { "" } else { ".$Port" }
$OutLog = Join-Path $Root "proxy$LogSuffix.out.log"
$ErrLog = Join-Path $Root "proxy$LogSuffix.err.log"

if (Test-Path $DeepSeekConfig) {
    . $DeepSeekConfig
}

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

function Test-DeepSeekConfigured {
    return -not [string]::IsNullOrWhiteSpace($env:DEEPSEEK_API_KEY)
}

function Get-LanIPv4Addresses {
    try {
        return @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
            Where-Object {
                $_.IPAddress -notlike "127.*" -and
                $_.IPAddress -notlike "169.254.*" -and
                $_.AddressState -eq "Preferred"
            } |
            Sort-Object InterfaceMetric |
            Select-Object -ExpandProperty IPAddress -Unique)
    } catch {
        return @()
    }
}

function Ensure-ProxyFirewallRule {
    $TcpRuleName = "LeXin Demo Proxy TCP $Port"
    $UdpPort = $Port + 1
    $UdpRuleName = "LeXin Demo Discovery UDP $UdpPort"
    try {
        $TcpRule = Get-NetFirewallRule -DisplayName $TcpRuleName -ErrorAction SilentlyContinue
        $UdpRule = Get-NetFirewallRule -DisplayName $UdpRuleName -ErrorAction SilentlyContinue
        if ($null -ne $TcpRule -and $TcpRule.Enabled -eq "True" -and
            $null -ne $UdpRule -and $UdpRule.Enabled -eq "True") {
            return $true
        }
    } catch {
        Write-Host "Could not inspect Windows Firewall; continuing with the existing settings."
        return $false
    }

    Write-Host "First run: requesting Windows permission for ESP32-P4 local access..."
    $Command = "if (-not (Get-NetFirewallRule -DisplayName '$TcpRuleName' -ErrorAction SilentlyContinue)) { New-NetFirewallRule -DisplayName '$TcpRuleName' -Direction Inbound -Action Allow -Protocol TCP -LocalPort $Port -Profile Any -RemoteAddress LocalSubnet | Out-Null }; if (-not (Get-NetFirewallRule -DisplayName '$UdpRuleName' -ErrorAction SilentlyContinue)) { New-NetFirewallRule -DisplayName '$UdpRuleName' -Direction Inbound -Action Allow -Protocol UDP -LocalPort $UdpPort -Profile Any -RemoteAddress LocalSubnet | Out-Null }"
    try {
        $Process = Start-Process -FilePath "powershell.exe" -Verb RunAs -Wait -PassThru `
            -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", $Command)
        return $Process.ExitCode -eq 0
    } catch {
        Write-Host "Windows Firewall permission was not granted. The proxy still starts, but the board may not reach it."
        return $false
    }
}

function Test-EnterpriseProxy {
    try {
        $health = Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/health" -TimeoutSec 2
        $insight = Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/insight" -TimeoutSec 15
        $basicOk = ($health.Content.Trim() -eq "OK PET" -and
                    $insight.Content -like "*MODEL:*" -and
                    $insight.Content -like "*INSIGHT:*")
        if (-not $basicOk) {
            return $false
        }
        if (Test-DeepSeekConfigured) {
            return ($insight.Content -like "*MODEL: DEEPSEEK*")
        }
        return $true
    } catch {
        return $false
    }
}

function Stop-LeXinProxy {
    $stopped = $false
    $procs = Get-CimInstance Win32_Process |
        Where-Object { $_.Name -like "node*" -and $_.CommandLine -like "*lexin_proxy.js*" }
    foreach ($proc in $procs) {
        Stop-Process -Id $proc.ProcessId -Force
        Write-Host "Stopped old LeXin proxy process PID $($proc.ProcessId)."
        $stopped = $true
    }
    return $stopped
}

$LanAddresses = Get-LanIPv4Addresses
if ($LanAddresses.Count -gt 0) {
    Write-Host "PC LAN address: $($LanAddresses -join ', ')"
    Write-Host "The ESP32-P4 will discover this address automatically."
} else {
    Write-Host "No active LAN IPv4 address was found. Connect this PC to the demo WiFi first."
}
$FirewallReady = Ensure-ProxyFirewallRule
if ($FirewallReady) {
    Write-Host "Windows Firewall: local ESP32-P4 access allowed."
}

$NeedsStart = $true
if (Test-Proxy) {
    if (Test-EnterpriseProxy) {
        Write-Host "LeXin DeepSeek pet proxy already running on port $Port."
        $NeedsStart = $false
    } else {
        Write-Host "Old LeXin proxy detected on port $Port. Restarting for DeepSeek pet insight..."
        if (-not (Stop-LeXinProxy)) {
            Write-Host "No LeXin node process was found to stop. Port $Port may be used by another program."
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
        "LeXin proxy startup",
        "Root: $Root",
        "Node: $NodePath",
        "Node version: $NodeVersion",
        "Script: $ProxyScript",
        "Port: $Port"
    )
    $ErrLog = Initialize-LogFile -Path $ErrLog -Kind "err" -Value @("")

    Write-Host "Starting LeXin proxy on port $Port..."
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
    if (Test-DeepSeekConfigured) {
        Write-Host "Proxy is running, but DeepSeek is not connected."
        Write-Host "Check the API key, model name, network, or proxy.err.log."
    } else {
        Write-Host "Proxy is running, but /insight is not available. Check proxy.err.log."
    }
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
Write-Host "AI pet insight:"
Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/insight" -TimeoutSec 12 |
    Select-Object -ExpandProperty Content
