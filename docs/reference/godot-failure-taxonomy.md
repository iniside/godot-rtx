# Godot / C++ Failure Taxonomy - godot-rtx

This is the routing source for correctness risks a generic review misses in
this Godot 4.8-dev NVIDIA fork. Named implementation/review/proof roles select
classes from here instead of restating them. Rule text lives in
`.agents/shared/` and the linked `docs/reference/` documents.

Given a diff, use the file table at the bottom to select the relevant classes.
For each class, run its attack and verify the fix at its authority. A concern
without a concrete failing scenario is not a finding.

Each class describes the risk, an attack that can expose a plausible-looking
bug, and the authority that owns the correction.

---

## Class 1 - ClassDB Binding, Registration, And Documentation

**Risk.** A C++ member reaches GDScript, the inspector, and saved resources only
through the complete ClassDB contract: `_bind_methods()`, class registration,
property/signal/enum metadata, defaults, and `doc/classes/*.xml`.

**Attack.** Look for a setter/getter without `ADD_PROPERTY`, an enum without
`BIND_ENUM_CONSTANT`, an emitted signal without `ADD_SIGNAL`, or a method bound
under a script name different from the C++ symbol. Compare `PropertyInfo` hints
and defaults with the XML and implementation. Find ProjectSettings read through
`GLOBAL_GET` without registration/default/property info. After a rename, search
XML, `.tscn`/`.tres`, scripts, settings, and compatibility bindings for the old
name. A field that compiles but is not registered can silently fail to serialize.

**Authority.** `.agents/shared/godot-rules.md` **C++ And API Contracts**;
`docs/reference/cpp-navigation.md`; the neighboring class's binding and
registration code; `doc/classes/*.xml`; the current doctool output.

## Class 2 - Object, Ref, RID, And GPU Resource Lifetime

**Risk.** `Object`/`Node`, `RefCounted`, server RIDs, descriptor indices,
pipelines, acceleration structures, and GPU allocations have different owners
and release rules. Mixing them causes leaks, use-after-free, or device loss.

**Attack.** Follow every successful allocation through normal teardown and each
early `ERR_FAIL_*` path. Find an RID copied into two owners or freed twice; a
`Ref<T>` cycle; `memdelete` on a `RefCounted`; a raw `Object *` cached across
frames without a valid lifetime mechanism; or a pointer into a container kept
across resize. For GPU resources, check whether an in-flight frame, deferred
free queue, uniform set, or acceleration structure can still reference the
resource. Check device loss/recreation and partial initialization, not only the
happy destructor.

**Authority.** `.agents/shared/godot-rules.md` **Ownership, Threads, And
Rendering**; `core/object/`, `core/templates/rid_owner.h`, RenderingDevice, and
the surrounding subsystem's actual create/free path.

## Class 3 - RenderingDevice, Shader, And Backend Synchronization

**Risk.** RenderingDevice resources, shader layouts, descriptors, barriers,
pipeline variants, and generated GLSL headers form one contract. This fork adds
ray tracing, bindless allocation, DLSS/Streamline, and backend-specific state.

**Attack.** Trace write-to-read dependencies and required resource states across
passes. Look for a uniform set built against a stale layout, C++ and GLSL
bindings/defines that disagree, CPU/GPU struct alignment mismatches, an
acceleration structure not rebuilt after geometry changes, a bindless index not
reclaimed, or scratch resources reused before completion. A new `.glsl` or
include may be missing its `SCsub` input and leave a stale generated header.
Compare raytracing and forward-renderer siblings and D3D12/Vulkan/Metal paths;
one-backend success can hide an invalid barrier or state transition. Require
real-device evidence for visual output, synchronization, and shader execution.

**Authority.** `.agents/shared/godot-rules.md`; `servers/rendering/`,
`servers/rendering/renderer_rd/`, the touched renderer/fork path,
`drivers/{d3d12,vulkan,metal,streamline,aftermath}/`, relevant `SCsub`, and the
GLSL builders configured by `SConstruct`.

