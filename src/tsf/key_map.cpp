#include "tsf/key_map.hpp"

namespace fusion::tsf {

KeyAction classify_key(std::uint32_t virtual_key, bool ctrl, bool alt, bool win) {
  if (ctrl || alt || win) {
    return {KeyAction::Type::Pass, '\0', virtual_key, ctrl || alt || win};
  }
  if (virtual_key >= 'A' && virtual_key <= 'Z') {
    // Raw IME-style input is lowercase regardless of shift state (dev policy).
    return {KeyAction::Type::AppendChar, static_cast<char>(virtual_key - 'A' + 'a'), virtual_key};
  }
  if (virtual_key >= '0' && virtual_key <= '9') {
    return {KeyAction::Type::AppendChar, static_cast<char>(virtual_key), virtual_key};
  }
  if (virtual_key >= kVkNumpad0 && virtual_key <= kVkNumpad9) {
    return {KeyAction::Type::AppendChar, static_cast<char>('0' + (virtual_key - kVkNumpad0)),
            virtual_key};
  }
  if (virtual_key == kVkSpace) {
    return {KeyAction::Type::Commit, '\0', virtual_key};
  }
  if (virtual_key == kVkBack) {
    return {KeyAction::Type::Backspace, '\0', virtual_key};
  }
  if (virtual_key == kVkEscape) {
    return {KeyAction::Type::Reject, '\0', virtual_key};
  }
  return {KeyAction::Type::Pass, '\0', virtual_key, ctrl || alt || win};
}

KeyAction translate_key(std::uint32_t vk, bool ctrl, bool alt, bool win, char32_t character,
                        bool shift, bool caps) {
  KeyAction action = classify_key(vk, ctrl, alt, win);
  if (ctrl || alt || win)
    return action; // unsupported system/modifier combinations: platform policy
  action.modifiers = (shift ? 1u : 0u) | (caps ? 2u : 0u);
  if (character >= 0x20 && character <= 0x10ffff && !(character >= 0xd800 && character <= 0xdfff)) {
    action.keysym = character < 0x100 ? static_cast<std::uint32_t>(character)
                                      : 0x01000000u | static_cast<std::uint32_t>(character);
    return action;
  }
  // Virtual-key to keysym translation only. No IME semantic key classes.
  switch (vk) {
  case kVkBack:
    action.keysym = 0xff08;
    break;
  case 0x09:
    action.keysym = 0xff09;
    break;
  case kVkReturn:
    action.keysym = 0xff0d;
    break;
  case kVkEscape:
    action.keysym = 0xff1b;
    break;
  case kVkSpace:
    action.keysym = 0x20;
    break;
  case 0x21:
    action.keysym = 0xff55;
    break;
  case 0x22:
    action.keysym = 0xff56;
    break;
  case 0x23:
    action.keysym = 0xff57;
    break;
  case 0x24:
    action.keysym = 0xff50;
    break;
  case 0x25:
    action.keysym = 0xff51;
    break;
  case 0x26:
    action.keysym = 0xff52;
    break;
  case 0x27:
    action.keysym = 0xff53;
    break;
  case 0x28:
    action.keysym = 0xff54;
    break;
  case 0x2d:
    action.keysym = 0xff63;
    break;
  case 0x2e:
    action.keysym = 0xffff;
    break;
  default:
    if (vk >= 0x70 && vk <= 0x87)
      action.keysym = 0xffbe + vk - 0x70;
    break;
  }
  return action;
}

} // namespace fusion::tsf
