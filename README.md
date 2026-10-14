# Fusion Windows

Windows product implementation for the Fusion Enhanced IME line. Today it is a
portable, sans-IO C++20 correctness core (`FusionCore`), a deterministic test-only
scheduler and fault harness (`FusionMock`), and a minimal semantic Mock Backend with
an explicit Host-side boundary (`fusion::Host`) that translates backend semantic
intent into existing FusionCore operations. There is no end-user Windows input
behavior yet; independent review is required before integration.

**Current implementation snapshot.** Stage 1 (portable FusionCore + deterministic
simulator + executable RG-0 tests) and Stage 2 (minimal semantic Mock Backend +
`fusion::Host` boundary + end-to-end Stage 2 tests) are implemented and merged into
`main`. The next stage is **Stage 2.5** (Real TSF + Mock Backend), which is not
implemented; later stages are not implemented. Current Windows stage status is
authoritative in D072 §22/§25 and is not duplicated here.

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
- `src/core.cpp` — FusionCore implementation.
- `tests/mock.{hpp,cpp}` — deterministic FusionMock scheduler, fault harness and oracles.
- `tests/mock_backend.{hpp,cpp}` — Stage 2 semantic Mock Backend (a test abstraction).
- `tests/rg0.cpp` — Stage 1 RG-0 invariant tests.
- `tests/stage2.cpp` — Stage 2 end-to-end tests (stimulus → Mock Backend → Host → Core).
- `CMakeLists.txt` — C++20 library plus CTest targets.

## Local validation

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
clang-format --dry-run --Werror include/fusion/core.hpp include/fusion/host.hpp src/core.cpp tests/mock.hpp tests/mock.cpp tests/mock_backend.hpp tests/mock_backend.cpp tests/rg0.cpp tests/stage2.cpp
```

CMake 3.20+, a C++20 compiler, and CTest are sufficient. There are no external
runtime/test dependencies and no CI workflow. Formatting uses clang-format 18.
The library alone builds with `-DBUILD_TESTING=OFF`.

## Portable model boundary

`Core` represents one Windows-owned logical context. The future TSF adapter
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
they do not select those product policies. No Fcitx5/LibIME integration exists.
