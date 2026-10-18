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

namespace {

// PoC incarnation identity for one Host process/instance: never 0 (0 means "no
// incarnation"), distinct across Host restarts on this machine. This is an
// engineering identifier for incarnation fencing, not a frozen production ID.
std::uint64_t generate_incarnation() {
  static std::atomic<std::uint64_t> sequence{0};
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  std::uint64_t value = static_cast<std::uint64_t>(counter.QuadPart);
  value ^= static_cast<std::uint64_t>(GetCurrentProcessId()) * 0x9E3779B97F4A7C15ull;
  value ^= (sequence.fetch_add(1) + 1) * 0xC2B2AE3D27D4EB4Full;
  return value == 0 ? 1 : value;
}

// The per-principal suffix of a pipe name: `<user SID>-<session id>`. The claim and
// the stop event reuse it so all three kernel objects name exactly the same scope.
std::wstring principal_suffix(const std::wstring &pipe_name) {
  std::wstring suffix = pipe_name;
  const std::wstring prefix = default_pipe_name_prefix;
  if (suffix.rfind(prefix, 0) == 0) {
    suffix.erase(0, prefix.size());
  }
  // Sanitize only the prefix the pipe contributed; the `Local\` namespace marker
  // must survive or the object would land on a name of its own.
  for (wchar_t &character : suffix) {
    if (character == L'\\' || character == L'/') {
      character = L'-';
    }
  }
  return suffix;
}

} // namespace

