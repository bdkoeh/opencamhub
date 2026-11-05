#include "Source.h"
#include "../common/OpenCamHub.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <thread>

#define RIF(x) do { HRESULT _hr = (x); if (FAILED(_hr)) { Log(L"%S failed 0x%08X (line %d)", #x, _hr, __LINE__); return _hr; } } while (0)

void Log(const wchar_t* fmt, ...)
{
    wchar_t buf[512] = L"[OpenCamHub] ";
    va_list args;
    va_start(args, fmt);
    size_t prefix = wcslen(buf);
    _vsnwprintf_s(buf + prefix, 512 - prefix - 1, _TRUNCATE, fmt, args);
    va_end(args);
    wcscat_s(buf, L"\n");
    OutputDebugStringW(buf);
}

//////////////////////////////////////////////////////////////////////////////////////////
// FrameProcessor: runs the mirror and LUT on each frame from the camera.

void FrameProcessor::Process(IMFSample* sample, UINT width, UINT height, YuvFormat format)
{
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)))
        return;

    ComPtr<IMF2DBuffer2> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d)))
    {
        BYTE* scan0 = nullptr;
        BYTE* start = nullptr;
        LONG pitch = 0;
        DWORD length = 0;
        if (FAILED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_ReadWrite, &scan0, &pitch, &start, &length)))
            return;
        if (pitch > 0)
        {
            // The chroma plane follows the luma plane; some buffers pad the luma height.
            size_t available = length - (size_t)(scan0 - start);
            UINT rows = (UINT)(available * 2 / (3 * (size_t)pitch));
            if (rows < height) rows = height;
            if ((size_t)pitch * rows + (size_t)pitch * (height / 2) <= available)
                m_fx.Apply(scan0, pitch, scan0 + (size_t)pitch * rows, pitch, width, height, format);
        }
        buffer2d->Unlock2D();
        return;
    }

    BYTE* data = nullptr;
    DWORD max = 0, length = 0;
    if (FAILED(buffer->Lock(&data, &max, &length)))
        return;
    if (length >= (DWORD)width * height * 3 / 2)
        m_fx.Apply(data, width, data + (size_t)width * height, width, width, height, format);
    buffer->Unlock();
}

//////////////////////////////////////////////////////////////////////////////////////////
// Stream

static HRESULT CloneMediaType(IMFMediaType* source, IMFMediaType** clone)
{
    ComPtr<IMFMediaType> mt;
    RIF(MFCreateMediaType(&mt));
    RIF(source->CopyAllItems(mt.Get()));
    *clone = mt.Detach();
    return S_OK;
}

