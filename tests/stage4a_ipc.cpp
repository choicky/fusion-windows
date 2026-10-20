// Stage 4A IPC tests: protocol serialization, loopback pipe, HostServer, and the
// end-to-end FusionHost.exe integration.
//
// Usage: stage4a <mode>, one of: protocol | pipeendpoint | loopback | hostserver |
// wp3singleton | wp3shutdown | wp4stopevent | wp4sessionend | integration
//
// - protocol: serialization round-trip, bounds, malformed input, trailing byte rejection
// - pipeendpoint: the settings client's endpoint name — the default startup path
//   keeps the product identity and round-trips to compute_pipe_name(), an explicit
//   isolated endpoint keeps its complete name, and empty/malformed endpoints are
//   rejected instead of being rewritten into another endpoint.
// - loopback: real Named Pipe with a CallbackBackend server thread (no Fcitx)
// - hostserver: real HostServer with a fake Backend that mimics FcitxBackend
//   admission (duplicate bind rejected, every call fails on admission contention):
//   concurrent multi-client bind/send with context remapping, disconnect
//   isolation, transport-failure separation (definitely-not-written vs
//   uncertain-write vs post-write), healthy() performs no IPC, completed
//   connection reaping, and stop()/join shutdown.
// - wp3singleton / wp3shutdown: the Host singleton claim and the single-owner
//   ordered shutdown (Stage 6 WP3).
// - wp4stopevent: the named stop event an elevated install/uninstall signals
//   instead of terminating the Host; the watcher only requests the stop and the
//   owning thread still drains before the backend is destroyed (Stage 6 WP4).
// - wp4sessionend: the session-end decision — a cancelled logoff (query only, or
//   WM_ENDSESSION(FALSE)) stops nothing, only WM_ENDSESSION(TRUE) requests the stop
//   and waits, bounded; an external WM_CLOSE before the exit requests the same stop
//   and keeps the window (Stage 6 WP4; R34 clarifies that surviving a cancelled end is
//   the task's event trigger, not this decision).
// - hostdiag: the R30 opt-in Host diagnostic log — off by default, file and line
//   format when enabled, privacy-marker redaction, the size cap and retention.
// - integration: full FusionHost.exe + Fcitx backend over IPC (only in the
//   FUSION_WITH_FCITX5 build; requires a running FusionHost.exe and the Fcitx
//   runtime). Set FUSION_STAGE4A_SKIP_INTEGRATION=1 to skip instead of fail.
//
// protocol/loopback/hostserver build and run in the Mock-only build too: the
// transport, the IpcBackend client and the HostServer do not depend on Fcitx.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "fusion/backend.hpp"
#include "fusion/ipc_protocol.hpp"
#include "fusion/settings_protocol.hpp"
#include "host/host_diag.hpp"
#include "host/host_server.hpp"
#include "ipc/ipc_backend.hpp"
#include "ipc/pipe_security.hpp"
#include "ipc/pipe_server.hpp"
#include "tsf/semantic.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace fusion;
using namespace fusion::tsf;
using namespace fusion::ipc;

void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}

// ---------------------------------------------------------------------------
// Server fixtures
//
// A failing assertion must report its message. A joinable std::thread on the
// unwinding stack would call std::terminate instead, so every server thread is
// owned by a fixture that disconnects, stops and joins on scope exit.
// ---------------------------------------------------------------------------

constexpr std::chrono::milliseconds kWait{10000};

// Stage 4B response envelope: a test server that hand-writes Hello responses
// must stamp the responding Host incarnation.
constexpr std::uint64_t kTestIncarnation = 0x4B00000000000001ull;

std::wstring test_pipe_name(const wchar_t *suffix) {
  return std::wstring(L"\\\\.\\pipe\\FusionHost-ipc-test-") + suffix;
}

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

BackendStimulus character_stimulus(const Binding &binding, std::uint64_t stimulus_id,
                                   char character) {
  BackendStimulus stimulus;
  stimulus.binding = binding;
  stimulus.stimulus_id = stimulus_id;
  stimulus.type = BackendStimulus::Type::Character;
  stimulus.character = character;
  stimulus.keysym = static_cast<std::uint32_t>(static_cast<unsigned char>(character));
  return stimulus;
}

// HostServer::run() creates the first pipe instance asynchronously, so connect
// with retries instead of sleeping for a fixed time.
std::unique_ptr<IpcBackend> connect_client(const std::wstring &pipe_name) {
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (std::chrono::steady_clock::now() < deadline) {
    auto client = std::make_unique<IpcBackend>(pipe_name);
    if (client->connect(std::chrono::milliseconds(200)))
      return client;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return nullptr;
}

bool connect_settings_client(PipeClient &client, std::uint32_t protocol,
                             std::uint32_t api, std::uint64_t &incarnation,
                             SettingsStatus &status) {
  if (!client.connect(std::chrono::milliseconds(500))) {
    return false;
  }
  if (client.send_request(MessageType::SettingsHelloRequest,
                          serialize_settings_hello(protocol, api)) != WriteOutcome::Written) {
    return false;
  }
  auto response = client.recv_response(std::chrono::seconds(5));
  if (!response || response->type != MessageType::SettingsHelloResponse) {
    return false;
  }
  detail::Reader reader{response->payload.data(), response->payload.size()};
  auto host = reader.read_u64();
  auto hostProtocol = reader.read_u32();
  auto session = reader.read_u64();
  auto hostApi = reader.read_u32();
  auto prefix = deserialize_settings_status(reader);
  (void)session;
  (void)hostApi;
  if (!host || !hostProtocol || !prefix) {
    return false;
  }
  incarnation = *host;
  status = prefix->status;
  return reader.at_end();
}

std::optional<WireFrame> settings_request(PipeClient &client, MessageType type,
                                          const std::vector<std::uint8_t> &payload,
                                          std::uint64_t incarnation) {
  if (client.send_request(type, payload) != WriteOutcome::Written) {
    return std::nullopt;
  }
  auto response = client.recv_response(std::chrono::seconds(5));
  if (!response) {
    return std::nullopt;
  }
  detail::Reader reader{response->payload.data(), response->payload.size()};
  auto host = reader.read_u64();
  if (!host || *host != incarnation) {
    return std::nullopt;
  }
  response->payload.erase(response->payload.begin(), response->payload.begin() + 8);
  return response;
}

// Raw PipeServer plus its serving thread.
class RawPipeServer {
public:
  explicit RawPipeServer(std::wstring pipe_name)
      : server_(std::make_unique<PipeServer>(std::move(pipe_name))) {}

  bool create() { return server_->create(); }
  PipeServer &server() { return *server_; }
  // Closes the server end (used to force a client-side write failure).
  void close() { server_.reset(); }

  template <typename Body> void serve(Body body) {
    thread_ = std::thread([this, body] { body(*this); });
  }

  ~RawPipeServer() {
    if (server_)
      server_->disconnect_client();
    if (thread_.joinable())
      thread_.join();
  }

private:
  std::unique_ptr<PipeServer> server_;
  std::thread thread_;
};

// ---------------------------------------------------------------------------
// Tiny-buffer raw pipe server.
//
// One request frame is far larger than this server's inbound buffer, so the
// client's frame is accepted only partially: that is the state in which a write
// failure can no longer prove that zero bytes reached the transport. Deliberately
// minimal — a test seam for the write-certainty classification, not a transport
// fixture.
// ---------------------------------------------------------------------------

constexpr DWORD kTinyInBuffer = 8;

class TinyBufferPipeServer {
public:
  explicit TinyBufferPipeServer(std::wstring pipe_name) : pipe_name_(std::move(pipe_name)) {}

  bool create() {
    handle_ = CreateNamedPipeW(pipe_name_.c_str(), PIPE_ACCESS_DUPLEX,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096,
                               kTinyInBuffer, 0, nullptr);
    return handle_ != INVALID_HANDLE_VALUE;
  }

  template <typename Body> void serve(Body body) {
    thread_ = std::thread([this, body] { body(*this); });
  }

  // Reads the Hello frame and answers it, so the client's connect() succeeds.
  bool answer_hello(std::uint64_t session_id) {
    std::uint8_t header[header_size];
    if (!read_exact(header, header_size))
      return false;
    std::uint32_t length = 0;
    std::memcpy(&length, header, sizeof(length));
    if (length == 0 || length > max_message_payload ||
        header[4] != static_cast<std::uint8_t>(MessageType::HelloRequest))
      return false;
    std::vector<std::uint8_t> hello(length);
    if (!read_exact(hello.data(), length))
      return false;
    WireFrame response;
    response.type = MessageType::HelloResponse;
    detail::write_u64(response.payload, kTestIncarnation);
    detail::write_u32(response.payload, protocol_version);
    detail::write_u64(response.payload, session_id);
    const std::vector<std::uint8_t> wire = frame_message(response.type, response.payload);
    return write_all(wire.data(), wire.size());
  }

  // Bytes the client pushed that this server never consumed: a non-zero value
  // proves that part of a frame was accepted by the transport.
  DWORD peek_available() {
    DWORD available = 0;
    if (handle_ == INVALID_HANDLE_VALUE)
      return 0;
    if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr))
      return 0;
    return available;
  }

  // Drops the connection while the client may still have bytes in flight.
  void abort_connection() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      DisconnectNamedPipe(handle_);
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

  ~TinyBufferPipeServer() {
    abort_connection();
    if (thread_.joinable())
      thread_.join();
  }

private:
  bool read_exact(void *data, std::size_t size) {
    auto *ptr = static_cast<std::uint8_t *>(data);
    std::size_t total = 0;
    while (total < size) {
      DWORD read = 0;
      if (!ReadFile(handle_, ptr + total, static_cast<DWORD>(size - total), &read, nullptr) ||
          read == 0)
        return false;
      total += read;
    }
    return true;
  }

  bool write_all(const void *data, std::size_t size) {
    const auto *ptr = static_cast<const std::uint8_t *>(data);
    std::size_t total = 0;
    while (total < size) {
      DWORD written = 0;
      if (!WriteFile(handle_, ptr + total, static_cast<DWORD>(size - total), &written, nullptr) ||
          written == 0)
        return false;
      total += written;
    }
    return true;
  }

  std::wstring pipe_name_;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  std::thread thread_;
};

// HostServer plus its accept-loop thread.
class RunningHost {
public:
  RunningHost(std::unique_ptr<Backend> backend, std::wstring pipe_name)
      : server_(std::move(backend), std::move(pipe_name)) {}

  bool start() {
    if (!server_.start())
      return false;
    thread_ = std::thread([this] { server_.run(); });
    return true;
  }

  HostServer &server() { return server_; }

  void stop_and_join() {
    server_.stop();
    if (thread_.joinable())
      thread_.join();
  }

  ~RunningHost() { stop_and_join(); }

private:
  HostServer server_;
  std::thread thread_;
};

// ---------------------------------------------------------------------------
// Layer 1: Protocol unit tests
// ---------------------------------------------------------------------------

