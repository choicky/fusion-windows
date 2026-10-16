#pragma once

// Host server: wraps a Backend (typically FcitxBackend) and dispatches
// IPC requests from connected TSF DLL clients.
//
// Multi-client: the accept loop creates a new PipeServer per connection
// and spawns a per-connection thread. All backend calls are serialized
// through a shared mutex. Each connection remaps client context IDs to
// Host-unique context IDs and unbinds all contexts on disconnect.
//
// Lifecycle: every connection is owned together with its thread. A finished
// connection marks itself completed and is reaped (joined and erased) by the
// accept path, so completed connections do not accumulate in a long-running
// Host; stop() still disconnects and joins every remaining live connection and
// returns without leaving a thread that could touch backend_. Running the Host
// normally (run() until the process is terminated) is unchanged.

#include "fusion/backend.hpp"
#include "ipc/pipe_security.hpp"
#include "ipc/pipe_server.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fusion::ipc {

class HostServer {
public:
  HostServer(std::unique_ptr<Backend> backend, std::wstring pipe_name = compute_pipe_name());
  ~HostServer();

  HostServer(const HostServer &) = delete;
  HostServer &operator=(const HostServer &) = delete;

  bool start();
  void run();
  // Stops accepting, disconnects every client and joins every connection
  // thread before returning. Idempotent; never leaves a running thread.
  void stop();

  // Development/test observable: number of tracked connections (live plus any
  // completed one not yet reaped). Not product semantics.
  std::size_t tracked_connection_count() const;

private:
  struct Connection {
    Connection(std::unique_ptr<PipeServer> pipe_server, Backend &backend_ref,
               std::mutex &backend_mutex_ref, std::atomic<std::uint64_t> &next_context)
        : pipe(std::move(pipe_server)), backend(backend_ref), backend_mutex(backend_mutex_ref),
          next_host_context(next_context) {}

    std::unique_ptr<PipeServer> pipe;
    Backend &backend;
    std::mutex &backend_mutex;
    std::atomic<std::uint64_t> &next_host_context;
    std::map<std::uint64_t, Binding> context_map; // client_context → host binding

    void run();
    // Cancels a blocking receive and drops the client so run() can leave its
    // loop. Safe to call from another thread (stop()).
    void disconnect();
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

  // One connection: its state, the thread that serves it, and the completion
  // flag the thread sets as its last action so the accept path can join and
  // erase it without racing a live connection. Held by pointer so the flag has a
  // stable address while the tracking vector grows.
  struct TrackedConnection {
    std::unique_ptr<Connection> connection;
    std::thread thread;
    std::atomic<bool> completed{false};
  };

  std::unique_ptr<Backend> backend_;
  std::wstring pipe_name_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::mutex backend_mutex_;
  std::atomic<std::uint64_t> next_host_context_{1};
  bool first_instance_ = true;

  mutable std::mutex connections_mutex_;
  std::vector<std::unique_ptr<TrackedConnection>> connections_;

  // Accept-loop exit handshake: stop() must not drain connections while the
  // accept loop could still spawn one.
  std::atomic<bool> accept_loop_active_{false};
  std::mutex accept_done_mutex_;
  std::condition_variable accept_done_cv_;

  void accept_loop();
  void wake_accept_loop();
  // Joins and erases every completed connection. Never joins a live connection,
  // and the connection thread never takes connections_mutex_.
  void reap_completed_connections();
  void drain_connections();
};

} // namespace fusion::ipc
