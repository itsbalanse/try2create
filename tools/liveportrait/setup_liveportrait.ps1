# Sets up LivePortrait (github.com/kijai/ComfyUI-LivePortraitKJ, MIT; the models: KwaiVGI LivePortrait, MIT) for ComfyUI Desktop, for
# "copy the facial expressions" after a character swap: the swapped faces get the original performers' mouth and eye movements.
#   1. the custom node, at the commit this app was tested with, into ComfyUI\custom_nodes\ComfyUI-LivePortraitKJ, with four fixes:
#      - its face detector import works under the module name a current ComfyUI gives a node pack (it failed: "No module named
#        'ComfyUI-LivePortraitKJ'")
#      - retargeting holds the last face found through frames where the original's face is not found (it stopped with an error)
#      - its face_alignment cropper takes faces largest first (it took them in detection order, so "face 2" of the swap and of the original
#        could be different people) and a frame without that face counts as no face (it stopped: "list index out of range")
#      - a video whose first frame does not show that face is fine (it stopped: "No face detected in FIRST source image" - the second
#        person is often too small to find in a wide opening shot)
#   2. scikit-image, tifffile, pykalman and scikit-base into ComfyUI's Python, without their dependencies (the node's own list pins
#      numpy<=1.26.4, which would downgrade ComfyUI's numpy; these four run on ComfyUI's numpy 2 - tested)
#   3. the models (about 0.6 GB, checked against their SHA-256) into ComfyUI\models\liveportrait
# Its face finder (face_alignment: BlazeFace and FAN, about 100 MB) downloads itself into the torch cache on first use.
# Running it again does only what is missing. Then restart ComfyUI.
# Try2Create runs it from the Video page; -Log gets one line per step and "ok" or "error: ..." at the end.
param(
    [string]$Install = "$env:LOCALAPPDATA\Comfy-Desktop\ComfyUI-Installs\ComfyUI",
    [string]$Nodes = '',    # default: <Install>\ComfyUI\custom_nodes
    [string]$Models = '',   # default: <Install>\ComfyUI\models\liveportrait
    [string]$PyTarget = '', # tests: the Python packages into this folder instead of ComfyUI's Python
    [string]$Log = ''
)
$ErrorActionPreference = 'Stop'
$commit = '4d9dc6205b793ffd0fb319816136d9b8c0dbfdff' # 5 Aug 2024
$nodesSha = '07450845653ef62d491ded8ffad08649412a3cc664d38ea80bde3814359ff897' # nodes.py at that commit (as GitHub's archive has it), before the fixes
$comfy = Join-Path $Install 'ComfyUI'
if (Test-Path (Join-Path $Install 'main.py')) { $comfy = $Install } # (a git clone: main.py in the folder itself)
if (-not $Nodes) { $Nodes = Join-Path $comfy 'custom_nodes' }
if (-not $Models) { $Models = Join-Path $comfy 'models\liveportrait' }
$python = Join-Path $comfy '.venv\Scripts\python.exe'
if (-not (Test-Path $python)) { $python = Join-Path $Install 'standalone-env\python.exe' }
foreach ($p in 'python_embeded\python.exe', 'python_embedded\python.exe', 'ComfyUI\venv\Scripts\python.exe', 'venv\Scripts\python.exe', '.venv\Scripts\python.exe') { if (-not (Test-Path $python)) { $python = Join-Path $Install $p } } # (a portable ComfyUI, a git clone)
$files = [ordered]@{
    'appearance_feature_extractor.safetensors' = '38bef5de50a92bf1fc66e8c511051a19dfacdf80c37f8713425ec15dc9ca7d34'
    'motion_extractor.safetensors'             = '3568cd410e29d046771acb55ecfdfe4c7c197d345bd8b7f95942ef63130b6c9e'
    'spade_generator.safetensors'              = 'ca04fbec765745e9eae836d2d7522c274647b277ce5f25104fa1705b75222212'
    'stitching_retargeting_module.safetensors' = '60725cf3523ae413880da28ed583c9e84c5c25695a0cef1b77210ea31cc424ea'
    'warping_module.safetensors'               = 'f7b7834bd6039b4088f72e5161e60ad366f68a3763df8a3eac0bc0f9d46fdbbf'
    'landmark.onnx'                            = '31d22a5041326c31f19b78886939a634a5aedcaa5ab8b9b951a1167595d147db'
}

function Say($text) {
    Write-Host $text
    if ($Log) { Add-Content -Path $Log -Value $text -Encoding utf8 }
}

function Fix($path, $old, $new) { # one exact edit; already made counts as done
    $text = [IO.File]::ReadAllText($path)
    if ($text.Contains($new)) { return }
    if (-not $text.Contains($old)) { throw "the node is not the version this app knows ($([IO.Path]::GetFileName($path)))" }
    [IO.File]::WriteAllText($path, $text.Replace($old, $new), (New-Object Text.UTF8Encoding $false))
}

