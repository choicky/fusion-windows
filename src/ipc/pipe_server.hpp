#pragma once

// Windows Named Pipe server transport for Stage 4A IPC.
// Accepts one client connection at a time (single-client v0.x).

#include "fusion/ipc_protocol.hpp"

#include <chrono>
#include <functional>
#include <string>

namespace fusion::ipc {

class PipeServer {
public:
  explicit PipeServer(std::wstring pipe_name = default_pipe_name);
  ~PipeServer();

  PipeServer(const PipeServer &) = delete;
  PipeServer &operator=(const PipeServer &) = delete;

  bool create();
  bool wait_for_client(std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));
  void disconnect_client();
  void shutdown();
  bool client_connected() const;

  bool send_frame(const WireFrame &frame);
  std::optional<WireFrame> recv_frame(std::chrono::milliseconds timeout =
                                          std::chrono::milliseconds(5000));

private:
  std::wstring pipe_name_;
  void *handle_ = reinterpret_cast<void *>(-1);
  void *pipe_handle_ = reinterpret_cast<void *>(-1);
  bool client_connected_ = false;
  bool shutting_down_ = false;

  bool write_all(const void *data, std::size_t size);
  bool read_all(void *data, std::size_t size, std::chrono::milliseconds timeout);
  bool wait_for_data(std::chrono::milliseconds timeout);
};

} // namespace fusion::ipc
