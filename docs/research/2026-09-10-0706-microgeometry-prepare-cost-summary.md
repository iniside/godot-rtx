# Remaining microgeometry preparation costs

Checked 2026-09-10 UTC at frozen `030fa486b560436a86ad622973407f3b08520841`.
This source map answers the owner's request for counters that identify remaining
CPU work. It is not a new architecture plan or an optimization result. Step 5
source and bounded proof are complete; Step 6 recording is implemented separately.

The historical batched01 profile reports last-ten window medians of 6.134 ms
for Microgeometry RT Prepare and 5.350 ms for Microgeometry Raster Prepare.
These are elapsed intervals with the host-interference limits in the
[performance status](2026-09-09-1456-microgeometry-performance-status.md).
The orbit has 10000 instances, two assets and two to three cuts/BLAS, not a
strict two-cut workload. Eight pages, 108 CLAS and zero new page builds settle.

## Actual boundaries

`render_raytracing.cpp:4489-4494` encloses `_prepare_micro_geometry` only.
Its retained-resource path contains two immediately joined single-worker jobs:

- `:1860-1889`, MicrogeometryInputSignature: hash all selection tasks, relevant
  persistent instance/surface fields and all RT tasks with local motion_base
  cleared. `_rt_scene_hash` at `:49` applies Murmur twice per input.
- `:1976-2053`, MicrogeometryInputDependencies: rewrite every task's motion_base,
  build camera/settings parameters, hash full persistent instance/surface data,
  asset descriptors and buffer RIDs, and reinsert retained frame dependencies.

There is no RD list recording or upload in that retained-resource branch.
Resource acquisition/allocation follows a signature miss at `:1890-1972`.
Cut epochs, publication, dispatches and AS work occur later in
`_build_micro_geometry` (`:2057+`). Stable pages do not imply unchanged selection.

Raster preparation is in `render_forward_clustered.cpp`, not
`micro_geometry_selection.cpp`:

- `:428-512`, discovery: walk camera surfaces, eligibility checks, per-surface
  mesh-owner lookup, persistent-handle deduplication and temporary source lists.
  Eligibility at `:443-446` calls `MeshStorage::mesh_get_micro_geometry_asset`
  (`mesh_storage.cpp:1153`) before asset-source deduplication. Its mesh owner is
  thread-safe (`mesh_storage.h:199`). This proves repeated lookup, not lock cost.
- `:514-620`, chunk tasks: rebuild tasks/local bins and 19 uint64 snapshot words
  per accepted surface. Ten thousand surfaces produce 40 chunks and about
  1.52 MB of snapshot words before merge.
- `:622-730`, merge and parameter jobs: merge/remap bins and tasks, append
  snapshots, deduplicate material usage, prepare camera/dependency parameters
  and compare the full snapshot at `:723`. Owner material-use marking remains.

The first internal timestamp at `:743`, Microgeometry Raster Dependencies, ends
the quoted raster interval before reuse at `:746`. Reuse/return and temporary
destruction are in the following interval, historically about 0.26 ms. A lone
Raster Allocate sample is startup evidence.

## Phase counters

Commit `bbf18999d02a534d73aa09b512bffe0fbabca85d` extends sampled RenderPrep
rows around these six phases with queued,
worker begin/end, owner wait entry, join, worker/coordinator identity and actual
work counts. Separate RT instance hashing from asset/dependency processing;
separate raster snapshot comparison from parameter work. Record candidate and
eligible surfaces, mesh lookups, task/bin/snapshot sizes and existing reuse
decisions. Resource-miss allocation/upload belongs in its own owner interval.
No per-instance output, second profiler, public setting or automated test is added.
Fresh exact and cumulative source review passes. The combined ordinary editor
build passes; a successful ordinary native capture remains pending.
`signature_records_usec` includes fixed settings hashing. Eligibility samples
use stride 64 and snapshot samples use the first item in each 256-item chunk;
`source_lookups` counts source-cache misses. Raw phase endpoints precede print
formatting; enclosing coarse timestamps include its overhead.

Existing input/selection/dependency/transform signatures in
`render_raytracing.h:542-547`, raster snapshot_key, persistent_surfaces_dirty and
persistent_scene_generation are useful observables. The generation advances on
material publication, actual changed uploads and release (`render_raytracing.cpp`
`:5340`, `:5604-5615`, `:5643`). It is not a proven replacement for the hashes:
camera/settings, residency, membership/eligibility and dependencies are distinct.

Existing GodotProfileZone names do not establish native RenderPrep measurements;
none of these six phase names appears in the retained native log, and the tracing
macros may compile to no-ops. RasterPassLists queued-to-join includes intentional
overlap with RT preparation: a 29-us payload inside 20.661 ms is not a measured
20-ms scheduler wait. Frame Device End/Execute counters belong to later graph
compilation, recording and submission, not these two preparation intervals.

Navigation: root compiledb clang-nav, actual frozen declarations/implementations,
scoped history and bounded lower-bound log inventories. Read-only source check;
no build, launch, test or source edit accompanies this map. The subsequent counter commit is identified above. Native measurements are
required before selecting a behavior change. SceneTree, DDGI and paging architecture remain out
of this investigation.


## Bounded reuse candidate

A follow-up read-only check of the same frozen source identifies only the full
persistent-record traversal at RT `:2010-2018` as a candidate for generation
guarding. Cache validity would require the current retained build/task identity
and the persistent generation published after owner updates. The current hash
is camera-seeded, so its previous result cannot be reused directly: a separate
record component would be composed with fresh camera/residency inputs. No such
optimization is implemented or measured.

Normal lifecycle writers are `update_persistent_instances` (`:5431-5615`),
`release_persistent_instance` (`:5620-5643`), and conservative material generation
updates (`:5323-5340`). Payload workers join before publication. The release
error path can clear surfaces before an invalid instance handle returns at
`:5632`; any generation guard must close that invalidation hole.

Structural task/settings signatures, motion-base rewrites, camera/settings,
residency and frame-buffer dependencies must remain live. Cache only the
record-derived conservative flag; current time/previous-time/GPU-instance flags
remain separately authoritative. Broad scene generation may invalidate on every
moving frame, so native hit-rate evidence is required. This is not a proposal to
cache all preparation, alter SceneTree, or suppress CLAS/selection updates.
