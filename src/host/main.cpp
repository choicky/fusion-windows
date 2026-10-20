// FusionHost.exe — Stage 4A out-of-process Fcitx Host.
//
// Creates a Named Pipe server, instantiates the real Fcitx backend,
// and dispatches IPC requests from connected TSF DLL clients.
// Default pipe name is computed from the user SID and session ID.
//
// Stage 6 WP3 adds three ordered steps to this entry point:
//   * the pipe singleton is claimed before the backend exists (A5), so a Host that
//     is not the singleton never constructs Fcitx and never touches user data;
//   * the Fcitx per-user path is enabled for the product Host and skipped for
//     tests/fixtures (A1/A2), through the backend option;
//   * console control events request an orderly stop, which runs the backend
//     shutdown and therefore Fcitx's own save on instance exit (A4).
//
// Stage 6 WP4 makes the packaged Host console-free and adds the two non-console
// ways to ask for the same orderly stop:
//   * windowless (default): the process has no console at all, and a hidden
//     top-level window receives WM_QUERYENDSESSION / WM_ENDSESSION, so logoff and
//     shutdown still run the ordered shutdown (R26);
//   * a named stop event (per user SID x session, user-only DACL) that an elevated
//     install/upgrade/uninstall signals instead of terminating the Host.
// Both only *request* the stop: main() stays the single thread that performs the
// ordered shutdown (stop -> shutdown_backend -> Fcitx's own save on exit), exactly
// as the console path does. `--console` keeps the console mode with its control
// handler for development and tests.
//
// Stage 6 R34: none of these paths makes the Host survive a *cancelled* session end.
// Windows delivers WM_ENDSESSION(TRUE) to applications one after another during a
// logoff, so an application processed before the blocking one has already ended when the
// user's Cancel takes effect (measured 2026-10-18). Surviving that is the scheduled
// task's event trigger (User32 1077 -> start the Host again), not this process.

#include "fusion/backend.hpp"
#include "fusion/ipc_protocol.hpp"
#include "host/host_diag.hpp"
#include "host/host_server.hpp"
#include "ipc/pipe_security.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// CommandLineToArgvW (the windowless entry point parses wide arguments itself).
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::wstring parse_argument(const std::vector<std::wstring> &arguments, const wchar_t *name) {
  for (size_t i = 1; i + 1 < arguments.size(); ++i) {
    if (arguments[i] == name)
      return arguments[i + 1];
  }
  return {};
}

std::wstring parse_pipe_name(const std::vector<std::wstring> &arguments) {
  const auto override_name = parse_argument(arguments, L"--pipe-name");
  return override_name.empty() ? fusion::ipc::compute_pipe_name() : override_name;
}

std::wstring pipe_object_scope(const std::wstring &pipe_name) {
  std::wstring scope = pipe_name;
  const std::wstring prefix = fusion::ipc::default_pipe_name_prefix;
  if (scope.rfind(prefix, 0) == 0) {
    scope.erase(0, prefix.size());
  }
  for (wchar_t &character : scope) {
    if (character == L'\\' || character == L'/') {
      character = L'-';
    }
  }
  return scope;
}

bool has_switch(const std::vector<std::wstring> &arguments, const wchar_t *name) {
  for (size_t i = 1; i < arguments.size(); ++i) {
    if (arguments[i] == name)
      return true;
  }
  return false;
}

bool env_flag(const char *name) {
  const char *value = std::getenv(name);
  return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
}

// The Host instance the console handler may ask to stop. The pointer is guarded by
// `host_server_mutex`: the handler holds the lock while it uses the server, and main
// holds it when it clears the pointer, so the handler can never call into a destroyed
// HostServer.
std::mutex host_server_mutex;
fusion::ipc::HostServer *host_server = nullptr;

// Signals that main() has finished the orderly exit (accept loop ended, connections
// drained and joined, backend destroyed and Fcitx's own save completed). A console
// close/logoff/shutdown handler and the session-end window wait on it, bounded,
// instead of shutting anything down themselves: main() is the single thread that
// performs the ordered shutdown.
HANDLE exit_complete = nullptr;

// Windows allows about 5 s between a console close / session end notification and
// forced termination.
constexpr DWORD console_exit_timeout_ms = 4000;

// Requests the stop; never drains, joins or touches the backend. `path` names the
// caller for the diagnostic log (R30): stop event, session end, window close or
// console control. The log line is written before taking the server lock, so it can
// never extend the session-end bounded wait.
void request_host_stop(const char *path) {
  fusion::host::diag::event("stop_requested", (std::string("path=") + path).c_str());
  std::lock_guard lock(host_server_mutex);
  if (host_server != nullptr)
    host_server->request_stop();
}

