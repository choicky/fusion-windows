// Stage 4B tests: Host failure, reconnect, semantic readiness and incarnation
// fencing on the real product classes (PipeClient/IpcBackend/HostServer and the
// Stage 2.5/3C SemanticController).
//
// Usage: stage4b <link|failover|cycles|fencing|contexts|semantic>
//
// - link:     the explicit state ladder (transport disconnected -> transport
//             connected -> protocol/Host incarnation established), bounded
//             ensure_link, idempotence, and the preserved Stage 4A
//             definitely-not-written ownership boundary.
// - failover: Host death while idle; the target side survives; the dead
//             incarnation is observable; a restarted Host is reconnected through
//             the bounded path; a new incarnation and semantic readiness are
//             established; new input works.
// - cycles:   repeated Host loss/restart cycles: distinct incarnations, one
//             connection generation per establishment, no leaked connection or
//             request state, no replay across cycles.
// - fencing:  a result that does not belong to the connection's established
//             incarnation is rejected deterministically (not by timing), and an
//             uncertain/certain-loss request is never replayed.
// - contexts: context isolation across an incarnation change; a context that has
//             not re-established its binding cannot reach the new Host or
//             another context; no cross-context effect.
// - semantic: the Stage 4A semantic controller on the real IPC path: stale STATE
//             from the dead incarnation cannot become current STATE for the new
//             one, owned/uncertain accounting survives the change, and an
//             unresolved EFFECT is never converted into PASS.
//
// All modes build and run in the Mock-only build too: the transport, the
// IpcBackend client, the HostServer and the semantic controller are independent
// of Fcitx. The Host-side backend here is a deterministic test double.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "fusion/backend.hpp"
#include "fusion/ipc_protocol.hpp"
#include "host/host_server.hpp"
#include "ipc/ipc_backend.hpp"
#include "ipc/pipe_security.hpp"
#include "ipc/pipe_server.hpp"
#include "tsf/semantic.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace fusion;
using namespace fusion::ipc;
using namespace fusion::tsf;

namespace {

constexpr std::chrono::milliseconds kWait{10000};

// Scripted Hello identities: two different Host incarnations and one unrelated
// incarnation used to prove that a foreign result is refused.
constexpr std::uint64_t kIncarnationA = 0xA100000000000001ull;
constexpr std::uint64_t kIncarnationB = 0xB200000000000002ull;
constexpr std::uint64_t kIncarnationForeign = 0xF0000000000000FFull;

void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}

std::wstring pipe_name(const wchar_t *suffix) {
  return std::wstring(L"\\\\.\\pipe\\FusionHost-stage4b-") + suffix;
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

// The pipe name is free again: a restarted Host creates its first instance with
// FILE_FLAG_FIRST_PIPE_INSTANCE, so the previous Host must be fully gone.
bool pipe_absent(const std::wstring &name) {
  HANDLE handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              0, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return GetLastError() == ERROR_FILE_NOT_FOUND;
  CloseHandle(handle);
  return false;
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

BackendStimulus space_stimulus(const Binding &binding, std::uint64_t stimulus_id) {
  BackendStimulus stimulus;
  stimulus.binding = binding;
  stimulus.stimulus_id = stimulus_id;
  stimulus.type = BackendStimulus::Type::Space;
  stimulus.keysym = 0x20;
  return stimulus;
}

KeyAction physical(char character) {
  // Windows virtual keys are upper-case; the character is the translated input.
  const auto upper =
      static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(character)));
  return translate_key(upper, false, false, false, character);
}

KeyAction space_key() { return translate_key(kVkSpace, false, false, false); }

// A key outside the development vocabulary with no keysym: a genuine PASS
// candidate that the real frontend does not route to the engine. A bare Shift is no
// longer such a key — it is relayed for Fcitx's own modifier-only binding
// (Hotkey/AltTriggerKeys, stage3c) — so this uses another modifier that stays
// unrouted.
KeyAction unrouted_pass_key() { return translate_key(0x11 /*VK_CONTROL*/, false, false, false); }

// --- Host-side response scripting -------------------------------------------

void send_enveloped(PipeServer &pipe, MessageType type, std::uint64_t incarnation,
                    const std::vector<std::uint8_t> &body) {
  WireFrame frame;
  frame.type = type;
  frame.payload = envelop_response(incarnation, body);
  if (!pipe.send_frame(frame))
    throw std::runtime_error("scripted send failed");
}

void send_hello(PipeServer &pipe, std::uint64_t incarnation, std::uint32_t version,
                std::uint64_t session_id) {
  std::vector<std::uint8_t> body;
  detail::write_u32(body, version);
  detail::write_u64(body, session_id);
  send_enveloped(pipe, MessageType::HelloResponse, incarnation, body);
}

// Raw pipe server plus its serving thread (scripted per test).
class RawServer {
public:
  explicit RawServer(std::wstring name) : server_(std::make_unique<PipeServer>(std::move(name))) {}

  bool create() { return server_->create(); }
  PipeServer &pipe() { return *server_; }
  // Closes the server end, dropping the transport under the client.
  void close() { server_.reset(); }

  template <typename Body> void serve(Body body) {
    thread_ = std::thread([this, body] {
      try {
        body(*this);
      } catch (...) {
        // A scripted assertion failure must surface on the test thread, so only
        // the failure flag is propagated from here.
        failed_ = true;
      }
    });
  }

  bool failed() const { return failed_; }

  ~RawServer() {
    if (server_)
      server_->disconnect_client();
    if (thread_.joinable())
      thread_.join();
  }

private:
  std::unique_ptr<PipeServer> server_;
  std::thread thread_;
  std::atomic<bool> failed_{false};
};

// HostServer plus its accept-loop thread.
class RunningHost {
public:
  RunningHost(std::unique_ptr<Backend> backend, std::wstring name)
      : server_(std::move(backend), std::move(name)) {}

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

// HostServer creates its first pipe instance asynchronously, so a restarted Host
// is not listening the instant start() returns. Poll the instance without
// consuming it: a missing Host is exactly what the product path fails open on,
// so the library itself must not retry.
bool wait_for_host_pipe(const std::wstring &name, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (WaitNamedPipeW(name.c_str(), 1))
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WaitNamedPipeW(name.c_str(), 1) != FALSE;
}

// The same retry, expressed through the product reconnect entry point.
bool connect_with_retry(IpcBackend &client, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (client.ensure_link(std::chrono::milliseconds(200)))
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return client.ensure_link(std::chrono::milliseconds(200));
}

// A Host that dies while the connection is idle is not observable locally: the
// first request is what discovers it, and Stage 4A requires that request to stay
// definitely-not-written (Forward, caller-owned). This drives exactly that
// request so the loss becomes an explicit, observable link state.
void observe_transport_loss(IpcBackend &client, const Binding &binding) {
  const BackendBatch batch = client.send(character_stimulus(binding, 999, 'x'));
  require(batch.disposition == BackendBatch::Disposition::Forward,
          "a request with no Host is definitely-not-written (Forward)");
  require(client.link_status().state == LinkState::Disconnected,
          "the Host loss becomes an observable disconnected link");
}

// --- Deterministic Host-side engine double ----------------------------------
//
// Faithful enough to observe identity, ordering and replay: per-context preedit
// state, monotonic stimulus admission, and the ordered callback vocabulary
// (SetPreedit / CommitText / ForwardKey) the real frontend produces. It is a
// test double, not an engine: an empty-preedit Space commits a fixed marker so
// that a commit-only dispatch stays observable.
class FakeEngine final : public Backend {
public:
  explicit FakeEngine(std::uint64_t session) : sid_(session) {}

  bool bind(Binding binding) override {
    std::lock_guard lock(mutex_);
    ++bind_calls_;
    if (contexts_.count(binding.context) != 0)
      return false;
    contexts_[binding.context] = std::string();
    last_stimulus_[binding.context] = 0;
    return true;
  }

