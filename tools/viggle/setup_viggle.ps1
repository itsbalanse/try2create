# Viggle-Animate for Star in a video's "Repaint a frame" swap, by the user's click: drbaph/Viggle-Animate-ComfyUI's pruned int8 model (21 GB),
# its DMD LoRA at rank 64 and the frozen text conditioning, each into its ComfyUI folder (resumable, then checked against the SHA-256 Hugging
# Face lists), and Saganaki22's ComfyUI-Viggle-Animate-H3 nodes (Apache-2.0) at the tested commit into custom_nodes. ComfyUI loads the nodes
# at its next start.
param([Parameter(Mandatory)] [string]$Models, [Parameter(Mandatory)] [string]$Install, [string]$Log = "")
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$comfy = Join-Path $Install 'ComfyUI'
if (Test-Path (Join-Path $Install 'main.py')) { $comfy = $Install } # (a git clone: main.py in the folder itself)
function Say([string]$s) { if ($Log) { Add-Content -Path $Log -Value $s -Encoding utf8 } else { $s } }
try {
    $repo = 'drbaph/Viggle-Animate-ComfyUI'
    $files = @(
        @{ path = 'diffusion_models/minimax_h3_ref2va_viggle_pruned_int8_convrot.safetensors'; dest = "$Models\diffusion_models" },
        @{ path = 'loras/viggle_animate_dmd_lora_r64.safetensors'; dest = "$Models\loras\MiniMax H3" },
        @{ path = 'text_cond/fixed_embed_fwd_anyframe.safetensors'; dest = "$comfy\models\text_cond" }
    )
    $tree = @()
    foreach ($dir in 'diffusion_models', 'loras', 'text_cond') { $tree += Invoke-RestMethod "https://huggingface.co/api/models/$repo/tree/main/$dir" }
    foreach ($f in $files) {
        $name = Split-Path $f.path -Leaf
        $out = Join-Path $f.dest $name
        if (Test-Path $out) { Say "already there: $name"; continue }
        $info = $tree | Where-Object { $_.path -eq $f.path } | Select-Object -First 1
        New-Item -ItemType Directory -Force $f.dest | Out-Null
        $part = "$out.part"
        Say "downloading $name ($([math]::Round($info.size / 1GB, 2)) GB)..."
        $ErrorActionPreference = 'Continue' # (Windows PowerShell 5.1 turns a native program's stderr into errors)
        & cmd.exe /c "curl.exe -sS -L --fail --retry 5 --retry-delay 5 -C - -o `"$part`" `"https://huggingface.co/$repo/resolve/main/$($f.path)`" 2>&1"
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($code -ne 0) { throw "the download of $name failed (curl $code); press Set up again to go on from where it stopped" }
        if ($info.lfs) {
            Say "checking $name..."
            $have = (Get-FileHash $part -Algorithm SHA256).Hash.ToLower()
            if ($have -ne $info.lfs.oid) { Remove-Item $part -Force; throw "$name came down damaged (its SHA-256 does not match): press Set up again" }
        }
        Move-Item $part $out -Force
    }
    $nodes = "$comfy\custom_nodes\ComfyUI-Viggle-Animate-H3"
    if (-not (Test-Path "$nodes\nodes.py")) {
        Say "getting its ComfyUI nodes..."
        $work = Join-Path (Split-Path $Models -Parent) 'viggle_nodes_tmp'
        if (Test-Path $work) { Remove-Item $work -Recurse -Force }
        New-Item -ItemType Directory -Force $work | Out-Null
        Invoke-WebRequest 'https://github.com/Saganaki22/ComfyUI-Viggle-Animate-H3/archive/6ae081af5b3175f99ef7cddebd5dfaf93390ab70.zip' -OutFile "$work\nodes.zip" -UseBasicParsing
        Expand-Archive "$work\nodes.zip" "$work\x" -Force
        $src = Get-ChildItem "$work\x" -Directory | Select-Object -First 1
        Move-Item $src.FullName $nodes
        Remove-Item $work -Recurse -Force
    }
    Say "done: restart ComfyUI once so it loads the Viggle-Animate nodes"
} catch {
    Say "error: $($_.Exception.Message)"
    exit 1
}
