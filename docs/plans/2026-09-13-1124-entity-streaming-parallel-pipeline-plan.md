# Entity Streaming Parallel Pipeline

## Context And Approval Decisions

Replace the current streamed-cell install path with a dependency-driven,
array-backed pipeline which keeps the owner and render threads bounded while
allowing prepared results to arrive several frames later. This supersedes the
architecture of
`docs/plans/2026-09-13-0707-streamed-cell-install-throughput-plan.md`; its
landed-in-the-working-tree read batching, columnar decode, Flecs bulk install,
metadata consolidation, and initial render preparation are the starting point,
not a second path to preserve.

The proposed approval shape is:

- Keep every one of the 5,001 individual text files in each stress cell, their
  names, paths, format, and lazy discovery behavior. Do not pack, replace, or
  delete them.
- Vendor and use **enkiTS** as the entity subsystem's dependency scheduler.
  It replaces entity streaming's `WorkerThreadPool` task IDs, group waits,
  polling state machine, and manual successor dispatch. It does not replace
  Godot's global `WorkerThreadPool` for unrelated engine work.
- Keep one Flecs world owner on the current owner thread. All preparation is
  parallel; `ecs_bulk_init` and structural Flecs mutation remain one short,
  exclusive publish. Flecs stages do not make concurrent bulk creation safe.
- Keep Flecs **4.1.6** initially. The checkout already contains the current
  latest stable release and its exact upstream revision. A future stable
  release may replace it only after the same workload shows a material win and
  the C/C++ API, observers, lifecycle hooks, and Windows double build remain
  correct; tracking upstream `master` is not part of this work.
- Remove per-entity hashes from the streaming/catalog/world/render hot paths.
  Persistent IDs remain authoritative, but arbitrary-ID entry points use a
  bounded set of sorted locator runs. Once resolved, all work carries a stable
  block generation and row index.
- Preserve atomic **logical** cell activation in the catalog/Flecs world.
  Rendering may become ready progressively over later frames. During partial
  render admission, picking and culling see only coherent completed rows.
- Keep persistent `EntityId`, generational Flecs handles, renderer RIDs,
  prefab ownership, transform parenthood, and cell membership as distinct
  identities. Individual renderer RID/Instance semantics remain.
- Keep public `EntityScene` bindings and `.escn` serialization unchanged. Add
  no ProjectSettings, compatibility bridge, cache format, multiview/XR path, or
  automated tests.

The observed baseline comes from the owner's earlier retained runs against the
current dirty implementation: read wall is about 0.6 s per 5,001-file cell;
decode about 229 ms; stable 10k Flecs creation about 2.3-3.6 ms; transform
finalization about 1.4-2.1 ms; metadata about 6.0 ms; total owner install about
24.0 ms; initial packet preparation about 4.6 ms; renderer apply about 42 ms,
reference work about 6 ms, and dirty completion about 29 ms. These are the
comparison baseline, not fresh proof for this plan.

## Why These Authorities Are Extended Or Replaced

| Existing authority | Decision |
| --- | --- |
| `EntityScene::CellJob` and `WorkerThreadPool` tasks | Replace the manual `ENUMERATING -> READING -> DECODING -> READY` state machine with one enkiTS dependency graph per request incarnation. No coordinator task waits on child work. |
| `PreparedSet`/`PreparedGroup` | Keep columnar ownership during decode, but address rows directly and transfer the metadata block at publish. Remove ID and group-pointer lookup maps. |
| `EntityCatalog::Record` plus `SectionMap`/`OrderMap` facades | Replace as the streamed metadata authority with exact-sized cell blocks and sparse row overlays. The facades and their callers disappear in the same step. |
| `EntityWorld::residents` and `changed` hashes | Replace with `RowState[]`, per-block dirty bitsets, and compact row queues. Component storage remains solely in Flecs. |
| `EntityRenderPacket` and renderer `native_entities` hot lookup | Replace with column batches addressed by block/row/incarnation and a budgeted render-thread admission queue. Cross-block references still resolve through persistent identity at a boundary. |
| Flecs stages/threads | Use only where normal Flecs systems later benefit. They are not an install mechanism: stage merge is serial and `ecs_bulk_init` requires exclusive access to the real world. |
| Godot `ResourceLoader` | Use its threaded request/status/get API for missing assets; the entity DAG resumes affected decode ranges after the external dependency completes. Do not synchronously load missing assets in `_commit_cell`. |

