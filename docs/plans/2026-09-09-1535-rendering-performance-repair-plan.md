# Rendering performance repair

2026-09-09 UTC. Approved by the owner ("no to zaczynaj"), based on `24ad711af4`
and reviewed instrumentation `38ae7d5077`. Implementation proceeds in the ordered
steps below; approval covers their stated scope and validation boundaries.

Independent plan review: round 2 PASS. Round 1's same-address BLAS/TLAS
invalidation finding is incorporated in Step 3; canvas/HUD worker ownership is
explicit in Steps 5-6. Review confirms plan closure, not runtime performance.

## Context and required outcome

Owner request: "przygotuj plan naprawczy zaczynajac od optymalizacji zbedne pracy
a pozniej rozbicie na watki". The distinction is explicit: main thread performs
game logic and transfers scene changes; workers prepare rendering and record
commands; render thread coordinates dependencies and submission. Moving the
existing monolithic renderer to one other thread does not meet the requirement.

Order: remove unnecessary CPU work -> remove unnecessary GPU work -> establish
frame ownership -> worker preparation -> worker command recording. GPU async
compute is a separate final, measured extension, not a prerequisite for CPU jobs.

Preserve imported meshlets/DAG and CLAS, nondeforming microgeometry, rigid and
MultiMesh motion, existing deformables, simple RT error/offscreen controls,
offscreen RT visibility, debug views and freeze semantics. No quality tuning,
foliage, voxelization, new reflections, split screen, XR/VR support, mesh format
changes, replacement job system, new renderer, or automatic quality/importance
algorithm. The owner explicitly excluded split screen and XR/VR during execution;
manual validation uses one game viewport and normal editor operation. Appearance is
the owner's decision. No automated tests are proposed or authorized.

## Evidence and existing authorities

Measurements: [performance status](../research/2026-09-09-1456-microgeometry-performance-status.md).
600-frame Vulkan/RTX4090 captures, 1280x720, identical quality and timestamp budget
2048: moving RTXDI CPU intervals approximately 19 ms each, still 0.7-0.9 ms;
RT Prepare/Publish/Transform GPU intervals dominate native TLAS construction.
Main profiling reports separate rendering disabled. These are reported batch
means/wall intervals; exact helper CPU attribution and profiler overhead remain
unmeasured. Invalid timestamp-overflow runs are excluded.

Current-source anchors (navigation: clangd and implementation reads, then bounded
settings/shader/history inspection; source inventories are lower bounds):

- `servers/rendering/rendering_device.cpp:5266`,
  `_uniform_set_add_acceleration_structure_dependencies`: repeated TLAS/BLAS/CLAS
  traversal; `_cluster_tracker_push_unique` uses linear `LocalVector::has`.
  `blas_build_from_clusters` attaches the supplied CLAS storage set per BLAS.
- `servers/rendering/renderer_rd/storage_rd/micro_geometry_storage.cpp:650`,
  `get_clas_dependencies`: all allocated asset page CLAS storage, not selected cut.
- `servers/rendering/renderer_rd/shaders/forward_clustered/micro_geometry_rt.slang:95`
  and `:173`: one lane per geometry, serial full-cluster scan even for transforms.
- `servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp:1998`,
  `_build_micro_geometry`: Prepare/Publish cycling without a selection-input dirty
  predicate; `build_tlas:2694` mixes gathering, mutable registries and buffer work.
- `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp:446`,
  `_prepare_micro_geometry`, `:1430` `_fill_render_list`, `:2301` `_render_scene`:
  per-pass task construction and shared mutable instance/list/scene state.
- `main/main.cpp:5171` waits in `RenderingServer::sync()` before the next draw;
  startup around `:2807` forces nonseparate rendering in editor/project manager.
- `RenderingServerDefault` already owns a command queue and a WorkerThreadPool
  rendering pump. Extend this authority, not a second producer/consumer server.
- `RenderingDeviceGraph` has secondary-command-buffer allocation/record/wait
  helpers, but the existing worker record helper is not evidence of active task
  dispatch. Close actual scheduling in Step 6 instead of assuming it exists.
- `rendering_device.cpp:162` sets `SECONDARY_COMMAND_BUFFERS_PER_FRAME` to zero;
  graph `add_draw_list_end:2495` always uses primary recording. Graph instruction
  storage (`rendering_device_graph.h:845`) is shared/reset per frame. Existing
  `SecondaryCommandBuffer` and secondary execution instructions are reusable;
  deprecated public split-draw APIs are not the worker interface.
