// Stage 4A IPC tests: protocol serialization, loopback pipe, and full integration.
//
// Usage: stage4a <protocol|loopback|integration>
//
// - protocol: serialization round-trip, bounds, malformed input (no pipe, no Fcitx)
// - loopback: real Named Pipe with a CallbackBackend server thread (no Fcitx)
// - integration: full FusionHost.exe + Fcitx backend (requires Fcitx runtime)

#include "fusion/backend.hpp"
#include "fusion/ipc_protocol.hpp"
#include "ipc/ipc_backend.hpp"
#include "ipc/pipe_server.hpp"
#include "tsf/semantic.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
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

  // u64 round-trip
  {
    std::vector<std::uint8_t> buf;
    serialize(buf, std::uint64_t{0xDEADBEEFCAFE1234ULL});
    auto v = deserialize_u64(buf.data(), buf.size());
    require(v && *v == 0xDEADBEEFCAFE1234ULL, "u64 round-trip");
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

  std::cout << "PASS protocol serialization, bounds, and malformed input\n";
}

// ---------------------------------------------------------------------------
// Layer 2: Loopback pipe test with CallbackBackend
// ---------------------------------------------------------------------------

class CallbackBackend final : public Backend {
public:
  BackendBatch next;
  std::uint64_t sid = 42;

  bool bind(Binding) override { return true; }
  bool reset(Binding) override { return true; }
  void unbind(Binding) override {}
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
  if (!server.wait_for_client(std::chrono::milliseconds(10000))) {
    done = true;
    return;
  }
  while (done.load() == false && server.client_connected()) {
    auto frame = server.recv_frame(std::chrono::milliseconds(1000));
    if (!frame)
      break;
    WireFrame response;
    switch (frame->type) {
    case MessageType::SessionIdRequest: {
      response.type = MessageType::SessionIdResponse;
      serialize(response.payload, backend.session_id());
      break;
    }
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
    case MessageType::HealthyRequest: {
      response.type = MessageType::HealthyResponse;
      serialize(response.payload, backend.healthy());
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
  std::wstring pipe_name = L"\\\\.\\pipe\\FusionHost-ipc-test-loopback";
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
  server.shutdown();
  server_thread.join();

  // Now test SemanticController over IPC loopback
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
    server2.shutdown();
    server_thread2.join();
  }

  std::cout << "PASS loopback pipe with CallbackBackend and SemanticController\n";
}

// ---------------------------------------------------------------------------
// Layer 3: Full integration test (requires FusionHost.exe + Fcitx)
// ---------------------------------------------------------------------------

void test_integration() {
  // This test requires FusionHost.exe to be running or auto-launchable.
  // For now, we just verify the IPC backend can connect to a running host.
  auto backend = make_ipc_backend();
  if (!backend) {
    std::cout << "SKIP integration (FusionHost not reachable)\n";
    return;
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
    std::fprintf(stderr, "Usage: stage4a <protocol|loopback|integration>\n");
    return 1;
  }
  std::string mode = argv[1];
  try {
    if (mode == "protocol")
      test_protocol();
    else if (mode == "loopback")
      test_loopback();
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