`enkiTS` 1.11, tag commit
`6ffccbdb1000253d8d513dd7a5ae9226e5023a5c`, is selected over the alternatives
because its maintained API supports
C++11, task dependencies, completion actions, priorities, range tasks,
external threads, custom allocation, and up-front allocation-friendly
scheduling. Current Taskflow 4 requires C++20, oneTBB adds a much broader
runtime, and Marl's fibers solve blocking waits rather than this pipeline's
explicit DAG. Vendor a pinned upstream revision and its zlib license using the
repository's third-party provenance policy.

## Target Ownership And Data Flow

```text
owner request + immutable catalog snapshot
                  |
                  v
       enumerate / plan file ranges
                  |
             read ranges  * N
                  |
       merge + parse + sort + count
                  |
        allocate exact cell columns
                  |
             decode ranges * N
             /             \
   missing asset gate       hierarchy/index build
             \             /
       transform layers * N
                  |
       render columns / change rows * N
                  |
        completed batch mailbox
                  |
     owner revalidate + Flecs bulk publish
                  |
       logical cell activation (atomic)
                  |
     render-thread budgeted admission
                  |
     coherent rows visible over later frames
```

`CellRuntimeBlock` owns exact-sized source metadata arrays, stable row IDs,
topological order, child adjacency, archetype-to-row spans, and parallel
`RowState[]`. A row state contains catalog activity, Flecs handle, revision,
pin/dirty flags, render state, and optional overlay index. Base source metadata
is immutable; an edited row selects exactly one replacement record from a
sparse overlay, so there is no second active truth.

The catalog's only arbitrary-ID structure is an owner-thread collection of
immutable sorted `EntityId -> {block_id, block_generation, row}` runs. A cell
load adds one run; bulk background compaction merges runs when their count
crosses a fixed internal threshold; inactive rows and stale generations are
discarded during compaction. Lookup binary-searches newest runs first. This
preserves lazy discovery and avoids both a concurrent hash and an O(total)
sorted-vector insertion for every cell. Worker input receives an immutable
snapshot; it never reads the live catalog.

Clean ordinary rows leave the locator on cell release. A block remains while
it contains a dirty, pinned, prefab, dependency, or tombstone row, and is freed
only when its last retained row is gone. A first implementation may therefore
retain one 5,001-row metadata block for one pinned row; retained-block bytes are
measured explicitly. Relocation into a small retained block is not silently
added unless the measured memory requires it.

Every asynchronous result carries document/world generation, `CellKey`,
request incarnation, block generation, row incarnation, expected entity
revision, and expected external-parent revision. Release/re-request, edit,
delete, reparent, undo, save, play-document replacement, and scenario teardown
invalidate stale work through these tokens rather than waits or shared mutable
flags alone.

## Ordered Implementation Sequence

### Step 1 - Vendor enkiTS and replace entity job scheduling `[independent]`

Add a pinned enkiTS import under `thirdparty/enkits`, its license/provenance,
`thirdparty/README.md` and `COPYRIGHT.txt` entries, and the SCons integration.
Add `scene/entity/entity_task_scheduler.h/.cpp` as the single adapter and own
one process-wide scheduler from scene entity subsystem initialization through
`unregister_scene_types()` shutdown. Size its worker set once from available
hardware while reserving the owner, render, and ingress threads; entity work no
longer also enters `WorkerThreadPool`.

The owner never calls `AddTaskSetToPipe` for read, decode, transform, or packet
work. enkiTS is allowed to execute a submitted task immediately when its pipe
is full, so the owner pushes only a small request descriptor into a bounded
MPSC ingress queue. One Godot-created, named ingress thread registers as the
configured enkiTS external caller, builds/submits graphs, and deregisters at
shutdown; pipe-full fallback therefore executes only on that ingress thread.
The queue applies backpressure before accepting a request, rather than making
the owner execute worker work. enkiTS worker start/stop hooks establish thread
names/profiling and the implementation audits every Godot API used by those
foreign workers; Object/RID/resource-server access remains on its established
owner. Saturation instrumentation records the actual caller thread for every
phase and fails the step if read/decode/transform/packet code runs on owner.