  bool reset(Binding binding) override {
    std::lock_guard lock(mutex_);
    ++reset_calls_;
    auto it = contexts_.find(binding.context);
    if (it == contexts_.end())
      return false;
    it->second.clear();
    return true;
  }

  void unbind(Binding binding) override {
    std::lock_guard lock(mutex_);
    contexts_.erase(binding.context);
    last_stimulus_.erase(binding.context);
    unbound_.push_back(binding.context);
  }

  bool healthy() const override { return true; }
  std::uint64_t session_id() const override { return sid_; }

  BackendBatch send(const BackendStimulus &stimulus) override {
    std::lock_guard lock(mutex_);
    received_.push_back({stimulus.binding.context, stimulus.stimulus_id});
    BackendBatch batch;
    batch.backend_session_id = sid_;
    batch.binding = stimulus.binding;
    batch.stimulus_id = stimulus.stimulus_id;
    auto it = contexts_.find(stimulus.binding.context);
    if (it == contexts_.end() || stimulus.stimulus_id <= last_stimulus_[stimulus.binding.context]) {
      // Unknown context, or a stimulus that is not newer than the last admitted
      // one: an out-of-order/replayed request must never be served.
      batch.disposition = BackendBatch::Disposition::Reject;
      return batch;
    }
    last_stimulus_[stimulus.binding.context] = stimulus.stimulus_id;
    std::string &preedit = it->second;
    batch.disposition = BackendBatch::Disposition::Consumed;
    switch (stimulus.type) {
    case BackendStimulus::Type::Character:
      preedit.push_back(stimulus.character);
      batch.operations.push_back({BackendOperation::Type::SetPreedit, preedit, {}, false});
      break;
    case BackendStimulus::Type::Space:
      batch.operations.push_back({BackendOperation::Type::CommitText,
                                  preedit.empty() ? std::string("commit") : preedit,
                                  {},
                                  false});
      preedit.clear();
      break;
    case BackendStimulus::Type::Backspace:
      if (!preedit.empty())
        preedit.pop_back();
      batch.operations.push_back({BackendOperation::Type::SetPreedit, preedit, {}, false});
      break;
    case BackendStimulus::Type::Escape:
      preedit.clear();
      batch.operations.push_back({BackendOperation::Type::SetPreedit, std::string(), {}, false});
      break;
    case BackendStimulus::Type::Return: {
      BackendOperation forward{BackendOperation::Type::ForwardKey, "Return", {}, true};
      batch.operations.push_back(forward);
      batch.disposition = BackendBatch::Disposition::Forward;
      break;
    }
    }
    return batch;
  }

