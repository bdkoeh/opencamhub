#include "Matting.h"
#include <onnxruntime_c_api.h>
#include <dml_provider_factory.h>
#include <cmath>

namespace
{
    std::wstring Widen(const char* s)
    {
        if (!s) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
        return w;
    }
}

Matting::~Matting() { Release(); }

bool Matting::Check(void* status, std::wstring* error)
{
    if (!status) return true;
    OrtStatus* s = (OrtStatus*)status;
    if (error) *error = Widen(m_api->GetErrorMessage(s));
    m_api->ReleaseStatus(s);
    return false;
}

void Matting::Release()
{
    if (m_api)
    {
        for (auto& r : m_rec) if (r) { m_api->ReleaseValue(r); r = nullptr; }
        if (m_session) m_api->ReleaseSession(m_session);
        if (m_mem) m_api->ReleaseMemoryInfo(m_mem);
        if (m_env) m_api->ReleaseEnv(m_env);
    }
    m_session = nullptr;
    m_mem = nullptr;
    m_env = nullptr;
    // The DLLs stay loaded: unloading ONNX Runtime while its threads wind down is not safe.
}

bool Matting::Load(const std::wstring& dir, const std::wstring& modelPath, UINT inWidth, UINT inHeight, std::wstring* error)
{
    Release();
    // Load DirectML first from our folder, so ONNX Runtime uses this copy and not an older one in System32.
    if (!m_dml) m_dml = LoadLibraryExW((dir + L"\\DirectML.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m_ort) m_ort = LoadLibraryExW((dir + L"\\onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m_ort) { if (error) *error = L"onnxruntime.dll not found in " + dir; return false; }
    typedef const OrtApiBase*(ORT_API_CALL * GetApiBaseFn)();
    auto getBase = (GetApiBaseFn)GetProcAddress(m_ort, "OrtGetApiBase");
    m_api = getBase ? getBase()->GetApi(ORT_API_VERSION) : nullptr;
    if (!m_api) { if (error) *error = L"onnxruntime.dll is not a compatible version"; return false; }

    if (!Check(m_api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "OpenCamHub", &m_env), error)) return false;
    m_api->DisableTelemetryEvents(m_env);

    OrtSessionOptions* opts = nullptr;
    if (!Check(m_api->CreateSessionOptions(&opts), error)) return false;
    m_api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);
    m_api->SetIntraOpNumThreads(opts, 2);
    // Don't let idle worker threads spin waiting for work; the GPU does the heavy lifting.
    m_api->AddSessionConfigEntry(opts, "session.intra_op.allow_spinning", "0");
    m_api->AddSessionConfigEntry(opts, "session.inter_op.allow_spinning", "0");
    // DirectML needs these two settings.
    m_api->DisableMemPattern(opts);
    m_api->SetSessionExecutionMode(opts, ORT_SEQUENTIAL);

    const OrtDmlApi* dml = nullptr;
    m_gpu = m_dml && !m_api->GetExecutionProviderApi("DML", ORT_API_VERSION, (const void**)&dml) && dml &&
            Check(dml->SessionOptionsAppendExecutionProvider_DML(opts, 0), nullptr);

    bool ok = Check(m_api->CreateSession(m_env, modelPath.c_str(), opts, &m_session), error);
    m_api->ReleaseSessionOptions(opts);
    if (!ok) { Release(); return false; }
    if (!Check(m_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m_mem), error)) { Release(); return false; }

    m_inW = inWidth;
    m_inH = inHeight;
    // RVM works best when its internal pass sees about 256-512 pixels on the long side.
    m_ratio = 384.f / (float)(inWidth > inHeight ? inWidth : inHeight);
    if (m_ratio > 1.f) m_ratio = 1.f;
    m_src.assign((size_t)3 * m_inW * m_inH, 0.f);
    m_alpha.assign((size_t)m_inW * m_inH, 0);
    ResetHistory();
    return true;
}

void Matting::ResetHistory()
{
    for (auto& r : m_rec) if (r) { m_api->ReleaseValue(r); r = nullptr; }
}

