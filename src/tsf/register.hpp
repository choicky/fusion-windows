#pragma once

// Reversible development registration for the Stage 2.5 TSF text service.
//
// DllRegisterServer/DllUnregisterServer are invoked by an elevated regsvr32
// (see tools/tsf/*.cmd). Persistent cross-process registration requires the
// HKLM CTF\TIP writes that ITfInputProcessorProfiles::Register/RegisterProfile
// perform, which a non-elevated token cannot create (D072 §14 registration
// observation). Everything written here is removed by the unregister path.

#include <windows.h>

namespace fusion::tsf {

HRESULT RegisterServer(HMODULE module);
HRESULT UnregisterServer();

} // namespace fusion::tsf
