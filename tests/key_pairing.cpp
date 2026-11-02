// R53 measurement: the T/K pairing counters are content-free, and these are the checks that say
// so — the counts themselves, and the one property that makes them safe to write: a summary line
// carries no key identity at all.

#include "tsf/key_pairing.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

using fusion::tsf::KeyEventId;
using fusion::tsf::KeyPairing;

namespace {

void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}

KeyEventId id(std::uint32_t scan, std::uint32_t time) { return KeyEventId{scan, time}; }

void check_pairing() {
  KeyPairing pairing;
  // One key, T then K: one of each, nothing unpaired.
  require(pairing.on_test_key(id(0x1E, 100), true) == 1, "the first key did not start at 1");
  require(pairing.on_key_down(id(0x1E, 100)) == 1, "K did not reuse the key's sequence number");
  require(pairing.counters().t == 1 && pairing.counters().t_true == 1 &&
              pairing.counters().k == 1,
          "a paired key was not counted once");
  require(pairing.counters().t_true_without_k == 0, "a completed pair was counted as unpaired");

  // The next key is a new sequence number.
  require(pairing.on_test_key(id(0x30, 200), false) == 2, "a new key did not advance the number");
  require(pairing.on_key_down(id(0x30, 200)) == 2, "K of the second key lost its number");
  require(pairing.counters().t_true == 1, "a FALSE T was counted as TRUE");

  // T TRUE and no K before the next key: the claim the host did not follow through.
  (void)pairing.on_test_key(id(0x1F, 300), true);
  (void)pairing.on_test_key(id(0x20, 400), true);
  require(pairing.counters().t_true_without_k == 1,
          "a T that answered TRUE with no K was not counted");
  (void)pairing.on_key_down(id(0x20, 400));
  require(pairing.counters().t_true_without_k == 1, "a completed pair changed the unpaired count");

  // The same physical key asked twice: same scan code and message time.
  const std::uint64_t first = pairing.on_test_key(id(0x21, 500), true);
  const std::uint64_t second = pairing.on_test_key(id(0x21, 500), true);
  require(first == second, "a repeated T was given a new sequence number");
  require(pairing.counters().t_repeated == 1, "a repeated T was not counted");
  (void)pairing.on_key_down(id(0x21, 500));

  // K without T.
  (void)pairing.on_key_down(id(0x22, 600));
  require(pairing.counters().k_without_t == 1, "a K with no T was not counted");

  // The same scan code at a different message time is a different press, not a repeat.
  (void)pairing.on_test_key(id(0x21, 700), true);
  require(pairing.counters().t_repeated == 1, "a second press was counted as a repeated T");

  // A release is numbered but is not a press.
  const auto before = pairing.counters();
  const std::uint64_t release = pairing.on_test_key_up(id(0x21, 800));
  require(release > 0, "a release was not numbered");
  require(pairing.counters().t == before.t && pairing.counters().k == before.k,
          "a release was counted as a press");

  // flush() closes an open TRUE pair, which is what deactivate needs.
  (void)pairing.on_test_key(id(0x23, 900), true);
  const std::uint64_t unpaired = pairing.counters().t_true_without_k;
  pairing.flush();
  require(pairing.counters().t_true_without_k == unpaired + 1,
          "flush did not close the open pair");
  pairing.flush();
  require(pairing.counters().t_true_without_k == unpaired + 1, "flush counted the same pair twice");

  // async_effects is a plain counter.
  pairing.on_async_effect();
  pairing.on_async_effect();
  require(pairing.counters().async_effects == 2, "async effects were not counted");
}

void check_summary_shape() {
  KeyPairing pairing;
  for (std::uint32_t i = 0; i < 200; ++i) {
    const KeyEventId key = id(0x10 + (i % 20), 1000 + i);
    (void)pairing.on_test_key(key, i % 3 == 0);
    (void)pairing.on_key_down(key);
    if (i + 1 < 200)
      require(!pairing.summary_due(), "the summary was due before 200 keys");
  }
  require(pairing.summary_due(), "the summary was not due at 200 keys");
  const std::string line = pairing.summary();
  // Exactly the documented fields, in order, and nothing else: a summary line that carried a
  // key, a context or a character would defeat the point of counting instead of logging keys.
  require(line.rfind("t=200 t_true=67 k=200 ", 0) == 0, "the summary fields are not the contract");
  require(line.find("t_true_without_k=") != std::string::npos &&
              line.find("t_repeated=") != std::string::npos &&
              line.find("k_without_t=") != std::string::npos &&
              line.find("async_effects=") != std::string::npos,
          "a summary field is missing");
  require(line.find("vk") == std::string::npos && line.find("scan") == std::string::npos &&
              line.find("0x") == std::string::npos,
          "the summary carries a key identity");
  std::printf("summary: %s\n", line.c_str());
}

} // namespace

int main() {
  try {
    check_pairing();
    check_summary_shape();
    std::printf("PASS key pairing counters\n");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
  }
}