bool Matting::Run(const BYTE* yPlane, LONG yPitch, const BYTE* uvPlane, LONG uvPitch, UINT width, UINT height, YuvFormat format)
{
    if (!m_session || width < 2 || height < 2) return false;

    // Scale the frame to the model's input size and convert to planar RGB floats.
    int ys, rv, gu, gv, bu, yOff;
    if (format.fullRange) { ys = 1024; yOff = 0; } else { ys = 1192; yOff = 16; }
    if (format.bt709) { if (format.fullRange) { rv = 1613; gu = -192; gv = -479; bu = 1900; } else { rv = 1836; gu = -218; gv = -546; bu = 2163; } }
    else { if (format.fullRange) { rv = 1436; gu = -352; gv = -731; bu = 1815; } else { rv = 1634; gu = -401; gv = -832; bu = 2066; } }
    const size_t plane = (size_t)m_inW * m_inH;
    float* R = m_src.data();
    float* G = R + plane;
    float* B = G + plane;
    const float k = 1.f / (255.f * 1024.f);
    ParallelFor((int)m_inH, [&](int row) {
        UINT oy = (UINT)row;
        UINT sy = (UINT)(((uint64_t)oy * 2 + 1) * height / (2 * m_inH));
        const BYTE* yr = yPlane + (size_t)sy * yPitch;
        const BYTE* uvr = uvPlane + (size_t)(sy / 2) * uvPitch;
        size_t o = (size_t)oy * m_inW;
        for (UINT ox = 0; ox < m_inW; ox++, o++)
        {
            UINT sx = (UINT)(((uint64_t)ox * 2 + 1) * width / (2 * m_inW));
            int c = ((int)yr[sx] - yOff) * ys;
            int u = (int)uvr[sx & ~1u] - 128, v = (int)uvr[(sx & ~1u) + 1] - 128;
            auto clampf = [](int x) { return (float)(x < 0 ? 0 : (x > 255 * 1024 ? 255 * 1024 : x)); };
            R[o] = clampf(c + rv * v) * k;
            G[o] = clampf(c + gu * u + gv * v) * k;
            B[o] = clampf(c + bu * u) * k;
        }
    });
    // Inputs: src, r1i..r4i (zeros on the first frame), downsample_ratio.
    int64_t srcShape[4] = { 1, 3, (int64_t)m_inH, (int64_t)m_inW };
    int64_t zeroShape[4] = { 1, 1, 1, 1 };
    int64_t ratioShape[1] = { 1 };
    float zero = 0.f, ratio = m_ratio;
    OrtValue* src = nullptr;
    OrtValue* ratioValue = nullptr;
    OrtValue* zeros[4] = {};
    bool ok = Check(m_api->CreateTensorWithDataAsOrtValue(m_mem, m_src.data(), m_src.size() * sizeof(float), srcShape, 4,
                        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &src), nullptr) &&
              Check(m_api->CreateTensorWithDataAsOrtValue(m_mem, &ratio, sizeof(float), ratioShape, 1,
                        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &ratioValue), nullptr);
    for (int i = 0; ok && i < 4; i++)
        if (!m_rec[i])
            ok = Check(m_api->CreateTensorWithDataAsOrtValue(m_mem, &zero, sizeof(float), zeroShape, 4,
                           ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &zeros[i]), nullptr);

    const char* inNames[] = { "src", "r1i", "r2i", "r3i", "r4i", "downsample_ratio" };
    const char* outNames[] = { "pha", "r1o", "r2o", "r3o", "r4o" };
    OrtValue* outputs[5] = {};
    if (ok)
    {
        const OrtValue* inputs[6] = { src, m_rec[0] ? m_rec[0] : zeros[0], m_rec[1] ? m_rec[1] : zeros[1],
                                      m_rec[2] ? m_rec[2] : zeros[2], m_rec[3] ? m_rec[3] : zeros[3], ratioValue };
        ok = Check(m_api->Run(m_session, nullptr, inNames, inputs, 6, outNames, 5, outputs), nullptr);
    }
    if (src) m_api->ReleaseValue(src);
    if (ratioValue) m_api->ReleaseValue(ratioValue);
    for (auto z : zeros) if (z) m_api->ReleaseValue(z);
    if (!ok) { ResetHistory(); return false; }

    float* pha = nullptr;
    m_api->GetTensorMutableData(outputs[0], (void**)&pha);
    if (pha)
        for (size_t i = 0; i < plane; i++)
        {
            float a = pha[i] * 255.f + 0.5f;
            m_alpha[i] = (uint8_t)(a < 0.f ? 0 : (a > 255.f ? 255 : (int)a));
        }
    m_api->ReleaseValue(outputs[0]);
    for (int i = 0; i < 4; i++)
    {
        if (m_rec[i]) m_api->ReleaseValue(m_rec[i]);
        m_rec[i] = outputs[i + 1];
    }
    return pha != nullptr;
}
