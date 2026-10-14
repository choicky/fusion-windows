#include "tsf/register.hpp"

#include "tsf/diag.hpp"
#include "tsf/guids.hpp"

#include <msctf.h>
#include <objbase.h>

#include <cstdio>
#include <cwchar>
#include <vector>

namespace fusion::tsf {

namespace {

wchar_t g_module_path[MAX_PATH] = {};

// Distinct LANGIDs of the currently installed keyboard layouts: the profile is
// registered for the languages the user can already type in.
std::vector<LANGID> installed_langids() {
  std::vector<LANGID> langids;
  int count = GetKeyboardLayoutList(0, nullptr);
  if (count <= 0) {
    langids.push_back(static_cast<LANGID>(GetUserDefaultUILanguage()));
    return langids;
  }
  std::vector<HKL> layouts(static_cast<std::size_t>(count));
  GetKeyboardLayoutList(count, layouts.data());
  for (HKL hkl : layouts) {
    const LANGID langid = LOWORD(hkl);
    bool seen = false;
    for (LANGID existing : langids) {
      if (existing == langid) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      langids.push_back(langid);
    }
  }
  return langids;
}

LSTATUS set_registry_value(HKEY key, const wchar_t *name, const wchar_t *value) {
  return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value),
                        static_cast<DWORD>((std::wcslen(value) + 1) * sizeof(wchar_t)));
}

HRESULT write_com_registration() {
  wchar_t clsid_text[64];
  StringFromGUID2(CLSID_FusionTsfTextService, clsid_text, 64);
  wchar_t key_path[256];
  swprintf(key_path, std::size(key_path), L"SOFTWARE\\Classes\\CLSID\\%ls", clsid_text);
  HKEY clsid_key = nullptr;
  LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key_path, 0, nullptr, 0, KEY_SET_VALUE,
                                   nullptr, &clsid_key, nullptr);
  if (status != ERROR_SUCCESS) {
    return HRESULT_FROM_WIN32(status);
  }
  status = set_registry_value(clsid_key, nullptr, L"Fusion Stage 2.5 TSF Text Service (dev)");
  HKEY server_key = nullptr;
  if (status == ERROR_SUCCESS) {
    status = RegCreateKeyExW(clsid_key, L"InprocServer32", 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                             &server_key, nullptr);
  }
  if (status == ERROR_SUCCESS) {
    status = set_registry_value(server_key, nullptr, g_module_path);
  }
  if (status == ERROR_SUCCESS) {
    status = set_registry_value(server_key, L"ThreadingModel", L"Apartment");
  }
  if (server_key != nullptr) {
    RegCloseKey(server_key);
  }
  RegCloseKey(clsid_key);
  return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
}

HRESULT delete_com_registration() {
  wchar_t clsid_text[64];
  StringFromGUID2(CLSID_FusionTsfTextService, clsid_text, 64);
  wchar_t key_path[256];
  swprintf(key_path, std::size(key_path), L"SOFTWARE\\Classes\\CLSID\\%ls", clsid_text);
  const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key_path);
  return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ? S_OK
                                                                   : HRESULT_FROM_WIN32(status);
}

const wchar_t *kProfileDescription = L"Fusion Stage 2.5 (Mock Backend, development)";

} // namespace

void remember_module_path(HMODULE module) {
  if (module != nullptr) {
    GetModuleFileNameW(module, g_module_path, MAX_PATH);
  }
}

HRESULT RegisterServer(HMODULE module) {
  remember_module_path(module);
  if (g_module_path[0] == L'\0') {
    return E_FAIL;
  }
  HRESULT result = write_com_registration();
  if (FAILED(result)) {
    return result;
  }
  ITfInputProcessorProfiles *profiles_raw = nullptr;
  result =
      CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                       IID_ITfInputProcessorProfiles, reinterpret_cast<void **>(&profiles_raw));
  if (FAILED(result)) {
    return result;
  }
  result = profiles_raw->Register(CLSID_FusionTsfTextService);
  if (FAILED(result)) {
    profiles_raw->Release();
    return result;
  }
  ITfInputProcessorProfileMgr *profile_mgr_raw = nullptr;
  result = profiles_raw->QueryInterface(IID_ITfInputProcessorProfileMgr,
                                        reinterpret_cast<void **>(&profile_mgr_raw));
  if (FAILED(result)) {
    profiles_raw->Release();
    return result;
  }
  ITfCategoryMgr *category_raw = nullptr;
  result = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                            reinterpret_cast<void **>(&category_raw));
  if (FAILED(result)) {
    profile_mgr_raw->Release();
    profiles_raw->Release();
    return result;
  }
  for (LANGID langid : installed_langids()) {
    result = profile_mgr_raw->RegisterProfile(
        CLSID_FusionTsfTextService, langid, GUID_FusionTsfProfile, kProfileDescription,
        static_cast<ULONG>(std::wcslen(kProfileDescription)), nullptr, 0, 0, 0, 0, TRUE, 0);
    if (FAILED(result))
      break;
    result = profiles_raw->EnableLanguageProfile(CLSID_FusionTsfTextService, langid,
                                                 GUID_FusionTsfProfile, TRUE);
    if (FAILED(result))
      break;
  }
  if (SUCCEEDED(result))
    result = category_raw->RegisterCategory(CLSID_FusionTsfTextService, GUID_TFCAT_TIP_KEYBOARD,
                                            CLSID_FusionTsfTextService);
  category_raw->Release();
  profile_mgr_raw->Release();
  profiles_raw->Release();
  return result;
}

HRESULT UnregisterServer() {
  ITfInputProcessorProfiles *profiles_raw = nullptr;
  HRESULT result =
      CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                       IID_ITfInputProcessorProfiles, reinterpret_cast<void **>(&profiles_raw));
  if (FAILED(result)) {
    return result;
  }
  ITfInputProcessorProfileMgr *profile_mgr_raw = nullptr;
  result = profiles_raw->QueryInterface(IID_ITfInputProcessorProfileMgr,
                                        reinterpret_cast<void **>(&profile_mgr_raw));
  if (FAILED(result)) {
    profiles_raw->Release();
    return result;
  }
  ITfCategoryMgr *category_raw = nullptr;
  result = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                            reinterpret_cast<void **>(&category_raw));
  if (FAILED(result)) {
    profile_mgr_raw->Release();
    profiles_raw->Release();
    return result;
  }
  HRESULT first_failure = S_OK;
  const auto observe = [&](HRESULT hr) {
    if (FAILED(hr) && SUCCEEDED(first_failure))
      first_failure = hr;
  };
  observe(category_raw->UnregisterCategory(CLSID_FusionTsfTextService, GUID_TFCAT_TIP_KEYBOARD,
                                           CLSID_FusionTsfTextService));
  for (LANGID langid : installed_langids()) {
    observe(profile_mgr_raw->UnregisterProfile(CLSID_FusionTsfTextService, langid,
                                               GUID_FusionTsfProfile, 0));
  }
  observe(profiles_raw->Unregister(CLSID_FusionTsfTextService));
  category_raw->Release();
  profile_mgr_raw->Release();
  profiles_raw->Release();
  observe(delete_com_registration());
  return first_failure;
}

} // namespace fusion::tsf
