// Development-only activation/query helper for the Stage 2.5 TSF fixture.
// Uses the real TSF API with session-scope activation.
#include <initguid.h>
#include <msctf.h>
#include <objbase.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

// {8B7C4A90-5E2D-4B1F-9A31-6C0F42D1E7A4}
DEFINE_GUID(CLSID_FusionTsfTextService, 0x8b7c4a90, 0x5e2d, 0x4b1f, 0x9a, 0x31, 0x6c, 0x0f, 0x42,
            0xd1, 0xe7, 0xa4);
// {D4A1F6B2-3C58-4E77-8A90-1B5D9C06E4F1}
DEFINE_GUID(GUID_FusionTsfProfile, 0xd4a1f6b2, 0x3c58, 0x4e77, 0x8a, 0x90, 0x1b, 0x5d, 0x9c, 0x06,
            0xe4, 0xf1);

namespace {

const wchar_t *kStateFile = L"FusionTsfPrevProfile.bin";

bool save_previous(const TF_INPUTPROCESSORPROFILE &profile) {
  wchar_t path[MAX_PATH];
  if (GetTempPathW(MAX_PATH, path) == 0) {
    return false;
  }
  wcscat(path, kStateFile);
  HANDLE file =
      CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok =
      WriteFile(file, &profile, sizeof(profile), &written, nullptr) && written == sizeof(profile);
  CloseHandle(file);
  return ok != FALSE;
}

bool load_previous(TF_INPUTPROCESSORPROFILE &profile) {
  wchar_t path[MAX_PATH];
  if (GetTempPathW(MAX_PATH, path) == 0) {
    return false;
  }
  wcscat(path, kStateFile);
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD read_bytes = 0;
  const BOOL ok = ReadFile(file, &profile, sizeof(profile), &read_bytes, nullptr) &&
                  read_bytes == sizeof(profile);
  CloseHandle(file);
  return ok != FALSE;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: activate [on|off|query]\n");
    return 2;
  }
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr)) {
    return 1;
  }
  ITfInputProcessorProfileMgr *mgr = nullptr;
  hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                        IID_ITfInputProcessorProfileMgr, reinterpret_cast<void **>(&mgr));
  if (FAILED(hr)) {
    std::fprintf(stderr, "CoCreate profile mgr failed 0x%08lX\n", static_cast<unsigned long>(hr));
    CoUninitialize();
    return 1;
  }
  const bool is_on = strcmp(argv[1], "on") == 0;
  const bool is_off = strcmp(argv[1], "off") == 0;
  const bool is_query = strcmp(argv[1], "query") == 0;
  if (!is_on && !is_off && !is_query) {
    mgr->Release();
    CoUninitialize();
    return 2;
  }
  if (is_query) {
    TF_INPUTPROCESSORPROFILE active{};
    hr = mgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active);
    if (SUCCEEDED(hr)) {
      std::printf(
          "active type=%lu langid=0x%04X hkl=%p clsid=%08lX-%04X-%04X profile=%08lX-%04X-%04X\n",
          static_cast<unsigned long>(active.dwProfileType), active.langid, active.hkl,
          active.clsid.Data1, active.clsid.Data2, active.clsid.Data3, active.guidProfile.Data1,
          active.guidProfile.Data2, active.guidProfile.Data3);
    } else {
      std::printf("GetActiveProfile hr=0x%08lX\n", static_cast<unsigned long>(hr));
    }
  } else if (is_on) {
    TF_INPUTPROCESSORPROFILE previous{};
    const bool has_previous = SUCCEEDED(mgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &previous));
    // Session-scope activation: the profile becomes the active input processor
    // for this logon session (equivalent to picking it in the input switcher).
    hr = mgr->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, LANGIDFROMLCID(GetKeyboardLayout(0)),
                              CLSID_FusionTsfTextService, GUID_FusionTsfProfile, nullptr,
                              TF_IPPMF_FORSESSION);
    const bool saved = hr == S_OK && has_previous &&
                       !IsEqualCLSID(previous.clsid, CLSID_FusionTsfTextService) &&
                       save_previous(previous);
    std::printf("activate-for-session hr=0x%08lX saved-previous=%d\n",
                static_cast<unsigned long>(hr), saved ? 1 : 0);
  } else if (is_off) {
    TF_INPUTPROCESSORPROFILE previous{};
    if (!load_previous(previous) || IsEqualCLSID(previous.clsid, CLSID_FusionTsfTextService)) {
      IEnumTfInputProcessorProfiles *profiles = nullptr;
      hr = mgr->EnumProfiles(LANGIDFROMLCID(GetKeyboardLayout(0)), &profiles);
      bool found = false;
      if (SUCCEEDED(hr)) {
        while (profiles->Next(1, &previous, nullptr) == S_OK) {
          if (!IsEqualCLSID(previous.clsid, CLSID_FusionTsfTextService)) {
            found = true;
            break;
          }
        }
        profiles->Release();
      }
      if (!found) {
        std::fprintf(stderr, "no non-Fusion profile available\n");
        mgr->Release();
        CoUninitialize();
        return 1;
      }
    }
    hr = mgr->ActivateProfile(
        previous.dwProfileType, previous.langid, previous.clsid, previous.guidProfile,
        previous.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR ? nullptr : previous.hkl,
        TF_IPPMF_FORSESSION);
    std::printf("restore-for-session hr=0x%08lX\n", static_cast<unsigned long>(hr));
  }
  mgr->Release();
  CoUninitialize();
  return hr == S_OK ? 0 : 1;
}
