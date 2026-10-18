# Fusion Windows

Windows product implementation for the Fusion Enhanced IME line. Today it is a
portable, sans-IO C++20 correctness core (`FusionCore`), a deterministic test-only
scheduler and fault harness (`FusionMock`), a minimal semantic Mock Backend with
an explicit Host-side boundary (`fusion::Host`) that translates backend semantic
intent into existing FusionCore operations, and a Stage 2.5 real Windows TSF/COM
development shell (`FusionTsf`). Stage 3B/3C provide the real Fcitx5 backend PoC
and faithful frontend path. Stage 4A adds an out-of-process Fcitx Host
(`FusionHost.exe`) connected by length-prefixed binary IPC over a local Named Pipe;
Stage 4B adds Host-incarnation identity, a bounded product-path reconnect, explicit
semantic readiness and incarnation fencing. Stage 5 projects the engine's candidate
page, its panel text rows and the displayed preedit (format segments + engine cursor)
across that same IPC path, presents the preedit as a styled TSF composition with the
engine caret, and renders a first minimal in-process candidate surface that turns a
pointer hit into an explicit non-key candidate action.
There is no end-user Windows input product yet; independent review is required
before further integration.

**Current implementation snapshot.** Current Windows stage status is authoritative in D072
§22/§25 and is not duplicated here.

## Project authority

Normative cross-platform requirements, decisions, roadmap and architecture live in
`fusion-controller`, not in this repository:

