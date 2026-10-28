# Sets up Fish Audio Drama 3 (github.com/PedroMarinhoDev/ComfyUI-Drama3, GPL-3.0; weights under the Fish Audio Community License)
# for ComfyUI Desktop:
#   1. the custom nodes, at the commit this app was tested with, into ComfyUI\custom_nodes\ComfyUI-Drama3 (nothing to pip install)
#   2. models\asr: the nodes look for Qwen3-ASR in ComfyUI's own models folder, which ComfyUI Desktop does not use: it becomes a link
#      to the shared models folder's asr
#   3. the files, from Hugging Face, checked by size: the Drama 3 model (-Variant), its codec, and Qwen3-ASR (-Asr)
# -Gguf (since 2026-10-26, what Try2Create uses): instead of the big model and its codec, audio.cpp's server (into <Tools>\audiocpp) and
#   Jmica/Drama-3-GGUF's Q3 experts / Q8 backbone file (into models\audiocpp), plus Try2Create's Drama3GGUFGenerate node (from
#   <Tools>\drama3gguf) with its settings; ComfyUI-Drama3 is still installed for its Qwen3-ASR transcript nodes.
# Then restart ComfyUI. Every step is skipped when it is already done, so running it again finishes a broken run (downloads resume).
# Try2Create runs it from the Drama page; -Log gets one line per step and "ok" or "error: ..." at the end.
param(
    [string]$Install = "$env:LOCALAPPDATA\Comfy-Desktop\ComfyUI-Installs\ComfyUI",
    [string]$Models = "$env:LOCALAPPDATA\Comfy-Desktop\ComfyUI-Shared\models",
    [string]$Nodes = '',          # default: <Install>\ComfyUI\custom_nodes
    [ValidateSet('int8', 'w6a8', 'w4a8', 'fp8')][string]$Variant = 'int8',
    [ValidateSet('1.7b', '0.6b')][string]$Asr = '1.7b',
    [switch]$NodesOnly,
    [switch]$Gguf,
    [string]$Tools = '',
    [string]$Log = ''
)
$ErrorActionPreference = 'Stop'
$commit = '66c4fb9cd3c0151e1e2ccd207847cb8335e87ab4'
$comfy = Join-Path $Install 'ComfyUI'
if (-not $Nodes) { $Nodes = Join-Path $comfy 'custom_nodes' }
$hf = 'https://huggingface.co/PedroMarinhoDev'
$sizes = @{
    'drama3_int8experts_bf16.safetensors' = 38696401232
    'drama3_w6a8experts_bf16.safetensors' = 32656613840
    'drama3_w4a8experts_bf16.safetensors' = 24603565376
    'drama3_fp8experts_bf16.safetensors'  = 38578417928
    'drama3_codec.safetensors'            = 1565647484
    'qwen3_asr_1.7b_bf16.safetensors'     = 4076192608
    'qwen3_asr_0.6b_bf16.safetensors'     = 1564928184
    'drama-3-backbone-8bit-experts-Q3_K_S.gguf' = 20325196992
}
$audiocppZip = 'https://github.com/0xShug0/audio.cpp/releases/download/v0.9.2-drama3-base-small/audio-v0.9.2-drama3-base-small-bin-windows-x64-cuda12.4.zip'

function Say($text) {
    Write-Host $text
    if ($Log) { Add-Content -Path $Log -Value $text -Encoding utf8 }
}

# curl with its stderr kept out of PowerShell's error records; the exit code decides
function Fetch($url, $dest) {
    $ErrorActionPreference = 'Continue'
    & curl.exe -L --fail --retry 10 --retry-delay 10 -C - --no-progress-meter -o $dest $url 2>&1 | Out-Host
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { throw "download failed (curl $code): $url" }
}

# one model file into models\<folder>, resumed from its .part
function GetModel($repo, $file, $folder) {
    $dir = Join-Path $Models $folder
    $dest = Join-Path $dir $file
    $size = $sizes[$file]
    if ((Test-Path $dest) -and (Get-Item $dest).Length -eq $size) { Say "$($file): already there"; return }
    New-Item -ItemType Directory -Force $dir | Out-Null
    Say ("{0}: downloading ({1:N1} GB)" -f $file, ($size / 1GB))
    Fetch "$hf/$repo/resolve/main/$file" "$dest.part"
    if ((Get-Item "$dest.part").Length -ne $size) { throw "$file came out the wrong size; run the setup again to resume it" }
    Move-Item -Force "$dest.part" $dest
    Say "$($file): done"
}

