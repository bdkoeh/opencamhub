// Person matting with Robust Video Matting (RVM, github.com/PeterL1n/RobustVideoMatting, GPL-3.0),
// run by ONNX Runtime on the GPU through DirectML. Given a camera frame it produces an alpha matte:
// 255 where the person is, 0 for background, soft values along hair and edges.
//
// ONNX Runtime is loaded from OpenCamHub's own folder at run time, because the virtual camera runs
// inside Windows' camera service, whose DLL search path doesn't include our folder.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include "../vcam/Lut.h"

struct OrtApi;
struct OrtEnv;
struct OrtSession;
struct OrtValue;
struct OrtMemoryInfo;

class Matting
{
public:
    ~Matting();
    // dir holds onnxruntime.dll and DirectML.dll; modelPath is an RVM .onnx file.
    // inWidth/inHeight is the size frames are scaled to before matting.
    bool Load(const std::wstring& dir, const std::wstring& modelPath, UINT inWidth, UINT inHeight, std::wstring* error);
    bool Loaded() const { return m_session != nullptr; }
    bool OnGpu() const { return m_gpu; }

    // Mattes one NV12 frame. On success Alpha() holds AlphaWidth() x AlphaHeight() bytes.
    bool Run(const BYTE* y, LONG yPitch, const BYTE* uv, LONG uvPitch, UINT width, UINT height, YuvFormat format);
    const std::vector<uint8_t>& Alpha() const { return m_alpha; }
    UINT AlphaWidth() const { return m_inW; }
    UINT AlphaHeight() const { return m_inH; }
    void ResetHistory();   // forget earlier frames (after a cut or a format change)

private:
    void Release();
    bool Check(void* status, std::wstring* error);

    HMODULE m_ort = nullptr, m_dml = nullptr;
    const OrtApi* m_api = nullptr;
    OrtEnv* m_env = nullptr;
    OrtSession* m_session = nullptr;
    OrtMemoryInfo* m_mem = nullptr;
    OrtValue* m_rec[4] = {};        // recurrent state carried between frames
    bool m_gpu = false;
    UINT m_inW = 0, m_inH = 0;
    float m_ratio = 0.4f;
    std::vector<float> m_src;       // 1 x 3 x H x W, RGB 0..1
    std::vector<uint8_t> m_alpha;
};
