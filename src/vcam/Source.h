// OpenCamHub virtual camera media source.
//
// Windows' camera service (Frame Server) loads this DLL when an app opens the "OpenCamHub" camera. The source
// wraps the physical Facecam that Frame Server hands us, passes its NV12 stream through, and applies the
// colour LUT chosen in settings.ini. Nothing runs while no app is using the camera.
//
// The structure follows Microsoft's VirtualCamera sample (AugmentedMediaSource), MIT licence:
// https://github.com/microsoft/Windows-Camera/tree/master/Samples/VirtualCamera
#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfvirtualcamera.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <wrl.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>
#include "Lut.h"
#include "../common/Effects.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::ChainInterfaces;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

// Debug output, visible with DebugView or a debugger attached to the FrameServer service.
void Log(const wchar_t* fmt, ...);

class SrwLock
{
public:
    void lock() { AcquireSRWLockExclusive(&m_lock); }
    void unlock() { ReleaseSRWLockExclusive(&m_lock); }
private:
    SRWLOCK m_lock = SRWLOCK_INIT;
};
using Guard = std::lock_guard<SrwLock>;

// IMFAsyncCallback that forwards to a member function on a serial work queue.
template <class T>
class AsyncCallback : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFAsyncCallback>
{
public:
    AsyncCallback(T* parent, void (T::*fn)(IMFAsyncResult*), DWORD queue) : m_parent(parent), m_fn(fn), m_queue(queue) {}
    STDMETHODIMP GetParameters(DWORD* flags, DWORD* queue) override { *flags = 0; *queue = m_queue; return S_OK; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override { (m_parent.Get()->*m_fn)(result); return S_OK; }
private:
    ComPtr<T> m_parent;
    void (T::*m_fn)(IMFAsyncResult*);
    DWORD m_queue;
};

class Source;

// Applies mirroring and the LUT from settings.ini to each frame.
class FrameProcessor
{
public:
    void Process(IMFSample* sample, UINT width, UINT height, YuvFormat format);
private:
    FrameEffects m_fx;
};

class Stream : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>>
{
public:
    HRESULT Initialize(Source* parent, DWORD streamId, IMFStreamDescriptor* deviceStreamDesc);
    HRESULT SetMediaStream(IMFMediaStream* deviceStream);
    HRESULT Shutdown();
    HRESULT GetDescriptor(IMFStreamDescriptor** desc);
    DWORD DeviceStreamId() const { return m_deviceStreamId; }
    DWORD StreamId() const { return m_streamId; }
    ComPtr<IMFAttributes> Attributes() { return m_attributes; }
    void OnDeviceStreamEvent(IMFAsyncResult* result);

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT status, const PROPVARIANT* value) override;
    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** desc) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;
    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

private:
    HRESULT CheckShutdown() const;
    HRESULT OnStarted();
    HRESULT SetStreamAttributes(IMFAttributes* attrs);

    SrwLock m_lock;
    ComPtr<Source> m_parent;
    ComPtr<IMFMediaStream> m_deviceStream;
    ComPtr<IMFStreamDescriptor> m_desc;
    ComPtr<IMFMediaEventQueue> m_events;
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<AsyncCallback<Stream>> m_deviceEventCb;
    bool m_shutdown = false;
    DWORD m_streamId = 0;
    DWORD m_deviceStreamId = 0;
    DWORD m_workQueue = 0;
    UINT32 m_width = 0, m_height = 0;
    YuvFormat m_format;
    FrameProcessor m_lut;
};

class Source : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
    ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>, IMFGetService, IKsControl, IMFSampleAllocatorControl>
{
public:
    ~Source();
    HRESULT Initialize(IMFMediaSource* device);

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT status, const PROPVARIANT* value) override;
    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override;
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* startPos) override;
    STDMETHODIMP Stop() override;
    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attrs) override;
    STDMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attrs) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;
    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;
    // IKsControl: forwarded to the Facecam, so apps can still adjust the real camera through OpenCamHub.
    STDMETHODIMP KsProperty(PKSPROPERTY prop, ULONG propLen, LPVOID data, ULONG dataLen, ULONG* returned) override;
    STDMETHODIMP KsMethod(PKSMETHOD method, ULONG methodLen, LPVOID data, ULONG dataLen, ULONG* returned) override;
    STDMETHODIMP KsEvent(PKSEVENT ev, ULONG evLen, LPVOID data, ULONG dataLen, ULONG* returned) override;
    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD streamId, IUnknown* allocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) override;

    void OnDeviceEvent(IMFAsyncResult* result);

