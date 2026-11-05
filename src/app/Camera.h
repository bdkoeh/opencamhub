// Reads and writes the Facecam MK.2's settings: standard UVC controls through DirectShow, and
// Elgato's own controls (metering, vibrancy, tint, HDR, noise reduction, save) through its
// extension units. The extension unit GUIDs and selectors were mapped by the open-source
// "facecam" project (github.com/nklmilojevic/facecam) and confirmed on this camera.
#pragma once
#include <windows.h>
#include <dshow.h>
#include <ks.h>
#include <ksproxy.h>
#include <vidcap.h>
#include <wrl.h>

enum class Ctl
{
    Zoom, Pan, Tilt,
    Contrast, Saturation, Sharpness, Vibrancy,
    AutoExposure, Metering, Shutter, Brightness, DynamicRange,
    AutoWhiteBalance, Temperature, Tint,
    NoiseReduction, AntiFlicker,
    Count
};

struct CtlRange
{
    long min = 0, max = 0, step = 1, def = 0;
};

class Camera
{
public:
    bool Open();            // finds the Facecam; false if it isn't connected
    bool IsOpen() const { return m_filter != nullptr; }
    void Close();

    bool Range(Ctl c, CtlRange& r);
    bool Get(Ctl c, long& value);
    bool Set(Ctl c, long value);
    bool SaveToCamera();    // stores the current settings as the camera's power-on settings

private:
    struct Xu { Microsoft::WRL::ComPtr<IKsControl> ks; ULONG node = 0; };
    bool XuGet(Xu& xu, const GUID& set, ULONG selector, int size, bool isSigned, long& value);
    bool XuSet(Xu& xu, const GUID& set, ULONG selector, int size, long value);
    bool XuRange(Xu& xu, const GUID& set, ULONG selector, int size, bool isSigned, CtlRange& r);

    Microsoft::WRL::ComPtr<IBaseFilter> m_filter;
    Microsoft::WRL::ComPtr<IAMVideoProcAmp> m_amp;
    Microsoft::WRL::ComPtr<IAMCameraControl> m_cam;
    Xu m_image, m_device;
};
