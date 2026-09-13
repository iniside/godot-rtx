# Streamed Cell Install Throughput

## Context

Optimize the existing individual-file streamed-cell path while preserving all
5,001 files in the stress cell, their text format and names, atomic cell
commit, persistent `EntityId` ownership, the worker/owner boundary, and current
renderer semantics. Do not add cluster files, packed caches, a second storage
authority, compatibility paths, settings, bindings, or automated tests.

The measured warm 10k-cell owner install is about 71 ms. The current path also
allocates one aligned block per decoded component, performs scalar Flecs table
transitions, adds transform state after installation, repeatedly hashes the
same entity metadata, and reads the thousands of cell files serially.

## Sequence

### Step 1 - Batched individual-file reads `[independent]`

Replace the whole-cell worker with an explicit nonblocking `CellJob` state
machine in `scene/resources/entity_scene.h` and
`scene/resources/entity_scene.cpp`. Enumerate and sort filenames first, then
post bounded native group tasks whose elements are fixed-size ranges of files.
Each range writes to an exclusive result slot; owner code polls completion,
joins only completed groups, and merges in filename order. Preserve duplicate
detection, cancel/flush cleanup, missing-asset resume without rereading completed
files, and nearest-cell scheduling without nested worker waits. Record
enumerate, read, parse, file, range, and lane counters. Target at least 4x lower
first-cell read/parse wall time without regressing 16-cell throughput.

### Step 2 - Columnar prepared storage `[independent]`

Replace streamed `PreparedEntity::values` allocations with groups keyed by a
canonical stable component signature. Each group owns one aligned,
schema-constructed column per component and decodes records directly into its
rows. Track constructed rows and safely destroy partially decoded or
moved-from values on every success, error, resume, and cancellation exit.
Retain raw pending records only while assets are missing. The streamed path has
zero per-component heap allocations.

### Step 3 - Final-archetype Flecs materialization and transforms `[independent]`

In `scene/entity/entity_world.*`, `scene/entity/entity_transform_system.*`, and
`scene/resources/entity_scene.*`, resolve runtime component IDs once per group
and call `ecs_bulk_init` once per archetype with `Identity`, component columns,
and the transform system's private state in the final type. Consume returned
entity IDs immediately, reserve/populate residents, and set rare parents only
after every handle exists. Exclude rows that revalidation found resident.
Finalize parent-dependent transform, visibility, and reset state in topological
order before one coalesced changed/render-dirty mark. Target at most 5 ms for
uniform 10k Flecs creation plus initial components and no post-install state
transition.

### Step 4 - One-pass metadata commit `[independent]`

Build one prevalidated commit item per row, insert new catalog records with
their final parents once, change existing parent indexes only when required,
move sections, and use the already known cell key for membership. Remove the
streamed `_assign_cell` pass and repeated `PreparedSet` lookups while retaining
one business-rule authority for scalar and batch inputs. Reserve all involved
containers once. Target at most 15 ms total warm owner install per 10k cell.

### Step 5 - Correct initial render publication `[independent]`

After transform finalization, build initial `EntityRenderUpdate` packets
linearly from live ECS/group handles in `scene/entity/entity_render_system.*`,
bypassing the dirty map and repeated optional-component lookups for newly
loaded cell entities. Never read released prepared columns. Keep the sparse
dirty path for later edits and keep individual renderer RID/Instance semantics.
Target at most 5 ms packet preparation; measure renderer apply/dirty separately.

### Step 6 - Validation

After each step build the primary Windows double editor. Run the unchanged
individual-file 16x16 stress input on Vulkan/RTX 4090 in runtime and editor,
stationary and with controlled camera motion. Exercise cancellation,
missing-asset resume, unload/reload, and clean exit. Record first/16-cell wall
time, CPU phase sums, file/range/lane counts, allocation count, bulk groups,
transform finalization, owner install, packet preparation, renderer apply/dirty,
and peak prepared memory. Verify file count, names, and checksums are unchanged.
Do not add or run automated tests.
