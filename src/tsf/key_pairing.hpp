#pragma once

// R53 measurement: content-free T/K pairing counters.
//
// TSF asks the text service twice per key — OnTestKeyDown (T) and OnKeyDown (K) — and some
// hosts ask T more than once, or never deliver K, or deliver K without T. Whether a host does
// that decides which design Fusion can use (see the key-decision survey): the Weasel model
// processes in T and caches for K, and it only works if T and K really do pair.
//
// This unit counts the pairs, and nothing else. It never sees a character: the identity of a
// key is its scan code and the message time of the event that carried it, both of which the
// log already redacts for every key that could carry content. The counters are the only thing
// that reaches the log, and they carry no identity at all.
//
// Everything here is a pure state machine so it can be unit-tested without TSF.

#include <cstdint>
#include <string>

namespace fusion::tsf {

// One physical key event, as the TIP can name it without naming the key: the scan code from the
// lparam and the message time the event was delivered with.
struct KeyEventId {
  std::uint32_t scan = 0;
  std::uint32_t time = 0;
  bool operator==(const KeyEventId &other) const {
    return scan == other.scan && time == other.time;
  }
  bool operator!=(const KeyEventId &other) const { return !(*this == other); }
};

struct KeyPairingCounters {
  std::uint64_t t = 0;                // T calls
  std::uint64_t t_true = 0;           // ... that answered TRUE
  std::uint64_t k = 0;                // K calls
  std::uint64_t t_true_without_k = 0; // T answered TRUE and no K followed before the next key
  std::uint64_t t_repeated = 0;       // a second T for the same physical key
  std::uint64_t k_without_t = 0;      // K arrived with no T for that key
  std::uint64_t async_effects = 0;    // document writes that ran in an async edit session
};

class KeyPairing {
public:
  // Every entry point returns the sequence number of the physical key the event belongs to, so
  // the log can pair the lines without carrying the key. T and K for one key share it; a new
  // key starts the next one.
  std::uint64_t on_test_key(KeyEventId id, bool eligible);
  std::uint64_t on_key_down(KeyEventId id);
  // Releases are numbered from the same counter so the log stays ordered, but they are not part
  // of the press counters: a release is a different physical event with its own message time.
  std::uint64_t on_test_key_up(KeyEventId id);
  std::uint64_t on_key_up(KeyEventId id);
  void on_async_effect();

  // A T that answered TRUE and was never followed by K is only knowable when the next key
  // arrives — or when the context goes away. `flush` closes the open pair without inventing a
  // key, so a deactivate does not lose the last one.
  void flush();

  // True once every 200 K events, which is where the caller writes the summary line.
  bool summary_due() const;
  // `t=.. t_true=.. k=.. t_true_without_k=.. t_repeated=.. k_without_t=.. async_effects=..`
  std::string summary() const;
  const KeyPairingCounters &counters() const { return counters_; }
  void reset();

private:
  std::uint64_t touch(KeyEventId id);
  KeyPairingCounters counters_{};
  KeyEventId current_{};
  std::uint64_t sequence_ = 0;
  bool started_ = false;
  bool seen_t_ = false;     // a T was seen for the current key
  bool pending_true_ = false; // ... and it answered TRUE with no K since
};

} // namespace fusion::tsf
