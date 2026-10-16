#pragma once

// Host server: wraps a Backend (typically FcitxBackend) and dispatches
// IPC requests from connected TSF DLL clients.

#include "fusion/backend.hpp"
#include "ipc/pipe_server.hpp"

#include <memory>
#include <string>

namespace fusion::ipc {

class HostServer {
public:
  HostServer(std::unique_ptr<Backend> backend, std::wstring pipe_name = default_pipe_name);
  ~HostServer();

  HostServer(const HostServer &) = delete;
  HostServer &operator=(const HostServer &) = delete;

  bool start();
  void run();
  void stop();

private:
  std::unique_ptr<Backend> backend_;
  PipeServer server_;
  bool running_ = false;

  void handle_client();
  void dispatch(const WireFrame &request);

  void handle_bind(const std::vector<std::uint8_t> &payload);
  void handle_reset(const std::vector<std::uint8_t> &payload);
  void handle_unbind(const std::vector<std::uint8_t> &payload);
  void handle_healthy();
  void handle_session_id();
  void handle_send(const std::vector<std::uint8_t> &payload);

  void send_response(MessageType type, const std::vector<std::uint8_t> &payload);
  void send_bool_response(MessageType type, bool value);
};

} // namespace fusion::ipc
