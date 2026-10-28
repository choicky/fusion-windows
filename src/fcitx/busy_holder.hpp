#pragma once

// R51: naming the process a busy Host is waiting for.
//
// A settings save that arrives while an input is still live is refused with Busy, and the window
// then says it is waiting for the current input to end or the Host to become free. The owner's
// test (2026-10-28, R51) found that sentence unusable: it does not say whose input, so there is
// no window to go back and finish. The Host knows both halves of that answer already — which of
// the two checks a context failed, and which process bound it, because a pipe can report the
// process at its other end — so a refusal states them.
//
// The head is machine-readable because the sentence that follows it belongs to the Host and the
// status line the user reads belongs to the settings window, which speaks Chinese. It carries a
// process id, a base file name and the name of one of two conditions: no preedit, no key, no
// candidate, no commit, and no directory.

#include <windows.h>

#include <cstdint>
#include <string>

namespace fusion {

// The busy check's whole answer: whether anything is outstanding, and if so which of its two
// conditions and in which process. `pid` is the client that bound the context, 0 when the Host
// was never told — a context created without a pipe in front of it, or a pipe that would not say.
struct composition_holder {
  bool busy = false;
  std::uint32_t pid = 0;
  const char *condition = "";
};

// The base name of a client's executable, empty when the system will not say. A name and not a
// path: the folder a program lives in does not help anyone find the window to return to. Called
// on a refusal, which is a settings action and never a keystroke.
inline std::string client_process_name(std::uint32_t pid) {
  if (pid == 0)
    return {};
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr)
    return {};
  // On the heap, not on the stack: this runs on the Fcitx loop thread, whose stack is also
  // the engine's own call chain, and a 32 KiB array there buys nothing — the name that comes
  // out is a leaf of at most MAX_PATH characters. The buffer size is the documented maximum
  // for the call, so nothing about the answer changes.
  std::wstring buffer(32768, L'\0');
  DWORD characters = static_cast<DWORD>(buffer.size());
  std::string name;
  if (QueryFullProcessImageNameW(process, 0, buffer.data(), &characters) != 0) {
    const std::wstring path(buffer.data(), characters);
    const auto separator = path.find_last_of(L"\\/");
    const std::wstring leaf = separator == std::wstring::npos ? path : path.substr(separator + 1);
    std::string utf8;
    utf8.resize(leaf.size() * 3 + 1);
    const int written =
        WideCharToMultiByte(CP_UTF8, 0, leaf.data(), static_cast<int>(leaf.size()), utf8.data(),
                            static_cast<int>(utf8.size()), nullptr, nullptr);
    if (written > 0) {
      utf8.resize(static_cast<std::size_t>(written));
      name = std::move(utf8);
    }
  }
  CloseHandle(process);
  return name;
}

// `holder pid=16772 why=preedit exe=SearchHost.exe | `. `pid` is always present: 0 says the Host
// does not know, which is still an answer. `exe` appears only when the name resolved, and `why`
// only when there is a condition to name — so nothing is claimed that was not observed.
inline std::string busy_holder(const composition_holder &holder) {
  std::string head = "holder pid=" + std::to_string(holder.pid);
  if (holder.condition != nullptr && holder.condition[0] != '\0')
    head += std::string(" why=") + holder.condition;
  const std::string name = client_process_name(holder.pid);
  if (!name.empty())
    head += " exe=" + name;
  head += " | ";
  return head;
}

} // namespace fusion