void test_protocol() {
  // Binding round-trip
  {
    Binding b{100, 2, 3};
    std::vector<std::uint8_t> buf;
    serialize(buf, b);
    auto decoded = deserialize_binding(buf.data(), buf.size());
    require(decoded && *decoded == b, "binding round-trip");
  }

  // Binding trailing byte rejection
  {
    Binding b{100, 2, 3};
    std::vector<std::uint8_t> buf;
    serialize(buf, b);
    buf.push_back(0xFF); // trailing byte
    auto decoded = deserialize_binding(buf.data(), buf.size());
    require(!decoded, "binding trailing byte rejected");
  }

  // BackendStimulus round-trip
  {
    BackendStimulus s;
    s.binding = {42, 1, 1};
    s.stimulus_id = 7;
    s.type = BackendStimulus::Type::Character;
    s.character = 'a';
    s.keysym = 0x61;
    s.modifiers = 0;
    std::vector<std::uint8_t> buf;
    serialize(buf, s);
    auto decoded = deserialize_stimulus(buf.data(), buf.size());
    require(decoded && decoded->binding == s.binding && decoded->stimulus_id == s.stimulus_id &&
                decoded->type == s.type && decoded->character == s.character &&
                decoded->keysym == s.keysym && decoded->modifiers == s.modifiers,
            "stimulus round-trip");
  }

  // Stimulus trailing byte rejection
  {
    BackendStimulus s;
    s.binding = {1, 1, 1};
    s.stimulus_id = 1;
    s.type = BackendStimulus::Type::Character;
    s.character = 'a';
    s.keysym = 0x61;
    s.modifiers = 0;
    std::vector<std::uint8_t> buf;
    serialize(buf, s);
    buf.push_back(0xFF);
    auto decoded = deserialize_stimulus(buf.data(), buf.size());
    require(!decoded, "stimulus trailing byte rejected");
  }

  // BackendBatch round-trip with multiple operations
  {
    BackendBatch batch;
    batch.backend_session_id = 99;
    batch.binding = {10, 1, 1};
    batch.stimulus_id = 5;
    batch.disposition = BackendBatch::Disposition::Consumed;
    batch.operations.push_back({BackendOperation::Type::CommitText, "hello", {}, false});
    batch.operations.push_back({BackendOperation::Type::SetPreedit, "pre", {}, false});
    batch.operations.push_back({BackendOperation::Type::ForwardKey, "Tab", {}, true});
    std::vector<std::uint8_t> buf;
    serialize(buf, batch);
    auto decoded = deserialize_batch(buf.data(), buf.size());
    require(decoded && decoded->backend_session_id == batch.backend_session_id &&
                decoded->binding == batch.binding && decoded->stimulus_id == batch.stimulus_id &&
                decoded->disposition == batch.disposition && decoded->operations.size() == 3 &&
                decoded->operations[0].type == BackendOperation::Type::CommitText &&
                decoded->operations[0].text == "hello" &&
                decoded->operations[1].type == BackendOperation::Type::SetPreedit &&
                decoded->operations[1].text == "pre" &&
                decoded->operations[2].type == BackendOperation::Type::ForwardKey &&
                decoded->operations[2].original_key == true,
            "batch round-trip");
  }

  // Batch trailing byte rejection
  {
    BackendBatch batch;
    batch.backend_session_id = 1;
    batch.binding = {1, 1, 1};
    batch.stimulus_id = 1;
    batch.disposition = BackendBatch::Disposition::Forward;
    std::vector<std::uint8_t> buf;
    serialize(buf, batch);
    buf.push_back(0xFF);
    auto decoded = deserialize_batch(buf.data(), buf.size());
    require(!decoded, "batch trailing byte rejected");
  }

  // Batch with a candidate page snapshot
  {
    BackendBatch batch;
    batch.backend_session_id = 1;
    batch.binding = {1, 1, 1};
    batch.stimulus_id = 1;
    batch.disposition = BackendBatch::Disposition::Consumed;
    BackendOperation op;
    op.type = BackendOperation::Type::CandidateSnapshot;
    CandidatePage page;
    page.cursor_index = 1;
    page.layout = CandidateLayout::Horizontal;
    page.has_next = true;
    CandidateEntry a;
    a.label = "1";
    a.text = "a";
    CandidateEntry b;
    b.label = "2";
    b.text = "b";
    b.comment = "cmt";
    b.placeholder = true;
    page.entries = {a, b};
    op.page = page;
    batch.operations.push_back(op);
    std::vector<std::uint8_t> buf;
    serialize(buf, batch);
    auto decoded = deserialize_batch(buf.data(), buf.size());
    require(decoded && decoded->operations.size() == 1 && decoded->operations[0].page &&
                decoded->operations[0].page->entries.size() == 2 &&
                decoded->operations[0].page->entries[0].text == "a" &&
                decoded->operations[0].page->entries[1].placeholder &&
                decoded->operations[0].page->entries[1].comment == "cmt" &&
                decoded->operations[0].page->cursor_index == 1 &&
                decoded->operations[0].page->layout == CandidateLayout::Horizontal &&
                decoded->operations[0].page->has_next,
            "batch with candidate page round-trip");
  }

  // Malformed candidate page: cursor beyond the entry count is fail-closed.
  {
    std::vector<std::uint8_t> buf;
    detail::write_u64(buf, 1); // session
    detail::write_binding(buf, Binding{1, 1, 1});
    detail::write_u64(buf, 1); // stimulus
    detail::write_u8(buf, static_cast<std::uint8_t>(BackendBatch::Disposition::Consumed));
    detail::write_u32(buf, 1); // one operation
    detail::write_u8(buf, static_cast<std::uint8_t>(BackendOperation::Type::CandidateSnapshot));
    detail::write_string(buf, std::string()); // op text
    detail::write_u32(buf, 1);                // page entry count
    detail::write_string(buf, "1");           // label
    detail::write_string(buf, "x");           // text
    detail::write_string(buf, "");            // comment
    detail::write_u8(buf, 0);                 // placeholder
    detail::write_u32(buf, 5);                // cursor_index 5 with 1 entry -> invalid
    detail::write_u8(buf, 0);                 // layout
    detail::write_u8(buf, 0);                 // has_prev
    detail::write_u8(buf, 0);                 // has_next
    detail::write_u8(buf, 0);                 // truncated
    detail::write_string(buf, "");            // aux_up
    detail::write_string(buf, "");            // panel_preedit
    detail::write_string(buf, "");            // aux_down
    detail::write_u8(buf, 0);                 // original_key
    auto decoded = deserialize_batch(buf.data(), buf.size());
    require(!decoded, "candidate page cursor-out-of-range fail-closed");
  }

  // Stage 5 closure: the displayed preedit's fcitx::Text contract and the panel
  // rows round-trip exactly; an absent projection stays absent.
  {
    BackendBatch batch;
    batch.backend_session_id = 1;
    batch.binding = {1, 1, 1};
    batch.stimulus_id = 1;
    batch.disposition = BackendBatch::Disposition::Consumed;
    BackendOperation styled{BackendOperation::Type::SetPreedit, "\xE4\xBD\xA0hao"};
    styled.preedit = FormattedPreedit{
        {{3, text_format::highlight | text_format::underline}, {3, text_format::underline}}, 3};
    batch.operations.push_back(styled);
    batch.operations.push_back({BackendOperation::Type::SetPreedit, "plain", {}, false});
    BackendOperation panel;
    panel.type = BackendOperation::Type::CandidateSnapshot;
    CandidatePage page;
    page.aux_up = "[Stroke Filtering]h";
    page.panel_preedit = "ni hao";
    page.aux_down = "down";
    panel.page = page;
    batch.operations.push_back(panel);
    std::vector<std::uint8_t> buf;
    serialize(buf, batch);
    auto decoded = deserialize_batch(buf.data(), buf.size());
    require(decoded && decoded->operations.size() == 3, "preedit/panel batch decoded");
    const auto &p0 = decoded->operations[0];
    require(p0.preedit && p0.preedit->cursor == 3 && p0.preedit->segments.size() == 2 &&
                p0.preedit->segments[0].bytes == 3 &&
                p0.preedit->segments[0].format ==
                    (text_format::highlight | text_format::underline) &&
                p0.preedit->segments[1].bytes == 3 &&
                p0.preedit->segments[1].format == text_format::underline,
            "preedit segments/format/cursor round-trip");
    require(!decoded->operations[1].preedit, "absent preedit projection stays absent");
    const auto &p2 = decoded->operations[2];
    require(p2.page && p2.page->aux_up == "[Stroke Filtering]h" &&
                p2.page->panel_preedit == "ni hao" && p2.page->aux_down == "down" &&
                p2.page->entries.empty(),
            "panel rows round-trip");
  }

  // Preedit projection that does not tile its text, or whose cursor is past
  // it, or that carries unknown format bits, is fail-closed.
  {
    const auto encode = [](const FormattedPreedit &format, const std::string &text) {
      BackendBatch batch;
      batch.backend_session_id = 1;
      batch.binding = {1, 1, 1};
      batch.stimulus_id = 1;
      batch.disposition = BackendBatch::Disposition::Consumed;
      BackendOperation op{BackendOperation::Type::SetPreedit, text};
      op.preedit = format;
      batch.operations.push_back(op);
      std::vector<std::uint8_t> buf;
      serialize(buf, batch);
      return buf;
    };
    auto short_tiling = encode(FormattedPreedit{{{2, 0}}, -1}, "abc");
    require(!deserialize_batch(short_tiling.data(), short_tiling.size()),
            "preedit segments not tiling the text fail-closed");
    auto past_end = encode(FormattedPreedit{{{3, 0}}, 4}, "abc");
    require(!deserialize_batch(past_end.data(), past_end.size()),
            "preedit cursor past the text fail-closed");
    auto bad_flags = encode(FormattedPreedit{{{3, 1u << 12}}, -1}, "abc");
    require(!deserialize_batch(bad_flags.data(), bad_flags.size()),
            "preedit unknown format bits fail-closed");
  }

  // Key stimulus carrying a candidate index is fail-closed.
  {
    BackendStimulus s;
    s.binding = {1, 1, 1};
    s.stimulus_id = 1;
    s.type = BackendStimulus::Type::Character;
    s.character = 'a';
    s.keysym = 0x61;
    s.candidate_index = 3; // illegal for a key stimulus
    std::vector<std::uint8_t> buf;
    serialize(buf, s);
    auto decoded = deserialize_stimulus(buf.data(), buf.size());
    require(!decoded, "key stimulus with candidate index fail-closed");
  }

  // Bool round-trip
  {
    std::vector<std::uint8_t> buf_true, buf_false;
    serialize(buf_true, true);
    serialize(buf_false, false);
    auto t = deserialize_bool(buf_true.data(), buf_true.size());
    auto f = deserialize_bool(buf_false.data(), buf_false.size());
    require(t && *t == true && f && *f == false, "bool round-trip");
  }

  // Bool trailing byte rejection
  {
    std::vector<std::uint8_t> buf;
    serialize(buf, true);
    buf.push_back(0xFF);
    auto decoded = deserialize_bool(buf.data(), buf.size());
    require(!decoded, "bool trailing byte rejected");
  }

  // u64 round-trip
  {
    std::vector<std::uint8_t> buf;
    serialize(buf, std::uint64_t{0xDEADBEEFCAFE1234ULL});
    auto v = deserialize_u64(buf.data(), buf.size());
    require(v && *v == 0xDEADBEEFCAFE1234ULL, "u64 round-trip");
  }

  // u64 trailing byte rejection
  {
    std::vector<std::uint8_t> buf;
    serialize(buf, std::uint64_t{42});
    buf.push_back(0xFF);
    auto decoded = deserialize_u64(buf.data(), buf.size());
    require(!decoded, "u64 trailing byte rejected");
  }

  // Framing round-trip
  {
    std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
    auto frame = frame_message(MessageType::SendRequest, payload);
    require(frame.size() == header_size + payload.size(), "frame size");
    auto parsed = parse_header(frame.data(), frame.size());
    require(parsed && parsed->type == MessageType::SendRequest && parsed->payload == payload,
            "frame parse");
  }

  // Bound enforcement: oversized payload in header
  {
    std::vector<std::uint8_t> frame(header_size);
    std::uint32_t huge = static_cast<std::uint32_t>(max_message_payload + 1);
    std::memcpy(frame.data(), &huge, 4);
    frame[4] = static_cast<std::uint8_t>(MessageType::SendRequest);
    auto parsed = parse_header(frame.data(), frame.size());
    require(!parsed, "oversized payload rejected");
  }

  // Bound enforcement: invalid message type
  {
    std::vector<std::uint8_t> frame(header_size);
    std::uint32_t zero = 0;
    std::memcpy(frame.data(), &zero, 4);
    frame[4] = 0xFF;
    auto parsed = parse_header(frame.data(), frame.size());
    require(!parsed, "invalid message type rejected");
  }

  // Bound enforcement: incomplete frame (header says more payload than available)
  {
    std::vector<std::uint8_t> frame(header_size + 2);
    std::uint32_t len = 100;
    std::memcpy(frame.data(), &len, 4);
    frame[4] = static_cast<std::uint8_t>(MessageType::SendRequest);
    auto parsed = parse_header(frame.data(), frame.size());
    require(!parsed, "incomplete frame rejected");
  }

  // Truncated message
  {
    auto decoded = deserialize_binding(nullptr, 0);
    require(!decoded, "empty binding rejected");
  }
  {
    std::vector<std::uint8_t> buf(10, 0);
    auto decoded = deserialize_stimulus(buf.data(), buf.size());
    require(!decoded, "truncated stimulus rejected");
  }

  // Oversized string in deserialization
  {
    std::vector<std::uint8_t> buf;
    detail::write_u32(buf, static_cast<std::uint32_t>(max_string_length + 1));
    buf.insert(buf.end(), 100, 'x');
    detail::Reader r{buf.data(), buf.size()};
    auto s = r.read_string();
    require(!s, "oversized string rejected");
  }

  // Invalid enum value in stimulus: rebuild the payload with an out-of-range
  // type byte. Constructed field by field with the same writers as the
  // serializer, because the wire format is UNFROZEN and a hard-coded byte
  // offset silently stops testing the type field when a field is added.
  {
    std::vector<std::uint8_t> buf;
    detail::write_binding(buf, Binding{1, 1, 1});
    detail::write_u64(buf, 1);
    detail::write_u8(buf, 0xFF); // invalid BackendStimulus::Type
    detail::write_u8(buf, static_cast<std::uint8_t>('a'));
    detail::write_u32(buf, 0x61);
    detail::write_u32(buf, 0);
    auto decoded = deserialize_stimulus(buf.data(), buf.size());
    require(!decoded, "invalid stimulus type rejected");
  }

  // Settings schema/value trees are opaque transport data derived at runtime
  // from Fcitx; nested values and property maps round-trip without a second schema.
  {
    SettingsSnapshot snapshot;
    snapshot.values.entries.emplace("PageSize", SettingsValue::string("7"));
    auto nested = SettingsValue::object();
    nested.entries.emplace("Fuzzy", SettingsValue::string("True"));
    snapshot.values.entries.emplace("Nested", std::move(nested));
    SettingsConfigType type;
    type.name = "PinyinEngineConfig";
    SettingsOptionDescription option;
    option.name = "PageSize";
    option.type = "Integer";
    option.description = "Candidates per page";
    option.default_value = SettingsValue::string("5");
    option.properties.entries.emplace("Min", SettingsValue::string("3"));
    type.options.push_back(std::move(option));
    snapshot.types.push_back(std::move(type));
    std::vector<std::uint8_t> payload;
    require(settings_detail::write_snapshot(payload, snapshot), "settings snapshot serializes");
    detail::Reader reader{payload.data(), payload.size()};
    SettingsSnapshot decoded;
    require(settings_detail::read_snapshot(reader, decoded) && reader.at_end() &&
                decoded.values == snapshot.values && decoded.types.size() == 1 &&
                decoded.types[0].name == "PinyinEngineConfig" &&
                decoded.types[0].options.size() == 1 &&
                decoded.types[0].options[0].properties.entries.at("Min").scalar == "3",
            "settings snapshot values/schema/properties round-trip");
  }

  // Duplicate map keys are malformed, not last-writer-wins.
  {
    std::vector<std::uint8_t> payload;
    detail::write_u8(payload, 1); // map
    detail::write_u32(payload, 2);
    detail::write_string(payload, "PageSize");
    detail::write_u8(payload, 0);
    detail::write_string(payload, "7");
    detail::write_string(payload, "PageSize");
    detail::write_u8(payload, 0);
    detail::write_string(payload, "9");
    detail::Reader reader{payload.data(), payload.size()};
    std::size_t nodes = 0;
    require(!settings_detail::read_value(reader, 0, nodes),
            "duplicate settings keys fail-closed");
  }

  // Additive settings message types are framed under v4 without changing the TIP handshake.
  {
    const auto frame = frame_message(MessageType::SettingsHelloRequest,
                                     serialize_settings_hello(protocol_version,
                                                              settings_api_version));
    auto parsed = parse_header(frame.data(), frame.size());
    require(parsed && parsed->type == MessageType::SettingsHelloRequest,
            "settings capability frame is accepted by the v4 parser");
    require(protocol_version == 4,
            "settings messages do not bump the existing TIP protocol version");
  }

  std::cout << "PASS protocol serialization, bounds, trailing bytes, and malformed input\n";
}

