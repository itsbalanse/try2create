# Sets up Fizgig H3 Tweaks (github.com/shootthesound/ComfyUI-Fizgig-H3-Tweaks, MIT) for ComfyUI Desktop's MiniMax H3:
# one custom node, no models and no Python packages, at the commit this app was tested with, into ComfyUI\custom_nodes.
# Then restart ComfyUI. Running it again when it is there does nothing.
# Try2Create runs it from the Video page; -Log gets one line per step and "ok" or "error: ..." at the end.
param(
    [string]$Install = "$env:LOCALAPPDATA\Comfy-Desktop\ComfyUI-Installs\ComfyUI",
    [string]$Nodes = '', # default: <Install>\ComfyUI\custom_nodes
    [string]$Log = ''
)
$ErrorActionPreference = 'Stop'
$commit = '5f8b48a3b54978b1b7d81ea0ddf5f581752a6218' # v1.0.2, 25 Sep 2026
$comfy = Join-Path $Install 'ComfyUI'
if (Test-Path (Join-Path $Install 'main.py')) { $comfy = $Install } # (a git clone: main.py in the folder itself)
if (-not $Nodes) { $Nodes = Join-Path $comfy 'custom_nodes' }

function Say($text) {
    Write-Host $text
    if ($Log) { Add-Content -Path $Log -Value $text -Encoding utf8 }
}

try {
    if (-not (Test-Path (Join-Path $comfy 'main.py')) -and -not $PSBoundParameters.ContainsKey('Nodes')) { throw "ComfyUI was not found in $Install" }
    $node = Join-Path $Nodes 'ComfyUI-Fizgig-H3-Tweaks'
    if (Test-Path (Join-Path $node 'h3tweaks.py')) {
        Say 'node: already there'
    } else {
        Say 'node: downloading Fizgig H3 Tweaks'
        $tmp = Join-Path $env:TEMP "fizgig_setup_$PID"
        New-Item -ItemType Directory -Force $tmp | Out-Null
        $zip = Join-Path $tmp 'fizgig.zip'
        $ErrorActionPreference = 'Continue'
        & curl.exe -L --fail --retry 5 --retry-delay 5 --no-progress-meter -o $zip "https://github.com/shootthesound/ComfyUI-Fizgig-H3-Tweaks/archive/$commit.zip" 2>&1 | Out-Host
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($code -ne 0) { throw "download failed (curl $code)" }
        Expand-Archive -Path $zip -DestinationPath $tmp -Force
        New-Item -ItemType Directory -Force $Nodes | Out-Null
        Move-Item (Join-Path $tmp "ComfyUI-Fizgig-H3-Tweaks-$commit") $node
        Remove-Item -Recurse -Force $tmp
        if (-not (Test-Path (Join-Path $node 'h3tweaks.py'))) { throw 'the download did not hold the node' }
        Say "node: installed in $node"
    }
    Say 'ok'
} catch {
    Say "error: $($_.Exception.Message)"
    exit 1
}
