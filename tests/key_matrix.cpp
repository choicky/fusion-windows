// R53 measurement: the full keyboard matrix against the real pinned engine.
//
// This is a measurement tool, not a gate. For every (key, state, modifier) cell it records
//   (a) what OnTestKeyDown answers today — the same two predicates the shell uses
//       (`routes_key` and `engine_has_work_for`) plus the hard policy, which this fixture
//       passes as allowed so the matrix is about the key rules and not about password fields;
//   (b) what the key path does — route, eaten/forward, the ordered frontend operations, and
//       whether the R44 fallback inserted the character the engine handed back;
//   (c) what Fcitx itself answers for the same key in the same state. When the shell dispatched,
//       the decision carries the engine's own answer (`forward` is set from the batch, and the
//       fallback is the one case where a forwarded key still produced text). When the shell did
//       not dispatch, a second context of the same backend is driven into the same state and the
//       stimulus is sent to the engine directly, which is the only way to tell a MISSED cell
//       from an ok one;
//   (d) a verdict (ok / LOST / MISSED / FALLBACK / RECOVERY).
//
// The four cases the R53 stopgap claims are asserted; every other cell is recorded. A cell that
// cannot be reached is reported with the reason, never skipped silently.
//
// Characters are the ones `ToUnicodeEx` returns for this thread's keyboard layout with the
// matrix's own modifier state, which is exactly what the product's `physical_key` passes to
// `translate_key`; nothing here re-implements the product's classification.

#include "fusion/backend.hpp"
#include "tsf/key_map.hpp"
#include "tsf/semantic.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fusion;
using namespace fusion::tsf;

namespace {

void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}

// --- the key set -----------------------------------------------------------------------------
//
// `keysym` is the canonical X11 keysym of the key, used only when the shell itself sends none
// (a `Type::Pass` key outside `translate_key`'s switch): the probe then asks the engine what it
// would do with the key, which is how a MISSED cell is told apart from an ok one. The shell never
// presents such a key — that is itself one of the things this matrix records.
struct KeyDef {
  std::uint32_t vk;
  const char *name;
  const char *klass;
  std::uint32_t keysym; // 0: `translate_key` decides, or the shell sends nothing
  std::uint32_t scan;
  bool numpad;      // a keypad key measured with NumLock on
  bool text;        // the key carries a character on a US layout
  bool numlock_off; // the same key measured with NumLock off
};

constexpr std::uint32_t kNoKeysym = 0;