HRESULT Stream::Initialize(Source* parent, DWORD streamId, IMFStreamDescriptor* deviceDesc)
{
    Guard g(m_lock);
    m_parent = parent;
    m_streamId = streamId;
    RIF(deviceDesc->GetStreamIdentifier(&m_deviceStreamId));

    // Offer the Facecam's NV12 formats (Frame Server also offers decoded NV12 versions of its MJPEG modes).
    ComPtr<IMFMediaTypeHandler> handler;
    RIF(deviceDesc->GetMediaTypeHandler(&handler));
    DWORD count = 0;
    RIF(handler->GetMediaTypeCount(&count));
    std::vector<ComPtr<IMFMediaType>> types;
    for (DWORD i = 0; i < count; i++)
    {
        ComPtr<IMFMediaType> mt;
        if (FAILED(handler->GetMediaTypeByIndex(i, &mt))) continue;
        GUID major = GUID_NULL, subtype = GUID_NULL;
        mt->GetGUID(MF_MT_MAJOR_TYPE, &major);
        mt->GetGUID(MF_MT_SUBTYPE, &subtype);
        UINT32 w = 0, h = 0, num = 0, den = 1;
        MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &num, &den);
        double fps = den ? (double)num / den : 0;
        if (major == MFMediaType_Video && subtype == MFVideoFormat_NV12 && w > 0 && h > 0 && fps >= 14.5 && fps <= 61)
        {
            ComPtr<IMFMediaType> copy;
            RIF(CloneMediaType(mt.Get(), &copy));
            types.push_back(copy);
            Log(L"offering NV12 %ux%u @ %.2f fps", w, h, fps);
        }
    }
    if (types.empty())
    {
        Log(L"the camera offers no NV12 formats");
        return MF_E_INVALIDMEDIATYPE;
    }
    // Largest first, 30 fps before 60, so apps that just take the default get 1080p30.
    auto key = [](IMFMediaType* mt) {
        UINT32 w = 0, h = 0, num = 0, den = 1;
        MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(mt, MF_MT_FRAME_RATE, &num, &den);
        return std::make_pair(-(long long)w * h, den ? (double)num / den : 0.0);
    };
    std::stable_sort(types.begin(), types.end(), [&](const ComPtr<IMFMediaType>& a, const ComPtr<IMFMediaType>& b) {
        return key(a.Get()) < key(b.Get());
    });

    std::vector<IMFMediaType*> raw;
    for (auto& t : types) raw.push_back(t.Get());
    RIF(MFCreateStreamDescriptor(m_streamId, (DWORD)raw.size(), raw.data(), &m_desc));
    ComPtr<IMFMediaTypeHandler> ourHandler;
    RIF(m_desc->GetMediaTypeHandler(&ourHandler));
    RIF(ourHandler->SetCurrentMediaType(raw[0]));

    RIF(MFCreateAttributes(&m_attributes, 10));
    RIF(deviceDesc->CopyAllItems(m_attributes.Get()));
    RIF(SetStreamAttributes(m_attributes.Get()));
    RIF(deviceDesc->CopyAllItems(m_desc.Get()));
    RIF(SetStreamAttributes(m_desc.Get()));

    RIF(MFCreateEventQueue(&m_events));
    RIF(MFAllocateSerialWorkQueue(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, &m_workQueue));
    m_deviceEventCb = Microsoft::WRL::Make<AsyncCallback<Stream>>(this, &Stream::OnDeviceStreamEvent, m_workQueue);
    if (!m_deviceEventCb) return E_OUTOFMEMORY;
    return S_OK;
}

HRESULT Stream::SetStreamAttributes(IMFAttributes* attrs)
{
    RIF(attrs->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
    RIF(attrs->SetUINT32(MF_DEVICESTREAM_STREAM_ID, m_streamId));
    RIF(attrs->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
    RIF(attrs->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));
    return S_OK;
}

HRESULT Stream::CheckShutdown() const
{
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (!m_events) return E_UNEXPECTED;
    return S_OK;
}

HRESULT Stream::GetDescriptor(IMFStreamDescriptor** desc)
{
    Guard g(m_lock);
    RIF(CheckShutdown());
    return m_desc.CopyTo(desc);
}

HRESULT Stream::SetMediaStream(IMFMediaStream* deviceStream)
{
    Guard g(m_lock);
    RIF(CheckShutdown());
    m_deviceStream = deviceStream;
    RIF(m_deviceStream->BeginGetEvent(m_deviceEventCb.Get(), m_deviceStream.Get()));
    return S_OK;
}

HRESULT Stream::Shutdown()
{
    Guard g(m_lock);
    m_shutdown = true;
    m_parent.Reset();
    m_deviceStream.Reset();
    m_deviceEventCb.Reset();
    HRESULT hr = S_OK;
    if (m_events)
    {
        hr = m_events->Shutdown();
        m_events.Reset();
    }
    if (m_workQueue)
    {
        MFUnlockWorkQueue(m_workQueue);
        m_workQueue = 0;
    }
    m_desc.Reset();
    m_attributes.Reset();
    return hr;
}

HRESULT Stream::OnStarted()
{
    Guard g(m_lock);
    ComPtr<IMFMediaTypeHandler> handler;
    RIF(m_desc->GetMediaTypeHandler(&handler));
    ComPtr<IMFMediaType> mt;
    RIF(handler->GetCurrentMediaType(&mt));
    MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &m_width, &m_height);
    UINT32 matrix = 0, range = 0;
    mt->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
    mt->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range);
    m_format.bt709 = matrix == MFVideoTransferMatrix_BT709 || (matrix == 0 && m_height >= 720);
    m_format.fullRange = range == MFNominalRange_0_255;
    Log(L"stream started %ux%u, matrix %u, range %u", m_width, m_height, matrix, range);
    return S_OK;
}

