# Sets up a dedicated Python virtual environment for FunASR Mandarin ASR.
#
# Usage:
#   cd D:\esp32p4\classmate_code
#   .\tools\setup_asr.ps1            # create .asr_venv and install deps
#   .\tools\setup_asr.ps1 -Warmup    # also download the models once
#
# After this succeeds, start_lexin_proxy.ps1 auto-detects .asr_venv and
# wires LEXIN_ASR_CMD so the proxy runs real speech recognition.

param(
    [switch]$Warmup,
    [string]$PythonLauncher = "py",
    [string]$PythonVersion = "3.11"
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$VenvDir = Join-Path $Root ".asr_venv"
$VenvPython = Join-Path $VenvDir "Scripts\python.exe"
$AsrScript = Join-Path $Root "tools\asr_funasr.py"

Write-Host "LeXin ASR setup"
Write-Host "Root:  $Root"
Write-Host "Venv:  $VenvDir"

if (-not (Test-Path $VenvPython)) {
    Write-Host "Creating virtual environment (.asr_venv)..."
    try {
        & $PythonLauncher "-$PythonVersion" -m venv $VenvDir
    } catch {
        Write-Host "py launcher unavailable, trying 'python -m venv'..."
        & python -m venv $VenvDir
    }
}

if (-not (Test-Path $VenvPython)) {
    throw "Failed to create venv at $VenvDir"
}

Write-Host "Upgrading pip..."
& $VenvPython -m pip install --upgrade pip

Write-Host "Installing CPU PyTorch + FunASR (this can take a while)..."
# CPU wheels are the default on Windows; keep it explicit for clarity.
& $VenvPython -m pip install numpy
& $VenvPython -m pip install torch torchaudio --index-url https://download.pytorch.org/whl/cpu
& $VenvPython -m pip install funasr modelscope

if ($Warmup) {
    Write-Host "Warming up: downloading FunASR models (one-time, ~1 GB)..."
    # Generate a short silent WAV and transcribe it so all models download.
    $WarmWav = Join-Path $env:TEMP "lexin_asr_warmup.wav"
    $gen = @"
import wave, struct
with wave.open(r'$WarmWav', 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
    w.writeframes(struct.pack('<' + 'h' * 16000, *([0] * 16000)))
"@
    & $VenvPython -c $gen
    & $VenvPython $AsrScript $WarmWav zh
    Remove-Item $WarmWav -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "ASR setup complete."
Write-Host "Python: $VenvPython"
Write-Host ""
Write-Host "Next: run .\tools\start_lexin_proxy.ps1 (it will detect .asr_venv)."
Write-Host "Optional (faster replies): start the always-on ASR daemon with:"
Write-Host "  & '$VenvPython' '$AsrScript' --serve"
