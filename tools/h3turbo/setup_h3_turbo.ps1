# Sets up Wuli-art's MiniMax-H3-Turbo (4 steps) for native ComfyUI:
#   1. downloads the official FL2VA base (Comfy-Org, pruned int8_convrot, 19.5 GB) if it isn't there
#   2. downloads Wuli's 4-step adapter (2.6 GB) and converts it into a normal ComfyUI LoRA (convert_wuli_4step.py)
# Then load H3_Turbo_4step_*_API.json (ComfyUI: Workflow > Open, or this app's ComfyUI tab).
param(
    [string]$Models = "$env:LOCALAPPDATA\Comfy-Desktop\ComfyUI-Shared\models",
    [string]$Python = "$env:LOCALAPPDATA\Comfy-Desktop\ComfyUI-Installs\ComfyUI\ComfyUI\.venv\Scripts\python.exe",
    [switch]$SkipBase
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$base = Join-Path $Models 'diffusion_models\minimax_h3_fl2va_pruned_int8_convrot.safetensors'
$lora = Join-Path $Models 'loras\minimax_h3_turbo_4step_wuli_v1.0_comfyui.safetensors'
$tmp = Join-Path $env:TEMP 'h3turbo'
$wuli = Join-Path $tmp 'Wuli-MiniMax-H3-Turbo-4steps-V1.0-bf16.safetensors'

function Fetch($url, $dest) {
    if (Test-Path $dest) { Write-Host "have $dest"; return }
    New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
    Write-Host "downloading $url"
    # Windows PowerShell turns anything curl writes to stderr into an error record: keep it quiet, and go by the exit code
    $ErrorActionPreference = 'Continue'
    & curl.exe -L --fail --retry 5 --retry-delay 10 -C - --no-progress-meter -o "$dest.part" $url 2>&1 | Out-Host
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { throw "download failed ($code): $url" }
    Move-Item "$dest.part" $dest
}

if (-not $SkipBase) {
    Fetch 'https://huggingface.co/Comfy-Org/MiniMax-H3/resolve/main/diffusion_models/minimax_h3_fl2va_pruned_int8_convrot.safetensors' $base
}
Fetch 'https://huggingface.co/Wuli-art/MiniMax-H3-Turbo/resolve/main/turbo-4step/Wuli-MiniMax-H3-Turbo-4steps-V1.0-bf16.safetensors' $wuli

# the bank is stored relative to the base heads: read them from the base (or, without it, from any FL2VA checkpoint)
$headsFrom = if (Test-Path $base) { $base } else { throw "need the FL2VA base at $base (its output heads are read)" }
& $Python -I (Join-Path $here 'convert_wuli_4step.py') $wuli $headsFrom $lora
if ($LASTEXITCODE -ne 0) { throw 'conversion failed' }
Remove-Item $wuli
Write-Host "done: $lora"
Write-Host 'settings: LoRA strength 1.0, 4 steps, simple, euler, CFG 1, ModelSamplingMiniMaxH3 shift_video 6 / shift_audio 3'
