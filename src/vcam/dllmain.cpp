#include "Source.h"
#include "../common/OpenCamHub.h"

static HMODULE g_module = nullptr;

class Factory : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory>
{
public:
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto activate = Microsoft::WRL::Make<Activate>();
        if (!activate) return E_OUTOFMEMORY;
        HRESULT hr = activate->Initialize();
        if (FAILED(hr)) return hr;
        return activate->QueryInterface(riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL) override { return S_OK; }
};

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != CLSID_OpenCamHubSource) return CLASS_E_CLASSNOTAVAILABLE;
    auto factory = Microsoft::WRL::Make<Factory>();
    if (!factory) return E_OUTOFMEMORY;
    return factory->QueryInterface(riid, ppv);
}

// Frame Server keeps the DLL for its lifetime; never unloading avoids races with background LUT builds.
STDAPI DllCanUnloadNow()
{
    return S_FALSE;
}

// regsvr32 vcam.dll: registers the media source for all users (needs administrator rights).
STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    std::wstring key = L"Software\\Classes\\CLSID\\" OPENCAMHUB_CLSID_STR L"\\InprocServer32";
    HKEY hkey = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hkey, nullptr);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    st = RegSetValueExW(hkey, nullptr, 0, REG_SZ, (const BYTE*)path, (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
    const wchar_t both[] = L"Both";
    if (st == ERROR_SUCCESS)
        st = RegSetValueExW(hkey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)both, sizeof(both));
    RegCloseKey(hkey);
    return HRESULT_FROM_WIN32(st);
}

STDAPI DllUnregisterServer()
{
    LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID\\" OPENCAMHUB_CLSID_STR);
    return (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(st);
}
