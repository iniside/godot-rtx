# Remove recurring CPU microgeometry preparation

Approved by owner ("zaczynaj") on 2026-09-10; plan prepared 08:07 UTC. Source baseline 2108bbedf1701f1a27ed59511c4a898e8ec6e856.

## Outcome and scope

On the existing 5000 Lucy + 5000 Thai scene, stationary objects and a moving camera must not cause full CPU scans to rediscover microgeometry, rebuild task arrays, reconstruct bins, compare snapshots, or hash scene records. CPU ingests object/resource changes, services asynchronous page requests/uploads and records/submits work. GPU evaluates camera/light visibility, DAG cuts, cluster selection, indirect raster work and RT geometry inputs. CPU work may scale with actual changes, resident resource/batch management and fallbacks; it must not repeatedly scale with all unchanged eligible instances.

This supersedes the remaining performance direction of the 2026-09-09 rendering repair plan. Keep already implemented workers and graph infrastructure, but do not extend generic renderer threading as the next optimization. No proof-auditor, proof packages, independent acceptance audits or mandatory multi-round review. At most one short code review focused on concrete correctness risks. Owner instruction overrides the earlier repository workflow for this task. Acceptance is: builds, game works, game gets faster at unchanged settings.

Keep the current import format, meshoptimizer/DAG, storage/page lifecycle, real instancing, shared cuts/CLAS, RT error and offscreen multiplier, and triangle/cluster debug view. Nondeforming geometry only; existing deformation/custom-vertex/material fallback continues. One game viewport. No Nodes/Flecs rewrite, foliage, voxels, reflections, XR/VR/split screen, generic scheduling framework or quality reduction.

## Current diagnosis and existing authorities

Native ordinary Vulkan sample: camera raster prepare about4.2ms, RT scene gather about5.1ms, RT microgeometry prepare about5.9ms, CPU interval labelled TLAS Build about2.8ms. These are enclosing intervals, not pure algorithm timings or additive GPU costs. Full-record hashing alone measures3.47ms. No whole-frame speedup is established. Latest 8c8833 fixes recording diagnostics; 2108bbed adds limited record-hash reuse. Both compile in the combined ordinary build; their runtime/performance is unmeasured.

Extend existing mechanisms, not a new GPU scene subsystem:
- RendererSceneCull::_scene_cull (renderer_scene_cull.cpp:3178) currently traverses scenario instance_data and fills camera/shadow/RT instance lists (:3247/:3262/:3281).
- RenderForwardClustered::_prepare_micro_geometry (:449) discovers eligible surfaces on CPU, builds tasks/material bins and a19-word-per-task snapshot before reuse. Camera discovery walks geometry_surface_compilation_all_list.
- RenderForwardClustered::GeometryInstanceForwardClustered dirty/data-dirty hooks, dependency callbacks (:5508), persistent_instance/persistent_surfaces and motion updates already express lifecycle and changes.
- RaytracingRender persistent instance/surface/material records and publication/release paths already own stable data. Extend them as the authority for registered eligible surfaces and change publication.
- MicroGeometrySelection::{Task,Pass,create,select,recover} already own GPU task/selection/indirect buffers. micro_geometry_select.slang already implements GPU selection and raster output.
- MicroGeometryStorage already owns asset metadata, asynchronous feedback/read tasks, resident pages and CLAS. Preserve it; its CPU I/O/resource management is legitimate.

## Step1 -> Publish changed microgeometry data only [independent]

Files: forward_clustered/render_forward_clustered.{cpp,h}, render_raytracing.{cpp,h}; renderer_scene_cull.{cpp,h} and existing renderer geometry interface only where needed for change/routing notification. Storage files only for actual asset readiness/replacement notifications.