void Stream::OnDeviceStreamEvent(IMFAsyncResult* result)
{
    ComPtr<IUnknown> state;
    ComPtr<IMFMediaStream> deviceStream;
    ComPtr<IMFMediaEvent> ev;
    MediaEventType met = MEUnknown;
    HRESULT hr = result->GetState(&state);
    if (SUCCEEDED(hr)) hr = state.As(&deviceStream);
    if (SUCCEEDED(hr)) hr = deviceStream->EndGetEvent(result, &ev);
    if (SUCCEEDED(hr)) hr = ev->GetType(&met);

    if (SUCCEEDED(hr) && met == MEMediaSample)
    {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(ev->GetValue(&value)) && value.vt == VT_UNKNOWN && value.punkVal)
        {
            ComPtr<IMFSample> sample;
            if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&sample))))
            {
                UINT32 w, h;
                YuvFormat format;
                {
                    Guard g(m_lock);
                    w = m_width; h = m_height; format = m_format;
                }
                m_lut.Process(sample.Get(), w, h, format);
            }
        }
        PropVariantClear(&value);
    }
    else if (SUCCEEDED(hr) && met == MEStreamStarted)
    {
        OnStarted();
    }

    ComPtr<Source> parent;
    {
        Guard g(m_lock);
        if (FAILED(CheckShutdown()))
            return;
        if (SUCCEEDED(hr))
        {
            // Pass the event (and the processed sample) on to whoever is reading OpenCamHub.
            m_events->QueueEvent(ev.Get());
            if (met != MEError)
                hr = deviceStream->BeginGetEvent(m_deviceEventCb.Get(), deviceStream.Get());
        }
        if (FAILED(hr))
            parent = m_parent;
    }
    if (parent)
    {
        Log(L"stream event error 0x%08X", hr);
        parent->QueueEvent(MEError, GUID_NULL, hr, nullptr);
    }
}

STDMETHODIMP Stream::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state)
{
    Guard g(m_lock);
    RIF(CheckShutdown());
    return m_events->BeginGetEvent(cb, state);
}

STDMETHODIMP Stream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev)
{
    Guard g(m_lock);
    RIF(CheckShutdown());
    return m_events->EndGetEvent(result, ev);
}

STDMETHODIMP Stream::GetEvent(DWORD flags, IMFMediaEvent** ev)
{
    ComPtr<IMFMediaEventQueue> queue;
    {
        Guard g(m_lock);
        RIF(CheckShutdown());
        queue = m_events;
    }
    return queue->GetEvent(flags, ev);
}

STDMETHODIMP Stream::QueueEvent(MediaEventType met, REFGUID ext, HRESULT status, const PROPVARIANT* value)
{
    Guard g(m_lock);
    RIF(CheckShutdown());
    return m_events->QueueEventParamVar(met, ext, status, value);
}

STDMETHODIMP Stream::GetMediaSource(IMFMediaSource** source)
{
    if (!source) return E_POINTER;
    *source = nullptr;
    Guard g(m_lock);
    RIF(CheckShutdown());
    if (!m_parent) return E_UNEXPECTED;
    return m_parent.CopyTo(source);
}

STDMETHODIMP Stream::GetStreamDescriptor(IMFStreamDescriptor** desc)
{
    if (!desc) return E_POINTER;
    *desc = nullptr;
    Guard g(m_lock);
    RIF(CheckShutdown());
    return m_desc.CopyTo(desc);
}

STDMETHODIMP Stream::RequestSample(IUnknown* token)
{
    ComPtr<IMFMediaStream> device;
    {
        Guard g(m_lock);
        RIF(CheckShutdown());
        device = m_deviceStream;
    }
    if (!device) return MF_E_INVALIDREQUEST;
    return device->RequestSample(token);
}

STDMETHODIMP Stream::SetStreamState(MF_STREAM_STATE state)
{
    ComPtr<IMFMediaStream2> device;
    {
        Guard g(m_lock);
        RIF(CheckShutdown());
        if (!m_deviceStream) return MF_E_INVALIDREQUEST;
        RIF(m_deviceStream.As(&device));
    }
    return device->SetStreamState(state);
}

STDMETHODIMP Stream::GetStreamState(MF_STREAM_STATE* state)
{
    if (!state) return E_POINTER;
    ComPtr<IMFMediaStream2> device;
    {
        Guard g(m_lock);
        RIF(CheckShutdown());
        if (!m_deviceStream) { *state = MF_STREAM_STATE_STOPPED; return S_OK; }
        RIF(m_deviceStream.As(&device));
    }
    return device->GetStreamState(state);
}

//////////////////////////////////////////////////////////////////////////////////////////
// Source

