#pragma once

// Stage 2.5 development-only TSF identities. These are stable development GUIDs
// for this proof slice, not frozen product identifiers (D072 §11 wire/identifier
// set remains UNFROZEN).

#include <guiddef.h>

// {8B7C4A90-5E2D-4B1F-9A31-6C0F42D1E7A4}
DEFINE_GUID(CLSID_FusionTsfTextService, 0x8b7c4a90, 0x5e2d, 0x4b1f, 0x9a, 0x31, 0x6c, 0x0f, 0x42,
            0xd1, 0xe7, 0xa4);

// {D4A1F6B2-3C58-4E77-8A90-1B5D9C06E4F1}
DEFINE_GUID(GUID_FusionTsfProfile, 0xd4a1f6b2, 0x3c58, 0x4e77, 0x8a, 0x90, 0x1b, 0x5d, 0x9c, 0x06,
            0xe4, 0xf1);
