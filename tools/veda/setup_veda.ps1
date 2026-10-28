# Sets up Veda sparse attention (github.com/veda-sparse/Veda-on-ComfyUI, MIT) for ComfyUI Desktop's MiniMax H3:
#   1. the custom node, at the commit this app was tested with, into ComfyUI\custom_nodes\Veda-on-ComfyUI
#   2. triton-windows 3.8 (the version that goes with torch 2.12) into ComfyUI's Python, if it has no triton
#   3. the predictors (275 MB each, checked against their SHA-256) into ComfyUI\models\veda:
#      T2VA (for text and picture to video) and R2VA (for reference to video: Star in a video)
# Then restart ComfyUI. Every step is skipped when it is already done, so running it again finishes a broken run.
# Try2Create runs it from the Video page; -Log gets one line per step and "ok" or "error: ..." at the end.
param(
    [string]$Install = "$env:LOCALAPPDATA\Comfy-Desktop\ComfyUI-Installs\ComfyUI",
    [string]$Nodes = '',      # default: <Install>\ComfyUI\custom_nodes
    [string]$Predictors = '', # default: <Install>\ComfyUI\models\veda
    [string]$PyTarget = '',   # install triton into this folder instead of ComfyUI's Python (tests)
    [string]$Log = ''
)
$ErrorActionPreference = 'Stop'
$commit = '67e4eb5541b648555563144401ebaba832bd73ec'
$comfy = Join-Path $Install 'ComfyUI'
if (-not $Nodes) { $Nodes = Join-Path $comfy 'custom_nodes' }
if (-not $Predictors) { $Predictors = Join-Path $comfy 'models\veda' }
$python = Join-Path $comfy '.venv\Scripts\python.exe'
if (-not (Test-Path $python)) { $python = Join-Path $Install 'standalone-env\python.exe' }
$uv = Join-Path $Install 'standalone-env\uv.exe'

function Say($text) {
    Write-Host $text
    if ($Log) { Add-Content -Path $Log -Value $text -Encoding utf8 }
}

# curl with its stderr kept out of PowerShell's error records; the exit code decides
function Fetch($url, $dest) {
    $ErrorActionPreference = 'Continue'
    & curl.exe -L --fail --retry 5 --retry-delay 10 -C - --no-progress-meter -o $dest $url 2>&1 | Out-Host
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { throw "download failed (curl $code): $url" }
}

try {
    if (-not (Test-Path (Join-Path $comfy 'main.py'))) { throw "ComfyUI was not found in $Install" }

    # 1. the node
    $node = Join-Path $Nodes 'Veda-on-ComfyUI'
    if (Test-Path (Join-Path $node 'veda_comfy\nodes.py')) {
        Say 'node: already there'
    } else {
        Say 'node: downloading Veda-on-ComfyUI'
        $tmp = Join-Path $env:TEMP "veda_setup_$PID"
        New-Item -ItemType Directory -Force $tmp | Out-Null
        $zip = Join-Path $tmp 'veda.zip'
        Fetch "https://github.com/veda-sparse/Veda-on-ComfyUI/archive/$commit.zip" $zip
        Expand-Archive -Path $zip -DestinationPath $tmp -Force
        New-Item -ItemType Directory -Force $Nodes | Out-Null
        Move-Item (Join-Path $tmp "Veda-on-ComfyUI-$commit") $node
        Remove-Item -Recurse -Force $tmp
        Say "node: installed in $node"
    }

    # 2. triton (the sparse kernel is written in it)
    $ErrorActionPreference = 'Continue'
    if ($PyTarget) { $env:PYTHONPATH = $PyTarget }
    & $python -c "import triton" 2>$null
    $haveTriton = $LASTEXITCODE -eq 0
    $ErrorActionPreference = 'Stop'
    if ($haveTriton) {
        Say 'triton: already there'
    } else {
        Say 'triton: installing triton-windows 3.8'
        $spec = 'triton-windows>=3.8,<3.9'
        $ErrorActionPreference = 'Continue'
        if (Test-Path $uv) {
            if ($PyTarget) { & $uv pip install --python $python --target $PyTarget $spec 2>&1 | Out-Host }
            else { & $uv pip install --python $python $spec 2>&1 | Out-Host }
        } else {
            if ($PyTarget) { & $python -m pip install --target $PyTarget $spec 2>&1 | Out-Host }
            else { & $python -m pip install $spec 2>&1 | Out-Host }
        }
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($code -ne 0) { throw "installing triton-windows failed ($code)" }
        Say 'triton: installed'
    }

    # 3. the predictors
    New-Item -ItemType Directory -Force $Predictors | Out-Null
    $files = @(
        @{ name = 'minimax_h3_t2va_veda_8nfe_600step_preview_fp8.safetensors'; repo = 'Veda-Sparse/Minimax-H3-T2VA-Veda-8NFE-600Step-Preview'
           rev = '9a1fd3a41b4a754a7886e64e82edbddf599fd1bd'; sha = '2a8d8845c5342756a2781e8e69563940e4bb573c9a40ebb534915ff8fd76573a' },
        @{ name = 'minimax_h3_r2va_veda_preview_fp8.safetensors'; repo = 'Veda-Sparse/Minimax-H3-R2VA-Veda-Preview'
           rev = '1598b407905b7c0af85aa0c13acdfa6f01ff2982'; sha = '23edace948693772be33d5cc945d302d2b037a29bd44bd57071bfbdd8b02517d' }
    )
    foreach ($f in $files) {
        $dest = Join-Path $Predictors $f.name
        if (Test-Path $dest) { Say "predictor: have $($f.name)"; continue }
        Say "predictor: downloading $($f.name) (275 MB)"
        Fetch "https://huggingface.co/$($f.repo)/resolve/$($f.rev)/$($f.name)" "$dest.part"
        $hash = (Get-FileHash -Algorithm SHA256 "$dest.part").Hash.ToLower()
        if ($hash -ne $f.sha) { Remove-Item "$dest.part"; throw "$($f.name) did not match its SHA-256 (got $hash)" }
        Move-Item "$dest.part" $dest
    }
    Say 'ok'
} catch {
    Say "error: $($_.Exception.Message)"
    exit 1
}
