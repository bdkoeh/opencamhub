#include "Effects.h"
#include <cstdio>
#include <string>
#include <thread>
#include "OpenCamHub.h"

namespace
{
    void Log(const wchar_t* fmt, ...)
    {
        wchar_t buf[512] = L"[OpenCamHub] ";
        va_list args;
        va_start(args, fmt);
        size_t prefix = wcslen(buf);
        _vsnwprintf_s(buf + prefix, _countof(buf) - prefix - 1, _TRUNCATE, fmt, args);
        va_end(args);
        wcscat_s(buf, L"\n");
        OutputDebugStringW(buf);
    }

    // Matting input size: half of 1080p. Large enough for clean edges, small enough to be quick.
    const UINT MATTE_W = 960, MATTE_H = 540;
}

void FrameEffects::CheckSettings(YuvFormat format, UINT width, UINT height)
{
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    FILETIME time = {};
    if (GetFileAttributesExW(OpenCamHubSettingsPath().c_str(), GetFileExInfoStandard, &info))
        time = info.ftLastWriteTime;

    FILETIME imageTime = {};
    if (GetFileAttributesExW(OpenCamHubImagePath().c_str(), GetFileExInfoStandard, &info))
        imageTime = info.ftLastWriteTime;
    AcquireSRWLockShared(&m_s->lock);
    bool sameImage = CompareFileTime(&imageTime, &m_s->imageTime) == 0 &&
                     width == m_s->width && height == m_s->height && m_s->builtFormat == format;
    auto cachedImage = sameImage ? m_s->image : nullptr;
    bool current = sameImage && m_s->built && CompareFileTime(&time, &m_s->builtTime) == 0 && m_s->builtFormat == format;
    ReleaseSRWLockShared(&m_s->lock);
    if (current || m_s->building.exchange(true))
        return;

    // Parsing the .cube file, building the table and loading the matting model take a few hundred
    // ms, so do it off the frame path.
    std::shared_ptr<Shared> s = m_s;
    std::thread([s, format, time, imageTime, width, height, cachedImage]() {
        LutSettings settings = ReadLutSettings();
        bool mirror = ReadMirrorSetting();
        BackgroundSettings bg = ReadBackgroundSettings();

        std::shared_ptr<YuvLutTable> table;
        if (settings.enabled && !settings.file.empty() && settings.intensity > 0
            && settings.file.find(L'\\') == std::wstring::npos && settings.file.find(L'/') == std::wstring::npos)
        {
            CubeLut lut;
            std::wstring error;
            if (lut.Load(OpenCamHubLutDir() + L"\\" + settings.file, &error))
                table = YuvLutTable::Build(lut, settings.intensity / 100.f, format);
            else
                Log(L"LUT not loaded: %s", error.c_str());
        }

        auto image = bg.mode == 2 ? cachedImage : nullptr;
        if (bg.mode == 2 && !image)
        {
            HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto candidate = std::make_shared<BackgroundImage>();
            if (SUCCEEDED(init))
            {
                if (candidate->Load(OpenCamHubImagePath(), width, height, format)) image = candidate;
                CoUninitialize();
            }
            if (!image) Log(L"Background image could not be loaded");
        }
        bool needMatte = bg.mode == 1 || image;
        // Keep the matting model loaded while the background effect is on; free it when it's off.
        AcquireSRWLockShared(&s->lock);
        std::shared_ptr<Matting> matting = needMatte ? s->matting : nullptr;
        ReleaseSRWLockShared(&s->lock);
        if (needMatte && !matting)
        {
            auto m = std::make_shared<Matting>();
            std::wstring dir = ThisModuleDir(), error;
            ULONGLONG t0 = GetTickCount64();
            if (m->Load(dir, dir + L"\\" OPENCAMHUB_MATTING_MODEL, MATTE_W, MATTE_H, &error))
            {
                matting = m;
                Log(L"matting model loaded in %llu ms on %s", GetTickCount64() - t0, m->OnGpu() ? L"GPU" : L"CPU");
            }
            else
                Log(L"matting model not loaded: %s", error.c_str());
        }

        AcquireSRWLockExclusive(&s->lock);
        s->table = table;
        s->mirror = mirror;
        s->matting = matting;
        s->image = image;
        s->imageTime = imageTime;
        s->width = width;
        s->height = height;
        s->blurStrength = bg.strength;
        s->builtTime = time;
        s->builtFormat = format;
        s->built = true;
        ReleaseSRWLockExclusive(&s->lock);
        Log(L"LUT %s (%s, intensity %d), mirror %s, background %s", table ? L"on" : L"off", settings.file.c_str(),
            settings.intensity, mirror ? L"on" : L"off", matting ? (image ? L"image" : L"blur") : L"off");
        s->building = false;
    }).detach();
}

void FrameEffects::Apply(BYTE* y, LONG yPitch, BYTE* uv, LONG uvPitch, UINT width, UINT height, YuvFormat format)
{
    // Look at settings.ini about twice a second; reading a file timestamp is cheap.
    if (m_frame++ % 15 == 0)
        CheckSettings(format, width, height);
    if (width < 2 || height < 2)
        return;

    AcquireSRWLockShared(&m_s->lock);
    std::shared_ptr<YuvLutTable> table = m_s->table;
    bool mirror = m_s->mirror;
    std::shared_ptr<Matting> matting = m_s->matting;
    auto image = m_s->image;
    bool imageFits = width == m_s->width && height == m_s->height && format == m_s->builtFormat;
    int strength = m_s->blurStrength;
    ReleaseSRWLockShared(&m_s->lock);

    if (mirror)
        MirrorNV12(y, yPitch, uv, uvPitch, width, height);
    if (matting && (!image || imageFits))
    {
        // A new model or a new frame size means earlier frames no longer apply.
        if (matting.get() != m_lastMatting || width != m_lastW || height != m_lastH)
        {
            matting->ResetHistory();
            m_lastMatting = matting.get();
            m_lastW = width;
            m_lastH = height;
        }
        if (matting->Run(y, yPitch, uv, uvPitch, width, height, format))
        {
            const auto* alpha = matting->Alpha().data();
            if (image)
                image->Apply(y, yPitch, uv, uvPitch, alpha, matting->AlphaWidth(), matting->AlphaHeight());
            else
                m_blur.Apply(y, yPitch, uv, uvPitch, width, height, alpha,
                             matting->AlphaWidth(), matting->AlphaHeight(), strength);
        }
    }
    else
        m_lastMatting = nullptr;
    if (table && table->Format() == format)
        table->ApplyNV12(y, yPitch, uv, uvPitch, width, height);
}
