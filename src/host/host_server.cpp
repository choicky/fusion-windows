#include "host/host_server.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace fusion::ipc {

HostServer::HostServer(std::unique_ptr<Backend> backend, std::wstring pipe_name)
    : backend_(std::move(backend)), pipe_name_(std::move(pipe_name)) {}

HostServer::~HostServer() { stop(); }

bool HostServer::start() {
  if (!backend_)
    return false;
  return true;
}

void HostServer::run() {
  accept_loop_active_ = true;
  // A stop() that raced ahead of run() must not be undone here.
  if (!stopping_.load())
    running_ = true;
  std::fprintf(stderr, "[FusionHost] accepting connections on: %ls\n", pipe_name_.c_str());
  accept_loop();
  accept_loop_active_ = false;
  std::lock_guard lock(accept_done_mutex_);
  accept_done_cv_.notify_all();
}

void HostServer::stop() {
  stopping_ = true;
  running_ = false;
  // ConnectNamedPipe is called without a timeout, so the accept loop cannot
  // observe running_ by itself: one short-lived client connection completes the
  // pending call and lets the loop exit.
  wake_accept_loop();
  {
    // Do not drain while the accept loop could still spawn a connection.
    std::unique_lock lock(accept_done_mutex_);
    accept_done_cv_.wait_for(lock, std::chrono::seconds(5),
                             [this] { return !accept_loop_active_.load(); });
  }
  drain_connections();
}

void HostServer::wake_accept_loop() {
  HANDLE wake = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (wake != INVALID_HANDLE_VALUE)
    CloseHandle(wake);
}

void HostServer::drain_connections() {
  std::vector<TrackedConnection> connections;
  {
    std::lock_guard lock(connections_mutex_);
    connections.swap(connections_);
  }
  for (auto &tracked : connections) {
    // Unblock the connection's pending receive, then wait for it to finish so
    // no thread can touch backend_ after this call returns.
    tracked.connection->disconnect();
    if (tracked.thread.joinable())
      tracked.thread.join();
  }
}

