# Sets up MaskVidExperiments (github.com/drozbay/MaskVidExperiments, GPL-3.0) for ComfyUI Desktop: video mask tools that let a character
# swap regenerate only the masked person (mask cleanup, and a pixel-to-latent mask reduction that knows MiniMax H3's frame grouping).
# One custom node pack, no models and no Python packages beyond ComfyUI's own, at the commit this app was tested with, into
# ComfyUI\custom_nodes. Then restart ComfyUI. Running it again when it is there does nothing.
# Try2Create runs it from the Video page; -Log gets one line per step and "ok" or "error: ..." at the end.
param(
    [string]$Install = "$env:LOCALAPPDATA\Comfy-Desktop\ComfyUI-Installs\ComfyUI",
    [string]$Nodes = '', # default: <Install>\ComfyUI\custom_nodes
    [string]$Log = ''
)
$ErrorActionPreference = 'Stop'
$commit = 'c32ed8c' # 11 Sep 2026: "Change divisible_by default to 32 on crop nodes"
$comfy = Join-Path $Install 'ComfyUI'
if (-not $Nodes) { $Nodes = Join-Path $comfy 'custom_nodes' }

function Say($text) {
    Write-Host $text
    if ($Log) { Add-Content -Path $Log -Value $text -Encoding utf8 }
}

try {
    if (-not (Test-Path (Join-Path $comfy 'main.py')) -and -not $PSBoundParameters.ContainsKey('Nodes')) { throw "ComfyUI was not found in $Install" }
    $node = Join-Path $Nodes 'MaskVidExperiments'
    if (Test-Path (Join-Path $node 'nodes_mask_to_latent.py')) {
        Say 'node: already there'
    } else {
        Say 'node: downloading MaskVidExperiments'
        $tmp = Join-Path $env:TEMP "maskvid_setup_$PID"
        New-Item -ItemType Directory -Force $tmp | Out-Null
        $zip = Join-Path $tmp 'maskvid.zip'
        $ErrorActionPreference = 'Continue'
        & curl.exe -L --fail --retry 5 --retry-delay 5 --no-progress-meter -o $zip "https://github.com/drozbay/MaskVidExperiments/archive/$commit.zip" 2>&1 | Out-Host
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($code -ne 0) { throw "download failed (curl $code)" }
        Expand-Archive -Path $zip -DestinationPath $tmp -Force
        $folder = Get-ChildItem $tmp -Directory | Where-Object { $_.Name -like 'MaskVidExperiments-*' } | Select-Object -First 1
        if (-not $folder) { throw 'the download did not hold the node' }
        New-Item -ItemType Directory -Force $Nodes | Out-Null
        Move-Item $folder.FullName $node
        Remove-Item -Recurse -Force $tmp
        if (-not (Test-Path (Join-Path $node 'nodes_mask_to_latent.py'))) { throw 'the download did not hold the node' }
        Say "node: installed in $node"
    }
    Say 'ok'
} catch {
    Say "error: $($_.Exception.Message)"
    exit 1
}
