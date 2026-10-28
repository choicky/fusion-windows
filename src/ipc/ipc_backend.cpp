#include "ipc/ipc_backend.hpp"

namespace fusion::ipc {

IpcBackend::IpcBackend(std::wstring pipe_name) : client_(std::move(pipe_name)) {}

IpcBackend::~IpcBackend() { client_.disconnect(); }

LinkStatus IpcBackend::link_status() const {
  return LinkStatus{link_state_.load(), incarnation_.load(), connection_generation_.load()};
}

std::uint64_t IpcBackend::incarnation() const { return incarnation_.load(); }

void IpcBackend::drop_link() {
  client_.disconnect();
  link_state_ = LinkState::Disconnected;
  // The incarnation/generation of the dropped connection are deliberately kept:
  // they are the identity that later work must not be accepted against, and the
  // generation only ever advances when a new connection is established.
}

bool IpcBackend::handshake(std::chrono::milliseconds timeout, ConnectFailure *failure) {
  // Hello handshake: send protocol_version, expect envelope(incarnation) +
  // version + session_id.
  //
  // A transport failure and a protocol refusal are different facts: the first
  // says the bytes never moved, the second says the Host answered and the answer
  // was not acceptable. `failure` keeps them apart, and for a transport failure
  // carries the Win32 code the read/write path captured at the call that failed —
  // not GetLastError() here, which the transport's own cleanup has already
  // overwritten by the time it returns.
  const auto transport_failed = [failure](ConnectStage stage, unsigned long error) {
    if (failure) {
      failure->stage = stage;
      failure->win32_error = error;
    }
    return false;
  };
  const auto protocol_refused = [failure]() {
    if (failure) {
      failure->stage = ConnectStage::HelloProtocol;
      failure->win32_error = 0;
    }
    return false;
  };

  std::vector<std::uint8_t> hello_payload;
  detail::write_u32(hello_payload, protocol_version);

  unsigned long io_error = 0;
  if (client_.send_request(MessageType::HelloRequest, hello_payload, &io_error) !=
      WriteOutcome::Written)
    return transport_failed(ConnectStage::HelloWrite, io_error);
  auto response = client_.recv_response(timeout, &io_error);
  if (!response)
    return transport_failed(ConnectStage::HelloRead, io_error);
  if (response->type != MessageType::HelloResponse)
    return protocol_refused();

  detail::Reader r(response->payload.data(), response->payload.size());
  auto envelope_incarnation = r.read_u64();
  auto version = r.read_u32();
  auto sid = r.read_u64();
  // A zero incarnation is never a usable Host identity, and a version mismatch
  // (including the Host's explicit version=0 rejection) fails closed.
  if (!envelope_incarnation || *envelope_incarnation == 0 || !version || !sid || !r.at_end())
    return protocol_refused();
  if (*version != protocol_version)
    return protocol_refused();

  cached_session_id_ = *sid;
  incarnation_ = *envelope_incarnation;
  connection_generation_ = connection_generation_.load() + 1;
  link_state_ = LinkState::Established;
  return true;
}

bool IpcBackend::ensure_link(std::chrono::milliseconds timeout) {
  std::lock_guard lock(rpc_mutex_);
  if (link_state_.load() == LinkState::Established && client_.connected())
    return true;

  // Never carry a previous connection (or its identity) into a new one.
  drop_link();
  last_connect_failure_ = ConnectFailure{};
  if (!client_.connect(timeout, &last_connect_failure_))
    return false;
  // Transport is up; the Host incarnation is not established until Hello
  // succeeds, so nothing semantic may be admitted from here.
  link_state_ = LinkState::Connected;
  if (!handshake(timeout, &last_connect_failure_)) {
    drop_link();
    return false;
  }
  return true;
}

bool IpcBackend::connect(std::chrono::milliseconds timeout) { return ensure_link(timeout); }

bool IpcBackend::envelope_matches(const WireFrame &response) const {
  if (response.payload.size() < response_envelope_size)
    return false;
  detail::Reader r(response.payload.data(), response.payload.size());
  auto envelope_incarnation = r.read_u64();
  return envelope_incarnation && *envelope_incarnation == incarnation_.load();
}

std::optional<WireFrame> IpcBackend::rpc(MessageType request_type,
                                         const std::vector<std::uint8_t> &payload,
                                         MessageType expected_response) {
  std::lock_guard lock(rpc_mutex_);
  if (link_state_.load() != LinkState::Established || !client_.connected())
    return std::nullopt;
  if (client_.send_request(request_type, payload) != WriteOutcome::Written) {
    drop_link();
    return std::nullopt;
  }
  auto response = client_.recv_response(kRequestTimeout);
  if (!response || response->type != expected_response || !envelope_matches(*response)) {
    drop_link();
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
  auto result = deserialize_bool(response->payload.data() + response_envelope_size,
                                 response->payload.size() - response_envelope_size);
  return result.value_or(false);
}

bool IpcBackend::reset(Binding binding) {
  std::vector<std::uint8_t> payload;
  serialize(payload, binding);
  auto response = rpc(MessageType::ResetRequest, payload, MessageType::ResetResponse);
  if (!response)
    return false;
  auto result = deserialize_bool(response->payload.data() + response_envelope_size,
                                 response->payload.size() - response_envelope_size);
  return result.value_or(false);
}

void IpcBackend::unbind(Binding binding) {
  std::vector<std::uint8_t> payload;
  serialize(payload, binding);
  (void)rpc(MessageType::UnbindRequest, payload, MessageType::UnbindResponse);
}

bool IpcBackend::healthy() const {
  // Local state only — no IPC probe. "Healthy" means the protocol handshake for
  // the current connection completed; it is not semantic readiness.
  return link_state_.load() == LinkState::Established && client_.connected();
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

BackendBatch IpcBackend::reject_batch(const BackendStimulus &stimulus) const {
  BackendBatch batch;
  batch.disposition = BackendBatch::Disposition::Reject;
  batch.backend_session_id = cached_session_id_;
  batch.binding = stimulus.binding;
  batch.stimulus_id = stimulus.stimulus_id;
  return batch;
}

BackendBatch IpcBackend::send(const BackendStimulus &stimulus) {
  std::vector<std::uint8_t> payload;
  serialize(payload, stimulus);

  std::lock_guard lock(rpc_mutex_);

  // Pre-write check: without an established incarnation no request may be
  // written at all, so the stimulus cannot have reached any Host.
  if (link_state_.load() != LinkState::Established || !client_.connected())
    return forward_batch(stimulus);

  // Attempt to write the request. Only a write that provably accepted no byte of
  // this frame leaves the physical key caller-owned; a partial/uncertain write
  // must never be reported as an ordinary Forward, because the Host may already
  // have observed enough of the request for the outcome to be unknown.
  const WriteOutcome write = client_.send_request(MessageType::SendRequest, payload);
  if (write == WriteOutcome::NotWritten) {
    drop_link();
    return forward_batch(stimulus);
  }
  if (write == WriteOutcome::Uncertain) {
    // No replay, no synthetic key: report the uncertain stimulus as Reject and
    // keep the identity fields so caller-side accounting matches what was sent.
    drop_link();
    return reject_batch(stimulus);
  }

  // Post-write: the request is on the wire. Any failure now is a Reject.
  auto response = client_.recv_response(kRequestTimeout);
  if (!response || response->type != MessageType::SendResponse || !envelope_matches(*response)) {
    drop_link();
    return reject_batch(stimulus);
  }

  auto batch = deserialize_batch(response->payload.data() + response_envelope_size,
                                 response->payload.size() - response_envelope_size);
  if (!batch) {
    drop_link();
    return reject_batch(stimulus);
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
