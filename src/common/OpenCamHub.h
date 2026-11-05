// Shared constants for the OpenCamHub virtual camera DLL and its tools.
#pragma once
#include <windows.h>
#include <string>

// CLSID of the virtual camera media source (vcam.dll).
// {5B2B95F8-965B-4FC7-8A72-0ED88649C542}
static const GUID CLSID_OpenCamHubSource = { 0x5b2b95f8, 0x965b, 0x4fc7, { 0x8a, 0x72, 0x0e, 0xd8, 0x86, 0x49, 0xc5, 0x42 } };
#define OPENCAMHUB_CLSID_STR L"{5B2B95F8-965B-4FC7-8A72-0ED88649C542}"

// Name apps such as Discord show for the virtual camera.
#define OPENCAMHUB_FRIENDLY_NAME L"OpenCamHub"

// The physical camera we wrap: Elgato Facecam MK.2.
#define FACECAM_HWID L"vid_0fd9&pid_0094"

// Settings live in ProgramData so the camera service (which runs as LOCAL SERVICE) can read them.
inline std::wstring OpenCamHubDataDir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = ExpandEnvironmentStringsW(L"%ProgramData%\\OpenCamHub", buf, MAX_PATH);
    return (n > 0 && n <= MAX_PATH) ? std::wstring(buf) : std::wstring(L"C:\\ProgramData\\OpenCamHub");
}
inline std::wstring OpenCamHubSettingsPath() { return OpenCamHubDataDir() + L"\\settings.ini"; }
inline std::wstring OpenCamHubLutDir() { return OpenCamHubDataDir() + L"\\luts"; }

// The LUT part of settings.ini:
//   [lut]
//   enabled=1
//   file=Coffee_Break.cube   (a file name inside the luts folder)
//   intensity=61             (0-100)
struct LutSettings
{
    bool enabled = false;
    std::wstring file;
    int intensity = 100;
};

inline LutSettings ReadLutSettings()
{
    LutSettings s;
    std::wstring path = OpenCamHubSettingsPath();
    s.enabled = GetPrivateProfileIntW(L"lut", L"enabled", 0, path.c_str()) != 0;
    wchar_t file[MAX_PATH] = {};
    GetPrivateProfileStringW(L"lut", L"file", L"", file, MAX_PATH, path.c_str());
    s.file = file;
    s.intensity = (int)GetPrivateProfileIntW(L"lut", L"intensity", 100, path.c_str());
    if (s.intensity < 0) s.intensity = 0;
    if (s.intensity > 100) s.intensity = 100;
    return s;
}

inline void WriteLutSettings(const LutSettings& s)
{
    std::wstring path = OpenCamHubSettingsPath();
    WritePrivateProfileStringW(L"lut", L"enabled", s.enabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"lut", L"file", s.file.c_str(), path.c_str());
    WritePrivateProfileStringW(L"lut", L"intensity", std::to_wstring(s.intensity).c_str(), path.c_str());
}

// The video part of settings.ini:
//   [video]
//   mirror=1      (flip the picture left-to-right)
inline bool ReadMirrorSetting()
{
    return GetPrivateProfileIntW(L"video", L"mirror", 0, OpenCamHubSettingsPath().c_str()) != 0;
}
inline bool WriteMirrorSetting(bool mirror)
{
    return WritePrivateProfileStringW(L"video", L"mirror", mirror ? L"1" : L"0", OpenCamHubSettingsPath().c_str()) != FALSE;
}
// The background part of settings.ini:
//   [background]
//   mode=1        (0 = off, 1 = blur)
//   strength=60   (0-100)
struct BackgroundSettings
{
    int mode = 0;
    int strength = 60;
};
inline BackgroundSettings ReadBackgroundSettings()
{
    BackgroundSettings s;
    std::wstring path = OpenCamHubSettingsPath();
    s.mode = (int)GetPrivateProfileIntW(L"background", L"mode", 0, path.c_str());
    s.strength = (int)GetPrivateProfileIntW(L"background", L"strength", 60, path.c_str());
    if (s.strength < 0) s.strength = 0;
    if (s.strength > 100) s.strength = 100;
    return s;
}
inline bool WriteBackgroundSettings(const BackgroundSettings& s)
{
    std::wstring path = OpenCamHubSettingsPath();
    return WritePrivateProfileStringW(L"background", L"mode", std::to_wstring(s.mode).c_str(), path.c_str()) &&
           WritePrivateProfileStringW(L"background", L"strength", std::to_wstring(s.strength).c_str(), path.c_str());
}

// The folder holding this module (vcam.dll or OpenCamHub.exe), where ONNX Runtime and the model live.
extern "C" IMAGE_DOS_HEADER __ImageBase;
inline std::wstring ThisModuleDir()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW((HMODULE)&__ImageBase, path, MAX_PATH);
    std::wstring s(path, n);
    size_t slash = s.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}
#define OPENCAMHUB_MATTING_MODEL L"rvm_resnet50_fp32.onnx"