Use existing create/free, scenario, transform, material, visibility/layer, MultiMesh and dependency dirty hooks to register, update and remove eligible microgeometry surfaces. Keep persistent task records and material/pipeline membership alongside their existing owner. Update only affected slots/ranges; publish a stable GPU scene version for the frame. Topology/material changes can rebuild affected batches; camera motion cannot. Preserve generation-checked identity, motion history and deferred GPU retirement. Handle initial registration, unload/reload, resource replacement and transition to/from the existing unsupported-material/deformed path.

Replace the per-frame microgeometry list-production authority as the consumer is switched; no parallel legacy list retained as validation fallback. This is renderer ingestion, not a new scene model. Remove the full-record/subset hashes and snapshot machinery once their consumers use explicit changes; the just-landed hash cache is temporary work to delete in the same replacement, not another retained authority.

Why first: raster and RT need stable inputs before their repeated preparation can be removed.

## Step2 -> Raster and shadows consume persistent GPU inputs [independent]

Files: render_forward_clustered.{cpp,h}, micro_geometry_selection.{cpp,h}, shaders/forward_clustered/micro_geometry_{select,inc}.slang, plus renderer_scene_cull routing/interface files fromStep1.

Replace _prepare_micro_geometry discovery/task/snapshot/bin reconstruction with reuse of registered GPU records and stable compatible material/pipeline bins. Each pass updates only camera/light parameters and small pass state. GPU evaluates visibility, layers/scenario, shadow casting, HZB and DAG selection and emits indirect commands/counts. Preserve surface/material semantics, depth/shadow behavior and the existing debug views. CPU binds actual bins and records a bounded set of passes; no per-instance/per-meshlet draw loop.

Remove eligible microgeometry from per-view CPU camera/shadow collection and _fill_render_list input production. A per-instance CPU early-continue inside the same full scan is not the intended endpoint: maintain the conventional CPU work domain on membership changes. Keep noneligible geometry and other scene objects on their existing route. This bounded routing change must not become a generic scene-culling rewrite.

Why second: removes the measured4.2ms producer and supplies a GPU-authoritative raster path before RT shares the same published inputs.

## Step3 -> RT consumes persistent scene and GPU-produced geometry work [independent]

Files: render_raytracing.{cpp,h}, micro_geometry_selection.{cpp,h} where shared, micro_geometry_rt.slang / micro_geometry_inc.slang; RD/graph/Vulkan interfaces only for the concrete existing AS-input contract that needs extension.

Existing mode15 in micro_geometry_rt.slang already writes microgeometry TLAS descriptors. The missing replacement is the CPU layout rebuilt by prepare_frame():655-665, build_tlas():3297, append_micro_surface():3726-3844 and finalize_buffers():2773-2799. Keep geometry_base/motion_base, TLAS custom_index/SBT offset, material/hit-program and motion consumers consistent through one stable indexing contract; use retained slots for eligible records and a defined noncolliding range/remap for existing conventional/deformed records. Preserve correct lookup when classic geometry count or material programs change. Updating input membership without these consumers is not sufficient.

Replace microgeometry entries rebuilt by build_tlas scene gather and _prepare_micro_geometry with persistent registered instance/surface/task data. Keep RT membership independent of camera raster visibility. GPU selects the RT DAG cut with existing error/offscreen controls, assigns existing shared cut/BLAS groups, writes RT instance transforms/masks/addresses and drives the available indirect CLAS/BLAS work. Same geometry and pure object movement must not rebuild page CLAS. Only changed cuts/residency/topology trigger affected BLAS work. Preserve classic/deformed geometry and mixed RT scenes through their existing supported route.

Choose the existing tlas_build_from_buffer contract: host admitted slot count/capacity changes on membership/growth, GPU writes active or inactive descriptors. Do not require GPU-compacted TLAS count in this plan. Current Vulkan CmdBuildAccelerationStructuresKHR primitiveCount is host-supplied; CLAS/cluster-BLAS source/count buffers are already GPU-readable. Measure inactive-slot overhead and maintain/compact the admitted layout on topology changes, not camera motion. Existing cut publication may allocate/retire host BLAS RIDs when representatives change; retain that and exact page pins.

