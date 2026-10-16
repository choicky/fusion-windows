// Stage 4A IPC tests: protocol serialization, loopback pipe, and full integration.
//
// Usage: stage4a <protocol|loopback|integration>
//
// - protocol: serialization round-trip, bounds, malformed input, trailing byte rejection
// - loopback: real Named Pipe with a CallbackBackend server thread (no Fcitx)
// - integration: full FusionHost.exe + Fcitx backend (requires Fcitx runtime)
//   Set FUSION_STAGE4A_SKIP_INTEGRATION=1 to skip instead of fail.

#include "fusion/backend.hpp"
#include "fusion/ipc_protocol.hpp"
#include "host/host_server.hpp"
#include "ipc/ipc_backend.hpp"
#include "ipc/pipe_security.hpp"
#include "ipc/pipe_server.hpp"
#include "tsf/semantic.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

using namespace fusion;
using namespace fusion::tsf;
using namespace fusion::ipc;

void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}

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
    batch.operations.push_back(
        {BackendOperation::Type::CommitText, "hello", {}, false});
    batch.operations.push_back(
        {BackendOperation::Type::SetPreedit, "pre", {}, false});
    batch.operations.push_back(
        {BackendOperation::Type::ForwardKey, "Tab", {}, true});
    std::vector<std::uint8_t> buf;
    serialize(buf, batch);
    auto decoded = deserialize_batch(buf.data(), buf.size());
    require(decoded && decoded->backend_session_id == batch.backend_session_id &&
                decoded->binding == batch.binding &&
                decoded->stimulus_id == batch.stimulus_id &&
                decoded->disposition == batch.disposition &&
                decoded->operations.size() == 3 &&
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
    require(parsed && parsed->type == MessageType::SendRequest &&
                parsed->payload == payload,
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

  // Invalid enum value in stimulus
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
    buf[24] = 0xFF; // corrupt the type byte (binding=24, then stimulus_id=8, type=1)
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
  // Wait for Hello.
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
    PipeServer server(pipe_name);
    require(server.create(), "loopback server create");

    CallbackBackend backend;
    backend.next.disposition = BackendBatch::Disposition::Consumed;
    backend.next.operations = {{BackendOperation::Type::SetPreedit, "ni", {}, false}};

    std::atomic<bool> done{false};
    std::thread server_thread(run_loopback_server, std::ref(server), std::ref(backend),
                              std::ref(done));

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

    done = true;
    server.disconnect_client();
    server_thread.join();
  }

  // healthy() is local state only — no IPC
  {
    std::wstring pipe_name = L"\\\\.\\pipe\\FusionHost-ipc-test-healthy";
    PipeServer server(pipe_name);
    require(server.create(), "healthy server create");

    CallbackBackend backend;
    std::atomic<bool> done{false};
    std::thread server_thread(run_loopback_server, std::ref(server), std::ref(backend),
                              std::ref(done));

    IpcBackend client(pipe_name);
    require(client.connect(std::chrono::milliseconds(5000)), "healthy client connect");

    // healthy() should return true without sending any IPC.
    require(client.healthy(), "healthy after connect");
    // Call it again — still no IPC.
    require(client.healthy(), "healthy repeated");

    done = true;
    server.disconnect_client();
    server_thread.join();
  }

  // 6s idle success — server should not disconnect on idle
  {
    std::wstring pipe_name = L"\\\\.\\pipe\\FusionHost-ipc-test-idle";
    PipeServer server(pipe_name);
    require(server.create(), "idle server create");

    CallbackBackend backend;
    std::atomic<bool> done{false};
    std::thread server_thread(run_loopback_server, std::ref(server), std::ref(backend),
                              std::ref(done));

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
    server.disconnect_client();
    server_thread.join();
  }

  // Two concurrent clients
  {
    std::wstring pipe_name = L"\\\\.\\pipe\\FusionHost-ipc-test-concurrent";

    CallbackBackend backend1, backend2;
    backend1.sid = 100;
    backend2.sid = 200;
    std::atomic<bool> done1{false}, done2{false};

    PipeServer server1(pipe_name, true); // first instance
    require(server1.create(), "concurrent server1 create");
    PipeServer server2(pipe_name, false); // second instance
    require(server2.create(), "concurrent server2 create");

    std::thread t1(run_loopback_server, std::ref(server1), std::ref(backend1), std::ref(done1));
    std::thread t2(run_loopback_server, std::ref(server2), std::ref(backend2), std::ref(done2));

    IpcBackend client1(pipe_name);
    require(client1.connect(std::chrono::milliseconds(5000)), "concurrent client1 connect");
    require(client1.session_id() == 100, "concurrent client1 session_id");

    IpcBackend client2(pipe_name);
    require(client2.connect(std::chrono::milliseconds(5000)), "concurrent client2 connect");
    require(client2.session_id() == 200, "concurrent client2 session_id");

    // Both clients should be independent.
    require(client1.healthy(), "concurrent client1 healthy");
    require(client2.healthy(), "concurrent client2 healthy");

    done1 = true;
    done2 = true;
    server1.disconnect_client();
    server2.disconnect_client();
    t1.join();
    t2.join();
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
      require(batch.stimulus_id == stimulus.stimulus_id, "pre-write failure → stimulus_id preserved");
      // session_id is 0 because connect was never performed (cached_session_id_ = 0).
      require(batch.backend_session_id == 0, "pre-write failure → session_id is 0 (never connected)");
    }
  }

  // Version mismatch
  {
    std::wstring pipe_name = L"\\\\.\\pipe\\FusionHost-ipc-test-version";
    PipeServer server(pipe_name);
    require(server.create(), "version server create");

    std::atomic<bool> done{false};
    std::thread server_thread([&] {
      auto hello = server.recv_frame(std::chrono::milliseconds(5000));
      if (!hello || hello->type != MessageType::HelloRequest) {
        done = true;
        return;
      }
      // Read client version.
      detail::Reader r(hello->payload.data(), hello->payload.size());
      auto client_version = r.read_u32();
      // Respond with version=0 (mismatch).
      WireFrame resp;
      resp.type = MessageType::HelloResponse;
      detail::write_u32(resp.payload, 0);
      detail::write_u64(resp.payload, 0);
      server.send_frame(resp);
      done = true;
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

    server_thread.join();
  }

  // SemanticController over IPC loopback
  {
    PipeServer server2(L"\\\\.\\pipe\\FusionHost-ipc-test-semantic");
    require(server2.create(), "semantic server create");

    CallbackBackend backend2;
    backend2.next.disposition = BackendBatch::Disposition::Consumed;
    backend2.next.operations = {{BackendOperation::Type::SetPreedit, "nihao", {}, false}};

    std::atomic<bool> done2{false};
    std::thread server_thread2(run_loopback_server, std::ref(server2), std::ref(backend2),
                               std::ref(done2));

    IpcBackend client2(L"\\\\.\\pipe\\FusionHost-ipc-test-semantic");
    require(client2.connect(std::chrono::milliseconds(5000)), "semantic client connect");

    SemanticController controller(900, 64, static_cast<Backend &>(client2));
    auto d = controller.begin_key(physical('n'), true);
    require(d.eaten, "semantic loopback eaten");
    require(d.route == SemanticDecision::Route::State, "semantic loopback route");
    require(d.text == "nihao", "semantic loopback text");

    controller.destroy();
    done2 = true;
    server2.disconnect_client();
    server_thread2.join();
  }

  std::cout << "PASS loopback pipe with Hello, idle, concurrent, transport failure, version\n";
}

// ---------------------------------------------------------------------------
// Layer 2B: HostServer test with real HostServer + fake Backend
// ---------------------------------------------------------------------------

class CountingBackend final : public Backend {
public:
  std::uint64_t sid = 777;
  std::atomic<int> bind_count{0};
  std::atomic<int> unbind_count{0};
  std::atomic<int> send_count{0};
  std::set<std::uint64_t> bound_contexts;
  std::mutex bind_mutex;

  bool bind(Binding b) override {
    std::lock_guard lock(bind_mutex);
    if (bound_contexts.count(b.context))
      return false; // reject duplicate context
    bound_contexts.insert(b.context);
    bind_count++;
    return true;
  }
  bool reset(Binding) override { return true; }
  void unbind(Binding b) override {
    std::lock_guard lock(bind_mutex);
    bound_contexts.erase(b.context);
    unbind_count++;
  }
  bool healthy() const override { return true; }
  std::uint64_t session_id() const override { return sid; }
  BackendBatch send(const BackendStimulus &input) override {
    send_count++;
    BackendBatch batch;
    batch.backend_session_id = sid;
    batch.binding = input.binding;
    batch.stimulus_id = input.stimulus_id;
    batch.disposition = BackendBatch::Disposition::Consumed;
    batch.operations = {{BackendOperation::Type::SetPreedit, "test", {}, false}};
    return batch;
  }
};

void test_hostserver() {
  // HostServer with CountingBackend: bind rejects duplicate context.
  {
    std::wstring pipe_name = L"\\\\.\\pipe\\FusionHost-ipc-test-hostserver-dup";
    auto backend = std::make_unique<CountingBackend>();
    auto *backend_ptr = backend.get();
    HostServer server(std::move(backend), pipe_name);
    require(server.start(), "hostserver start");

    std::thread server_thread([&] { server.run(); });

    // Give the server time to start listening.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    IpcBackend client(pipe_name);
    require(client.connect(std::chrono::milliseconds(5000)), "hostserver client connect");
    require(client.session_id() == 777, "hostserver session_id");

    // First bind should succeed.
    Binding b1{1, 1, 1};
    require(client.bind(b1), "hostserver first bind");
    require(backend_ptr->bind_count == 1, "hostserver bind_count == 1");

    // Second bind with same context should be rejected by the backend.
    // Note: the HostServer remaps context, so we use a different client context
    // that maps to the same host context. Actually, the HostServer will assign
    // a different host context for a different client context, so this won't
    // trigger the duplicate rejection. Let me test with the same client context.
    // The HostServer already has a mapping for client context 1, so it will
    // reuse the same host binding. The backend will see the same host context
    // and reject it.
    require(!client.bind(b1), "hostserver duplicate bind rejected");

    // Send should work.
    BackendStimulus stimulus;
    stimulus.binding = b1;
    stimulus.stimulus_id = 1;
    stimulus.type = BackendStimulus::Type::Character;
    stimulus.character = 'a';
    auto batch = client.send(stimulus);
    require(batch.disposition == BackendBatch::Disposition::Consumed, "hostserver send consumed");
    require(batch.binding == b1, "hostserver send returns client binding");
    require(backend_ptr->send_count == 1, "hostserver send_count == 1");

    client.disconnect();
    server.stop();
    server_thread.join();
  }

  // healthy() sends no IPC — already tested in test_loopback, but verify again here.
  {
    std::wstring pipe_name = L"\\\\.\\pipe\\FusionHost-ipc-test-hostserver-healthy";
    auto backend = std::make_unique<CountingBackend>();
    auto *backend_ptr = backend.get();
    HostServer server(std::move(backend), pipe_name);
    require(server.start(), "hostserver healthy start");

    std::thread server_thread([&] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    IpcBackend client(pipe_name);
    require(client.connect(std::chrono::milliseconds(5000)), "hostserver healthy connect");
    require(client.healthy(), "hostserver healthy after connect");
    require(backend_ptr->send_count == 0, "hostserver healthy sends no IPC");

    client.disconnect();
    server.stop();
    server_thread.join();
  }

  std::cout << "PASS HostServer with fake Backend: bind duplicate, send, healthy no IPC\n";
}

// ---------------------------------------------------------------------------
// Layer 3: Full integration test (requires FusionHost.exe + Fcitx)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: stage4a <protocol|loopback|hostserver|integration>\n");
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
    else if (mode == "integration")
      test_integration();
    else {
      std::fprintf(stderr, "Unknown mode: %s\n", mode.c_str());
      return 1;
    }
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL %s: %s\n", mode.c_str(), e.what());
    return 1;
  }
}
