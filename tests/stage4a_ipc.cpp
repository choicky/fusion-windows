// Stage 4A IPC tests: protocol serialization, loopback pipe, HostServer, and the
// end-to-end FusionHost.exe integration.
//
// Usage: stage4a <protocol|loopback|hostserver|integration>
//
// - protocol: serialization round-trip, bounds, malformed input, trailing byte rejection
// - loopback: real Named Pipe with a CallbackBackend server thread (no Fcitx)
// - hostserver: real HostServer with a fake Backend that mimics FcitxBackend
//   admission (duplicate bind rejected, every call fails on admission contention):
//   concurrent multi-client bind/send with context remapping, disconnect
//   isolation, transport-failure separation, healthy() performs no IPC, and
//   stop()/join shutdown.
// - integration: full FusionHost.exe + Fcitx backend over IPC (only in the
//   FUSION_WITH_FCITX5 build; requires a running FusionHost.exe and the Fcitx
//   runtime). Set FUSION_STAGE4A_SKIP_INTEGRATION=1 to skip instead of fail.
//
// protocol/loopback/hostserver build and run in the Mock-only build too: the
// transport, the IpcBackend client and the HostServer do not depend on Fcitx.

#include "fusion/backend.hpp"
#include "fusion/ipc_protocol.hpp"
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

  // Batch with candidates
  {
    BackendBatch batch;
    batch.backend_session_id = 1;
    batch.binding = {1, 1, 1};
    batch.stimulus_id = 1;
    batch.disposition = BackendBatch::Disposition::Consumed;
    BackendOperation op;
    op.type = BackendOperation::Type::CandidateSnapshot;
    op.text = "candidates";
    op.candidates = {"a", "b", "c"};
    batch.operations.push_back(op);
    std::vector<std::uint8_t> buf;
    serialize(buf, batch);
    auto decoded = deserialize_batch(buf.data(), buf.size());
    require(decoded && decoded->operations.size() == 1 &&
                decoded->operations[0].candidates.size() == 3 &&
                decoded->operations[0].candidates[0] == "a" &&
                decoded->operations[0].candidates[2] == "c",
            "batch with candidates round-trip");
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

  // Protocol version constant
  {
    require(protocol_version == 1, "protocol_version is 1");
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
    if (!server.send_frame(response))
      break;
  }
  done = true;
}

KeyAction physical(char character) {
  return translate_key(static_cast<unsigned char>(character), false, false, false, character);
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
      // Respond with version=0 (mismatch).
      WireFrame resp;
      resp.type = MessageType::HelloResponse;
      detail::write_u32(resp.payload, 0);
      detail::write_u64(resp.payload, 0);
      s.server().send_frame(resp);
    });

    // Send a Hello with wrong version.
    PipeClient raw_client(pipe_name);
    require(raw_client.connect(std::chrono::milliseconds(5000)), "version client connect");
    std::vector<std::uint8_t> hello_payload;
    detail::write_u32(hello_payload, 999); // wrong version
    require(raw_client.send_request(MessageType::HelloRequest, hello_payload),
            "version hello send");
    auto resp = raw_client.recv_response(std::chrono::milliseconds(5000));
    require(resp && resp->type == MessageType::HelloResponse, "version response received");
    detail::Reader r2(resp->payload.data(), resp->payload.size());
    auto server_version = r2.read_u32();
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

// Pre-write failure (transport never saw the request) → Forward with identity
// preserved; post-write failure (request on the wire, response lost) → Reject.
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

void test_hostserver() {
  test_hostserver_multi_client();
  test_hostserver_transport_failure();
  test_hostserver_healthy_no_ipc();
  test_hostserver_shutdown();
  std::cout << "PASS HostServer: multi-client remapping, disconnect isolation, transport "
               "failure separation, healthy() no IPC, clean stop/join\n";
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
  const char *usage = "Usage: stage4a <protocol|loopback|hostserver|integration>\n";
#else
  const char *usage = "Usage: stage4a <protocol|loopback|hostserver>\n";
#endif
  if (argc < 2) {
    std::fprintf(stderr, "%s", usage);
    return 1;
  }
  std::string mode = argv[1];
  try {
    if (mode == "protocol")
      test_protocol();
    else if (mode == "loopback")
      test_loopback();
    else if (mode == "hostserver")
      test_hostserver();
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