Source::~Source()
{
    Shutdown();
}

HRESULT Source::Initialize(IMFMediaSource* device)
{
    Guard g(m_lock);
    if (m_state != State::Invalid) return MF_E_ALREADY_INITIALIZED;
    if (!device) return E_POINTER;
    m_device = device;

    RIF(CreateSourceAttributes());
    RIF(MFCreateEventQueue(&m_events));
    RIF(MFAllocateSerialWorkQueue(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, &m_workQueue));
    m_deviceEventCb = Microsoft::WRL::Make<AsyncCallback<Source>>(this, &Source::OnDeviceEvent, m_workQueue);
    if (!m_deviceEventCb) return E_OUTOFMEMORY;
    RIF(m_device->BeginGetEvent(m_deviceEventCb.Get(), m_device.Get()));
    RIF(m_device->CreatePresentationDescriptor(&m_devicePd));

    // Use the camera's colour capture stream (or its preview stream if it has no capture stream).
    DWORD count = 0;
    RIF(m_devicePd->GetStreamDescriptorCount(&count));
    bool found = false, havePreview = false;
    DWORD previewIndex = 0;
    for (DWORD i = 0; i < count && !found; i++)
    {
        ComPtr<IMFStreamDescriptor> sd;
        BOOL selected = FALSE;
        RIF(m_devicePd->GetStreamDescriptorByIndex(i, &selected, &sd));
        GUID category = GUID_NULL;
        UINT32 type = 0;
        sd->GetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, &category);
        sd->GetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, &type);
        if (type != MFFrameSourceTypes_Color) continue;
        if (category == PINNAME_VIDEO_CAPTURE) { m_deviceStreamIndex = i; found = true; }
        else if (category == PINNAME_VIDEO_PREVIEW && !havePreview) { previewIndex = i; havePreview = true; }
    }
    if (!found && havePreview) { m_deviceStreamIndex = previewIndex; found = true; }
    if (!found)
    {
        Log(L"no colour stream on the camera");
        return MF_E_CAPTURE_SOURCE_NO_VIDEO_STREAM_PRESENT;
    }

    ComPtr<IMFStreamDescriptor> deviceSd;
    BOOL selected = FALSE;
    RIF(m_devicePd->GetStreamDescriptorByIndex(m_deviceStreamIndex, &selected, &deviceSd));
    m_stream = Microsoft::WRL::Make<Stream>();
    if (!m_stream) return E_OUTOFMEMORY;
    RIF(m_stream->Initialize(this, 0, deviceSd.Get()));

    ComPtr<IMFStreamDescriptor> ourSd;
    RIF(m_stream->GetDescriptor(&ourSd));
    IMFStreamDescriptor* list[] = { ourSd.Get() };
    RIF(MFCreatePresentationDescriptor(1, list, &m_pd));

    m_state = State::Stopped;
    Log(L"source initialised");
    return S_OK;
}

HRESULT Source::CreateSourceAttributes()
{
    RIF(MFCreateAttributes(&m_attributes, 1));
    // A legacy profile is required so apps that don't know about camera profiles still work.
    ComPtr<IMFSensorProfileCollection> profiles;
    ComPtr<IMFSensorProfile> profile;
    RIF(MFCreateSensorProfileCollection(&profiles));
    RIF(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &profile));
    RIF(profile->AddProfileFilter(0, L"((RES==;FRT<=60,1;SUT==))"));
    RIF(profiles->AddProfile(profile.Get()));
    RIF(m_attributes->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.Get()));
    return S_OK;
}

HRESULT Source::CheckShutdown() const
{
    if (m_state == State::Shutdown) return MF_E_SHUTDOWN;
    if (!m_events || !m_stream || !m_device) return E_UNEXPECTED;
    return S_OK;
}

HRESULT Source::FindStream(DWORD streamId, DWORD* index, bool* selected)
{
    DWORD count = 0;
    RIF(m_pd->GetStreamDescriptorCount(&count));
    for (DWORD i = 0; i < count; i++)
    {
        ComPtr<IMFStreamDescriptor> sd;
        BOOL sel = FALSE;
        RIF(m_pd->GetStreamDescriptorByIndex(i, &sel, &sd));
        DWORD id = 0;
        RIF(sd->GetStreamIdentifier(&id));
        if (id == streamId) { *index = i; *selected = !!sel; return S_OK; }
    }
    return MF_E_NOT_FOUND;
}

