#pragma once

// IPC Backend: implements fusion::Backend over a Named Pipe connection
// to an out-of-process FusionHost. Linked into FusionTsf.dll.
//
// Stage 4B link lifecycle (D072 §13). The transport is owned here and is never
// reused across an establishment:
//
//   Disconnected --ensure_link--> Connected --Hello--> Established(incarnation N)
//        ^                                                      |
//        +--------------- any failure / disconnect -------------+
//
// ensure_link is idempotent and bounded, and performs no I/O once the link is
// established. It never replays a request: a request is only written by send()
// on the connection whose Hello established the current incarnation, and a
// transport failure keeps the Stage 4A classification (provably-not-written ->
// Forward, uncertain write or lost response -> Reject).
//
// healthy() is local state only — no IPC probe.

#include "fusion/backend.hpp"
#include "ipc/pipe_client.hpp"
#include "win32/srw_mutex.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace fusion::ipc {

class IpcBackend final : public Backend {
public:
  explicit IpcBackend(std::wstring pipe_name = compute_pipe_name());
  ~IpcBackend() override;

  bool bind(Binding binding) override;
  bool reset(Binding binding) override;
  void unbind(Binding binding) override;
  bool healthy() const override;
  std::uint64_t session_id() const override;
  BackendBatch send(const BackendStimulus &stimulus) override;

  // Link lifecycle (see the header comment).
  LinkStatus link_status() const override;
  // Diagnostics only: the stage and Win32 code of the last failed establishment.
  ConnectFailure last_connect_failure() const override { return last_connect_failure_; }
  // Bounded establishment: transport connect (up to `timeout`) plus one Hello
  // round trip (up to `timeout`). Never unbounded, and no I/O while established.
  bool ensure_link(std::chrono::milliseconds timeout) override;

  // Stage 4A entry point, kept as connect-or-report: establish the link within
  // the bound and report whether the handshake completed. Prefer ensure_link()
  // when the caller owns a reconnect lifecycle.
  bool connect(std::chrono::milliseconds timeout = std::chrono::milliseconds(200));

  // Identity of the last established link, 0 before any establishment. It is
  // deliberately retained across a disconnect so the dead incarnation stays
  // identifiable; link_status().state reports usability.
  std::uint64_t incarnation() const;

private:
  bool handshake(std::chrono::milliseconds timeout, ConnectFailure *failure);

  PipeClient client_;
  ConnectFailure last_connect_failure_;
  // Not std::mutex: this class is compiled into the TIP DLL, which must not hold
  // CRT-owned synchronization state (see win32/srw_mutex.hpp).
  mutable win32::SrwMutex rpc_mutex_;
  std::uint64_t cached_session_id_ = 0;
  // Link state is read without rpc_mutex_ (healthy() must not block a send).
  std::atomic<LinkState> link_state_{LinkState::Disconnected};
  std::atomic<std::uint64_t> incarnation_{0};
  std::atomic<std::uint64_t> connection_generation_{0};

  // Hello handshake on the current connection. On success it publishes
  // incarnation_/cached_session_id_ and bumps connection_generation_.
  // The caller holds rpc_mutex_.
  bool handshake(std::chrono::milliseconds timeout);
  // Drops the transport and every link identity derived from it.
  void drop_link();
  // Validates a response envelope against the established incarnation.
  bool envelope_matches(const WireFrame &response) const;

  // Definitely-not-written transport failure: no byte of the request reached the
  // Host, so the physical stimulus stays caller-owned and is reported as a
  // genuine Forward. Identity fields are preserved so the caller's accounting
  // matches the stimulus it sent. Used by both definitely-not-written branches
  // (not connected, and a write that provably accepted no byte) so they cannot
  // diverge; an uncertain write must NOT use this path.
  BackendBatch forward_batch(const BackendStimulus &stimulus) const;
  // The stimulus is no longer provably caller-owned (uncertain write, lost
  // response, protocol violation): explicit non-PASS terminal with the identity
  // fields preserved. Never a replay.
  BackendBatch reject_batch(const BackendStimulus &stimulus) const;

  // Send a request and receive the expected response.
  // Returns nullopt on any failure (write fail, read fail, wrong message type,
  // envelope mismatch). The caller must track whether the write succeeded for
  // transport failure classification.
  std::optional<WireFrame> rpc(MessageType request_type, const std::vector<std::uint8_t> &payload,
                               MessageType expected_response);
};

std::unique_ptr<Backend> make_ipc_backend();
std::unique_ptr<Backend> make_ipc_backend(std::chrono::milliseconds timeout);
std::unique_ptr<Backend> make_ipc_backend(std::wstring pipe_name,
                                          std::chrono::milliseconds timeout);

} // namespace fusion::ipc
