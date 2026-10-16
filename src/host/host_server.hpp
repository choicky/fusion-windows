#pragma once

// Host server: wraps a Backend (typically FcitxBackend) and dispatches
// IPC requests from connected TSF DLL clients.
//
// Multi-client: the accept loop creates a new PipeServer per connection
// and spawns a per-connection thread. All backend calls are serialized
// through a shared mutex. Each connection remaps client context IDs to
// Host-unique context IDs and unbinds all contexts on disconnect.

#include "fusion/backend.hpp"
#include "ipc/pipe_security.hpp"
#include "ipc/pipe_server.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace fusion::ipc {

class HostServer {
public:
  HostServer(std::unique_ptr<Backend> backend, std::wstring pipe_name = compute_pipe_name());
  ~HostServer();

  HostServer(const HostServer &) = delete;
  HostServer &operator=(const HostServer &) = delete;

  bool start();
  void run();
  void stop();

private:
  struct Connection {
    std::unique_ptr<PipeServer> pipe;
    Backend &backend;
    std::mutex &backend_mutex;
    std::atomic<std::uint64_t> &next_host_context;
    std::map<std::uint64_t, Binding> context_map; // client_context → host binding

    void run();
    void handle_hello();
    void handle_bind(const std::vector<std::uint8_t> &payload);
    void handle_reset(const std::vector<std::uint8_t> &payload);
    void handle_unbind(const std::vector<std::uint8_t> &payload);
    void handle_send(const std::vector<std::uint8_t> &payload);
    void send_response(MessageType type, const std::vector<std::uint8_t> &payload);
    void send_bool_response(MessageType type, bool value);
    void unbind_all();
    Binding remap_binding(const Binding &client_binding);
  };

  std::unique_ptr<Backend> backend_;
  std::wstring pipe_name_;
  std::atomic<bool> running_{false};
  std::mutex backend_mutex_;
  std::atomic<std::uint64_t> next_host_context_{1};
  bool first_instance_ = true;

  void accept_loop();
};

} // namespace fusion::ipc
