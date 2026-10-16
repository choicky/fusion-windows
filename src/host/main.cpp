// FusionHost.exe — Stage 4A out-of-process Fcitx Host.
//
// Creates a Named Pipe server, instantiates the real Fcitx backend,
// and dispatches IPC requests from connected TSF DLL clients.
// Default pipe name is computed from the user SID and session ID.

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

} // namespace

int main(int argc, char *argv[]) {
  auto pipe_name = parse_pipe_name(argc, argv);
  std::fprintf(stderr, "[FusionHost] starting with pipe: %ls\n", pipe_name.c_str());

  std::unique_ptr<fusion::Backend> backend;
  try {
    backend = fusion::make_fcitx_backend();
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

  fusion::ipc::HostServer server(std::move(backend), pipe_name);
  if (!server.start()) {
    std::fprintf(stderr, "[FusionHost] failed to initialize server\n");
    return 1;
  }
  // The incarnation distinguishes this Host process from any earlier or later
  // one for client-side fencing; it is an engineering identity, not a frozen ID.
  std::fprintf(stderr, "[FusionHost] incarnation=%llu\n",
               static_cast<unsigned long long>(server.incarnation()));

  std::fprintf(stderr, "[FusionHost] entering accept loop\n");
  server.run();
  std::fprintf(stderr, "[FusionHost] shutting down\n");
  return 0;
}