Replace `CellJob::task/group/read_ranges/read_lanes` orchestration in
`scene/resources/entity_scene.h/.cpp` with preallocated graph nodes and
successor dependencies. Preserve sorted file order, 64-file read granularity
as the initial measured quantum, cancellation, missing-only resume, nearest
cell priority, and mandatory graph collection. Use separate high priority for
release/edit work, normal priority for wanted cells, and low priority for
locator compaction and distant prefetch. Completion actions enqueue owned
results to an owner mailbox; they never touch `EntityScene` after destruction.

Remove the old manual polling/dispatch implementation in this step. Keep a
bounded byte budget and graph-node arena per active request so task scheduling
does not allocate in steady state. Build the primary Windows double editor and
compare identical read/parse work against the existing scheduler before the
next structural change.

### Step 2 - Install the block/row catalog authority `[independent]`

Replace `scene/entity/entity_catalog.h` storage and its consumers in
`scene/resources/entity_scene.*`, `scene/entity/entity_scene_io.cpp`,
`scene/entity/entity_scene_commands.cpp`, `scene/entity/entity_world.*`, and
`editor/scene/entity/entity_scene_editor.cpp` with `CellRuntimeBlock`,
`RowState`, sparse effective-row overlays, exact child adjacency, and sorted
locator runs. Add a `.cpp` and update `scene/entity/SCsub` if ownership or
destruction requires out-of-line code.

Remove `catalog.records`, `SectionMap`, `OrderMap`, per-entity cell/global/
resident/dirty/pin/prefab membership hashes, `PreparedSet.lookup`, and the
group-pointer index in the same step. Preserve lazy ordinary-cell discovery,
deleted source paths until successful save, prefab reconciliation, undo,
separate edit/play documents, pinned ancestors, unload refusal rules, and the
current `get_record_count` meaning. Component values remain Flecs-owned after
publish; prepared columns remain transient job-owned transfer buffers.

All scalar editor/runtime commands resolve an ID once and then use a row
location. Per-block dirty bitsets plus compact row indices replace `changed`;
removal events copy the persistent ID before their row becomes inactive.

### Step 3 - Build the complete parallel preparation DAG `[independent]`

Refactor the producer stages in `scene/resources/entity_scene.*`,
`scene/entity/entity_record_parser.*`, and
`scene/entity/entity_component_schema.*` into disjoint range tasks:

1. Merge and sort read results off-owner.
2. Sort IDs/signatures, detect adjacent duplicates, count rows per archetype,
   prefix-sum exact column sizes, and allocate once.
3. Decode row ranges in parallel. Every range owns error, missing-asset,
   construction, and profiling output; shared columns are written only at
   disjoint rows. Each lane sets and clears its own thread-local cached-only
   schema state.
4. Build parent row indices, child adjacency, roots, and dependency layers.
5. Compute transform, effective visibility, reset revision, component masks,
   and render columns in parallel by hierarchy layer. External parents are
   immutable snapshot inputs tagged with their expected revision.

Replace synchronous `_load_cell_assets` with
`ResourceLoader::load_threaded_request`, status polling at the owner boundary,
and `load_threaded_get` only after completion. Resume only affected decode
ranges; completed files and rows are not reread or reconstructed.

An owner-thread asset-ticket table owns every successful threaded request.
Each ticket must reach exactly one `load_threaded_get`, including load failure,
cell cancellation, document destruction, save flush, and engine teardown;
cancellation suppresses downstream decode but does not abandon the loader
token. The ticket retains the returned `Ref<Resource>` in the cell job's owned
asset set until every resumed decode range has completed or acknowledged
cancellation, matching the current `cell_assets` lifetime. Shutdown first stops
new requests, then drains outstanding tickets and graph acknowledgements, and
only then destroys the scene and enkiTS scheduler.