const KeyDef kKeys[] = {
    // Letters.
    {0x41, "A", "letter", kNoKeysym, 0x1E, false, true},
    {0x42, "B", "letter", kNoKeysym, 0x30, false, true},
    {0x43, "C", "letter", kNoKeysym, 0x2E, false, true},
    {0x44, "D", "letter", kNoKeysym, 0x20, false, true},
    {0x45, "E", "letter", kNoKeysym, 0x12, false, true},
    {0x46, "F", "letter", kNoKeysym, 0x21, false, true},
    {0x47, "G", "letter", kNoKeysym, 0x22, false, true},
    {0x48, "H", "letter", kNoKeysym, 0x23, false, true},
    {0x49, "I", "letter", kNoKeysym, 0x17, false, true},
    {0x4A, "J", "letter", kNoKeysym, 0x24, false, true},
    {0x4B, "K", "letter", kNoKeysym, 0x25, false, true},
    {0x4C, "L", "letter", kNoKeysym, 0x26, false, true},
    {0x4D, "M", "letter", kNoKeysym, 0x32, false, true},
    {0x4E, "N", "letter", kNoKeysym, 0x31, false, true},
    {0x4F, "O", "letter", kNoKeysym, 0x18, false, true},
    {0x50, "P", "letter", kNoKeysym, 0x19, false, true},
    {0x51, "Q", "letter", kNoKeysym, 0x10, false, true},
    {0x52, "R", "letter", kNoKeysym, 0x13, false, true},
    {0x53, "S", "letter", kNoKeysym, 0x1F, false, true},
    {0x54, "T", "letter", kNoKeysym, 0x14, false, true},
    {0x55, "U", "letter", kNoKeysym, 0x16, false, true},
    {0x56, "V", "letter", kNoKeysym, 0x2F, false, true},
    {0x57, "W", "letter", kNoKeysym, 0x11, false, true},
    {0x58, "X", "letter", kNoKeysym, 0x2D, false, true},
    {0x59, "Y", "letter", kNoKeysym, 0x15, false, true},
    {0x5A, "Z", "letter", kNoKeysym, 0x2C, false, true},
    // Digits.
    {0x30, "0", "digit", kNoKeysym, 0x0B, false, true},
    {0x31, "1", "digit", kNoKeysym, 0x02, false, true},
    {0x32, "2", "digit", kNoKeysym, 0x03, false, true},
    {0x33, "3", "digit", kNoKeysym, 0x04, false, true},
    {0x34, "4", "digit", kNoKeysym, 0x05, false, true},
    {0x35, "5", "digit", kNoKeysym, 0x06, false, true},
    {0x36, "6", "digit", kNoKeysym, 0x07, false, true},
    {0x37, "7", "digit", kNoKeysym, 0x08, false, true},
    {0x38, "8", "digit", kNoKeysym, 0x09, false, true},
    {0x39, "9", "digit", kNoKeysym, 0x0A, false, true},
    // OEM punctuation, including the 102nd key.
    {0xBA, "OEM_1", "oem", kNoKeysym, 0x27, false, true},
    {0xBB, "OEM_PLUS", "oem", kNoKeysym, 0x0C, false, true},
    {0xBC, "OEM_COMMA", "oem", kNoKeysym, 0x33, false, true},
    {0xBD, "OEM_MINUS", "oem", kNoKeysym, 0x0C, false, true},
    {0xBE, "OEM_PERIOD", "oem", kNoKeysym, 0x34, false, true},
    {0xBF, "OEM_2", "oem", kNoKeysym, 0x35, false, true},
    {0xC0, "OEM_3", "oem", kNoKeysym, 0x29, false, true},
    {0xDB, "OEM_4", "oem", kNoKeysym, 0x1A, false, true},
    {0xDC, "OEM_5", "oem", kNoKeysym, 0x2B, false, true},
    {0xDD, "OEM_6", "oem", kNoKeysym, 0x1B, false, true},
    {0xDE, "OEM_7", "oem", kNoKeysym, 0x28, false, true},
    {0xE2, "OEM_102", "oem", kNoKeysym, 0x56, false, true},
    // The function row.
    {0x70, "F1", "function", kNoKeysym, 0x3B, false, false},
    {0x71, "F2", "function", kNoKeysym, 0x3C, false, false},
    {0x72, "F3", "function", kNoKeysym, 0x3D, false, false},
    {0x73, "F4", "function", kNoKeysym, 0x3E, false, false},
    {0x74, "F5", "function", kNoKeysym, 0x3F, false, false},
    {0x75, "F6", "function", kNoKeysym, 0x40, false, false},
    {0x76, "F7", "function", kNoKeysym, 0x41, false, false},
    {0x77, "F8", "function", kNoKeysym, 0x42, false, false},
    {0x78, "F9", "function", kNoKeysym, 0x43, false, false},
    {0x79, "F10", "function", kNoKeysym, 0x44, false, false},
    {0x7A, "F11", "function", kNoKeysym, 0x57, false, false},
    {0x7B, "F12", "function", kNoKeysym, 0x58, false, false},
    {0x7C, "F13", "function", kNoKeysym, 0x64, false, false},
    {0x7D, "F14", "function", kNoKeysym, 0x65, false, false},
    {0x7E, "F15", "function", kNoKeysym, 0x66, false, false},
    {0x7F, "F16", "function", kNoKeysym, 0x67, false, false},
    {0x80, "F17", "function", kNoKeysym, 0x68, false, false},
    {0x81, "F18", "function", kNoKeysym, 0x69, false, false},
    {0x82, "F19", "function", kNoKeysym, 0x6A, false, false},
    {0x83, "F20", "function", kNoKeysym, 0x6B, false, false},
    {0x84, "F21", "function", kNoKeysym, 0x6C, false, false},
    {0x85, "F22", "function", kNoKeysym, 0x6D, false, false},
    {0x86, "F23", "function", kNoKeysym, 0x6E, false, false},
    {0x87, "F24", "function", kNoKeysym, 0x76, false, false},
    // Editing and navigation.
    {0x1B, "Escape", "editing", kNoKeysym, 0x01, false, false},
    {0x09, "Tab", "editing", kNoKeysym, 0x0F, false, false},
    {0x0D, "Return", "editing", kNoKeysym, 0x1C, false, false},
    {0x08, "Backspace", "editing", kNoKeysym, 0x0E, false, false},
    {0x20, "Space", "editing", kNoKeysym, 0x39, false, true},
    {0x2D, "Insert", "navigation", kNoKeysym, 0x52, false, false},
    {0x2E, "Delete", "navigation", kNoKeysym, 0x53, false, false},
    {0x24, "Home", "navigation", kNoKeysym, 0x47, false, false},
    {0x23, "End", "navigation", kNoKeysym, 0x4F, false, false},
    {0x21, "PageUp", "navigation", kNoKeysym, 0x49, false, false},
    {0x22, "PageDown", "navigation", kNoKeysym, 0x51, false, false},
    {0x25, "Left", "navigation", kNoKeysym, 0x4B, false, false},
    {0x26, "Up", "navigation", kNoKeysym, 0x48, false, false},
    {0x27, "Right", "navigation", kNoKeysym, 0x4D, false, false},
    {0x28, "Down", "navigation", kNoKeysym, 0x50, false, false},
    // The numeric keypad. With NumLock on the key carries a digit or an operator; with NumLock
    // off Windows reports the same virtual key and `ToUnicodeEx` returns no character, so the
    // shell has no keysym to send — which is a measurement, not an omission.
    {0x60, "Numpad0", "numpad", 0xffb0, 0x52, true, true},
    {0x61, "Numpad1", "numpad", 0xffb1, 0x4F, true, true},
    {0x62, "Numpad2", "numpad", 0xffb2, 0x50, true, true},
    {0x63, "Numpad3", "numpad", 0xffb3, 0x51, true, true},
    {0x64, "Numpad4", "numpad", 0xffb4, 0x4B, true, true},
    {0x65, "Numpad5", "numpad", 0xffb5, 0x4C, true, true},
    {0x66, "Numpad6", "numpad", 0xffb6, 0x4D, true, true},
    {0x67, "Numpad7", "numpad", 0xffb7, 0x47, true, true},
    {0x68, "Numpad8", "numpad", 0xffb8, 0x48, true, true},
    {0x69, "Numpad9", "numpad", 0xffb9, 0x49, true, true},
    {0x6A, "NumpadMultiply", "numpad", 0xffaa, 0x37, true, true},
    {0x6B, "NumpadAdd", "numpad", 0xffab, 0x4E, true, true},
    {0x6D, "NumpadSubtract", "numpad", 0xffad, 0x4A, true, true},
    {0x6E, "NumpadDecimal", "numpad", 0xffae, 0x53, true, true},
    {0x6F, "NumpadDivide", "numpad", 0xffaf, 0x35, true, true},
    // The same fifteen keys with NumLock off. Windows reports the same virtual key and
    // `ToUnicodeEx` returns no character. For the five operators that leaves nothing to send, so
    // the shell never presents them and the probe asks the engine with the keypad keysym. For the
    // ten digits it is worse than that: `classify_key` maps the keypad range to a digit whatever
    // the NumLock state, the keysym is empty, and the Host falls back to the stimulus's symbolic
    // name — so a keypad digit with NumLock off is presented as the top-row digit. Measured: it
    // types "0".."9" through the engine where the application would have seen End/Down/… .
    // Without these cells the matrix would be claiming coverage it does not have.
    {0x60, "Numpad0-NumLockOff", "numpad", 0xffb0, 0x52, false, false, true},
    {0x61, "Numpad1-NumLockOff", "numpad", 0xffb1, 0x4F, false, false, true},
    {0x62, "Numpad2-NumLockOff", "numpad", 0xffb2, 0x50, false, false, true},
    {0x63, "Numpad3-NumLockOff", "numpad", 0xffb3, 0x51, false, false, true},
    {0x64, "Numpad4-NumLockOff", "numpad", 0xffb4, 0x4B, false, false, true},
    {0x65, "Numpad5-NumLockOff", "numpad", 0xffb5, 0x4C, false, false, true},
    {0x66, "Numpad6-NumLockOff", "numpad", 0xffb6, 0x4D, false, false, true},
    {0x67, "Numpad7-NumLockOff", "numpad", 0xffb7, 0x47, false, false, true},
    {0x68, "Numpad8-NumLockOff", "numpad", 0xffb8, 0x48, false, false, true},
    {0x69, "Numpad9-NumLockOff", "numpad", 0xffb9, 0x49, false, false, true},
    {0x6A, "NumpadMultiply-NumLockOff", "numpad", 0xffaa, 0x37, false, false, true},
    {0x6B, "NumpadAdd-NumLockOff", "numpad", 0xffab, 0x4E, false, false, true},
    {0x6D, "NumpadSubtract-NumLockOff", "numpad", 0xffad, 0x4A, false, false, true},
    {0x6E, "NumpadDecimal-NumLockOff", "numpad", 0xffae, 0x53, false, false, true},
    {0x6F, "NumpadDivide-NumLockOff", "numpad", 0xffaf, 0x35, false, false, true},
    // Locks and system keys.
    {0x14, "CapsLock", "lock", 0xffe5, 0x3A, false, false},
    {0x90, "NumLock", "lock", 0xff7f, 0x45, false, false},
    {0x91, "ScrollLock", "lock", 0xff14, 0x46, false, false},
    {0x2C, "PrintScreen", "system", 0xff61, 0x37, false, false},
    {0x13, "Pause", "system", 0xff13, 0x45, false, false},
    {0x5D, "Apps", "system", 0xff67, 0x5D, false, false},
    // Browser, volume, media and launch.
    {0xA6, "BrowserBack", "media", 0x1008ff26, 0x6A, false, false},
    {0xA7, "BrowserForward", "media", 0x1008ff27, 0x69, false, false},
    {0xA8, "BrowserRefresh", "media", 0x1008ff29, 0x67, false, false},
    {0xA9, "BrowserStop", "media", 0x1008ff28, 0x68, false, false},
    {0xAA, "BrowserSearch", "media", 0x1008ff1b, 0x65, false, false},
    {0xAB, "BrowserFavorites", "media", 0x1008ff30, 0x66, false, false},
    {0xAC, "BrowserHome", "media", 0x1008ff18, 0x32, false, false},
    {0xAD, "VolumeMute", "media", 0x1008ff12, 0x20, false, false},
    {0xAE, "VolumeDown", "media", 0x1008ff11, 0x2E, false, false},
    {0xAF, "VolumeUp", "media", 0x1008ff13, 0x30, false, false},
    {0xB0, "MediaNext", "media", 0x1008ff17, 0x19, false, false},
    {0xB1, "MediaPrev", "media", 0x1008ff16, 0x10, false, false},
    {0xB2, "MediaStop", "media", 0x1008ff15, 0x24, false, false},
    {0xB3, "MediaPlayPause", "media", 0x1008ff14, 0x22, false, false},
    {0xB4, "LaunchMail", "media", 0x1008ff19, 0x6C, false, false},
    {0xB5, "LaunchMedia", "media", 0x1008ff32, 0x6D, false, false},
    {0xB6, "LaunchApp1", "media", 0x1008ff1c, 0x6B, false, false},
    {0xB7, "LaunchApp2", "media", 0x1008ff2a, 0x21, false, false},
    // Modifiers.
    {0xA0, "LShift", "modifier", kNoKeysym, 0x2A, false, false},
    {0xA1, "RShift", "modifier", kNoKeysym, 0x36, false, false},
    {0xA2, "LCtrl", "modifier", 0xffe3, 0x1D, false, false},
    {0xA3, "RCtrl", "modifier", 0xffe4, 0x1D, false, false},
    {0xA4, "LAlt", "modifier", 0xffe9, 0x38, false, false},
    {0xA5, "RAlt", "modifier", 0xffea, 0x38, false, false},
    {0x5B, "LWin", "modifier", 0xffeb, 0x5B, false, false},
    {0x5C, "RWin", "modifier", 0xffec, 0x5C, false, false},
};

