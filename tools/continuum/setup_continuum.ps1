# Sets up H3 Continuum (github.com/ukr8b3g-cmyk/ComfyUI-H3-Continuum, MIT) for ComfyUI Desktop's MiniMax H3: long videos made in chunks
# that continue each other (picture and sound), past H3's 15 seconds. One custom node pack, no models and no Python packages: the v3.9.8
# release ZIP (checked against its published SHA-256), without its tests, into ComfyUI\custom_nodes. Then restart ComfyUI.
# Running it again when it is there does nothing. Try2Create runs it from the Video page; -Log gets one line per step and "ok" or "error: ...".
param(
    [string]$Install = "$env:LOCALAPPDATA\Comfy-Desktop\ComfyUI-Installs\ComfyUI",
    [string]$Nodes = '', # default: <Install>\ComfyUI\custom_nodes
    [string]$Log = ''
)
$ErrorActionPreference = 'Stop'
$version = '3.9.8'
$url = "https://github.com/ukr8b3g-cmyk/ComfyUI-H3-Continuum/releases/download/v$version/ComfyUI-H3-Continuum-v$version.zip"
$sha = '98daf8693d4ae89ec721979131491a7da10913b48218f235df098d5d7a737a45'
$comfy = Join-Path $Install 'ComfyUI'
if (-not $Nodes) { $Nodes = Join-Path $comfy 'custom_nodes' }

function Say($text) {
    Write-Host $text
    if ($Log) { Add-Content -Path $Log -Value $text -Encoding utf8 }
}

try {
    if (-not (Test-Path (Join-Path $comfy 'main.py')) -and -not $PSBoundParameters.ContainsKey('Nodes')) { throw "ComfyUI was not found in $Install" }
    $node = Join-Path $Nodes 'ComfyUI-H3-Continuum'
    if (Test-Path (Join-Path $node 'nodes.py')) {
        Say 'node: already there'
    } else {
        Say "node: downloading H3 Continuum $version (20 MB)"
        $tmp = Join-Path $env:TEMP "continuum_setup_$PID"
        New-Item -ItemType Directory -Force $tmp | Out-Null
        $zip = Join-Path $tmp 'continuum.zip'
        $ErrorActionPreference = 'Continue'
        & curl.exe -L --fail --retry 5 --retry-delay 5 --no-progress-meter -o $zip $url 2>&1 | Out-Host
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($code -ne 0) { throw "download failed (curl $code)" }
        $got = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
        if ($got -ne $sha) { throw "the download is not the published release (SHA-256 $got)" }
        Say 'node: unpacking'
        # entry by entry, without tests\ and .github\ (its test fixtures have paths too long for Windows' 260-character limit)
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
        $partial = "$node.partial"
        if (Test-Path $partial) { Remove-Item -Recurse -Force $partial }
        try {
            foreach ($e in $archive.Entries) {
                $rel = $e.FullName -replace '^ComfyUI-H3-Continuum/', ''
                if ($rel -eq $e.FullName -or $rel -eq '' -or $rel -match '^(tests|\.github)/') { continue }
                $dest = Join-Path $partial ($rel -replace '/', '\')
                if ($e.FullName.EndsWith('/')) { New-Item -ItemType Directory -Force $dest | Out-Null; continue }
                New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
                [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dest, $true)
            }
        } finally {
            $archive.Dispose()
        }
        Move-Item $partial $node
        Remove-Item -Recurse -Force $tmp
        if (-not (Test-Path (Join-Path $node 'nodes.py'))) { throw 'the download did not hold the node' }
        Say "node: installed in $node"
    }
    Say 'ok'
} catch {
    Say "error: $($_.Exception.Message)"
    exit 1
}