  std::size_t bound_count() const {
    std::lock_guard lock(mutex_);
    return contexts_.size();
  }
  int bind_calls() const {
    std::lock_guard lock(mutex_);
    return bind_calls_;
  }
  int reset_calls() const {
    std::lock_guard lock(mutex_);
    return reset_calls_;
  }
  std::string preedit(std::uint64_t context) const {
    std::lock_guard lock(mutex_);
    auto it = contexts_.find(context);
    return it == contexts_.end() ? std::string() : it->second;
  }
  std::vector<std::uint64_t> contexts() const {
    std::lock_guard lock(mutex_);
    std::vector<std::uint64_t> ids;
    for (const auto &[id, text] : contexts_) {
      (void)text;
      ids.push_back(id);
    }
    return ids;
  }
  std::vector<std::pair<std::uint64_t, std::uint64_t>> received() const {
    std::lock_guard lock(mutex_);
    return received_;
  }
  std::size_t received_count(std::uint64_t context) const {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(
        std::count_if(received_.begin(), received_.end(),
                      [context](const auto &entry) { return entry.first == context; }));
  }

private:
  mutable std::mutex mutex_;
  std::uint64_t sid_;
  std::map<std::uint64_t, std::string> contexts_;
  std::map<std::uint64_t, std::uint64_t> last_stimulus_;
  std::vector<std::uint64_t> unbound_;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> received_;
  int bind_calls_ = 0;
  int reset_calls_ = 0;
};

// ---------------------------------------------------------------------------
// link: the explicit state ladder and the preserved write-certainty boundary
// ---------------------------------------------------------------------------

void test_link() {
  const auto name = pipe_name(L"link");

  // (1) Nothing is claimed before a transport exists: no incarnation, not
  //     healthy, not established.
  IpcBackend client(name);
  require(client.link_status().state == LinkState::Disconnected,
          "fresh client reports transport disconnected");
  require(client.incarnation() == 0, "fresh client has no Host incarnation");
  require(!client.healthy(), "fresh client is not healthy");

  // (2) Transport connected is NOT protocol-established and NOT semantic ready.
  //     The server holds the handshake so the intermediate state is observable.
  RawServer server(name);
  require(server.create(), "link server create");
  std::atomic<bool> connected_seen{false};
  std::atomic<bool> handshake_allowed{false};
  std::atomic<int> frames{0};
  server.serve([&](RawServer &s) {
    if (!s.pipe().wait_for_client(kWait))
      return;
    connected_seen = true;
    while (!handshake_allowed.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto hello = s.pipe().recv_frame(kWait);
    if (!hello || hello->type != MessageType::HelloRequest)
      return;
    ++frames;
    send_hello(s.pipe(), kIncarnationA, protocol_version, 11);
    while (s.pipe().client_connected()) {
      if (!s.pipe().recv_frame(std::chrono::milliseconds(0)))
        break;
      ++frames;
    }
  });

  bool linked = false;
  std::thread worker([&] { linked = client.ensure_link(kWait); });
  require(wait_until([&] { return connected_seen.load(); }, kWait),
          "server observed the transport connect");
  require(wait_until([&] { return client.link_status().state == LinkState::Connected; }, kWait),
          "transport connected while the handshake is incomplete");
  require(client.incarnation() == 0, "no incarnation before the handshake completes");
  require(!client.healthy(), "connected != established != semantic ready");
  handshake_allowed = true;
  worker.join();

  require(linked, "ensure_link established the link");
  require(!server.failed(), "the scripted handshake completed");
  require(client.link_status().state == LinkState::Established, "protocol established");
  require(client.incarnation() == kIncarnationA, "Host incarnation published by the handshake");
  require(client.link_status().connection_generation == 1, "first establishment is generation 1");
  require(client.session_id() == 11, "backend session id comes from the handshake");
  require(client.healthy(), "established link is healthy");

  // (3) ensure_link is idempotent and does no I/O while established: only the
  //     Hello frame was ever seen by the server.
  const int frames_after_handshake = frames.load();
  require(frames_after_handshake == 1, "handshake wrote exactly one request frame");
  for (int i = 0; i < 5; ++i)
    require(client.ensure_link(kWait), "ensure_link is idempotent while established");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  require(frames.load() == frames_after_handshake,
          "ensure_link performed no I/O while already established");
  require(client.link_status().connection_generation == 1, "no new connection generation");

  // (4) The Stage 4A write-certainty boundary is unchanged: a request that
  //     provably reached no Host stays caller-owned Forward, and the loss is
  //     observable.
  server.close();
  const auto stimulus = character_stimulus({42, 1, 1}, 5, 'a');
  const BackendBatch batch = client.send(stimulus);
  require(batch.disposition == BackendBatch::Disposition::Forward,
          "Host loss before the request -> definitely-not-written Forward");
  require(batch.operations.empty(), "definitely-not-written -> no operations");
  require(batch.binding == stimulus.binding && batch.stimulus_id == stimulus.stimulus_id,
          "definitely-not-written -> identity preserved for caller accounting");
  require(client.link_status().state == LinkState::Disconnected,
          "the link returns to transport disconnected");
  require(!client.healthy(), "the client is unhealthy after the loss");
  require(client.incarnation() == kIncarnationA,
          "the dead incarnation stays identifiable while it is no longer usable");

  // (5) The same client object reconnects through the bounded path, and the
  //     dead incarnation stays identifiable while no longer being usable.
  RawServer server2(name);
  require(server2.create(), "link server2 create");
  server2.serve([&](RawServer &s) {
    if (!s.pipe().wait_for_client(kWait))
      return;
    auto hello = s.pipe().recv_frame(kWait);
    if (!hello || hello->type != MessageType::HelloRequest)
      return;
    send_hello(s.pipe(), kIncarnationB, protocol_version, 22);
    while (s.pipe().client_connected()) {
      if (!s.pipe().recv_frame(std::chrono::milliseconds(0)))
        break;
    }
  });
  require(client.ensure_link(kWait), "bounded reconnect succeeds");
  require(client.link_status().state == LinkState::Established, "re-established");
  require(client.incarnation() == kIncarnationB, "the new incarnation is published");
  require(client.link_status().connection_generation == 2,
          "one new connection generation per establishment");
  require(client.session_id() == 22, "session id re-read from the new handshake");
  server2.close();
  std::printf("PASS stage4b link\n");
}

// ---------------------------------------------------------------------------
// failover: Host death while idle, restart, bounded reconnect, new incarnation
// ---------------------------------------------------------------------------

void test_failover() {
  const auto name = pipe_name(L"failover");
  IpcBackend client(name);
  const Binding binding{7, 1, 1};

  auto engine1 = std::make_unique<FakeEngine>(700);
  FakeEngine *fake1 = engine1.get();
  std::uint64_t incarnation1 = 0;
  std::uint64_t host_context1 = 0;
  int fake1_bind_calls = 0;
  std::size_t fake1_received = 0;
  {
    RunningHost host(std::move(engine1), name);
    require(host.start(), "failover host1 start");
    require(wait_for_host_pipe(name, kWait), "failover host1 listens");
    incarnation1 = host.server().incarnation();
    require(incarnation1 != 0, "Host has an incarnation identity");
    require(connect_with_retry(client, kWait), "failover connect");
    require(client.incarnation() == incarnation1, "client sees the Host incarnation");
    require(client.bind(binding), "failover bind");

    const auto batch = client.send(character_stimulus(binding, 1, 'n'));
    require(batch.disposition == BackendBatch::Disposition::Consumed, "baseline send consumed");
    require(batch.operations.size() == 1 && batch.operations[0].text == "n",
            "baseline callback received");
    host_context1 = fake1->contexts().at(0);
    require(fake1->preedit(host_context1) == "n", "baseline Host state");
    fake1_bind_calls = fake1->bind_calls();
    fake1_received = fake1->received_count(host_context1);

    // Host death while the connection is idle.
    host.stop_and_join();
  }

  require(pipe_absent(name), "failover host1 released the pipe");
  observe_transport_loss(client, binding);
  require(client.incarnation() == incarnation1,
          "the dead incarnation stays identifiable while it is unusable");

  // A later usable Host incarnation becomes reachable through the bounded path.
  auto engine2 = std::make_unique<FakeEngine>(800);
  FakeEngine *fake2 = engine2.get();
  RunningHost host2(std::move(engine2), name);
  require(host2.start(), "failover host2 start");
  require(wait_for_host_pipe(name, kWait), "failover host2 listens");
  const std::uint64_t incarnation2 = host2.server().incarnation();
  require(incarnation2 != incarnation1, "a restarted Host is a different incarnation");

  require(connect_with_retry(client, kWait), "bounded reconnect to the new Host");
  require(client.incarnation() == incarnation2, "the new incarnation is established");
  require(client.bind(binding), "context rebind on the new incarnation");
  require(fake2->preedit(fake2->contexts().at(0)).empty(),
          "the new incarnation starts from empty semantic state");

  const auto resumed = client.send(character_stimulus(binding, 3, 'a'));
  require(resumed.disposition == BackendBatch::Disposition::Consumed, "new input is served");
  require(resumed.operations.size() == 1 && resumed.operations[0].text == "a",
          "new input is served by the new incarnation");
  require(fake2->preedit(fake2->contexts().at(0)) == "a",
          "the new incarnation's projection is not the dead one's");
  require(fake1_bind_calls == 1 && fake2->bind_calls() == 1,
          "exactly one context binding per incarnation");
  require(fake1_received == 1,
          "the dead incarnation received exactly the stimuli sent to it (no replay)");
  host2.stop_and_join();
  std::printf("PASS stage4b failover\n");
}

// ---------------------------------------------------------------------------
// cycles: repeated Host loss/restart without stale generation reuse or leaks
// ---------------------------------------------------------------------------

void test_cycles() {
  const auto name = pipe_name(L"cycles");
  IpcBackend client(name);
  const Binding binding{9, 1, 1};

  // The first data-pipe descriptor initialises the AppContainer SID derivation
  // (userenv), which takes a fixed set of process-wide handles once. It is warmed here so
  // the measurement below is about the reconnect cycles and not about that one-time cost:
  // a second hundred descriptors add nothing.
  {
    SECURITY_ATTRIBUTES warm = make_data_pipe_security(true);
    free_pipe_security(warm);
  }
  DWORD handles_before = 0;
  GetProcessHandleCount(GetCurrentProcess(), &handles_before);

  constexpr int kCycles = 4;
  std::set<std::uint64_t> incarnations;
  std::uint64_t next_stimulus = 0;
  std::uint64_t expected_generation = 0;

  for (int cycle = 0; cycle < kCycles; ++cycle) {
    auto engine = std::make_unique<FakeEngine>(900 + static_cast<std::uint64_t>(cycle));
    FakeEngine *fake = engine.get();
    RunningHost host(std::move(engine), name);
    require(host.start(), "cycles host start");
    require(wait_for_host_pipe(name, kWait), "cycles host listens");
    const std::uint64_t incarnation = host.server().incarnation();
    require(incarnations.insert(incarnation).second,
            "every Host restart is a distinct incarnation (no stale reuse)");

    require(connect_with_retry(client, kWait), "cycles reconnect");
    require(client.incarnation() == incarnation, "cycles incarnation matches the Host");
    ++expected_generation;
    require(client.link_status().connection_generation == expected_generation,
            "exactly one connection generation per establishment");
    require(client.bind(binding), "cycles bind");

    const std::uint64_t stimulus_a = ++next_stimulus;
    const std::uint64_t stimulus_b = ++next_stimulus;
    require(client.send(character_stimulus(binding, stimulus_a, 'x')).disposition ==
                BackendBatch::Disposition::Consumed,
            "cycles send accepted");
    require(client.send(character_stimulus(binding, stimulus_b, 'y')).disposition ==
                BackendBatch::Disposition::Consumed,
            "cycles second send accepted");

    require(fake->contexts().size() == 1, "cycles: one bound context per cycle");
    const std::uint64_t context = fake->contexts().at(0);
    require(fake->bind_calls() == 1,
            "cycles: the connection was bound exactly once on this incarnation");
    require(fake->received().size() == 2, "the cycle's Host received exactly its own stimuli");
    for (const auto &entry : fake->received()) {
      require(entry.first == context && (entry.second == stimulus_a || entry.second == stimulus_b),
              "no stimulus from another cycle reached this incarnation");
    }
    require(fake->preedit(context) == "xy", "cycles projection built from this cycle only");

    host.stop_and_join();
    require(pipe_absent(name), "cycles host released the pipe");
    const std::uint64_t orphan = ++next_stimulus;
    const auto lost = client.send(character_stimulus(binding, orphan, 'z'));
    require(lost.disposition == BackendBatch::Disposition::Forward,
            "cycles: request with no Host is definitely-not-written");
    require(fake->received().size() == 2, "no replay or late delivery after the Host is gone");
  }

  DWORD handles_after = 0;
  GetProcessHandleCount(GetCurrentProcess(), &handles_after);
  require(handles_after <= handles_before + 16, "repeated reconnect cycles do not leak handles");
  require(incarnations.size() == kCycles, "no incarnation was reused across cycles");
  std::printf("PASS stage4b cycles\n");
}

// ---------------------------------------------------------------------------
// fencing: a foreign-incarnation result can never satisfy current work
// ---------------------------------------------------------------------------

void test_fencing() {
  const auto name = pipe_name(L"fencing");

  // (1) The connection is established with incarnation A, but the SendResponse
  //     that comes back is stamped with a different incarnation. This must be
  //     refused (Reject), the link dropped, and the request not replayed.
  {
    RawServer server(name);
    require(server.create(), "fencing server create");
    std::atomic<int> send_requests{0};
    std::atomic<bool> answered{false};
    server.serve([&](RawServer &s) {
      if (!s.pipe().wait_for_client(kWait))
        return;
      auto hello = s.pipe().recv_frame(kWait);
      if (!hello || hello->type != MessageType::HelloRequest)
        return;
      send_hello(s.pipe(), kIncarnationA, protocol_version, 31);
      bool served = false;
      while (s.pipe().client_connected()) {
        auto frame = s.pipe().recv_frame(std::chrono::milliseconds(0));
        if (!frame)
          break;
        if (frame->type == MessageType::BindRequest) {
          std::vector<std::uint8_t> body;
          serialize(body, true);
          send_enveloped(s.pipe(), MessageType::BindResponse, kIncarnationA, body);
          continue;
        }
        if (frame->type != MessageType::SendRequest)
          break;
        ++send_requests;
        if (served) {
          // A replay would arrive here; the test asserts it never does.
          continue;
        }
        served = true;
        // A result that belongs to some other Host incarnation.
        BackendBatch foreign;
        foreign.backend_session_id = 31;
        foreign.disposition = BackendBatch::Disposition::Consumed;
        std::vector<std::uint8_t> body;
        serialize(body, foreign);
        send_enveloped(s.pipe(), MessageType::SendResponse, kIncarnationForeign, body);
        answered = true;
      }
    });

    IpcBackend client(name);
    require(client.ensure_link(kWait), "fencing client connect");
    const Binding binding{3, 1, 1};
    require(client.bind(binding), "fencing bind");
    const auto stimulus = character_stimulus(binding, 41, 'a');
    const BackendBatch batch = client.send(stimulus);
    require(answered.load(), "the foreign result was delivered");
    require(batch.disposition == BackendBatch::Disposition::Reject,
            "a foreign-incarnation result is never accepted");
    require(batch.operations.empty(), "a foreign-incarnation result carries no operation");
    require(batch.binding == stimulus.binding && batch.stimulus_id == stimulus.stimulus_id,
            "the refused stimulus keeps its identity for caller accounting");
    require(!client.healthy(), "a protocol-identity violation drops the link");
    require(client.link_status().state == LinkState::Disconnected,
            "the violating connection is not reused");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(send_requests.load() == 1, "the refused request was never replayed");
  }
  require(pipe_absent(name), "fencing server released the pipe");

  // (2) A response without the Stage 4B envelope is refused the same way.
  {
    RawServer server(name);
    require(server.create(), "fencing2 server create");
    server.serve([&](RawServer &s) {
      if (!s.pipe().wait_for_client(kWait))
        return;
      auto hello = s.pipe().recv_frame(kWait);
      if (!hello || hello->type != MessageType::HelloRequest)
        return;
      send_hello(s.pipe(), kIncarnationA, protocol_version, 32);
      while (s.pipe().client_connected()) {
        auto frame = s.pipe().recv_frame(std::chrono::milliseconds(0));
        if (!frame)
          break;
        if (frame->type == MessageType::BindRequest) {
          std::vector<std::uint8_t> body;
          serialize(body, true);
          send_enveloped(s.pipe(), MessageType::BindResponse, kIncarnationA, body);
          continue;
        }
        if (frame->type != MessageType::SendRequest)
          break;
        // A legacy/truncated response: body only, no incarnation envelope.
        BackendBatch legacy;
        legacy.backend_session_id = 32;
        legacy.disposition = BackendBatch::Disposition::Consumed;
        WireFrame response;
        response.type = MessageType::SendResponse;
        serialize(response.payload, legacy);
        s.pipe().send_frame(response);
      }
    });

    IpcBackend client(name);
    require(client.ensure_link(kWait), "fencing2 client connect");
    require(client.bind({3, 1, 1}), "fencing2 bind");
    const BackendBatch batch = client.send(character_stimulus({3, 1, 1}, 42, 'a'));
    require(batch.disposition == BackendBatch::Disposition::Reject,
            "a response without an incarnation envelope is refused");
    require(!client.healthy(), "the link is dropped");
  }
  require(pipe_absent(name), "fencing2 server released the pipe");

  // (3) A correct, fully enveloped round trip on a fresh connection is accepted:
  //     the refusals above are about identity, not about refusing everything.
  //     The next request carries a fresh stimulus id, never the refused one.
  {
    auto engine = std::make_unique<FakeEngine>(33);
    FakeEngine *fake = engine.get();
    RunningHost host(std::move(engine), name);
    require(host.start(), "fencing3 host start");
    require(wait_for_host_pipe(name, kWait), "fencing3 host listens");
    IpcBackend client(name);
    require(connect_with_retry(client, kWait), "fencing3 connect");
    require(client.link_status().connection_generation == 1, "fresh client starts at generation 1");
    const Binding binding{3, 1, 1};
    require(client.bind(binding), "fencing3 bind");
    const BackendBatch batch = client.send(character_stimulus(binding, 43, 'a'));
    require(batch.disposition == BackendBatch::Disposition::Consumed,
            "a current result is accepted");
    require(batch.operations.size() == 1 && batch.operations[0].text == "a",
            "the current incarnation's callback is accepted");
    require(fake->received().size() == 1 && fake->received()[0].second == 43,
            "exactly the new stimulus reached the Host");
    require(fake->received()[0].second != 41, "the refused stimulus was never reissued");
    host.stop_and_join();
  }
  std::printf("PASS stage4b fencing\n");
}

// ---------------------------------------------------------------------------
// contexts: isolation across an incarnation change
// ---------------------------------------------------------------------------

void test_contexts() {
  const auto name = pipe_name(L"contexts");
  IpcBackend client1(name);
  IpcBackend client2(name);
  const Binding binding1{10, 1, 1};
  const Binding binding2{20, 1, 1};

  auto engine1 = std::make_unique<FakeEngine>(1100);
  FakeEngine *fake1 = engine1.get();
  std::uint64_t context1_before = 0;
  std::uint64_t context2_before = 0;
  std::uint64_t context_second_connection = 0;
  std::size_t received_before_loss = 0;
  {
    RunningHost host(std::move(engine1), name);
    require(host.start(), "contexts host1 start");
    require(wait_for_host_pipe(name, kWait), "contexts host1 listens");
    require(connect_with_retry(client1, kWait) && connect_with_retry(client2, kWait),
            "contexts connect");
    require(client1.bind(binding1) && client2.bind(binding2), "contexts bind");
    require(fake1->bound_count() == 2, "each client owns a Host context");
    require(client1.send(character_stimulus(binding1, 1, 'o')).disposition ==
                BackendBatch::Disposition::Consumed,
            "context1 baseline");
    require(client2.send(character_stimulus(binding2, 1, 't')).disposition ==
                BackendBatch::Disposition::Consumed,
            "context2 baseline");
    require(fake1->contexts().size() == 2, "two distinct Host contexts");
    context1_before = fake1->contexts()[0];
    context2_before = fake1->contexts()[1];
    require(context1_before != context2_before, "the two contexts are isolated");
    require(client1.incarnation() == client2.incarnation(),
            "both connections observed the same Host incarnation");

    // A further connection to the *same* Host must be remapped to a new Host
    // context: context identity is per connection and is never rebound to an
    // existing Host context.
    IpcBackend extra(name);
    require(connect_with_retry(extra, kWait) && extra.bind(binding1),
            "a second connection to the same Host binds");
    require(fake1->bound_count() == 3, "the same client context maps to a new Host context");
    context_second_connection = fake1->contexts()[2];
    require(context_second_connection != context1_before &&
                context_second_connection != context2_before,
            "a new connection is remapped to a new Host context");
    require(client1.send(character_stimulus(binding1, 2, 'o')).disposition ==
                BackendBatch::Disposition::Consumed,
            "the first connection keeps its own Host context");

    received_before_loss = fake1->received().size();
    host.stop_and_join();
  }

  require(pipe_absent(name), "contexts host1 released the pipe");
  observe_transport_loss(client1, binding1);
  observe_transport_loss(client2, binding2);
  require(received_before_loss == 3, "no further work reached the dead incarnation");

  auto engine2 = std::make_unique<FakeEngine>(1200);
  FakeEngine *fake2 = engine2.get();
  RunningHost host2(std::move(engine2), name);
  require(host2.start(), "contexts host2 start");
  require(wait_for_host_pipe(name, kWait), "contexts host2 listens");
  require(host2.server().incarnation() != client1.incarnation(), "a new incarnation is running");

  // Only context 1 re-establishes its binding. Context 2 must not become able to
  // reach the new Host, and must not mutate context 1's Host context.
  require(connect_with_retry(client1, kWait) && client1.bind(binding1), "context1 rebinds");
  require(fake2->bound_count() == 1, "only context1 is bound on the new incarnation");
  const BackendBatch unbound = client2.send(character_stimulus(binding2, 3, 't'));
  require(unbound.disposition == BackendBatch::Disposition::Forward,
          "an unreconnected context cannot write to the new incarnation");
  require(fake2->received().empty(), "an unreconnected context causes no Host work at all");
  require(fake2->bound_count() == 1, "an unreconnected context causes no cross-context binding");

  require(client1.send(character_stimulus(binding1, 3, 'o')).disposition ==
              BackendBatch::Disposition::Consumed,
          "context1 works on the new incarnation");
  require(connect_with_retry(client2, kWait) && client2.bind(binding2), "context2 rebinds");
  require(fake2->bound_count() == 2, "both contexts are bound on the new incarnation");
  require(client2.send(character_stimulus(binding2, 4, 't')).disposition ==
              BackendBatch::Disposition::Consumed,
          "context2 works on the new incarnation");

  const auto received = fake2->received();
  require(received.size() == 2, "exactly the two post-reconnect stimuli reached the new Host");
  require(received[0].first != received[1].first,
          "each context was served by its own Host context");
  require(received[0].second == 3 && received[1].second == 4,
          "each stimulus was served exactly once, in order, by its own context");
  require(fake2->contexts().size() == 2, "no duplicated or cross-context materialization");
  host2.stop_and_join();
  std::printf("PASS stage4b contexts\n");
}

// ---------------------------------------------------------------------------
// semantic: the Stage 4A/3C controller on the real IPC path
// ---------------------------------------------------------------------------

// Routed-key helper: dispatch one key and settle the ordered frontend callbacks
// the way the TSF edit sessions do (AppliedReadback from the document authority).
SemanticDecision dispatch_key(SemanticController &controller, const KeyAction &action) {
  const SemanticDecision decision = controller.begin_key(action, true);
  for (const auto &work : decision.frontend) {
    if (work.operation.type == BackendOperation::Type::CommitText) {
      auto ticket = controller.open_effect(work.key);
      require(ticket.has_value(), "frontend effect ticket opened");
      require(controller.settle_effect(*ticket, EditResult::AppliedReadback) == Result::Accepted,
              "frontend effect settled");
    } else if (work.operation.type == BackendOperation::Type::SetPreedit) {
      require(controller.settle_state(work.key, EditResult::AppliedReadback) == Result::Accepted,
              "frontend state settled");
    }
  }
  return decision;
}

// Phase 1: the dead incarnation's Host-provided projection must not become
// current STATE for the new incarnation, while owned/ordering accounting survives.
void run_semantic_stale_state() {
  const auto name = pipe_name(L"semantic-state");
  IpcBackend client(name);
  std::unique_ptr<SemanticController> controller;
  Key pending_state_key{};
  std::uint64_t incarnation1 = 0;

  {
    auto engine1 = std::make_unique<FakeEngine>(2100);
    FakeEngine *fake1 = engine1.get();
    RunningHost host(std::move(engine1), name);
    require(host.start(), "semantic-state host1 start");
    require(wait_for_host_pipe(name, kWait), "semantic-state host1 listens");
    incarnation1 = host.server().incarnation();
    require(connect_with_retry(client, kWait), "semantic-state connect");
    // The product path creates the per-context controller only after the link is
    // established (resolve_or_create_entry gates on ensure_link).
    controller = std::make_unique<SemanticController>(901, 65, client, 1);
    require(controller->semantic_ready(), "semantic readiness established");
    require(controller->link_status().host_incarnation == incarnation1,
            "the controller is bound to the Host incarnation");

    require(dispatch_key(*controller, physical('n')).route == SemanticDecision::Route::State,
            "first STATE callback settled");
    // The second STATE stays owned and unsettled: it must survive the incarnation
    // change as Windows-side accounting, and must still order later work.
    const SemanticDecision second = controller->begin_key(physical('i'), true);
    require(second.route == SemanticDecision::Route::State && second.key.has_value(),
            "second STATE callback owned");
    pending_state_key = *second.key;
    require(controller->raw_buffer() == "ni", "client projection mirrors the Host projection");
    require(fake1->preedit(fake1->contexts().at(0)) == "ni", "Host projection");

    const std::size_t records_before = controller->core().records().size();
    host.stop_and_join();
    require(pipe_absent(name), "semantic-state host1 released the pipe");
    observe_transport_loss(client, controller->core().binding());
    require(!controller->semantic_ready(), "the loss fences semantic readiness");
    require(controller->core().records().size() == records_before,
            "a transport loss does not silently drop owned records");
  }

  auto engine2 = std::make_unique<FakeEngine>(2200);
  FakeEngine *fake2 = engine2.get();
  RunningHost host2(std::move(engine2), name);
  require(host2.start(), "semantic-state host2 start");
  require(wait_for_host_pipe(name, kWait), "semantic-state host2 listens");
  const std::uint64_t incarnation2 = host2.server().incarnation();
  require(incarnation2 != incarnation1, "a different Host incarnation is running");

  // A commit that the fresh engine answers with a commit-only callback: no
  // SetPreedit follows, so the client projection stays observable.
  const SemanticDecision resumed = controller->begin_key(space_key(), true);
  require(controller->semantic_ready(), "semantic readiness re-established");
  require(controller->link_status().host_incarnation == incarnation2,
          "the controller is rebound to the new incarnation");
  require(fake2->bind_calls() == 1, "the context was bound exactly once on the new incarnation");
  require(fake2->contexts().size() == 1, "one Host context on the new incarnation");
  const std::uint64_t host_context2 = fake2->contexts().at(0);
  require(fake2->preedit(host_context2).empty(),
          "the new incarnation's semantic state starts empty (nothing of the dead one is live)");
  require(resumed.route == SemanticDecision::Route::Effect && resumed.key.has_value(),
          "the commit is routed as an EFFECT");
  require(resumed.text == "commit",
          "the new incarnation answered from its own fresh state, not the dead one's projection");
  require(controller->raw_buffer().empty(),
          "the dead incarnation's Host projection is not current STATE for the new one");

  const Record *effect = nullptr;
  for (const auto &record : controller->core().records()) {
    if (record.key == *resumed.key)
      effect = &record;
  }
  require(effect != nullptr, "the new incarnation's effect is accounted");
  require(effect->required_state == 0,
          "the new effect carries no stale STATE dependency from the dead incarnation");

  // No new-incarnation effect overtakes the preserved unresolved prior STATE.
  require(!controller->open_effect(*resumed.key).has_value(),
          "an effect cannot open while earlier owned STATE work is unresolved");
  require(controller->settle_state(pending_state_key, EditResult::AppliedReadback) ==
              Result::Accepted,
          "the pre-loss owned STATE is still settleable (accounting survived the change)");
  auto ticket = controller->open_effect(*resumed.key);
  require(ticket.has_value(), "the effect opens once the prior STATE is settled");
  require(controller->settle_effect(*ticket, EditResult::AppliedReadback) == Result::Accepted,
          "the new incarnation's effect settles");
  host2.stop_and_join();
  std::printf("PASS stage4b semantic stale-state fencing\n");
}

// Phase 2: an indeterminate (unresolved) EFFECT keeps its explicit responsibility
// across the incarnation change: it is never converted into PASS, never replayed,
// and never re-materialized.
void run_semantic_effect() {
  const auto name = pipe_name(L"semantic-effect");
  IpcBackend client(name);
  std::unique_ptr<SemanticController> controller;
  Key effect_key{};
  std::optional<Ticket> ticket;

  {
    auto engine1 = std::make_unique<FakeEngine>(2300);
    RunningHost host(std::move(engine1), name);
    require(host.start(), "semantic-effect host1 start");
    require(wait_for_host_pipe(name, kWait), "semantic-effect host1 listens");
    require(connect_with_retry(client, kWait), "semantic-effect connect");
    controller = std::make_unique<SemanticController>(902, 65, client, 1);
    require(controller->semantic_ready(), "semantic-effect readiness");

    require(dispatch_key(*controller, physical('n')).route == SemanticDecision::Route::State,
            "semantic-effect preedit settled");
    const SemanticDecision commit = controller->begin_key(space_key(), true);
    require(commit.route == SemanticDecision::Route::Effect && commit.key.has_value(),
            "commit routed as EFFECT");
    effect_key = *commit.key;
    ticket = controller->open_effect(effect_key);
    require(ticket.has_value(), "one execution ticket");
    require(controller->settle_effect(*ticket, EditResult::Indeterminate) == Result::Accepted,
            "an indeterminate mutation settles as an uncertain terminal");
    require(controller->pass_probe() == Pass::RecoveryRequired,
            "uncertainty fences the PASS barrier");
    require(!controller->open_effect(effect_key).has_value(),
            "an indeterminate effect is never re-issued");

    const std::size_t records_before = controller->core().records().size();
    host.stop_and_join();
    require(pipe_absent(name), "semantic-effect host1 released the pipe");
    observe_transport_loss(client, controller->core().binding());
    require(!controller->semantic_ready(), "the loss fences semantic readiness");
    require(controller->core().records().size() == records_before,
            "uncertain accounting survives the incarnation change");
  }

  auto engine2 = std::make_unique<FakeEngine>(2400);
  FakeEngine *fake2 = engine2.get();
  RunningHost host2(std::move(engine2), name);
  require(host2.start(), "semantic-effect host2 start");
  require(wait_for_host_pipe(name, kWait), "semantic-effect host2 listens");
  require(host2.server().incarnation() != client.incarnation(), "a new incarnation is running");

  const SemanticDecision blocked = controller->begin_key(physical('a'), true);
  require(blocked.route == SemanticDecision::Route::FailOpen,
          "unresolved uncertainty admits no new ownership");
  require(!blocked.eaten, "a blocked key stays application-owned");
  require(controller->semantic_ready(),
          "readiness is re-established even while uncertainty blocks admission");
  require(fake2->received().empty(), "nothing is replayed or newly sent into the new incarnation");
  require(fake2->bind_calls() == 1 && fake2->reset_calls() == 0,
          "the new incarnation only received the rebind");

  // A later genuine PASS must not overtake the unresolved prior EFFECT, and the
  // current key must reach an explicit non-PASS terminal.
  require(controller->pass_probe() == Pass::RecoveryRequired, "still recovery-required");
  require(!controller->finish_pass(), "a later PASS cannot be reported");
  const SemanticDecision pass_key = controller->begin_key(unrouted_pass_key(), true);
  require(pass_key.route == SemanticDecision::Route::FailOpen, "the PASS key is not owned");
  require(!controller->finish_pass(), "the PASS key cannot be reported as PASS");
  const Key cancelled = controller->cancel_pass_recovery();
  require(cancelled.sequence != 0,
          "the blocked key reaches an explicit cancelled recovery terminal");
  require(!controller->finish_pass(), "cancellation is never a PASS");

  bool uncertain_effect = false;
  for (const auto &record : controller->core().records()) {
    if (record.key == effect_key && record.kind == Kind::Effect && record.uncertain)
      uncertain_effect = true;
  }
  require(uncertain_effect, "the indeterminate effect is still explicitly accounted");
  require(!controller->open_effect(effect_key).has_value(), "no duplicate materialization");
  require(controller->host().reconcile(*ticket, Readback::Applied) == Result::Accepted,
          "readback reconciliation resolves accounting");
  require(!controller->open_effect(effect_key).has_value(),
          "reconciliation issues no second execution ticket");
  host2.stop_and_join();
  std::printf("PASS stage4b semantic unresolved-effect responsibility\n");
}

// Phase 3: a deferred Host action owned by the dead incarnation is not replayed
// into the new one.
void run_semantic_deferred_action() {
  const auto name = pipe_name(L"semantic-reset");
  IpcBackend client(name);
  std::unique_ptr<SemanticController> controller;

  {
    auto engine1 = std::make_unique<FakeEngine>(2500);
    RunningHost host(std::move(engine1), name);
    require(host.start(), "semantic-reset host1 start");
    require(wait_for_host_pipe(name, kWait), "semantic-reset host1 listens");
    require(connect_with_retry(client, kWait), "semantic-reset connect");
    controller = std::make_unique<SemanticController>(903, 65, client, 1);
    require(controller->semantic_ready(), "semantic-reset readiness");
    require(dispatch_key(*controller, physical('n')).route == SemanticDecision::Route::State,
            "semantic-reset state settled");
    // An external composition termination defers one Host reset.
    controller->note_composition_terminated();
    host.stop_and_join();
    require(pipe_absent(name), "semantic-reset host1 released the pipe");
    observe_transport_loss(client, controller->core().binding());
    require(!controller->semantic_ready(), "the loss fences semantic readiness");
  }

  auto engine2 = std::make_unique<FakeEngine>(2600);
  FakeEngine *fake2 = engine2.get();
  RunningHost host2(std::move(engine2), name);
  require(host2.start(), "semantic-reset host2 start");
  require(wait_for_host_pipe(name, kWait), "semantic-reset host2 listens");
  const SemanticDecision decision = controller->begin_key(physical('a'), true);
  require(fake2->bind_calls() == 1, "the context is bound exactly once on the new incarnation");
  require(fake2->reset_calls() == 0,
          "a deferred action from the dead incarnation is not replayed into the new one");
  require(decision.route == SemanticDecision::Route::State,
          "the new incarnation serves the new key normally");
  require(controller->raw_buffer() == "a",
          "the new projection comes from the new incarnation only");
  host2.stop_and_join();
  std::printf("PASS stage4b semantic deferred-action fencing\n");
}

void test_semantic() {
  run_semantic_stale_state();
  run_semantic_effect();
  run_semantic_deferred_action();
  std::printf("PASS stage4b semantic\n");
}

#ifdef FUSION_WITH_FCITX5

// ---------------------------------------------------------------------------
// hostprocess: the real FusionHost.exe process, the real Fcitx backend and the
// real IPC client. The test owns the Host process, so it can terminate and
// restart it exactly as an operator would, and the same client/controller pair
// must reconnect, rebind and keep working.
// ---------------------------------------------------------------------------

std::wstring executable_directory() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH)
    return {};
  const std::wstring value(path, length);
  const auto slash = value.find_last_of(L'\\');
  return slash == std::wstring::npos ? std::wstring() : value.substr(0, slash);
}

