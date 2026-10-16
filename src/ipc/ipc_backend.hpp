#pragma once

// IPC Backend: implements fusion::Backend over a Named Pipe connection
// to an out-of-process FusionHost. Linked into FusionTsf.dll.
//
// healthy() is local state only — no IPC probe.
// send() distinguishes pre-write failure (Forward, no ops) from post-write
// failure (Reject, mark unhealthy). Connect uses Hello protocol.

#include "fusion/backend.hpp"
#include "ipc/pipe_client.hpp"

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

  // Connect and perform Hello handshake. Returns false on connect failure,
  // version mismatch, or malformed Hello response.
  bool connect(std::chrono::milliseconds timeout = std::chrono::milliseconds(200));

private:
  PipeClient client_;
  mutable std::mutex rpc_mutex_;
  std::uint64_t cached_session_id_ = 0;
  std::atomic<bool> healthy_{false};

  // Definitely-not-written transport failure: no byte of the request reached the
  // Host, so the physical stimulus stays caller-owned and is reported as a
  // genuine Forward. Identity fields are preserved so the caller's accounting
  // matches the stimulus it sent. Used by both definitely-not-written branches
  // (not connected, and a write that provably accepted no byte) so they cannot
  // diverge; an uncertain write must NOT use this path.
  BackendBatch forward_batch(const BackendStimulus &stimulus) const;

  // Send a request and receive the expected response.
  // Returns nullopt on any failure (write fail, read fail, wrong message type).
  // The caller must track whether the write succeeded for transport failure classification.
  std::optional<WireFrame> rpc(MessageType request_type, const std::vector<std::uint8_t> &payload,
                               MessageType expected_response);
};

std::unique_ptr<Backend> make_ipc_backend();
std::unique_ptr<Backend> make_ipc_backend(std::chrono::milliseconds timeout);
std::unique_ptr<Backend> make_ipc_backend(std::wstring pipe_name,
                                          std::chrono::milliseconds timeout);

} // namespace fusion::ipc
