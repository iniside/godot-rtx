# RT preparation ownership

Read-only Step 5 research, 2026-09-09 UTC, frozen RT source `e319cca724`.
The approved performance repair plan remains authoritative. This is a source
handoff, not implemented jobs or measured worker overlap. Canvas findings are
in [the companion inspection](2026-09-09-1919-canvas-worker-preparation-summary.md).

The existing `RenderRaytracing::build_tlas()` instance walk starts at
`servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp:3284`.
It cannot run unchanged in parallel: surface/material/deformation caches,
GPU resources, descriptor allocation and output vectors all have shared writes.
The viable boundary is flat discovery batches, unique-resource resolution by
the existing owner, flat assembly batches, worker finalization, then owner
uploads and AS operations.

Discovery consumes the admitted renderer version, not live SceneTree objects.
Contiguous instance/surface ranges produce local candidates and exact resource,
material RID and signed instance-uniform-offset keys. Preserve existing stable
order, including pending ordinary MultiMesh output appended in Phase 2
(`:3638-3791`). Do not write surface cached transforms from discovery jobs.
Resolve storage once per unique resource before readers; copying a whole scene
or scheduling a job per meshlet is unnecessary.

Owner-controlled effects include `process_material()` (`:1327`, cache-use and
dependency writes even on hits), `process_surface()` (`:773`, BLAS/cache
mutation), `_populate_surface_blas()` (`:1180`, address queries and compute),
`process_deformed_surface()` (`:840`, pool mutation and GPU copies), and
`_build_merged_mm_blas()` (`:2608`, shared handles and compute). MultiMesh local
data access in `storage_rd/mesh_storage.h:866` can invoke allocation and GPU
readback through `_multimesh_make_local()` (`mesh_storage.cpp:2057`). Resolve
that span before jobs. `MicroGeometryStorage::get_primitive_lookup()` also
calls RD (`micro_geometry_storage.cpp:702`); it is not a pure metadata getter.

After resolution fixes successful/fallback output counts, prefix offsets allow
disjoint array writes without shared push/resize operations. Required output
includes geometry/material/program records, transforms, BLAS/masks/flags,
motion, emissive sources, dependencies and selection/RT task descriptors.
Adjust every index-bearing field together: selection bin, RT selection bin,
geometry/motion bases, motion indices and emissive geometry indices. A large
expanded MultiMesh needs ordinal-range batches, not one job for the whole mesh.

CPU material payload packing can use copied shader layouts, normalized values
and resolved texture indices for unique dirty materials. Descriptor allocation,
uniform pools and cache publication retain their owner. Generation queries
read uniforms, global/instance buffers and textures
(`material_storage.cpp:2519-2598`), so they must describe the same admitted
version as the packed payload.

Worker finalization can prepare dependency unions, ordered signatures,
hit-program indices (`update_material_pipeline():4269`), changed upload ranges
(`finalize_buffers():2557`) and TLAS descriptors (`:2526`), using resolved
addresses. Preserve ordered hashing; an unordered XOR or worker-count-dependent
grouping changes the contract. Owner uploads commit the completed payload and
retain same-address `acceleration_structure_needs_rebuild()` checks (`:2546`).

Jobs prepare microgeometry inputs; they do not acquire ownership of shared cuts.
`_prepare_micro_geometry():1843` and `_build_micro_geometry():2019` retain asset,
selection, page-pin, feedback and candidate/committed publication lifetimes.
Residency generations, full instance/surface inputs, frozen state, TIME,
previous TIME and conservative GPU-instance flags remain in invalidation.
Join CPU jobs before their inputs expire; GPU retirement remains submission-based.

Existing `WorkerThreadPool::add_native_group_task()` is declared in
`core/object/worker_thread_pool.h:264`. Use flat batch leaves, with no nested
group waits. Group completion (`worker_thread_pool.cpp:727`) blocks on a
semaphore. Task completion (`:403`) can execute queued work collaboratively
(`:488`), which would put preparation on a waiting coordinator. Scheduling must
leave available workers for leaves while the coordinator joins without doing
their preparation. The pool's zero-thread path executes on the caller (`:230`);
a blocked pool-based coordinator with one total worker also cannot progress.
These constraints require closure in Step 5 scheduler integration.

Methods: independent Astra explorer using clang-nav find/docsym/refs, current
declarations and implementations, scoped Git history/diffs and bounded text
fallback. `build_tlas` references returned only its definition, so no complete
caller-map claim is made. No edits, builds, tests or launches occurred during
research. Public APIs, XR/VR/multiview, quality and SceneTree scalability remain
outside this internal preparation change.
