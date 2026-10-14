# AGENTS.md — fusion-windows

Repository-local execution rules for the `fusion-windows` product repository. This
file applies to the repository and all subdirectories unless a future narrower
`AGENTS.md` overrides it.

These rules are deliberately **short**. They do not restate cross-project governance.
`fusion-controller` is the canonical home for requirements, accepted decisions,
roadmap, normative architecture and branch policy. Where a rule below says "see
controller …", read that authority rather than inferring it from this file. Do not
build a parallel Windows governance framework.

## 9.1 Authority / source-first

- Project Instructions are the highest project-level authority for a given task.
- `fusion-controller` holds the normative cross-platform requirements, decisions,
  roadmap and architecture.
- `fusion-controller/docs/windows-architecture.md` (**D072**) is the normative Windows
  architecture authority; `fusion-controller/docs/branch-policy.md` (**D076**) is the
  normative branch governance authority. Both are the current-state authority for
  their subjects.
- `fusion-windows` owns Windows product implementation, tests, and repository-local
  execution rules and the repository entry point (README).
- Evidence files, experiment reports, gate/preflight reports and agent final reports do
  **not** automatically become Accepted Decisions, and are not current stage-status
  authority; current Windows stage status is read from D072 §22/§25 plus the merged
  `main` reality.
- Important assumptions must be checked against the current source and real tests, not
  against document wording, history, memory, or interface names alone. If accepted
  design conflicts with latest source/API/tests, STOP and report the conflict; do not
  silently pick one side.

## 9.2 Task scope / Change Contract

For meaningful work record, as needed: `Goal`, `Deliverable`, `Authority / Inputs`,
`Allowed Scope`, `Must Preserve`, `Expected Behavioral Delta`, `Hard Stop`,
`Validation`, `Required Artifacts`, `Commit / Push / PR policy`. Keep small, clearly
non-behavioral tasks lightweight; do not turn every edit into a large process.

## 9.3 Modification boundary

- Make the smallest change that satisfies the current task; no unrelated refactor, no
  speculative future infrastructure.
- Do not silently advance the product into a later Windows stage.
- Preserve the D072 responsibility boundaries; D072 OPEN/UNFROZEN items (D072 §23 and its
  P0 set) remain OPEN until explicitly accepted by a new Accepted Decision.
- Do not turn a test/model abstraction or enum into product policy.

## 9.4 Actual diff + negative diff

Before an important commit/push/review, inspect the actual base, actual HEAD, the
changed-file list, and the complete relevant base-to-HEAD diff (including newly created
files), plus surrounding source / API / call-chain context where needed. Explicitly
answer: *Besides the intended/requested behavior, what else changed?* An implementation
agent's final report is a claim index, not a substitute for reviewing the actual code.

## 9.5 Review discipline

Default to one comprehensive primary review that identifies all known blockers in that
pass where practical. Classify findings as `BLOCKING` / `IMPORTANT BUT NON-BLOCKING` /
`MINOR`. Cheap in-scope non-blocking issues may be fixed in the same pass; otherwise
defer them. Minor / style / theoretical-hardening findings do not create another review
cycle. After blocker fixes, normally perform at most one targeted blocker-only
re-review; another round requires a newly exposed material correctness, architecture,
safety or evidence-integrity problem. Keep small issues small.

## 9.6 Validation

Use risk-proportional validation and preserve the three tiers:

- **Gate 1 — Source / Semantic Review**; **Gate 2 — Mechanical / Static Review**
  (full actual diff, `git diff --check`, available formatter/static tooling,
  include/namespace/declaration consistency); **Gate 3 — Real Validation** (actual
  compiler/build, unit/product-path tests, CI or device tests).
- Never claim Gate 3 from Gate 1/2. If no compiler/build was actually run, write
  `STATIC REVIEW PASS; COMPILE UNVERIFIED`.
- Use the real Windows/native build, test, formatter and sanitizer tooling available at
  the current stage; do not invent a permanent Windows build-environment assumption
  before the real toolchain exists. Prefer local authoritative validation where
  available.