- `RenderingDevice::initialize` and `execute_chained_cmds` use the combined
  graphics/compute main queue. The transfer queue is not async compute.

Overlap decisions: retain `RenderingServerDefault`, `WorkerThreadPool`,
`RenderingDeviceGraph`, persistent geometry records, `RTViewportState`, and the
existing completed-submission retirement mechanism. Extend their internal
ownership/data flow. Per-frame immutable inputs and task-local recording state
are necessary for concurrent consumers of currently mutable state; they do not
introduce a second authoritative scene or a general scheduling framework.

## Step 1 -> Remove repeated quadratic CPU dependency work [independent]

Files: `servers/rendering/rendering_device.{cpp,h}`; inspect the CLAS dependency
producer in `micro_geometry_storage.{cpp,h}` and its RT consumers.

Add bounded helper-level CPU timing/work counts through the existing profiler
to confirm attribution, then replace linear duplicate search with linear/near
linear unique construction. Form the transitive BLAS/CLAS dependency union when
the owning AS dependency topology changes, and reuse it at dispatch. Register
the required resource usages with the graph on every consuming command; caching
the dependency set must never suppress a barrier or an AS/storage read hazard.

Own the derived union on the existing AS/resource lifetime. Invalidate on BLAS
dependency replacement, TLAS rebuild/update, CLAS storage changes and retirement;
RID reuse must not resurrect tracker pointers. Cover GPU-indirect build paths
and early exits. Do not require GPU selection readback to narrow dependencies:
the conservative complete set is valid once duplicate/repeated construction is
cheap. Remove the old per-dispatch quadratic path in this step.

Why first: measurable CPU work reduction without changing thread ownership or
rendering output. Acceptance: direct helper cost/work counts establish the cause,
quadratic unique-search behavior is gone, moving RTXDI CPU preparation improves,
and actual Vulkan AS dependencies remain correct through reload/streaming.

## Step 2 -> Remove serial full-mesh GPU preparation [independent]

Files: `forward_clustered/render_raytracing.{cpp,h}`,
`shaders/forward_clustered/micro_geometry_rt.slang`, related shader build inputs.

Retain committed/candidate nonempty or active-count metadata so the transform
path writes only transforms, motion and instance references: O(instance count),
without scanning mesh membership. For changed cuts, distribute clearing,
membership comparison and reference emission across cluster workgroups with
bounded reductions/prefix offsets and a small per-instance finalization pass.
Avoid per-instance rescanning of the same MultiMesh selected bin. Preserve stable
selected triangle identity and consistent reference/build destination ordering.

Publish candidate membership and counts atomically as one generation. Preserve
empty/nonempty transitions, initial/restore modes, debug frozen selection,
retirement pins, indirect capacity handling and double-precision origins. Close
C++/Slang layouts, shader variants, dispatch/barrier dependencies and buffer free
paths. Remove the old serial cluster loop rather than retaining a selectable
legacy implementation. No new simplification policy or import-format change.

Why now: eliminates the measured GPU topology bottleneck before scheduling.
Acceptance: transform dispatch does no cluster scan; changed-cut GPU work scales
with parallel cluster batches; timings improve at unchanged controls and scene.

## Step 3 -> Recompute only when relevant inputs change [independent]

Files: `render_raytracing.{cpp,h}`, `micro_geometry_selection.{cpp,h}`,
`micro_geometry_storage.{cpp,h}`, persistent geometry dirty-update producers in
`render_forward_clustered.{cpp,h}` where necessary.

Use the existing per-view build/selection state to distinguish topology/cut
changes, transforms, and no change. Reuse prepared tasks, dependency sets and
unchanged buffer ranges. Stop unconditional Prepare -> Publish -> Prepare cycles.
Update/rebuild TLAS when instance data, referenced BLAS addresses or BLAS content
generations change, or when RD invalidates it. A BLAS build/refit can invalidate
TLAS while retaining the same address: preserve the existing deformation path
and `_blas_remove_tlas_dependencies` contract, including same-address updates.
When motion stops, settle previous/current transforms and motion-history state
before skipping their updates; a stale previous transform must not persist.

