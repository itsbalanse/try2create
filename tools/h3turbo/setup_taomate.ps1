# Sets up TaoMate-H3-FL2VA (TaoLiveAIGC, MiniMax H3 Community License) as the Video page's "Presenter" quality:
#   1. downloads the 3-step adapter (2.5 GB, checked against its SHA-256) into %TEMP%\taomate
#   2. converts it into a normal ComfyUI LoRA (convert_taomate.py, with ComfyUI's own Python) in models\loras (1.2 GB), then deletes the download
# Needs the FL2VA base (Comfy-Org minimax_h3_fl2va_pruned_int8_convrot) that Fast uses: every LoRA target is checked against it.
# Try2Create runs it from the Video page; -Log gets one line per step and "ok" or "error: ..." at the end.
param(
    [string]$Models = "$env:LOCALAPPDATA\Comfy-Desktop\ComfyUI-Shared\models",
    [string]$Python = "$env:LOCALAPPDATA\Comfy-Desktop\ComfyUI-Installs\ComfyUI\ComfyUI\.venv\Scripts\python.exe",
    [string]$Log = ''
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$base = Join-Path $Models 'diffusion_models\minimax_h3_fl2va_pruned_int8_convrot.safetensors'
$lora = Join-Path $Models 'loras\taomate_h3_fl2va_step300_comfyui.safetensors'
$tmp = Join-Path $env:TEMP 'taomate'
$raw = Join-Path $tmp 'adapter_model.safetensors'
$sha = '9f4b2ebb54d9aef45ae498982ce4a6166e44ac66a7852a007ae1e52305ca921d'
$urls = @('https://modelscope.ai/api/v1/models/TaoLiveAIGC/TaoMate-H3-FL2VA/repo?Revision=master&FilePath=adapter_model.safetensors',
          'https://huggingface.co/TaoLiveAIGC/TaoMate-H3-FL2VA/resolve/main/adapter_model.safetensors')

function Say($text) {
    Write-Host $text
    if ($Log) { Add-Content -Path $Log -Value $text -Encoding utf8 }
}

try {
    if (Test-Path $lora) { Say 'lora: already there'; Say 'ok'; exit 0 }
    if (-not (Test-Path $base)) { throw "the FL2VA base is not in $base (the Video page's Fast quality needs it too)" }
    if (-not (Test-Path $Python)) { throw "ComfyUI's Python was not found at $Python" }
    New-Item -ItemType Directory -Force $tmp | Out-Null
    if (-not (Test-Path $raw)) {
        $done = $false
        foreach ($url in $urls) {
            Say 'download: the TaoMate adapter (2.5 GB)'
            $ErrorActionPreference = 'Continue'
            & curl.exe -L --fail --retry 5 --retry-delay 10 -C - --no-progress-meter -o "$raw.part" $url 2>&1 | Out-Host
            $code = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            if ($code -eq 0) { $done = $true; break }
            Say "download: that address failed ($code), trying the next"
        }
        if (-not $done) { throw 'the adapter could not be downloaded' }
        Say 'check: SHA-256'
        if ((Get-FileHash "$raw.part" -Algorithm SHA256).Hash.ToLower() -ne $sha) { Remove-Item "$raw.part"; throw 'the download is not the published file (SHA-256 differs)' }
        Move-Item -Force "$raw.part" $raw
    }
    Say 'convert: into a ComfyUI LoRA'
    New-Item -ItemType Directory -Force (Split-Path $lora) | Out-Null
    $ErrorActionPreference = 'Continue'
    & $Python -I (Join-Path $here 'convert_taomate.py') $raw $base $lora 2>&1 | Out-Host
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0 -or -not (Test-Path $lora)) { throw "the conversion failed ($code)" }
    Remove-Item $raw
    Say 'ok'
} catch {
    Say "error: $($_.Exception.Message)"
    exit 1
}
