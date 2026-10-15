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

} // namespace fusion::tsf