// ---------------------------------------------------------------------------
// Layer 2: Loopback pipe test with CallbackBackend
// ---------------------------------------------------------------------------

class CallbackBackend final : public Backend {
public:
  BackendBatch next;
  std::uint64_t sid = 42;
  std::atomic<int> bind_count{0};
  std::atomic<int> unbind_count{0};

  bool bind(Binding) override {
    bind_count++;
    return true;
  }
  bool reset(Binding) override { return true; }
  void unbind(Binding) override { unbind_count++; }
  bool healthy() const override { return true; }
  std::uint64_t session_id() const override { return sid; }
  BackendBatch send(const BackendStimulus &input) override {
    auto batch = next;
    batch.backend_session_id = sid;
    batch.binding = input.binding;
    batch.stimulus_id = input.stimulus_id;
    return batch;
  }
};

void run_loopback_server(PipeServer &server, CallbackBackend &backend, std::atomic<bool> &done) {
  // Accept one client, then wait for Hello.
  if (!server.wait_for_client(std::chrono::milliseconds(10000))) {
    done = true;
    return;
  }
  auto hello = server.recv_frame(std::chrono::milliseconds(10000));
  if (!hello || hello->type != MessageType::HelloRequest) {
    done = true;
    return;
  }
  // Respond with Hello.
  WireFrame hello_resp;
  hello_resp.type = MessageType::HelloResponse;
  detail::write_u64(hello_resp.payload, kTestIncarnation);
  detail::write_u32(hello_resp.payload, protocol_version);
  detail::write_u64(hello_resp.payload, backend.session_id());
  if (!server.send_frame(hello_resp)) {
    done = true;
    return;
  }

  while (done.load() == false && server.client_connected()) {
    auto frame = server.recv_frame(std::chrono::milliseconds(0)); // block indefinitely
    if (!frame)
      break;
    WireFrame response;
    switch (frame->type) {
    case MessageType::BindRequest: {
      auto binding = deserialize_binding(frame->payload.data(), frame->payload.size());
      response.type = MessageType::BindResponse;
      serialize(response.payload, binding ? backend.bind(*binding) : false);
      break;
    }
    case MessageType::ResetRequest: {
      auto binding = deserialize_binding(frame->payload.data(), frame->payload.size());
      response.type = MessageType::ResetResponse;
      serialize(response.payload, binding ? backend.reset(*binding) : false);
      break;
    }
    case MessageType::UnbindRequest: {
      auto binding = deserialize_binding(frame->payload.data(), frame->payload.size());
      if (binding)
        backend.unbind(*binding);
      response.type = MessageType::UnbindResponse;
      break;
    }
    case MessageType::SendRequest: {
      auto stimulus = deserialize_stimulus(frame->payload.data(), frame->payload.size());
      response.type = MessageType::SendResponse;
      if (stimulus) {
        auto batch = backend.send(*stimulus);
        serialize(response.payload, batch);
      } else {
        BackendBatch rejected;
        rejected.disposition = BackendBatch::Disposition::Reject;
        serialize(response.payload, rejected);
      }
      break;
    }
    default:
      done = true;
      return;
    }
    // Stage 4B: every response payload carries the responding incarnation.
    response.payload = envelop_response(kTestIncarnation, response.payload);
    if (!server.send_frame(response))
      break;
  }
  done = true;
}

KeyAction physical(char character) {
  return translate_key(static_cast<unsigned char>(character), false, false, false, character);
}

// ---------------------------------------------------------------------------
// Layer 1b: the settings endpoint name
// ---------------------------------------------------------------------------
//
// FusionSettings hands QLocalSocket::connectToServer() a *logical* pipe name; Qt
// prepends `\\.\pipe\` itself (qtbase v6.8.3 qlocalsocket_win.cpp:173-177, and
// qlocalserver_win.cpp:209-213 on the server end). This exercises the same
// fusion::ipc::logical_pipe_name() the product calls, so a regression in the
// conversion fails here rather than in an installed window.
void test_pipe_endpoint() {
  const std::wstring device(fusion::ipc::pipe_device_path_prefix);
  require(device == L"\\\\.\\pipe\\", "device path prefix is the Win32 pipe path");

  // 1. Default startup keeps the product identity and round-trips exactly to the
  //    endpoint compute_pipe_name() creates for the Host.
  {
    const std::wstring production = fusion::ipc::compute_pipe_name();
    require(!production.empty(), "compute_pipe_name() yields an endpoint");
    require(production.rfind(device, 0) == 0, "compute_pipe_name() returns a device path");
    const std::wstring logical = fusion::ipc::logical_pipe_name(production);
    require(!logical.empty(), "the production endpoint converts to a logical name");
    require(logical == production.substr(device.size()),
            "the default conversion removes the device path only");
    require(logical.rfind(L"FusionHost-v1-", 0) == 0,
            "the default conversion retains the product pipe identity");
    require(device + logical == production,
            "Qt re-adding the device path reaches the Host endpoint exactly");
  }

  // 2. An explicit endpoint keeps its complete name, whichever accepted form the
  //    caller used (the isolated runner passes the device path).
  {
    const std::wstring isolated = L"fusion-selftest-8f2c";
    require(fusion::ipc::logical_pipe_name(device + isolated) == isolated,
            "an explicit device-path endpoint keeps its complete name");
    require(fusion::ipc::logical_pipe_name(isolated) == isolated,
            "an explicit logical endpoint keeps its complete name");
    const std::wstring production_shaped = L"FusionHost-v1-S-1-5-21-1-1";
    require(fusion::ipc::logical_pipe_name(device + production_shaped) == production_shaped,
            "an explicit production-shaped endpoint keeps the product identity");
  }

  // 3. Empty and malformed endpoints are rejected, never rewritten into another
  //    endpoint. The caller fails closed on the empty result.
  {
    require(fusion::ipc::logical_pipe_name(L"").empty(), "an empty endpoint is rejected");
    require(fusion::ipc::logical_pipe_name(device).empty(),
            "a bare device path with no name is rejected");
    require(fusion::ipc::logical_pipe_name(L"   ").empty(), "a whitespace endpoint is rejected");
    require(fusion::ipc::logical_pipe_name(device + L"   ").empty(),
            "a whitespace-only name after the device path is rejected");
    require(fusion::ipc::logical_pipe_name(L"fusion\\nested").empty(),
            "a nested path is rejected");
    require(fusion::ipc::logical_pipe_name(L"fusion:bad").empty(),
            "a name Win32 rejects is rejected");
    require(fusion::ipc::logical_pipe_name(L"fusion\nbad").empty(),
            "a control character is rejected");
    require(fusion::ipc::logical_pipe_name(L"\\\\?\\pipe\\fusion").empty(),
            "an unsupported device path form is rejected");
  }
}

