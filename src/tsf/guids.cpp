#include <initguid.h>

// Single translation unit that instantiates every GUID the TSF shell uses:
// the SDK's msctf.h GUIDs are declared EXTERN_C in each includer and defined
// exactly once here via <initguid.h>. The Windows SDK for 10.0.26100.0 ships no
// msctf.lib import library, so nothing else needs to be linked for these.
#include <msctf.h>

#include "tsf/guids.hpp"
