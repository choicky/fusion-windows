#include "host/host_server.hpp"

#include "host/host_diag.hpp"
#include "fusion/settings_protocol.hpp"

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
#include <string>

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

// R30: one technical detail line for a failed pipe operation. The kind says which
// operation failed; the error is the Win32 code the transport recorded.
std::string io_failure_detail(std::uint64_t connection_id, const char *kind, std::uint32_t error) {
  std::string details = "connection=" + std::to_string(connection_id) + " kind=" + kind +
                        " error=" + std::to_string(error);
  if (error == ERROR_TIMEOUT) {
    details += " timeout=1";
  }
  return details;
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
    fusion::host::diag::event("session_end_decision", "query=answered_true action=none");
    return TRUE;
  case WM_ENDSESSION:
    if (wparam == FALSE) {
      // The session end was cancelled: nothing to do.
      fusion::host::diag::event("session_end_decision", "endsession=false action=none");
      return 0;
    }
    fusion::host::diag::event("session_end_decision",
                              "endsession=true action=request_stop_and_wait");
    // The session is really ending. Request the stop and wait (bounded) for main()
    // to finish the ordered shutdown, including Fcitx's own save.
    std::fprintf(stderr, "[FusionHost] session end: waiting for the orderly exit\n");
    if (request_stop_) {
      request_stop_("session_end");
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
    fusion::host::diag::event("window_close", "decision=destroy reason=exit_complete");
    return false; // main() has finished the orderly exit: this close is its own
  }
  // An external close before the exit would otherwise leave the Host running with no
  // logoff/shutdown path. Ask for the same orderly stop a session end would, and keep
  // the window: main() closes it after exit_complete is set.
  std::fprintf(stderr, "[FusionHost] window close: requesting the orderly exit\n");
  fusion::host::diag::event("window_close", "decision=keep_and_stop");
  if (request_stop_) {
    request_stop_("window_close");
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
    // The data pipe is the one object the search package may reach (D089); the claim
    // and stop events below are created separately and stay user-only.
    auto pipe = std::make_unique<PipeServer>(pipe_name_, is_first, true);
    bool was_first = is_first;
    is_first = false;
    if (!pipe->create()) {
      if (was_first) {
        std::fprintf(stderr, "[FusionHost] first pipe instance create failed, exiting\n");
        fusion::host::diag::event(
            "pipe_create",
            ("first=1 ok=0 error=" + std::to_string(pipe->last_error()) + " action=exit").c_str());
        std::exit(1);
      }
      std::fprintf(stderr, "[FusionHost] pipe create failed, retrying...\n");
      fusion::host::diag::event(
          "pipe_create",
          ("first=0 ok=0 error=" + std::to_string(pipe->last_error()) + " action=retry").c_str());
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
    const std::uint64_t connection_id = next_connection_id_.fetch_add(1);
    std::fprintf(stderr, "[FusionHost] client connected\n");
    fusion::host::diag::event("client_connected",
                              ("connection=" + std::to_string(connection_id)).c_str());
    auto connection = std::make_unique<Connection>(std::move(pipe), *backend_, backend_mutex_,
                                                   next_host_context_, incarnation_, connection_id);
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

  // TIP connections retain their existing idle behavior. A settings connection
  // performs exactly one bounded operation after its capability handshake and is
  // then closed, so it never leaves an idle Host worker or partial-frame wait.
  while (pipe->client_connected()) {
    const auto timeout = std::chrono::milliseconds(settings_capable ? 5000 : 0);
    auto frame = pipe->recv_frame(timeout);
    if (!frame) {
      std::fprintf(stderr, "[FusionHost] client read failed, disconnecting\n");
      fusion::host::diag::event("client_read_failed",
                                io_failure_detail(id, "recv", pipe->last_error()).c_str());
      break;
    }
    switch (frame->type) {
    case MessageType::BindRequest:
      if (settings_capable) {
        pipe->disconnect_client();
      } else {
        handle_bind(frame->payload);
      }
      break;
    case MessageType::ResetRequest:
      if (settings_capable) {
        pipe->disconnect_client();
      } else {
        handle_reset(frame->payload);
      }
      break;
    case MessageType::UnbindRequest:
      if (settings_capable) {
        pipe->disconnect_client();
      } else {
        handle_unbind(frame->payload);
      }
      break;
    case MessageType::SendRequest:
      if (settings_capable) {
        pipe->disconnect_client();
      } else {
        handle_send(frame->payload);
      }
      break;
    case MessageType::SettingsHelloRequest:
      // The settings hello is valid only as the first frame handled by
      // handle_hello(); a second one is a malformed protocol transition.
      pipe->disconnect_client();
      break;
    case MessageType::SettingsGetConfigRequest:
      if (settings_capable) {
        handle_settings_get_config(frame->payload);
      } else {
        pipe->disconnect_client();
      }
      break;
    case MessageType::SettingsSetConfigRequest:
      if (settings_capable) {
        handle_settings_set_config(frame->payload);
      } else {
        pipe->disconnect_client();
      }
      break;
    case MessageType::SettingsGetSelectionRequest:
      if (settings_capable) {
        handle_settings_get_selection(frame->payload);
      } else {
        pipe->disconnect_client();
      }
      break;
    case MessageType::SettingsSelectInputMethodRequest:
      if (settings_capable) {
        handle_settings_select_input_method(frame->payload);
      } else {
        pipe->disconnect_client();
      }
      break;
    case MessageType::SettingsGetImListRequest:
      if (settings_capable) {
        handle_settings_get_im_list(frame->payload);
      } else {
        pipe->disconnect_client();
      }
      break;
    case MessageType::SettingsSetImListRequest:
      if (settings_capable) {
        handle_settings_set_im_list(frame->payload);
      } else {
        pipe->disconnect_client();
      }
      break;
    case MessageType::SettingsListDictionariesRequest:
      if (settings_capable) {
        handle_settings_list_dictionaries(frame->payload);
      } else {
        pipe->disconnect_client();
      }
      break;
    case MessageType::SettingsImportDictionaryRequest:
      if (settings_capable) {
        handle_settings_import_dictionary(frame->payload);
      } else {
        pipe->disconnect_client();
      }
      break;
    case MessageType::SettingsSetDictionaryEnabledRequest:
      if (settings_capable) {
        handle_settings_set_dictionary_enabled(frame->payload);
      } else {
        pipe->disconnect_client();
      }
      break;
    case MessageType::SettingsDeleteDictionaryRequest:
      if (settings_capable) {
        handle_settings_delete_dictionary(frame->payload);
      } else {
        pipe->disconnect_client();
      }
      break;
    case MessageType::SettingsReloadDictionariesRequest:
      if (settings_capable) {
        handle_settings_reload_dictionaries(frame->payload);
      } else {
        pipe->disconnect_client();
      }
      break;
    default: {
      const unsigned type = static_cast<unsigned>(static_cast<std::uint8_t>(frame->type));
      std::fprintf(stderr, "[FusionHost] unknown message type 0x%02x, disconnecting\n", type);
      char details[96];
      std::snprintf(details, sizeof(details), "connection=%llu type=0x%02x",
                    static_cast<unsigned long long>(id), type);
      fusion::host::diag::event("unknown_message", details);
      pipe->disconnect_client();
      break;
    }
    }
    if (close_after_settings_request) {
      break;
    }
  }

  // Unbind all contexts owned by this connection.
  unbind_all();
  std::fprintf(stderr, "[FusionHost] client disconnected, contexts unbound\n");
  fusion::host::diag::event("client_disconnected",
                            ("connection=" + std::to_string(id) + " contexts_unbound=1").c_str());
}

void HostServer::Connection::handle_hello() {
  auto frame = pipe->recv_frame(std::chrono::milliseconds(5000));
  if (!frame) {
    std::fprintf(stderr, "[FusionHost] expected Hello, got type=0x%02x\n", 0u);
    fusion::host::diag::event("hello", io_failure_detail(id, "recv", pipe->last_error()).c_str());
    pipe->disconnect_client();
    return;
  }
  if (frame->type == MessageType::SettingsHelloRequest) {
    handle_settings_hello(frame->payload);
    return;
  }
  if (frame->type != MessageType::HelloRequest) {
    const unsigned type = static_cast<unsigned>(static_cast<std::uint8_t>(frame->type));
    std::fprintf(stderr, "[FusionHost] expected Hello, got type=0x%02x\n", type);
    char details[96];
    std::snprintf(details, sizeof(details), "connection=%llu kind=unexpected_type type=0x%02x",
                  static_cast<unsigned long long>(id), type);
    fusion::host::diag::event("hello", details);
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
  fusion::host::diag::event(
      "hello", ("connection=" + std::to_string(id) + " version=" + std::to_string(*client_version) +
                (*client_version == protocol_version ? " ok=1" : " ok=0 reason=version_mismatch"))
                   .c_str());
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

void HostServer::Connection::handle_settings_hello(const std::vector<std::uint8_t> &payload) {
  const auto hello = deserialize_settings_hello(payload.data(), payload.size());
  if (!hello) {
    pipe->disconnect_client();
    return;
  }
  const bool protocol_matches = hello->protocol == protocol_version;
  // Additive API: an older client is served (it never sends the newer types) and a
  // newer client is refused, because this Host cannot know what it would need.
  const bool api_matches = hello->api >= settings_api_min_version &&
                           hello->api <= settings_api_version;
  std::uint64_t session = 0;
  if (protocol_matches) {
    std::lock_guard lock(backend_mutex);
    session = backend.session_id();
  }
  const SettingsStatus status = !protocol_matches || !api_matches
                                    ? SettingsStatus::VersionMismatch
                                    : SettingsStatus::Ok;
  const std::string error = !protocol_matches
                                ? "The Host IPC protocol version is not supported."
                                : (!api_matches ? "The Host settings API version is not supported."
                                                : std::string());
  std::vector<std::uint8_t> response_payload;
  detail::write_u32(response_payload, protocol_matches ? protocol_version : 0);
  detail::write_u64(response_payload, session);
  detail::write_u32(response_payload, settings_api_version);
  const auto status_payload = serialize_settings_status(status, error);
  response_payload.insert(response_payload.end(), status_payload.begin(), status_payload.end());
  send_response(MessageType::SettingsHelloResponse, response_payload);
  settings_capable = status == SettingsStatus::Ok;
  close_after_settings_request = !settings_capable;
}

void HostServer::Connection::handle_settings_get_config(
    const std::vector<std::uint8_t> &payload) {
  if (payload.size() > max_settings_payload) {
    pipe->disconnect_client();
    return;
  }
  const auto uri = deserialize_settings_get_request(payload.data(), payload.size());
  if (!uri) {
    pipe->disconnect_client();
    return;
  }
  SettingsSnapshot snapshot;
  std::string error;
  const auto status = backend.settings_get_config(*uri, snapshot, error);
  std::vector<std::uint8_t> response_payload = serialize_settings_status(status, error);
  if (status == SettingsStatus::Ok &&
      !settings_detail::write_snapshot(response_payload, snapshot)) {
    response_payload = serialize_settings_status(
        SettingsStatus::Error, "The settings schema exceeds the supported response bound.");
  }
  send_response(MessageType::SettingsGetConfigResponse, response_payload);
  close_after_settings_request = true;
}

void HostServer::Connection::handle_settings_set_config(
    const std::vector<std::uint8_t> &payload) {
  if (payload.size() > max_settings_payload) {
    pipe->disconnect_client();
    return;
  }
  std::string uri;
  SettingsValue values;
  if (!deserialize_settings_set_request(payload.data(), payload.size(), uri, values)) {
    pipe->disconnect_client();
    return;
  }
  SettingsSnapshot snapshot;
  std::string error;
  const auto status = backend.settings_set_config(uri, values, snapshot, error);
  std::vector<std::uint8_t> response_payload = serialize_settings_status(status, error);
  if (status == SettingsStatus::Ok &&
      !settings_detail::write_snapshot(response_payload, snapshot)) {
    response_payload = serialize_settings_status(
        SettingsStatus::Indeterminate,
        "Fcitx accepted the apply, but the Host could not return the resulting settings. Reload before retrying.");
  }
  send_response(MessageType::SettingsSetConfigResponse, response_payload);
  close_after_settings_request = true;
}

void HostServer::Connection::handle_settings_get_selection(
    const std::vector<std::uint8_t> &payload) {
  if (!payload.empty()) {
    pipe->disconnect_client();
    return;
  }
  SettingsSelection selection;
  std::string error;
  const auto status = backend.settings_get_selection(selection, error);
  std::vector<std::uint8_t> response_payload = serialize_settings_status(status, error);
  if (status == SettingsStatus::Ok &&
      !settings_detail::write_selection(response_payload, selection)) {
    response_payload = serialize_settings_status(
        SettingsStatus::Error, "The input-method list exceeds the supported response bound.");
  }
  send_response(MessageType::SettingsGetSelectionResponse, response_payload);
  close_after_settings_request = true;
}

void HostServer::Connection::handle_settings_select_input_method(
    const std::vector<std::uint8_t> &payload) {
  if (payload.size() > max_settings_payload) {
    pipe->disconnect_client();
    return;
  }
  detail::Reader reader{payload.data(), payload.size()};
  auto method = reader.read_string();
  if (!method || !reader.at_end()) {
    pipe->disconnect_client();
    return;
  }
  SettingsSelection selection;
  std::string error;
  const auto status = backend.settings_select_input_method(*method, selection, error);
  std::vector<std::uint8_t> response_payload = serialize_settings_status(status, error);
  if (status == SettingsStatus::Ok &&
      !settings_detail::write_selection(response_payload, selection)) {
    response_payload = serialize_settings_status(
        SettingsStatus::Indeterminate,
        "The input method may have changed, but the Host could not confirm the selection. Reload before retrying.");
  }
  send_response(MessageType::SettingsSelectInputMethodResponse, response_payload);
  close_after_settings_request = true;
}

void HostServer::Connection::handle_settings_get_im_list(
    const std::vector<std::uint8_t> &payload) {
  if (!payload.empty()) {
    pipe->disconnect_client();
    return;
  }
  ImList list;
  std::string error;
  const auto status = backend.settings_get_im_list(list, error);
  std::vector<std::uint8_t> response_payload = serialize_settings_status(status, error);
  if (status == SettingsStatus::Ok && !settings_detail::write_im_list(response_payload, list)) {
    response_payload = serialize_settings_status(
        SettingsStatus::Error, "The input-method list exceeds the supported response bound.");
  }
  send_response(MessageType::SettingsGetImListResponse, response_payload);
  close_after_settings_request = true;
}

void HostServer::Connection::handle_settings_set_im_list(
    const std::vector<std::uint8_t> &payload) {
  if (payload.size() > max_settings_payload) {
    pipe->disconnect_client();
    return;
  }
  std::vector<std::string> enabled;
  if (!deserialize_settings_set_im_list_request(payload.data(), payload.size(), enabled)) {
    pipe->disconnect_client();
    return;
  }
  ImList list;
  std::string error;
  const auto status = backend.settings_set_im_list(enabled, list, error);
  std::vector<std::uint8_t> response_payload = serialize_settings_status(status, error);
  if (status == SettingsStatus::Ok && !settings_detail::write_im_list(response_payload, list)) {
    response_payload = serialize_settings_status(
        SettingsStatus::Indeterminate,
        "The input-method list may have changed, but the Host could not confirm the result. Reload "
        "before retrying.");
  }
  send_response(MessageType::SettingsSetImListResponse, response_payload);
  close_after_settings_request = true;
}

void HostServer::Connection::handle_settings_list_dictionaries(
    const std::vector<std::uint8_t> &payload) {
  if (!payload.empty()) {
    pipe->disconnect_client();
    return;
  }
  DictionaryList list;
  std::string error;
  const auto status = backend.settings_list_dictionaries(list, error);
  std::vector<std::uint8_t> response_payload = serialize_settings_status(status, error);
  if (status == SettingsStatus::Ok &&
      !settings_detail::write_dictionary_list(response_payload, list)) {
    response_payload = serialize_settings_status(
        SettingsStatus::Error, "The dictionary list exceeds the supported response bound.");
  }
  send_response(MessageType::SettingsListDictionariesResponse, response_payload);
  close_after_settings_request = true;
}

void HostServer::Connection::handle_settings_import_dictionary(
    const std::vector<std::uint8_t> &payload) {
  if (payload.size() > max_settings_payload) {
    pipe->disconnect_client();
    return;
  }
  std::string path;
  bool overwrite = false;
  if (!deserialize_settings_import_dictionary_request(payload.data(), payload.size(), path,
                                                      overwrite)) {
    pipe->disconnect_client();
    return;
  }
  DictionaryEntry imported;
  DictionaryList list;
  std::string error;
  const auto status = backend.settings_import_dictionary(path, overwrite, imported, list, error);
  std::vector<std::uint8_t> response_payload = serialize_settings_status(status, error);
  if (status == SettingsStatus::Ok) {
    settings_detail::write_dictionary_entry(response_payload, imported);
    if (!settings_detail::write_dictionary_list(response_payload, list)) {
      response_payload = serialize_settings_status(
          SettingsStatus::Indeterminate,
          "The dictionary was imported, but the Host could not return the resulting list. "
          "Reload before retrying.");
    }
  }
  send_response(MessageType::SettingsImportDictionaryResponse, response_payload);
  close_after_settings_request = true;
}

void HostServer::Connection::handle_settings_set_dictionary_enabled(
    const std::vector<std::uint8_t> &payload) {
  if (payload.size() > max_settings_payload) {
    pipe->disconnect_client();
    return;
  }
  std::string name;
  bool enabled = false;
  if (!deserialize_settings_set_dictionary_enabled_request(payload.data(), payload.size(), name,
                                                           enabled)) {
    pipe->disconnect_client();
    return;
  }
  DictionaryList list;
  std::string error;
  const auto status = backend.settings_set_dictionary_enabled(name, enabled, list, error);
  std::vector<std::uint8_t> response_payload = serialize_settings_status(status, error);
  if (status == SettingsStatus::Ok &&
      !settings_detail::write_dictionary_list(response_payload, list)) {
    response_payload = serialize_settings_status(
        SettingsStatus::Indeterminate,
        "The dictionary state was written, but the Host could not return the resulting list. "
        "Reload before retrying.");
  }
  send_response(MessageType::SettingsSetDictionaryEnabledResponse, response_payload);
  close_after_settings_request = true;
}

void HostServer::Connection::handle_settings_delete_dictionary(
    const std::vector<std::uint8_t> &payload) {
  if (payload.size() > max_settings_payload) {
    pipe->disconnect_client();
    return;
  }
  std::string name;
  if (!deserialize_settings_dictionary_name_request(payload.data(), payload.size(), name)) {
    pipe->disconnect_client();
    return;
  }
  DictionaryList list;
  std::string error;
  const auto status = backend.settings_delete_dictionary(name, list, error);
  std::vector<std::uint8_t> response_payload = serialize_settings_status(status, error);
  if (status == SettingsStatus::Ok &&
      !settings_detail::write_dictionary_list(response_payload, list)) {
    response_payload = serialize_settings_status(
        SettingsStatus::Indeterminate,
        "The dictionary was deleted, but the Host could not return the resulting list. Reload "
        "before retrying.");
  }
  send_response(MessageType::SettingsDeleteDictionaryResponse, response_payload);
  close_after_settings_request = true;
}

void HostServer::Connection::handle_settings_reload_dictionaries(
    const std::vector<std::uint8_t> &payload) {
  if (!payload.empty()) {
    pipe->disconnect_client();
    return;
  }
  DictionaryList list;
  std::string error;
  const auto status = backend.settings_reload_dictionaries(list, error);
  std::vector<std::uint8_t> response_payload = serialize_settings_status(status, error);
  if (status == SettingsStatus::Ok &&
      !settings_detail::write_dictionary_list(response_payload, list)) {
    response_payload = serialize_settings_status(
        SettingsStatus::Error, "The dictionary list exceeds the supported response bound.");
  }
  send_response(MessageType::SettingsReloadDictionariesResponse, response_payload);
  close_after_settings_request = true;
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
    // R51: the pipe knows which process asked for this context. The Host passes that to the
    // backend, because the refusal that later waits for the context arrives on another
    // connection — the settings window's own — which cannot see this one. Recorded first, so
    // a context is never live without the process that owns it. 0 when the pipe would not say.
    backend.note_context_owner(host_binding.context, pipe->client_pid());
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
    fusion::host::diag::event("send_failed",
                              io_failure_detail(id, "send", pipe->last_error()).c_str());
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