Selection inputs include view/projection and resolution/error controls, instance
transform and bounds, mesh/MultiMesh topology and visibility/layers, material
eligibility/emission/deformation, origin changes, residency readiness/generation,
and freeze/restore state. Motion can require selection; it must not be blindly
classified as transform-only. TIME/deformed/GPU-authored instance paths keep
conservative invalidation when no reliable dirty signal exists.

Version pending feedback against the selection inputs that produced it; changes
during feedback cannot be lost or publish stale state. Outstanding streaming
requests and newly ready pages keep progress alive even with a still camera.
Retain current valid committed cut until a valid replacement is ready. Do not
reset lighting history for unchanged geometry. Share view-independent task data,
but keep camera/shadow/RT selections and per-view history distinct.

Why now: makes later worker jobs small and event-driven. Acceptance: a settled
still scene has no repeated cut rebuild/publication/upload work; camera/object
motion, page arrival, settings and reload still trigger all necessary updates.

## Dense-instance extension authorized during execution

The owner supplied Lucy and Thai and requested at least 5000 instances of each,
then explicitly required real instancing and streaming. Fresh independent review
of this extension passes against `0d1ed0abe9`; implementation remains separate.
SceneTree/game-logic
performance remains outside this repair. Insert Steps 3A and 3B before Step 4:
they establish the instance/cut resource ownership consumed by later worker jobs.
No new simplification solver, quality tiers, serialized mesh format, public
setting, or streaming subsystem is authorized or needed.

Frozen `984dccbd32` capacity evidence: RT reserves 16 bytes per instance times
full asset DAG cluster count before other storage and rejects work above
`UINT32_MAX / 8`. The source-triangle lower bound already exceeds that guard
2.77 times. Shared raster/RT selection independently replaces normal selection
with a global coarse fallback above 4194304 work items. Existing storage already
shares source assets, resident pages and CLAS, but RT allocates a BLAS per
instance and pins/attaches overly broad page sets. Removing one guard is not a
solution. See the performance status for exact counts and source evidence.

## Step 3A -> Sparse GPU selection over shared asset DAGs [independent]

Files: `forward_clustered/micro_geometry_selection.{cpp,h}`,
`shaders/forward_clustered/micro_geometry_select.slang`, selection consumers in
`render_forward_clustered.{cpp,h}` and `render_raytracing.{cpp,h}`; existing shared
shader layouts as required.

Replace per-instance full-DAG group/cluster arrays and dispatch ranges with
bounded GPU traversal queues and sparse state keyed by task, instance and group.
Keep imported DAG metadata shared once per asset. Start from terminal groups,
apply the existing error/offscreen/residency predicates, and emit compact selected
records. A refined group becomes active only when all unique parent groups are
active; retain depth ordering and deduplicate shared DAG descendants. Preserve
stable source cluster/triangle identity, task/instance routing and material
eligibility. Keep a compact rejected-cluster list for raster HZB recovery.

Replace the global work-limit/coarse-fallback path and fixed tiny extra-cluster
allowance per task. Queue/selected-buffer overflow retains the affected task's
valid committed output, records required capacity, and grows/retries or continues
bounded batches. It must not force unrelated tasks to a coarse cut. A task with
no committed output may use its resident terminal cut during refinement;
force-finest surfaces keep their contract and wait for required geometry.
Physical resource admission failures stay explicit. Do not raise limits into
unbounded allocations or suppress safety guards.

Keep camera raster, RT and internal shadow predicates distinct. No worker/thread
ownership changes here. Close C++/Slang bindings, strides, variants, barriers,
allocation failures and resource retirement; preserve Step 3 dirty-input and
motion semantics. Existing Slang compilation units remain the build authority.

Why now: sparse selection is needed independently of RT BLAS sharing. Acceptance:
no instance-count times full-DAG working allocation or unconditional scan remains
in the shared selector; single-view Vulkan selection, HZB recovery and residency
progress work. The full 10k RT scene still awaits Step 3B, so this intermediate
step cannot claim dense-scene completion. Build ordinary/double, record source
and binary hashes, and obtain a fresh exact/cumulative review. No automated tests.

## Step 3B -> Compact shared RT cuts and streaming ownership [independent]

Files: `render_raytracing.{cpp,h}`, `micro_geometry_rt.slang`,
`storage_rd/micro_geometry_storage.{cpp,h}`, and necessary compact-selection
interfaces from Step 3A. Extend existing RD contracts only if an actual missing
native ownership/dependency operation is established; no public binding is planned.