void test_loopback() {
  // Basic send/recv with Hello handshake
  {
    std::wstring pipe_name = L"\\\\.\\pipe\\FusionHost-ipc-test-loopback-basic";
    RawPipeServer server(pipe_name);
    require(server.create(), "loopback server create");

    CallbackBackend backend;
    backend.next.disposition = BackendBatch::Disposition::Consumed;
    backend.next.operations = {{BackendOperation::Type::SetPreedit, "ni", {}, false}};

    std::atomic<bool> done{false};
    server.serve([&](RawPipeServer &s) { run_loopback_server(s.server(), backend, done); });

    IpcBackend client(pipe_name);
    require(client.connect(std::chrono::milliseconds(5000)), "loopback client connect");
    require(client.session_id() == 42, "loopback session_id");
    require(client.healthy(), "loopback healthy");

    Binding binding{1, 1, 1};
    require(client.bind(binding), "loopback bind");

    BackendStimulus stimulus;
    stimulus.binding = binding;
    stimulus.stimulus_id = 1;
    stimulus.type = BackendStimulus::Type::Character;
    stimulus.character = 'n';
    stimulus.keysym = 'n';
    auto batch = client.send(stimulus);
    require(batch.backend_session_id == 42, "loopback batch session_id");
    require(batch.binding == binding, "loopback batch binding");
    require(batch.stimulus_id == 1, "loopback batch stimulus_id");
    require(batch.disposition == BackendBatch::Disposition::Consumed, "loopback batch disposition");
    require(batch.operations.size() == 1, "loopback batch ops count");
    require(batch.operations[0].type == BackendOperation::Type::SetPreedit, "loopback op type");
    require(batch.operations[0].text == "ni", "loopback op text");

    client.unbind(binding);
    done = true; // the fixture disconnects and joins on scope exit
  }

  // healthy() is local state only — no IPC
  {
    std::wstring pipe_name = L"\\\\.\\pipe\\FusionHost-ipc-test-healthy";
    RawPipeServer server(pipe_name);
    require(server.create(), "healthy server create");

    CallbackBackend backend;
    std::atomic<bool> done{false};
    server.serve([&](RawPipeServer &s) { run_loopback_server(s.server(), backend, done); });

    IpcBackend client(pipe_name);
    require(client.connect(std::chrono::milliseconds(5000)), "healthy client connect");

    // healthy() should return true without sending any IPC.
    require(client.healthy(), "healthy after connect");
    // Call it again — still no IPC.
    require(client.healthy(), "healthy repeated");

    done = true;
  }

  // 6s idle success — server should not disconnect on idle
  {
    std::wstring pipe_name = L"\\\\.\\pipe\\FusionHost-ipc-test-idle";
    RawPipeServer server(pipe_name);
    require(server.create(), "idle server create");

    CallbackBackend backend;
    std::atomic<bool> done{false};
    server.serve([&](RawPipeServer &s) { run_loopback_server(s.server(), backend, done); });

    IpcBackend client(pipe_name);
    require(client.connect(std::chrono::milliseconds(5000)), "idle client connect");

    // Wait 6 seconds — the server should not disconnect us.
    std::this_thread::sleep_for(std::chrono::seconds(6));

    // We should still be connected and healthy.
    require(client.healthy(), "healthy after 6s idle");

    // Send a request to verify the connection is still working.
    Binding binding{1, 1, 1};
    require(client.bind(binding), "bind after 6s idle");

    done = true;
  }

  // Two concurrent clients, each on its own pipe instance and backend
  {
    std::wstring pipe_name = L"\\\\.\\pipe\\FusionHost-ipc-test-concurrent";

    CallbackBackend backend1, backend2;
    backend1.sid = 100;
    backend2.sid = 200;
    backend1.next.disposition = BackendBatch::Disposition::Consumed;
    backend1.next.operations = {{BackendOperation::Type::SetPreedit, "one", {}, false}};
    backend2.next.disposition = BackendBatch::Disposition::Consumed;
    backend2.next.operations = {{BackendOperation::Type::SetPreedit, "two", {}, false}};
    std::atomic<bool> done1{false}, done2{false};

    RawPipeServer server1(pipe_name); // first instance
    require(server1.create(), "concurrent server1 create");
    RawPipeServer server2(pipe_name); // second instance
    require(server2.create(), "concurrent server2 create");

    server1.serve([&](RawPipeServer &s) { run_loopback_server(s.server(), backend1, done1); });
    server2.serve([&](RawPipeServer &s) { run_loopback_server(s.server(), backend2, done2); });

    IpcBackend client1(pipe_name);
    require(client1.connect(std::chrono::milliseconds(5000)), "concurrent client1 connect");
    IpcBackend client2(pipe_name);
    require(client2.connect(std::chrono::milliseconds(5000)), "concurrent client2 connect");

    // Which instance a client lands on is not ordered by Windows, so the
    // invariant is that both instances are used and neither client shares
    // another client's backend state.
    const auto session1 = client1.session_id();
    const auto session2 = client2.session_id();
    require((session1 == 100 || session1 == 200) && (session2 == 100 || session2 == 200) &&
                session1 != session2,
            "each concurrent client is served by its own server instance");
    require(client1.healthy() && client2.healthy(), "concurrent clients healthy");

    const auto batch1 = client1.send(character_stimulus({1, 1, 1}, 1, 'a'));
    const auto batch2 = client2.send(character_stimulus({1, 1, 1}, 1, 'a'));
    require(batch1.backend_session_id == session1 && batch2.backend_session_id == session2,
            "concurrent clients keep their own session id");
    require(batch1.operations.size() == 1 && batch2.operations.size() == 1,
            "concurrent clients receive a response each");
    require(batch1.operations[0].text != batch2.operations[0].text,
            "concurrent clients are backed by independent backends");

    done1 = true;
    done2 = true;
  }

  // Pre-write failure → Forward with session_id, binding, stimulus_id, no ops
  {
    // Pre-write: client not connected.
    {
      IpcBackend client(L"\\\\.\\pipe\\FusionHost-ipc-test-nonexistent");
      // Don't connect — send should return Forward with fields populated.
      BackendStimulus stimulus;
      stimulus.binding = {42, 7, 3};
      stimulus.stimulus_id = 99;
      stimulus.type = BackendStimulus::Type::Character;
      stimulus.character = 'a';
      auto batch = client.send(stimulus);
      require(batch.disposition == BackendBatch::Disposition::Forward,
              "pre-write failure → Forward");
      require(batch.operations.empty(), "pre-write failure → no ops");
      require(batch.binding == stimulus.binding, "pre-write failure → binding preserved");
      require(batch.stimulus_id == stimulus.stimulus_id,
              "pre-write failure → stimulus_id preserved");
      // session_id is 0 because connect was never performed (cached_session_id_ = 0).
      require(batch.backend_session_id == 0,
              "pre-write failure → session_id is 0 (never connected)");
    }
  }

  // Version mismatch
  {
    std::wstring pipe_name = L"\\\\.\\pipe\\FusionHost-ipc-test-version";
    RawPipeServer server(pipe_name);
    require(server.create(), "version server create");

    server.serve([&](RawPipeServer &s) {
      if (!s.server().wait_for_client(kWait))
        return;
      auto hello = s.server().recv_frame(std::chrono::milliseconds(5000));
      if (!hello || hello->type != MessageType::HelloRequest)
        return;
      detail::Reader r(hello->payload.data(), hello->payload.size());
      auto client_version = r.read_u32();
      (void)client_version;
      // Respond with version=0 (mismatch), still under the Stage 4B envelope.
      WireFrame resp;
      resp.type = MessageType::HelloResponse;
      detail::write_u64(resp.payload, kTestIncarnation);
      detail::write_u32(resp.payload, 0);
      detail::write_u64(resp.payload, 0);
      s.server().send_frame(resp);
    });

    // Send a Hello with wrong version.
    PipeClient raw_client(pipe_name);
    require(raw_client.connect(std::chrono::milliseconds(5000)), "version client connect");
    std::vector<std::uint8_t> hello_payload;
    detail::write_u32(hello_payload, 999); // wrong version
    require(raw_client.send_request(MessageType::HelloRequest, hello_payload) ==
                WriteOutcome::Written,
            "version hello send");
    auto resp = raw_client.recv_response(std::chrono::milliseconds(5000));
    require(resp && resp->type == MessageType::HelloResponse, "version response received");
    detail::Reader r2(resp->payload.data(), resp->payload.size());
    auto envelope_incarnation = r2.read_u64();
    auto server_version = r2.read_u32();
    require(envelope_incarnation && *envelope_incarnation == kTestIncarnation,
            "every response carries the Host incarnation envelope");
    require(server_version && *server_version == 0, "version mismatch → version=0");
  }

  // SemanticController over IPC loopback
  {
    RawPipeServer server(L"\\\\.\\pipe\\FusionHost-ipc-test-semantic");
    require(server.create(), "semantic server create");

    CallbackBackend backend;
    backend.next.disposition = BackendBatch::Disposition::Consumed;
    backend.next.operations = {{BackendOperation::Type::SetPreedit, "nihao", {}, false}};

    std::atomic<bool> done{false};
    server.serve([&](RawPipeServer &s) { run_loopback_server(s.server(), backend, done); });

    IpcBackend client(L"\\\\.\\pipe\\FusionHost-ipc-test-semantic");
    require(client.connect(std::chrono::milliseconds(5000)), "semantic client connect");

    SemanticController controller(900, 64, static_cast<Backend &>(client));
    auto d = controller.begin_key(physical('n'), true);
    require(d.eaten, "semantic loopback eaten");
    require(d.route == SemanticDecision::Route::State, "semantic loopback route");
    require(d.text == "nihao", "semantic loopback text");

    controller.destroy();
    done = true;
  }

  std::cout << "PASS loopback pipe with Hello, idle, concurrent clients, transport failure, "
               "version mismatch\n";
}

// ---------------------------------------------------------------------------
// Layer 2B: HostServer tests with the real HostServer + fake Backend
// ---------------------------------------------------------------------------

namespace {

// Fake backend that mimics the real FcitxBackend's admission constraints:
//  * a context can be bound only once (a duplicate bind is rejected);
//  * every call must take the admission lock with try_lock, so a contended call
//    FAILS instead of queueing.
// HostServer must serialize backend calls and remap client contexts, so no call
// ever contends, and each connection keeps its own Host context.
class FcitxLikeBackend final : public Backend {
public:
  std::uint64_t sid = 555;
  std::atomic<int> call_count{0};
  std::atomic<int> contention_count{0};
  std::atomic<bool> composition_active{false};
  std::atomic<int> settings_set_count{0};
  SettingsSnapshot settings_snapshot;
  SettingsSelection selection_state{"pinyin", {"pinyin", "shuangpin"}};

  FcitxLikeBackend() {
    settings_snapshot.values = SettingsValue::object();
    settings_snapshot.values.entries.emplace("PageSize", SettingsValue::string("7"));
    settings_snapshot.values.entries.emplace("ShuangpinProfile", SettingsValue::string("Ziranma"));
    settings_snapshot.values.entries.emplace("SpellEnabled", SettingsValue::string("true"));
    settings_snapshot.values.entries.emplace("FutureOption", SettingsValue::string("keep-me"));
    SettingsConfigType type;
    type.name = "PinyinEngineConfig";
    for (const auto &[name, defaultValue] :
         {std::pair{"PageSize", "5"}, std::pair{"ShuangpinProfile", "Ziranma"},
          std::pair{"SpellEnabled", "true"}}) {
      SettingsOptionDescription option;
      option.name = name;
      option.type = name == std::string("PageSize") ? "Integer" : "Enum";
      option.description = name;
      option.default_value = SettingsValue::string(defaultValue);
      type.options.push_back(std::move(option));
    }
    settings_snapshot.types.push_back(std::move(type));
  }

  bool bind(Binding binding) override {
    call_count++;
    std::unique_lock<std::mutex> admission(admission_, std::try_to_lock);
    if (!admission) {
      contention_count++;
      return false;
    }
    std::lock_guard lock(state_);
    if (contexts_.count(binding.context))
      return false; // duplicate context
    contexts_.insert(binding.context);
    return true;
  }

  bool reset(Binding binding) override {
    call_count++;
    std::unique_lock<std::mutex> admission(admission_, std::try_to_lock);
    if (!admission) {
      contention_count++;
      return false;
    }
    std::lock_guard lock(state_);
    return contexts_.count(binding.context) != 0;
  }

  void unbind(Binding binding) override {
    call_count++;
    std::unique_lock<std::mutex> admission(admission_, std::try_to_lock);
    if (!admission) {
      contention_count++;
      return;
    }
    std::lock_guard lock(state_);
    contexts_.erase(binding.context);
    unbound_.push_back(binding.context);
  }

  bool healthy() const override { return true; }
  std::uint64_t session_id() const override { return sid; }

  BackendBatch send(const BackendStimulus &stimulus) override {
    call_count++;
    BackendBatch batch;
    std::unique_lock<std::mutex> admission(admission_, std::try_to_lock);
    if (!admission) {
      contention_count++;
      batch.disposition = BackendBatch::Disposition::Reject;
      return batch;
    }
    std::lock_guard lock(state_);
    if (!contexts_.count(stimulus.binding.context)) {
      batch.disposition = BackendBatch::Disposition::Reject;
      return batch;
    }
    batch.backend_session_id = sid;
    batch.binding = stimulus.binding;
    batch.stimulus_id = stimulus.stimulus_id;
    batch.disposition = BackendBatch::Disposition::Consumed;
    // The Host context that served the call travels back to the client, so the
    // test can verify per-connection identity without HostServer internals.
    batch.operations.push_back(
        {BackendOperation::Type::SetPreedit, std::to_string(stimulus.binding.context), {}, false});
    return batch;
  }

  SettingsStatus settings_get_config(const std::string &uri, SettingsSnapshot &snapshot,
                                    std::string &error) override {
    if (uri != "fcitx://config/inputmethod/pinyin" &&
        uri != "fcitx://config/inputmethod/shuangpin") {
      error = "unsupported URI";
      return SettingsStatus::Invalid;
    }
    std::lock_guard lock(settings_mutex_);
    snapshot = settings_snapshot;
    return SettingsStatus::Ok;
  }

  SettingsStatus settings_set_config(const std::string &uri, const SettingsValue &values,
                                     SettingsSnapshot &snapshot, std::string &error) override {
    if (uri != "fcitx://config/inputmethod/pinyin" &&
        uri != "fcitx://config/inputmethod/shuangpin") {
      error = "unsupported URI";
      return SettingsStatus::Invalid;
    }
    if (composition_active.load()) {
      error = "finish composition first";
      return SettingsStatus::Busy;
    }
    std::lock_guard lock(settings_mutex_);
    for (const auto &[key, value] : values.entries) {
      settings_snapshot.values.entries[key] = value;
    }
    snapshot = settings_snapshot;
    settings_set_count++;
    return SettingsStatus::Ok;
  }

  SettingsStatus settings_get_selection(SettingsSelection &selection,
                                        std::string &) override {
    std::lock_guard lock(settings_mutex_);
    selection = selection_state;
    return SettingsStatus::Ok;
  }

  SettingsStatus settings_select_input_method(const std::string &method,
                                              SettingsSelection &selection,
                                              std::string &error) override {
    if (composition_active.load()) {
      error = "finish composition first";
      return SettingsStatus::Busy;
    }
    std::lock_guard lock(settings_mutex_);
    if (std::find(selection_state.available.begin(), selection_state.available.end(), method) ==
        selection_state.available.end()) {
      error = "method unavailable";
      return SettingsStatus::Invalid;
    }
    selection_state.selected = method;
    selection = selection_state;
    return SettingsStatus::Ok;
  }

  std::size_t bound_count() {
    std::lock_guard lock(state_);
    return contexts_.size();
  }

