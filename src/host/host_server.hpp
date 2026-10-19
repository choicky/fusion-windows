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
#include <functional>
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

// Stage 6 WP4: the named stop event, the second way (besides the console control
// handler and the session-end window) to ask a running Host for an *orderly*
// stop. An elevated install/upgrade/uninstall signals it instead of terminating
// the process, so Fcitx's own save on instance exit still runs.
//
// Mechanism and scope:
//   * Object type: a manual-reset, initially non-signalled event. The Host
//     creates it and keeps the handle open for its whole lifetime, so the object
//     disappears when the Host exits (a dead Host can never leave a stale event
//     that a later install would signal).
//   * Name scope: `Local\FusionHost-stop-<user SID>-<session id>` — exactly the
//     principal and session the pipe and the claim are scoped to.
//   * Security: the same user-only DACL as the pipe and HostClaim, so another
//     principal can neither create nor signal it.
//   * The event carries no request payload and no acknowledgement: signalling it
//     means "perform the normal ordered shutdown". The HostClaim stays the
//     authority for "only one Host per user x session".
class HostStopEvent {
public:
  HostStopEvent() = default;
  ~HostStopEvent() { release(); }
  HostStopEvent(const HostStopEvent &) = delete;
  HostStopEvent &operator=(const HostStopEvent &) = delete;

  // Creates the event for this user x session. False when the descriptor cannot
  // be built, creation fails, or the name already exists (another Host for this
  // principal is alive; the claim has already refused this one).
  //
  // `scope` exists for tests, which need a name of their own on a machine that may
  // already run the product Host; the product Host always uses the default (this
  // user x session), exactly like the pipe name it is paired with.
  bool create(const std::wstring &scope = std::wstring());
  void release();
  bool valid() const { return handle_ != nullptr; }
  void *handle() const { return handle_; }
  // The kernel object name used by the last create() (empty before the first
  // call), so the Host's log states the scope an installer must target.
  const std::wstring &name() const { return name_; }

private:
  void *handle_ = nullptr;
  std::wstring name_;
};

// Waits until either the stop event is signalled (true: the caller requests the
// stop) or the Host has finished its orderly exit (false). It never drains, joins
// or touches the backend, so any thread may run it; main() remains the single
// thread that performs the ordered shutdown.
bool wait_for_stop_request(void *stop_event, void *exit_complete);

// Stage 6 WP4: the session-end decision of the windowless Host's hidden window,
// kept out of the window procedure so it can be driven without a real session end.
//
// A logoff or shutdown can still be cancelled *after* WM_QUERYENDSESSION (an
// application asks to save and the user cancels). That message therefore answers
// TRUE and stops nothing — a Host that stopped there would stay gone until the next
// logon even though the session continues. Only WM_ENDSESSION with wParam TRUE means
// the session is really ending: then the handler requests the stop and waits,
// bounded, for main() to finish the ordered shutdown (Fcitx's save included).
// WM_ENDSESSION with wParam FALSE (the session end was cancelled) does nothing.
//
// This handler does NOT make the Host survive a *cancelled* session end. Windows
// delivers WM_ENDSESSION(TRUE) to applications one after another during a logoff, so an
// application processed before the blocking one has already ended when the user's Cancel
// takes effect (measured 2026-10-18: the Host saved at 19:03:13, the cancel was logged at
// 19:03:20, and the process was gone). Surviving that is the scheduled task's event
// trigger (User32 1077 -> start the Host again), not this handler.
class SessionEndHandler {
public:
  // R30: the path is passed so the diagnostic log can name which way the stop came
  // (session end vs an external window close).
  using RequestStop = std::function<void(const char *path)>;

  SessionEndHandler(RequestStop request_stop, void *exit_complete, unsigned long timeout_ms)
      : request_stop_(std::move(request_stop)), exit_complete_(exit_complete),
        timeout_ms_(timeout_ms) {}

  // True for the two session-end messages this handler owns.
  static bool handles(unsigned int message);
  // The value the window procedure must return for a message it owns.
  std::intptr_t handle(unsigned int message, std::uintptr_t wparam);

  // The window procedure's WM_CLOSE decision. Before main() has finished, an external
  // close (taskkill without /f, a window closer) must not remove the Host's only
  // logoff/shutdown path: it requests the same orderly stop and the window is kept.
  // After exit_complete is signalled the close is main()'s own and the window is
  // destroyed. Returns true when the window must be kept.
  bool close_keeps_window();

private:
  RequestStop request_stop_;
  void *exit_complete_;
  unsigned long timeout_ms_;
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
  // Asks the server to stop: sets the stop flags and wakes the accept loop. It does
  // not drain, join or touch the backend, so it is safe from any thread (the console
  // control handler) and idempotent. The server's own thread performs the ordered
  // shutdown afterwards: stop() → shutdown_backend().
  void request_stop();
  // Stops accepting, disconnects every client and joins every connection
  // thread before returning. Idempotent; never leaves a running thread. Only the
  // thread that runs the server should call it.
  void stop();
  // Destroys the backend now, after stop(), so the backend's orderly shutdown
  // (and Fcitx's own save on instance exit) runs while the caller can still wait
  // for it. Idempotent; only the thread that runs the server should call it.
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
               std::uint64_t host_incarnation, std::uint64_t connection_id)
        : pipe(std::move(pipe_server)), backend(backend_ref), backend_mutex(backend_mutex_ref),
          next_host_context(next_context), incarnation(host_incarnation), id(connection_id) {}

    std::unique_ptr<PipeServer> pipe;
    Backend &backend;
    std::mutex &backend_mutex;
    std::atomic<std::uint64_t> &next_host_context;
    std::uint64_t incarnation;
    // R30: this connection's identity in the diagnostic log (no payload, no client data).
    const std::uint64_t id;
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
  std::atomic<std::uint64_t> next_connection_id_{1};
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