try {
    if (-not (Test-Path (Join-Path $comfy 'main.py'))) { throw "ComfyUI was not found in $Install" }

    # 1. the nodes
    $node = Join-Path $Nodes 'ComfyUI-Drama3'
    if (Test-Path (Join-Path $node 'nodes.py')) {
        Say 'nodes: already there'
    } else {
        Say 'nodes: downloading ComfyUI-Drama3'
        $tmp = Join-Path $env:TEMP "drama3_setup_$PID"
        New-Item -ItemType Directory -Force $tmp | Out-Null
        $zip = Join-Path $tmp 'drama3.zip'
        Fetch "https://github.com/PedroMarinhoDev/ComfyUI-Drama3/archive/$commit.zip" $zip
        Expand-Archive -Path $zip -DestinationPath $tmp -Force
        New-Item -ItemType Directory -Force $Nodes | Out-Null
        Move-Item (Join-Path $tmp "ComfyUI-Drama3-$commit") $node
        Remove-Item -Recurse -Force $tmp
        Say "nodes: installed in $node"
    }
    if ($NodesOnly) { Say 'ok'; exit 0 }

    # 2. models\asr: a link to the shared folder's (the nodes read <ComfyUI>\models\asr only)
    $shared = Join-Path $Models 'asr'
    $own = Join-Path $comfy 'models\asr'
    New-Item -ItemType Directory -Force $shared | Out-Null
    if ((Resolve-Path $shared).Path -ne (Join-Path (Resolve-Path (Join-Path $comfy 'models')).Path 'asr')) {
        $item = Get-Item $own -ErrorAction SilentlyContinue
        if (-not $item) {
            New-Item -ItemType Junction -Path $own -Target $shared | Out-Null
            Say 'asr: models\asr links to the shared models folder'
        } elseif ($item.LinkType -eq 'Junction') {
            Say 'asr: already linked'
        } else {
            $shared = $own # a real folder there already: the model goes into it
            Say 'asr: using ComfyUI''s own models\asr'
        }
    }

    # 3. the files
    if ($Gguf) {
        if (-not $Tools) { $Tools = Join-Path (Split-Path $PSScriptRoot -Parent) '' }
        # audio.cpp's server, with the CUDA 12 runtime the app's llama build already has
        $ac = Join-Path $Tools 'audiocpp'
        if (Test-Path (Join-Path $ac 'audiocpp_server.exe')) {
            Say 'audio.cpp: already there'
        } else {
            Say 'audio.cpp: downloading (464 MB)'
            $tmp = Join-Path $env:TEMP "audiocpp_setup_$PID"
            New-Item -ItemType Directory -Force $tmp | Out-Null
            Fetch $audiocppZip (Join-Path $tmp 'audiocpp.zip')
            New-Item -ItemType Directory -Force $ac | Out-Null
            Expand-Archive -Path (Join-Path $tmp 'audiocpp.zip') -DestinationPath $ac -Force
            Remove-Item -Recurse -Force $tmp
            Say "audio.cpp: installed in $ac"
        }
        foreach ($dll in 'cudart64_12.dll', 'cublas64_12.dll', 'cublasLt64_12.dll') {
            $src = Join-Path $Tools "llama-cpp-cuda\$dll"
            if (-not (Test-Path (Join-Path $ac $dll)) -and (Test-Path $src)) { Copy-Item $src $ac }
        }
        # and cuFFT 11, which its server also loads (not in the zip nor llama's build): from NVIDIA's own wheel for CUDA 12.4
        if (-not (Test-Path (Join-Path $ac 'cufft64_11.dll'))) {
            Say 'audio.cpp: downloading cuFFT (200 MB)'
            $tmp = Join-Path $env:TEMP "cufft_setup_$PID"
            New-Item -ItemType Directory -Force $tmp | Out-Null
            $whl = Join-Path $tmp 'cufft.zip'
            Fetch 'https://files.pythonhosted.org/packages/f6/ee/3f3f8e9874f0be5bbba8fb4b62b3de050156d159f8b6edc42d6f1074113b/nvidia_cufft_cu12-11.2.1.3-py3-none-win_amd64.whl' $whl
            if ((Get-FileHash $whl -Algorithm SHA256).Hash -ne 'D802F4954291101186078CCBE22FC285A902136F974D369540FD4A5333D1440B') { throw 'cuFFT download is damaged' }
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $zip = [IO.Compression.ZipFile]::OpenRead($whl)
            try { [IO.Compression.ZipFileExtensions]::ExtractToFile($zip.GetEntry('nvidia/cufft/bin/cufft64_11.dll'), (Join-Path $ac 'cufft64_11.dll'), $true) } finally { $zip.Dispose() }
            Remove-Item -Recurse -Force $tmp
        }
        # the GGUF (hosted by Jmica, not PedroMarinhoDev)
        $keepHf = $hf; $hf = 'https://huggingface.co/Jmica'
        GetModel 'Drama-3-GGUF' 'drama-3-backbone-8bit-experts-Q3_K_S.gguf' 'audiocpp'
        $hf = $keepHf
        # our node, and where it finds the server and the model
        $gnode = Join-Path $Nodes 'ComfyUI-Drama3GGUF'
        New-Item -ItemType Directory -Force $gnode | Out-Null
        Copy-Item -Force (Join-Path $Tools 'drama3gguf\ComfyUI-Drama3GGUF\__init__.py') $gnode
        $settings = @{ exe = (Join-Path $ac 'audiocpp_server.exe'); model = (Join-Path $Models 'audiocpp\drama-3-backbone-8bit-experts-Q3_K_S.gguf'); port = 8035 }
        [IO.File]::WriteAllText((Join-Path $gnode 'drama3gguf.json'), ($settings | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
        Say "node: Drama3GGUFGenerate in $gnode"
    } else {
        GetModel 'Drama-3-ComfyUI' 'drama3_codec.safetensors' 'vae'
    }
    if ($shared -eq $own) {
        $keep = $Models; $Models = Join-Path $comfy 'models'
        GetModel 'Qwen3-ASR-ComfyUI' "qwen3_asr_$($Asr)_bf16.safetensors" 'asr'
        $Models = $keep
    } else {
        GetModel 'Qwen3-ASR-ComfyUI' "qwen3_asr_$($Asr)_bf16.safetensors" 'asr'
    }
    if (-not $Gguf) { GetModel 'Drama-3-ComfyUI' "drama3_$($Variant)experts_bf16.safetensors" 'diffusion_models' }
    Say 'ok'
} catch {
    Say "error: $($_.Exception.Message)"
    exit 1
}
