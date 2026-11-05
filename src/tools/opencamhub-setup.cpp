// opencamhub-setup: registers/removes the OpenCamHub virtual camera and helps test it.
//
//   --register                         create the virtual camera (administrator; install.ps1 does this)
//   --unregister                       remove the virtual camera
//   --list                             list cameras
//   --formats <camera>                 list a camera's formats
//   --grab <camera> <out.bmp> [frames] save one frame as a bitmap
//   --bench <camera> [fps] [seconds]   stream 1920x1080 NV12 and report the frame rate
//   --lutbench <file.cube> [intensity] time LUT loading and processing
//   --mattebench <dir> <model.onnx> <w> <h> [frames]
//                                      time background matting on live Facecam frames
//   --shared                           (with --grab/--bench) open the camera in shared mode, so it
//                                      works while another app is using it
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mfvirtualcamera.h>
#include <wrl.h>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include "../common/OpenCamHub.h"
#include "../vcam/Lut.h"
#include "../common/Matting.h"
#include "../common/Background.h"

using Microsoft::WRL::ComPtr;

static bool g_shared = false;   // --shared

struct Camera { std::wstring name, link; ComPtr<IMFActivate> activate; };

static std::vector<Camera> ListCameras()
{
    std::vector<Camera> result;
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &devices, &count))) return result;
    for (UINT32 i = 0; i < count; i++)
    {
        Camera c;
        wchar_t* s = nullptr;
        UINT32 n = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &s, &n))) { c.name = s; CoTaskMemFree(s); }
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &s, &n))) { c.link = s; CoTaskMemFree(s); }
        c.activate.Attach(devices[i]);
        result.push_back(c);
    }
    CoTaskMemFree(devices);
    return result;
}

static std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

static Camera* Find(std::vector<Camera>& cams, const std::wstring& name)
{
    for (auto& c : cams) if (Lower(c.name) == Lower(name)) return &c;
    for (auto& c : cams) if (Lower(c.name).find(Lower(name)) != std::wstring::npos) return &c;
    return nullptr;
}

static Camera* FindFacecam(std::vector<Camera>& cams)
{
    for (auto& c : cams) if (Lower(c.link).find(FACECAM_HWID) != std::wstring::npos) return &c;
    return nullptr;
}

static HRESULT CreateVCam(IMFVirtualCamera** vcam)
{
    return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
        MFVirtualCameraAccess_AllUsers, OPENCAMHUB_FRIENDLY_NAME, OPENCAMHUB_CLSID_STR, nullptr, 0, vcam);
}

static int Register()
{
    auto cams = ListCameras();
    Camera* facecam = FindFacecam(cams);
    if (!facecam) { wprintf(L"Facecam MK.2 not found.\n"); return 1; }
    ComPtr<IMFVirtualCamera> vcam;
    HRESULT hr = CreateVCam(&vcam);
    if (FAILED(hr)) { wprintf(L"MFCreateVirtualCamera failed 0x%08X\n", hr); return 1; }
    hr = vcam->AddDeviceSourceInfo(facecam->link.c_str());
    if (FAILED(hr)) { wprintf(L"AddDeviceSourceInfo failed 0x%08X\n", hr); return 1; }
    hr = vcam->Start(nullptr);
    if (FAILED(hr)) { wprintf(L"Start failed 0x%08X\n", hr); return 1; }
    wprintf(L"OpenCamHub registered, wrapping %s\n", facecam->name.c_str());
    return 0;
}

static int Unregister()
{
    ComPtr<IMFVirtualCamera> vcam;
    HRESULT hr = CreateVCam(&vcam);
    if (FAILED(hr)) { wprintf(L"MFCreateVirtualCamera failed 0x%08X\n", hr); return 1; }
    hr = vcam->Remove();
    if (FAILED(hr)) { wprintf(L"Remove failed 0x%08X\n", hr); return 1; }
    wprintf(L"OpenCamHub removed\n");
    return 0;
}