enum class State { Inactive, Idle, Composing, ComposingSelection, AfterCommit, AfterToggle };

const char *state_name(State state) {
  switch (state) {
  case State::Inactive:
    return "inactive";
  case State::Idle:
    return "idle";
  case State::Composing:
    return "composing";
  case State::ComposingSelection:
    return "composing-selection";
  case State::AfterCommit:
    return "after-commit";
  case State::AfterToggle:
    return "after-toggle";
  }
  return "?";
}

struct Modifier {
  const char *name;
  bool shift;
  bool ctrl;
  bool alt;
};

// AltGr is Ctrl+Alt on Windows, which is what the shell's `translate_key` sees.
const Modifier kModifiers[] = {
    {"none", false, false, false},     {"shift", true, false, false},
    {"ctrl", false, true, false},      {"alt", false, false, true},
    {"ctrl+shift", true, true, false}, {"ctrl+alt", true, true, true},
};

// X11 state bits, the shape `BackendStimulus::modifiers` carries. The shell only ever sends the
// Shift/Caps bits (a chord never reaches it), so the probe builds the chord's own state: that is
// the only way to ask whether Fcitx would have consumed the combination.
std::uint32_t probe_modifiers(const Modifier &modifier) {
  return (modifier.shift ? 1u : 0u) | (modifier.ctrl ? 4u : 0u) | (modifier.alt ? 8u : 0u);
}

