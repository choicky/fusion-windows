#pragma once

// Stage 2.5 development key classification: how a physical Windows key event is
// presented to the existing Mock Backend vocabulary. This is a development-only
// Stage 2.5 policy; the production effect-capable key classification is
// UNFROZEN in D072 §5 and is not decided here.
//
// Virtual-key codes appear as plain constants (no windows.h) so this logic and
// its tests build on every platform the repository supports.

#include <cstdint>

namespace fusion::tsf {

// Virtual-key codes referenced by the Stage 2.5 classification.
inline constexpr std::uint32_t kVkBack = 0x08;
inline constexpr std::uint32_t kVkReturn = 0x0D;
inline constexpr std::uint32_t kVkEscape = 0x1B;
inline constexpr std::uint32_t kVkSpace = 0x20;
inline constexpr std::uint32_t kVkNumpad0 = 0x60;
inline constexpr std::uint32_t kVkNumpad9 = 0x69;

// X11 keysyms of the keys whose Fcitx handling needs a live composition: the engine's
// branches for them sit inside its non-empty-input branch. Named here so the shell's
// admission rule and the translation table cannot drift apart.
inline constexpr std::uint32_t kKeysymBackSpace = 0xff08;
inline constexpr std::uint32_t kKeysymReturn = 0xff0d;
inline constexpr std::uint32_t kKeysymHome = 0xff50;
inline constexpr std::uint32_t kKeysymLeft = 0xff51;
inline constexpr std::uint32_t kKeysymUp = 0xff52;
inline constexpr std::uint32_t kKeysymRight = 0xff53;
inline constexpr std::uint32_t kKeysymDown = 0xff54;
inline constexpr std::uint32_t kKeysymEnd = 0xff57;
inline constexpr std::uint32_t kKeysymDelete = 0xffff;

// The two Shift keys, kept apart: Fcitx matches its modifier-only bindings against the
// exact key (`Hotkey/AltTriggerKeys` defaults to Shift_L only), so a right Shift must
// never be presented as a left one.
inline constexpr std::uint32_t kVkLShift = 0xA0;
inline constexpr std::uint32_t kVkRShift = 0xA1;
inline constexpr std::uint32_t kKeysymShiftL = 0xffe1;
inline constexpr std::uint32_t kKeysymShiftR = 0xffe2;
// Scan codes that separate the two Shift keys when Windows reports the generic VK_SHIFT.
inline constexpr std::uint32_t kScanShiftLeft = 0x2A;
inline constexpr std::uint32_t kScanShiftRight = 0x36;

struct KeyAction {
  enum class Type {
    AppendChar, // append one raw character to the composition buffer (STATE input)
    Commit,     // commit the current composition buffer (EFFECT input)
    Backspace,  // drop the last raw character (STATE input)
    Reject,     // abandon the composition (failure-after-ownership input)
    Pass,       // not part of the Stage 2.5 vocabulary: genuine PASS candidate
  };
  Type type = Type::Pass;
  char ch = '\0'; // valid only for AppendChar
  std::uint32_t virtual_key = 0;
  bool modified = false;
  // True for the key-up half of a key the engine may bind as a modifier-only hotkey. A
  // release carries no text and owns no composition: it exists so Fcitx can decide
  // whether a lone modifier press completed within its own configured timeout.
  bool release = false;
  std::uint32_t keysym = 0;
  std::uint32_t modifiers = 0;
};

// Pure classification of one physical key-down. Modifier combinations and every
// key outside the development vocabulary classify as Pass.
KeyAction classify_key(std::uint32_t virtual_key, bool ctrl, bool alt, bool win);

// Physical-key translation; this does not predict engine consumption. `scan_code` is the
// low byte of the lparam scan field and is needed only to tell the two Shift keys apart
// when Windows reports the generic VK_SHIFT.
KeyAction translate_key(std::uint32_t virtual_key, bool ctrl, bool alt, bool win,
                        char32_t character = 0, bool shift = false, bool caps = false,
                        std::uint32_t scan_code = 0);

} // namespace fusion::tsf
