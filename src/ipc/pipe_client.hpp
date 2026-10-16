#pragma once

// Windows Named Pipe client transport for Stage 4A IPC.
// Thread-safe: internal mutex serializes all operations.
// On any error or timeout, marks disconnected; all subsequent operations fail.

#include "fusion/ipc_protocol.hpp"

#include <chrono>
#include <mutex>
#include <string>

namespace fusion::ipc {

class PipeClient {
public:
  explicit PipeClient(std::wstring pipe_name = default_pipe_name);
  ~PipeClient();

  PipeClient(const PipeClient &) = delete;
  PipeClient &operator=(const PipeClient &) = delete;

  bool connect(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
  void disconnect();
  bool connected() const;

  bool send_frame(const WireFrame &frame);
  std::optional<WireFrame> recv_frame(std::chrono::milliseconds timeout =
                                          std::chrono::milliseconds(5000));

  bool send_request(MessageType type, const std::vector<std::uint8_t> &payload);
  std::optional<WireFrame> recv_response(std::chrono::milliseconds timeout =
                                             std::chrono::milliseconds(5000));

private:
  std::wstring pipe_name_;
  void *handle_ = reinterpret_cast<void *>(-1); // INVALID_HANDLE_VALUE
  mutable std::mutex mutex_;
  bool connected_ = false;

  bool write_all(const void *data, std::size_t size);
  bool read_all(void *data, std::size_t size, std::chrono::milliseconds timeout);
  bool wait_for_data(std::chrono::milliseconds timeout);
};

} // namespace fusion::ipc