  bool saw_unbind(std::uint64_t context) {
    std::lock_guard lock(state_);
    return std::find(unbound_.begin(), unbound_.end(), context) != unbound_.end();
  }

private:
  std::mutex admission_; // FcitxBackend admission lock: try_lock, never wait
  std::mutex state_;
  std::mutex settings_mutex_;
  std::set<std::uint64_t> contexts_;
  std::vector<std::uint64_t> unbound_;
};

// Two IpcBackend clients using the SAME client context id, binding and sending
// concurrently. The Host must give them distinct host contexts, serialize every
// backend call, and return each client its own binding.
void test_hostserver_multi_client() {
  const auto pipe_name = test_pipe_name(L"hostserver-multi");
  auto backend = std::make_unique<FcitxLikeBackend>();
  FcitxLikeBackend *fake = backend.get();
  RunningHost host(std::move(backend), pipe_name);
  require(host.start(), "hostserver start");

  auto client1 = connect_client(pipe_name);
  require(client1 != nullptr, "hostserver client1 connect");
  auto client2 = connect_client(pipe_name);
  require(client2 != nullptr, "hostserver client2 connect");
  require(client1->session_id() == fake->sid && client2->session_id() == fake->sid,
          "hostserver session id comes from the Hello handshake");

  const Binding client_binding{1, 1, 1}; // identical client-side context for both clients
  require(client1->bind(client_binding), "client1 bind");
  require(client2->bind(client_binding), "client2 bind");
  require(fake->bound_count() == 2, "each client got its own host context");

  constexpr int kIterations = 16;
  std::string host_context1;
  std::string host_context2;
  std::string error1;
  std::string error2;
  auto drive = [&](IpcBackend &client, std::string &host_context, std::string &error) {
    try {
      for (int i = 1; i <= kIterations; ++i) {
        const auto stimulus =
            character_stimulus(client_binding, static_cast<std::uint64_t>(i), 'a');
        const BackendBatch batch = client.send(stimulus);
        if (batch.disposition != BackendBatch::Disposition::Consumed ||
            batch.binding != client_binding || batch.stimulus_id != stimulus.stimulus_id ||
            batch.backend_session_id != fake->sid || batch.operations.size() != 1) {
          error = "concurrent send result mismatch";
          return;
        }
        host_context = batch.operations[0].text;
      }
    } catch (const std::exception &e) {
      error = e.what();
    }
  };
  std::thread t1(drive, std::ref(*client1), std::ref(host_context1), std::ref(error1));
  std::thread t2(drive, std::ref(*client2), std::ref(host_context2), std::ref(error2));
  t1.join();
  t2.join();
  require(error1.empty() && error2.empty(), "concurrent clients: every call succeeded");
  require(!host_context1.empty() && !host_context2.empty() && host_context1 != host_context2,
          "each client was served by its own host context");
  require(fake->contention_count == 0,
          "Host serialized every backend call (the fake fails on admission contention)");

  // Disconnecting one client must unbind exactly that client's host context.
  const auto host_context_a = static_cast<std::uint64_t>(std::stoull(host_context1));
  const auto host_context_b = static_cast<std::uint64_t>(std::stoull(host_context2));
  client1.reset(); // closing the pipe is what the Host observes as a disconnect
  require(wait_until([fake, host_context_a] { return fake->saw_unbind(host_context_a); }, kWait),
          "disconnected client's host context was unbound");
  require(!fake->saw_unbind(host_context_b), "the other client's host context was untouched");
  require(fake->bound_count() == 1, "exactly one host context remains bound");
  client2.reset();
  require(wait_until([fake] { return fake->bound_count() == 0; }, kWait),
          "second client's host context was unbound");

  host.stop_and_join();
  require(fake->contention_count == 0, "no admission contention during shutdown");
}

// Transport failure classification: a definitely-not-written request (never
// connected, or a write that provably accepted no byte) → Forward with identity
// preserved; an uncertain write (some bytes accepted, outcome unknown) → Reject;
// a completed write whose response is lost → Reject.
void test_hostserver_transport_failure() {
  // (a) Not connected at all.
  {
    IpcBackend client(test_pipe_name(L"never-connected"));
    const auto stimulus = character_stimulus({42, 7, 3}, 99, 'a');
    const BackendBatch batch = client.send(stimulus);
    require(batch.disposition == BackendBatch::Disposition::Forward, "never connected -> Forward");
    require(batch.operations.empty(), "never connected -> no operations");
    require(batch.binding == stimulus.binding, "never connected -> binding preserved");
    require(batch.stimulus_id == stimulus.stimulus_id, "never connected -> stimulus_id preserved");
    require(batch.backend_session_id == 0, "never connected -> no session id cached");
    require(!client.healthy(), "never connected -> not healthy");
  }

  // (b) The server closed the pipe after the Hello handshake, so the request
  //     cannot be written at all. The server waits for the client's read of the
  //     Hello response before closing, because closing a pipe discards data the
  //     peer has not consumed yet.
  {
    const auto pipe_name = test_pipe_name(L"write-failure");
    RawPipeServer server(pipe_name);
    require(server.create(), "write-failure server create");
    std::atomic<bool> hello_read{false};
    std::atomic<bool> closed{false};
    server.serve([&](RawPipeServer &s) {
      if (!s.server().wait_for_client(kWait))
        return;
      auto hello = s.server().recv_frame(kWait);
      if (!hello || hello->type != MessageType::HelloRequest)
        return;
      WireFrame response;
      response.type = MessageType::HelloResponse;
      detail::write_u64(response.payload, kTestIncarnation);
      detail::write_u32(response.payload, protocol_version);
      detail::write_u64(response.payload, 4242);
      s.server().send_frame(response);
      const auto deadline = std::chrono::steady_clock::now() + kWait;
      while (!hello_read.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      s.close(); // close the server end: the client's write must now fail
      closed = true;
    });

    IpcBackend client(pipe_name);
    require(client.connect(std::chrono::seconds(5)), "write-failure client connect");
    require(client.session_id() == 4242, "write-failure session id cached from Hello");
    hello_read = true; // the Hello response is consumed; the server may close
    require(wait_until([&closed] { return closed.load(); }, kWait), "write-failure server closed");

    const auto stimulus = character_stimulus({5, 1, 2}, 7, 'a');
    const BackendBatch batch = client.send(stimulus);
    require(batch.disposition == BackendBatch::Disposition::Forward, "write failure -> Forward");
    require(batch.operations.empty(), "write failure -> no operations");
    require(batch.binding == stimulus.binding, "write failure -> binding preserved");
    require(batch.stimulus_id == stimulus.stimulus_id, "write failure -> stimulus_id preserved");
    require(batch.backend_session_id == 4242, "write failure -> cached session id preserved");
    require(!client.healthy(), "write failure marks the client unhealthy");
  }

  // (c) The server read the request and closed without replying: the Host may
  //     have consumed the stimulus, so this must never be reported as Forward.
  {
    const auto pipe_name = test_pipe_name(L"post-write");
    RawPipeServer server(pipe_name);
    require(server.create(), "post-write server create");
    std::atomic<bool> request_seen{false};
    server.serve([&](RawPipeServer &s) {
      if (!s.server().wait_for_client(kWait))
        return;
      auto hello = s.server().recv_frame(kWait);
      if (!hello || hello->type != MessageType::HelloRequest)
        return;
      WireFrame response;
      response.type = MessageType::HelloResponse;
      detail::write_u64(response.payload, kTestIncarnation);
      detail::write_u32(response.payload, protocol_version);
      detail::write_u64(response.payload, 555);
      s.server().send_frame(response);
      auto request = s.server().recv_frame(kWait);
      request_seen = request && request->type == MessageType::SendRequest;
      s.server().disconnect_client(); // close without replying
    });

    IpcBackend client(pipe_name);
    require(client.connect(std::chrono::seconds(5)), "post-write client connect");
    const auto stimulus = character_stimulus({9, 1, 1}, 3, 'b');
    const BackendBatch batch = client.send(stimulus);
    require(request_seen.load(), "post-write: server received the SendRequest");
    require(batch.disposition == BackendBatch::Disposition::Reject, "post-write failure -> Reject");
    require(batch.operations.empty(), "post-write failure -> no operations");
    require(!client.healthy(), "post-write failure marks the client unhealthy");
  }

  // (d) The transport accepted part of the request frame and only then died. Some
  //     bytes of this frame are on the pipe, so zero-byte status cannot be
  //     proven: the stimulus is no longer provably caller-owned and must never
  //     become an ordinary Forward. Nothing is replayed and no key is injected.
  {
    const auto pipe_name = test_pipe_name(L"uncertain-write");
    TinyBufferPipeServer server(pipe_name);
    require(server.create(), "uncertain-write server create");
    std::atomic<bool> hello_answered{false};
    std::atomic<bool> partial_seen{false};
    std::atomic<DWORD> partial_bytes{0};
    server.serve([&](TinyBufferPipeServer &s) {
      hello_answered = s.answer_hello(9090);
      if (!hello_answered.load())
        return;
      // Stop reading: the next frame only fills part of the tiny inbound buffer.
      const auto deadline = std::chrono::steady_clock::now() + kWait;
      while (std::chrono::steady_clock::now() < deadline && !partial_seen.load()) {
        const DWORD available = s.peek_available();
        if (available > 0) {
          partial_bytes = available;
          partial_seen = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      s.abort_connection(); // the client still has bytes in flight
    });

    IpcBackend client(pipe_name);
    require(client.connect(std::chrono::seconds(5)), "uncertain-write client connect");
    require(wait_until([&hello_answered] { return hello_answered.load(); }, kWait),
            "uncertain-write hello answered");
    require(client.session_id() == 9090, "uncertain-write session id cached");

    const auto stimulus = character_stimulus({11, 1, 1}, 21, 'c');
    const BackendBatch batch = client.send(stimulus);
    require(partial_seen.load() && partial_bytes.load() > 0,
            "uncertain-write: server accepted part of the request frame");
    require(batch.disposition != BackendBatch::Disposition::Forward,
            "uncertain write must never Forward");
    require(batch.disposition == BackendBatch::Disposition::Reject,
            "uncertain write -> Reject (non-PASS terminal)");
    require(batch.operations.empty(), "uncertain write -> no operations");
    require(batch.binding == stimulus.binding, "uncertain write -> binding preserved");
    require(batch.stimulus_id == stimulus.stimulus_id, "uncertain write -> stimulus_id preserved");
    require(batch.backend_session_id == 9090, "uncertain write -> session id preserved");
    // The transport drops the connection and the backend stays unhealthy, so the
    // uncertain request is never retried or replayed.
    require(!client.healthy(), "uncertain write marks the client unhealthy");
  }
}

// healthy() is local state only: it must not write a single frame, counted on
// the server side, and must not reach the backend through HostServer.
void test_hostserver_healthy_no_ipc() {
  // Server-side received-frame count on a raw pipe server.
  {
    const auto pipe_name = test_pipe_name(L"healthy-frames");
    RawPipeServer server(pipe_name);
    require(server.create(), "healthy server create");
    std::atomic<int> frames_received{0};
    std::atomic<bool> hello_handled{false};
    server.serve([&](RawPipeServer &s) {
      if (!s.server().wait_for_client(kWait))
        return;
      auto hello = s.server().recv_frame(kWait);
      if (!hello || hello->type != MessageType::HelloRequest)
        return;
      ++frames_received;
      WireFrame response;
      response.type = MessageType::HelloResponse;
      detail::write_u64(response.payload, kTestIncarnation);
      detail::write_u32(response.payload, protocol_version);
      detail::write_u64(response.payload, 7);
      s.server().send_frame(response);
      hello_handled = true;
      while (s.server().client_connected()) {
        auto frame = s.server().recv_frame(std::chrono::milliseconds(0));
        if (!frame)
          break;
        ++frames_received; // any further frame is exactly what must not happen
      }
    });

    {
      IpcBackend client(pipe_name);
      require(client.connect(std::chrono::seconds(5)), "healthy client connect");
      require(wait_until([&hello_handled] { return hello_handled.load(); }, kWait),
              "healthy hello handled");
      const int frames_after_connect = frames_received.load();
      for (int i = 0; i < 5; ++i)
        require(client.healthy(), "healthy() reports local transport state");
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      require(frames_received.load() == frames_after_connect,
              "healthy() sent no frame (server-side received-frame count unchanged)");
    }
  }

  // HostServer-level: healthy() must not reach the backend either.
  {
    const auto pipe_name = test_pipe_name(L"healthy-hostserver");
    auto backend = std::make_unique<FcitxLikeBackend>();
    FcitxLikeBackend *fake = backend.get();
    RunningHost host(std::move(backend), pipe_name);
    require(host.start(), "healthy hostserver start");
    {
      auto client = connect_client(pipe_name);
      require(client != nullptr, "healthy hostserver connect");
      for (int i = 0; i < 5; ++i)
        require(client->healthy(), "healthy() over a Host connection");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      require(fake->call_count == 0, "healthy() performed no backend call");
    }
    host.stop_and_join();
  }
}

// stop() must wake the accept loop, disconnect live clients and join every
// connection thread: no hang, and no thread left that could touch the backend.
void test_hostserver_shutdown() {
  // No client at all.
  {
    const auto pipe_name = test_pipe_name(L"shutdown-idle");
    auto backend = std::make_unique<FcitxLikeBackend>();
    RunningHost host(std::move(backend), pipe_name);
    require(host.start(), "shutdown idle start");
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let it block on the pipe
    const auto begin = std::chrono::steady_clock::now();
    host.stop_and_join();
    require(std::chrono::steady_clock::now() - begin < std::chrono::seconds(5),
            "stop() unblocked the idle accept loop without hanging");
  }

  // Connected but idle client: the connection thread is blocked in a receive.
  {
    const auto pipe_name = test_pipe_name(L"shutdown-connected");
    auto backend = std::make_unique<FcitxLikeBackend>();
    FcitxLikeBackend *fake = backend.get();
    RunningHost host(std::move(backend), pipe_name);
    require(host.start(), "shutdown connected start");
    {
      auto client = connect_client(pipe_name);
      require(client != nullptr, "shutdown connected client");
      require(client->bind(Binding{1, 1, 1}), "shutdown connected bind");
      const auto begin = std::chrono::steady_clock::now();
      host.stop_and_join();
      require(std::chrono::steady_clock::now() - begin < std::chrono::seconds(5),
              "stop() with a live client did not hang");
      require(wait_until([fake] { return fake->bound_count() == 0; }, std::chrono::seconds(5)),
              "stop() unbound the live client's contexts");
      require(fake->contention_count == 0, "stop() caused no backend contention");
    }
  }

  // Repeated stop() (also the destructor path) stays safe.
  {
    const auto pipe_name = test_pipe_name(L"shutdown-twice");
    auto backend = std::make_unique<FcitxLikeBackend>();
    RunningHost host(std::move(backend), pipe_name);
    require(host.start(), "shutdown twice start");
    host.server().stop();
    host.server().stop();
    host.stop_and_join();
  }
}

// Completed connections must be released during normal operation instead of
// accumulating until Host shutdown: the retained tracked-connection count stays
// bounded by the live connections (plus at most the one that finished last),
// however many connect/disconnect cycles the Host has served.
void test_hostserver_connection_reaping() {
  const auto pipe_name = test_pipe_name(L"reaping");
  auto backend = std::make_unique<FcitxLikeBackend>();
  FcitxLikeBackend *fake = backend.get();
  RunningHost host(std::move(backend), pipe_name);
  require(host.start(), "reaping start");

  constexpr int kBatch = 6;
  const auto run_cycles = [&](int first) {
    for (int i = first; i < first + kBatch; ++i) {
      auto client = connect_client(pipe_name);
      require(client != nullptr, "reaping client connect");
      // A live connection is tracked and must not be reaped.
      require(wait_until([&host] { return host.server().tracked_connection_count() == 1; }, kWait),
              "live connection is the only tracked connection");
      const Binding binding{static_cast<std::uint64_t>(100 + i), 1, 1};
      require(client->bind(binding), "reaping bind");
      const BackendBatch batch = client->send(character_stimulus(binding, 5 + i, 'a'));
      require(batch.disposition == BackendBatch::Disposition::Consumed,
              "live connection is still served");
      client.reset(); // disconnect: the connection thread finishes on its own
      require(wait_until([fake] { return fake->bound_count() == 0; }, kWait),
              "disconnected connection unbound its context");
      // live 0 + at most the connection that just completed: the count must not
      // grow with the number of historical connections.
      require(host.server().tracked_connection_count() <= 2,
              "completed connections do not accumulate");
    }
  };

  DWORD handles_before = 0;
  DWORD handles_middle = 0;
  DWORD handles_after = 0;
  GetProcessHandleCount(GetCurrentProcess(), &handles_before);
  run_cycles(0);
  GetProcessHandleCount(GetCurrentProcess(), &handles_middle);
  run_cycles(kBatch);
  GetProcessHandleCount(GetCurrentProcess(), &handles_after);
  std::printf("  reaping: %d cycles in %d batches, tracked=%zu handles %lu -> %lu -> %lu\n",
              2 * kBatch, 2, host.server().tracked_connection_count(), handles_before,
              handles_middle, handles_after);
  // A second batch of the same size must not consume more resources than the
  // first: that is what distinguishes reaping from retaining every connection.
  require(handles_after <= handles_middle + 3,
          "connection resources do not grow with further connect/disconnect cycles");
  require(fake->bound_count() == 0, "no context left bound after the cycles");
  require(fake->contention_count == 0, "no admission contention during reaping");

  // A live connection at stop() time is still disconnected and joined, and the
  // reaping path leaves nothing behind.
  {
    auto client = connect_client(pipe_name);
    require(client != nullptr, "reaping stop client");
    require(client->bind(Binding{777, 1, 1}), "reaping stop bind");
    host.stop_and_join();
    require(wait_until([fake] { return fake->bound_count() == 0; }, kWait),
            "stop() unbound the live connection");
    require(host.server().tracked_connection_count() == 0,
            "stop() released every tracked connection");
    host.server().stop(); // repeated stop stays safe
  }
}

void test_settings_host() {
  const auto pipe_name = test_pipe_name(L"settings-control");
  auto backend = std::make_unique<FcitxLikeBackend>();
  FcitxLikeBackend *fake = backend.get();
  RunningHost host(std::move(backend), pipe_name);
  require(host.start(), "settings Host start");

  auto send_one = [&](MessageType type, const std::vector<std::uint8_t> &payload) {
    PipeClient client(pipe_name);
    std::uint64_t incarnation = 0;
    SettingsStatus hello_status = SettingsStatus::Error;
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    bool connected = false;
    while (!connected && std::chrono::steady_clock::now() < deadline) {
      connected = connect_settings_client(client, protocol_version, settings_api_version,
                                          incarnation, hello_status);
      if (!connected)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(connected && hello_status == SettingsStatus::Ok && incarnation != 0,
            "settings API capability handshake succeeds");
    auto reply = settings_request(client, type, payload, incarnation);
    require(reply.has_value(), "settings request receives an incarnation-fenced reply");
    return std::move(*reply);
  };

  // The settings extension remains protocol v4-compatible with a TIP client that
  // sends only the original Hello/Bind/Send messages.
  auto old_tip = connect_client(pipe_name);
  require(old_tip != nullptr, "existing v4 TIP client connects alongside settings");
  const Binding binding{73, 1, 1};
  require(old_tip->bind(binding), "existing v4 TIP Bind still succeeds");
  auto batch = old_tip->send(character_stimulus(binding, 1, 'a'));
  require(batch.disposition == BackendBatch::Disposition::Consumed,
          "existing v4 TIP Send still succeeds after the settings extension");

  // The settings handshake reports protocol/API incompatibility distinctly.
  {
    PipeClient client(pipe_name);
    std::uint64_t incarnation = 0;
    SettingsStatus status = SettingsStatus::Ok;
    require(connect_settings_client(client, protocol_version + 1, settings_api_version,
                                    incarnation, status),
            "settings protocol mismatch receives a bounded response");
    require(status == SettingsStatus::VersionMismatch,
            "settings protocol mismatch is explicit");
  }
  {
    PipeClient client(pipe_name);
    std::uint64_t incarnation = 0;
    SettingsStatus status = SettingsStatus::Ok;
    require(connect_settings_client(client, protocol_version, settings_api_version + 1,
                                    incarnation, status),
            "settings API mismatch receives a bounded response");
    require(status == SettingsStatus::VersionMismatch,
            "settings API mismatch is explicit");
  }

  auto get_payload = serialize_settings_get_request("fcitx://config/inputmethod/pinyin");
  auto get_reply = send_one(MessageType::SettingsGetConfigRequest, get_payload);
  detail::Reader get_reader{get_reply.payload.data(), get_reply.payload.size()};
  auto get_status = deserialize_settings_status(get_reader);
  SettingsSnapshot snapshot;
  require(get_status && get_status->status == SettingsStatus::Ok &&
              settings_detail::read_snapshot(get_reader, snapshot) && get_reader.at_end() &&
              snapshot.values.entries.at("PageSize").scalar == "7" &&
              snapshot.values.entries.at("FutureOption").scalar == "keep-me" &&
              snapshot.types.size() == 1 && snapshot.types[0].options.size() == 3,
          "GetConfig delivers dynamic values and official-schema-shaped metadata");

  SettingsValue requested = SettingsValue::object();
  requested.entries.emplace("PageSize", SettingsValue::string("9"));
  std::vector<std::uint8_t> set_payload;
  require(serialize_settings_set_request(set_payload,
                                         "fcitx://config/inputmethod/pinyin", requested),
          "SetConfig request serialization");
  auto set_reply = send_one(MessageType::SettingsSetConfigRequest, set_payload);
  detail::Reader set_reader{set_reply.payload.data(), set_reply.payload.size()};
  auto set_status = deserialize_settings_status(set_reader);
  SettingsSnapshot applied;
  require(set_status && set_status->status == SettingsStatus::Ok &&
              settings_detail::read_snapshot(set_reader, applied) && set_reader.at_end() &&
              applied.values.entries.at("PageSize").scalar == "9" &&
              applied.values.entries.at("FutureOption").scalar == "keep-me" &&
              fake->settings_set_count == 1,
          "SetConfig is acknowledged only after the backend returns the resulting state");

  // The two settings pages share options but are applied independently. A page that
  // submits only what it changed must not restore a shared value the other page has
  // just written, in either direction.
  const auto apply_delta = [&](const char *uri, const char *key, const char *value) {
    SettingsValue delta = SettingsValue::object();
    delta.entries.emplace(key, SettingsValue::string(value));
    std::vector<std::uint8_t> payload;
    require(serialize_settings_set_request(payload, uri, delta),
            "partial SetConfig request serialization");
    auto reply = send_one(MessageType::SettingsSetConfigRequest, payload);
    detail::Reader reader{reply.payload.data(), reply.payload.size()};
    auto status = deserialize_settings_status(reader);
    SettingsSnapshot snapshot_after;
    require(status && status->status == SettingsStatus::Ok &&
                settings_detail::read_snapshot(reader, snapshot_after) && reader.at_end(),
            "partial SetConfig is acknowledged");
    return snapshot_after;
  };
  const auto shuangpin_delta =
      apply_delta("fcitx://config/inputmethod/shuangpin", "ShuangpinProfile", "MS");
  require(shuangpin_delta.values.entries.at("PageSize").scalar == "9" &&
              shuangpin_delta.values.entries.at("ShuangpinProfile").scalar == "MS" &&
              shuangpin_delta.values.entries.at("FutureOption").scalar == "keep-me" &&
              fake->settings_set_count == 2,
          "a partial apply from the other page preserves the shared value it did not submit");
  require(apply_delta("fcitx://config/inputmethod/shuangpin", "PageSize", "8")
                  .values.entries.at("PageSize")
                  .scalar == "8",
          "the other page can change the shared value");
  const auto pinyin_delta =
      apply_delta("fcitx://config/inputmethod/pinyin", "SpellEnabled", "false");
  require(pinyin_delta.values.entries.at("PageSize").scalar == "8" &&
              pinyin_delta.values.entries.at("SpellEnabled").scalar == "false" &&
              pinyin_delta.values.entries.at("ShuangpinProfile").scalar == "MS" &&
              fake->settings_set_count == 4,
          "the reverse page direction preserves the shared value it did not submit");

  auto selection_reply = send_one(MessageType::SettingsGetSelectionRequest, {});
  detail::Reader selection_reader{selection_reply.payload.data(), selection_reply.payload.size()};
  auto selection_status = deserialize_settings_status(selection_reader);
  SettingsSelection selection;
  require(selection_status && selection_status->status == SettingsStatus::Ok &&
              settings_detail::read_selection(selection_reader, selection) &&
              selection_reader.at_end() && selection.selected == "pinyin" &&
              selection.available.size() == 2,
          "selection reads the Fcitx group state");

  std::vector<std::uint8_t> select_payload;
  detail::write_string(select_payload, "shuangpin");
  auto select_reply = send_one(MessageType::SettingsSelectInputMethodRequest, select_payload);
  detail::Reader select_reader{select_reply.payload.data(), select_reply.payload.size()};
  auto select_status = deserialize_settings_status(select_reader);
  SettingsSelection selected;
  require(select_status && select_status->status == SettingsStatus::Ok &&
              settings_detail::read_selection(select_reader, selected) && select_reader.at_end() &&
              selected.selected == "shuangpin",
          "selection apply returns the Host-accepted current method");

  fake->composition_active = true;
  std::vector<std::uint8_t> busy_select_payload;
  detail::write_string(busy_select_payload, "pinyin");
  auto busy_reply = send_one(MessageType::SettingsSelectInputMethodRequest, busy_select_payload);
  detail::Reader busy_reader{busy_reply.payload.data(), busy_reply.payload.size()};
  auto busy_status = deserialize_settings_status(busy_reader);
  require(busy_status && busy_status->status == SettingsStatus::Busy && busy_reader.at_end(),
          "active composition returns a visible retry condition without switching");
  auto unchanged_reply = send_one(MessageType::SettingsGetSelectionRequest, {});
  detail::Reader unchanged_reader{unchanged_reply.payload.data(), unchanged_reply.payload.size()};
  auto unchanged_status = deserialize_settings_status(unchanged_reader);
  SettingsSelection unchanged;
  require(unchanged_status && unchanged_status->status == SettingsStatus::Ok &&
              settings_detail::read_selection(unchanged_reader, unchanged) &&
              unchanged.selected == "shuangpin",
          "active-composition refusal preserves the selected method");
  fake->composition_active = false;

  // Malformed settings payloads fail closed and never reach the backend.
  {
    PipeClient client(pipe_name);
    std::uint64_t incarnation = 0;
    SettingsStatus status = SettingsStatus::Error;
    require(connect_settings_client(client, protocol_version, settings_api_version,
                                    incarnation, status) && status == SettingsStatus::Ok,
            "malformed-request test handshake");
    const std::vector<std::uint8_t> malformed{0xFF};
    require(client.send_request(MessageType::SettingsSetConfigRequest, malformed) ==
                WriteOutcome::Written,
            "malformed request reached the Host parser");
    require(!client.recv_response(std::chrono::seconds(2)),
            "malformed settings request is disconnected without an apply");
    require(fake->settings_set_count == 4, "malformed request never reached backend apply");
  }
  old_tip.reset();
}

void test_hostserver() {
  test_settings_host();
  test_hostserver_multi_client();
  test_hostserver_transport_failure();
  test_hostserver_healthy_no_ipc();
  test_hostserver_connection_reaping();
  test_hostserver_shutdown();
  std::cout << "PASS HostServer: multi-client remapping, disconnect isolation, transport "
               "failure separation, healthy() no IPC, connection reaping, clean stop/join\n";
}

// Stage 6 WP3 A5: the singleton is claimed before a backend exists, so a Host that
// loses the claim never constructs the engine (and so never loads or saves
// per-user data). The claim is released when the holder goes away.
void test_singleton_claim() {
  const auto pipe_name = test_pipe_name(L"wp3-singleton");
  {
    HostClaim first;
    require(first.acquire(pipe_name), "first claim owns the host name");
    require(first.held(), "claim reports held");
    HostClaim second;
    require(!second.acquire(pipe_name), "a second claim is refused");
    require(!second.held(), "refused claim is not held");
  }
  HostClaim again;
  require(again.acquire(pipe_name), "the claim is released with its holder");
  again.release();
  require(!again.held(), "release drops the claim");

  // The claimed name still serves a real Host (the pipe is created by the accept
  // loop, after the backend is ready).
  const auto host_pipe = test_pipe_name(L"wp3-singleton-host");
  HostClaim claim;
  require(claim.acquire(host_pipe), "host claim");
  auto backend = std::make_unique<FcitxLikeBackend>();
  RunningHost host(std::move(backend), host_pipe);
  require(host.start(), "host starts under the claim");
  auto client = connect_client(host_pipe);
  require(client != nullptr, "client connects");
  require(client->bind(Binding{1, 1, 1}), "bind");
  std::cout << "PASS WP3 singleton: claimed before the backend, second claim refused\n";
}

// Stage 6 WP3: the console control handler may only request the stop; exactly one
// thread (main) performs the ordered shutdown — stop() (end accept loop, disconnect
// and join every connection thread), then shutdown_backend() (destroy the backend),
// then destruction. This drives that sequence with a client connected and watches
// the backend: destroyed exactly once, after every connection thread ended, never
// during a call.
class ShutdownWatchBackend final : public Backend {
public:
  static void reset() {
    destroyed_.store(0);
    destroyed_during_call_.store(false);
    in_call_.store(0);
  }
  static int destroyed() { return destroyed_.load(); }
  static bool destroyed_during_call() { return destroyed_during_call_.load(); }
  static int in_call() { return in_call_.load(); }

  ~ShutdownWatchBackend() override {
    if (in_call_.load() > 0)
      destroyed_during_call_.store(true);
    destroyed_.fetch_add(1);
  }

  bool bind(Binding) override {
    CallGuard guard;
    return true;
  }
  bool reset(Binding) override {
    CallGuard guard;
    return true;
  }
  void unbind(Binding) override { CallGuard guard; }
  bool healthy() const override { return true; }
  std::uint64_t session_id() const override { return 777; }
  BackendBatch send(const BackendStimulus &stimulus) override {
    CallGuard guard;
    return BackendBatch{session_id(), stimulus.binding, stimulus.stimulus_id};
  }

private:
  struct CallGuard {
    CallGuard() { in_call_.fetch_add(1); }
    ~CallGuard() { in_call_.fetch_sub(1); }
  };
  inline static std::atomic<int> in_call_{0};
  inline static std::atomic<int> destroyed_{0};
  inline static std::atomic<bool> destroyed_during_call_{false};
};

void test_shutdown_order() {
  const auto pipe_name = test_pipe_name(L"wp3-shutdown");
  ShutdownWatchBackend::reset();
  {
    auto backend = std::make_unique<ShutdownWatchBackend>();
    HostServer server(std::move(backend), pipe_name);
    require(server.start(), "shutdown start");
    std::thread runner([&server] { server.run(); });
    auto client = connect_client(pipe_name);
    require(client != nullptr, "shutdown client connected");
    require(client->bind(Binding{1, 1, 1}), "shutdown bind");

    // What the console handler does: request the stop from another thread and return
    // without draining, joining or touching the backend.
    const auto begin = std::chrono::steady_clock::now();
    std::thread handler([&server] { server.request_stop(); });
    handler.join();
    require(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2),
            "request_stop returns promptly and does not drain");

    // What main() does, in order, on one thread.
    if (runner.joinable())
      runner.join();
    server.stop();
    require(server.tracked_connection_count() == 0, "no connection thread survives the drain");
    require(ShutdownWatchBackend::in_call() == 0, "no backend call is in flight after the drain");
    require(ShutdownWatchBackend::destroyed() == 0,
            "the backend is still alive before its shutdown");
    server.shutdown_backend();
    require(ShutdownWatchBackend::destroyed() == 1, "the backend is destroyed exactly once");
    require(!ShutdownWatchBackend::destroyed_during_call(),
            "the backend is never destroyed during a call");
  }
  require(ShutdownWatchBackend::destroyed() == 1,
          "leaving the scope does not destroy the backend a second time");
  std::cout << "PASS WP3 shutdown: handler requests the stop, one thread drains then destroys the "
               "backend, no connection thread survives\n";
}

// Stage 6 WP4 A2/A6: the named stop event is the non-console way an elevated
// install/upgrade/uninstall asks a running Host for an *orderly* stop. This drives
// the product sequence with a client connected: an outside caller (here the test,
// opening the event by name exactly as the scripts do) signals it, the Host's
// watcher only requests the stop, and the single owning thread still drains before
// the backend is destroyed.
void test_stop_event() {
  const auto pipe_name = test_pipe_name(L"wp4-stop-event");
  const std::wstring scope = L"wp4-stop-event-test";
  ShutdownWatchBackend::reset();

  HostStopEvent stop_event;
  require(stop_event.create(scope), "stop event created");
  require(stop_event.valid(), "stop event is held");
  require(stop_event.name() == std::wstring(L"Local\\FusionHost-stop-") + scope,
          "the stop event is named in Local\\ for the user x session scope");

  // A second Host for the same principal cannot claim the same stop path.
  HostStopEvent second;
  require(!second.create(scope), "a second stop event for the same scope is refused");

  HANDLE exit_complete = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  require(exit_complete != nullptr, "exit signal created");

  {
    auto backend = std::make_unique<ShutdownWatchBackend>();
    HostServer server(std::move(backend), pipe_name);
    require(server.start(), "stop-event start");
    std::thread runner([&server] { server.run(); });
    auto client = connect_client(pipe_name);
    require(client != nullptr, "stop-event client connected");
    require(client->bind(Binding{1, 1, 1}), "stop-event bind");

    // What the Host does: one watcher thread that may only request the stop.
    std::thread watcher([&server, &stop_event, exit_complete] {
      if (wait_for_stop_request(stop_event.handle(), exit_complete))
        server.request_stop();
    });

    // What install.ps1 / uninstall.ps1 do: open the event by name and signal it.
    HANDLE outside = OpenEventW(EVENT_MODIFY_STATE, FALSE, stop_event.name().c_str());
    require(outside != nullptr, "an outside caller opens the stop event by name");
    require(SetEvent(outside) != FALSE, "the outside caller signals the stop event");
    CloseHandle(outside);

    // The signal itself shuts nothing down: the connection is still owned and the
    // backend is untouched until the owning thread performs the ordered shutdown.
    require(wait_until([&server] { return server.tracked_connection_count() == 1; },
                       std::chrono::milliseconds(100)),
            "the connection is still owned after the stop was requested");
    require(ShutdownWatchBackend::destroyed() == 0, "signalling the stop event destroys nothing");

    if (runner.joinable())
      runner.join();
    server.stop();
    require(server.tracked_connection_count() == 0, "no connection thread survives the drain");
    require(ShutdownWatchBackend::destroyed() == 0,
            "the backend is still alive before its shutdown");
    server.shutdown_backend();
    require(ShutdownWatchBackend::destroyed() == 1, "the backend is destroyed exactly once");
    require(!ShutdownWatchBackend::destroyed_during_call(),
            "the backend is never destroyed during a call");

    SetEvent(exit_complete);
    watcher.join();
  }
  CloseHandle(exit_complete);
  require(ShutdownWatchBackend::destroyed() == 1,
          "leaving the scope does not destroy the backend a second time");
  std::cout << "PASS WP4 stop event: an outside caller signals it by name, the watcher only "
               "requests the stop, one thread drains then destroys the backend\n";
}

// Stage 6 WP4: a logoff/shutdown can still be cancelled *after* WM_QUERYENDSESSION
// (an application asks to save and the user cancels), so that message must not stop
// the Host — a Host that stopped there would stay gone until the next logon. Only
// WM_ENDSESSION with wParam TRUE means the session is really ending; it requests the
// stop and waits, bounded, for the ordered shutdown.
//
// R34: this decision does not make the Host survive a *cancelled* end — Windows delivers
// ENDSESSION(TRUE) before the Cancel takes effect, and restarting afterwards is the
// scheduled task's event trigger. What is tested here is only the decision itself, plus
// the external WM_CLOSE case: before the exit it must request the stop and keep the
// window, after the exit it must close it.
void test_session_end() {
  HANDLE exit_complete = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  require(exit_complete != nullptr, "session-end exit signal created");
  int requests = 0;
  constexpr unsigned long timeout_ms = 300;
  fusion::ipc::SessionEndHandler handler([&requests](const char *) { requests++; }, exit_complete,
                                         timeout_ms);

  require(fusion::ipc::SessionEndHandler::handles(WM_QUERYENDSESSION), "query-end is handled");
  require(fusion::ipc::SessionEndHandler::handles(WM_ENDSESSION), "end-session is handled");
  require(!fusion::ipc::SessionEndHandler::handles(WM_CLOSE), "window close is not a session end");

  // A logoff that may still be cancelled: the query is answered TRUE, nothing stops.
  require(handler.handle(WM_QUERYENDSESSION, 0) == TRUE, "WM_QUERYENDSESSION answers TRUE");
  require(requests == 0, "WM_QUERYENDSESSION does not stop the Host");

  // WM_ENDSESSION(FALSE): the session end was cancelled after the query.
  require(handler.handle(WM_ENDSESSION, FALSE) == 0, "WM_ENDSESSION(FALSE) returns 0");
  require(requests == 0, "a cancelled session end does not stop the Host");

  // WM_ENDSESSION(TRUE): the session is ending. One request, then a bounded wait.
  const auto begin = std::chrono::steady_clock::now();
  require(handler.handle(WM_ENDSESSION, TRUE) == 0, "WM_ENDSESSION(TRUE) returns 0");
  const auto waited = std::chrono::steady_clock::now() - begin;
  require(requests == 1, "WM_ENDSESSION(TRUE) requests the stop exactly once");
  require(waited >= std::chrono::milliseconds(timeout_ms - 100),
          "WM_ENDSESSION(TRUE) waits for the ordered exit, bounded");

  // With the exit already signalled the same call returns at once.
  SetEvent(exit_complete);
  const auto again = std::chrono::steady_clock::now();
  require(handler.handle(WM_ENDSESSION, TRUE) == 0, "WM_ENDSESSION(TRUE) again returns 0");
  require(std::chrono::steady_clock::now() - again < std::chrono::milliseconds(timeout_ms - 100),
          "a finished exit releases the wait immediately");
  require(requests == 2, "each WM_ENDSESSION(TRUE) requests the stop once");

  // R34: a WM_CLOSE from outside (taskkill without /f) must not remove the Host's only
  // logoff/shutdown path. Before main() has finished it requests the same orderly stop
  // and the window is kept; after the exit it is main()'s own close and the window goes.
  {
    HANDLE close_exit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(close_exit != nullptr, "close-test exit signal created");
    int close_requests = 0;
    fusion::ipc::SessionEndHandler close_handler(
        [&close_requests](const char *) { close_requests++; }, close_exit, timeout_ms);
    require(close_handler.close_keeps_window(),
            "an external WM_CLOSE keeps the window before the exit");
    require(close_requests == 1, "an external WM_CLOSE requests the orderly stop");
    SetEvent(close_exit);
    require(!close_handler.close_keeps_window(), "after the exit, WM_CLOSE closes the window");
    require(close_requests == 1, "the close after the exit requests nothing more");
    CloseHandle(close_exit);
  }

  CloseHandle(exit_complete);
  std::cout << "PASS WP4 session end: a query or a cancelled end stops nothing, only "
               "WM_ENDSESSION(TRUE) requests the stop and waits, bounded; an external "
               "WM_CLOSE before the exit requests the same stop instead of removing the "
               "window\n";
}

// ---------------------------------------------------------------------------
// R30: the opt-in Host diagnostic log
// ---------------------------------------------------------------------------
//
// The module keeps process-wide state, so the paths run in order in this one
// process: off, opt-in, redaction, size cap, retention. %LOCALAPPDATA% is
// redirected to a private root, so the real profile is never touched.

namespace host_diag_test {

bool exists(const std::string &path) {
  return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string read_all(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)), {});
}

unsigned long long size_of(const std::string &path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
    return 0;
  ULARGE_INTEGER value{};
  value.LowPart = data.nFileSizeLow;
  value.HighPart = data.nFileSizeHigh;
  return value.QuadPart;
}

void write_text(const std::string &path, const std::string &text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}

// Gives a fixture an age so retention has a defined order.
void set_age_hours(const std::string &path, int hours) {
  HANDLE file = CreateFileA(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(file != INVALID_HANDLE_VALUE, "open a retention fixture");
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER value{};
  value.LowPart = now.dwLowDateTime;
  value.HighPart = now.dwHighDateTime;
  value.QuadPart -= static_cast<unsigned long long>(hours) * 3600ull * 10000000ull;
  FILETIME stamp{};
  stamp.dwLowDateTime = value.LowPart;
  stamp.dwHighDateTime = value.HighPart;
  SetFileTime(file, nullptr, nullptr, &stamp);
  CloseHandle(file);
}

int count_logs(const std::string &directory) {
  WIN32_FIND_DATAA data{};
  HANDLE find = FindFirstFileA((directory + "\\fusion-host-*.log").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE)
    return 0;
  int count = 0;
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
      ++count;
  } while (FindNextFileA(find, &data) != 0);
  FindClose(find);
  return count;
}

void remove_tree(const std::string &directory) {
  WIN32_FIND_DATAA data{};
  HANDLE find = FindFirstFileA((directory + "\\*").c_str(), &data);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (std::strcmp(data.cFileName, ".") == 0 || std::strcmp(data.cFileName, "..") == 0)
        continue;
      const std::string child = directory + "\\" + data.cFileName;
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        remove_tree(child);
      else
        DeleteFileA(child.c_str());
    } while (FindNextFileA(find, &data) != 0);
    FindClose(find);
  }
  RemoveDirectoryA(directory.c_str());
}

} // namespace host_diag_test

