// R51: the head a busy Host puts in front of its refusal, on its own.
//
// A settings save that arrives while an input is still live is refused, and the window says it is
// waiting. The owner's test found that sentence unusable without a name: the user has no window to
// go back and finish. The Host knows the name — the process at the other end of the pipe that bound
// the context — so a refusal states it, in a head the settings window can read. This check feeds
// that head the shapes the Host actually produces and reads back what a user would see; the pipe
// that supplies the process id is covered by the Host-side check, and the Chinese clause built from
// this head by the settings window's own tests.
//
// What it does NOT cover: which condition a live Fcitx context fails (that is the composition gate,
// unchanged and asserted where it is used), and a client process the Host has no pipe in front of.

#include "fcitx/busy_holder.hpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

using fusion::busy_holder;
using fusion::client_process_name;
using fusion::composition_holder;

constexpr const char *kSentence = "Nothing was written.";

// The base name of this test binary, spelled the way the product spells it: read from the system
// and converted, rather than written into the check as a literal that could rot.
std::string self_name() {
  wchar_t buffer[32768];
  const DWORD characters = GetModuleFileNameW(
      nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]) - 1));
  if (characters == 0)
    return {};
  const std::wstring path(buffer, characters);
  const auto separator = path.find_last_of(L"\\/");
  const std::wstring leaf = separator == std::wstring::npos ? path : path.substr(separator + 1);
  std::string utf8;
  utf8.resize(leaf.size() * 3 + 1);
  const int written =
      WideCharToMultiByte(CP_UTF8, 0, leaf.data(), static_cast<int>(leaf.size()), utf8.data(),
                          static_cast<int>(utf8.size()), nullptr, nullptr);
  if (written <= 0)
    return {};
  utf8.resize(static_cast<std::size_t>(written));
  return utf8;
}

void an_unidentified_client_is_still_answered() {
  // A context bound without a pipe transport in front of it — the in-process tests, a Host that
  // asked its pipe and was refused — carries pid 0. That is an answer, so the head stays.
  const std::string head = busy_holder(composition_holder{true, 0, "preedit"});
  CHECK(head == std::string("holder pid=0 why=preedit | "));
  CHECK(head.find("exe=") == std::string::npos);
}

void a_known_client_is_named_by_process_and_condition() {
  const std::uint32_t pid = GetCurrentProcessId();
  const std::string head = busy_holder(composition_holder{true, pid, "pending-events"});
  CHECK(head.rfind("holder pid=" + std::to_string(pid) + " why=pending-events", 0) == 0);
  CHECK(head.find(" exe=" + self_name() + " | ") != std::string::npos);
  // The head always ends by handing off to the Host's own sentence; a caller appends the two.
  CHECK(head.size() >= 2 && head.substr(head.size() - 2) == "| ");
}

void nothing_is_claimed_that_was_not_observed() {
  // Two independent absences: no condition because the check that fired is not one of the two the
  // Host can name, and no name because the system would not say what a process it does not
  // recognise is. Neither is invented to fill the gap.
  const std::string no_condition = busy_holder(composition_holder{true, 0, ""});
  CHECK(no_condition == "holder pid=0 | ");
  CHECK(busy_holder(composition_holder{true, 0, nullptr}) == no_condition);
  const std::string unknown_process = client_process_name(MAXDWORD);
  // Either the system would not say what this process is, or it said a base name — never a path,
  // and never a name the Host was not given.
  CHECK(unknown_process.find('\\') == std::string::npos);
  CHECK(unknown_process.find('/') == std::string::npos);
  const std::string head = busy_holder(composition_holder{true, MAXDWORD, "preedit"});
  CHECK(head.rfind("holder pid=4294967295 why=preedit", 0) == 0);
  CHECK(unknown_process.empty() ? head.find("exe=") == std::string::npos
                                : head.find("exe=" + unknown_process) != std::string::npos);
}

void the_two_conditions_are_named_as_the_gate_reports_them() {
  // The settings window maps these two words to the two Chinese clauses it shows, so they are part
  // of the contract, not the Host's private vocabulary.
  CHECK(busy_holder(composition_holder{true, 0, "preedit"}).find("why=preedit ") !=
        std::string::npos);
  CHECK(busy_holder(composition_holder{true, 0, "pending-events"}).find("why=pending-events ") !=
        std::string::npos);
  const std::string with_sentence = busy_holder(composition_holder{true, 0, "preedit"}) + kSentence;
  CHECK(with_sentence == "holder pid=0 why=preedit | Nothing was written.");
}

} // namespace

int main() {
  an_unidentified_client_is_still_answered();
  a_known_client_is_named_by_process_and_condition();
  nothing_is_claimed_that_was_not_observed();
  the_two_conditions_are_named_as_the_gate_reports_them();
  if (failures == 0)
    std::printf("busy_holder: ok\n");
  std::fprintf(stderr, "busy_holder: %d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