class HostProcess {
public:
  HostProcess(std::wstring executable, std::wstring pipe)
      : executable_(std::move(executable)), pipe_(std::move(pipe)) {}

  ~HostProcess() { kill(); }

  bool start() {
    kill();
    std::wstring command = L"\"" + executable_ + L"\" --pipe-name " + pipe_;
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    // No handle inheritance: the child must never hold a duplicate of a test
    // pipe handle, otherwise the Host's death would not close the transport.
    if (!CreateProcessW(executable_.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
      return false;
    CloseHandle(info.hThread);
    process_ = info.hProcess;
    return true;
  }

  bool running() const {
    if (process_ == nullptr)
      return false;
    DWORD code = 0;
    return GetExitCodeProcess(process_, &code) && code == STILL_ACTIVE;
  }

  void kill() {
    if (process_ == nullptr)
      return;
    if (running()) {
      TerminateProcess(process_, 1);
      WaitForSingleObject(process_, 10000);
    }
    CloseHandle(process_);
    process_ = nullptr;
  }

private:
  std::wstring executable_;
  std::wstring pipe_;
  HANDLE process_ = nullptr;
};

// Settles one ordered frontend dispatch the way the TSF edit sessions do and
// returns the text committed by it.
std::string settle_frontend(SemanticController &controller, const SemanticDecision &decision) {
  std::string committed;
  for (const auto &work : decision.frontend) {
    if (work.operation.type == BackendOperation::Type::CommitText) {
      committed += work.operation.text;
      auto ticket = controller.open_effect(work.key);
      require(ticket.has_value(), "real Fcitx effect ticket");
      require(controller.settle_effect(*ticket, EditResult::AppliedReadback) == Result::Accepted,
              "real Fcitx effect settled");
    } else if (work.operation.type == BackendOperation::Type::SetPreedit) {
      require(controller.settle_state(work.key, EditResult::AppliedReadback) == Result::Accepted,
              "real Fcitx state settled");
    }
  }
  return committed;
}

// Types "nihao" and commits it with Space through the real engine.
std::string type_nihao_and_commit(SemanticController &controller) {
  for (char character : std::string("nihao")) {
    const SemanticDecision decision = controller.begin_key(physical(character), true);
    require(decision.route != SemanticDecision::Route::FailOpen &&
                decision.route != SemanticDecision::Route::CancelledRecovery,
            "real Fcitx served every Pinyin key");
    (void)settle_frontend(controller, decision);
  }
  const SemanticDecision commit = controller.begin_key(space_key(), true);
  require(commit.route != SemanticDecision::Route::FailOpen, "real Fcitx served the commit key");
  return settle_frontend(controller, commit);
}

// One key of the traced sequence: its routing decision and the ordered callbacks
// the real engine produced for it.
struct KeyTrace {
  std::string label;
  SemanticDecision::Route route = SemanticDecision::Route::FailOpen;
  bool eaten = false;
  bool forward = false;
  bool reached_engine = false; // a batch came back from the (possibly new) incarnation
  std::string preedit;
  std::string committed;
};

// Drives "nihao" + Space and reports the per-key execution path plus the text an
// application would actually end up with: forwarded (definitely-not-written)
// physical characters followed by the committed text of the engine callbacks.
std::vector<KeyTrace> trace_nihao_and_commit(SemanticController &controller,
                                             std::string &visible_text) {
  std::vector<KeyTrace> traces;
  std::string keys = "nihao";
  keys.push_back(' ');
  for (char character : keys) {
    const bool is_space = character == ' ';
    const SemanticDecision decision =
        controller.begin_key(is_space ? space_key() : physical(character), true);
    KeyTrace trace;
    trace.label = is_space ? "space" : std::string(1, character);
    trace.route = decision.route;
    trace.eaten = decision.eaten;
    trace.forward = decision.forward;
    trace.reached_engine = decision.backend_session_id != 0;
    std::string committed = settle_frontend(controller, decision);
    for (const auto &work : decision.frontend) {
      if (work.operation.type == BackendOperation::Type::SetPreedit)
        trace.preedit = work.operation.text;
    }
    trace.committed = committed;
    // What the application sees: a forwarded physical character is delivered to
    // the application unchanged; a commit inserts the engine's text.
    if (decision.forward && decision.route == SemanticDecision::Route::Pass && !is_space)
      visible_text += character;
    visible_text += committed;
    traces.push_back(std::move(trace));
  }
  return traces;
}

void print_traces(const char *label, const std::vector<KeyTrace> &traces,
                  const std::string &visible_text) {
  std::printf("[%s] per-key execution path:\n", label);
  for (const auto &trace : traces) {
    std::printf("  key=%-5s route=%-17d eaten=%d forward=%d engine=%d preedit=\"%s\" "
                "commit=\"%s\"\n",
                trace.label.c_str(), static_cast<int>(trace.route), trace.eaten ? 1 : 0,
                trace.forward ? 1 : 0, trace.reached_engine ? 1 : 0, trace.preedit.c_str(),
                trace.committed.c_str());
  }
  std::printf("[%s] application-visible text: \"%s\"\n", label, visible_text.c_str());
}

void test_host_process() {
  if (std::getenv("FUSION_STAGE4B_SKIP_HOST_PROCESS")) {
    std::printf("SKIP stage4b hostprocess (FUSION_STAGE4B_SKIP_HOST_PROCESS set)\n");
    return;
  }
  const std::wstring executable = executable_directory() + L"\\FusionHost.exe";
  if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::printf("SKIP stage4b hostprocess (FusionHost.exe not built next to the test)\n");
    return;
  }

  const auto name = pipe_name(L"hostprocess-real");
  IpcBackend client(name);
  HostProcess host(executable, name);

  require(host.start(), "real FusionHost.exe started");
  require(wait_for_host_pipe(name, kWait), "real FusionHost.exe listens");
  require(connect_with_retry(client, kWait), "connect to the real Host");
  const Binding binding{31, 1, 1};
  require(client.bind(binding), "bind against the real Host");
  auto controller = std::make_unique<SemanticController>(911, 65, client, 1);
  require(controller->semantic_ready(), "semantic readiness against the real Host");
  require(type_nihao_and_commit(*controller) == "你好",
          "real Fcitx composition and commit before the Host failure");
  const std::uint64_t incarnation_before = client.incarnation();

  // Terminate the Host exactly as an operator would while the IME is idle.
  host.kill();
  require(wait_until([&] { return !host.running(); }, kWait), "the Host process terminated");
  observe_transport_loss(client, binding);
  require(!controller->semantic_ready(), "the Host failure fences semantic readiness");
  // T eligibility is key-class only: an application that consults T must still
  // deliver K, where the bounded reconnect and rebind run.
  require(controller->routes_key(physical('n')),
          "T eligibility of a Pinyin key must not depend on semantic readiness");
  require(!controller->routes_key(unrouted_pass_key()), "an unrouted key stays ineligible");

  require(host.start(), "real FusionHost.exe restarted");
  require(wait_for_host_pipe(name, kWait), "the restarted Host listens");
  require(type_nihao_and_commit(*controller) == "你好",
          "real Fcitx composition and commit after the reconnect");
  require(client.incarnation() != incarnation_before,
          "a restarted Host process is a different incarnation");
  require(client.healthy(), "the reconnected link is healthy");
  host.kill();
  std::printf("PASS stage4b hostprocess\n");
}

// The exact real-smoke sequence: the Host dies *silently* (nothing detects it),
// it is restarted, and the operator types "nihao" + Space with no intervening
// probe. This is the case the manual smoke hit; it is traced, not asserted away.
void test_silent_death_sequence() {
  if (std::getenv("FUSION_STAGE4B_SKIP_HOST_PROCESS")) {
    std::printf("SKIP stage4b silentdeath (FUSION_STAGE4B_SKIP_HOST_PROCESS set)\n");
    return;
  }
  const std::wstring executable = executable_directory() + L"\\FusionHost.exe";
  if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::printf("SKIP stage4b silentdeath (FusionHost.exe not built next to the test)\n");
    return;
  }