void test_host_diag() {
  namespace diag = fusion::host::diag;
  using namespace host_diag_test;

  char temp[MAX_PATH]{};
  require(GetTempPathA(MAX_PATH, temp) != 0, "GetTempPath failed");
  const std::string root =
      std::string(temp) + "fusion-host-diag-" + std::to_string(GetCurrentProcessId());
  const std::string retention_root = root + "-retention";
  remove_tree(root);
  remove_tree(retention_root);
  require(CreateDirectoryA(root.c_str(), nullptr) != 0, "create the diagnostic test root");
  require(SetEnvironmentVariableA("LOCALAPPDATA", root.c_str()) != 0, "redirect LOCALAPPDATA");
  const std::string directory = root + "\\fusion-host";
  const std::string file =
      directory + "\\fusion-host-" + std::to_string(GetCurrentProcessId()) + ".log";

  // 1. Off by default: no directory, no file, no work.
  diag::reset_for_tests();
  SetEnvironmentVariableA("FUSION_HOST_DIAGNOSTICS", nullptr);
  diag::initialize();
  require(!diag::enabled(), "the Host log is on without the opt-in");
  diag::event("off_probe", "x=1");
  require(!exists(directory), "a directory was created while the log was off");
  std::cout << "PASS R30 host diag: off by default (no directory, no file)\n";

  // 2. Opt-in (case-insensitive), file created at startup.
  diag::reset_for_tests();
  SetEnvironmentVariableA("FUSION_HOST_DIAGNOSTICS", "On");
  diag::initialize();
  require(diag::enabled(), "the opt-in did not enable the Host log");
  require(exists(file), "the log file was not created at startup");
  diag::event("startup_probe", "x=1");
  std::string contents = read_all(file);
  require(contents.find("startup_probe x=1") != std::string::npos, "the startup event is missing");
  require(contents.rfind("ts=", 0) == 0, "the line format does not start with ts=");
  require(contents.find("pid=" + std::to_string(GetCurrentProcessId())) != std::string::npos,
          "the line format does not carry the pid");
  std::cout << "PASS R30 host diag: opt-in creates the file with the expected startup line\n";

  // 3. Privacy markers are redacted.
  diag::event("policy_probe", "vk=0x41 text=secret");
  contents = read_all(file);
  require(contents.find("policy_probe privacy=redacted") != std::string::npos,
          "a marker-bearing detail was not redacted");
  require(contents.find("vk=") == std::string::npos, "key data reached the Host log");
  std::cout << "PASS R30 host diag: privacy markers are redacted\n";

  // 4. Size cap: the file is recreated empty once it reaches the bound.
  diag::event("before_cap", "x=1");
  const std::string filler(900, 'a');
  for (int i = 0; i < 3000; ++i)
    diag::event("cap_probe", ("pad=" + filler).c_str());
  const unsigned long long capped = size_of(file);
  require(capped > 0 && capped <= diag::kMaxFileBytes, "the Host log exceeded its bound");
  contents = read_all(file);
  require(contents.find("before_cap") == std::string::npos, "the log was not truncated at the cap");
  require(contents.find("cap_probe") != std::string::npos, "the log stopped writing after the cap");
  std::cout << "PASS R30 host diag: the size cap truncates and keeps writing (" << capped
            << " bytes)\n";

  // 5. Retention: the newest four old files stay, older ones go, nothing else is touched.
  require(CreateDirectoryA(retention_root.c_str(), nullptr) != 0, "create the retention root");
  require(SetEnvironmentVariableA("LOCALAPPDATA", retention_root.c_str()) != 0,
          "redirect LOCALAPPDATA");
  const std::string retention_directory = retention_root + "\\fusion-host";
  require(CreateDirectoryA(retention_directory.c_str(), nullptr) != 0,
          "create the retention log directory");
  for (int i = 0; i < 7; ++i) {
    const std::string path = retention_directory + "\\fusion-host-90" + std::to_string(i) + ".log";
    write_text(path, "old\n");
    set_age_hours(path, 7 - i); // ...-900 is the oldest, ...-906 the newest
  }
  const std::string other = retention_directory + "\\keepme.txt";
  write_text(other, "not a log\n");
  diag::reset_for_tests();
  diag::initialize();
  require(diag::enabled(), "the opt-in was lost across the reset");
  require(count_logs(retention_directory) == diag::kRetentionFiles,
          "retention did not keep five files");
  require(!exists(retention_directory + "\\fusion-host-900.log") &&
              !exists(retention_directory + "\\fusion-host-901.log") &&
              !exists(retention_directory + "\\fusion-host-902.log"),
          "retention did not delete the oldest files");
  require(exists(retention_directory + "\\fusion-host-906.log"), "retention deleted a newest file");
  require(exists(other), "retention touched a file that is not a Host log");
  std::cout << "PASS R30 host diag: retention keeps the newest five and nothing else\n";

  diag::reset_for_tests();
  SetEnvironmentVariableA("FUSION_HOST_DIAGNOSTICS", nullptr);
  remove_tree(root);
  remove_tree(retention_root);
}

} // namespace