STDMETHODIMP Source::Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* startPos)
{
    if (!pd || !startPos) return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;

    Guard g(m_lock);
    RIF(CheckShutdown());
    if (m_state == State::Invalid) return MF_E_INVALID_STATE_TRANSITION;

    DWORD count = 0;
    RIF(pd->GetStreamDescriptorCount(&count));
    if (count != 1) return E_INVALIDARG;

    bool anySelected = false;
    for (DWORD i = 0; i < count; i++)
    {
        ComPtr<IMFStreamDescriptor> sd;
        BOOL selected = FALSE;
        RIF(pd->GetStreamDescriptorByIndex(i, &selected, &sd));
        DWORD id = 0;
        RIF(sd->GetStreamIdentifier(&id));
        DWORD ourIndex = 0;
        bool wasSelected = false;
        RIF(FindStream(id, &ourIndex, &wasSelected));
        if (!selected)
        {
            if (wasSelected)
            {
                RIF(m_pd->DeselectStream(ourIndex));
                RIF(m_devicePd->DeselectStream(m_deviceStreamIndex));
            }
            continue;
        }
        anySelected = true;
        RIF(m_pd->SelectStream(ourIndex));
        RIF(m_devicePd->SelectStream(m_deviceStreamIndex));

        // Ask the camera for the format the app picked on OpenCamHub.
        ComPtr<IMFMediaTypeHandler> ourHandler, deviceHandler;
        ComPtr<IMFMediaType> current;
        RIF(sd->GetMediaTypeHandler(&ourHandler));
        RIF(ourHandler->GetCurrentMediaType(&current));
        ComPtr<IMFStreamDescriptor> deviceSd;
        BOOL devSel = FALSE;
        RIF(m_devicePd->GetStreamDescriptorByIndex(m_deviceStreamIndex, &devSel, &deviceSd));
        RIF(deviceSd->GetMediaTypeHandler(&deviceHandler));
        RIF(deviceHandler->SetCurrentMediaType(current.Get()));
    }
    if (!anySelected) return E_INVALIDARG;

    RIF(m_device->Start(m_devicePd.Get(), timeFormat, startPos));
    m_state = State::Started;
    // MENewStream / MESourceStarted come from the camera and are forwarded in OnDeviceEvent.
    return S_OK;
}

STDMETHODIMP Source::Stop()
{
    Guard g(m_lock);
    RIF(CheckShutdown());
    if (m_state != State::Started) return MF_E_INVALID_STATE_TRANSITION;
    RIF(m_device->Stop());
    m_state = State::Stopped;
    return S_OK;
}

STDMETHODIMP Source::Pause()
{
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP Source::Shutdown()
{
    ComPtr<IMFMediaSource> device;
    ComPtr<Stream> stream;
    ComPtr<IMFMediaEventQueue> events;
    {
        Guard g(m_lock);
        if (m_state == State::Shutdown) return S_OK;
        m_state = State::Shutdown;
        device.Swap(m_device);
        stream.Swap(m_stream);
        events.Swap(m_events);
        m_devicePd.Reset();
        m_pd.Reset();
        m_attributes.Reset();
        m_deviceEventCb.Reset();
    }
    if (device) device->Shutdown();
    if (stream) stream->Shutdown();
    if (events) events->Shutdown();
    if (m_workQueue)
    {
        MFUnlockWorkQueue(m_workQueue);
        m_workQueue = 0;
    }
    return S_OK;
}

void Source::OnDeviceEvent(IMFAsyncResult* result)
{
    ComPtr<IUnknown> state;
    ComPtr<IMFMediaSource> device;
    ComPtr<IMFMediaEvent> ev;
    MediaEventType met = MEUnknown;
    HRESULT hr = result->GetState(&state);
    if (SUCCEEDED(hr)) hr = state.As(&device);
    if (SUCCEEDED(hr)) hr = device->EndGetEvent(result, &ev);
    if (SUCCEEDED(hr)) hr = ev->GetType(&met);

    Guard g(m_lock);
    if (FAILED(CheckShutdown())) return;
    if (FAILED(hr))
    {
        Log(L"device event error 0x%08X", hr);
        return;
    }

    if (met == MENewStream || met == MEUpdatedStream)
    {
        // The camera announces its stream; hand it to our stream and announce ours instead.
        PROPVARIANT value;
        PropVariantInit(&value);
        ComPtr<IMFMediaStream> deviceStream;
        if (SUCCEEDED(ev->GetValue(&value)) && value.vt == VT_UNKNOWN && value.punkVal
            && SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&deviceStream))))
        {
            ComPtr<IMFStreamDescriptor> sd;
            DWORD id = 0;
            if (SUCCEEDED(deviceStream->GetStreamDescriptor(&sd)) && SUCCEEDED(sd->GetStreamIdentifier(&id))
                && id == m_stream->DeviceStreamId())
            {
                if (SUCCEEDED(m_stream->SetMediaStream(deviceStream.Get())))
                {
                    ComPtr<IUnknown> unk;
                    m_stream.As(&unk);
                    m_events->QueueEventParamUnk(met, GUID_NULL, S_OK, unk.Get());
                }
            }
        }
        PropVariantClear(&value);
    }
    else
    {
        if (met == MESourceStopped) m_state = State::Stopped;
        m_events->QueueEvent(ev.Get());
    }

    hr = m_device->BeginGetEvent(m_deviceEventCb.Get(), m_device.Get());
    if (FAILED(hr)) Log(L"BeginGetEvent on device failed 0x%08X", hr);
}

