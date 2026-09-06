# Planning And Dispatch

## Model Selection

Select model and effort for the whole task by difficulty, uncertainty, and
failure impact, independently of the main agent's model. Use the standard
model by default; use a smaller model for simple, fully specified work, and
the strongest model for architecture, difficult diagnosis, cross-subsystem
behavior, concurrency/GPU lifetime, or complex rendering algorithms. Apply the
same judgment to research and review; neither is automatically cheap.

Roles define responsibilities, not fixed models. Adapters map these capability
tiers to available runtime identifiers. Set effort to medium for simple work
and high for ordinary or complex work unless the task or owner warrants another
supported level. Honor explicit owner model/effort choices.

Plan lanes authorize responsibilities, not frozen model choices. Select again
when dispatching; complexity discovered during work permits escalation without
renewed approval or a scope change. Do not bake model defaults into plans or
role manifests. Record the selected model and a short reason in the handoff.

## Plan Writing Workflow - MANDATORY

Before writing or reviewing a plan, read and follow
`docs/reference/plan-writing-workflow.md`. Preserve the owner's requested shape,
map overlap, and close only research gaps that can affect approval. Cover
affected APIs and consumers, behavior and ownership, files, public contracts,
dependency order, validation, and dispatch lanes; leave private implementation
details to the implementer. Tests appear only on explicit owner request.

Extraction, move, and replacement work relocates or replaces the existing
authority and adds only the requested API. Remove a fork-local or game path in
the same step that installs its replacement. An unavoidable broader mechanism
is a blocking owner question.

## Implementation Mode - MANDATORY

Before implementation or delegation, read
`docs/reference/implementation-mode.md` and
`docs/reference/subagent-dispatch.md`. Choose the lane for the whole plan step
or named fix; never divide a feature to fit the inline threshold. Use the active
adapter's tools and select model/effort by Model Selection above. Existing authorization
covers the selected lane; do not ask again unless scope or constraints change.

Commit each completed above-threshold task before review unless the owner
explicitly requested an uncommitted result. Below-threshold work has no subagent
or review round.

## Hostile Diff Review - MANDATORY

Every above-threshold task gets a fresh, evidence-based review after its commit.
Capture the target SHA immediately after that commit. Use a subagent, never
inline. The reviewer examines both the exact task commit and the cumulative diff
from the recorded task-start commit through that frozen target SHA.
Owner-requested uncommitted work instead uses its exact working diff and the same
cumulative baseline. Route code through
`docs/reference/godot-failure-taxonomy.md`.

Reject only a concrete defect, scope violation, or missing required validation
supported by a source anchor and an affected requirement or failing scenario.
List unverified concerns separately; they do not change the verdict. Do not
require unrelated hardening, invent validation, or reopen an approved design.

Every round uses a fresh reviewer spawn, including round 2 after fix commits;
never resume or message the previous reviewer. The reviewer must:

1. Read the original request, approved plan when present, exact task commit,
   frozen target SHA, and cumulative task diff; the author's report is not
   evidence.
2. Check scope first: owner shape, mechanism necessity, and no compatibility
   bridge or second source of truth for fork-local/game behavior.
3. Verify relevant claims against source and diff, then compare the work with
   each plan step: all named symbols, no unauthorized touches, and no skipped,
   stubbed, or silently simplified work.
4. Route the touched files through the Godot failure taxonomy and attack the
   applicable ClassDB, lifetime, RID, threading, shader, SCons, backend,
   upstream-compatibility, and proof risks.
5. Report each finding as `file:line`, evidence, and the affected requirement
   or concrete failing scenario. Put unresolved concerns in a separate section.
6. Return `PASS` with evidence or `REJECT` with a punch list; never rewrite the
   work or use "pass with reservations".

If a proposed fix exists only because the task introduced an unrequested
mechanism, return `REJECT - scope loop`: remove it or return to the approved
boundary. A fix needing a new abstraction, storage layer, subsystem, or public
API beyond the request/plan is a blocking owner question.

Review does not gate an independent next task. Serialize when files overlap or
the later task depends on the earlier API/behavior. A reviewer is read-only; do
not edit a file held by a live writer. Fix findings in their own commit after
any conflicting writer lands.

There are at most two rounds per task, not per commit. Commit round-one fixes
and freeze the new target SHA before round 2. Round 2 reviews the fix commit(s),
exact task history, and task-start-to-target cumulative diff; there is no round
3. After round 2, stop and report every remaining issue with its recommended fix.

Select review model and effort by Model Selection above. Changes to tests, fixtures, CI, or
other executable proof also require the named `proof-auditor`; policy-document
edits alone do not.

## Refactor Safety

Read `docs/reference/implementation-mode.md` **Refactor safety** before a
refactor or SCons dependency change.
