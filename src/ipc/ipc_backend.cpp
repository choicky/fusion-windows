#include "ipc/ipc_backend.hpp"

namespace fusion::ipc {

IpcBackend::IpcBackend(std::wstring pipe_name) : client_(std::move(pipe_name)) {}

IpcBackend::~IpcBackend() { client_.disconnect(); }

bool IpcBackend::connect(std::chrono::milliseconds timeout) {
  if (!client_.connect(timeout))
    return false;
  std::vector<std::uint8_t> empty;
  auto response = rpc(MessageType::SessionIdRequest, empty, MessageType::SessionIdResponse);
  if (!response) {
    client_.disconnect();
    return false;
  }
  auto sid = deserialize_u64(response->payload.data(), response->payload.size());
  if (!sid) {
    client_.disconnect();
    return false;
  }
  cached_session_id_ = *sid;
  healthy_ = true;
  return true;
}

std::optional<WireFrame> IpcBackend::rpc(MessageType request_type,
                                          const std::vector<std::uint8_t> &payload,
                                          MessageType expected_response) {
  std::lock_guard lock(rpc_mutex_);
  if (!client_.connected())
    return std::nullopt;
  if (!client_.send_request(request_type, payload)) {
    healthy_ = false;
    return std::nullopt;
  }
  auto response = client_.recv_response();
  if (!response) {
    healthy_ = false;
    return std::nullopt;
  }
  if (response->type != expected_response) {
    healthy_ = false;
    return std::nullopt;
  }
  return response;
}

bool IpcBackend::bind(Binding binding) {
  std::vector<std::uint8_t> payload;
  serialize(payload, binding);
  auto response = rpc(MessageType::BindRequest, payload, MessageType::BindResponse);
  if (!response)
    return false;
  auto result = deserialize_bool(response->payload.data(), response->payload.size());
  return result.value_or(false);
}

bool IpcBackend::reset(Binding binding) {
  std::vector<std::uint8_t> payload;
  serialize(payload, binding);
  auto response = rpc(MessageType::ResetRequest, payload, MessageType::ResetResponse);
  if (!response)
    return false;
  auto result = deserialize_bool(response->payload.data(), response->payload.size());
  return result.value_or(false);
}

void IpcBackend::unbind(Binding binding) {
  std::vector<std::uint8_t> payload;
  serialize(payload, binding);
  (void)rpc(MessageType::UnbindRequest, payload, MessageType::UnbindResponse);
}

bool IpcBackend::healthy() const {
  if (!healthy_)
    return false;
  std::lock_guard lock(rpc_mutex_);
  if (!client_.connected()) {
    healthy_ = false;
    return false;
  }
  std::vector<std::uint8_t> empty;
  if (!client_.send_request(MessageType::HealthyRequest, empty)) {
    healthy_ = false;
    return false;
  }
  auto response = client_.recv_response();
  if (!response || response->type != MessageType::HealthyResponse) {
    healthy_ = false;
    return false;
  }
  auto result = deserialize_bool(response->payload.data(), response->payload.size());
  return result.value_or(false);
}

std::uint64_t IpcBackend::session_id() const { return cached_session_id_; }

BackendBatch IpcBackend::send(const BackendStimulus &stimulus) {
  std::vector<std::uint8_t> payload;
  serialize(payload, stimulus);
  auto response = rpc(MessageType::SendRequest, payload, MessageType::SendResponse);
  if (!response) {
    BackendBatch batch;
    batch.disposition = BackendBatch::Disposition::Reject;
    return batch;
  }
  auto batch = deserialize_batch(response->payload.data(), response->payload.size());
  if (!batch) {
    healthy_ = false;
    BackendBatch rejected;
    rejected.disposition = BackendBatch::Disposition::Reject;
    return rejected;
  }
  return *batch;
}

std::unique_ptr<Backend> make_ipc_backend(std::wstring pipe_name) {
  auto backend = std::make_unique<IpcBackend>(std::move(pipe_name));
  if (!backend->connect())
    return nullptr;
  return backend;
}

std::unique_ptr<Backend> make_ipc_backend(std::chrono::milliseconds timeout) {
  auto backend = std::make_unique<IpcBackend>();
  if (!backend->connect(timeout))
    return nullptr;
  return backend;
}

std::unique_ptr<Backend> make_ipc_backend(std::wstring pipe_name,
                                           std::chrono::milliseconds timeout) {
  auto backend = std::make_unique<IpcBackend>(std::move(pipe_name));
  if (!backend->connect(timeout))
    return nullptr;
  return backend;
}

} // namespace fusion::ipc
