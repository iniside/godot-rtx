# Implementation Mode

Detail for **Implementation Mode - MANDATORY** in
`.agents/shared/planning-dispatch.md`. Cross-cutting dispatch rules live in
[subagent-dispatch.md](subagent-dispatch.md).

## Lanes

Choose a lane for the whole plan step or named fix. Runtime adapters map these
capabilities to tools and models actually available in the session.

- `[inline]`: the main agent handles the closed threshold below. It may also
  make a few-line local fix inside one existing function in one file, including
  an existing condition. The fix cannot introduce a function/type/binding/
  setting, public API, cross-file behavior, refactor, or threading/RID/resource
  lifecycle work.
- `[independent]`: a fresh, high-capability implementation context for whole
  features, API design, cross-subsystem work, threading, RenderingDevice/RID
  lifetime, render-graph/barrier changes, shader pipelines, or broad edits to
  upstream-tracked code.
- `[mechanical]`: a separate implementation context for fully specified rename
  sweeps, scaffolding, repeated edits, XML binding sync, `SCsub`/SCons/config
  edits, and compile fixes that remain above the inline threshold.
- `[test-author]`: the only lane that writes or updates tests, and only after an
  explicit owner request. It is a separate task after the implementation it
  covers has built. Visual/GPU work is inspected on the real path first.
- `[review]`: a fresh read-only reviewer context.

Tags approved with a plan remain authorized. Select model and effort using
shared Model Selection at dispatch time; a lane does not freeze either choice.
Do not ask again unless scope or owner constraints change.

## Dispatch Threshold

The following work is inline and gets no subagent or review round:

- comments or documentation comments
- log, error, format, or UI strings
- include add/reorder or a mechanical include sweep
- a literal or typo fix
- a one-file rename
- a few-line fix inside one existing function in one file, including an
  existing condition

This list is closed. Never split a feature into threshold-sized patches. New
functions/types/bindings/settings, public APIs, cross-file behavior, refactors,
and threading/RID/resource-lifecycle work are always above threshold.

## Dispatch And Review

Implementation subagents run per step, may edit and commit, and receive a fresh
hostile review after the task commit. Research agents are read-only and are
synthesized by the main agent.

1. Record the task-start commit before implementation.
2. Give the writer owned files, source anchors, constraints, expected behavior,
   validation boundary, the navigation chain when code is involved, and
   `comments: default NONE`.
3. Inspect the actual diff and staged set. Commit the completed above-threshold
   task before review unless the owner requested it uncommitted.
4. Capture the target SHA immediately after the task commit, then dispatch a
   fresh reviewer on that exact commit and the cumulative task-start-to-target
   diff. Independent later work may proceed in parallel without drifting the
   frozen range; serialize overlapping files or dependent APIs.
5. Commit fixes, freeze the new target SHA, and use a fresh reviewer for round
   2. Two rounds is the cap.

## Refactor Safety

- Verify removal against semantic navigation, binding/XML/config surfaces, and
  an appropriate build. `rg` alone misses virtual/macro-generated and
  script-facing consumers.
- A bound API change closes `_bind_methods()`, `doc/classes/*.xml`, property/
  signal/enum metadata, registration, defaults, and compatibility obligations.
- A new or moved source/shader file closes its `SCsub` and include/build graph.
- Preserve RID/`Ref<>`/Object/GPU ownership and thread contracts on every exit
  and teardown path.
- Check relevant raytracing/forward-renderer and D3D12/Vulkan/Metal siblings,
  or document why a sibling is not applicable.
- Keep upstream-file diffs narrow. Never hand-edit `thirdparty/` or generated
  files.
- Replace fork-local/game behavior at its authority and remove the old path in
  the same step. Preserve compatibility only for API shipped by stable upstream
  Godot.
- Choose validation by risk: editor/template and precision axes, doctool/XML,
  scene/resource load, and real-device GPU behavior. Automated tests remain
  owner-requested only.