Retain CPU allocations/capacity reservations and necessary driver API command metadata; do not promise device-generated submission or unsupported indirect TLAS counts. Remove CPU processing per unchanged microgeometry instance, not required host command submission. Keep GPU-to-CPU feedback asynchronous and limited to streaming/capacity demand, never a same-frame readback of visible geometry/counts to rebuild CPU lists. Resource dependencies remain complete even for address-loaded data.

Why third: removes recurring microgeometry RT gather/hash/task assembly and CPU-generated instance payloads; actual TLAS CPU residual gets measured separately from GPU AS building.

## Step4 -> Fan out all ready independent CPU work [independent]

Files: existing render_forward_clustered render_scene/build_tlas integration and graph pass scheduling only where required.

Owner explicitly requires fan-out, not a chain of individual worker tasks. Model dependencies between the remaining frame jobs using the existing worker/graph machinery. Enqueue every ready independent job before waiting for any of its peers; completion unlocks its actual consumers. No frame-wide/stage-wide join where only one branch is required. Use substantial batches rather than one task per object or command.

After one publication of changed scene data, raster and RT preparation consume the same immutable version independently. Apply the same rule to independent remaining material/light/decal preparation and command recording, subject to their actual resource and thread contracts. Do not move guarded RD calls or Node/script callbacks to arbitrary workers. Render owner dispatches, handles required owner-only resource operations and submits; it must not perform a serial tour of immediate enqueue-and-wait pairs. Join at the actual consumer, not immediately after each scheduled task. Never make RT wait for camera-visible raster lists. Reuse existing worker/graph machinery and preserve script callback ownership. GPU visibility/selection may still depend on depth/HZB; trace waits for required AS completion. Keep these real edges. Do not equate CPU job overlap with GPU queue overlap.

GPU async compute is optional follow-up only if a measured remaining independent workload benefits. It does not block or substitute for removal of the CPU loops.

## Runtime checks and completion

Start by launching the current combined build once and obtaining a baseline on the existing dense scene. For each coherent replacement, compile the affected Windows target, run the same game with the same scene, resolution, quality, camera route and profiler state; warmup/import time is excluded. Primary speed comparison has detailed profiling disabled. Use the existing CPU/GPU/frame HUD and FPS reporting; short sampled counters only when needed to locate remaining work. Repeat only an ambiguous/noisy comparison or after a meaningful fix, not a mandatory evidence matrix.

Report before/after total frame time/FPS, renderer CPU time and GPU time separately, with main active versus wait where relevant. No threshold based only on worker counts, deleted loops or faster isolated counters. A real net improvement without reducing geometry/lighting quality is required; if absent, identify the remaining dominant cost before continuing.

A short manual moving-object and stream/unload exercise checks that change-only publication stays live; preserve normal exit and the existing fallback in an ordinary mixed scene. No automated tests, screenshot/appearance campaign, manufactured fixtures, proof bundles or audit agents. Ordinary build plus relevant template/precision compilation when touched contracts require it, not repeated unchanged matrices. One short scoped code review at most. State remaining known issues plainly.

Internal C++/GPU layouts may change together; keep Slang layouts/defines and existing SCsub input ownership synchronized. No importer format, public ProjectSettings, ClassDB/XML or shader-facing user API change is planned. If implementation touches one, include its concrete consumer closure; do not add new settings to choose old/new behavior. Preserve public upstream API contracts and document unsupported backend limits without building an unrelated portability project.

Proposed implementation order: changed-data ingestion -> GPU raster/shadows -> GPU RT inputs -> remaining real dependency scheduling -> final game speed comparison. Implementation is authorized by the owner approval above.

Brief plan sanity review: PASS, limited to scope/dependencies and supplied source facts. No additional audit, build or test was requested or performed.
