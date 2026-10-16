#pragma once

// IPC Backend: implements fusion::Backend over a Named Pipe connection
// to an out-of-process FusionHost. Linked into FusionTsf.dll.

#include "fusion/backend.hpp"
#include "ipc/pipe_client.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace fusion::ipc {

class IpcBackend final : public Backend {
public:
  explicit IpcBackend(std::wstring pipe_name = default_pipe_name);
  ~IpcBackend() override;

  bool bind(Binding binding) override;
  bool reset(Binding binding) override;
  void unbind(Binding binding) override;
  bool healthy() const override;
  std::uint64_t session_id() const override;
  BackendBatch send(const BackendStimulus &stimulus) override;

  bool connect(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

private:
  PipeClient client_;
  mutable std::mutex rpc_mutex_;
  std::uint64_t cached_session_id_ = 0;
  std::atomic<bool> healthy_{false};

  std::optional<WireFrame> rpc(MessageType request_type,
                               const std::vector<std::uint8_t> &payload,
                               MessageType expected_response);
};

std::unique_ptr<Backend> make_ipc_backend(std::wstring pipe_name = default_pipe_name);
std::unique_ptr<Backend> make_ipc_backend(std::chrono::milliseconds timeout);
std::unique_ptr<Backend> make_ipc_backend(std::wstring pipe_name, std::chrono::milliseconds timeout);

} // namespace fusion::ipc
