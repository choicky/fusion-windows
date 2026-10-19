#pragma once

// U6: which document managers have already received this thread's
// GUID_COMPARTMENT_KEYBOARD_OPENCLOSE "open" declaration.
//
// The declaration has to happen once per new application window and never from a key
// path, so an explicit Ctrl+Space close stays effective before and after the first key.
// This tracker answers only "is this document manager new to me?"; it decides nothing
// about the compartment itself.
//
// Lifetime: an entry is removed when TSF reports the document manager destroyed
// (OnUninitDocumentMgr), so a later window whose manager reuses the same pointer value
// is correctly treated as new. Identity is the pointer value, which is all the
// lifecycle callbacks provide.
//
// Capacity: the bound exists so a machine that never reports destruction cannot grow
// the set without limit. Reaching it never forgets a known manager and never declares
// one again: further managers are not tracked and are reported as at-capacity, so the
// caller skips the write instead of dropping what it already knows.

#include <cstddef>
#include <set>

namespace fusion::tsf {

class OpenDeclarationTracker {
public:
  enum class Mark {
    Added,          // new manager, now tracked: the caller declares the compartment open
    AlreadyTracked, // this window was already declared: never write the compartment again
    AtCapacity,     // new manager, not tracked: no declaration, nothing forgotten
  };

  static constexpr std::size_t kCapacity = 256;

  Mark mark_new(const void *manager) {
    if (manager == nullptr)
      return Mark::AtCapacity; // no identity to track; never a reason to write
    if (tracked_.count(manager) != 0)
      return Mark::AlreadyTracked;
    if (tracked_.size() >= kCapacity)
      return Mark::AtCapacity;
    tracked_.insert(manager);
    return Mark::Added;
  }

  void forget(const void *manager) { tracked_.erase(manager); }
  void clear() { tracked_.clear(); }

  std::size_t size() const { return tracked_.size(); }
  bool at_capacity() const { return tracked_.size() >= kCapacity; }

private:
  std::set<const void *> tracked_;
};

} // namespace fusion::tsf