// ---------------------------------------------------------------------------
// Layer 3: Full integration test (FUSION_WITH_FCITX5 build only)
// ---------------------------------------------------------------------------

#ifdef FUSION_WITH_FCITX5
void test_integration() {
  // Check if we should skip.
  if (std::getenv("FUSION_STAGE4A_SKIP_INTEGRATION")) {
    std::cout << "SKIP integration (FUSION_STAGE4A_SKIP_INTEGRATION set)\n";
    return;
  }

  // This test requires FusionHost.exe to be running.
  auto backend = make_ipc_backend(std::chrono::milliseconds(2000));
  if (!backend) {
    std::fprintf(stderr, "FAIL integration: FusionHost not reachable\n");
    std::fprintf(stderr, "  Set FUSION_STAGE4A_SKIP_INTEGRATION=1 to skip.\n");
    throw std::runtime_error("integration: FusionHost not reachable");
  }
  require(backend->healthy(), "integration healthy");

  SemanticController controller(901, 64, *backend);
  Binding binding{901, 1, 1};

  // Type "nihao" and verify preedit
  std::string expected_preedit;
  for (char c : std::string("nihao")) {
    auto d = controller.begin_key(physical(c), true);
    require(d.eaten && d.backend_session_id &&
                d.route != SemanticDecision::Route::CancelledRecovery,
            "integration Pinyin key did not reach Fcitx");
    for (const auto &work : d.frontend) {
      if (work.operation.type == BackendOperation::Type::SetPreedit) {
        expected_preedit = work.operation.text;
        controller.settle_state(work.key, EditResult::AppliedReadback);
      } else if (work.operation.type == BackendOperation::Type::CommitText) {
        auto ticket = controller.open_effect(work.key);
        require(ticket.has_value(), "integration EFFECT ticket");
        controller.settle_effect(*ticket, EditResult::AppliedReadback);
      }
    }
  }
  require(!expected_preedit.empty(), "integration nihao preedit");

  // Space to commit
  auto space_action = translate_key(0x20, false, false, false);
  auto d = controller.begin_key(space_action, true);
  require(d.eaten && d.backend_session_id, "integration space");
  std::string committed;
  for (const auto &work : d.frontend) {
    if (work.operation.type == BackendOperation::Type::CommitText) {
      committed += work.operation.text;
      auto ticket = controller.open_effect(work.key);
      require(ticket.has_value(), "integration commit ticket");
      controller.settle_effect(*ticket, EditResult::AppliedReadback);
    } else if (work.operation.type == BackendOperation::Type::SetPreedit) {
      controller.settle_state(work.key, EditResult::AppliedReadback);
    }
  }
  require(committed == "你好", "integration nihao+space commit");

  // Stage 5 discriminating real-engine evidence: start a fresh composition and
  // select a projected candidate. The commit must be the candidate page's OWN
  // text, proving selection goes through Backend::select() rather than a synthetic
  // numeric candidate key, and that the real pinyin page survives the v3 IPC wire.
  for (char c : std::string("nihao")) {
    auto k = controller.begin_key(physical(c), true);
    require(k.route != SemanticDecision::Route::CancelledRecovery,
            "integration second-round pinyin key reached Fcitx");
    for (const auto &work : k.frontend) {
      if (work.operation.type == BackendOperation::Type::CommitText) {
        auto ticket = controller.open_effect(work.key);
        if (ticket)
          controller.settle_effect(*ticket, EditResult::AppliedReadback);
      } else if (work.operation.type == BackendOperation::Type::SetPreedit) {
        controller.settle_state(work.key, EditResult::AppliedReadback);
      }
    }
  }
  const auto projected = controller.displayed_page();
  require(projected.has_value() && !projected->entries.empty(),
          "integration real engine projected a non-empty candidate page over IPC");
  std::int32_t pick = -1;
  std::string pick_text;
  for (std::size_t i = 0; i < projected->entries.size(); ++i) {
    if (projected->entries[i].placeholder || projected->entries[i].text.empty())
      continue;
    pick = static_cast<std::int32_t>(i);
    pick_text = projected->entries[i].text;
    break;
  }
  require(pick >= 0, "integration candidate page had a selectable entry");
  auto sel = controller.begin_candidate_action(CandidateAction::Select, pick);
  require(sel.route == SemanticDecision::Route::Effect,
          "integration candidate select routed to an EFFECT");
  std::string selected_commit;
  for (const auto &work : sel.frontend) {
    if (work.operation.type == BackendOperation::Type::CommitText) {
      selected_commit += work.operation.text;
      auto ticket = controller.open_effect(work.key);
      require(ticket.has_value(), "integration candidate select EFFECT ticket");
      controller.settle_effect(*ticket, EditResult::AppliedReadback);
    } else if (work.operation.type == BackendOperation::Type::SetPreedit) {
      controller.settle_state(work.key, EditResult::AppliedReadback);
    }
  }
  require(!selected_commit.empty() && selected_commit == pick_text,
          "integration candidate select committed the projected entry's own text");
  std::cout << "PASS integration candidate select committed the projected candidate\n";

  controller.destroy();
  std::cout << "PASS integration with real Fcitx over IPC\n";
}
#endif // FUSION_WITH_FCITX5

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
  // Diagnostics to stderr: an assertion must never open a modal dialog in an
  // automated ctest run (same convention as the other test targets).
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
#ifdef FUSION_WITH_FCITX5
  const char *usage = "Usage: stage4a "
                      "<protocol|pipeendpoint|loopback|hostserver|wp3singleton|wp3shutdown|"
                      "wp4stopevent|wp4sessionend|hostdiag|integration>\n";