  const auto name = pipe_name(L"silent-death-real");
  IpcBackend client(name);
  HostProcess host(executable, name);
  require(host.start(), "silentdeath host started");
  require(wait_for_host_pipe(name, kWait), "silentdeath host listens");
  require(connect_with_retry(client, kWait), "silentdeath connect");
  const Binding binding{33, 1, 1};
  require(client.bind(binding), "silentdeath bind");
  auto controller = std::make_unique<SemanticController>(913, 65, client, 1);
  require(controller->semantic_ready(), "silentdeath readiness");
  std::string before_text;
  (void)trace_nihao_and_commit(*controller, before_text);
  require(before_text == "你好", "baseline commit before the silent death");
  const std::uint64_t incarnation_before = client.incarnation();
  const std::uint64_t generation_before = client.link_status().connection_generation;

  // Silent death + restart, with NO request in between: exactly what the manual
  // smoke does when the operator restarts the Host and then types.
  host.kill();
  require(wait_until([&] { return !host.running(); }, kWait), "silentdeath host terminated");
  require(host.start(), "silentdeath host restarted");
  require(wait_for_host_pipe(name, kWait), "the restarted silentdeath host listens");

  std::string visible;
  const std::vector<KeyTrace> traces = trace_nihao_and_commit(*controller, visible);
  print_traces("silentdeath: type immediately after the restart", traces, visible);