Replace per-instance full-DAG membership/reference/group-usage storage and
full-capacity BLAS allocations with compact, immutable shared cut objects. GPU
PREPARE canonicalizes selected `(cluster ID, page generation)` sequences using
integer phases in the existing RT pipeline. Asset/source-surface identity and
geometry-affecting build flags qualify equality. Hashes only locate candidates;
GPU exact sequence comparison establishes identity and groups equal cuts.
Process collision buckets in bounded rounds with continuation. Existing float
SortEffects is not an exact integer sorter and must not be used as one.

Keep geometry-to-cut mappings and cluster sequences on GPU. Bounded asynchronous
feedback supplies only new representative descriptors/counts/capacities, slot
usage and unique per-cut page/generation associations. Do not transfer selected
cluster or triangle topology to CPU for interning. Feedback arenas support
continuation; incomplete feedback cannot publish a partial cut. The existing
PREPARE/feedback/PUBLISH epoch remains authoritative, with one candidate epoch
in flight and explicit producing-input generations.

CPU owns cut slot IDs/generations, compact GPU storage, BLAS RIDs, references,
page pins and submission-based retirement. GPU receives representative-to-slot
and address mappings. Identical cuts reuse one BLAS; distinct instance transforms,
SBT indices, masks and motion remain in individual TLAS records. Bootstrap one
resident terminal cut per unique asset/source surface for ordinary eligible
surfaces. Transform-only changes preserve shared BLAS. Different membership or
page generation uses a new cut generation while old committed users remain live.

Size destinations and scratch from selected-cut capacities and actual unique
build batches. Current RD requires unique destination RIDs and common-capacity
compatible batches; repeated addresses belong in TLAS instance records, not BLAS
build destination lists. Attach each cut's exact CLAS page dependency subset.
Keep compute/CLAS/BLAS/TLAS graph hazards and same-address invalidation sound.

Extend existing MicroGeometryStorage rather than adding a second streamer.
Replace ready-group-wide temporary pins with one bounded lease over the resident
page snapshot for the selection epoch. This protects pages until exact candidate
pins arrive; free slots may still receive uploads. Release the lease promptly on
completion, cancellation or errors, and validate generations before publication.
Committed/candidate/retired cut owners retain their exact page pins until CPU
feedback and relevant GPU submissions are finished. Unrelated page eviction must
not invalidate every BLAS. Preserve shared asset/page/CLAS allocations and terminal
page residency, and report actual metadata/geometry/CLAS/BLAS/scratch budgets.
Imported manifests determine asset admission; do not silently multiply budgets
by instance count or duplicate asset payloads.

Why before threading: this replaces the GPU/CPU resource ownership that worker
jobs will consume. Acceptance uses the actual single-view Lucy/Thai fixture with
5000 instances of each, unchanged RT controls, bounded shared residency and
working storage proportional to queued/selected records and unique cuts. Report
logical instance count, unique assets/cuts/BLAS, resident/pending pages, memory,
CPU/GPU time and setup separately. Exercise camera motion and streaming progress;
no appearance tuning, SceneTree scalability repair, split-screen or XR/VR matrix.
Preserve empty/nonempty, reload, cancellation and shutdown lifetimes. Build
ordinary/double, inspect real Vulkan execution and obtain fresh source and proof
reviews. Exact performance/fit is measured, not guaranteed by the source bound.

## Step 4 -> Separate main from rendering with bounded frame ownership [independent]

Files: `main/main.cpp`, `servers/rendering/rendering_server_default.{cpp,h}`,
`rendering_server_globals.*`, relevant `renderer_scene_cull.*`,
`renderer_viewport.*`, `scene/main/scene_tree.cpp` and command-queue call sites
identified by semantic closure;
`doc/classes/ProjectSettings.xml` if the existing default changes.

Use the existing separate rendering model as the RTX default and remove the
editor/project-manager forced fallback for this path. Preserve shipped enum,
setting and explicit synchronous API contracts; no duplicate thread-model setting.
The normal RTX execution path, including editor operation, must do no scene
culling, render preparation, graph recording or GPU submission on main.

