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
      int len = std::MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, nullptr, 0);
      if (len > 0) {
        std::wstring name(static_cast<size_t>(len - 1), L'\0');
        std::MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, name.data(), len);
        return name;
      }
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

  std::fprintf(stderr, "[FusionHost] entering accept loop\n");
  server.run();
  std::fprintf(stderr, "[FusionHost] shutting down\n");
  return 0;
}