STDMETHODIMP Source::CreatePresentationDescriptor(IMFPresentationDescriptor** pd)
{
    if (!pd) return E_POINTER;
    *pd = nullptr;
    Guard g(m_lock);
    RIF(CheckShutdown());
    return m_pd->Clone(pd);
}

STDMETHODIMP Source::GetCharacteristics(DWORD* characteristics)
{
    if (!characteristics) return E_POINTER;
    Guard g(m_lock);
    RIF(CheckShutdown());
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP Source::GetSourceAttributes(IMFAttributes** attrs)
{
    if (!attrs) return E_POINTER;
    *attrs = nullptr;
    Guard g(m_lock);
    RIF(CheckShutdown());
    return m_attributes.CopyTo(attrs); // must be the live store, not a copy
}

STDMETHODIMP Source::GetStreamAttributes(DWORD streamId, IMFAttributes** attrs)
{
    if (!attrs) return E_POINTER;
    *attrs = nullptr;
    Guard g(m_lock);
    RIF(CheckShutdown());
    if (streamId != m_stream->StreamId()) return MF_E_INVALIDSTREAMNUMBER;
    return m_stream->Attributes().CopyTo(attrs);
}

STDMETHODIMP Source::SetD3DManager(IUnknown*)
{
    // Keep frames in system memory: the LUT is applied on the CPU, and reading GPU frames back would cost more.
    return S_OK;
}

STDMETHODIMP Source::GetService(REFGUID service, REFIID riid, LPVOID* object)
{
    ComPtr<IMFGetService> gs;
    {
        Guard g(m_lock);
        RIF(CheckShutdown());
        if (FAILED(m_device.As(&gs))) return MF_E_UNSUPPORTED_SERVICE;
    }
    return gs->GetService(service, riid, object);
}

STDMETHODIMP Source::KsProperty(PKSPROPERTY prop, ULONG propLen, LPVOID data, ULONG dataLen, ULONG* returned)
{
    ComPtr<IKsControl> ks;
    {
        Guard g(m_lock);
        RIF(CheckShutdown());
        if (FAILED(m_device.As(&ks))) return E_NOTIMPL;
    }
    return ks->KsProperty(prop, propLen, data, dataLen, returned);
}

STDMETHODIMP Source::KsMethod(PKSMETHOD method, ULONG methodLen, LPVOID data, ULONG dataLen, ULONG* returned)
{
    ComPtr<IKsControl> ks;
    {
        Guard g(m_lock);
        RIF(CheckShutdown());
        if (FAILED(m_device.As(&ks))) return E_NOTIMPL;
    }
    return ks->KsMethod(method, methodLen, data, dataLen, returned);
}

STDMETHODIMP Source::KsEvent(PKSEVENT ev, ULONG evLen, LPVOID data, ULONG dataLen, ULONG* returned)
{
    ComPtr<IKsControl> ks;
    {
        Guard g(m_lock);
        RIF(CheckShutdown());
        if (FAILED(m_device.As(&ks))) return E_NOTIMPL;
    }
    return ks->KsEvent(ev, evLen, data, dataLen, returned);
}

STDMETHODIMP Source::SetDefaultAllocator(DWORD, IUnknown* allocator)
{
    ComPtr<IMFSampleAllocatorControl> control;
    DWORD deviceStreamId;
    {
        Guard g(m_lock);
        RIF(CheckShutdown());
        RIF(m_device.As(&control));
        deviceStreamId = m_stream->DeviceStreamId();
    }
    return control->SetDefaultAllocator(deviceStreamId, allocator);
}

STDMETHODIMP Source::GetAllocatorUsage(DWORD, DWORD* inputStreamId, MFSampleAllocatorUsage* usage)
{
    if (!inputStreamId || !usage) return E_POINTER;
    Guard g(m_lock);
    RIF(CheckShutdown());
    // The LUT edits the camera's samples in place.
    *usage = MFSampleAllocatorUsage_DoesNotAllocate;
    *inputStreamId = m_stream->StreamId();
    return S_OK;
}

STDMETHODIMP Source::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state)
{
    Guard g(m_lock);
    RIF(CheckShutdown());
    return m_events->BeginGetEvent(cb, state);
}