  // What the implementation does, stated as the regression contract. It used to be that the
  // first physical key could not be proven to have reached any Host, so it stayed caller-owned
  // and was forwarded (Stage 4A boundary). That rule loses the key in exactly the applications
  // this round is about: a window that honours OnTestKeyDown alone never delivers a key the
  // shell claimed in T, so "hand it back" means "it never arrives" (R53, Notepad++). The shell
  // therefore delivers the character itself — once, the same character the Linux frontend would
  // have passed on — and the following key reconnects to the new incarnation and is served from
  // its fresh state. Owner decision, 2026-10-30.
  require(!traces.empty() && !traces.front().forward && traces.front().eaten,
          "the first key after a silent death is delivered, not handed back");
  require(traces.front().committed == "n", "the first printable key commits exactly its character");
  require(!traces[1].forward && traces[1].eaten, "the second key reaches the new incarnation");
  require(client.incarnation() != incarnation_before, "a new incarnation is established");
  require(client.link_status().connection_generation == generation_before + 1,
          "exactly one new connection generation");
  require(controller->semantic_ready(), "semantic readiness is re-established");
  require(visible.rfind("n", 0) == 0, "the application received the delivered prefix");
  require(visible.find("n", 1) == std::string::npos,
          "the first key appears exactly once: no duplicate, no loss");
  require(visible.size() > 1, "the new incarnation committed the remaining input");