try {
    if (-not (Test-Path (Join-Path $comfy 'main.py')) -and -not $PSBoundParameters.ContainsKey('Nodes')) { throw "ComfyUI was not found in $Install" }
    if (-not (Test-Path $python)) { throw "ComfyUI's Python was not found" }

    # 1. the node
    $node = Join-Path $Nodes 'ComfyUI-LivePortraitKJ'
    if (Test-Path (Join-Path $node 'nodes.py')) {
        Say 'node: already there'
    } else {
        Say 'node: downloading ComfyUI-LivePortraitKJ'
        $tmp = Join-Path $env:TEMP "liveportrait_setup_$PID"
        New-Item -ItemType Directory -Force $tmp | Out-Null
        $zip = Join-Path $tmp 'lp.zip'
        $ErrorActionPreference = 'Continue'
        & curl.exe -L --fail --retry 5 --retry-delay 5 --no-progress-meter -o $zip "https://github.com/kijai/ComfyUI-LivePortraitKJ/archive/$commit.zip" 2>&1 | Out-Host
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($code -ne 0) { throw "download failed (curl $code)" }
        Expand-Archive -Path $zip -DestinationPath $tmp -Force
        $folder = Get-ChildItem $tmp -Directory | Where-Object { $_.Name -like 'ComfyUI-LivePortraitKJ-*' } | Select-Object -First 1
        if (-not $folder) { throw 'the download did not hold the node' }
        if ((Get-FileHash (Join-Path $folder.FullName 'nodes.py') -Algorithm SHA256).Hash.ToLower() -ne $nodesSha) { throw 'the downloaded node is not the version this app was tested with' }
        New-Item -ItemType Directory -Force $Nodes | Out-Null
        Move-Item $folder.FullName $node
        Remove-Item -Recurse -Force $tmp
        Say "node: installed in $node"
    }
    Fix (Join-Path $node 'face_alignment\api.py') `
        "face_detector_module = importlib.import_module('.face_alignment.detection.' + face_detector, package=package_directory_name)" `
        "face_detector_module = importlib.import_module('.detection.' + face_detector, package=__package__)  # (Try2Create: works under any module name ComfyUI gives the node)"
    Fix (Join-Path $node 'nodes.py') `
        "        for crop in driving_crop_info[`"crop_info_list`"]:`n            driving_landmarks.append(crop['lmk_crop'])" `
        "        last = next((c['lmk_crop'] for c in driving_crop_info[`"crop_info_list`"] if c is not None), None)`n        for crop in driving_crop_info[`"crop_info_list`"]:  # (Try2Create: a frame with no face found holds the last landmarks)`n            if crop is not None:`n                last = crop['lmk_crop']`n            driving_landmarks.append(last)"
    Fix (Join-Path $node 'liveportrait\utils\cropper.py') `
        "        if face_result is None:`n            ret_dct = {}`n            cropped_image_256 = None`n            return ret_dct, cropped_image_256`n`n        face_landmarks = face_result[face_index]" `
        "        if face_result is None or len(face_result) <= face_index:  # (Try2Create: no such face is no face, not an error)`n            ret_dct = {}`n            cropped_image_256 = None`n            return ret_dct, cropped_image_256`n`n        face_result = sorted(face_result, key=lambda p: -float(np.ptp(np.asarray(p)[:, 0]) * np.ptp(np.asarray(p)[:, 1])))  # (Try2Create: largest first)`n        face_landmarks = face_result[face_index]"
    $pipe = Join-Path $node 'liveportrait\live_portrait_pipeline.py'
    Fix $pipe "        driving_rot_list = []`n        `n        for i in tqdm(range(driving_images.shape[0])" `
        "        driving_rot_list = []`n        first = None  # (Try2Create: the first frame with this face, not frame 0)`n        `n        for i in tqdm(range(driving_images.shape[0])"
    Fix $pipe "                if i == 0:`n                    raise ValueError(`"No face detected in FIRST source image`")`n                continue" `
        "                continue  # (Try2Create: a first frame without this face is skipped, not an error)"
    Fix $pipe "            if i == 0:`n                first = x_d_info`n" "            if first is None:`n                first = x_d_info`n"
    Say 'node: fixed for this ComfyUI'

    # 2. the Python packages
    $ErrorActionPreference = 'Continue'
    if ($PyTarget) { $env:PYTHONPATH = $PyTarget }
    & $python -c "import skimage.io, tifffile; from pykalman import KalmanFilter" 2>$null
    $have = $LASTEXITCODE -eq 0
    $ErrorActionPreference = 'Stop'
    if ($have) {
        Say 'packages: already there'
    } else {
        Say 'packages: installing scikit-image, tifffile, pykalman, scikit-base'
        $specs = @('scikit-image', 'tifffile', 'pykalman', 'scikit-base')
        $ErrorActionPreference = 'Continue'
        if ($PyTarget) { & $python -m pip install --no-deps --only-binary=:all: --target $PyTarget @specs 2>&1 | Out-Host }
        else { & $python -m pip install --no-deps --only-binary=:all: @specs 2>&1 | Out-Host }
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($code -ne 0) { throw "installing the packages failed ($code)" }
        Say 'packages: installed'
    }

    # 3. the models
    New-Item -ItemType Directory -Force $Models | Out-Null
    foreach ($name in $files.Keys) {
        $dest = Join-Path $Models $name
        if ((Test-Path $dest) -and (Get-FileHash $dest -Algorithm SHA256).Hash.ToLower() -eq $files[$name]) { continue }
        Say "models: downloading $name"
        $part = "$dest.part"
        $ErrorActionPreference = 'Continue'
        & curl.exe -L --fail --retry 5 --retry-delay 5 --no-progress-meter -C - -o $part "https://huggingface.co/Kijai/LivePortrait_safetensors/resolve/main/$name" 2>&1 | Out-Host
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($code -ne 0) { throw "downloading $name failed (curl $code)" }
        if ((Get-FileHash $part -Algorithm SHA256).Hash.ToLower() -ne $files[$name]) { Remove-Item $part; throw "$name did not match its checksum" }
        Move-Item -Force $part $dest
    }
    Say "models: in $Models"
    Say 'ok'
} catch {
    Say "error: $($_.Exception.Message)"
    exit 1
}