BOOL WINAPI console_control(DWORD type) {
  switch (type) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
    // main() keeps running after this handler, so only request the stop; main() then
    // performs the whole ordered shutdown on its own thread.
    fusion::host::diag::event("console_control", ("type=" + std::to_string(type)).c_str());
    request_host_stop("console_control");
    return TRUE;
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    // Windows terminates the process when this handler returns, so request the stop
    // and wait (bounded) for main() to finish it, including Fcitx's save. The wait
    // never holds host_server_mutex, so main can clear the pointer and destroy the
    // server while this thread is only waiting. On timeout the process is terminating
    // anyway and Fcitx's forced-termination semantics apply.
    std::fprintf(stderr, "[FusionHost] console control %lu: waiting for the orderly exit\n",
                 static_cast<unsigned long>(type));
    fusion::host::diag::event("console_control", ("type=" + std::to_string(type)).c_str());
    request_host_stop("console_control");
    if (exit_complete != nullptr)
      WaitForSingleObject(exit_complete, console_exit_timeout_ms);
    return TRUE;
  default:
    return FALSE;
  }
}

// The windowless Host's hidden top-level window. It must be a top-level window:
// a message-only window (HWND_MESSAGE) receives no session-end broadcast.
constexpr wchar_t session_end_window_class[] = L"FusionHostSessionEnd";

HWND session_end_window = nullptr;
DWORD session_end_window_thread = 0;
std::mutex session_end_window_mutex;

// Created before the window thread starts, read by the window procedure, released
// after that thread has been joined.
std::unique_ptr<fusion::ipc::SessionEndHandler> session_end_handler;

LRESULT CALLBACK session_end_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  (void)lparam;
  if (session_end_handler != nullptr && fusion::ipc::SessionEndHandler::handles(message))
    return static_cast<LRESULT>(session_end_handler->handle(message, wparam));
  switch (message) {
  case WM_CLOSE:
    // An external close (taskkill without /f, a window closer) must not silently remove
    // the Host's only logoff/shutdown path: before the exit it requests the orderly stop
    // and the window stays; after the exit this is main()'s own close and the window
    // goes. With no handler (never in a started Host, but the branch must not hang the
    // join) the window is destroyed as before.
    if (session_end_handler == nullptr || !session_end_handler->close_keeps_window()) {
      DestroyWindow(window);
    }
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProcW(window, message, wparam, lparam);
  }
}

