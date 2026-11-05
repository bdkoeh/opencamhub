#include "Preview.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <wrl.h>
#include "../common/OpenCamHub.h"
#include "../common/Effects.h"

using Microsoft::WRL::ComPtr;

namespace
{
    // Debug log, only written when the OPENCAMHUB_PREVIEW_LOG environment variable names a file.
    void Trace(const wchar_t* fmt, ...)
    {
        static wchar_t path[MAX_PATH] = {};
        static bool checked = false;
        if (!checked) { checked = true; GetEnvironmentVariableW(L"OPENCAMHUB_PREVIEW_LOG", path, MAX_PATH); }
        if (!path[0]) return;
        wchar_t buf[512];
        va_list a;
        va_start(a, fmt);
        _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, a);
        va_end(a);
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"a, ccs=UTF-8") == 0 && f) { fwprintf(f, L"%lu %s\n", GetTickCount(), buf); fclose(f); }
    }

    // Opens a camera in shared mode: OpenCamHub by name, or the Facecam by hardware id.
    ComPtr<IMFMediaSource> OpenShared(bool facecam)
    {
        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(&attrs, 1);
        attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        ComPtr<IMFMediaSource> source;
        if (FAILED(MFEnumDeviceSources(attrs.Get(), &devices, &count))) return source;
        for (UINT32 i = 0; i < count; i++)
        {
            if (!source)
            {
                wchar_t* s = nullptr;
                UINT32 n = 0;
                bool match = false;
                if (facecam && SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &s, &n)))
                {
                    std::wstring link(s);
                    for (auto& c : link) c = towlower(c);
                    match = link.find(FACECAM_HWID) != std::wstring::npos;
                    CoTaskMemFree(s);
                }
                else if (!facecam && SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &s, &n)))
                {
                    match = std::wstring(s).rfind(OPENCAMHUB_FRIENDLY_NAME, 0) == 0;
                    CoTaskMemFree(s);
                }
                if (match)
                {
                    devices[i]->SetUINT32(MF_DEVSOURCE_ATTRIBUTE_FRAMESERVER_SHARE_MODE, 1);
                    devices[i]->ActivateObject(IID_PPV_ARGS(&source));
                }
            }
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
        return source;
    }
}

// True if OpenCamHub can deliver a frame right now.
static bool OpenCamHubAvailable()
{
    ComPtr<IMFMediaSource> source = OpenShared(false);
    if (!source) return false;
    ComPtr<IMFSourceReader> reader;
    bool ok = false;
    if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader)))
    {
        for (int i = 0; i < 10 && !ok; i++)
        {
            DWORD stream, flags = 0;
            LONGLONG ts;
            ComPtr<IMFSample> sample;
            if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample))) break;
            if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) break;
            ok = sample != nullptr;
        }
    }
    reader.Reset();
    source->Shutdown();
    return ok;
}

void Preview::Start(HWND notify, UINT msg, UINT32 targetWidth)
{
    if (Running()) return;
    m_notify = notify;
    m_msg = msg;
    m_target = targetWidth;
    m_stop = false;
    SetState(Mode::Starting, L"Starting preview...");
    m_thread = std::thread([this] { Run(); });
}

void Preview::Stop()
{
    if (!Running()) return;
    m_stop = true;
    m_thread.join();
    AcquireSRWLockExclusive(&m_lock);
    m_frame.clear();
    m_w = m_h = 0;
    m_seq++;
    m_status = L"Preview off";
    ReleaseSRWLockExclusive(&m_lock);
    m_mode = Mode::Off;
}

void Preview::SetState(Mode m, const std::wstring& status)
{
    AcquireSRWLockExclusive(&m_lock);
    bool changed = m_status != status || m_mode != m;
    m_status = status;
    m_mode = m;
    if (m == Mode::Unavailable) { m_frame.clear(); m_w = m_h = 0; m_seq++; }
    ReleaseSRWLockExclusive(&m_lock);
    if (changed && m_notify) PostMessageW(m_notify, m_msg, 0, 0);
}

std::wstring Preview::Status()
{
    AcquireSRWLockShared(&m_lock);
    std::wstring s = m_status;
    ReleaseSRWLockShared(&m_lock);
    return s;
}

void Preview::Wait(int ms)
{
    for (int t = 0; t < ms && !m_stop; t += 50) Sleep(50);
}

void Preview::Run()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!m_stop)
    {
        Result r = Stream(false);
        if (r == Result::Stopped) break;
        if (r == Result::Busy)
        {
            // Another app has the Facecam itself, so OpenCamHub can't start. Show the camera directly.
            r = Stream(true);
            if (r == Result::Stopped) break;
        }
        if (r == Result::Failed || r == Result::Busy)
            SetState(Mode::Unavailable, L"Camera not available. Is the Facecam plugged in?");
        Wait(r == Result::Ended ? 500 : 2000);
    }
    CoUninitialize();
}