  // Phase 2: the operational sequence that actually re-establishes the link. A
  // silent death is learned from the first operation, which is therefore forwarded;
  // only the NEXT operation reconnects. One key can never both detect and
  // re-establish, so an expectation of reconnection after a single key is wrong.
  host.kill();
  require(wait_until([&] { return !host.running(); }, kWait), "silentdeath host terminated again");
  require(host.start(), "silentdeath host restarted again");
  require(wait_for_host_pipe(name, kWait), "the restarted host listens again");

  const std::uint64_t incarnation_stale = client.incarnation();
  const std::uint64_t generation_stale = client.link_status().connection_generation;

  // A character-less key, deliberately: this phase is about the link lifecycle, and R53's
  // delivery rule is about printable keys only — Phase 1 above is where that rule is asserted.
  // Keeping this key character-less leaves every assertion below about detection alone.
  const SemanticDecision detect =
      controller->begin_key(translate_key(kVkReturn, false, false, false), true);
  std::printf("  [detection key] route=%d eaten=%d forward=%d ops=%zu link_state=%d\n",
              static_cast<int>(detect.route), detect.eaten ? 1 : 0, detect.forward ? 1 : 0,
              detect.operation_count, static_cast<int>(client.link_status().state));
  require(detect.forward && !detect.eaten, "the detection key is forwarded to the application");
  require(detect.operation_count == 0, "the detection key produced no engine callback");
  require(client.link_status().state == LinkState::Disconnected, "detection drops the dead link");
  require(client.incarnation() == incarnation_stale, "detection alone does not reconnect");
  require(!controller->semantic_ready(), "detection alone is not semantic readiness");

