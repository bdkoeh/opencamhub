# Downloads the third-party pieces OpenCamHub needs to build into .\third_party, checking each
# file against a pinned SHA-256 hash. Nothing is downloaded at run time; this is a build step only.
#
#   ONNX Runtime 1.24.4 with DirectML (MIT)          - runs the matting model on the GPU
#   DirectML 1.15.4 (Microsoft DirectML licence)      - GPU backend for ONNX Runtime
#   Robust Video Matting ResNet50 fp32 (GPL-3.0)       - the background matting model
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$tp = Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party'

$deps = @(
    @{ Name = 'ONNX Runtime 1.24.4 (DirectML)'
       Url  = 'https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg'
       File = 'ort\ort.zip'; Extract = 'ort\pkg'
       Sha256 = '57E9F11B73437BEF7A309496135D4C1F96B1A8E9DDBA60013FA27BFC1D788681' }
    @{ Name = 'DirectML 1.15.4'
       Url  = 'https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg'
       File = 'directml\directml.zip'; Extract = 'directml\pkg'
       Sha256 = '4E7CB7DDCE8CF837A7A75DC029209B520CA0101470FCDF275C1F49736A3615B9' }
    @{ Name = 'Robust Video Matting (ResNet50, fp32)'
       Url  = 'https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_resnet50_fp32.onnx'
       File = 'rvm\rvm_resnet50_fp32.onnx'
       Sha256 = '25DB300FCB6EE27F941A1B52C97856E8D1F13C7F35817F81A612F89AF0E8A85C' }
)

foreach ($d in $deps) {
    $path = Join-Path $tp $d.File
    New-Item -ItemType Directory -Force (Split-Path $path -Parent) | Out-Null
    $ok = (Test-Path $path) -and ((Get-FileHash $path -Algorithm SHA256).Hash -eq $d.Sha256)
    if (-not $ok) {
        Write-Host "Downloading $($d.Name)..."
        Invoke-WebRequest $d.Url -OutFile $path -UseBasicParsing
        $hash = (Get-FileHash $path -Algorithm SHA256).Hash
        if ($hash -ne $d.Sha256) {
            Remove-Item $path -Force
            throw "$($d.Name): SHA-256 mismatch (got $hash, expected $($d.Sha256))"
        }
    } else {
        Write-Host "$($d.Name): already downloaded"
    }
    if ($d.Extract) {
        $dir = Join-Path $tp $d.Extract
        if (-not (Test-Path $dir) -or $ok -eq $false) {
            if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
            Expand-Archive $path $dir -Force
        }
    }
}
Write-Host "Dependencies ready in $tp"
