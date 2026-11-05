#include "Camera.h"
#include <ksmedia.h>
#include <string>
#include "../common/OpenCamHub.h"

using Microsoft::WRL::ComPtr;

namespace
{
    // {23E49ED0-1178-4F31-AE52-D2FB8A8D3001}: image pipeline unit
    const GUID XU_IMAGE = { 0x23e49ed0, 0x1178, 0x4f31, { 0xae, 0x52, 0xd2, 0xfb, 0x8a, 0x8d, 0x30, 0x01 } };
    // {2C49D16A-32B8-4485-3EA8-643A15236002}: device unit
    const GUID XU_DEVICE = { 0x2c49d16a, 0x32b8, 0x4485, { 0x3e, 0xa8, 0x64, 0x3a, 0x15, 0x23, 0x60, 0x02 } };

    enum Kind { Amp, Cam, XuImage };
    struct Def { Kind kind; long prop; int size; bool isSigned; };

    Def Lookup(Ctl c)
    {
        switch (c)
        {
        case Ctl::Zoom:             return { Cam, CameraControl_Zoom, 0, true };
        case Ctl::Pan:              return { Cam, CameraControl_Pan, 0, true };
        case Ctl::Tilt:             return { Cam, CameraControl_Tilt, 0, true };
        case Ctl::Contrast:         return { Amp, VideoProcAmp_Contrast, 0, true };
        case Ctl::Saturation:       return { Amp, VideoProcAmp_Saturation, 0, true };
        case Ctl::Sharpness:        return { Amp, VideoProcAmp_Sharpness, 0, true };
        case Ctl::Brightness:       return { Amp, VideoProcAmp_Brightness, 0, true };
        case Ctl::AntiFlicker:      return { Amp, 13 /* VideoProcAmp_PowerlineFrequency */, 0, true };
        case Ctl::Shutter:          return { Cam, CameraControl_Exposure, 0, true };
        case Ctl::AutoExposure:     return { Cam, CameraControl_Exposure, 0, true };
        case Ctl::Temperature:      return { Amp, VideoProcAmp_WhiteBalance, 0, true };
        case Ctl::AutoWhiteBalance: return { Amp, VideoProcAmp_WhiteBalance, 0, true };
        case Ctl::Metering:         return { XuImage, 0x02, 1, false };
        case Ctl::Vibrancy:         return { XuImage, 0x0E, 2, false };
        case Ctl::Tint:             return { XuImage, 0x12, 2, true };
        case Ctl::DynamicRange:     return { XuImage, 0x15, 1, false };
        case Ctl::NoiseReduction:   return { XuImage, 0x16, 1, false };
        default:                    return { Amp, -1, 0, false };
        }
    }

    bool IsAutoToggle(Ctl c) { return c == Ctl::AutoExposure || c == Ctl::AutoWhiteBalance; }

    long FromBytes(const BYTE* b, int size, bool isSigned)
    {
        unsigned long v = 0;
        for (int i = 0; i < size; i++) v |= (unsigned long)b[i] << (8 * i);
        if (isSigned && size < 4 && (v & (1ul << (8 * size - 1)))) v |= ~((1ul << (8 * size)) - 1);
        return (long)v;
    }

    HRESULT KsNode(IKsControl* ks, const GUID& set, ULONG id, ULONG flags, ULONG node, void* data, ULONG size, ULONG* returned)
    {
        KSP_NODE p = {};
        p.Property.Set = set;
        p.Property.Id = id;
        p.Property.Flags = flags | KSPROPERTY_TYPE_TOPOLOGY;
        p.NodeId = node;
        return ks->KsProperty(&p.Property, sizeof(p), data, size, returned);
    }
}

bool Camera::Open()
{
    Close();
    ComPtr<ICreateDevEnum> devEnum;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum)))) return false;
    ComPtr<IEnumMoniker> monikers;
    if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0) != S_OK) return false;

    ComPtr<IMoniker> m;
    while (monikers->Next(1, &m, nullptr) == S_OK)
    {
        ComPtr<IPropertyBag> bag;
        if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))))
        {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(bag->Read(L"DevicePath", &v, nullptr)) && v.vt == VT_BSTR)
            {
                std::wstring path(v.bstrVal);
                for (auto& ch : path) ch = towlower(ch);
                if (path.find(FACECAM_HWID) != std::wstring::npos)
                    m->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&m_filter));
            }
            VariantClear(&v);
        }
        m.Reset();
        if (m_filter) break;
    }
    if (!m_filter) return false;

    m_filter.As(&m_amp);
    m_filter.As(&m_cam);

    // Find which topology nodes are Elgato's two extension units.
    ComPtr<IKsTopologyInfo> topo;
    if (SUCCEEDED(m_filter.As(&topo)))
    {
        DWORD nodes = 0;
        topo->get_NumNodes(&nodes);
        for (DWORD n = 0; n < nodes; n++)
        {
            GUID type;
            if (FAILED(topo->get_NodeType(n, &type)) || type != KSNODETYPE_DEV_SPECIFIC) continue;
            ComPtr<IKsControl> ks;
            if (FAILED(topo->CreateNodeInstance(n, IID_PPV_ARGS(&ks)))) continue;
            BYTE buf[64];
            ULONG got = 0;
            if (!m_image.ks && SUCCEEDED(KsNode(ks.Get(), XU_IMAGE, 0x02, KSPROPERTY_TYPE_GET, n, buf, 1, &got)))
                m_image = { ks, n };
            else if (!m_device.ks && KsNode(ks.Get(), XU_DEVICE, 0x01, KSPROPERTY_TYPE_GET, n, buf, sizeof(buf), &got) != HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND))
                m_device = { ks, n };
        }
    }
    return true;
}

