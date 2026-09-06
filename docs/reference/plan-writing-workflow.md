# Plan Writing Workflow

Detail for **Plan Writing Workflow - MANDATORY** in
`.agents/shared/planning-dispatch.md`.

Front-load approval-changing decisions. A plan lets the owner review scope,
contracts, dependency order, and validation; it does not prescribe every private
line of implementation.

## Step 0 - Preserve The Owner's Shape

Start Context with the requested outcome and explicit exclusions in the owner's
terms. Research supplies facts and constraints; it does not turn a narrow
replacement, move, or feature into a broader architecture.

For each unrequested subsystem, cache, resource format, bridge, abstraction, or
public contract, name the requirement that cannot be met without it. If none
exists, omit it. If it appears necessary and changes scope, ask one blocking
question.

For fork-local or game replacement work, remove the old path in the same step
that installs the replacement. Do not plan a compatibility bridge, parallel
authority, or cleanup deferred until the new path is "proven." Stable upstream
Godot API is the exception and keeps upstream compatibility obligations.

## Step 1 - Map Overlap

For a proposed new module, server, `Node`, `Resource`, effect, shader pipeline,
setting, or replacement, document each meaningful existing candidate: what it
does, how the request differs, and why extending it cannot satisfy the requested
shape. Include both upstream Godot and NVIDIA-fork surfaces.

## Step 2 - Close Approval-Changing Research Gaps

Resolve questions that can change scope, public/serialized compatibility,
threading, RID/resource lifetime, shader/backend topology, feasibility, or the
owner's requested shape. Give every dispatched question one owner and reuse
sufficient completed research. Record smaller implementation uncertainties with
their impact instead of blocking approval.

## Step 3 - Cover Three Evidence Angles

- **Affected APIs and contracts:** C++ signatures, ClassDB bindings,
  `doc/classes` XML, ProjectSettings, scenes/resources, shader layouts, and
  formats created, moved, changed, or removed.
- **Consumers and usages:** construction, call, registration, loading, render
  pass, backend, and serialized data-flow sites that must change or remain valid.
- **Behavior, ownership, and patterns:** the current authority, Object/Ref/RID
  and GPU lifetime, thread boundary, relevant upstream/fork pattern, and SCons
  build ownership.

All three are required for an API plan, whether gathered directly or delegated.
Synthesize them in the main context.

## Step 4 - Write Concrete Specifics

Name affected files and symbols, behavior and owner, public/serialized
contracts, dependency order, and final validation. Include exact signatures or
API calls when they constrain consumers or scope. For Godot changes, explicitly
close relevant ClassDB/XML/registration, `SCsub`/GLSL include, RID/free,
threading, `TOOLS_ENABLED`, renderer/backend sibling, and upstream-compatibility
obligations. Leave private implementation choices to the implementing context.

## Step 5 - Write An Ordered Sequence

Use `Step 1 -> Step 2 -> ...`, not a catalog. Every step states:

- **what:** exact files/symbols and the old fork-local/game path removed;
- **why now:** the dependency that places it before the next step;
- **how:** the behavior/contract actions and non-mechanical closure that constrain
  correctness;
- **dispatch:** `[inline]`, `[independent]`, `[mechanical]`, or
  owner-requested `[test-author]`, using
  [implementation-mode.md](implementation-mode.md).

A step may leave an intermediate build broken when later approved steps complete
the same replacement, but it cannot keep two authorities alive. The final
implementation sequence states the appropriate build, docs/resource load, and
real-device validation boundary.

### Tests Are Owner-Requested

Do not propose, create, update, or run automated tests unless the owner asks.
When requested, put them in a separate later `[test-author]` step after the
covered implementation builds. State the observable requirement, branch or
contract tested, discovery/filter expectation, and topology. GPU correctness
needs real-device validation; a headless proxy does not replace it.

## Step 6 - Review The Plan

For an above-threshold implementation plan, use the adapter's fresh named
`hostile-reviewer`. Give it the original request, plan, relevant source anchors,
Godot failure classes, and the adapter's default/approved effort. It returns a
punch list, not a rewrite.

The reviewer rejects only an evidence-backed scope violation, logical hole,
missing approval-changing decision, rule conflict, or required validation gap.
It does not reopen the owner's shape or require unrelated hardening. Address
findings before presenting the plan, or state any deferred item and its impact.