Backpressure uses actual pending bytes and task counts, not only a 1-4 cell
clamp. Cancellation stops scheduling successors, but buffers are reclaimed only
after all already-running tasks acknowledge completion.

### Step 4 - Reduce owner publication to validation and Flecs mutation `[independent]`

Refactor `_revalidate_job`, `_commit_cell`, `EntityWorld::_materialize_bulk`,
and `EntityTransformSystem::finalize_created` so the owner performs only:

- generation/incarnation/revision and current-demand validation;
- conflict reduction for rows changed while the job ran;
- one `ecs_bulk_init` per final archetype and rare parent-edge installation;
- `RowState` handle/revision updates and one atomic logical block activation;
- enqueue of the already-owned render batch.

Do not copy metadata into new maps, rebuild hierarchy, reread Flecs columns,
or recompute initial transforms here. Invalid external-parent snapshots requeue
only the affected subtree calculation. Edits win over older streamed data.
An in-flight cell which is no longer wanted is rejected before Flecs install.

Keep the whole 10k-row Flecs publish atomic initially because its measured cost
is already in the low milliseconds. If it alone exceeds the gate after the
other work is removed, the same step may use bounded hidden-install chunks plus
a private activation marker, but only after every public resolve/query/editor/
render path demonstrably excludes pending rows. Do not move the world to a
dedicated thread: current synchronous `resolve/get/query`, runtime camera tick,
editor inspection, teardown, and Flecs table access all share its owner.

Apply the same model to later component changes: resolve once, prepare compact
row ranges on workers, owner-apply structural changes or contiguous value
writes, increment row revisions, and send narrow render columns. Remove the
per-entity dirty hash path rather than retaining it for scalar updates.

This step also replaces the complete live transform rail, not only
`finalize_created`: migrate `EntityTransformSystem::mark_dirty`, `update`,
`begin_tick`, `interpolate`, and `EntityWorld::teleport` from the current
`dirty/moving/render_dirty/reset` hash sets to per-block bitsets and compact row
ranges. Worker snapshots compute hierarchy-layer current transforms, inherited
visibility, and affected descendants; the owner reduction publishes current
state and reset revisions. At tick start, previous pose is captured before
current-state publication; interpolation reads one stable previous/current
epoch and writes render poses for the same epoch; renderer batches carry that
epoch so a late component update cannot restore an older pose. Moving rows stay
in a compact block list until current equals target, while teleport advances
the reset revision and atomically sets previous/current/render pose before the
next interpolation epoch. Moving membership follows the actual interpolation
invariant `previous != current`; tick rollover copies `current` into `previous`
before that membership is reduced. Remove the old live sets and serial
depth-sort path in the same step.

### Step 5 - Replace wide render packets with owned column batches `[independent]`

In `scene/entity/entity_render_system.*`, replace `EntityRenderUpdate`,
`EntityInitialRenderGroup`, `initial_updates`, `prepare_initial`, and the
all-component sparse publication loop with one immutable owning batch format:

- block/request/row incarnations and publication sequence;
- compact row indices and persistent IDs;
- pose/reset/visibility columns;
- signature groups containing only present component columns;
- narrow changed-row/component columns for later edits;
- explicit remove/cancel records and an acknowledgement token.

Workers build these columns from their owned prepared snapshot. They do not
retain Flecs table pointers, call `Resource::get_rid()`, or touch renderer
objects. The owner assigns ordering only when accepting a valid result.
Cross-block references retain persistent IDs until the render owner resolves
them; packet-local references use row locations.

Update the fork-local declarations together in `servers/rendering_server.h`,
`servers/rendering/rendering_method.h`,
`servers/rendering/rendering_server_default.h/.cpp`, and
`servers/rendering/renderer_scene_cull.h/.cpp`. There is no ClassDB/XML binding
to preserve. `CommandQueueMT` must transfer an owning payload; the receiver may
not retain a callback reference.

### Step 6 - Budget complete render admission and retirement `[independent]`