// --- the document the callbacks are applied to ------------------------------------------------

struct Document {
  std::string committed;
  std::string preedit;
  void apply(SemanticController &controller, const SemanticDecision &decision) {
    for (const auto &work : decision.frontend) {
      if (work.operation.type == BackendOperation::Type::SetPreedit) {
        preedit = work.operation.text;
        (void)controller.settle_state(work.key, EditResult::AppliedReadback);
      } else if (work.operation.type == BackendOperation::Type::CommitText) {
        committed += work.operation.text;
        preedit.clear();
        auto ticket = controller.open_effect(work.key);
        if (ticket)
          (void)controller.settle_effect(*ticket, EditResult::AppliedReadback);
      }
    }
  }
};

// --- the cell ---------------------------------------------------------------------------------

struct Cell {
  std::string key;
  std::string klass;
  std::string state;
  std::string modifier;
  std::uint32_t vk = 0;
  std::uint32_t keysym_sent = 0;
  std::uint32_t probe_keysym = 0;
  bool text_key = false;
  bool t_answer = false;
  bool k_dispatched = false;
  std::string k_route;
  bool k_eaten = false;
  bool k_forward = false;
  std::size_t k_ops = 0;
  bool k_fallback = false;
  std::string k_committed;
  bool engine_consumed = false;
  bool engine_forwarded = false;
  bool engine_probed = false;
  std::string verdict;
  std::string note;
};

std::string csv_field(const std::string &value) {
  if (value.find_first_of(",\"\n") == std::string::npos)
    return value;
  std::string quoted = "\"";
  for (char c : value) {
    if (c == '"')
      quoted.push_back('"');
    quoted.push_back(c);
  }
  quoted.push_back('"');
  return quoted;
}

std::string utf8_of(char32_t code) {
  std::string text;
  if (code < 0x80) {
    text.push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    text.push_back(static_cast<char>(0xc0 | (code >> 6)));
    text.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  } else if (code < 0x10000) {
    text.push_back(static_cast<char>(0xe0 | (code >> 12)));
    text.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
    text.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  } else {
    text.push_back(static_cast<char>(0xf0 | (code >> 18)));
    text.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
    text.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
    text.push_back(static_cast<char>(0x80 | (code & 0x3f)));
  }
  return text;
}

// The character `physical_key` would hand to `translate_key`: ToUnicodeEx with the matrix's own
// modifier state, against this thread's layout. A dead key (count < 0) and a multi-unit result
// are not text this matrix can present, and are reported as "no character".
char32_t character_for(const KeyDef &key, bool shift, bool numlock) {
  BYTE state[256] = {};
  if (shift)
    state[VK_SHIFT] = 0x80;
  if (numlock)
    state[VK_NUMLOCK] = 0x01;
  wchar_t text[8] = {};
  const int count = ToUnicodeEx(key.vk, key.scan, state, text, 8, 4, GetKeyboardLayout(0));
  if (count == 1 && !(text[0] >= 0xd800 && text[0] <= 0xdfff))
    return text[0];
  if (count == 2 && text[0] >= 0xd800 && text[0] <= 0xdbff && text[1] >= 0xdc00 &&
      text[1] <= 0xdfff)
    return 0x10000u + ((text[0] - 0xd800u) << 10) + text[1] - 0xdc00u;
  return 0;
}

KeyAction action_for(const KeyDef &key, const Modifier &modifier, bool numlock) {
  const char32_t character = key.text ? character_for(key, modifier.shift, numlock) : 0;
  return translate_key(key.vk, modifier.ctrl, modifier.alt, false, character, modifier.shift,
                       false, key.scan);
}

// The stimulus `begin_real_key` builds for this action, so the engine probe asks the same question
// the product asks. `keysym` overrides the one the shell would send, for the keys the shell never
// presents at all.
BackendStimulus stimulus_for(const KeyAction &action, const Binding &binding, std::uint64_t id,
                             std::uint32_t keysym) {
  BackendStimulus stimulus{binding, id};
  switch (action.type) {
  case KeyAction::Type::AppendChar:
    stimulus.type = BackendStimulus::Type::Character;
    stimulus.character = action.ch;
    break;
  case KeyAction::Type::Commit:
    stimulus.type = BackendStimulus::Type::Space;
    break;
  case KeyAction::Type::Backspace:
    stimulus.type = BackendStimulus::Type::Backspace;
    break;
  case KeyAction::Type::Reject:
    stimulus.type = BackendStimulus::Type::Escape;
    break;
  case KeyAction::Type::Pass:
    stimulus.type = BackendStimulus::Type::Return;
    break;
  }
  stimulus.keysym = keysym;
  stimulus.modifiers = action.modifiers;
  return stimulus;
}