## Class 4 - Upstream Fork Drift And Compatibility

**Risk.** Each edit to upstream-tracked Godot code is future merge-conflict
surface. Stable upstream bound APIs serve user projects and GDExtension, while
fork-local pre-release APIs may be replaced without a compatibility rail.

**Attack.** Use history/upstream comparison to classify every changed API.
Reject a broad upstream function rewrite when a narrow fork hook suffices, fork
behavior smeared across unrelated upstream files, or a hand edit to `thirdparty/`
or generated files. Find stable upstream methods/properties/enums/resource
fields removed or re-signed without current upstream compatibility binding and
XML deprecation practice. Conversely, reject a migration, toggle, alias, or
dual path added for fork-local/game behavior that should be deleted in the same
replacement step. Check that history was not rewritten across upstream merges.

**Authority.** `.agents/shared/core-rules.md` **Compatibility** and **Git
Safety**; `.agents/shared/godot-rules.md` **Repository And Upstream**; git
history and current upstream practice in this checkout.

## Class 5 - SCons, Platform, And Build-Axis Integrity

**Risk.** The SCons graph varies by platform, editor/template target,
`TOOLS_ENABLED`, debug/dev defines, precision, tests, modules, and renderer
backend. One green editor build does not cover those axes.

**Attack.** Find a new/moved C++ or GLSL source absent from `SCsub`; runtime code
that references editor symbols without `TOOLS_ENABLED`; required behavior hidden
inside `DEV_ENABLED`; `real_t` assumptions that break `precision=double`; or a
platform-specific call in shared code. Confirm dependencies do not form an
SCons/module cycle. Check that the validation command actually built the
relevant target and current files. Reject `scons -c`, deleting
`.sconsign*.dblite`, or claiming that an editor build proves template behavior.
For tests, require `tests=yes` at build time.

**Authority.** `.agents/shared/godot-rules.md` **Build And Validation**;
`SConstruct`, touched `SCsub`, platform detection files, module registration,
and `docs/reference/testing.md` when tests are owner-requested.

## Class 6 - Proof And Validation Soundness

**Risk.** A build, test, scene launch, or screenshot can be green without
exercising the changed behavior. Headless tests do not prove GPU execution.

**Attack.** Verify that the executable was rebuilt from the current diff, test
support was compiled with `tests=yes`, the filter selected the expected cases,
and assertions depend on the changed branch. Check whether a unit test bypasses
the SceneTree, ResourceLoader, ClassDB binding, or other production topology.
Reject a GPU claim based only on compile success, `--headless`, Null/compatibility
rendering, or a stale capture. For RTX/DLSS/shaders/barriers, require the relevant
real backend, scene/project, configuration, and observable output. Distinguish
"covers expected behavior" from "covers the previously failing branch"; the
latter is required only when the owner requested regression coverage.

**Authority.** `docs/reference/testing.md`; `.agents/shared/godot-rules.md`
**Build And Validation**; the named `proof-auditor` role; the actual command,
log, artifact, and changed production path.

## Class 7 - C++ Style, Errors, Threads, And Crash Surface

**Risk.** Godot conventions encode ownership and runtime safety. Engine crashes
often come from a recoverable error made fatal, partial mutation, unchecked
input, or access from the wrong thread.

**Attack.** Find raw `new`/`delete`, exceptions, containers or naming that
conflict with neighboring engine code, and format/lint drift. Check every
fallible input and index for the appropriate `ERR_*` handling, and ensure an
error does not return a half-mutated object. Challenge `CRASH_*`/`DEV_ASSERT` on
runtime data. Trace main-thread `Object`/`Node` access, RenderingServer thread
ownership, worker callbacks, and RenderingDevice calls. Find editor headers in
runtime code, strings or allocation in hot render loops, stale pointers across
container growth, and comments that assert behavior the implementation does not
provide.