Replace synchronous `RendererSceneCull::scene_publish_entities` application
with renderer-owned pending batches and a per-frame scheduler. A work quantum
must finish a coherent row or resumable component subphase and account for:
RID/base creation, component slot application, reference reconciliation,
dependency changes, BVH/spatial update, publication-caused dirty resources,
and retirement. Merely limiting the packet loop is insufficient because scene
queries, frame update, and free paths currently force global dirty drains.

Keep unfinished rows out of visible/cullable scenario structures and out of
the ordinary global dirty list. Prioritize unload/cancel and live edits above
initial distant-cell admission, guarantee progress every render frame, request
continued redraw while work remains, and enforce pending-byte/age limits.
Advance accepted and completed sequences separately; completion acknowledgement
uses an owned mailbox token and never calls a destroyed scene/world object.

Cancel/unload invalidates the residency serial immediately. Reloaded IDs get a
new row incarnation, so old work and acknowledgements cannot mutate or free the
new instance. Preserve unresolved-reference refresh, lowest-ID camera and
environment selection, reset revisions, individual RIDs, and submission-serial
GPU retirement. Render completion means coherent cull state, not proof of GPU
presentation.

### Step 7 - Integration closure and measured validation `[independent]`

After every implementation step, build
`bin/godot.windows.editor.double.x86_64.exe`. Step 1 also closes build ownership:
entity and enkiTS sources are compiled/initialized only inside the existing
`not env["disable_3d"]` scene branch; editor consumers remain `TOOLS_ENABLED`
only; the runtime scheduler and shutdown path contain no tools dependency; and
the renderer batch virtual method is updated at the shared RenderingServer /
RendererSceneCull boundary used by every compiled 3D backend rather than added
to one backend only. The final boundary additionally builds the primary Windows
`target=template_debug precision=double` configuration so non-tools runtime
initialization, third-party compilation, and shutdown are covered. Threadless
platform support is not introduced by this fork-local 3D feature; any platform
that compiles it must provide the engine's normal thread support.

At the final boundary, use the
unchanged 5,001-file-per-cell input in runtime and the actual editor, stationary
and on the controlled camera route, with one Vulkan/RTX viewport. Do not add or
run automated tests.

Record per phase and per frame:

- file/range/task counts, enkiTS ready/running/dependency latency, scheduling
  allocations, worker utilization, and queue bytes/age;
- enumerate/read/merge/parse/count/decode/hierarchy/transform/render-column
  wall and CPU time, missing-asset wait, cancellation latency, and peak owned
  memory;
- sorted-locator run count, lookup/compaction cost, retained block bytes, and
  zero per-entity hash operations in the instrumented hot path;
- owner revalidation, Flecs bulk/parents, activation, component update, and
  worst frame stall;
- render admission/apply/reference/dirty/retirement time, budget overshoot,
  first-visible latency, fully-ready latency, and stale-result drops.

The acceptance gates for a uniform 10k-row cell are: owner logical install p95
at most 5 ms and maximum at most 8 ms; non-structural owner component apply p95
at most 2 ms; render publication p95 at most 2 ms per frame with any indivisible
overshoot reported; no renderer-wide unbounded dirty drain attributable to a
pending cell; and no regression in warm read throughput. Full render readiness
should complete within eight render frames after logical activation for the
uniform stress cell; mixed heavy components are reported separately because
one indivisible resource operation may define their bound.

Exercise missing-asset resume; cancel in every DAG and render state; unload and
reload the same IDs; edit/delete/reparent/undo during load; pinned and dirty
ancestor retention; prefab and external-parent resolution; save/Save-As;
edit/play document replacement; cross-cell references arriving in either
order; component add/remove and bulk value updates; resource reload; scenario
release; and clean shutdown. Verify counts, filenames, contents, and checksums
of all stress files are unchanged.

## Dispatch And Review Order

Each step is above the inline threshold and owns one coherent replacement.
Dispatch one step at a time because the authority and touched files overlap.
Select the strongest architecture/concurrency-capable implementation context at
high effort, commit the completed step with only owned paths staged, then use a
fresh hostile reviewer against that exact commit and the cumulative task diff.
Renderer-thread proof gets a separate proof audit only if the owner later asks
for executable tests or fixtures. Stop after two review rounds per step and
report any remaining issue rather than adding an unapproved bridge.
