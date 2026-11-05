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

void FrameEffects::CheckSettings(YuvFormat format)
{
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    FILETIME time = {};
    if (GetFileAttributesExW(OpenCamHubSettingsPath().c_str(), GetFileExInfoStandard, &info))
        time = info.ftLastWriteTime;

    AcquireSRWLockShared(&m_s->lock);
    bool current = m_s->built && CompareFileTime(&time, &m_s->builtTime) == 0 && m_s->builtFormat == format;
    ReleaseSRWLockShared(&m_s->lock);
    if (current || m_s->building.exchange(true))
        return;

    // Parsing the .cube file, building the table and loading the matting model take a few hundred
    // ms, so do it off the frame path.
    std::shared_ptr<Shared> s = m_s;
    std::thread([s, format, time]() {
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

        // Keep the matting model loaded while the background effect is on; free it when it's off.
        AcquireSRWLockShared(&s->lock);
        std::shared_ptr<Matting> matting = bg.mode == 1 ? s->matting : nullptr;
        ReleaseSRWLockShared(&s->lock);
        if (bg.mode == 1 && !matting)
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
        s->blurStrength = bg.strength;
        s->builtTime = time;
        s->builtFormat = format;
        s->built = true;
        ReleaseSRWLockExclusive(&s->lock);
        Log(L"LUT %s (%s, intensity %d), mirror %s, background %s", table ? L"on" : L"off", settings.file.c_str(),
            settings.intensity, mirror ? L"on" : L"off", matting ? L"blur" : L"off");
        s->building = false;
    }).detach();
}

void FrameEffects::Apply(BYTE* y, LONG yPitch, BYTE* uv, LONG uvPitch, UINT width, UINT height, YuvFormat format)
{
    // Look at settings.ini about twice a second; reading a file timestamp is cheap.
    if (m_frame++ % 15 == 0)
        CheckSettings(format);
    if (width < 2 || height < 2)
        return;

    AcquireSRWLockShared(&m_s->lock);
    std::shared_ptr<YuvLutTable> table = m_s->table;
    bool mirror = m_s->mirror;
    std::shared_ptr<Matting> matting = m_s->matting;
    int strength = m_s->blurStrength;
    ReleaseSRWLockShared(&m_s->lock);

    if (mirror)
        MirrorNV12(y, yPitch, uv, uvPitch, width, height);
    if (matting)
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
            m_blur.Apply(y, yPitch, uv, uvPitch, width, height, matting->Alpha().data(),
                         matting->AlphaWidth(), matting->AlphaHeight(), strength);
    }
    else
        m_lastMatting = nullptr;
    if (table && table->Format() == format)
        table->ApplyNV12(y, yPitch, uv, uvPitch, width, height);
}