// --- the fixture ------------------------------------------------------------------------------

// Drives one context into the state a cell is measured in. Returns false (with a reason) when the
// state cannot be reached, which is reported rather than hidden.
bool reach_state(SemanticController &controller, State state, Document &doc, std::string &why) {
  auto press = [&](const KeyAction &action) {
    const SemanticDecision decision = controller.begin_key(action, true);
    doc.apply(controller, decision);
    return decision;
  };
  auto compose = [&] {
    for (char c : std::string("nihao"))
      (void)press(translate_key(static_cast<std::uint32_t>(c), false, false, false, c));
  };
  // The engine's own modifier-only binding (Hotkey/AltTriggerKeys, Shift_L by default) is
  // settled on the release of a press the engine received. The press is forwarded and the
  // release toggles the context into the group's off slot. Returns false when the engine did
  // not act on the release — which is what a configuration without that binding looks like.
  auto toggle_off = [&]() {
    KeyAction shift =
        translate_key(kVkLShift, false, false, false, 0, false, false, kScanShiftLeft);
    const SemanticDecision down = press(shift);
    if (!down.forward) {
      why = "the engine consumed the Shift press instead of forwarding it";
      return false;
    }
    shift.release = true;
    if (!controller.routes_release(shift) || !controller.release_admissible(shift)) {
      why = "the Shift release was not relayed";
      return false;
    }
    const SemanticDecision release = controller.release_key(shift);
    doc.apply(controller, release);
    if (release.backend_session_id == 0) {
      why = "the Shift release did not reach the engine";
      return false;
    }
    return true;
  };
  switch (state) {
  case State::Inactive: {
    // F6 is bound in this backend's own instance as Fcitx's own trigger key, which is how
    // stage3c reaches the off state: a global key is handled before any input method and does
    // not need an active context. The toggle's own batch reports the state the context was in
    // *before* it, so the shell still believes the engine is on; one more key is needed for it
    // to learn the off state, and that key has to be one an active engine would compose —
    // a character-less key is forwarded by both states and would teach nothing. The letter is
    // therefore delivered by the R44 fallback, which is the stopgap's own first-key behaviour.
    (void)press(translate_key(0x74, false, false, false)); // bind and activate the context
    if (!toggle_off())
      return false;
    // The toggle's own batch reports the state the context was in *before* it, so the shell
    // still believes the engine is on; one more key is needed for it to learn the off state,
    // and it has to be one an active engine would compose — a character-less key is forwarded
    // by both states and would teach nothing.
    (void)press(translate_key(0x58, false, false, false, U'x'));
    // The check is the engine's, not the shell's: an active engine would have started a
    // composition on that letter, and the off slot forwards it. Asking the shell's own predicate
    // would make the fixture depend on the very rule it is measuring — and would make the same
    // file unusable against a build from before that rule existed.
    if (!doc.preedit.empty()) {
      why = "the fixture did not reach the engine's off state (the letter composed)";
      return false;
    }
    return true;
  }
  case State::Idle: {
    // One forwarded key binds the context (binding is what activates it) and leaves nothing
    // composing: F5 is a key the idle engine hands back under its default configuration.
    (void)press(translate_key(0x74, false, false, false));
    if (!controller.raw_buffer().empty() || controller.displayed_page().has_value()) {
      why = "the idle fixture is not idle after the binding key";
      return false;
    }
    return true;
  }
  case State::Composing: {
    compose();
    if (controller.raw_buffer().empty() || !controller.displayed_page().has_value()) {
      why = "the composing fixture produced no preedit or no page";
      return false;
    }
    return true;
  }
  case State::ComposingSelection: {
    compose();
    if (!controller.displayed_page().has_value()) {
      why = "no page to move a candidate cursor on";
      return false;
    }
    SemanticDecision cursor;
    if (!controller.begin_candidate_cursor(1, cursor)) {
      why = "the page is not a scrolling window, so no candidate cursor can be moved";
      return false;
    }
    doc.apply(controller, cursor);
    return true;
  }
  case State::AfterCommit: {
    compose();
    const SemanticDecision commit = press(translate_key(kVkSpace, false, false, false));
    if (commit.frontend.empty() || !controller.raw_buffer().empty()) {
      why = "the commit fixture did not commit and clear the composition";
      return false;
    }
    return true;
  }
  case State::AfterToggle: {
    // The shape the owner hit in Notepad++: a composition is live, the lone Shift turns the
    // engine off, and the next key is the first one in English. The release ends the composition
    // as it toggles, and the shell's cached mode stays "on" — the batch reports the state the
    // context was in *before* it — which is exactly the condition being measured.
    compose();
    if (!toggle_off())
      return false;
    if (!doc.preedit.empty()) {
      why = "the Shift release did not end the composition";
      return false;
    }
    return true;
  }
  }
  why = "unknown state";
  return false;
}

// --- the measurement --------------------------------------------------------------------------

struct Fixture {
  std::unique_ptr<Backend> backend;        // the default presentation
  std::unique_ptr<Backend> scroll_backend; // the automatic scrolling window, for the cursor state
  std::uint64_t next_context = 2000;
  std::uint64_t next_stimulus = 1000000;

  Backend &for_state(State state) {
    if (state == State::ComposingSelection && scroll_backend)
      return *scroll_backend;
    return *backend;
  }
};