HostClaim &HostClaim::operator=(HostClaim &&other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool HostClaim::acquire(const std::wstring &pipe_name) {
  release();
  if (pipe_name.empty()) {
    return false;
  }
  // Readable name in the per-session object namespace. The pipe name already
  // carries the user SID and session id (compute_pipe_name), so stripping the
  // well-known pipe prefix keeps the claim scoped to the same principal and makes
  // the SID visible in tools such as WinObj.
  const std::wstring name = L"Local\\FusionHost-claim-" + principal_suffix(pipe_name);
  name_ = name;
  // The pipe's own user-only DACL: another principal cannot pre-create the name and
  // hold this Host down. Fail closed if the descriptor cannot be built.
  SECURITY_ATTRIBUTES attributes = make_pipe_security();
  if (attributes.lpSecurityDescriptor == nullptr) {
    return false;
  }
  HANDLE handle = CreateEventW(&attributes, TRUE, FALSE, name.c_str());
  const DWORD error = GetLastError();
  free_pipe_security(attributes);
  if (handle == nullptr) {
    return false;
  }
  if (error == ERROR_ALREADY_EXISTS) {
    // A live Host owns this principal's claim. Fail closed rather than share it.
    CloseHandle(handle);
    return false;
  }
  handle_ = handle;
  return true;
}

void HostClaim::release() {
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
}

bool HostStopEvent::create(const std::wstring &scope) {
  release();
  const std::wstring suffix = scope.empty() ? principal_suffix(compute_pipe_name()) : scope;
  if (suffix.empty()) {
    return false; // no principal: fail closed, there is nothing to name the event after
  }
  const std::wstring name = L"Local\\FusionHost-stop-" + suffix;
  // The pipe's own user-only DACL, so another principal can neither create nor
  // signal it. Fail closed if the descriptor cannot be built.
  SECURITY_ATTRIBUTES attributes = make_pipe_security();
  if (attributes.lpSecurityDescriptor == nullptr) {
    return false;
  }
  HANDLE handle = CreateEventW(&attributes, TRUE, FALSE, name.c_str());
  const DWORD error = GetLastError();
  free_pipe_security(attributes);
  if (handle == nullptr) {
    return false;
  }
  if (error == ERROR_ALREADY_EXISTS) {
    // Another Host for this principal is alive and owns the stop path.
    CloseHandle(handle);
    return false;
  }
  handle_ = handle;
  name_ = name;
  return true;
}

void HostStopEvent::release() {
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
}

bool wait_for_stop_request(void *stop_event, void *exit_complete) {
  if (stop_event == nullptr || exit_complete == nullptr) {
    return false;
  }
  HANDLE handles[2] = {static_cast<HANDLE>(stop_event), static_cast<HANDLE>(exit_complete)};
  return WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0;
}

bool SessionEndHandler::handles(unsigned int message) {
  return message == WM_QUERYENDSESSION || message == WM_ENDSESSION;
}

std::intptr_t SessionEndHandler::handle(unsigned int message, std::uintptr_t wparam) {
  switch (message) {
  case WM_QUERYENDSESSION:
    // Answer TRUE and stop nothing: the logoff/shutdown can still be cancelled
    // after this message (an application asks to save and the user cancels), and a
    // Host that stopped here would stay gone until the next logon.
    return TRUE;
  case WM_ENDSESSION:
    if (wparam == FALSE) {
      return 0; // the session end was cancelled: nothing to do
    }
    // The session is really ending. Request the stop and wait (bounded) for main()
    // to finish the ordered shutdown, including Fcitx's own save.
    std::fprintf(stderr, "[FusionHost] session end: waiting for the orderly exit\n");
    if (request_stop_) {
      request_stop_();
    }
    if (exit_complete_ != nullptr) {
      WaitForSingleObject(static_cast<HANDLE>(exit_complete_), timeout_ms_);
    }
    return 0;
  default:
    return 0;
  }
}

bool SessionEndHandler::close_keeps_window() {
  if (exit_complete_ != nullptr &&
      WaitForSingleObject(static_cast<HANDLE>(exit_complete_), 0) == WAIT_OBJECT_0) {
    return false; // main() has finished the orderly exit: this close is its own
  }
  // An external close before the exit would otherwise leave the Host running with no
  // logoff/shutdown path. Ask for the same orderly stop a session end would, and keep
  // the window: main() closes it after exit_complete is set.
  std::fprintf(stderr, "[FusionHost] window close: requesting the orderly exit\n");
  if (request_stop_) {
    request_stop_();
  }
  return true;
}

HostServer::HostServer(std::unique_ptr<Backend> backend, std::wstring pipe_name,
                       std::uint64_t incarnation)
    : backend_(std::move(backend)), pipe_name_(std::move(pipe_name)),
      incarnation_(incarnation == 0 ? generate_incarnation() : incarnation) {}

HostServer::~HostServer() {
  // Idempotent, and always the owner's thread: main() has normally already run
  // stop() and shutdown_backend(), which clears backend_. This only covers early
  // returns, and it keeps backend destruction behind backend_mutex_ either way.
  stop();
  shutdown_backend();
}

bool HostServer::start() {
  if (!backend_)
    return false;
  return true;
}

void HostServer::request_stop() {
  stopping_ = true;
  running_ = false;
  // ConnectNamedPipe is called without a timeout, so the accept loop cannot observe
  // running_ by itself: one short-lived client connection completes the pending call
  // and lets the loop exit. Nothing here touches connections or the backend, so a
  // console control handler cannot race the owner's shutdown.
  wake_accept_loop();
}

void HostServer::stop() {
  request_stop();
  {
    // Do not drain while the accept loop could still spawn a connection.
    std::unique_lock lock(accept_done_mutex_);
    accept_done_cv_.wait_for(lock, std::chrono::seconds(5),
                             [this] { return !accept_loop_active_.load(); });
  }
  drain_connections();
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

void HostServer::shutdown_backend() {
  // stop() has joined every connection thread, so no backend_ call is in flight.
  std::unique_ptr<Backend> backend;
  {
    std::lock_guard lock(backend_mutex_);
    backend = std::move(backend_);
  }
  // Destruction runs the backend's orderly shutdown; Fcitx saves on instance exit.
}

void HostServer::wake_accept_loop() {
  HANDLE wake = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (wake != INVALID_HANDLE_VALUE)
    CloseHandle(wake);
}

std::size_t HostServer::tracked_connection_count() const {
  std::lock_guard lock(connections_mutex_);
  return connections_.size();
}

void HostServer::reap_completed_connections() {
  std::lock_guard lock(connections_mutex_);
  for (auto it = connections_.begin(); it != connections_.end();) {
    TrackedConnection &tracked = **it;
    // completed is set by the connection thread as its last action, so the
    // thread needs nothing from this lock and joining cannot deadlock. A live
    // connection is never joined here.
    if (!tracked.completed.load()) {
      ++it;
      continue;
    }
    if (tracked.thread.joinable())
      tracked.thread.join();
    it = connections_.erase(it);
  }
}

void HostServer::drain_connections() {
  std::vector<std::unique_ptr<TrackedConnection>> connections;
  {
    std::lock_guard lock(connections_mutex_);
    connections.swap(connections_);
  }
  for (auto &tracked : connections) {
    // Unblock the connection's pending receive, then wait for it to finish so
    // no thread can touch backend_ after this call returns.
    tracked->connection->disconnect();
    if (tracked->thread.joinable())
      tracked->thread.join();
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
                                                   next_host_context_, incarnation_);
    Connection *raw = connection.get();
    // A finished connection is reaped on the next accepted connection, so the
    // retained entries stay bounded by the live connections plus one completed
    // one instead of growing with the Host's connection history.
    auto tracked = std::make_unique<TrackedConnection>();
    tracked->connection = std::move(connection);
    TrackedConnection *tracked_raw = tracked.get();
    tracked->thread = std::thread([raw, tracked_raw] {
      raw->run();
      tracked_raw->completed = true;
    });
    {
      std::lock_guard lock(connections_mutex_);
      connections_.push_back(std::move(tracked));
    }
    reap_completed_connections();
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
  // Stage 4B: every response is stamped with this Host incarnation, so a client
  // can reject a result that does not belong to the connection's incarnation.
  frame.payload = envelop_response(incarnation, payload);
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