- Windows architecture authority —
  [fusion-controller D072 / windows-architecture.md](https://github.com/choicky/fusion-controller/blob/main/docs/windows-architecture.md)
- Branch governance authority —
  [fusion-controller D076 / branch-policy.md](https://github.com/choicky/fusion-controller/blob/main/docs/branch-policy.md)
- Repository-local execution rules — [AGENTS.md](AGENTS.md)

`fusion-windows` owns the Windows product implementation, its tests, and this
repository entry point only.

## Repository structure

- `include/fusion/core.hpp` — portable FusionCore public model (Key/Kind/Result/Ticket, …).
- `include/fusion/host.hpp` — Stage 2 `fusion::Host` Host-side boundary (backend `Intent` → Core operations).
- `include/fusion/backend.hpp` — Stage 3B/4A/5 `fusion::Backend` interface and `BackendStimulus`/`BackendBatch`/`BackendOperation` types, plus the Stage 5 `CandidatePage`/`CandidateEntry`/`CandidateLayout` page projection and the non-key candidate action stimulus types.
- `include/fusion/ipc_protocol.hpp` — Stage 4A IPC protocol: length-prefixed binary framing and serialization (platform-free). `protocol_version` is 4 since the Stage 5 closure (3 added the candidate action types + `CandidatePage`; 4 added the preedit segment/format/cursor contract and the panel rows).
- `src/core.cpp` — FusionCore implementation.
- `src/tsf/` — Stage 2.5 real TSF/COM development shell (`FusionTsf` DLL): service, edit sessions, key map, semantic adapter, diagnostics, registration; `lifecycle.hpp` (Stage 4C) holds the lifecycle obligation table and the teardown summary shared by the sinks and their tests; `candidate_view.{hpp,cpp}` (Stage 5) is the minimal in-process candidate surface that paints the projected panel (geometry and hit testing live in the portable `candidate_layout.hpp`) and reports a pointer hit as a non-key action (the DLL links only GDI32/USER32 for it, never Fcitx/LibIME); `display_attribute.{hpp,cpp}` + `preedit_presentation.hpp` present the Fcitx preedit format as TSF display attributes and the engine cursor as the composition caret. The TSF DLL builds on WIN32+MSVC regardless of `FUSION_WITH_FCITX5`. When `FUSION_WITH_FCITX5` is ON, it additionally links the IPC backend to talk to FusionHost.exe.
- `src/ipc/` — Stage 4A IPC transport: `pipe_client.{hpp,cpp}` (overlapped I/O client), `pipe_server.{hpp,cpp}` (per-connection multi-instance server, Host-side only), `pipe_security.{hpp,cpp}` (per-user pipe name, server identity verification, security descriptor), `ipc_backend.{hpp,cpp}` (Backend over IPC).
- `src/host/` — Stage 4A FusionHost.exe: `main.cpp` (entry point), `host_server.{hpp,cpp}` (multi-client accept loop, per-connection threads, backend mutex, context remapping).
- `src/fcitx/` — Host-side real Fcitx backend: `backend.cpp` (Fcitx thread, InputContext frontend, dispatch and candidate actions) and `candidate_projection.{hpp,cpp}` (the engine's candidate page flattened to the transport `CandidatePage`, observing `Instance::outputFilter` per display field like the mature classic-ui display path).
- `tests/mock.{hpp,cpp}` — deterministic FusionMock scheduler, fault harness and oracles.
- `tests/mock_backend.{hpp,cpp}` — Stage 2 semantic Mock Backend (a test abstraction).
- `tests/rg0.cpp` — Stage 1 RG-0 invariant tests.
- `tests/stage2.cpp` — Stage 2 end-to-end tests (stimulus → Mock Backend → Host → Core).
- `tests/stage3b.cpp` — Stage 3B real Fcitx backend tests.
- `tests/stage3c.cpp` — Stage 3C faithful frontend tests.
- `tests/stage4a_ipc.cpp` — Stage 4A IPC tests: protocol serialization, loopback pipe, HostServer (multi-client, transport-failure separation, clean shutdown) and the FusionHost.exe integration mode. The first three modes run in the Mock-only build too; only the integration mode needs `FUSION_WITH_FCITX5=ON`.
- `tests/tsf_semantic.cpp` — Stage 2.5 deterministic tests for the key map and semantic adapter.
- `tests/tsf_lifecycle.cpp` — Stage 4C lifecycle obligation and teardown-reconciliation tests (context identity, unresolved responsibility, external termination, cross-context isolation).
- `tests/stage5_candidates.cpp` — Stage 5 candidate projection/action tests (platform-free): page STATE, placeholder index alignment, select-to-EFFECT ordering, paging highlight, stale-page and out-of-range refusal, truncated-page cursor normalization, focus-loss/presentation-failure invariants, and the closure modes (`utf16`, `preedit`, `panel`, `comment`). Runs in the Mock-only build; the Win32 painting itself is covered on the real product path.
- `tests/stage5_real_projection.cpp` — Stage 5 closure against the real pinned pinyin engine (in-process `FusionFcitxHost`, `FUSION_WITH_FCITX5` build): preedit segments/format/cursor and their movement with Left/Right, partial selection and continued composition, stroke-filter and QuickPhrase auxUp, and the empty-panel hide signal.
- `tests/fcitx_candidate_filter.cpp` — Stage 5 display outputFilter contract against the real Fcitx5 core (Windows `FUSION_WITH_FCITX5` build against the pinned runtime, and non-Windows builds with Fcitx5 dev libraries): candidate label/text/comment like classic-ui, the client preedit like the dbus frontend (segments, flags, cursor, engine password masking), and the panel rows like classic-ui.
- `tools/tsf/*.ps1` — reversible development registration/unregistration/state check for the TSF service.
- `tools/package/` — Stage 6 WP4 owner-only test packaging: `build-package.ps1` (zip + manifest from a fresh Release build), `install.ps1` / `uninstall.ps1` (elevated; versioned install directory, logon task, orderly stop through the Host's stop event), `host-state.ps1` (shared read-only state/identity helpers) and `verify-wp4.ps1` (the owner walkthrough).
- `CMakeLists.txt` — C++20 library plus CTest targets; the TSF DLL builds on WIN32+MSVC; FusionHost.exe builds when `FUSION_WITH_FCITX5=ON`.

## Local validation

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
clang-format --dry-run --Werror include/fusion/core.hpp include/fusion/host.hpp include/fusion/backend.hpp include/fusion/ipc_protocol.hpp src/core.cpp src/tsf/*.hpp src/tsf/*.cpp src/ipc/*.hpp src/ipc/*.cpp src/host/*.hpp src/host/*.cpp tests/mock.hpp tests/mock.cpp tests/mock_backend.hpp tests/mock_backend.cpp tests/rg0.cpp tests/stage2.cpp tests/stage3b.cpp tests/stage3c.cpp tests/stage4a_ipc.cpp tests/stage4b_reconnect.cpp tests/tsf_lifecycle.cpp tests/tsf_semantic.cpp
```

CMake 3.20+, a C++20 compiler, and CTest are sufficient. There are no external
runtime/test dependencies and no CI workflow. The original formatting baseline uses clang-format 18; native Stage 2.5 validation used the available clang-format 22.1.3 with the repository configuration.
The library alone builds with `-DBUILD_TESTING=OFF`. On Windows with MSVC the
build additionally produces the `FusionTsf.dll` x64 development TSF shell
(register it per "Stage 2.5" below; non-Windows and non-MSVC builds skip it).
When `FUSION_WITH_FCITX5=ON`, the build also produces `FusionHost.exe`.

**Windows/MSVC configure prerequisite (dependency tracking).** With a non-English MSVC
(e.g. Chinese), CMake mis-detects the localized `/showIncludes` banner, so Ninja records no
header dependencies: header-only edits silently stop recompiling dependents and stale mixed
objects can produce false failures (e.g. `/RTC` stack-corruption dialogs). A build
directory must therefore be **configured in a UTF-8 console** (`chcp 65001`, as
`tools/fcitx/build-native.ps1` already does); the prefix is detected at configure time, so
an existing build directory cannot be repaired by a later build. `VSLANG=1033` is harmless
but was measured not to change this installation's `cl.exe` banner, and is not what makes
dependency tracking work.

## Portable model boundary

`Core` represents one Windows-owned logical context. The TSF adapter
supplies lawful admission and abstract STATE/EFFECT classification; neither
Host proposals nor backend acceptance choose ownership. Key/context/lifetime/
epoch identities are local value types, not a final wire schema.

An owned EVENT stays in a capacity-limited ledger until an explicit terminal
outcome is acknowledged. Capacity is supplied by the caller, with no production
default or timing threshold. An adapter must resolve pending work through
completion, rejection, cancellation or a discontinuity; the core does not
invent a timeout policy. The terminal audit fails if work remains unresolved.

STATE consumption advances a watermark only after abstract successful edit and
readback. EFFECT proposals are immutable and non-coalescible. `begin_effect`
is the execution boundary: it checks identity, readiness, prior causal work,
and consumed state, then issues one execution ticket. The document authority
performs the edit and reports `AppliedReadback`, `RefusedNoMutation`, or
`Indeterminate`. A known no-mutation refusal permits another opportunity;
an indeterminate result never does. Tickets are not asynchronous edit requests;
later adapters must call this boundary after grant/revalidation, immediately
before mutation. Core never edits a document or asserts Host semantic intent.

`probe_pass` requires drain/recovery while a prior owned effect is unresolved,
including an effect whose Host response has not arrived. The physical key is
still caller-owned at this probe; no synthetic replay or deferred PASS storage
is provided. Cancellation has a distinct terminal value and cannot be reported
as PASS. T/K availability and edit admission are abstract caller conditions.

Disconnect/restart fences work. Issued effects retain uncertain terminal
accounting; unsent effects terminate with no mutation. Rebind requires explicit
readiness and resolved uncertainty. Readback reconciliation resolves accounting
without returning another execution ticket. Context replacement is an explicit
authority event, not a rule that focus loss changes epoch.

FusionMock schedules deterministic logical ticks and observes abstract document
effects separately from core accounting. Its synchronous reference workload
computes expected effect order directly, then compares all six response-order
permutations against the scheduled core model. It contains no engine or TSF
implementation.

## Stage 2 — Mock Backend and Host boundary

`tests/mock_backend.*` is a Stage 2 test abstraction distinct from FusionMock: it
assigns deterministic semantics to symbolic inputs (`preedit:` STATE, `commit:`
EFFECT intent, `reject:` failure after ownership, everything else a genuine PASS)
and holds no correctness policy, no causal state machine and no engine. FusionMock
remains the scheduler/fault harness for delivery timing (delay, duplicate, stale).

`fusion::Host` (`include/fusion/host.hpp`) is the minimal explicit Host-side
boundary: it translates a backend `Intent` into existing FusionCore operations
(consume STATE, propose an EFFECT at the current consumed-STATE watermark, open one
execution ticket, settle with the observed edit, reject/cancel/reconcile, and probe
the PASS barrier). Every method delegates a single transition to `Core` and returns
Core's verdict. It adds no second ledger, ownership authority, document
materialization, reliability state machine or wire protocol. The document authority
(the test harness) still performs the real edit and reports the observed
`EditResult`/`Readback` between an opened ticket and its completion. `Core` itself
is unchanged by Stage 2.

`tests/stage2.cpp` drives stimulus → Mock Backend → Host → FusionCore → document
observation end-to-end and reuses the FusionMock terminal/uniqueness oracles. It
exercises STATE, EFFECT, genuine PASS, the PASS barrier, delayed response, illegal
reorder, duplicate response, stale cross-context response, backend restart/reset,
post-ownership rejection and indeterminate irreversible mutation, confirming the
semantic path does not bypass the RG-0 invariants. Run them with
`ctest --test-dir build -R stage2`.

P0-A/P0-B/P0-D, recovery UX, permanent identifiers, final protocol/IPC/Host
topology, key classification, timing thresholds, candidate renderer and
environment policy remain open. The enums here describe test/model outcomes;
they do not select those product policies. Stage 2 was implemented before
Fcitx5/LibIME integration existed; the Mock Backend remains a test abstraction.

## Stage 2.5 — Real TSF + existing Host/Core + Mock Backend

`src/tsf/` is a thin native TSF/COM development shell (`FusionTsf.dll`, x64,
apartment-threaded) implementing the minimum real text-service surface needed to
prove: real Windows application → real TSF callback → existing `fusion::Host` /
FusionCore → existing Mock Backend → real TSF edit session. Its Windows-side responsibilities are: hard policy, key ownership, context
identity, causal ordering/barriers, edit-session control, projection and the
bounded causal ledger — nothing else. It adds no second engine, ledger, decoder
or materializer: the semantic oracle is the same `tests/mock_backend.cpp`
source linked into the DLL, and causal transitions delegate to existing `Core`/`Host`.

The co-located Host/Core and Mock Backend are a bounded Stage 2.5 integration
fixture, not D072 production process topology. It validates only shell/semantic
properties actually exercised. It does not validate Host crash isolation, bounded
IPC, transport failures, cross-process Host restart/incarnation fencing or production
cross-process lifecycle. D072 still requires a thin TSF DLL, bounded causal IPC and
an out-of-process Host. No production backend is moved into this DLL; this fixture
must not grow into the Stage 3 production runtime. P0-A/P0-B/P0-D and final
protocol/identifiers/topology remain OPEN. Stage 2.5 was implemented before
Stage 4A moved the backend out-of-process; Mock-only builds still use the in-process Mock
Backend, while with `FUSION_WITH_FCITX5=ON` the Fcitx backend runs in `FusionHost.exe`.

Boundary mapping (development-only Stage 2.5 policy; the production
effect-capable key classification stays UNFROZEN per D072 §5):

- letters/digits → `preedit:<raw buffer>` STATE, materialized through a real
  `ITfComposition` in a synchronous edit session;
- Space with a non-empty buffer → `commit:<raw buffer>` EFFECT, materialized
  through `SetText` + composition end (or `InsertTextAtSelection`) with
  readback, inside `TF_ES_SYNC | TF_ES_READWRITE`;
- Escape during composition → `reject:<raw buffer>`: explicit FailedNoEffect
  terminal, never PASS, never replay;
- Backspace edits the raw buffer (STATE); everything else — including all
  modifier combinations — is a genuine PASS candidate.

Correctness follows D072 A′: the execution ticket opens only after edit-session
grant and revalidation, immediately before mutation; a refused synchronous
session never fakes success (the owned key queues for an async session); a
genuine PASS cannot overtake an unresolved prior effect (drain-before-PASS
attempt, and an explicit CANCELLED recovery disposition when lawful drain is
impossible); external composition termination cancels queued work to explicit
terminals. `tests/tsf_semantic.cpp` tests this routing deterministically
(`ctest --test-dir build -R tsf`).

**Development registration (reversible, elevated).** Persistent cross-process
TIP registration writes HKLM `CTF\TIP` and the HKLM COM CLSID key, which needs
an elevated token (D072 §14). From a build directory:

```powershell
powershell -ExecutionPolicy Bypass -File tools/tsf/register.ps1   <build>\FusionTsf.dll
powershell -ExecutionPolicy Bypass -File tools/tsf/check-state.ps1
powershell -ExecutionPolicy Bypass -File tools/tsf/unregister.ps1 <build>\FusionTsf.dll
```

Activate the service per application through the input-method switcher
("Fusion Stage 2.5"). Diagnostics go to `%LOCALAPPDATA%\fusion-tsf\`
(or exactly `FUSION_TSF_LOG` when set, one file per process, bounded 2 MiB);
they trace key arrival → classification → ownership → backend verdict →
edit-session request/outer/inner results → mutation/readback → terminal
disposition, plus focus/context/fencing events. Diagnostics never change
behavior. `FusionTsfProfile.exe on|off|query` activates the fixture, restores the
previous non-Fusion profile, or queries the active keyboard profile through TSF.
After testing, restore normal input, then unregister and verify with
`check-state.ps1`. The checker reports both HKLM and HKCU COM paths; any manual
HKCU override must also be removed explicitly before claiming complete cleanup.
Registration failures can leave partial state: inspect it rather than assuming
an unsuccessful registration made no changes.

## Stage 4A — Out-of-process FusionHost.exe + IPC

Stage 4A moves the Fcitx backend from in-process (FusionTsf.dll) to an
out-of-process `FusionHost.exe` connected by a length-prefixed binary IPC
protocol over a local Named Pipe. The TSF DLL links an `IpcBackend` that
implements `fusion::Backend` over the pipe; the Host links the real Fcitx
backend and serves IPC requests from connected TSF DLL clients.

**IPC protocol (UNFROZEN).** Length-prefixed binary framing: `[4 bytes:
payload_length][1 byte: message_type][N bytes: payload]`. Strict
request/response alternation per connection. All deserialization is fail-closed
and rejects trailing bytes. Message types: Hello, Bind, Reset, Unbind, Send.
The Hello handshake exchanges `protocol_version` (currently 4) and the backend
`session_id`; since Stage 4B every Host response payload is prefixed with the
responding Host incarnation (`[8 bytes: host_incarnation]`). Version mismatch
returns version=0 and the client disconnects, so a mixed Stage 4B/Stage 5 pairing
fails the handshake instead of misparsing it.

**Pipe name and security.** The default pipe name is computed from the user SID
and session ID (`\\.\pipe\FusionHost-v1-<SID>-<sessionId>`), ensuring per-user
per-session isolation. The pipe is created with `PIPE_REJECT_REMOTE_CLIENTS`
and a DACL granting access only to the current user (Medium IL baseline per
D072 §14). The client verifies server identity by comparing the server process
token SID with its own after connect.

**Multi-client and context remapping.** The Host accepts multiple concurrent
clients, each on its own pipe instance. Each connection has its own thread and
remaps client context IDs to Host-unique context IDs (a new connection to the
same Host is always remapped to a new Host context). On disconnect, the Host
unbinds all contexts owned by that connection. All backend calls are serialized
through a shared mutex.

**Transport failure separation.** `IpcBackend::send()` distinguishes pre-write
failure (transport never saw the request → `Forward` with no ops) from
post-write failure (request on the wire, response lost → `Reject` + mark
unhealthy). `healthy()` is local state only — no IPC probe.

**TSF DLL fail-open.** When `FUSION_WITH_FCITX5` is ON, the TSF DLL creates its
`IpcBackend` during `Activate` and makes one bounded connection attempt
(200 ms). If the Host is unreachable, the DLL activates without a link and all
keys are genuine PASS (fail-open); the client object still exists, so a Host that
appears later is reached by the bounded reconnect described under Stage 4B. The
DLL does not auto-launch the Host (P0-D OPEN).

**FusionHost.exe startup.** The Host is a standalone executable that must be
started externally before the TSF DLL can connect. From a build directory:

```powershell
<build>\FusionHost.exe
```

The Host computes the default pipe name from the user SID and session ID. The
`--pipe-name <name>` override is for tests only. The Host enters an accept loop
and serves IPC requests until terminated.

**Tests.** `tests/stage4a_ipc.cpp` has four modes; `protocol`, `loopback` and `hostserver`
are compiled and registered in every Windows/MSVC build (Mock-only included), and only
`integration` requires `FUSION_WITH_FCITX5=ON`:

- `protocol`: serialization round-trip, bounds, trailing byte rejection, malformed input
- `loopback`: real Named Pipe with a CallbackBackend server thread (no Fcitx)
- `hostserver`: real HostServer with a fake Backend that mirrors FcitxBackend admission
  (duplicate bind rejected, every call fails on admission contention) — concurrent
  multi-client bind/send with context remapping and disconnect isolation, transport-failure
  separation (definitely-not-written `Forward` vs uncertain-write `Reject` vs post-write
  `Reject`), `healthy()` performs no IPC (server-side received-frame count), completed
  connection reaping during normal operation, and `stop()`/join shutdown without hanging
- `integration`: full FusionHost.exe + Fcitx backend over IPC (requires a running
  FusionHost.exe and the Fcitx runtime; set `FUSION_STAGE4A_SKIP_INTEGRATION=1` to skip
  instead of fail)

Run with `ctest --test-dir build -R stage4a`.

P0-A/P0-B/P0-D remain OPEN; Host startup/lifecycle belongs to P0-D. Recovery UX, permanent
identifiers, final wire format, timing thresholds, candidate renderer and environment
policy also remain OPEN. The IPC protocol is an engineering constant, not a frozen wire
format.

## Stage 4B — Host failure, reconnect, semantic readiness, incarnation fencing

Stage 4B makes the D072 §13 reconnect ladder reachable on the Stage 4A product path and
gives Host incarnations a concrete, testable identity. It changes no Fcitx/LibIME semantics
and adds no second ledger: ownership, uncertainty, ordering and accountings stay in the
existing `FusionCore` ledger.

**Identity.** `HostServer` owns one incarnation identity per Host process/instance
(a PoC engineering value from process id + monotonic clock + a process-local sequence; 0 is
reserved for "no incarnation"). The Hello response publishes it and every Host response
payload is prefixed with it (`[8 bytes: host_incarnation]`, `protocol_version` raised to 2 in Stage 4B, 3 in Stage 5, 4 at the Stage 5 closure). The
client refuses a response whose incarnation is not the one its connection established, and
`LinkStatus` exposes `{state, host_incarnation, connection_generation}` — the two causal
distinctions D072 §11 already named.

**The ladder is explicit.** `LinkState` is `Disconnected` → `Connected` (transport up,
handshake incomplete) → `Established` (protocol and incarnation known). "Semantic ready" is
a further, per-context condition: the context binding must belong to the current
incarnation *and* connection generation, because the Host unbinds every context of a
dropped connection and remaps a new connection to new Host context ids.

**Bounded reconnect.** `IpcBackend::ensure_link` is idempotent, bounded (transport connect
plus one Hello round trip, 200 ms engineering constant) and performs no I/O when already
established; it never reuses a connection across an establishment. It is driven from the
existing key/context path (`SemanticController::begin_real_key`,
`resolve_or_create_entry`) — no background thread and no busy loop. A Host that is not
running fails the transport open immediately, so repeated attempts are cheap; a failure is
`FailOpen` (the key stays application-owned), which is why a Host crash cannot crash the
target application. Because the DLL keeps its client object even when the Host is
unreachable at `Activate`, a Host started later is reached without switching the IME.

**Fencing and preserved responsibility.** On an incarnation or connection-generation move
the controller fences the previous incarnation's Host-provided semantic projection
(`buffer_`, `state_dependency_`, a deferred reset intent) and re-binds the context;
ownership/uncertainty accounting and the never-issued materialization queue are
deliberately *not* cleared, and the Core ledger (`prior_pending`, `consumed_state_`,
`probe_pass`) still prevents an effect from overtaking unresolved prior work. A request
whose execution certainty is unknown stays `Reject`/cancelled with an explicit terminal and
is never replayed; an indeterminate effect is never converted into ordinary PASS. Unresolved
P0-A/P0-B recovery is therefore preserved and observable, not resolved.

**Diagnostics.** The TSF shell emits one `LINK` diagnostic per observed link transition
(state, incarnation, connection generation, semantic readiness, route), so disconnect,
reconnect and rebind are visible in `%LOCALAPPDATA%\fusion-tsf\`.

**Tests.** `tests/stage4b_reconnect.cpp` drives the real product classes (`IpcBackend` +
`HostServer` + the Stage 2.5/3C `SemanticController`) against a deterministic Host-side test
double, so every mode builds and runs in the Mock-only build too:

- `link`: the state ladder (including "transport connected but not established"), bounded
  `ensure_link`, idempotence without I/O, and the preserved definitely-not-written `Forward`
  boundary
- `failover`: Host death while idle, an observable loss, restart, bounded reconnect, a new
  incarnation, rebind and working new input
- `cycles`: four Host loss/restart cycles — distinct incarnations, exactly one connection
  generation per establishment, no context reuse inside a Host, no leaked handles, no replay
- `fencing`: a result stamped with a foreign incarnation and a response without an envelope
  are both refused deterministically, while a correct round trip on a fresh connection is
  accepted and the refused stimulus is never reissued
- `contexts`: isolation across an incarnation change; a context that has not re-established
  its binding cannot reach the new Host or affect another context
- `semantic`: stale STATE from the dead incarnation cannot become current STATE for the new
  one, owned/uncertain accounting survives, an effect cannot overtake unresolved prior STATE,
  a deferred action of the dead incarnation is not replayed, and an unresolved EFFECT is not
  converted into PASS or re-materialized

Run with `ctest --test-dir build -R stage4b`.

P0-A/P0-B/P0-D remain OPEN. Stage 4B deliberately does not choose the final user-visible
recovery policy: a composition that the dead incarnation had already materialized in the
document is left in place (recovery representation/UX is P0-B), and the exact
reconnect/readiness numbers remain engineering constants.

## Stage 4C — TSF lifecycle wiring and context reconciliation

Stage 4C wires the real TSF lifecycle to the Fusion context boundary (D072 §25 item 3). It adds no
engine, ledger or recovery policy: `src/tsf/lifecycle.hpp` states what one lifecycle event obliges the
shell to do with a Fusion context, and the sinks in `src/tsf/service.cpp` apply it.

**Obligation table (evidence-bounded).** Only *teardown* events end a Fusion context — `Deactivate`,
`OnPopContext`, `OnUninitDocumentMgr`. An external `OnCompositionTerminated` reconciles the projection
and settles queued/pending work but keeps the context identity and Host binding. Focus
(`ITfThreadMgrEventSink::OnSetFocus`), thread foreground (`ITfKeyEventSink::OnSetFocus`), `OnPushContext`
and `OnInitDocumentMgr` are recorded and change nothing semantic: D072 §9 fixes that focus is not a semantic
incarnation and that focus loss alone is neither an epoch advance nor a composition termination, so push
and focus must not silently become discontinuity policy. Since Stage 5 a focus-loss event additionally hides
a displayed candidate *presentation* — that is presentation only: no Host unbind, no incarnation advance, no
projection or semantic-state discard, and a later admitted dispatch may re-present. Whether push/pop and
transitory contexts constitute a discontinuity remains **UNFROZEN** (§9/§18).

**Teardown reconciliation.** Every context teardown now (a) computes what is still unresolved (owned
keys, unsettled or uncertain EFFECTs, owned callbacks never issued to the document), (b) fences the
ledger to explicit terminals and unbinds the Host context, (c) bumps the entry generation so late edit
sessions and pumps can no longer match, and (d) emits one `CONTEXT_TEARDOWN` audit record naming the
counts, the resulting terminal dispositions and a session-level tally of teardowns that had to record
unresolved responsibility. It deliberately chooses no user-visible recovery: that stays P0-A/P0-B. An
external composition termination reports the same accounting in `COMPOSITION_TERMINATED`.

**Document-manager destruction** reclaims entries recorded against that document manager *and*, for
entries created while their context was not the focused top context (no recorded document manager), asks
the live context through `ITfContext::GetDocumentMgr`, so destruction cannot leave a semantically live
Fusion context and Host binding behind. A failed or foreign lookup keeps the previous behaviour.

**Tests.** `tests/tsf_lifecycle.cpp` asserts transitions, not callbacks: the obligation table; a clean
teardown; an unresolved teardown (unknown execution certainty retained as an explicit indeterminate
terminal, never-issued work reaching an explicit no-effect terminal, a late result for a destroyed context
fenced as `Stale`); external termination (projection cleared, queue drained explicitly, an applied EFFECT
never undone, identity and binding preserved, the context still usable); identity preserved across focus,
thread-foreground, push and document creation; and cross-context isolation (tearing one context down
cannot rebind, mutate or admit work for another).

Run with `ctest --test-dir build -R tsf_lifecycle`.

P0-A/P0-B/P0-D remain OPEN. Stage 4C does not decide the user-visible disposition of work that a teardown
had to leave unresolved, and does not freeze whether push/pop or transitory contexts constitute a
semantic discontinuity.

## Stage 5 — Candidate page projection and a first non-key candidate action

Stage 5 closes the loop between the engine's candidate list and the Windows client on the real Stage 4A/4B
product path. It adds no engine, ledger or recovery policy and chooses no final presentation authority: it
projects the engine's *current candidate page* across the existing IPC and turns a pointer hit back into an
explicit **non-key** candidate action.

**Projection is STATE, not a second ledger.** `Backend::send` now always appends one `CandidateSnapshot`
operation whose payload is a `CandidatePage` (`src/fcitx/candidate_projection.cpp::project_candidate_page`,
called from `observe_candidate_page`): labelled
entries that stay index-aligned with `CandidateList::candidate(i)`, a page-local cursor, the canonical
`PageableCandidateList` pager truth, and a `truncated` bound flag. When a transport bound truncates the page
to a retained prefix, the producer normalizes the engine cursor against the retained entries
(`CandidatePage::normalize_cursor_to_retained_prefix`): a still-valid cursor is preserved, a cursor whose
entry was dropped becomes no-highlight (-1), no different candidate is ever highlighted in its place, and the
engine's own cursor is not touched. Display fields pass through Fcitx's own
`Instance::outputFilter` before flattening — label, text and comment each, the same per-field call the mature
classic-ui panel display path makes (`src/ui/classic/inputwindow.cpp` at the pinned revision): candidate
display data stays Fcitx-defined (D082), Windows adds no second filtering authority and nothing is filtered
twice. Fcitx has no explicit hide signal, so the
snapshot of an *empty* candidate slot is the authoritative close. The controller records that projection as
display text on `SemanticDecision::page`; it never owns a Core record and is never a second copy of semantic
state. `SemanticController::displayed_page()` is the current projection.

**Actions are explicit stimuli, never a simulated key.** `BackendStimulus::Type` gained
`SelectCandidate`/`PageNext`/`PagePrev` with a page-local `candidate_index` (and `keysym`/`modifiers` stay
zero). `SemanticController::begin_candidate_action` reuses the exact admission, batch-validation, ordered
Core-record and incarnation/connection fencing of a real key dispatch, so a selection commits and its EFFECT
is ordered after the preceding STATE like any other effect. On the Fcitx side the selection calls the
candidate's own `select()` (partial selection rebuilds the list); paging calls the pager and re-reads the
panel. `ipc_protocol` raised `protocol_version` 2→3 for the new payload shape, so a mixed Stage 4B/5 pairing
fails the handshake rather than misparsing; the wire format and numerical bounds stay UNFROZEN (D072 §16, P0 set).

**A stale page is never acted on.** A `PageFence` (session id, binding, stimulus id, host incarnation,
connection generation) records which dispatch produced the displayed page. `begin_candidate_action` refuses
fail-open before dispatch when the fence no longer matches the live link, the index is outside the page the
user was shown, or no page is present; `fence_previous_incarnation`, external composition termination,
context replacement and destroy all drop the projection, so a page can never materialize or fire against a
new context or incarnation.

**Minimal in-process surface.** `src/tsf/candidate_view.{hpp,cpp}` is a non-activating, owned `WS_POPUP`
that paints the projected page (horizontal/vertical layout, placeholder indices preserved, cursor
highlight) and maps a `WM_LBUTTONUP` hit to a page-local index or a pager request. It owns no semantic state
and no engine; a click reports the index+generation and the service re-validates it through the same
stale-entry fence the edit sessions use before dispatching. `dumpbin /dependents` confirms the TSF DLL links
only Windows system libs plus the new `GDI32` — never Fcitx or LibIME; the engine stays solely in
`FusionHost.exe`. The caret anchor is resolved inside the read-only probe session from the authoritative
active view of the context itself (`ITfContext::GetActiveView`); a failing HRESULT, a null view or no active
view is a presentation failure only — the page is hidden, never shown unanchored, and no engine semantic
event is invented. Focus loss hides the displayed page in both `OnSetFocus` sinks (document-level and thread
foreground): presentation only, the projected page, incarnation, records and Host binding all survive, and a
normal dispatch after focus returns re-presents. `tests/stage5_candidates.cpp` locks the
presentation-failure/focus invariants at the controller level. This is the first seam only because it needs
no anchor-over-IPC and keeps the Host
headless; D072 §16 renderer placement remains **UNFROZEN** and is not decided here.

**Tests.** `tests/stage5_candidates.cpp` drives the real `SemanticController` real-frontend path against a
deterministic scripted Backend and asserts, without Fcitx/TSF/windows.h, page STATE and placeholder alignment,
select→EFFECT ordered after STATE with zero key identity, paging cursor/pager truth, the out-of-range and
stale-incarnation refusals that must dispatch nothing, truncated-page cursor normalization (inside boundary,
exactly/outside boundary, no-cursor, empty page), and the focus-loss/presentation-failure invariants (hide
only; context identity, binding, records and projection intact)
(`ctest -R stage5_candidates`, Mock-only build too).
`tests/fcitx_candidate_filter.cpp` drives the extracted `project_candidate_page` against the real Fcitx5
engine (the pinned runtime on Windows; on non-Windows builds only when the Fcitx5 development package is
present): projection identity when no consumer is connected, engine-filtered label/text/comment when one is,
and index identity, placeholder slot, cursor and pager truth preserved through filtering — the candidate
display outputFilter contract.
`tests/stage4a_ipc.cpp` extends the protocol tests to the `CandidatePage` round-trip and two new fail-closed
cases (cursor out of range, key stimulus claiming an index) plus the closure preedit/panel round-trip and its fail-closed cases (segments not tiling the text, cursor past the text, unknown format bits), and pins `protocol_version == 4`; its integration
mode adds real-engine evidence: typing `nihao` over the live `FusionHost` projects a non-empty pinyin page
across v3 IPC, and selecting a projected entry commits that entry's own text — proving selection is `select()`,
not a numeric candidate key.

**Stage 5 closure projection (the three accepted closure blockers).**
- *Faithful preedit.* `updatePreeditImpl` no longer flattens `clientPreedit().toString()`: `project_client_preedit`
  applies `Instance::outputFilter` to the InputPanel's own `clientPreedit()` Text (the dbus frontend boundary; the
  engine identity-checks that reference for password masking), normalizes it, and ships the flattened text plus a
  `FormattedPreedit` — segment byte lengths, `TextFormatFlag` bits and the engine byte cursor (-1 hidden; a cursor
  outside the text or inside a code point is shown hidden, the classic-ui rule). On the Windows build the pinyin
  defaults are `ComposingPinyin` with the cursor fixed at the beginning, so the HighLight|Underline segment is what
  shows the engine cursor. The controller rejects a projection that does not tile its text or does not sit on code
  points (CancelledRecovery, never re-interpreted); the TSF edit session maps it with `layout_preedit`
  (`fusion/text_utf16.hpp`: UTF-8 byte → UTF-16 unit, never inside a code point or a surrogate pair), puts the
  composition caret at the engine cursor (end when hidden or absent), and tags segments through
  `GUID_PROP_ATTRIBUTE` — Underline → composition input attribute, HighLight → highlighted attribute; Bold/Italic/
  Strike/DontCommit have no TSF display-attribute meaning and are not mapped. The text service is now an
  `ITfDisplayAttributeProvider` and registers `GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER` (re-run `register.ps1`).
  Attribute failures are presentation-only diagnostics; committed or externally terminated text keeps no styling.
- *Aux/panel.* The same `CandidateSnapshot` STATE now carries the panel rows of the same InputPanel update —
  `aux_up`, `panel_preedit` (server-side `preedit()`), `aux_down` — each through `Instance::outputFilter` and only
  when the InputPanel is not empty (classic-ui `InputWindow::update`). The presentation hides on
  `CandidatePage::panel_empty()` (no non-placeholder candidate and no row text), not on "no candidates": QuickPhrase
  (`;`, auxUp only, zero candidates) and the stroke filter (`` ` ``) stay visible. Candidate actions still need a
  candidate slot. No new identity or counter: the snapshot keeps the dispatch's `PageFence`.
- *Comment.* The renderer paints the already-projected, already-filtered comment after the candidate text (one
  space, the classic-ui default) in a secondary color; the slot rect covers label, text and comment, so the hit
  index stays `CandidateList::candidate(i)`.

P0-A/P0-B/P0-D remain OPEN. Stage 5 does not freeze D072 §16 renderer placement or the wire format, and does not
decide multi-window/fixed-position or full keyboard-candidate UX; `customLabel`, `spaceBetweenComment`,
`overlayMessage` and the other deferred capabilities stay as recorded in the controller's §25 disposition.

## Stage 6 WP4 — packaging and lifecycle

The packaged Host has no console window (`FusionHost.exe` is a Windows-subsystem binary). The console mode with its
control handler stays available for development and tests through `--console`; a redirected stderr keeps working in
both modes, because the CRT binds stdio to the inherited handle.

Because there is no console, logoff/shutdown reaches the Host through a hidden **top-level** window (a message-only
`HWND_MESSAGE` window receives no session-end broadcast). `WM_QUERYENDSESSION` only answers TRUE: a logoff can still
be cancelled after it (an application asks to save and the user cancels), so nothing stops there. Only
`WM_ENDSESSION` with `wParam` TRUE — the session is really ending — requests the stop and waits, bounded (4 s, the
same bound as the console path), for the ordered shutdown; `WM_ENDSESSION` with `wParam` FALSE does nothing. A named
stop event (`Local\FusionHost-stop-<user SID>-<session id>`, the same user-only DACL as the pipe and the singleton
claim) is the second non-console path: `install.ps1` / `uninstall.ps1` signal it instead of terminating the Host. All
paths only *request* the stop — `main()` remains the single thread that runs `stop()` → `shutdown_backend()` →
Fcitx's own save on instance exit, and the singleton claim is held until the process exits.

The package is a zip whose root maps onto the installed version directory `C:\Program Files\Fcitx5 Fusion\<version>\`:
`bin\` (Host, TSF DLL, `FusionFcitxHost.dll`, every runtime DLL), `data\` (fcitx5 + libime data), `lib\libime\`
(`zh_CN.lm`), `tools\` (the existing registration scripts), `install.ps1`, `uninstall.ps1` and `package.json`
(version, source commit, runtime identity, SHA-256 of every file). Fcitx resolves its data from the parent of the
directory that holds `Fcitx5Utils.dll`, which must therefore be named `bin`; every DLL sits beside the executable, so
nothing depends on `PATH`. `install.ps1` copies to the version directory, registers the TIP from there, creates or
updates the per-user logon task (interactive token, limited run level, no execution time limit, no second instance)
and starts the Host for the current user; an upgrade stops the old Host orderly, installs a new directory, re-points
registration and task, and leaves an old directory that is still locked by running applications for a later run.
`uninstall.ps1` stops the Host orderly, unregisters, deletes the task and removes the version directories it can;
`%APPDATA%\Fcitx5` is kept unless `-RemoveUserData` is given.

Known limits at this scope: an old version directory stays locked until the applications that loaded its DLL exit;
nothing restarts the Host after a crash (WP5/U7 evidence); the task, the pipe, the claim and the stop event are all
single user × session, so a machine with several interactive users needs the install per user; and no MSI, signing or
distribution channel is decided here (U4, WP8).
