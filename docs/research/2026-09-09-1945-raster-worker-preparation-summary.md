# Scene cull and raster preparation ownership

Read-only Step 5 handoff, 2026-09-09 UTC. Inspected at `5d1b591c96`;
concurrent Step 4 frame-input edits were excluded. This supplements the
[RT](2026-09-09-1919-rt-worker-preparation-summary.md) and
[canvas](2026-09-09-1919-canvas-worker-preparation-summary.md) source handoffs.
The approved repair plan remains authoritative; no worker implementation or
runtime gain is established here.

`renderer_scene_cull.cpp:3331,3437` already dispatches visibility and scene-cull
groups. Visibility bins retain parent-before-child dependency ordering.
Small workloads and result merging still execute inline. Per-task cull outputs
exist, but cull writes visibility hysteresis, timeout/fade, notifier state,
particle requests, pairing/capture changes and RT visibility. Jobs need local
change intents with owning publication before later readers. Existing
`PagedArray::merge_unordered()` does not preserve scenario order, even when
job slots are visited in order; retain input ordinals for deterministic output.

The current camera path is `PASS_MODE_RTXDI_SURFACE`, with serial list fill,
sort and instance filling at `forward_clustered/render_forward_clustered.cpp:2354`.
Retained enum/list names do not establish active generic opaque/alpha/motion
paths. Ordinary shadows remain active through `_pre_opaque_render():1978`
and `_render_shadow_append():2937`. Camera and shadow CPU preparation may
overlap after their shared storage inputs are frozen; cube atlas scratch and
render order must retain existing dependencies.

Pass-local elements should retain surface identity and resource references but
own sort keys, depth, LOD, flags and GI offsets. Copying whole surface-cache
objects also copies ownership/list links and is not the required boundary.
`_fill_render_list():1456-1654` currently writes instance depth and shared
GI/surface sort caches used by multiple passes. `_setup_environment():1163`
mixes pass dimensions and CPU UBO preparation with shared indices/RD uploads.
`_fill_instance_data():1279` maps/grows/flushes GPU buffers and writes singleton
state. Prepare host payloads on workers, then publish GPU storage on its owner.

Repeated-element run lengths depend on adjacent elements after complete sort
(`:1358-1389`); arbitrary worker chunks cannot restart those runs independently.
Secondary shadow-buffer growth can recopy all earlier elements (`:1287-1292`),
whose shared surface fields may already describe a later pass. Finalize all
per-pass host payloads and total offsets before mapping/publishing. Shadow
uniform sets are intentionally created after final buffer publication (`:3014`).

Material `set_as_used()` writes a non-atomic render-target boolean
(`storage_rd/material_storage.cpp:1288`). Parallel raster/microgeometry jobs
must emit usage intents for one deduplicated publication rather than call this
mutator concurrently. Equal writes still race.

Positional shadow gathering uses singleton scratch and regular-light planes
(`renderer_scene_cull.cpp:2362`, `rendering_light_culler.cpp:143`). One exclusive
positional-gather worker can retain these owners while later camera/shadow list
jobs overlap. Parallel per-light gathering additionally requires per-light
planes/results. Preserve atlas admission, shadow limits and dirty/version order.

Before overlapping camera, RT and shadow readers, finish cull-generated changes,
deduplicate deformable instance updates and publish common dirty mesh/geometry
state. `mesh_instance_check_for_update()` mutates intrusive lists and
`update_mesh_instances()` uploads/dispatches/swaps resources
(`storage_rd/mesh_storage.cpp:1443,1474`). Dirty geometry updates also resolve
descriptors and pipelines (`render_forward_clustered.cpp:4846`). These mixed
helpers cannot become independent arbitrary worker leaves.

Microgeometry raster CPU work at `_prepare_micro_geometry():446` can prepare
eligible candidates, bins, keys and task parameters from resolved immutable
records. Preserve camera enumeration over `geometry_surface_compilation_all_list`
(`:554`), distinct from shadow input lists (`:562`); replacing it with the
camera-visible list would alter GPU culling. Resolve GPU addresses first,
remap local bins deterministically, and retain existing selection, pin and
camera-retained-cut ownership outside those jobs.

The dependency sequence is ordered visibility bins, range cull, cull-intent merge
and positional gathering, common storage publication/freeze, parallel camera/
shadow/RT preparation, deterministic sort/payload finalization, GPU publication,
then ordered recording. Small work remains worker-owned rather than falling
back onto the coordinator. Use the existing pool without nested blocking waits
or a collaborative join that executes preparation on the coordinator.

Retain scene/pass inputs through CPU completion: current render wrappers use
stack-local `RenderSceneDataRD`, and geometry free immediately releases surface
allocations. A RID alone does not retain CPU pointers. GPU retirement remains
separate. Lazy vertex-array/pipeline creation in `_render_list_template():954`
and index-buffer creation in `mesh_storage.h:625` still require Step 6 closure.

Methods: independent Astra explorer, clang-nav find/refs/def with the root
compilation database, actual implementation/declaration reads, scoped history
and bounded text fallback. No files, builds, tests or launches changed during
inspection. Only one game viewport and ordinary internal shadows were studied;
no XR, VR, multiview, split-screen or SceneTree scalability work is implied.
