#include "tsf/key_pairing.hpp"

#include <cstdio>

namespace fusion::tsf {

std::uint64_t KeyPairing::touch(KeyEventId id) {
  if (!started_ || id != current_) {
    // The previous key is finished: a T that answered TRUE and never saw K was a claim the
    // host did not follow through, which is the one shape the Weasel model cannot tolerate.
    if (pending_true_)
      ++counters_.t_true_without_k;
    pending_true_ = false;
    seen_t_ = false;
    current_ = id;
    ++sequence_;
    started_ = true;
  }
  return sequence_;
}

std::uint64_t KeyPairing::on_test_key(KeyEventId id, bool eligible) {
  const std::uint64_t sequence = touch(id);
  ++counters_.t;
  if (seen_t_)
    ++counters_.t_repeated; // the same physical key asked twice
  seen_t_ = true;
  if (eligible) {
    ++counters_.t_true;
    pending_true_ = true;
  } else {
    pending_true_ = false;
  }
  return sequence;
}

std::uint64_t KeyPairing::on_key_down(KeyEventId id) {
  const std::uint64_t sequence = touch(id);
  ++counters_.k;
  if (pending_true_) {
    pending_true_ = false; // the pair completed
  } else if (!seen_t_) {
    ++counters_.k_without_t;
  }
  return sequence;
}

std::uint64_t KeyPairing::on_test_key_up(KeyEventId id) { return touch(id); }

std::uint64_t KeyPairing::on_key_up(KeyEventId id) { return touch(id); }

void KeyPairing::on_async_effect() { ++counters_.async_effects; }

void KeyPairing::flush() {
  if (pending_true_) {
    ++counters_.t_true_without_k;
    pending_true_ = false;
  }
  seen_t_ = false;
  started_ = false;
}

bool KeyPairing::summary_due() const {
  return counters_.k != 0 && counters_.k % 200 == 0;
}

std::string KeyPairing::summary() const {
  char line[320];
  const int length = std::snprintf(line, sizeof(line),
                                   "t=%llu t_true=%llu k=%llu t_true_without_k=%llu "
                                   "t_repeated=%llu k_without_t=%llu async_effects=%llu",
                                   static_cast<unsigned long long>(counters_.t),
                                   static_cast<unsigned long long>(counters_.t_true),
                                   static_cast<unsigned long long>(counters_.k),
                                   static_cast<unsigned long long>(counters_.t_true_without_k),
                                   static_cast<unsigned long long>(counters_.t_repeated),
                                   static_cast<unsigned long long>(counters_.k_without_t),
                                   static_cast<unsigned long long>(counters_.async_effects));
  return length > 0 ? std::string(line, static_cast<std::size_t>(length)) : std::string();
}

void KeyPairing::reset() {
  counters_ = KeyPairingCounters{};
  current_ = KeyEventId{};
  sequence_ = 0;
  started_ = false;
  seen_t_ = false;
  pending_true_ = false;
}

} // namespace fusion::tsf
