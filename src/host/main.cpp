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

#include "fusion/backend.hpp"
#include "fusion/ipc_protocol.hpp"
#include "host/host_server.hpp"
#include "ipc/pipe_security.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {

std::wstring parse_pipe_name(int argc, char *argv[]) {
  for (int i = 1; i < argc - 1; ++i) {
    if (std::strcmp(argv[i], "--pipe-name") == 0) {
      // -1 conversion: the returned length counts the terminating NUL, so the
      // buffer must have room for it and the conversion must be told that
      // capacity. A conversion failure (or an invalid UTF-8 argument) must not
      // produce a partially initialized pipe name.
      const int length =
          MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[i + 1], -1, nullptr, 0);
      if (length <= 0)
        return fusion::ipc::compute_pipe_name();
      std::wstring name(static_cast<size_t>(length), L'\0');
      const int converted =
          MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[i + 1], -1, name.data(), length);
      if (converted != length)
        return fusion::ipc::compute_pipe_name();
      name.resize(static_cast<size_t>(length - 1)); // drop the terminating NUL
      return name;
    }
  }
  return fusion::ipc::compute_pipe_name();
}

bool env_flag(const char *name) {
  const char *value = std::getenv(name);
  return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
}

// The Host instance the console handler may stop. Set for the lifetime of run().
std::atomic<fusion::ipc::HostServer *> host_server{nullptr};

// Signals that main() has finished the orderly exit (backend destroyed, Fcitx's own
// save completed). A console close/logoff/shutdown handler waits on it, bounded,
// instead of destroying the backend itself: main() is the single owner of backend
// destruction, so no second thread can race it or cut the save short.
HANDLE exit_complete = nullptr;

// Windows allows about 5 s between a console close event and forced termination.
constexpr DWORD console_exit_timeout_ms = 4000;

BOOL WINAPI console_control(DWORD type) {
  auto *server = host_server.load();
  if (server == nullptr)
    return FALSE;
  switch (type) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
    // main() keeps running after this handler, so only request the stop; main() then
    // runs the backend shutdown and Fcitx's own save on its own thread.
    server->stop();
    return TRUE;
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    // Windows terminates the process when this handler returns, so wait (bounded) for
    // main() to finish the orderly shutdown, including Fcitx's save. On timeout the
    // process is terminating anyway and Fcitx's forced-termination semantics apply.
    std::fprintf(stderr, "[FusionHost] console control %lu: waiting for the orderly exit\n",
                 static_cast<unsigned long>(type));
    server->stop();
    if (exit_complete != nullptr)
      WaitForSingleObject(exit_complete, console_exit_timeout_ms);
    return TRUE;
  default:
    return FALSE;
  }
}

} // namespace

int main(int argc, char *argv[]) {
  auto pipe_name = parse_pipe_name(argc, argv);
  std::fprintf(stderr, "[FusionHost] starting with pipe: %ls\n", pipe_name.c_str());

  // A5: claim the Host singleton before creating the backend, so a Host that is
  // not the singleton never constructs Fcitx, which would load the user profile
  // and save on exit. The pipe keeps its own first-instance authority.
  fusion::ipc::HostClaim claim;
  claim.acquire(pipe_name);
  if (!claim.held()) {
    std::fprintf(stderr, "[FusionHost] another Host already owns this pipe name; exiting without "
                         "touching user data\n");
    return 1;
  }
  std::fprintf(stderr, "[FusionHost] singleton claim: %ls\n", claim.name().c_str());

  // U1: the product Host persists through Fcitx's own per-user paths. The skip
  // switch exists for automated tests and fixtures that spawn this Host; it is
  // announced loudly so a stray environment variable cannot silently disable
  // persistence.
  const bool user_path = !env_flag("FUSION_FCITX_SKIP_USER_PATH");
  if (user_path) {
    std::fprintf(stderr, "[FusionHost] Fcitx user path: standard (per-user Fcitx paths)\n");
  } else {
    std::fprintf(stderr,
                 "[FusionHost] WARNING: FUSION_FCITX_SKIP_USER_PATH is set - Fcitx user data is "
                 "DISABLED (no persistence, no user dictionary)\n");
  }

  std::unique_ptr<fusion::Backend> backend;
  try {
    backend = fusion::make_fcitx_backend(fusion::FcitxBackendOptions{user_path});
  } catch (const std::exception &e) {
    std::fprintf(stderr, "[FusionHost] Fcitx backend creation failed: %s\n", e.what());
    return 1;
  }
  if (!backend) {
    std::fprintf(stderr, "[FusionHost] Fcitx backend creation returned null\n");
    return 1;
  }
  std::fprintf(stderr, "[FusionHost] Fcitx backend ready, session=%llu\n",
               static_cast<unsigned long long>(backend->session_id()));

  exit_complete = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  // The server lives in its own scope: main() is the single owner of backend
  // destruction, and the exit signal is set only after the server is gone.
  {
    fusion::ipc::HostServer server(std::move(backend), pipe_name, 0);
    if (!server.start()) {
      std::fprintf(stderr, "[FusionHost] failed to initialize server\n");
      return 1;
    }
    // The incarnation distinguishes this Host process from any earlier or later
    // one for client-side fencing; it is an engineering identity, not a frozen ID.
    std::fprintf(stderr, "[FusionHost] incarnation=%llu\n",
                 static_cast<unsigned long long>(server.incarnation()));

    host_server.store(&server);
    SetConsoleCtrlHandler(console_control, TRUE);
    std::fprintf(stderr, "[FusionHost] entering accept loop\n");
    server.run();
    // Runs the backend's orderly shutdown — and therefore Fcitx's own save on
    // instance exit — on this thread. No other thread may destroy the backend, so a
    // console close handler cannot race the save or cut it short; it only waits.
    server.shutdown_backend();
    host_server.store(nullptr);
  }

  SetConsoleCtrlHandler(console_control, FALSE);
  if (exit_complete != nullptr) {
    SetEvent(exit_complete);
    CloseHandle(exit_complete);
    exit_complete = nullptr;
  }
  std::fprintf(stderr, "[FusionHost] shutting down\n");
  return 0;
}