static std::wstring SubtypeName(const GUID& g)
{
    if (g == MFVideoFormat_NV12) return L"NV12";
    if (g == MFVideoFormat_MJPG) return L"MJPG";
    if (g == MFVideoFormat_YUY2) return L"YUY2";
    if (g == MFVideoFormat_RGB32) return L"RGB32";
    if (g == MFVideoFormat_H264) return L"H264";
    wchar_t buf[64];
    swprintf_s(buf, L"%c%c%c%c", (wchar_t)(g.Data1 & 0xFF), (wchar_t)((g.Data1 >> 8) & 0xFF), (wchar_t)((g.Data1 >> 16) & 0xFF), (wchar_t)(g.Data1 >> 24));
    return buf;
}

static int Formats(const std::wstring& name)
{
    auto cams = ListCameras();
    Camera* c = Find(cams, name);
    if (!c) { wprintf(L"camera not found\n"); return 1; }
    ComPtr<IMFMediaSource> source;
    HRESULT hr = c->activate->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(hr)) { wprintf(L"open failed 0x%08X\n", hr); return 1; }
    ComPtr<IMFPresentationDescriptor> pd;
    source->CreatePresentationDescriptor(&pd);
    DWORD streams = 0;
    pd->GetStreamDescriptorCount(&streams);
    for (DWORD s = 0; s < streams; s++)
    {
        BOOL sel; ComPtr<IMFStreamDescriptor> sd; ComPtr<IMFMediaTypeHandler> h;
        pd->GetStreamDescriptorByIndex(s, &sel, &sd);
        sd->GetMediaTypeHandler(&h);
        DWORD n = 0;
        h->GetMediaTypeCount(&n);
        wprintf(L"stream %u: %u formats\n", s, n);
        for (DWORD i = 0; i < n; i++)
        {
            ComPtr<IMFMediaType> mt;
            h->GetMediaTypeByIndex(i, &mt);
            GUID sub = GUID_NULL; UINT32 w = 0, hgt = 0, num = 0, den = 1, matrix = 0, range = 0;
            mt->GetGUID(MF_MT_SUBTYPE, &sub);
            MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &hgt);
            MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &num, &den);
            mt->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
            mt->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range);
            wprintf(L"  %-5s %4ux%-4u %6.2f fps  matrix %u range %u\n", SubtypeName(sub).c_str(), w, hgt, den ? (double)num / den : 0.0, matrix, range);
        }
    }
    source->Shutdown();
    return 0;
}

static int Grab(const std::wstring& name, const std::wstring& out, int skip, int shareMode = -1)
{
    auto cams = ListCameras();
    Camera* c = Find(cams, name);
    if (!c) { wprintf(L"camera not found\n"); return 1; }
    if (shareMode >= 0) c->activate->SetUINT32(MF_DEVSOURCE_ATTRIBUTE_FRAMESERVER_SHARE_MODE, (UINT32)shareMode);
    ComPtr<IMFMediaSource> source;
    HRESULT hr = c->activate->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(hr)) { wprintf(L"open failed 0x%08X\n", hr); return 1; }
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), attrs.Get(), &reader);
    if (FAILED(hr)) { wprintf(L"reader failed 0x%08X\n", hr); return 1; }
    ComPtr<IMFMediaType> rgb;
    MFCreateMediaType(&rgb);
    rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, rgb.Get());
    if (FAILED(hr)) { wprintf(L"RGB32 not available 0x%08X\n", hr); return 1; }
    ComPtr<IMFMediaType> current;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h);

    ComPtr<IMFSample> sample;
    DWORD start = GetTickCount();
    for (int got = 0; got <= skip;)
    {
        DWORD stream, flags; LONGLONG ts;
        sample.Reset();
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample);
        if (FAILED(hr)) { wprintf(L"ReadSample failed 0x%08X\n", hr); return 1; }
        if (sample) got++;
    }
    double seconds = (GetTickCount() - start) / 1000.0;
    ComPtr<IMFMediaBuffer> buffer;
    sample->ConvertToContiguousBuffer(&buffer);
    BYTE* data; DWORD max, len;
    buffer->Lock(&data, &max, &len);
    BITMAPFILEHEADER bf = {};
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi); bi.biWidth = (LONG)w; bi.biHeight = -(LONG)h; bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
    DWORD imageSize = w * h * 4;
    bf.bfType = 0x4D42; bf.bfOffBits = sizeof(bf) + sizeof(bi); bf.bfSize = bf.bfOffBits + imageSize;
    FILE* f = nullptr;
    if (_wfopen_s(&f, out.c_str(), L"wb") != 0 || !f) { wprintf(L"cannot write %s\n", out.c_str()); return 1; }
    fwrite(&bf, sizeof(bf), 1, f); fwrite(&bi, sizeof(bi), 1, f); fwrite(data, 1, imageSize < len ? imageSize : len, f);
    fclose(f);
    buffer->Unlock();
    wprintf(L"saved %ux%u frame from %s (%d frames in %.1f s)\n", w, h, c->name.c_str(), skip + 1, seconds);
    source->Shutdown();
    return 0;
}