  const SemanticDecision reestablish = controller->begin_key(space_key(), true);
  std::printf("  [reconnect key] route=%d eaten=%d forward=%d ops=%zu link_state=%d\n",
              static_cast<int>(reestablish.route), reestablish.eaten ? 1 : 0,
              reestablish.forward ? 1 : 0, reestablish.operation_count,
              static_cast<int>(client.link_status().state));
  // The engine may answer a forwarded key with no callback at all (a Space with an
  // empty preedit is disposition Forward with zero operations), so the reconnect is
  // asserted on the link identity, and the functional proof is the complete
  // composition traced below.
  require(client.link_status().state == LinkState::Established, "the next key reconnects");
  require(client.incarnation() != incarnation_stale, "a new incarnation is established");
  require(client.link_status().connection_generation == generation_stale + 1,
          "exactly one new connection generation");
  require(controller->semantic_ready(), "semantic readiness is re-established");
  (void)settle_frontend(*controller, reestablish);

  // This is the other restart shape: the incarnation moved before the key was typed, so
  // fence_previous_incarnation() reset the cached mode and the shell has no fact to refuse on.
  // The first key is therefore admitted, and the text below is the proof that it arrives
  // exactly once — nothing lost, nothing doubled.
  require(controller->engine_has_work_for(translate_key('n', false, false, false)),
          "the first key after the cached mode was reset is refused");

  std::string visible_primed;
  const std::vector<KeyTrace> primed = trace_nihao_and_commit(*controller, visible_primed);
  print_traces("silentdeath: detected, then re-established, then type", primed, visible_primed);
  require(!primed.front().forward, "after re-establishment the first key reaches the engine");
  require(visible_primed == "你好",
          "with the link re-established the composition commits exactly 你好");

  host.kill();
  std::printf("PASS stage4b silentdeath (traced, see the per-key paths above)\n");
}

#endif // FUSION_WITH_FCITX5

} // namespace

int main(int argc, char *argv[]) {
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
#ifdef FUSION_WITH_FCITX5
  const char *usage =
      "Usage: stage4b <link|failover|cycles|fencing|contexts|semantic|hostprocess|silentdeath>\n";
#else
  const char *usage = "Usage: stage4b <link|failover|cycles|fencing|contexts|semantic>\n";
#endif
  if (argc < 2) {
    std::fprintf(stderr, "%s", usage);
    return 1;
  }
  const std::string mode = argv[1];
  try {
    if (mode == "link")
      test_link();
    else if (mode == "failover")
      test_failover();
    else if (mode == "cycles")
      test_cycles();
    else if (mode == "fencing")
      test_fencing();
    else if (mode == "contexts")
      test_contexts();
    else if (mode == "semantic")
      test_semantic();
    else if (mode == "hostprocess") {
#ifdef FUSION_WITH_FCITX5
      test_host_process();
#else
      std::fprintf(stderr, "hostprocess mode requires the FUSION_WITH_FCITX5 build\n");
      return 1;
#endif
    } else if (mode == "silentdeath") {
#ifdef FUSION_WITH_FCITX5
      test_silent_death_sequence();
#else
      std::fprintf(stderr, "silentdeath mode requires the FUSION_WITH_FCITX5 build\n");
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