const char *route_name(SemanticDecision::Route route) {
  switch (route) {
  case SemanticDecision::Route::Pass:
    return "Pass";
  case SemanticDecision::Route::State:
    return "State";
  case SemanticDecision::Route::Effect:
    return "Effect";
  case SemanticDecision::Route::Reject:
    return "Reject";
  case SemanticDecision::Route::FailOpen:
    return "FailOpen";
  case SemanticDecision::Route::CancelledRecovery:
    return "CancelledRecovery";
  }
  return "?";
}

// What the shell did, and what the engine answered, for one cell.
Cell measure_cell(Fixture &fixture, const KeyDef &key, State state, const Modifier &modifier) {
  Cell cell;
  cell.key = key.name;
  cell.klass = key.klass;
  cell.state = state_name(state);
  cell.modifier = modifier.name;
  cell.vk = key.vk;
  cell.text_key = key.text;

  Backend &backend = fixture.for_state(state);
  const std::uint64_t shell_context = fixture.next_context++;
  SemanticController controller(shell_context, 64, backend);
  Document doc;
  std::string why;
  if (!reach_state(controller, state, doc, why)) {
    cell.verdict = "unreached";
    cell.note = why;
    controller.destroy();
    return cell;
  }
  if (controller.pass_probe() != Pass::Clear) {
    cell.verdict = "unreached";
    cell.note = "the fixture left a pass barrier up";
    controller.destroy();
    return cell;
  }

  const bool numlock = key.numpad && !key.numlock_off;
  const char32_t character = key.text ? character_for(key, modifier.shift, numlock) : 0;
  const KeyAction action = action_for(key, modifier, numlock);
  // The keysym the key carries on its own. `translate_key` returns before its keysym table for
  // a Ctrl/Alt/Win chord (that is the platform policy), so a chord's own action has none; the
  // probe still has to ask the engine about the key the chord is on.
  const KeyAction plain = action_for(key, Modifier{"none", false, false, false}, numlock);
  cell.keysym_sent = action.keysym;

  // (a) what T answers today: the two predicates the shell uses, with the hard policy allowed.
  cell.t_answer = controller.routes_key(action) && controller.engine_has_work_for(action);

  // (b) what the key path does.
  const std::string before = doc.committed;
  const SemanticDecision decision = controller.begin_key(action, true);
  cell.k_dispatched = decision.backend_session_id != 0;
  cell.k_route = route_name(decision.route);
  cell.k_eaten = decision.eaten;
  cell.k_forward = decision.forward;
  cell.k_ops = decision.frontend.size();
  doc.apply(controller, decision);
  cell.k_committed = doc.committed.substr(before.size());

  // The R44 fallback is the one case where a forwarded key still produced document text, and it
  // produces exactly the character the key carries: one CommitText with that text. An engine
  // commit of its own is never that (a punctuation key commits the full-width form, a letter
  // commits a candidate), so the comparison is decisive.
  const std::string key_text = character != 0 ? utf8_of(character) : std::string();
  const bool fallback_shape =
      !decision.forward && decision.frontend.size() == 1 && !key_text.empty() &&
      decision.frontend.front().operation.type == BackendOperation::Type::CommitText &&
      decision.frontend.front().operation.text == key_text;

  if (cell.k_dispatched) {
    cell.engine_forwarded = decision.forward;
    cell.engine_consumed = !decision.forward;
    if (fallback_shape && cell.t_answer) {
      // The shell delivered it, so the engine must have handed it back.
      cell.k_fallback = true;
      cell.engine_forwarded = true;
      cell.engine_consumed = false;
    }
  } else {
    // The shell never presented the key. Ask the engine what it would have done with it, in the
    // same state, through a second context of the same backend. A key the shell has no keysym for
    // is asked with its canonical one, which is the only shape the engine could ever see.
    cell.probe_keysym = action.keysym != 0   ? action.keysym
                        : plain.keysym != 0  ? plain.keysym
                                             : key.keysym;
    const std::uint64_t probe_context = fixture.next_context++;
    SemanticController probe(probe_context, 64, backend);
    Document probe_doc;
    std::string probe_why;
    if (!reach_state(probe, state, probe_doc, probe_why)) {
      cell.note = probe_why;
    } else if (cell.probe_keysym == 0) {
      cell.note = "no keysym to ask the engine with";
    } else {
      BackendStimulus stimulus = stimulus_for(action, Binding{probe_context, 1, 1},
                                              fixture.next_stimulus++, cell.probe_keysym);
      stimulus.modifiers = probe_modifiers(modifier);
      const BackendBatch batch = backend.send(stimulus);
      cell.engine_probed = true;
      cell.engine_forwarded = batch.disposition == BackendBatch::Disposition::Forward;
      for (const auto &operation : batch.operations)
        if (operation.type == BackendOperation::Type::ForwardKey)
          cell.engine_forwarded = true;
      cell.engine_consumed = !cell.engine_forwarded;
    }
    probe.destroy();
  }

  // (d) the verdict.
  if (decision.route == SemanticDecision::Route::CancelledRecovery) {
    cell.verdict = "RECOVERY";
    cell.note = "the dispatch ended in the cancelled-recovery path";
  } else if (cell.k_fallback) {
    cell.verdict = "FALLBACK";
    cell.note = "T claimed, the engine handed it back, the shell delivered the character";
  } else if (cell.t_answer && cell.engine_forwarded && cell.k_committed.empty()) {
    cell.verdict = "LOST";
    cell.note = "T claimed the key, the engine handed it back, nothing was delivered";
  } else if (!cell.t_answer && cell.engine_consumed) {
    cell.verdict = "MISSED";
    cell.note = "T declined a key the engine would consume";
  } else {
    cell.verdict = "ok";
  }
  controller.destroy();
  return cell;
}

// --- output -----------------------------------------------------------------------------------