#else
  const char *usage = "Usage: stage4a "
                      "<protocol|pipeendpoint|loopback|hostserver|wp3singleton|wp3shutdown|"
                      "wp4stopevent|wp4sessionend|hostdiag>\n";
#endif
  if (argc < 2) {
    std::fprintf(stderr, "%s", usage);
    return 1;
  }
  std::string mode = argv[1];
  try {
    if (mode == "protocol")
      test_protocol();
    else if (mode == "pipeendpoint")
      test_pipe_endpoint();
    else if (mode == "loopback")
      test_loopback();
    else if (mode == "hostserver")
      test_hostserver();
    else if (mode == "wp3singleton")
      test_singleton_claim();
    else if (mode == "wp3shutdown")
      test_shutdown_order();
    else if (mode == "wp4stopevent")
      test_stop_event();
    else if (mode == "wp4sessionend")
      test_session_end();
    else if (mode == "hostdiag")
      test_host_diag();
    else if (mode == "integration") {
#ifdef FUSION_WITH_FCITX5
      test_integration();
#else
      std::fprintf(stderr, "integration mode requires the FUSION_WITH_FCITX5 build\n");
      return 1;
#endif
    } else {
      std::fprintf(stderr, "Unknown mode: %s\n%s", mode.c_str(), usage);
      return 1;
    }
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL %s: %s\n", mode.c_str(), e.what());
    return 1;
  }
}
