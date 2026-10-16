#pragma once

// Windows Named Pipe server transport for Stage 4A IPC.
// Per-connection: each PipeServer instance owns one pipe handle for one client.
// Uses FILE_FLAG_OVERLAPPED for timeout support and PIPE_UNLIMITED_INSTANCES
// for multi-client support.

#include "fusion/ipc_protocol.hpp"
#include "ipc/pipe_security.hpp"

#include <chrono>
#include <string>

namespace fusion::ipc {

class PipeServer {
public:
  explicit PipeServer(std::wstring pipe_name, bool first_instance = false);
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
  std::optional<WireFrame> recv_frame(std::chrono::milliseconds timeout =
                                          std::chrono::milliseconds(0));

private:
  std::wstring pipe_name_;
  bool first_instance_;
  void *handle_ = reinterpret_cast<void *>(-1); // INVALID_HANDLE_VALUE
  bool client_connected_ = false;
  SECURITY_ATTRIBUTES security_attrs_{};

  bool write_all(const void *data, std::size_t size);
  bool read_all_overlapped(void *data, std::size_t size, std::chrono::milliseconds timeout);
};

} // namespace fusion::ipc