void write_csv(const std::string &path, const std::vector<Cell> &cells) {
  std::ofstream out(path, std::ios::binary);
  require(out.good(), "cannot open the CSV output");
  out << "key,key_class,state,modifier,vk,keysym_sent,probe_keysym,text_key,t_answer,k_dispatched,"
         "k_route,k_eaten,k_forward,k_ops,k_fallback,k_committed,engine_consumed,engine_forwarded,"
         "engine_probed,verdict,note\r\n";
  for (const Cell &cell : cells) {
    out << csv_field(cell.key) << ',' << csv_field(cell.klass) << ',' << csv_field(cell.state)
        << ',' << csv_field(cell.modifier) << ',' << cell.vk << ',' << cell.keysym_sent << ','
        << cell.probe_keysym << ',' << (cell.text_key ? 1 : 0) << ',' << (cell.t_answer ? 1 : 0)
        << ',' << (cell.k_dispatched ? 1 : 0) << ',' << csv_field(cell.k_route) << ','
        << (cell.k_eaten ? 1 : 0) << ',' << (cell.k_forward ? 1 : 0) << ',' << cell.k_ops << ','
        << (cell.k_fallback ? 1 : 0) << ',' << csv_field(cell.k_committed) << ','
        << (cell.engine_consumed ? 1 : 0) << ',' << (cell.engine_forwarded ? 1 : 0) << ','
        << (cell.engine_probed ? 1 : 0) << ',' << csv_field(cell.verdict) << ','
        << csv_field(cell.note) << "\r\n";
  }
}

const char *const kStates[] = {"inactive", "idle",         "composing",
                               "composing-selection", "after-commit", "after-toggle"};
const char *const kClasses[] = {"letter", "digit",      "oem",    "function", "editing",
                                "navigation", "numpad", "lock",   "system",   "media",
                                "modifier"};
const char *const kModifierNames[] = {"none", "shift", "ctrl", "alt", "ctrl+shift", "ctrl+alt"};
const char *const kVerdicts[] = {"ok", "LOST", "MISSED", "FALLBACK", "RECOVERY", "unreached"};

void write_summary(const std::string &path, const std::vector<Cell> &cells) {
  std::ofstream out(path, std::ios::binary);
  require(out.good(), "cannot open the summary output");
  std::map<std::string, int> by_verdict;
  std::map<std::string, std::map<std::string, int>> by_state;
  std::map<std::string, std::map<std::string, int>> by_class;
  for (const Cell &cell : cells) {
    ++by_verdict[cell.verdict];
    ++by_state[cell.verdict][cell.state];
    ++by_class[cell.verdict][cell.klass];
  }

  out << "# R53 key matrix — measured result\n\n";
  out << "Produced by `tests/key_matrix.cpp` against the pinned engine, current `main` (R53 "
         "stopgap included). One row per (key, state, modifier); the CSV beside this file is the "
         "full matrix. `T` is the answer `OnTestKeyDown` gives (the shell's `routes_key` and "
         "`engine_has_work_for`, with the hard policy allowed); `K` is the key path's own result; "
         "`engine` is what Fcitx answered, either from the batch the dispatch produced or from a "
         "direct probe of a second context in the same state.\n\n";
  out << "Total cells: **" << cells.size() << "**\n\n";
  out << "| verdict | cells |\n|---|---|\n";
  for (const char *verdict : kVerdicts)
    out << "| " << verdict << " | " << by_verdict[verdict] << " |\n";

  auto table = [&](const char *title, const char *const *names, std::size_t count,
                   const std::map<std::string, std::map<std::string, int>> &rows) {
    out << "\n## " << title << "\n\n| |";
    for (const char *verdict : kVerdicts)
      out << " " << verdict << " |";
    out << "\n|---|";
    for (std::size_t i = 0; i < std::size(kVerdicts); ++i)
      out << "---|";
    out << "\n";
    for (std::size_t i = 0; i < count; ++i) {
      out << "| " << names[i] << " |";
      for (const char *verdict : kVerdicts) {
        int value = 0;
        const auto row = rows.find(verdict);
        if (row != rows.end()) {
          const auto entry = row->second.find(names[i]);
          if (entry != row->second.end())
            value = entry->second;
        }
        out << " " << value << " |";
      }
      out << "\n";
    }
  };
  table("By state", kStates, std::size(kStates), by_state);
  table("By key class", kClasses, std::size(kClasses), by_class);

  auto list = [&](const char *verdict, const char *title, const char *note) {
    std::vector<std::string> items;
    for (const Cell &cell : cells)
      if (cell.verdict == verdict)
        items.push_back(cell.key + " / " + cell.state + " / " + cell.modifier);
    out << "\n## " << title << " (" << items.size() << ")\n\n";
    if (note != nullptr && *note != '\0')
      out << note << "\n\n";
    if (items.empty()) {
      out << "None.\n";
      return;
    }
    for (const std::string &item : items)
      out << "- " << item << "\n";
  };
  list("LOST", "LOST cells",
       "T claimed the key, the engine handed it back, nothing reached the document.");
  list("MISSED", "MISSED cells",
       "T declined a key the engine would consume. A modified chord is the platform policy (the "
       "shell never presents one), so an engine binding on it is given up; a character-less key "
       "is rule 2, and an editing key is the composition gate.");
  list("RECOVERY", "RECOVERY cells", "The dispatch ended in the cancelled-recovery path.");
  list("unreached", "Cells that could not be reached", "");

  out << "\n## Modifier coverage\n\n| modifier | cells |\n|---|---|\n";
  for (const char *name : kModifierNames) {
    int count = 0;
    for (const Cell &cell : cells)
      if (cell.modifier == name)
        ++count;
    out << "| " << name << " | " << count << " |\n";
  }
}

