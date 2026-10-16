# Fusion Windows

Windows product implementation for the Fusion Enhanced IME line. Today it is a
portable, sans-IO C++20 correctness core (`FusionCore`), a deterministic test-only
scheduler and fault harness (`FusionMock`), a minimal semantic Mock Backend with
an explicit Host-side boundary (`fusion::Host`) that translates backend semantic
intent into existing FusionCore operations, and a Stage 2.5 real Windows TSF/COM
development shell (`FusionTsf`). Stage 3B/3C provide the real Fcitx5 backend PoC
and faithful frontend path. Stage 4A adds an out-of-process Fcitx Host
(`FusionHost.exe`) connected by length-prefixed binary IPC over a local Named Pipe.
There is no end-user Windows input product yet; independent review is required
before further integration.

**Current implementation snapshot.** Stage 4A (out-of-process `FusionHost.exe` + IPC) is
unmerged work on a Draft PR; current Windows stage status is authoritative in D072
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
- `include/fusion/backend.hpp` — Stage 3B/4A `fusion::Backend` interface and `BackendStimulus`/`BackendBatch`/`BackendOperation` types.
- `include/fusion/ipc_protocol.hpp` — Stage 4A IPC protocol: length-prefixed binary framing and serialization (platform-free).
- `src/core.cpp` — FusionCore implementation.
- `src/tsf/` — Stage 2.5 real TSF/COM development shell (`FusionTsf` DLL): service, edit sessions, key map, semantic adapter, diagnostics, registration. The TSF DLL builds on WIN32+MSVC regardless of `FUSION_WITH_FCITX5`. When `FUSION_WITH_FCITX5` is ON, it additionally links the IPC backend to talk to FusionHost.exe.
- `src/ipc/` — Stage 4A IPC transport: `pipe_client.{hpp,cpp}` (overlapped I/O client), `pipe_server.{hpp,cpp}` (per-connection multi-instance server, Host-side only), `pipe_security.{hpp,cpp}` (per-user pipe name, server identity verification, security descriptor), `ipc_backend.{hpp,cpp}` (Backend over IPC).
- `src/host/` — Stage 4A FusionHost.exe: `main.cpp` (entry point), `host_server.{hpp,cpp}` (multi-client accept loop, per-connection threads, backend mutex, context remapping).
- `tests/mock.{hpp,cpp}` — deterministic FusionMock scheduler, fault harness and oracles.
- `tests/mock_backend.{hpp,cpp}` — Stage 2 semantic Mock Backend (a test abstraction).
- `tests/rg0.cpp` — Stage 1 RG-0 invariant tests.
- `tests/stage2.cpp` — Stage 2 end-to-end tests (stimulus → Mock Backend → Host → Core).
- `tests/stage3b.cpp` — Stage 3B real Fcitx backend tests.
- `tests/stage3c.cpp` — Stage 3C faithful frontend tests.
- `tests/stage4a_ipc.cpp` — Stage 4A IPC tests: protocol serialization, loopback pipe, HostServer (multi-client, transport-failure separation, clean shutdown) and the FusionHost.exe integration mode. The first three modes run in the Mock-only build too; only the integration mode needs `FUSION_WITH_FCITX5=ON`.
- `tests/tsf_semantic.cpp` — Stage 2.5 deterministic tests for the key map and semantic adapter.
- `tools/tsf/*.ps1` — reversible development registration/unregistration/state check for the TSF service.
- `CMakeLists.txt` — C++20 library plus CTest targets; the TSF DLL builds on WIN32+MSVC; FusionHost.exe builds when `FUSION_WITH_FCITX5=ON`.

## Local validation

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
clang-format --dry-run --Werror include/fusion/core.hpp include/fusion/host.hpp include/fusion/backend.hpp include/fusion/ipc_protocol.hpp src/core.cpp src/tsf/*.hpp src/tsf/*.cpp src/ipc/*.hpp src/ipc/*.cpp src/host/*.hpp src/host/*.cpp tests/mock.hpp tests/mock.cpp tests/mock_backend.hpp tests/mock_backend.cpp tests/rg0.cpp tests/stage2.cpp tests/stage3b.cpp tests/stage3c.cpp tests/stage4a_ipc.cpp tests/tsf_semantic.cpp
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
The Hello handshake exchanges `protocol_version` (currently 1) and the backend
`session_id`. Version mismatch returns version=0 and the client disconnects.

**Pipe name and security.** The default pipe name is computed from the user SID
and session ID (`\\.\pipe\FusionHost-v1-<SID>-<sessionId>`), ensuring per-user
per-session isolation. The pipe is created with `PIPE_REJECT_REMOTE_CLIENTS`
and a DACL granting access only to the current user (Medium IL baseline per
D072 §14). The client verifies server identity by comparing the server process
token SID with its own after connect.

**Multi-client and context remapping.** The Host accepts multiple concurrent
clients, each on its own pipe instance. Each connection has its own thread and
remaps client context IDs to Host-unique context IDs. On disconnect, the Host
unbinds all contexts owned by that connection. All backend calls are serialized
through a shared mutex.

**Transport failure separation.** `IpcBackend::send()` distinguishes pre-write
failure (transport never saw the request → `Forward` with no ops) from
post-write failure (request on the wire, response lost → `Reject` + mark
unhealthy). `healthy()` is local state only — no IPC probe.

**TSF DLL fail-open.** When `FUSION_WITH_FCITX5` is ON, the TSF DLL attempts
one IPC connection with a 200ms timeout during `Activate`. If the Host is
unreachable, the DLL activates without a backend and all keys are genuine PASS
(fail-open). The DLL does not auto-launch the Host (P0-D OPEN).

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
  separation (pre-write `Forward` vs post-write `Reject`), `healthy()` performs no IPC
  (server-side received-frame count), and `stop()`/join shutdown without hanging
- `integration`: full FusionHost.exe + Fcitx backend over IPC (requires a running
  FusionHost.exe and the Fcitx runtime; set `FUSION_STAGE4A_SKIP_INTEGRATION=1` to skip
  instead of fail)

Run with `ctest --test-dir build -R stage4a`.

P0-A/P0-B/P0-D remain OPEN; Host startup/lifecycle belongs to P0-D. Recovery UX, permanent
identifiers, final wire format, timing thresholds, candidate renderer and environment
policy also remain OPEN. The IPC protocol is an engineering constant, not a frozen wire
format. If an application activated the IME before `FusionHost.exe` was started, switch the
IME away and back in that application to connect.