void HostServer::accept_loop() {
  bool is_first = true;
  while (running_) {
    auto pipe = std::make_unique<PipeServer>(pipe_name_, is_first);
    bool was_first = is_first;
    is_first = false;
    if (!pipe->create()) {
      if (was_first) {
        std::fprintf(stderr, "[FusionHost] first pipe instance create failed, exiting\n");
        std::exit(1);
      }
      std::fprintf(stderr, "[FusionHost] pipe create failed, retrying...\n");
      Sleep(100);
      continue;
    }
    // Wait for a client — block indefinitely (no timeout).
    if (!pipe->wait_for_client(std::chrono::milliseconds(0))) {
      if (!running_)
        break; // woken by stop()
      continue;
    }
    if (!running_)
      break; // stop() woke this iteration: nothing to serve
    std::fprintf(stderr, "[FusionHost] client connected\n");
    auto connection = std::make_unique<Connection>(std::move(pipe), *backend_, backend_mutex_,
                                                   next_host_context_);
    Connection *raw = connection.get();
    std::lock_guard lock(connections_mutex_);
    TrackedConnection tracked;
    tracked.connection = std::move(connection);
    tracked.thread = std::thread([raw] { raw->run(); });
    connections_.push_back(std::move(tracked));
  }
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

void HostServer::Connection::disconnect() { pipe->disconnect_client(); }

void HostServer::Connection::run() {
  // First message must be Hello.
  handle_hello();
  if (!pipe->client_connected())
    return;

  // Process requests until disconnect.
  while (pipe->client_connected()) {
    auto frame = pipe->recv_frame(std::chrono::milliseconds(0)); // block indefinitely
    if (!frame) {
      std::fprintf(stderr, "[FusionHost] client read failed, disconnecting\n");
      break;
    }
    switch (frame->type) {
    case MessageType::BindRequest:
      handle_bind(frame->payload);
      break;
    case MessageType::ResetRequest:
      handle_reset(frame->payload);
      break;
    case MessageType::UnbindRequest:
      handle_unbind(frame->payload);
      break;
    case MessageType::SendRequest:
      handle_send(frame->payload);
      break;
    default:
      std::fprintf(stderr, "[FusionHost] unknown message type 0x%02x, disconnecting\n",
                   static_cast<unsigned>(static_cast<std::uint8_t>(frame->type)));
      pipe->disconnect_client();
      break;
    }
  }

  // Unbind all contexts owned by this connection.
  unbind_all();
  std::fprintf(stderr, "[FusionHost] client disconnected, contexts unbound\n");
}

void HostServer::Connection::handle_hello() {
  auto frame = pipe->recv_frame(std::chrono::milliseconds(5000));
  if (!frame || frame->type != MessageType::HelloRequest) {
    std::fprintf(stderr, "[FusionHost] expected Hello, got type=0x%02x\n",
                 frame ? static_cast<unsigned>(static_cast<std::uint8_t>(frame->type)) : 0);
    pipe->disconnect_client();
    return;
  }

  detail::Reader r(frame->payload.data(), frame->payload.size());
  auto client_version = r.read_u32();
  if (!client_version || !r.at_end()) {
    pipe->disconnect_client();
    return;
  }

  std::vector<std::uint8_t> response_payload;
  if (*client_version != protocol_version) {
    // Version mismatch: respond with version=0.
    detail::write_u32(response_payload, 0);
    detail::write_u64(response_payload, 0);
  } else {
    std::lock_guard lock(backend_mutex);
    detail::write_u32(response_payload, protocol_version);
    detail::write_u64(response_payload, backend.session_id());
  }
  send_response(MessageType::HelloResponse, response_payload);
}

void HostServer::Connection::handle_bind(const std::vector<std::uint8_t> &payload) {
  auto client_binding = deserialize_binding(payload.data(), payload.size());
  if (!client_binding) {
    pipe->disconnect_client();
    return;
  }

  auto host_binding = remap_binding(*client_binding);
  bool result = false;
  {
    std::lock_guard lock(backend_mutex);
    result = backend.bind(host_binding);
  }
  send_bool_response(MessageType::BindResponse, result);
}

void HostServer::Connection::handle_reset(const std::vector<std::uint8_t> &payload) {
  auto client_binding = deserialize_binding(payload.data(), payload.size());
  if (!client_binding) {
    pipe->disconnect_client();
    return;
  }

  auto it = context_map.find(client_binding->context);
  if (it == context_map.end()) {
    send_bool_response(MessageType::ResetResponse, false);
    return;
  }
  auto host_binding = it->second;

  bool result = false;
  {
    std::lock_guard lock(backend_mutex);
    result = backend.reset(host_binding);
  }
  send_bool_response(MessageType::ResetResponse, result);
}

void HostServer::Connection::handle_unbind(const std::vector<std::uint8_t> &payload) {
  auto client_binding = deserialize_binding(payload.data(), payload.size());
  if (!client_binding) {
    pipe->disconnect_client();
    return;
  }

  auto it = context_map.find(client_binding->context);
  if (it != context_map.end()) {
    auto host_binding = it->second;
    {
      std::lock_guard lock(backend_mutex);
      backend.unbind(host_binding);
    }
    context_map.erase(it);
  }
  send_response(MessageType::UnbindResponse, {});
}

void HostServer::Connection::handle_send(const std::vector<std::uint8_t> &payload) {
  auto stimulus = deserialize_stimulus(payload.data(), payload.size());
  if (!stimulus) {
    pipe->disconnect_client();
    return;
  }

  // Remap the binding context.
  auto it = context_map.find(stimulus->binding.context);
  if (it == context_map.end()) {
    // Unknown context: reject.
    BackendBatch batch;
    batch.disposition = BackendBatch::Disposition::Reject;
    std::vector<std::uint8_t> response_payload;
    serialize(response_payload, batch);
    send_response(MessageType::SendResponse, response_payload);
    return;
  }

  auto host_stimulus = *stimulus;
  host_stimulus.binding = it->second;

  BackendBatch batch;
  {
    std::lock_guard lock(backend_mutex);
    batch = backend.send(host_stimulus);
  }

  // Return the client's full original binding in the response.
  batch.binding = stimulus->binding;

  std::vector<std::uint8_t> response_payload;
  serialize(response_payload, batch);
  send_response(MessageType::SendResponse, response_payload);
}

void HostServer::Connection::send_response(MessageType type,
                                           const std::vector<std::uint8_t> &payload) {
  WireFrame frame;
  frame.type = type;
  frame.payload = payload;
  if (!pipe->send_frame(frame)) {
    std::fprintf(stderr, "[FusionHost] send failed, disconnecting client\n");
    pipe->disconnect_client();
  }
}

void HostServer::Connection::send_bool_response(MessageType type, bool value) {
  std::vector<std::uint8_t> payload;
  serialize(payload, value);
  send_response(type, payload);
}

void HostServer::Connection::unbind_all() {
  std::lock_guard lock(backend_mutex);
  for (auto &[client_ctx, host_binding] : context_map) {
    backend.unbind(host_binding);
  }
  context_map.clear();
}

Binding HostServer::Connection::remap_binding(const Binding &client_binding) {
  auto it = context_map.find(client_binding.context);
  if (it != context_map.end()) {
    return it->second;
  }
  // New mapping: only the context field is Host-unique; lifetime/epoch pass through.
  Binding host_binding{next_host_context.fetch_add(1), client_binding.lifetime,
                       client_binding.epoch};
  context_map[client_binding.context] = host_binding;
  return host_binding;
}

} // namespace fusion::ipc
