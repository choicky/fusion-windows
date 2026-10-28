#pragma once

// Windows Named Pipe server transport for Stage 4A IPC.
// Per-connection: each PipeServer instance owns one pipe handle for one client.
// Uses FILE_FLAG_OVERLAPPED for timeout support and PIPE_UNLIMITED_INSTANCES
// for multi-client support.

#include "fusion/ipc_protocol.hpp"
#include "ipc/pipe_security.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace fusion::ipc {

class PipeServer {
public:
  // `search_package_access` grants the taskbar search package's AppContainer SID the
  // client rights on this pipe (D089). The Host passes true for its data pipe; the
  // claim and stop events never go through here and stay user-only.
  explicit PipeServer(std::wstring pipe_name, bool first_instance = false,
                      bool search_package_access = false);
  ~PipeServer();

  PipeServer(const PipeServer &) = delete;
  PipeServer &operator=(const PipeServer &) = delete;

  bool create();
  // timeout=0 means block indefinitely.
  bool wait_for_client(std::chrono::milliseconds timeout = std::chrono::milliseconds(0));
  void disconnect_client();
  bool client_connected() const;

  bool send_frame(const WireFrame &frame);
  // timeout=0 means block indefinitely.
  std::optional<WireFrame>
  recv_frame(std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

  // R30: the Win32 error of the last failed operation on this instance (0 when the
  // last one succeeded or did not fail). Technical only — the Host records it in its
  // diagnostic log; it never changes behaviour.
  std::uint32_t last_error() const { return last_error_.load(); }

private:
  std::wstring pipe_name_;
  bool first_instance_;
  bool search_package_access_;
  void *handle_ = reinterpret_cast<void *>(-1); // INVALID_HANDLE_VALUE
  // Owned by the serving thread, but also cleared from another thread when
  // HostServer::stop() disconnects the client.
  std::atomic<bool> client_connected_{false};
  std::atomic<std::uint32_t> last_error_{0};
  SECURITY_ATTRIBUTES security_attrs_{};

  bool write_all(const void *data, std::size_t size);
  bool read_all_overlapped(void *data, std::size_t size, std::chrono::milliseconds timeout);
};

} // namespace fusion::ipc
