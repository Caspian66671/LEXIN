$ErrorActionPreference = "Continue"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $Root

function Test-CommandReady {
    param([string]$Name)
    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Write-Check {
    param(
        [string]$Name,
        [bool]$Ok,
        [string]$Hint = ""
    )
    if ($Ok) {
        Write-Host "[OK]   $Name"
    } else {
        Write-Host "[MISS] $Name"
        if ($Hint) {
            Write-Host "       $Hint"
        }
    }
}

Write-Host "WorkBuddy new PC check"
Write-Host "Project: $Root"
Write-Host ""

$nodeReady = Test-CommandReady "node"
$idfReady = Test-CommandReady "idf.py"
$gitReady = Test-CommandReady "git"

Write-Check "Git" $gitReady "Install Git if you want to clone or push this project."
Write-Check "Node.js" $nodeReady "Install Node.js 18 or newer to run the local proxy."
if ($nodeReady) {
    $nodeVersion = node --version
    Write-Host "       Node version: $nodeVersion"
    $nodeMajor = [int]($nodeVersion.TrimStart("v").Split(".")[0])
    if ($nodeMajor -lt 18) {
        Write-Host "       Warning: Node.js 18 or newer is recommended."
    }
}
Write-Check "ESP-IDF" $idfReady "Install ESP-IDF 5.x, or open an ESP-IDF VSCode terminal."

Write-Host ""
Write-Host "Required project files:"
$files = @(
    "CMakeLists.txt",
    "main\CMakeLists.txt",
    "main\idf_component.yml",
    "sdkconfig.defaults",
    "partitions.csv",
    "tools\workbuddy_proxy.js",
    "start_demo.bat",
    "build_firmware.bat"
)

foreach ($file in $files) {
    Write-Check $file (Test-Path (Join-Path $Root $file))
}

if ($nodeReady) {
    Write-Host ""
    Write-Host "Checking proxy scripts..."
    node --check tools\workbuddy_proxy.js
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[OK]   tools\workbuddy_proxy.js syntax"
    } else {
        Write-Host "[MISS] tools\workbuddy_proxy.js syntax"
    }

    node --check tools\generate_workbuddy_fonts.js
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[OK]   tools\generate_workbuddy_fonts.js syntax"
    } else {
        Write-Host "[MISS] tools\generate_workbuddy_fonts.js syntax"
    }
}

Write-Host ""
Write-Host "Next steps:"
Write-Host "1. Double-click start_demo.bat to start the local proxy."
Write-Host "2. Edit WiFi/proxy settings in sdkconfig.defaults or ESP-IDF menuconfig."
Write-Host "3. Run build_firmware.bat from an ESP-IDF terminal to build firmware."