STDMETHODIMP Source::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev)
{
    Guard g(m_lock);
    RIF(CheckShutdown());
    return m_events->EndGetEvent(result, ev);
}

STDMETHODIMP Source::GetEvent(DWORD flags, IMFMediaEvent** ev)
{
    ComPtr<IMFMediaEventQueue> queue;
    {
        Guard g(m_lock);
        RIF(CheckShutdown());
        queue = m_events;
    }
    return queue->GetEvent(flags, ev);
}

STDMETHODIMP Source::QueueEvent(MediaEventType met, REFGUID ext, HRESULT status, const PROPVARIANT* value)
{
    Guard g(m_lock);
    RIF(CheckShutdown());
    return m_events->QueueEventParamVar(met, ext, status, value);
}

//////////////////////////////////////////////////////////////////////////////////////////
// Activate

HRESULT Activate::Initialize()
{
    RIF(MFCreateAttributes(&m_attrs, 3));
    // Ask Frame Server to hand us the physical camera it associates with OpenCamHub.
    RIF(m_attrs->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1));
    return S_OK;
}

static HRESULT OpenFacecamDirectly(IMFMediaSource** source)
{
    ComPtr<IMFAttributes> attrs;
    RIF(MFCreateAttributes(&attrs, 1));
    RIF(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    RIF(MFEnumDeviceSources(attrs.Get(), &devices, &count));
    HRESULT hr = MF_E_NOT_FOUND;
    for (UINT32 i = 0; i < count; i++)
    {
        wchar_t* link = nullptr;
        UINT32 len = 0;
        if (hr == MF_E_NOT_FOUND && SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len)))
        {
            std::wstring lower(link);
            for (auto& c : lower) c = towlower(c);
            if (lower.find(FACECAM_HWID) != std::wstring::npos)
                hr = devices[i]->ActivateObject(IID_PPV_ARGS(source));
            CoTaskMemFree(link);
        }
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    return hr;
}

STDMETHODIMP Activate::ActivateObject(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    ComPtr<IMFMediaSource> device;
    ComPtr<IMFCollection> associated;
    if (SUCCEEDED(m_attrs->GetUnknown(MF_VIRTUALCAMERA_ASSOCIATED_CAMERA_SOURCES, IID_PPV_ARGS(&associated))))
    {
        DWORD n = 0;
        RIF(associated->GetElementCount(&n));
        if (n < 1) return MF_E_NOT_FOUND;
        ComPtr<IUnknown> unk;
        ComPtr<IMFActivate> act;
        RIF(associated->GetElement(0, &unk));
        RIF(unk.As(&act));
        RIF(act->ActivateObject(IID_PPV_ARGS(&device)));
        Log(L"wrapping the camera Frame Server provided");
    }
    else
    {
        RIF(OpenFacecamDirectly(&device));
        Log(L"wrapping the Facecam opened directly");
    }

    auto source = Microsoft::WRL::Make<Source>();
    if (!source) return E_OUTOFMEMORY;
    RIF(source->Initialize(device.Get()));
    m_source = source;
    return source->QueryInterface(riid, ppv);
}

STDMETHODIMP Activate::ShutdownObject()
{
    return S_OK;
}

STDMETHODIMP Activate::DetachObject()
{
    m_source.Reset();
    return S_OK;
}