// Streams a camera at its native W x H @ fps for some seconds and reports the frame rate achieved.
static int Bench(const std::wstring& name, UINT32 wantW, UINT32 wantH, UINT32 wantFps, int seconds)
{
    auto cams = ListCameras();
    Camera* c = Find(cams, name);
    if (!c) { wprintf(L"camera not found\n"); return 1; }
    ComPtr<IMFMediaSource> source;
    bool shared = g_shared;
    if (shared) c->activate->SetUINT32(MF_DEVSOURCE_ATTRIBUTE_FRAMESERVER_SHARE_MODE, 1);
    if (FAILED(c->activate->ActivateObject(IID_PPV_ARGS(&source)))) { wprintf(L"open failed\n"); return 1; }
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader))) { wprintf(L"reader failed\n"); return 1; }
    bool set = false;
    for (DWORD i = 0; !set; i++)
    {
        ComPtr<IMFMediaType> mt;
        if (FAILED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &mt))) break;
        GUID sub; UINT32 w = 0, h = 0, num = 0, den = 1;
        mt->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &num, &den);
        if (sub == MFVideoFormat_NV12 && w == wantW && h == wantH && den && (UINT32)((double)num / den + 0.5) == wantFps)
            set = SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt.Get()));
    }
    if (!set && !shared) { wprintf(L"format not available\n"); return 1; }
    int frames = 0;
    ULONGLONG start = 0;
    for (;;)
    {
        DWORD stream, flags; LONGLONG ts;
        ComPtr<IMFSample> sample;
        HRESULT rhr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample);
        if (FAILED(rhr)) { wprintf(L"ReadSample failed 0x%08X after %d frames\n", rhr, frames); break; }
        if (!sample) continue;
        if (!start) { start = GetTickCount64(); continue; }   // ignore start-up time
        frames++;
        if (GetTickCount64() - start >= (ULONGLONG)seconds * 1000) break;
    }
    double s = (GetTickCount64() - start) / 1000.0;
    wprintf(L"%ux%u NV12: %d frames in %.1f s = %.1f fps\n", wantW, wantH, frames, s, frames / s);
    source->Shutdown();
    return 0;
}