- Run CI only when required by the Task Contract, a release gate, a repository rule, a
  GitHub-specific validation need, or explicit user request; never to repeat a local
  validation that already succeeded.
- A failing test is not permission to weaken assertions, oracle, expected values,
  fixtures, behavioral inputs, or required invariants.

## 9.7 CLOSED evidence / blocker discipline

Do not reopen a CLOSED / VERIFIED matter because of an older TODO, a historical report,
or superseded current-state text. Reopen only on a substantive contradictory new fact
(product behavior/source change, dependency/artifact/version change, upstream API or
license change, new device evidence contradicting an accepted result, CI/release
regression, a decision-specific reopen condition, or reliable evidence directly against
current authority). Distinguish `CLOSED / VERIFIED`, `BLOCKER`, `VALIDATION DEBT`,
`FUTURE / DEFERRED`; "more testing is possible" is not itself a blocker.

## 9.8 Windows Hard Stop

Hard Stop only for architecture-level conflicts, for example: an accepted D072
responsibility boundary cannot be implemented; a backend would have to become the
Windows ownership authority; recovery would redefine ownership; two document
materialization authorities would become necessary; correctness would require competing
causal ledgers; or a forbidden dependency/layer must be modified. Ordinary
compiler / CMake / test / harness / format problems are **not** Hard Stops — fix them
autonomously within scope and continue.

## 9.9 Workspace artifact lifecycle

- `~/worktmp/fusion-windows/<task>/` — durable recovery workspace for checklist/state
  and important raw evidence that must survive interruption.
- `/tmp/fusion-windows/<task>/` — disposable/regenerable build, extraction, inspection
  and diagnostic scratch.
- `/dev/shm/fusion-windows/<task>/` — only explicit small, short-lived, fully-disposable
  RAM scratch.

Git, commits, PRs and accepted project documents remain the durable authority and the
normal review handoff. Do not treat `/tmp` as project authority.

## 9.10 Git / PR workflow

For meaningful implementation: authority/current-source check → task branch/worktree →
Change Contract → minimal implementation → local validation → complete actual + negative
diff review → commit → push → one coherent PR → independent review → blocker-only repair
if needed → targeted re-review → owner merge decision.

- Branch namespace follows D076 §1a: `main` is the only long-lived trunk; task branches
  use `feature/*`, `fix/*`, `docs/*`, optional `experiment/*`, `integration/*` (only when
  genuinely needed), `ci/*`, and `release/<version>` (exceptional). Do **not** create
  `phase/*` or `stage/*` branch categories.
- Do not commit directly to `main`. Do not merge without explicit authorization. Do not
  force-push or rewrite shared history unless explicitly authorized and D076 permits it.
- One PR represents one coherent logical change.

## 9.11 Autonomous execution

Within a clear approved scope, ordinary reversible steps are pre-authorized: inspection,
editing, building, testing, formatting, ordinary in-scope build/test repair, committing,
pushing the task branch, and opening the requested PR. Do not repeatedly ask permission
for routine steps. Stop for genuine product/architecture ambiguity, scope expansion, a
destructive/irreversible action, credentials/secrets/signing/payment/security-sensitive
action, a conflict with Accepted Authority, or an explicit Hard Stop.

## 9.12 Documentation maintenance

Apply the controller documentation-reconciliation rule
(`fusion-controller/AGENTS.md` → 「文档一致性维护 / Documentation Reconciliation」).
Documentation maintenance is **not** append-only writing. Before editing README or any
future Windows documentation: read the affected existing content first, classify each
existing statement (current-and-correct / state-changed / obsolete / partially
superseded / detailed-evidence / duplicated-fact / misleading-to-current-action), then
update / supersede / remove / consolidate in place rather than mechanically appending;
preserve historical evidence only where useful; update current status / next-step
wording when reality changed; remove stale actionable instructions; and search for
contradictions after editing. Do not duplicate controller Requirements / Decisions /
Roadmap into `fusion-windows`.

## 9.13 Final reports

Keep final reports concise and evidence-based: what changed; the actual validation
performed; commit / branch / push / PR state; blockers or remaining risk. Do not claim
unexecuted validation. Do not create extra closure work once the review gate is
satisfied.