**Authority.** `.agents/shared/godot-rules.md` **C++ And API Contracts** and
**Ownership, Threads, And Rendering**; `.agents/shared/core-rules.md`
**Comments**; `.clang-format`, `.clang-tidy`, `.pre-commit-config.yaml`, and
neighboring source.

## Class 8 - Dispatch, Commit, And Review Discipline

**Risk.** All agents share one dirty working tree. Wrong ownership, premature
review, or an inexact diff can hide another writer's changes and corrupt the
audit trail.

**Attack.** Check that above-threshold work was delegated as one coherent step,
while threshold work did not waste a subagent or review. Confirm each writer had
exclusive owned files, explicit live-runtime tool/model selection where
available, the Godot navigation handoff, and `comments: default NONE`. Inspect
the staged set and executing-agent trailer. Review must happen after the task
commit and cover both its exact SHA and the task-start-to-`HEAD` cumulative
diff. Reject a reused reviewer, review of generic `HEAD`, a third round, a
worktree/stash/discard operation, or tests run without owner request.

**Authority.** `.agents/shared/core-rules.md` **Git Safety** and **Commit
Message Format**; `.agents/shared/planning-dispatch.md` **Hostile Diff Review**;
`docs/reference/{implementation-mode,subagent-dispatch,commit-format}.md`; the
active runtime adapter.

## Class 9 - Scope Substitution And Fix-Forward Loops

**Risk.** A technically coherent change can still replace the owner-selected
shape with a broader architecture. Later fixes then harden problems created only
by that unrequested mechanism.

**Attack.** Compare the original request/exclusions, approved plan, exact task
commit, and cumulative task diff. Find an extraction that became a second
implementation; an unrequested subsystem, cache, resource format, abstraction,
bridge, setting, or public binding; old and new fork-local paths alive together;
or review fixes whose only purpose is supporting such additions. Feasibility,
reuse, and robustness do not authorize scope. For a replacement, mark the
authority of the old behavior and verify that the same step removes it.

**Authority.** `.agents/shared/planning-dispatch.md`; `.agents/shared/core-rules.md`
**Compatibility**; `docs/reference/plan-writing-workflow.md`; the original
owner request and approved plan.

**Verdict.** If a fix exists only because the task introduced an unrequested
mechanism, return `REJECT - scope loop`. Remove it or return to the approved
boundary. If removal changes the owner-selected shape, ask one blocking question.

---

## Cross-Cutting Review Checklist

| The diff touches... | Run classes |
|---|---|
| `_bind_methods`, registration, property, enum, signal, XML, or ProjectSettings | **1**, **4**, **5** |
| `Object`, `Node`, `Ref<>`, RID, descriptor, pipeline, AS, or GPU buffer lifetime | **2**, **3**, **7** |
| `servers/rendering`, renderer RD, ray tracing, DLSS, Streamline, bindless | **3**, **2**, **5**, **6** |
| GLSL, shader include, generated shader input, or shader `SCsub` | **3**, **5**, **6** |
| D3D12, Vulkan, Metal, Streamline, or Aftermath driver code | **3**, **5**, **4**, **6** |
| editor code or runtime code referencing editor symbols | **5**, **7** |
| `SConstruct`, `SCsub`, platform detection, module flags, or precision | **5**, **4** |
| upstream-tracked code, stable bound API, `thirdparty`, or generated files | **4**, **5** |
| `.tscn`, `.tres`, project settings, imported source assets, or GDScript | **1**, **4**, **6**, **9** |
| tests, fixtures, CI, executable proof, or validation claims | **6** plus the production class |
| any C++ | **7** |
| the task diff, staging, commit, dispatch, or review process | **8**, **9** |

A clean verdict enumerates the classes attacked and evidence checked for the
files touched. A verdict with no class list or source anchors is a skim, not a
review.