// Times the LUT on synthetic 1080p NV12 frames (build once, apply 600 times).
static int LutBench(const std::wstring& cube, int intensity)
{
    CubeLut lut;
    std::wstring error;
    ULONGLONG t0 = GetTickCount64();
    if (!lut.Load(cube, &error)) { wprintf(L"%s\n", error.c_str()); return 1; }
    ULONGLONG t1 = GetTickCount64();
    auto table = YuvLutTable::Build(lut, intensity / 100.f, YuvFormat{ false, false });
    ULONGLONG t2 = GetTickCount64();
    const UINT w = 1920, h = 1080;
    std::vector<BYTE> frame(w * h * 3 / 2);
    for (size_t i = 0; i < frame.size(); i++) frame[i] = (BYTE)(i * 2654435761u >> 24);
    FILETIME c, e, k0, u0, k1, u1;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k0, &u0);
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    const int frames = 600;
    for (int i = 0; i < frames; i++) table->ApplyNV12(frame.data(), w, frame.data() + w * h, w, w, h);
    QueryPerformanceCounter(&b);
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k1, &u1);
    auto ft = [](FILETIME t) { return ((ULONGLONG)t.dwHighDateTime << 32 | t.dwLowDateTime) / 10000.0; };
    double wall = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
    double cpu = (ft(k1) + ft(u1)) - (ft(k0) + ft(u0));
    wprintf(L"parse %llu ms, build %llu ms\n", t1 - t0, t2 - t1);
    wprintf(L"1080p frame: %.2f ms wall, %.2f ms CPU per frame -> at 60 fps %.1f%% of one core (%.1f%% of all %u threads)\n",
        wall / frames, cpu / frames, cpu / frames * 60 / 10, cpu / frames * 60 / 10 / std::thread::hardware_concurrency(), std::thread::hardware_concurrency());
    return 0;
}

