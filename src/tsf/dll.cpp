#include "tsf/guids.hpp"
#include "tsf/register.hpp"
#include "tsf/service.hpp"

#include <msctf.h>
#include <objbase.h>
#include <windows.h>

#include <atomic>
#include <new>

namespace fusion::tsf {

namespace {

HMODULE g_module = nullptr;
std::atomic<ULONG> g_factory_locks{0};

class ClassFactory final : public IClassFactory {
public:
  // IClassFactory
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (ppv == nullptr) {
      return E_POINTER;
    }
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
      *ppv = static_cast<IClassFactory *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_count_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = --ref_count_;
    if (count == 0) {
      delete this;
    }
    return count;
  }
  STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid, void **ppv) override {
    if (ppv == nullptr) {
      return E_POINTER;
    }
    *ppv = nullptr;
    if (outer != nullptr) {
      return CLASS_E_NOAGGREGATION;
    }
    auto *service = new (std::nothrow) TextService();
    if (service == nullptr) {
      return E_OUTOFMEMORY;
    }
    const HRESULT result = service->QueryInterface(riid, ppv);
    service->Release();
    return result;
  }
  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock) {
      ++g_factory_locks;
    } else {
      --g_factory_locks;
    }
    return S_OK;
  }

private:
  ~ClassFactory() = default;
  std::atomic<ULONG> ref_count_{1};
};

} // namespace

} // namespace fusion::tsf

using namespace fusion::tsf;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    DisableThreadLibraryCalls(instance);
  }
  (void)reserved;
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv) {
  if (ppv == nullptr) {
    return E_POINTER;
  }
  *ppv = nullptr;
  if (!IsEqualCLSID(rclsid, CLSID_FusionTsfTextService)) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  auto *factory = new (std::nothrow) ClassFactory();
  if (factory == nullptr) {
    return E_OUTOFMEMORY;
  }
  const HRESULT result = factory->QueryInterface(riid, ppv);
  factory->Release();
  return result;
}

STDAPI DllCanUnloadNow() {
  // Development shell: keep the DLL loaded for the process lifetime; unload
  // decisions are not part of this stage.
  return S_FALSE;
}

STDAPI DllRegisterServer() { return RegisterServer(g_module); }

STDAPI DllUnregisterServer() { return UnregisterServer(); }
