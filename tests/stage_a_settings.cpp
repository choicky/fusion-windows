// Stage A settings/list protocol and validation decisions — the two layers that
// are pure enough to test on any platform, without a Fcitx instance or a Host.
//
// Usage: stage_a_settings <protocol|bounds|validator>
//
// `protocol` and `bounds` exercise the additive settings_api_version 2 wire
// surface: the ImList envelope and the SetImList request. `validator` covers the
// product-side input-method list validation (im_list.hpp) that closes the two
// gaps the Fcitx core leaves open (an empty enabled list and an unavailable
// selection). The Host apply path itself (setGroup/save, the composition guard,
// global-config reload) needs the Fcitx event loop and is validated on Windows;
// it is NOT RUN here.

#include "fcitx/im_list.hpp"
#include "fusion/settings_protocol.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using fusion::ipc::detail::Reader;
using fusion::ipc::settings_detail::read_im_list;
using fusion::ipc::settings_detail::read_string_list;
using fusion::ipc::settings_detail::write_im_list;
using fusion::ipc::settings_detail::write_string_list;

void require(bool ok, const char *message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

std::vector<std::uint8_t> set_request_payload(const std::vector<std::string> &enabled) {
  return fusion::ipc::serialize_settings_set_im_list_request(enabled);
}

void test_protocol() {
  // A full ImList (active, fallback, ordered enabled, available) round-trips.
  const fusion::ImList list{
      "pinyin", "shuangpin", {"pinyin", "shuangpin"}, {"pinyin", "shuangpin", "wbpy"}};
  std::vector<std::uint8_t> payload;
  require(write_im_list(payload, list), "an in-bound ImList serializes");
  Reader reader{payload.data(), payload.size()};
  fusion::ImList back;
  require(read_im_list(reader, back), "an in-bound ImList deserializes");
  require(reader.at_end(), "ImList reader consumes the whole payload");
  require(back == list, "ImList round-trips field-for-field");

  // Empty enabled / available and an empty fallback are representable and stable.
  const fusion::ImList bare{"pinyin", "", {"pinyin"}, {}};
  std::vector<std::uint8_t> bare_payload;
  require(write_im_list(bare_payload, bare), "a single-method ImList serializes");
  Reader bare_reader{bare_payload.data(), bare_payload.size()};
  fusion::ImList bare_back;
  require(read_im_list(bare_reader, bare_back) && bare_reader.at_end(),
          "a single-method ImList round-trips");
  require(bare_back == bare, "empty fallback and available are preserved");

  // The SetImList request carries only the ordered enabled list; it round-trips,
  // including an empty list (which the validator later refuses at the Host).
  for (const std::vector<std::string> &enabled :
       {std::vector<std::string>{}, {"pinyin"}, {"shuangpin", "pinyin"}}) {
    const auto request = set_request_payload(enabled);
    std::vector<std::string> parsed;
    require(!request.empty() || enabled.empty(),
            "a non-empty enabled list never serializes to an empty request");
    require(fusion::ipc::deserialize_settings_set_im_list_request(request.data(), request.size(),
                                                                  parsed),
            "the SetImList request deserializes");
    require(parsed == enabled, "the SetImList request round-trips the enabled order");
  }

  // A GetImList / GetSelection request is an empty payload; max_message_type must
  // admit the new response types so a bound Host accepts the frame. Dictionary
  // management (settings API 3) added the last six, so the bound follows them.
  static_assert(fusion::ipc::max_message_type ==
                static_cast<std::uint8_t>(
                    fusion::ipc::MessageType::SettingsReloadDictionariesResponse));
}

void test_bounds() {
  // A list longer than max_vector_length is refused on write, not truncated.
  std::vector<std::string> too_long(fusion::ipc::max_vector_length + 1, "pinyin");
  std::vector<std::uint8_t> payload;
  require(!write_string_list(payload, too_long), "an over-bound list is refused on write");

  // Serializing an over-bound SetImList request yields an empty payload (failure).
  require(set_request_payload(too_long).empty(),
          "an over-bound SetImList request does not serialize");

  // A crafted list header claiming more entries than the bound must fail closed
  // when read, before any allocation follows it.
  std::vector<std::uint8_t> forged;
  fusion::ipc::detail::write_u32(forged, static_cast<std::uint32_t>(fusion::ipc::max_vector_length) + 1u);
  Reader reader{forged.data(), forged.size()};
  std::vector<std::string> items;
  require(!read_string_list(reader, items), "an over-bound list header fails closed on read");

  // Trailing bytes after a complete ImList are refused (fail-closed framing).
  const fusion::ImList list{"pinyin", "pinyin", {"pinyin"}, {"pinyin"}};
  std::vector<std::uint8_t> with_tail;
  require(write_im_list(with_tail, list), "base ImList serializes");
  with_tail.push_back(0x00); // one trailing byte
  Reader tail_reader{with_tail.data(), with_tail.size()};
  fusion::ImList out;
  bool consumed = read_im_list(tail_reader, out);
  require(consumed && !tail_reader.at_end(),
          "an ImList with trailing bytes is detectable and rejected by the caller");

  // A truncated ImList (declared but not present) fails closed.
  std::vector<std::uint8_t> truncated;
  fusion::ipc::detail::write_u32(truncated, 4u); // length prefix says "piny"
  fusion::ipc::detail::write_u8(truncated, static_cast<std::uint8_t>('p'));
  Reader short_reader{truncated.data(), truncated.size()};
  fusion::ImList unused;
  require(!read_im_list(short_reader, unused), "a truncated ImList fails closed on read");
}

void test_validator() {
  using fusion::selection::plan_im_list_update;
  const std::vector<std::string> available{"pinyin", "shuangpin"};

  // A reorder keeps the requested order and carries the previous fallback through
  // untouched — Fcitx's setDefaultInputMethod re-establishes its own invariant.
  const auto reordered = plan_im_list_update({"shuangpin", "pinyin"}, available, "pinyin");
  require(reordered.has_value(), "a valid reorder is accepted");
  require(reordered->methods == std::vector<std::string>{"shuangpin", "pinyin"},
          "the requested order is written verbatim");
  require(reordered->requested_fallback == "pinyin",
          "the previous fallback is passed straight to setDefaultInputMethod");

  // The active method is Items/0 — the first entry of the ordered list.
  require(!reordered->methods.empty() && reordered->methods.front() == "shuangpin",
          "the first enabled entry is the active method");

  // Duplicates collapse to the first occurrence; order is otherwise preserved.
  const auto deduped = plan_im_list_update({"pinyin", "shuangpin", "pinyin"}, available, "");
  require(deduped.has_value(), "a duplicate-bearing request is accepted after dedupe");
  require(deduped->methods == std::vector<std::string>{"pinyin", "shuangpin"},
          "duplicates collapse to their first occurrence");

  // An empty enabled list is refused: the core would persist a group with no
  // input method, leaving contexts nothing to select.
  require(!plan_im_list_update({}, available, "pinyin").has_value(),
          "an empty enabled list is refused");
  require(!plan_im_list_update({"wbpy"}, available, "").has_value(),
          "a list of only unavailable names is refused");

  // Any name outside the packaged runtime's defined set is refused rather than
  // silently dropped by setGroup, so a partial success cannot look like a save.
  require(!plan_im_list_update({"pinyin", "wbpy"}, available, "pinyin").has_value(),
          "a single unavailable name rejects the whole list");

  // A runtime that defines a single method still accepts that one method.
  const auto only_pinyin = plan_im_list_update({"pinyin"}, {"pinyin"}, "");
  require(only_pinyin.has_value() && only_pinyin->methods == std::vector<std::string>{"pinyin"},
          "a single-method runtime accepts its only method");
}

} // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  try {
    if (mode == "protocol") {
      test_protocol();
      std::cout << "PASS stage_a_settings protocol: ImList + SetImList round-trip\n";
    } else if (mode == "bounds") {
      test_bounds();
      std::cout << "PASS stage_a_settings bounds: over-bound and malformed lists fail closed\n";
    } else if (mode == "validator") {
      test_validator();
      std::cout << "PASS stage_a_settings validator: empty and unavailable lists refused\n";
    } else {
      std::cerr << "usage: stage_a_settings <protocol|bounds|validator>\n";
      return 2;
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL stage_a_settings " << mode << ": " << e.what() << "\n";
    return 1;
  }
}
