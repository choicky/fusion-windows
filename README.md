# Fusion Windows

Stage 1 provides portable C++20 FusionCore, deterministic test-only FusionMock,
and executable RG-0 tests. It has no end-user Windows input behavior. Independent
review is required before integration; later stages are not implemented.

Current architecture authority is
[fusion-controller D072](https://github.com/choicky/fusion-controller/blob/main/docs/windows-architecture.md),
reviewed at `b116c65e1b903dff98ba2813bad7987293383fc9`.

## Local validation

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
clang-format --dry-run --Werror include/fusion/core.hpp src/core.cpp tests/mock.hpp tests/mock.cpp tests/rg0.cpp
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

P0-A/P0-B/P0-D, recovery UX, permanent identifiers, final protocol/IPC/Host
topology, key classification, timing thresholds, candidate renderer and
environment policy remain open. The enums here describe test/model outcomes;
they do not select those product policies. No Fcitx5/LibIME integration exists.
