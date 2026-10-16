#include "host/host_server.hpp"

#include <cstdio>

namespace fusion::ipc {

HostServer::HostServer(std::unique_ptr<Backend> backend, std::wstring pipe_name)
    : backend_(std::move(backend)), server_(std::move(pipe_name)) {}

HostServer::~HostServer() { stop(); }

bool HostServer::start() {
  if (!backend_)
    return false;
  return server_.create();
}

void HostServer::run() {
  running_ = true;
  std::fprintf(stderr, "[FusionHost] waiting for client connection...\n");
  while (running_) {
    if (!server_.wait_for_client()) {
      if (!running_)
        break;
      std::fprintf(stderr, "[FusionHost] client wait failed, retrying...\n");
      continue;
    }
    std::fprintf(stderr, "[FusionHost] client connected\n");
    handle_client();
    std::fprintf(stderr, "[FusionHost] client disconnected\n");
    server_.disconnect_client();
  }
}

void HostServer::stop() {
  running_ = false;
  server_.shutdown();
}

void HostServer::handle_client() {
  while (running_ && server_.client_connected()) {
    auto frame = server_.recv_frame();
    if (!frame) {
      std::fprintf(stderr, "[FusionHost] malformed or no frame, closing client\n");
      return;
    }
    dispatch(*frame);
  }
}

void HostServer::dispatch(const WireFrame &request) {
  switch (request.type) {
  case MessageType::BindRequest:
    handle_bind(request.payload);
    break;
  case MessageType::ResetRequest:
    handle_reset(request.payload);
    break;
  case MessageType::UnbindRequest:
    handle_unbind(request.payload);
    break;
  case MessageType::HealthyRequest:
    handle_healthy();
    break;
  case MessageType::SessionIdRequest:
    handle_session_id();
    break;
  case MessageType::SendRequest:
    handle_send(request.payload);
    break;
  default:
    std::fprintf(stderr, "[FusionHost] unknown message type 0x%02x, closing client\n",
                 static_cast<unsigned>(static_cast<std::uint8_t>(request.type)));
    server_.disconnect_client();
    break;
  }
}

void HostServer::handle_bind(const std::vector<std::uint8_t> &payload) {
  auto binding = deserialize_binding(payload.data(), payload.size());
  if (!binding) {
    server_.disconnect_client();
    return;
  }
  bool result = backend_->bind(*binding);
  send_bool_response(MessageType::BindResponse, result);
}

void HostServer::handle_reset(const std::vector<std::uint8_t> &payload) {
  auto binding = deserialize_binding(payload.data(), payload.size());
  if (!binding) {
    server_.disconnect_client();
    return;
  }
  bool result = backend_->reset(*binding);
  send_bool_response(MessageType::ResetResponse, result);
}

void HostServer::handle_unbind(const std::vector<std::uint8_t> &payload) {
  auto binding = deserialize_binding(payload.data(), payload.size());
  if (!binding) {
    server_.disconnect_client();
    return;
  }
  backend_->unbind(*binding);
  send_response(MessageType::UnbindResponse, {});
}

void HostServer::handle_healthy() {
  send_bool_response(MessageType::HealthyResponse, backend_->healthy());
}

void HostServer::handle_session_id() {
  std::vector<std::uint8_t> payload;
  serialize(payload, backend_->session_id());
  send_response(MessageType::SessionIdResponse, payload);
}

void HostServer::handle_send(const std::vector<std::uint8_t> &payload) {
  auto stimulus = deserialize_stimulus(payload.data(), payload.size());
  if (!stimulus) {
    server_.disconnect_client();
    return;
  }
  auto batch = backend_->send(*stimulus);
  std::vector<std::uint8_t> response_payload;
  serialize(response_payload, batch);
  send_response(MessageType::SendResponse, response_payload);
}

void HostServer::send_response(MessageType type, const std::vector<std::uint8_t> &payload) {
  WireFrame frame;
  frame.type = type;
  frame.payload = payload;
  if (!server_.send_frame(frame)) {
    std::fprintf(stderr, "[FusionHost] send failed, closing client\n");
    server_.disconnect_client();
  }
}

void HostServer::send_bool_response(MessageType type, bool value) {
  std::vector<std::uint8_t> payload;
  serialize(payload, value);
  send_response(type, payload);
}

} // namespace fusion::ipc
