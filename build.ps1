# Builds OpenCamHub into .\out with the Microsoft C++ Build Tools.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'out'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'C++ Build Tools not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

# Third-party pieces (ONNX Runtime, DirectML, the RVM model) come from scripts\fetch-deps.ps1.
$tp = "$root\third_party"
$needed = "$tp\ort\pkg\build\native\include\onnxruntime_c_api.h", "$tp\ort\pkg\runtimes\win-x64\native\onnxruntime.dll", "$tp\directml\pkg\bin\x64-win\DirectML.dll", "$tp\rvm\rvm_resnet50_fp32.onnx"
if ($needed | Where-Object { -not (Test-Path $_) }) {
    Write-Host 'Third-party dependencies missing; fetching them...'
    & "$root\scripts\fetch-deps.ps1"
}

New-Item -ItemType Directory -Force "$out\obj\vcam", "$out\obj\tools", "$out\obj\app" | Out-Null

$ortInc = "$root\third_party\ort\pkg\build\native\include"
$common = '/nologo /std:c++20 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /DNOMINMAX' + " /I`"$ortInc`""
$steps = @(
    "cl $common /LD /Fo`"$out\obj\vcam\\`" `"$root\src\vcam\*.cpp`" `"$root\src\common\Effects.cpp`" `"$root\src\common\Matting.cpp`" `"$root\src\common\Background.cpp`" /Fe:`"$out\vcam.dll`" /link /DEF:`"$root\src\vcam\vcam.def`" mfplat.lib mf.lib mfuuid.lib mfsensorgroup.lib ole32.lib advapi32.lib"
    "cl $common /Fo`"$out\obj\tools\\`" `"$root\src\tools\opencamhub-setup.cpp`" `"$root\src\vcam\Lut.cpp`" `"$root\src\common\Matting.cpp`" `"$root\src\common\Background.cpp`" /Fe:`"$out\opencamhub-setup.exe`" /link mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib mfsensorgroup.lib ole32.lib"
)
if (Test-Path "$root\src\app") {
    $steps += "rc /nologo /fo `"$out\obj\app\app.res`" `"$root\src\app\app.rc`""
    $steps += "cl $common /Fo`"$out\obj\app\\`" `"$root\src\app\*.cpp`" `"$root\src\vcam\Lut.cpp`" `"$root\src\common\Effects.cpp`" `"$root\src\common\Matting.cpp`" `"$root\src\common\Background.cpp`" `"$out\obj\app\app.res`" /Fe:`"$out\OpenCamHub.exe`" /link /SUBSYSTEM:WINDOWS mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib ole32.lib oleaut32.lib strmiids.lib user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib advapi32.lib uxtheme.lib dwmapi.lib d2d1.lib dwrite.lib"
}

# Turn off the build tools' telemetry for this build.
$env:VSCMD_SKIP_SENDTELEMETRY = '1'
$cmd = "`"$vcvars`" >nul && " + ($steps -join ' && ')
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
# Runtime pieces for background blur: ONNX Runtime, DirectML and the RVM model.
Copy-Item "$tp\ort\pkg\runtimes\win-x64\native\onnxruntime.dll", "$tp\directml\pkg\bin\x64-win\DirectML.dll" $out -Force
Copy-Item "$tp\rvm\rvm_resnet50_fp32.onnx" $out -Force
Write-Host "Built into $out"
