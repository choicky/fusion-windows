#include "ipc/ipc_backend.hpp"

namespace fusion::ipc {

IpcBackend::IpcBackend(std::wstring pipe_name) : client_(std::move(pipe_name)) {}

IpcBackend::~IpcBackend() { client_.disconnect(); }

bool IpcBackend::connect(std::chrono::milliseconds timeout) {
  if (!client_.connect(timeout))
    return false;

  // Hello handshake: send protocol_version, expect version + session_id.
  std::vector<std::uint8_t> hello_payload;
  detail::write_u32(hello_payload, protocol_version);

  std::lock_guard lock(rpc_mutex_);
  if (!client_.send_request(MessageType::HelloRequest, hello_payload)) {
    client_.disconnect();
    return false;
  }
  auto response = client_.recv_response(kRequestTimeout);
  if (!response || response->type != MessageType::HelloResponse) {
    client_.disconnect();
    return false;
  }

  detail::Reader r(response->payload.data(), response->payload.size());
  auto version = r.read_u32();
  auto sid = r.read_u64();
  if (!version || *version == 0 || !sid || !r.at_end()) {
    client_.disconnect();
    return false;
  }
  if (*version != protocol_version) {
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
  if (!client_.send_request(request_type, payload))
    return std::nullopt;
  auto response = client_.recv_response(kRequestTimeout);
  if (!response || response->type != expected_response)
    return std::nullopt;
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
  // Local state only — no IPC probe.
  return healthy_.load() && client_.connected();
}

std::uint64_t IpcBackend::session_id() const { return cached_session_id_; }

BackendBatch IpcBackend::forward_batch(const BackendStimulus &stimulus) const {
  BackendBatch batch;
  batch.disposition = BackendBatch::Disposition::Forward;
  batch.backend_session_id = cached_session_id_;
  batch.binding = stimulus.binding;
  batch.stimulus_id = stimulus.stimulus_id;
  return batch;
}

BackendBatch IpcBackend::send(const BackendStimulus &stimulus) {
  std::vector<std::uint8_t> payload;
  serialize(payload, stimulus);

  std::lock_guard lock(rpc_mutex_);

  // Pre-write check: if not connected, the request cannot be written at all.
  if (!client_.connected())
    return forward_batch(stimulus);

  // Attempt to write the request.
  if (!client_.send_request(MessageType::SendRequest, payload)) {
    // Pre-write failure: transport never saw the request → Forward.
    healthy_ = false;
    return forward_batch(stimulus);
  }

  // Post-write: the request is on the wire. Any failure now is a Reject.
  auto response = client_.recv_response(kRequestTimeout);
  if (!response) {
    healthy_ = false;
    BackendBatch batch;
    batch.disposition = BackendBatch::Disposition::Reject;
    return batch;
  }
  if (response->type != MessageType::SendResponse) {
    healthy_ = false;
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

std::unique_ptr<Backend> make_ipc_backend() {
  auto backend = std::make_unique<IpcBackend>();
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
