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

// Stage 5 composition display attributes (Fcitx Underline / HighLight).
// {3DD443CF-D106-43A3-B90C-59F115F78BD1}
DEFINE_GUID(GUID_FusionDisplayAttributeInput, 0x3dd443cf, 0xd106, 0x43a3, 0xb9, 0x0c, 0x59, 0xf1,
            0x15, 0xf7, 0x8b, 0xd1);

// {4C35C49B-779F-4432-9FA2-79BF98646B24}
DEFINE_GUID(GUID_FusionDisplayAttributeHighlighted, 0x4c35c49b, 0x779f, 0x4432, 0x9f, 0xa2, 0x79,
            0xbf, 0x98, 0x64, 0x6b, 0x24);
