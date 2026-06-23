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

Write-Host "LeXin new PC check"
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
    "tools\lexin_proxy.js",
    "start_demo.bat",
    "set_deepseek_key.bat",
    "deepseek_config.example.ps1",
    "build_firmware.bat",
    "flash_firmware.bat",
    "tools\start_lexin_proxy.ps1"
)

foreach ($file in $files) {
    Write-Check $file (Test-Path (Join-Path $Root $file))
}

if ($nodeReady) {
    Write-Host ""
    Write-Host "Checking proxy scripts..."
    node --check tools\lexin_proxy.js
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[OK]   tools\lexin_proxy.js syntax"
    } else {
        Write-Host "[MISS] tools\lexin_proxy.js syntax"
    }

    node --check tools\generate_lexin_fonts.js
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[OK]   tools\generate_lexin_fonts.js syntax"
    } else {
        Write-Host "[MISS] tools\generate_lexin_fonts.js syntax"
    }
}

Write-Host ""
Write-Host "Next steps:"
Write-Host "1. Connect the PC to the demo WiFi configured in sdkconfig.defaults."
Write-Host "2. Double-click start_demo.bat and approve the one-time firewall prompt."
Write-Host "3. Run build_firmware.bat from an ESP-IDF terminal to build firmware."
Write-Host "4. Run flash_firmware.bat to auto-detect and flash the connected board."
Write-Host "The board discovers the new PC automatically; no proxy IP editing is needed."
