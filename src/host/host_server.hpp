#pragma once

// Host server: wraps a Backend (typically FcitxBackend) and dispatches
// IPC requests from connected TSF DLL clients.
//
// Multi-client: the accept loop creates a new PipeServer per connection
// and spawns a per-connection thread. All backend calls are serialized
// through a shared mutex. Each connection remaps client context IDs to
// Host-unique context IDs and unbinds all contexts on disconnect.
//
// Incarnation (Stage 4B): one HostServer instance represents exactly one Host
// process incarnation and answers every Hello with that identity; every response
// carries it as an envelope so a client can never accept a result from a
// different incarnation. The value is a PoC engineering identifier (process id,
// monotonic clock and a process-local sequence), not a frozen production
// identifier; 0 is reserved for "no incarnation".
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

// Stage 6 WP3 A5: the Host singleton claim, acquired before any Backend exists so
// that a Host which loses the claim never constructs Fcitx and never loads or
// saves per-user data.
//
// Mechanism and scope:
//   * Object type: a manual-reset, initially non-signalled **event**. Presence is
//     the claim; the object is never waited on.
//   * Name scope: `Local\FusionHost-claim-<pipe suffix>`. `Local\` is the
//     per-session object namespace, and the pipe name it is derived from already
//     carries the user SID and session id (compute_pipe_name), so the claim is per
//     user SID x session — the same scope as the pipe it protects.
//   * Security: the buffer's SECURITY_ATTRIBUTES is the pipe's own user-only DACL
//     (make_pipe_security), so another principal cannot pre-create the name and
//     hold the Host down.
//   * Fail closed: `acquire` returns false when the descriptor cannot be built,
//     when creation fails, or when the name already exists
//     (ERROR_ALREADY_EXISTS) — a live second Host is always refused.
//   * Abandoned owner: none by construction. A named event has no owner, and the
//     object disappears when its last handle closes, so a Host that crashed or was
//     killed releases the claim immediately; there is no WAIT_ABANDONED case to
//     interpret, and a dead Host can never block the next one.
//
// The pipe's own FILE_FLAG_FIRST_PIPE_INSTANCE stays the final authority for the
// name itself; this claim is what lets the loser exit before the engine exists.
// The pipe instance is deliberately still created by the accept loop, after the
// backend is ready: claiming the instance this early would make it connectable
// before the Host serves it, and a client would then lose its first reconnect key
// to a handshake timeout instead of a fast refused connect.
class HostClaim {
public:
  HostClaim() = default;
  ~HostClaim() { release(); }
  HostClaim(const HostClaim &) = delete;
  HostClaim &operator=(const HostClaim &) = delete;
  HostClaim(HostClaim &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  HostClaim &operator=(HostClaim &&other) noexcept;

  // True when this process is the first Host for this pipe name.
  bool acquire(const std::wstring &pipe_name);
  void release();
  bool held() const { return handle_ != nullptr; }
  // The kernel object name used by the last acquire() (empty before the first
  // call). Reported by the Host so the claim's scope is auditable in its log.
  const std::wstring &name() const { return name_; }

private:
  void *handle_ = nullptr;
  std::wstring name_;
};

class HostServer {
public:
  // `incarnation` 0 means "generate one for this instance" (tests may pin it).
  HostServer(std::unique_ptr<Backend> backend, std::wstring pipe_name = compute_pipe_name(),
             std::uint64_t incarnation = 0);
  ~HostServer();

  HostServer(const HostServer &) = delete;
  HostServer &operator=(const HostServer &) = delete;

  bool start();
  void run();
  // Stops accepting, disconnects every client and joins every connection
  // thread before returning. Idempotent; never leaves a running thread.
  void stop();
  // Destroys the backend now, after stop(), so the backend's orderly shutdown
  // (and Fcitx's own save on instance exit) runs while the caller can still wait
  // for it — used by the console close/logoff path. Idempotent.
  void shutdown_backend();

  // Development/test observable: number of tracked connections (live plus any
  // completed one not yet reaped). Not product semantics.
  std::size_t tracked_connection_count() const;

  // Identity of this Host incarnation, as sent in every response envelope.
  std::uint64_t incarnation() const { return incarnation_; }

private:
  struct Connection {
    Connection(std::unique_ptr<PipeServer> pipe_server, Backend &backend_ref,
               std::mutex &backend_mutex_ref, std::atomic<std::uint64_t> &next_context,
               std::uint64_t host_incarnation)
        : pipe(std::move(pipe_server)), backend(backend_ref), backend_mutex(backend_mutex_ref),
          next_host_context(next_context), incarnation(host_incarnation) {}

    std::unique_ptr<PipeServer> pipe;
    Backend &backend;
    std::mutex &backend_mutex;
    std::atomic<std::uint64_t> &next_host_context;
    std::uint64_t incarnation;
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
  const std::uint64_t incarnation_;
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