// Creates the hidden window and pumps its messages on a thread of its own, so the
// main thread can stay in the accept loop and remain the single thread that
// performs the ordered shutdown. `ready` is signalled once the window exists (or
// once window creation has failed, leaving session_end_window null).
void run_session_end_window(HANDLE ready) {
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = session_end_window_proc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = session_end_window_class;
  HWND window = nullptr;
  if (RegisterClassExW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
    window = CreateWindowExW(WS_EX_TOOLWINDOW, session_end_window_class, L"FusionHost", WS_POPUP, 0,
                             0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  }
  {
    std::lock_guard lock(session_end_window_mutex);
    session_end_window = window;
    session_end_window_thread = GetCurrentThreadId();
  }
  SetEvent(ready);
  if (window == nullptr)
    return;
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

// Console mode: make sure the process belongs to a console, because control
// events (CTRL_BREAK, close, logoff) only reach a process that has one. A
// Windows-subsystem binary does not inherit the caller's console, so it is
// attached explicitly; a redirected stderr is left untouched, which keeps the
// harness pattern (redirect stderr, then deliver CTRL_BREAK) working.
void prepare_console_mode() {
  if (GetConsoleWindow() != nullptr)
    return; // the launcher already gave this process a console
  if (!AttachConsole(ATTACH_PARENT_PROCESS))
    return; // no console to attach to; stderr may still be a pipe or a file
  const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
  if (error == nullptr || error == INVALID_HANDLE_VALUE) {
    FILE *stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stderr);
  }
}

int run_host(const std::vector<std::wstring> &arguments) {
  const bool console_mode = has_switch(arguments, L"--console");
  if (console_mode)
    prepare_console_mode();
  auto pipe_name = parse_pipe_name(arguments);
  std::fprintf(stderr, "[FusionHost] starting with pipe: %ls\n", pipe_name.c_str());
  std::fprintf(stderr, "[FusionHost] mode: %s\n", console_mode ? "console" : "windowless");
  fusion::host::diag::event("start", ("pipe=" + fusion::host::diag::utf8(pipe_name) +
                                      " mode=" + (console_mode ? "console" : "windowless"))
                                         .c_str());

  // A5: claim the Host singleton before creating the backend, so a Host that is
  // not the singleton never constructs Fcitx, which would load the user profile
  // and save on exit. The pipe keeps its own first-instance authority.
  fusion::ipc::HostClaim claim;
  claim.acquire(pipe_name);
  if (!claim.held()) {
    std::fprintf(stderr, "[FusionHost] another Host already owns this pipe name; exiting without "
                         "touching user data\n");
    fusion::host::diag::event("singleton_claim", "ok=0 action=exit");
    return 1;
  }
  std::fprintf(stderr, "[FusionHost] singleton claim: %ls\n", claim.name().c_str());
  fusion::host::diag::event("singleton_claim",
                            ("ok=1 name=" + fusion::host::diag::utf8(claim.name())).c_str());

  // WP4: the named stop event is created before the backend exists, for the same
  // reason as the claim: a Host that cannot be stopped orderly must not start
  // loading or saving user data. It is released only when this process exits.
  fusion::ipc::HostStopEvent stop_event;
  // Keep test Host scopes separate from the production stop event. The packaged
  // Host's default pipe derives the existing per-user/session stop-event name;
  // explicit --pipe-name test instances use their own matching Local namespace.
  std::wstring stop_scope = parse_argument(arguments, L"--stop-event-scope");
  if (stop_scope.empty()) {
    stop_scope = pipe_object_scope(pipe_name);
  }
  if (!stop_event.create(stop_scope)) {
    std::fprintf(stderr,
                 "[FusionHost] cannot create the stop event; refusing to run without an orderly "
                 "stop path\n");
    fusion::host::diag::event("stop_event", "ok=0 action=exit");
    return 1;
  }
  std::fprintf(stderr, "[FusionHost] stop event: %ls\n", stop_event.name().c_str());
  fusion::host::diag::event("stop_event",
                            ("ok=1 name=" + fusion::host::diag::utf8(stop_event.name())).c_str());

  // U1: the product Host persists through Fcitx's own per-user paths. The skip
  // switch exists for automated tests and fixtures that spawn this Host; it is
  // announced loudly so a stray environment variable cannot silently disable
  // persistence.
  const bool user_path = !env_flag("FUSION_FCITX_SKIP_USER_PATH");
  if (user_path) {
    std::fprintf(stderr, "[FusionHost] Fcitx user path: standard (per-user Fcitx paths)\n");
    fusion::host::diag::event("fcitx_user_path", "enabled=1");
  } else {
    std::fprintf(stderr,
                 "[FusionHost] WARNING: FUSION_FCITX_SKIP_USER_PATH is set - Fcitx user data is "
                 "DISABLED (no persistence, no user dictionary)\n");
    fusion::host::diag::event("fcitx_user_path", "enabled=0");
  }

  std::unique_ptr<fusion::Backend> backend;
  try {
    backend = fusion::make_fcitx_backend(fusion::FcitxBackendOptions{user_path});
  } catch (const std::exception &e) {
    std::fprintf(stderr, "[FusionHost] Fcitx backend creation failed: %s\n", e.what());
    fusion::host::diag::event("backend_create_failed", ("error=" + std::string(e.what())).c_str());
    return 1;
  }
  if (!backend) {
    std::fprintf(stderr, "[FusionHost] Fcitx backend creation returned null\n");
    fusion::host::diag::event("backend_create_failed", "error=null");
    return 1;
  }
  std::fprintf(stderr, "[FusionHost] Fcitx backend ready, session=%llu\n",
               static_cast<unsigned long long>(backend->session_id()));
  fusion::host::diag::event("backend_ready",
                            ("session=" + std::to_string(backend->session_id())).c_str());

  exit_complete = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (exit_complete == nullptr) {
    std::fprintf(stderr, "[FusionHost] cannot create the exit signal\n");
    fusion::host::diag::event("exit_signal", "ok=0 action=exit");
    return 1;
  }

  // Windowless mode: without a console there is no control handler, so the hidden
  // window is what carries logoff/shutdown. Without it the Host could not save on
  // session end (R26), so a failed window is fatal rather than silent.
  std::thread window_thread;
  if (!console_mode) {
    // The window procedure only delegates the session-end decision; it is created
    // before the window thread starts and released after that thread is joined.
    session_end_handler = std::make_unique<fusion::ipc::SessionEndHandler>(
        [](const char *path) { request_host_stop(path); }, exit_complete, console_exit_timeout_ms);
    HANDLE window_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (window_ready == nullptr) {
      std::fprintf(stderr, "[FusionHost] cannot create the window-start signal\n");
      fusion::host::diag::event("session_end_window", "ok=0 reason=start_signal action=exit");
      return 1;
    }
    window_thread = std::thread(run_session_end_window, window_ready);
    WaitForSingleObject(window_ready, 5000);
    CloseHandle(window_ready);
    if (session_end_window == nullptr) {
      std::fprintf(stderr,
                   "[FusionHost] cannot create the session-end window; refusing to run without a "
                   "logoff/shutdown path\n");
      fusion::host::diag::event("session_end_window", "ok=0 reason=create_window action=exit");
      SetEvent(exit_complete);
      window_thread.join();
      return 1;
    }
  }

  // The server lives in its own scope: main() is the single thread that performs the
  // ordered shutdown, and the exit signal is set only after the server is gone.
  {
    fusion::ipc::HostServer server(std::move(backend), pipe_name, 0);
    if (!server.start()) {
      std::fprintf(stderr, "[FusionHost] failed to initialize server\n");
      fusion::host::diag::event("server_start", "ok=0 action=exit");
      SetEvent(exit_complete);
      if (window_thread.joinable()) {
        PostMessageW(session_end_window, WM_CLOSE, 0, 0);
        window_thread.join();
      }
      return 1;
    }
    // The incarnation distinguishes this Host process from any earlier or later
    // one for client-side fencing; it is an engineering identity, not a frozen ID.
    std::fprintf(stderr, "[FusionHost] incarnation=%llu\n",
                 static_cast<unsigned long long>(server.incarnation()));
    fusion::host::diag::event("incarnation",
                              ("value=" + std::to_string(server.incarnation())).c_str());

    {
      std::lock_guard lock(host_server_mutex);
      host_server = &server;
    }
    // The stop-event waiter only requests the stop; it never drains, joins or
    // touches the backend. It ends when the Host exits (exit_complete).
    std::thread stop_watcher([&stop_event] {
      if (fusion::ipc::wait_for_stop_request(stop_event.handle(), exit_complete))
        request_host_stop("stop_event");
    });
    if (console_mode)
      SetConsoleCtrlHandler(console_control, TRUE);
    std::fprintf(stderr, "[FusionHost] entering accept loop\n");
    fusion::host::diag::event("accept_loop",
                              ("pipe=" + fusion::host::diag::utf8(pipe_name)).c_str());
    server.run();
    // Ordered shutdown, entirely on this thread: end the accept loop and join every
    // connection thread first, then destroy the backend — which runs its orderly
    // shutdown and therefore Fcitx's own save on instance exit. The console handler,
    // the session-end window and the stop event can only have requested the stop, so
    // none of them can race the drain or the save.
    fusion::host::diag::event("ordered_shutdown_begin", "accept_loop=ended");
    server.stop();
    fusion::host::diag::event("backend_shutdown_begin", "fcitx_save=on_exit");
    const ULONGLONG backend_shutdown_started = GetTickCount64();
    server.shutdown_backend();
    fusion::host::diag::event(
        "backend_shutdown_end",
        ("elapsed_ms=" + std::to_string(GetTickCount64() - backend_shutdown_started)).c_str());
    {
      // No handler can be inside the server after this point, so leaving the scope
      // (and destroying it) is safe.
      std::lock_guard lock(host_server_mutex);
      host_server = nullptr;
    }
    // Release every bounded wait first, then end the helper threads.
    SetEvent(exit_complete);
    if (window_thread.joinable()) {
      PostMessageW(session_end_window, WM_CLOSE, 0, 0);
      window_thread.join();
    }
    stop_watcher.join();
  }

  if (console_mode)
    SetConsoleCtrlHandler(console_control, FALSE);
  // No window thread can be inside the handler after the join above.
  session_end_handler.reset();
  CloseHandle(exit_complete);
  exit_complete = nullptr;
  std::fprintf(stderr, "[FusionHost] shutting down\n");
  fusion::host::diag::event("shutting_down", "");
  return 0;
}

// R30: the opt-in is read exactly once, before anything else runs, and every exit
// path (including the early refusals) records one exit line.
int run(const std::vector<std::wstring> &arguments) {
  fusion::host::diag::initialize();
  const int code = run_host(arguments);
  fusion::host::diag::event("exit", ("code=" + std::to_string(code)).c_str());
  return code;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  int argument_count = 0;
  LPWSTR *argument_list = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  std::vector<std::wstring> arguments;
  if (argument_list != nullptr) {
    arguments.reserve(static_cast<size_t>(argument_count));
    for (int i = 0; i < argument_count; ++i)
      arguments.emplace_back(argument_list[i]);
    LocalFree(argument_list);
  }
  return run(arguments);
}
