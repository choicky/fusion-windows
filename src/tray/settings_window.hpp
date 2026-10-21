#pragma once

// Locating and activating the settings window that is already open.
//
// The tray's Settings action must open the settings window or bring the existing one
// forward, and it must never start a second copy merely because Windows refused to raise
// a window. Three separate questions are answered here, because they fail independently:
//
//   1. Is there a window of the *intended* instance? The intended instance runs the exact
//      executable path the tray would launch, in this session, on the production endpoint.
//      A development copy, an older version directory, or a test instance started with
//      --pipe-name / --self-test / --verify-only is not it.
//   2. Was it activated? Foreground activation can be refused; that is not a reason to
//      launch again. The caller may request attention instead.
//   3. Is a launch already in flight? Repeated clicks while the process starts must not
//      stack copies, and a failed or exited launch must not lock the action out.
//
// Fusion's own tray icon is not the Windows input-language indicator; nothing here
// touches that.

#include <windows.h>

#include <functional>
#include <optional>
#include <string>

namespace fusion::tray {

struct SettingsInstance {
  HWND window = nullptr;
  DWORD pid = 0;
};

// What an already-running settings process is connected to.
enum class EndpointKind {
  Production, // no endpoint override: the same Host the tray would launch against
  Isolated,   // --pipe-name / --self-test / --verify-only: a test or development instance
  Unknown,    // the command line could not be read; treated as not the intended instance
};

// Pure decision over a settings command line (the part after the executable).
EndpointKind classify_endpoint(const std::wstring &command_line);

// The full command line of a same-user process, or nullopt when it cannot be read.
std::optional<std::wstring> query_command_line(DWORD pid);

// The intended instance: a visible, titled top-level window whose process runs
// `expected_executable_path` in `session_id` on the production endpoint. nullptr when
// there is none. An instance whose identity cannot be established is never returned.
SettingsInstance find_settings_instance(const std::wstring &expected_executable_path,
                                        DWORD session_id);

// What the tray should do about a click.
enum class SettingsAction {
  Activate,   // an intended instance exists and was raised
  NotifyOnly, // an intended instance exists but Windows refused to raise it
  Launch,     // no intended instance exists
};

// Pure decision, so the "found but activation refused" case is testable without a desktop.
SettingsAction decide_settings_action(bool instance_found, bool activated);

// Restores a minimized window and tries to bring it to the foreground. Returns true only
// when the window actually ends up in the foreground.
bool activate_window(HWND window);

// Requests the user's attention (taskbar flash) without changing the foreground window.
void request_attention(HWND window);

// Tracks one in-flight launch. Not thread-safe; used from the tray's UI thread only.
class LaunchGuard {
public:
  // `alive` reports whether the launched process is still running. The default queries the
  // handle; tests inject their own outcome.
  using AliveCheck = std::function<bool()>;

  explicit LaunchGuard(AliveCheck alive = {});
  LaunchGuard(const LaunchGuard &) = delete;
  LaunchGuard &operator=(const LaunchGuard &) = delete;
  ~LaunchGuard();

  // True while a launch is in flight and still inside the startup window.
  bool pending(ULONGLONG now) const;

  // Records a successful launch. Takes ownership of `process` (may be null).
  void started(HANDLE process, ULONGLONG now);

  // Forgets the in-flight launch (process exited, window appeared, or the user retried).
  void clear();

  static constexpr ULONGLONG kStartupTimeoutMs = 15000;

private:
  bool process_alive() const;

  HANDLE process_ = nullptr;
  ULONGLONG since_ = 0;
  bool started_ = false;
  AliveCheck alive_;
};

} // namespace fusion::tray
