# Third-party notices

OpenCamHub is licensed under the GNU GPL v3.0 (see [LICENSE](LICENSE)). It uses or is based on the
following. Components marked *downloaded* are not in this repository: `scripts/fetch-deps.ps1`
downloads them at build time, and the build copies them next to the program.

## Robust Video Matting (*downloaded*)

The background matting model, `rvm_resnet50_fp32.onnx`, from
<https://github.com/PeterL1n/RobustVideoMatting> (release v1.0.0).
Copyright (c) Shanchuan Lin and contributors. Licensed under the GNU General Public License v3.0.

## ONNX Runtime (*downloaded*)

`onnxruntime.dll` from the Microsoft.ML.OnnxRuntime.DirectML 1.24.4 NuGet package,
<https://github.com/microsoft/onnxruntime>. Copyright (c) Microsoft Corporation. MIT License.
Its own third-party notices are in the package (`ThirdPartyNotices.txt`).

## DirectML (*downloaded*)

`DirectML.dll` from the Microsoft.AI.DirectML 1.15.4 NuGet package,
<https://github.com/microsoft/DirectML>. Copyright (c) Microsoft Corporation. The redistributable
binary is licensed under Microsoft's DirectML license terms (`LICENSE.txt` in the package), not
under the GPL. OpenCamHub loads it at run time as a system component for GPU access.

## Windows-Camera VirtualCamera sample

The structure of the virtual camera media source (`src/vcam`) follows Microsoft's VirtualCamera
sample (AugmentedMediaSource), <https://github.com/microsoft/Windows-Camera>.

```
MIT License

Copyright (c) Microsoft Corporation.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE
```

## Facecam MK.2 extension-unit research

The Elgato extension-unit GUIDs and selectors (metering, vibrancy, tint, HDR, noise reduction,
save to camera) were first documented by the "facecam" project,
<https://github.com/nklmilojevic/facecam>, and confirmed on this camera. No code was copied from it.

## Not included

Elgato's own LUT files are not part of OpenCamHub. If Camera Hub is installed, `install.ps1` copies
the `.cube` files already on your PC into OpenCamHub's LUT folder for your own use.