Preview::Result Preview::Stream(bool direct)
{
    ComPtr<IMFMediaSource> source = OpenShared(direct);
    Trace(L"open %s: %s", direct ? L"facecam" : L"opencamhub", source ? L"ok" : L"failed");
    if (!source) return Result::Failed;

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);   // decodes MJPG/YUY2 if another app chose those
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromMediaSource(source.Get(), attrs.Get(), &reader);
    if (SUCCEEDED(hr))
    {
        ComPtr<IMFMediaType> nv12;
        MFCreateMediaType(&nv12);
        nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nv12.Get());
    }
    UINT32 w = 0, h = 0, matrix = 0, range = 0;
    if (SUCCEEDED(hr))
    {
        ComPtr<IMFMediaType> current;
        reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current);
        MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h);
        current->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
        current->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range);
    }
    Trace(L"setup hr=0x%08X %ux%u matrix %u range %u", hr, w, h, matrix, range);
    if (FAILED(hr) || w < 2 || h < 2)
    {
        source->Shutdown();
        return Result::Failed;
    }
    YuvFormat format;
    format.bt709 = matrix != MFVideoTransferMatrix_BT601;
    format.fullRange = range == MFNominalRange_0_255;

    FrameEffects effects;   // only used when showing the Facecam directly
    std::vector<BYTE> bgra, copy;
    int framesSinceProbe = 0;
    bool gotFrame = false;
    Result result = Result::Ended;
    while (!m_stop)
    {
        DWORD stream, flags = 0;
        LONGLONG ts;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample);
        if (!gotFrame) Trace(L"read hr=0x%08X flags=0x%X sample=%d", hr, flags, sample ? 1 : 0);
        if (FAILED(hr))
        {
            result = !gotFrame && hr == MF_E_HW_MFT_FAILED_START_STREAMING ? Result::Busy : (gotFrame ? Result::Ended : Result::Failed);
            break;
        }
        if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->GetBufferByIndex(0, &buffer))) continue;

        BYTE* y = nullptr;
        BYTE* uv = nullptr;
        LONG pitch = 0;
        ComPtr<IMF2DBuffer2> buffer2d;
        DWORD maxLen = 0, len = 0;
        bool locked2d = false, locked = false;
        if (SUCCEEDED(buffer.As(&buffer2d)))
        {
            BYTE* start = nullptr;
            DWORD length = 0;
            // Shared-mode frames are read-only.
            if (SUCCEEDED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &y, &pitch, &start, &length)))
            {
                locked2d = true;
                size_t available = length - (size_t)(y - start);
                UINT rows = pitch > 0 ? (UINT)(available * 2 / (3 * (size_t)pitch)) : 0;
                if (rows < h) rows = h;
                if (pitch > 0 && (size_t)pitch * rows + (size_t)pitch * (h / 2) <= available) uv = y + (size_t)pitch * rows;
            }
        }
        if (!locked2d && SUCCEEDED(buffer->Lock(&y, &maxLen, &len)))
        {
            locked = true;
            pitch = (LONG)w;
            if (len >= (DWORD)w * h * 3 / 2) uv = y + (size_t)w * h;
        }
        if (uv)
        {
            if (direct)
            {
                // Copy the frame so the mirror and LUT can be applied to it here.
                copy.resize((size_t)w * h * 3 / 2);
                for (UINT32 r = 0; r < h; r++) memcpy(&copy[(size_t)r * w], y + (size_t)r * pitch, w);
                for (UINT32 r = 0; r < h / 2; r++) memcpy(&copy[(size_t)w * h + (size_t)r * w], uv + (size_t)r * pitch, w);
                y = copy.data();
                uv = copy.data() + (size_t)w * h;
                pitch = (LONG)w;
                effects.Apply(y, pitch, uv, pitch, w, h, format);
            }
            UINT32 target = m_target;
            UINT step = target ? (w + target / 2) / target : 1;
            if (step < 1) step = 1;
            UINT32 ow = w / step, oh = h / step;
            bgra.resize((size_t)ow * oh * 4);
            NV12ToBGRA(y, pitch, uv, pitch, w, h, step, format, bgra.data());
            AcquireSRWLockExclusive(&m_lock);
            m_frame.swap(bgra);
            m_w = ow;
            m_h = oh;
            m_seq++;
            ReleaseSRWLockExclusive(&m_lock);
        }
        if (!gotFrame) Trace(L"buffer 2d=%d locked2d=%d locked=%d pitch=%ld uv=%d", buffer2d ? 1 : 0, locked2d, locked, pitch, uv ? 1 : 0);
        if (locked2d) buffer2d->Unlock2D();
        if (locked) buffer->Unlock();
        if (!uv) continue;

        if (!gotFrame)
        {
            gotFrame = true;
            wchar_t text[200];
            if (direct)
                swprintf_s(text, L"Another app is using the Facecam directly, not OpenCamHub");
            else
                swprintf_s(text, L"Live  \x00B7  %ux%u", w, h);
            SetState(direct ? Mode::Direct : Mode::OpenCamHub, text);
        }
        // Only queue a repaint if the last one has been drawn.
        if (!m_pending.exchange(true)) PostMessageW(m_notify, m_msg, 0, 0);

        // While showing the Facecam directly, check every few seconds whether OpenCamHub works again
        // (the other app may have let go of the camera), and switch back if so.
        if (direct && ++framesSinceProbe >= 150)
        {
            framesSinceProbe = 0;
            if (OpenCamHubAvailable()) { Trace(L"OpenCamHub available again"); break; }
        }
    }
    reader.Reset();
    source->Shutdown();
    return m_stop ? Result::Stopped : result;
}