private:
    enum class State { Invalid, Stopped, Started, Shutdown };
    HRESULT CheckShutdown() const;
    HRESULT CreateSourceAttributes();
    HRESULT FindStream(DWORD streamId, DWORD* index, bool* selected);

    SrwLock m_lock;
    State m_state = State::Invalid;
    DWORD m_workQueue = 0;
    ComPtr<IMFMediaEventQueue> m_events;
    ComPtr<IMFPresentationDescriptor> m_pd;
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<Stream> m_stream;
    ComPtr<IMFMediaSource> m_device;
    ComPtr<IMFPresentationDescriptor> m_devicePd;
    ComPtr<AsyncCallback<Source>> m_deviceEventCb;
    DWORD m_deviceStreamIndex = 0;
};

// The object Frame Server creates from our CLSID. It hands us the Facecam's media source.
class Activate : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>>
{
public:
    HRESULT Initialize();

    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

#define FWD(name, params, args) STDMETHODIMP name params override { return m_attrs->name args; }
    FWD(GetItem, (REFGUID k, PROPVARIANT* v), (k, v))
    FWD(GetItemType, (REFGUID k, MF_ATTRIBUTE_TYPE* t), (k, t))
    FWD(CompareItem, (REFGUID k, REFPROPVARIANT v, BOOL* r), (k, v, r))
    FWD(Compare, (IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r), (a, m, r))
    FWD(GetUINT32, (REFGUID k, UINT32* v), (k, v))
    FWD(GetUINT64, (REFGUID k, UINT64* v), (k, v))
    FWD(GetDouble, (REFGUID k, double* v), (k, v))
    FWD(GetGUID, (REFGUID k, GUID* v), (k, v))
    FWD(GetStringLength, (REFGUID k, UINT32* v), (k, v))
    FWD(GetString, (REFGUID k, LPWSTR s, UINT32 n, UINT32* l), (k, s, n, l))
    FWD(GetAllocatedString, (REFGUID k, LPWSTR* s, UINT32* l), (k, s, l))
    FWD(GetBlobSize, (REFGUID k, UINT32* v), (k, v))
    FWD(GetBlob, (REFGUID k, UINT8* b, UINT32 n, UINT32* l), (k, b, n, l))
    FWD(GetAllocatedBlob, (REFGUID k, UINT8** b, UINT32* l), (k, b, l))
    FWD(GetUnknown, (REFGUID k, REFIID i, LPVOID* p), (k, i, p))
    FWD(SetItem, (REFGUID k, REFPROPVARIANT v), (k, v))
    FWD(DeleteItem, (REFGUID k), (k))
    FWD(DeleteAllItems, (), ())
    FWD(SetUINT32, (REFGUID k, UINT32 v), (k, v))
    FWD(SetUINT64, (REFGUID k, UINT64 v), (k, v))
    FWD(SetDouble, (REFGUID k, double v), (k, v))
    FWD(SetGUID, (REFGUID k, REFGUID v), (k, v))
    FWD(SetString, (REFGUID k, LPCWSTR v), (k, v))
    FWD(SetBlob, (REFGUID k, const UINT8* b, UINT32 n), (k, b, n))
    FWD(SetUnknown, (REFGUID k, IUnknown* u), (k, u))
    FWD(LockStore, (), ())
    FWD(UnlockStore, (), ())
    FWD(GetCount, (UINT32* n), (n))
    FWD(GetItemByIndex, (UINT32 i, GUID* k, PROPVARIANT* v), (i, k, v))
    FWD(CopyAllItems, (IMFAttributes* d), (d))
#undef FWD

private:
    ComPtr<IMFAttributes> m_attrs;
    ComPtr<Source> m_source;
};