Replace the unconditional previous-draw sync with bounded admission of render
frames through the existing FIFO command authority. Main publishes changes and
frame requests; rendering consumes a stable version while later changes remain
queued. Move render-side `pre_draw`/tick work off main; main-only callbacks and
Object/Node operations stay on main. No workers read live SceneTree objects.
Preserve main `frame_pre_draw` notification. Associate deferred post-draw
callbacks/completion records with their admitted frame; do not share an
unqualified callback list across several outstanding frames.

Frame inputs retain references/generations until all consuming worker jobs finish;
GPU storage remains retained until its submission completes. Reuse existing
frame slots/retirement where applicable. Do not copy meshes or the whole scene
each frame. No lost mutation/free command, dropped callback or unbounded queue.

No synchronous wait for the newly enqueued current draw. Bounded backpressure
may wait for an older occupied frame slot; explicit synchronous getters, flush,
readback and teardown retain ordered barrier semantics. These exceptional waits
are measured separately and never make main execute renderer work. This is a
bounded pipeline, not a promise that CPU can outrun GPU indefinitely.

Why now: establishes ownership before parallel consumers. At this intermediate
step rendering may still be serial behind the boundary; it is not the final
threading result. Acceptance: main trace contains transfer/admission only,
main/renderer overlap is observable, editor startup and callbacks remain valid.

## Step 5 -> Move CPU rendering preparation into worker jobs [independent]

Files: `renderer_scene_cull.{cpp,h}`, `renderer_viewport.{cpp,h}`,
`renderer_canvas_cull.{cpp,h}`, `renderer_rd/renderer_canvas_render_rd.{cpp,h}`,
`forward_clustered/render_forward_clustered.{cpp,h}`,
`render_raytracing.{cpp,h}`, `micro_geometry_selection.{cpp,h}`,
`storage_rd/micro_geometry_storage.{cpp,h}` and relevant renderer scene data types.

Extend WorkerThreadPool use. Split scene gathering/culling, raster bin/list and
instance-data preparation, RT instance/material/build descriptors, and page
decode/upload payload preparation into jobs over immutable frame inputs. Batch
objects/surfaces instead of scheduling a tiny task for every meshlet. Resolve
shared dirty storage/pipeline updates once before readers, or with one owning job
and explicit dependency. GPU uploads consume prepared payloads later.

Replace shared `scene_state`, render lists, per-instance depth/sort writes and RT
scratch vectors with task/pass-local outputs where concurrently accessed. Merge
by deterministic offsets/stable IDs; keep temporal data per viewport and shadow
pass parameters separate. Workers may depend on other jobs, but main never waits
on them and the render coordinator does not perform their preparation itself.
GPU-free parsing/preparation must not call unrestricted RD or mutate shared
descriptor caches. Retain existing asynchronous storage IO and extend its output
ownership instead of adding another streaming system.

Assign canvas/HUD culling, interpolation, list preparation and upload-payload
preparation to rendering worker jobs as well. Preserve ordered SceneTree and
canvas mutations and main-only signals. Canvas passes and scene effects use the
same recording-task boundary in Step 6; do not leave their preparation loops in
the render coordinator merely because the dominant initial profile is 3D.

Why now: correct snapshot lifetimes exist from Step 4. Acceptance: thread trace
shows named preparation jobs on multiple workers for sufficient workload, main
has no preparation, coordinator work is bounded orchestration, and workers are
not serialized by one global renderer lock. Small scenes may use fewer jobs.

## Step 6 -> Record commands on workers; render thread coordinates/submits [independent]

Files: `servers/rendering/rendering_device.{cpp,h}`,
`rendering_device_graph.{cpp,h}`, relevant `rendering_device_driver.*`,
`drivers/vulkan/rendering_device_driver_vulkan.{cpp,h}`, and consuming renderer
pass entry points from Step 5. Shared interfaces require sibling backend closure.

Activate/extend actual graph task dispatch rather than relying on unused worker
helpers. Give recording tasks isolated command-list/allocator state and declared
resource usages. Reconcile usages and derive barriers/order in the existing graph;
record independent draw, compute and RT work with task-owned command contexts
and driver command pools. No blanket disabling of RD thread guards and no global
lock around whole pass preparation/recording. Public RD calls keep their existing
ownership contract; worker recording is an internal graph contract.

Coordinator schedules dependencies, joins ready recording results and submits
ordered command buffers; data-heavy graph compilation/recording also runs as
worker tasks. Serialize genuine resource dependencies, not every viewport or
pass by default. Keep script compositor/render callbacks on their documented
execution thread and isolated from concurrent mutation; do not run arbitrary
script callbacks on worker threads. This is not moving `_draw` wholesale into
one worker. Remove replaced serial fork-local preparation/recording paths.