// Runs RVM matting on live Facecam frames and reports speed and how much of the frame is "person".
static int MatteBench(const std::wstring& ortDir, const std::wstring& model, UINT inW, UINT inH, int frames)
{
    Matting m;
    std::wstring err;
    ULONGLONG t0 = GetTickCount64();
    if (!m.Load(ortDir, model, inW, inH, &err)) { wprintf(L"load failed: %s\n", err.c_str()); return 1; }
    wprintf(L"loaded in %llu ms, running on %s\n", GetTickCount64() - t0, m.OnGpu() ? L"GPU (DirectML)" : L"CPU");

    auto cams = ListCameras();
    Camera* c = FindFacecam(cams);
    if (!c) { wprintf(L"Facecam not found\n"); return 1; }
    c->activate->SetUINT32(MF_DEVSOURCE_ATTRIBUTE_FRAMESERVER_SHARE_MODE, 1);
    ComPtr<IMFMediaSource> source;
    if (FAILED(c->activate->ActivateObject(IID_PPV_ARGS(&source)))) { wprintf(L"open failed\n"); return 1; }
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    MFCreateSourceReaderFromMediaSource(source.Get(), attrs.Get(), &reader);
    ComPtr<IMFMediaType> nv12;
    MFCreateMediaType(&nv12);
    nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nv12.Get());
    ComPtr<IMFMediaType> cur;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
    wprintf(L"camera frames %ux%u, model input %ux%u\n", w, h, inW, inH);

    std::vector<BYTE> frame((size_t)w * h * 3 / 2);
    double total = 0, worst = 0, person = 0, soft = 0, blurTotal = 0, runCpu = 0, blurCpu = 0;
    BackgroundBlur blur;
    int timed = 0;
    for (int i = 0; i < frames;)
    {
        DWORD stream, flags; LONGLONG ts;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample))) { wprintf(L"read failed\n"); break; }
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buf;
        sample->ConvertToContiguousBuffer(&buf);
        BYTE* d; DWORD mx, len;
        if (FAILED(buf->Lock(&d, &mx, &len))) continue;
        memcpy(frame.data(), d, len < frame.size() ? len : frame.size());
        buf->Unlock();
        LARGE_INTEGER a, b, f;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        auto procCpu = [] { FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
            return ((((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10000.0; };
        double c0 = procCpu();
        bool ok = m.Run(frame.data(), w, frame.data() + (size_t)w * h, w, w, h, YuvFormat{ false, false });
        QueryPerformanceCounter(&b);
        double c1 = procCpu();
        if (i >= 5) runCpu += c1 - c0;
        if (!ok) { wprintf(L"run failed\n"); return 1; }
        double ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        QueryPerformanceCounter(&a);
        blur.Apply(frame.data(), w, frame.data() + (size_t)w * h, w, w, h, m.Alpha().data(), m.AlphaWidth(), m.AlphaHeight(), 60);
        QueryPerformanceCounter(&b);
        double blurMs = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        if (i >= 5) blurCpu += procCpu() - c1;
        if (i >= 5) { total += ms; blurTotal += blurMs; timed++; if (ms > worst) worst = ms; }   // skip warm-up
        if (i == frames - 1)
        {
            size_t n = m.Alpha().size(), on = 0, mid = 0;
            for (auto v : m.Alpha()) { if (v >= 128) on++; if (v > 16 && v < 240) mid++; }
            person = 100.0 * on / n; soft = 100.0 * mid / n;
        }
        i++;
    }
    FILETIME c_, e_, k_, u_;
    GetProcessTimes(GetCurrentProcess(), &c_, &e_, &k_, &u_);
    double cpuMs = ((((ULONGLONG)k_.dwHighDateTime << 32) | k_.dwLowDateTime) + (((ULONGLONG)u_.dwHighDateTime << 32) | u_.dwLowDateTime)) / 10000.0;
    wprintf(L"process CPU per frame (all threads, incl. camera reading): %.2f ms; during matting %.2f ms, during blur %.2f ms\n", cpuMs / frames, runCpu / (frames - 5), blurCpu / (frames - 5));
    wprintf(L"matting: %.2f ms average, %.2f ms worst over %d frames; blur+blend %.2f ms average\n", timed ? total / timed : 0, worst, timed, timed ? blurTotal / timed : 0);
    wprintf(L"last frame: %.1f%% person, %.1f%% soft edge\n", person, soft);
    source->Shutdown();
    return 0;
}
int wmain(int argc, wchar_t** argv)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    int rc = 2;
    // Pull out the --shared option wherever it appears.
    std::vector<wchar_t*> args;
    for (int i = 0; i < argc; i++)
        if (wcscmp(argv[i], L"--shared") == 0) g_shared = true; else args.push_back(argv[i]);
    argc = (int)args.size();
    argv = args.data();
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    if (cmd == L"--register") rc = Register();
    else if (cmd == L"--unregister") rc = Unregister();
    else if (cmd == L"--list")
    {
        for (auto& c : ListCameras()) wprintf(L"%s\n    %s\n", c.name.c_str(), c.link.c_str());
        rc = 0;
    }
    else if (cmd == L"--formats" && argc > 2) rc = Formats(argv[2]);
    else if (cmd == L"--mattebench" && argc > 5) rc = MatteBench(argv[2], argv[3], _wtoi(argv[4]), _wtoi(argv[5]), argc > 6 ? _wtoi(argv[6]) : 120);
    else if (cmd == L"--grab" && argc > 3) rc = Grab(argv[2], argv[3], argc > 4 ? _wtoi(argv[4]) : 30, g_shared ? 1 : -1);
    else if (cmd == L"--lutbench" && argc > 2) rc = LutBench(argv[2], argc > 3 ? _wtoi(argv[3]) : 100);
    else if (cmd == L"--bench" && argc > 2) rc = Bench(argv[2], 1920, 1080, argc > 3 ? _wtoi(argv[3]) : 60, argc > 4 ? _wtoi(argv[4]) : 10);
    else wprintf(L"usage: opencamhub-setup --register | --unregister | --list | --formats <camera> | --grab <camera> <out.bmp> [frames]\n"
                 L"       | --bench <camera> [fps] [seconds] | --lutbench <file.cube> [intensity] | --mattebench <dir> <model> <w> <h> [frames]\n"
                 L"       add --shared to --grab or --bench to open the camera in shared mode\n");
    MFShutdown();
    CoUninitialize();
    return rc;
}