void Camera::Close()
{
    m_image = {};
    m_device = {};
    m_amp.Reset();
    m_cam.Reset();
    m_filter.Reset();
}

bool Camera::XuGet(Xu& xu, const GUID& set, ULONG selector, int size, bool isSigned, long& value)
{
    if (!xu.ks) return false;
    BYTE buf[8] = {};
    ULONG got = 0;
    if (FAILED(KsNode(xu.ks.Get(), set, selector, KSPROPERTY_TYPE_GET, xu.node, buf, size, &got))) return false;
    value = FromBytes(buf, size, isSigned);
    return true;
}

bool Camera::XuSet(Xu& xu, const GUID& set, ULONG selector, int size, long value)
{
    if (!xu.ks) return false;
    BYTE buf[8] = {};
    for (int i = 0; i < size; i++) buf[i] = (BYTE)((unsigned long)value >> (8 * i));
    ULONG got = 0;
    return SUCCEEDED(KsNode(xu.ks.Get(), set, selector, KSPROPERTY_TYPE_SET, xu.node, buf, size, &got));
}

bool Camera::XuRange(Xu& xu, const GUID& set, ULONG selector, int size, bool isSigned, CtlRange& r)
{
    if (!xu.ks) return false;
    // BASICSUPPORT returns KSPROPERTY_DESCRIPTION (40 bytes) + KSPROPERTY_MEMBERSHEADER (16 bytes),
    // then the control's resolution, minimum and maximum as raw values.
    BYTE buf[256] = {};
    ULONG got = 0;
    if (FAILED(KsNode(xu.ks.Get(), set, selector, KSPROPERTY_TYPE_BASICSUPPORT, xu.node, buf, sizeof(buf), &got))) return false;
    if (got < (ULONG)(56 + 3 * size)) return false;
    r.step = FromBytes(buf + 56, size, false);
    r.min = FromBytes(buf + 56 + size, size, isSigned);
    r.max = FromBytes(buf + 56 + 2 * size, size, isSigned);
    if (r.step <= 0) r.step = 1;
    r.def = r.min < 0 ? 0 : r.min;
    return r.max > r.min;
}

bool Camera::Range(Ctl c, CtlRange& r)
{
    if (!IsOpen()) return false;
    if (IsAutoToggle(c)) { r = { 0, 1, 1, 1 }; return true; }
    Def d = Lookup(c);
    long min, max, step, def, caps;
    switch (d.kind)
    {
    case Amp:
        if (!m_amp || FAILED(m_amp->GetRange(d.prop, &min, &max, &step, &def, &caps))) return false;
        break;
    case Cam:
        if (!m_cam || FAILED(m_cam->GetRange(d.prop, &min, &max, &step, &def, &caps))) return false;
        break;
    default:
        return XuRange(m_image, XU_IMAGE, (ULONG)d.prop, d.size, d.isSigned, r);
    }
    r = { min, max, step > 0 ? step : 1, def };
    return true;
}

bool Camera::Get(Ctl c, long& value)
{
    if (!IsOpen()) return false;
    Def d = Lookup(c);
    long v = 0, flags = 0;
    switch (d.kind)
    {
    case Amp:
        if (!m_amp || FAILED(m_amp->Get(d.prop, &v, &flags))) return false;
        break;
    case Cam:
        if (!m_cam || FAILED(m_cam->Get(d.prop, &v, &flags))) return false;
        break;
    default:
        return XuGet(m_image, XU_IMAGE, (ULONG)d.prop, d.size, d.isSigned, value);
    }
    value = IsAutoToggle(c) ? ((flags & 1) ? 1 : 0) : v;   // flag 1 = auto for both interfaces
    return true;
}

bool Camera::Set(Ctl c, long value)
{
    if (!IsOpen()) return false;
    Def d = Lookup(c);
    if (d.kind == XuImage)
        return XuSet(m_image, XU_IMAGE, (ULONG)d.prop, d.size, value);

    long current = 0, flags = 0;
    if (IsAutoToggle(c))
    {
        // Switching auto on/off keeps the current manual value.
        if (d.kind == Amp) { if (FAILED(m_amp->Get(d.prop, &current, &flags))) return false; }
        else if (FAILED(m_cam->Get(d.prop, &current, &flags))) return false;
        long newFlags = value ? 1 : 2;
        return d.kind == Amp ? SUCCEEDED(m_amp->Set(d.prop, current, newFlags)) : SUCCEEDED(m_cam->Set(d.prop, current, newFlags));
    }
    // A manual value for exposure or white balance also switches that control to manual.
    long newFlags = 2;
    return d.kind == Amp ? SUCCEEDED(m_amp->Set(d.prop, value, newFlags)) : SUCCEEDED(m_cam->Set(d.prop, value, newFlags));
}

bool Camera::SaveToCamera()
{
    return XuSet(m_device, XU_DEVICE, 0x07, 1, 1);
}