// --- the known cases the stopgap claims --------------------------------------------------------

const Cell *find_cell(const std::vector<Cell> &cells, const char *key, const char *state,
                      const char *modifier) {
  for (const Cell &cell : cells)
    if (cell.key == key && cell.state == state && cell.modifier == modifier)
      return &cell;
  return nullptr;
}

void check_known_cases(const std::vector<Cell> &cells) {
  struct Expect {
    const char *key;
    const char *state;
    const char *modifier;
    const char *verdict;
    const char *why;
  };
  const Expect expectations[] = {
      {"F2", "idle", "none", "ok", "rule 2: an idle F2 is the application's"},
      {"F5", "idle", "none", "ok", "rule 2: an idle F5 is the application's"},
      {"N", "inactive", "none", "ok",
       "rule 1: a letter in the engine's off state is the application's"},
      {"N", "after-toggle", "none", "FALLBACK",
       "the first printable key after the toggle was not delivered once"},
      {"Return", "after-toggle", "none", "ok",
       "a character-less key after the toggle is not claimed"},
  };
  for (const Expect &expectation : expectations) {
    const Cell *cell = find_cell(cells, expectation.key, expectation.state, expectation.modifier);
    require(cell != nullptr, "a known case is missing from the matrix");
    std::printf("known case %s/%s/%s verdict=%s t=%d dispatched=%d forward=%d fallback=%d "
                "committed=[%s]\n",
                expectation.key, expectation.state, expectation.modifier, cell->verdict.c_str(),
                cell->t_answer ? 1 : 0, cell->k_dispatched ? 1 : 0, cell->k_forward ? 1 : 0,
                cell->k_fallback ? 1 : 0, cell->k_committed.c_str());
    require(cell->verdict == expectation.verdict, expectation.why);
  }
  // The first printable key after the toggle is delivered exactly once, and the text is the key.
  const Cell *first = find_cell(cells, "N", "after-toggle", "none");
  require(first != nullptr && first->k_committed == "n",
          "the first key after the toggle did not appear exactly once");
  // The keys the shell has no keysym for are never presented, so the Host's `key_name` — which
  // throws for a keysym-less stimulus — is unreachable from the key path.
  for (const char *name : {"PrintScreen", "Pause", "ScrollLock", "Apps"}) {
    const Cell *cell = find_cell(cells, name, "composing", "none");
    require(cell != nullptr, "a keysym-less key is missing from the matrix");
    std::printf("keysym-less %s/%s: dispatched=%d t=%d engine=%d\n", name, cell->state.c_str(),
                cell->k_dispatched ? 1 : 0, cell->t_answer ? 1 : 0, cell->engine_consumed ? 1 : 0);
    require(!cell->k_dispatched,
            "the shell presented a keysym-less key, which the Host cannot name");
    require(!cell->t_answer, "T claimed a keysym-less key the shell never sends");
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const std::string out_dir = argc > 1 ? argv[1] : ".";
    const bool quick = argc > 2 && std::string(argv[2]) == "--quick";

    // The owner's own profile shape: one input method (双拼), which is what makes the engine's
    // modifier-only "temporarily switch" binding a toggle into its off slot rather than a switch
    // to a second method. F6 is bound as the engine's own trigger key so the off state can also
    // be reached without a modifier-only release.
    Fixture fixture;
    FcitxBackendOptions base;
    base.fixture_input_methods = {"shuangpin"};
    base.fixture_default_method = "shuangpin";
    fixture.backend = make_fcitx_backend(base);
    require(fixture.backend != nullptr, "the default backend could not be created");
    FcitxBackendOptions scrolling = base;
    scrolling.fixture_candidate_layout = "AutoScroll";
    fixture.scroll_backend = make_fcitx_backend(std::move(scrolling));
    require(fixture.scroll_backend != nullptr, "the scrolling backend could not be created");

    // `--quick` runs only the cells the known-case checks need, so the suite can keep this tool
    // as a smoke test; the full matrix is a manual run.
    const char *const quick_keys[] = {"A",       "N",           "F2",   "F5",    "Return",
                                      "PrintScreen", "Pause",   "ScrollLock", "Apps"};
    std::vector<std::size_t> key_indices;
    for (std::size_t k = 0; k < std::size(kKeys); ++k) {
      if (!quick) {
        key_indices.push_back(k);
        continue;
      }
      for (const char *name : quick_keys)
        if (std::string(kKeys[k].name) == name)
          key_indices.push_back(k);
    }
    std::vector<Cell> cells;
    std::vector<State> states;
    if (quick) {
      states = {State::Inactive, State::Idle, State::Composing, State::AfterToggle};
    } else {
      for (std::size_t s = 0; s < std::size(kStates); ++s)
        states.push_back(static_cast<State>(s));
    }
    for (std::size_t index = 0; index < key_indices.size(); ++index) {
      const KeyDef &key = kKeys[key_indices[index]];
      for (State state : states) {
        for (const Modifier &modifier : kModifiers)
          cells.push_back(measure_cell(fixture, key, state, modifier));
      }
      if (!quick)
        std::fprintf(stderr, "key %zu/%zu done (%zu cells)\n", index + 1, key_indices.size(),
                     cells.size());
    }

    const std::string csv = out_dir + "/key-matrix.csv";
    const std::string summary = out_dir + "/key-matrix-summary.md";
    write_csv(csv, cells);
    write_summary(summary, cells);
    check_known_cases(cells);
    std::printf("PASS key matrix: %zu cells, CSV %s, summary %s\n", cells.size(), csv.c_str(),
                summary.c_str());
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
  }
}