Frame/job cancellation, resize, viewport removal, reload and shutdown join jobs
before command pools/CPU inputs are released; GPU retirement still waits for
completion. Prevent worker-pool starvation from nested jobs and a blocking pump.

Acceptance: CPU trace separates main, coordinator, preparation and recording;
independent recording overlaps; main does no rendering; coordinator contains
primarily synchronization/submission rather than the previous monolithic work.
This step completes the required CPU architecture.

## Step 7 -> Bounded GPU async compute extension [independent, after Step 6]

Files: RD/graph/driver interfaces and Vulkan backend from Step 6; only selected
microgeometry preparation pass annotations in the renderer.

First use a GPU timeline to select genuinely independent work, initially next
candidate-cut/page-CLAS preparation overlapping raster/shadows. If hardware/work
dependencies offer no useful overlap, retain the same graph on the current queue
and report the measurement; do not claim a benefit or expand into unrelated passes.
For supported overlap, add compute-queue assignment to the existing graph with
explicit semaphore waits/stages, ownership transfers where required, AS/storage
dependencies, scratch ownership and completion covering every participating queue.
Never reuse the single-main-queue retirement serial as proof of compute completion.
Current consumers wait for the data they need; no unsynchronized same-frame RT.

Acceptance: a real Vulkan timeline demonstrates overlap and net frame benefit,
including synchronization overhead. Shared graph contracts compile for supported
sibling backends; lack of compute overlap uses the same scheduling graph on one
queue. This extension must not delay measuring/completing Steps 1-6.

## Validation and completion

After each step: scoped commit and fresh exact/cumulative hostile review, then
matched before/after manual profiling at unchanged quality. Relevant failure
classes: 1/4 for startup/settings/API compatibility, 2/3/7 for jobs/RID/GPU
lifetimes, 5 for shader/backend/build closure, 6/8/9 for proof and scope.

The owner additionally authorized a dense workload scene under
`demos/rtxdi_manual` using `rawcontent/lucy.glb` and
`rawcontent/thai_statuette.glb`, with at least 5000 instances of each mesh.
Import both through the existing microgeometry importer and preserve the source
files. Use shared imported mesh resources and separate static object instances,
placed densely with a single camera, to exercise CPU preparation and GPU
selection. Keep setup out of steady-state measurements. SceneTree/game-logic
scalability repairs are explicitly outside this task; report their cost separately.
This fixture is a
separate delegated task; its source, import evidence and measured instance count
require build/source review and proof audit. Do not add automated tests.

Use `demos/rtxdi_manual/microgeometry/scene.tscn` with still and moving cases,
timestamp budget 2048 and recorded executable/source hashes. Add only needed
native profiler counters: invalidations/rebuilds, helper dependency work, job
execution/queue/wait, frame admission and submission. Compare profiling enabled
and disabled to quantify instrumentation overhead. No new profiler framework.

Structural checks are mandatory: no cluster scan for transforms; no unchanged
cut rebuild after settling; no rendering stacks on main; real worker preparation
and recording; no hidden monolithic render-thread bottleneck. Report measured
main active/transfer time separately from backpressure, render/worker CPU time,
GPU time and frame latency. Aim for sub-ms main rendering transfer on this scene;
do not promise an arbitrary total GPU target before fixes are measured.

Exercise already available rigid/MultiMesh motion, a single viewport, shadows,
deformable fallback, freeze, residency/reload, resize and shutdown as manual
lifetime checks. Preserve corrected empty/nonempty and far-plane behavior.
Do not restart the paused appearance/PCK/emissive quality verification matrix.

Build ordinary and double editor for shader/layout changes, and template_debug
for startup/threading changes; validate bare editor/project-manager launch plus
real Vulkan rendering. Close ClassDB/XML only where bound contracts/defaults
change, shader/SCsub inputs where new compilation units are needed, and explicitly
record unsupported or unverified backend axes. No generated/thirdparty edits.

Keep measurements and canonical project state current. Each full step uses a
separate implementation context; model/effort is selected by repository policy
at dispatch. Comments default NONE. No implementation approval is inferred from
the independent plan review; owner approval is recorded at the top of this plan.
