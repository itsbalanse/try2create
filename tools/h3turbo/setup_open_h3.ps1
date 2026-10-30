# Sets up Open-H3-Regenerate-2K (NVlabs; Efficient-Large-Model/Open-H3-Regenerate-2K, MiniMax H3 Community License) for the Video page's
# "Make it crisp":
#   1. downloads its merged LoRA (12.5 GB) and the LMS Sharp curve branches (1.2 GB), checked against their SHA-256, into -Temp
#   2. converts them into one ComfyUI LoRA (convert_open_h3_regenerate.py, with ComfyUI's own Python) in models\loras\MiniMax H3 (10 GB), then
#      deletes the downloads
#   3. when they are missing: the MiniMax H3 latent upscaler's nodes (LBH-123-AI, pinned) into custom_nodes and its bf16 model (0.7 GB)
# Needs the FL2VA base (Comfy-Org minimax_h3_fl2va_pruned_int8_convrot) that Fast uses: every LoRA target is checked against it.
# Try2Create runs it from the Video page; -Log gets one line per step and "ok" or "error: ..." at the end.
param(
    [string]$Models = "$env:LOCALAPPDATA\Comfy-Desktop\ComfyUI-Shared\models",
    [string]$Install = "$env:LOCALAPPDATA\Comfy-Desktop\ComfyUI-Installs\ComfyUI",
    [string]$Temp = "$env:TEMP",
    [string]$Log = ''
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$comfy = Join-Path $Install 'ComfyUI'
if (Test-Path (Join-Path $Install 'main.py')) { $comfy = $Install } # (a git clone: main.py in the folder itself)
$python = Join-Path $comfy '.venv\Scripts\python.exe'
foreach ($p in 'python_embeded\python.exe', 'python_embedded\python.exe', 'ComfyUI\venv\Scripts\python.exe', 'venv\Scripts\python.exe', '.venv\Scripts\python.exe') { if (-not (Test-Path $python)) { $python = Join-Path $Install $p } } # (a portable ComfyUI, a git clone)
$base = Join-Path $Models 'diffusion_models\minimax_h3_fl2va_pruned_int8_convrot.safetensors'
$lora = Join-Path $Models 'loras\MiniMax H3\open_h3_regenerate_2k_comfyui.safetensors'
$tmp = Join-Path $Temp 'open_h3'
$rev = '5809c33730c18b6b1c827b40974855a4288641b3'
$files = @(
    @{ name = 'open_h3_regenerate_2k_lora.safetensors'; gb = '12.5'; sha = '8e36a0912ab72f7f3b00f41e3c7c97c7dfdec21e66c88ab22f08b9b0c681bb04' },
    @{ name = 'sharp_curve_branches.safetensors'; gb = '1.2'; sha = '26ff1c4ea50fc5b0cea252912b67ec308a65d534cef026ef56f415ae92102e4e' })
$nodeDir = Join-Path $comfy 'custom_nodes\Comfyui_Minimax_h3_latent_Upscaler'
$nodeCommit = '40316cf008b2fd8663263270669eb4da23f89d2c'
$upName = 'minimax_h3_latent_upscaler_3d_conv_v1_bf16.safetensors'
$upSha = '4f57821f5837f32f7142b67d815606dbd7550f194e5c769f7d6c3f83b146a5e6'
$upUrl = "https://huggingface.co/LBH-123-AI/Minimax_h3_latent_Upscaler/resolve/3f941d5d182014dd5c0a5e16330420ee2d4aa0c6/minimax_h3_latent_upscaler_3d_conv_v1/$upName"

function Say($text) {
    Write-Host $text
    if ($Log) { Add-Content -Path $Log -Value $text -Encoding utf8 }
}

function Fetch($url, $to, $sha, $what) {
    Say "download: $what"
    $ErrorActionPreference = 'Continue'
    & curl.exe -L --fail --retry 5 --retry-delay 10 -C - --no-progress-meter -o "$to.part" $url 2>&1 | Out-Host
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { throw "$what could not be downloaded ($code)" }
    Say 'check: SHA-256'
    if ((Get-FileHash "$to.part" -Algorithm SHA256).Hash.ToLower() -ne $sha) { Remove-Item "$to.part"; throw "$what is not the published file (SHA-256 differs)" }
    Move-Item -Force "$to.part" $to
}

try {
    if (-not (Test-Path $base)) { throw "the FL2VA base is not in $base (the Video page's Fast quality needs it too)" }
    if (-not (Test-Path $python)) { throw "ComfyUI's Python was not found at $python" }
    # the latent upscaler: its nodes (a pinned zip; ComfyUI loads them on its next start) and its model
    if (-not (Test-Path (Join-Path $nodeDir '__init__.py'))) {
        New-Item -ItemType Directory -Force $tmp | Out-Null
        $zip = Join-Path $tmp 'upscaler.zip'
        Say 'download: the H3 latent upscaler nodes'
        & curl.exe -L --fail --retry 5 --no-progress-meter -o $zip "https://github.com/LBH-123-AI/Comfyui_Minimax_h3_latent_Upscaler/archive/$nodeCommit.zip" 2>&1 | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'the latent upscaler nodes could not be downloaded' }
        $unz = Join-Path $tmp 'upscaler'
        Expand-Archive -Force $zip $unz
        Move-Item (Get-ChildItem $unz -Directory | Select-Object -First 1).FullName $nodeDir
        Remove-Item -Recurse -Force $zip, $unz
    }
    $upDir = Join-Path $comfy 'models\latent_upscale_models'
    if (-not (Test-Path (Join-Path $upDir $upName)) -and -not (Test-Path (Join-Path $Models "latent_upscale_models\$upName"))) {
        New-Item -ItemType Directory -Force $upDir | Out-Null
        Fetch $upUrl (Join-Path $upDir $upName) $upSha 'the H3 latent upscaler model (0.7 GB)'
    }
    if (Test-Path $lora) { Say 'lora: already there'; Say 'ok'; exit 0 }
    New-Item -ItemType Directory -Force $tmp | Out-Null
    foreach ($f in $files) {
        $to = Join-Path $tmp $f.name
        if (-not (Test-Path $to)) {
            Fetch "https://huggingface.co/Efficient-Large-Model/Open-H3-Regenerate-2K/resolve/$rev/lora/$($f.name)" $to $f.sha "$($f.name) ($($f.gb) GB)"
        }
    }
    Say 'convert: into a ComfyUI LoRA'
    New-Item -ItemType Directory -Force (Split-Path $lora) | Out-Null
    $ErrorActionPreference = 'Continue'
    & $python -I (Join-Path $here 'convert_open_h3_regenerate.py') (Join-Path $tmp $files[0].name) (Join-Path $tmp $files[1].name) $base "$lora.part" 2>&1 | Out-Host
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0 -or -not (Test-Path "$lora.part")) { throw "the conversion failed ($code)" }
    Move-Item -Force "$lora.part" $lora
    foreach ($f in $files) { Remove-Item (Join-Path $tmp $f.name) }
    Say 'ok'
} catch {
    Say "error: $($_.Exception.Message)"
    exit 1
}
