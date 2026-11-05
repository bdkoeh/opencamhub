# Installs or updates OpenCamHub. Run as administrator:
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-LutSource <folder with .cube files>]
#
# If Elgato Camera Hub's LUTs are on this PC they are copied into OpenCamHub's LUT folder, so you can
# keep using them. (They are Elgato's files, so they are not part of this project.)
param([string]$LutSource = '', [string]$Log = '')
$ErrorActionPreference = 'Stop'
if ($Log) { Start-Transcript -Path $Log -Force | Out-Null }
trap { Write-Host "ERROR: $_"; if ($Log) { Stop-Transcript | Out-Null }; exit 1 }

$src = Join-Path $PSScriptRoot 'out'
$dest = "$env:ProgramFiles\OpenCamHub"
$data = "$env:ProgramData\OpenCamHub"
if (-not (Test-Path "$src\vcam.dll")) { throw 'Nothing to install: run build.ps1 first.' }

# The camera service keeps vcam.dll loaded; stop it so the file can be replaced (it restarts on demand).
foreach ($svc in 'FrameServer', 'FrameServerMonitor') {
    $s = Get-Service $svc -ErrorAction SilentlyContinue
    if ($s -and $s.Status -eq 'Running') { Stop-Service $svc -Force }
}
Get-Process OpenCamHub, LocalCam -ErrorAction SilentlyContinue | Stop-Process -Force

# Early development builds were called LocalCam. Remove that camera and carry its settings over.
$old = "$env:ProgramFiles\LocalCam"
if (Test-Path "$old\localcam-setup.exe") { & "$old\localcam-setup.exe" --unregister | Out-Null }
Remove-Item 'HKLM:\SOFTWARE\Classes\CLSID\{3B1F6C2A-8E4D-4A57-9C21-5D0E7A6B9F14}' -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\LocalCam.lnk" -Force -ErrorAction SilentlyContinue
$oldData = "$env:ProgramData\LocalCam"
if ((Test-Path $oldData) -and -not (Test-Path "$data\settings.ini")) {
    New-Item -ItemType Directory -Force "$data\luts" | Out-Null
    if (Test-Path "$oldData\settings.ini") { Copy-Item "$oldData\settings.ini" $data }
    if (Test-Path "$oldData\luts") { Copy-Item "$oldData\luts\*.cube" "$data\luts" -ErrorAction SilentlyContinue }
}

New-Item -ItemType Directory -Force $dest | Out-Null
foreach ($f in 'vcam.dll', 'opencamhub-setup.exe', 'OpenCamHub.exe', 'onnxruntime.dll', 'DirectML.dll', 'rvm_resnet50_fp32.onnx') {
    if (Test-Path "$src\$f") { Copy-Item "$src\$f" $dest -Force }
}

# Register the media source so the camera service can load it (what regsvr32 would do).
$key = 'HKLM:\SOFTWARE\Classes\CLSID\{5B2B95F8-965B-4FC7-8A72-0ED88649C542}\InprocServer32'
New-Item -Path $key -Force | Out-Null
Set-ItemProperty -Path $key -Name '(default)' -Value "$dest\vcam.dll"
Set-ItemProperty -Path $key -Name 'ThreadingModel' -Value 'Both'

# Settings folder: you can edit it, the camera service can read it.
New-Item -ItemType Directory -Force "$data\luts" | Out-Null
& icacls.exe $data /grant '*S-1-5-32-545:(OI)(CI)M' '*S-1-5-19:(OI)(CI)RX' /T /Q | Out-Null

$hub = "$env:APPDATA\Elgato\CameraHub"
$lutSources = @($LutSource, "$hub\Preinstalled\LUTs", "$hub\Custom\LUTs", "$hub\Marketplace\LUTs") |
    Where-Object { $_ -and (Test-Path $_) }
foreach ($dir in $lutSources) {
    Get-ChildItem $dir -Filter *.cube | ForEach-Object {
        if (-not (Test-Path "$data\luts\$($_.Name)")) { Copy-Item $_.FullName "$data\luts" }
    }
}
if (-not (Test-Path "$data\settings.ini")) {
    "[lut]`r`nenabled=0`r`nfile=`r`nintensity=100`r`n" | Set-Content "$data\settings.ini" -Encoding Unicode
}

# Start menu shortcut for the settings window.
if (Test-Path "$dest\OpenCamHub.exe") {
    $lnk = (New-Object -ComObject WScript.Shell).CreateShortcut("$env:ProgramData\Microsoft\Windows\Start Menu\Programs\OpenCamHub.lnk")
    $lnk.TargetPath = "$dest\OpenCamHub.exe"
    $lnk.Description = 'OpenCamHub camera settings'
    $lnk.Save()
}

& "$dest\opencamhub-setup.exe" --register
if ($LASTEXITCODE -ne 0) { throw 'registering the virtual camera failed' }
if (Test-Path "$old\vcam.dll") { Remove-Item $old -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host 'OpenCamHub installed.'
if ($Log) { Stop-Transcript | Out-Null }
