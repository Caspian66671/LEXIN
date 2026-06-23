$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ConfigPath = Join-Path $Root "deepseek_config.ps1"

$secureKey = Read-Host "Paste DeepSeek API Key" -AsSecureString
$keyPtr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secureKey)
$key = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($keyPtr)
[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($keyPtr)
if ([string]::IsNullOrWhiteSpace($key)) {
    Write-Host "No key entered."
    exit 1
}

$model = "deepseek-chat"

Set-Content -Encoding UTF8 -Path $ConfigPath -Value @(
    "# Local DeepSeek config. This file is ignored by Git.",
    "`$env:DEEPSEEK_API_KEY = `"$key`"",
    "`$env:DEEPSEEK_MODEL = `"$model`""
)

Write-Host "Saved local DeepSeek config:"
Write-Host $ConfigPath
Write-Host "Model: $model"
Write-Host ""
Write-Host "Starting LeXin proxy with DeepSeek..."
& (Join-Path $PSScriptRoot "start_lexin_proxy.ps1")
