# GPU-driven microgeometry — implementation plan

Source baseline: `43c02e86622aa45e7fce2b2a53a1b180f2035f3c`, 2026-09-09.
Owner authorization: “iimplementuj”, confirmed by “rawcontent/xyzrgb_dragon.glb
… możesz zaczynać”. This records the concrete execution of the discussed
[research](../research/2026-09-08-2005-gpu-microgeometry-research-summary.md).
Implementation authorization is present; fresh plan review precedes code edits.

## Scope and choices

Import nondeforming meshes into automatically simplified meshlet DAGs; render
eligible opaque/alpha-tested geometry using persistent GPU scene data, GPU
selection/culling and indirect work; choose an independently simplified RT DAG
cut for shadows/DDGI; stream geometry pages and show actual selected clusters.
Rigid transforms and MultiMesh instancing are included. Existing skinning,
blend shapes, shader deformation, procedural/mutable meshes and sorted alpha
blending keep working through their appropriate existing paths.

No foliage, voxels, ECS/world partition, texture streaming, new reflections,
mesh-shader requirement, software rasterizer or visibility buffer. Do not add
placeholder providers. Instance identity, immutable geometry data, selection
and raster/RT consumption remain separate enough for later representations.

Use meshoptimizer, not METIS: `meshopt_buildMeshletsSpatial`,
`meshopt_partitionClusters`, attribute-aware simplification and the `clodBuild`
algorithm in `demo/clusterlod.h` pinned to
`0870c3881655df9b7d22faa35c825393534416bc`. Preserve its license/provenance using
the dependency import mechanism. Do not implement another partitioner or DAG
simplifier. Start with 128 vertices/128 triangles per cluster and the builder's
grouping defaults; record actual import time/size rather than claiming optimum.
Update the whole dependency src/ and license plus demo header, not the header
alone: local 1.1.1 pin `b22872835dbabc56a6e4a366ea9917f62b7daf1a` lacks
`meshopt_SimplifyErrorClamped` and `meshopt_SimplifyPreserveFolds` used by this
builder. Add `misc/scripts/update_meshoptimizer.py` following the existing
RTXGI dependency import/provenance pattern, verifying the old import before
replacement. Disable permissive/sloppy fallbacks initially; do not introduce
an independent simplifier to bypass the dependency update.

Keep material surfaces and attribute discontinuities protected. Parent clusters
refer to valid selected-level triangle indices/attributes, not the old original
triangle offset remap. Multiple terminal groups are valid. Imported immutable
assets use one derived format, replacing CLUS v1 without a compatibility reader.
Upstream Mesh/ArrayMesh/ImporterMesh array and LOD APIs remain available.

RT settings: `Environment` geometry error in output-image pixels (initial 4.0,
range 0.1–64) and offscreen error multiplier (initial 2.0, range 1–16). Raster
error initially 1.0 output pixel. These are adjustable starting values, not
measured optima. Apply the offscreen multiplier per instance outside the camera
frustum, never remove that instance from RT for this reason. No light/probe/
receiver influence algorithm, adaptive quality solver or large compensating
ray bias. Aggressive settings knowingly trade shadow/GI accuracy for detail.
Potentially emissive surfaces retain finest DAG leaves and stable light triangle
identity initially; material changes refresh eligibility before use.

## Shared contracts

- `MicroGeometry` native Resource with `.mgdata` loader/saver: fixed header,
  format/build/content identity, surface/material mapping, attribute layouts,
  cluster/group DAG, traversal bounds/error, terminal group set, page directory,
  and independently encoded payload pages in the same file. No GPU addresses or
  serialized hardware AS. Loader reads metadata without reading all pages.
- `ArrayMesh` owns a storage-only `Ref<MicroGeometry>`. Its external-resource
  dependency reaches the complete `.mgdata` file; page dependencies are internal
  offsets. Saving copies/re-emits source pages, never reads the full GPU mesh.
  The resource wraps immutable `Ref<MicroGeometryData>` declared in
  `servers/rendering/micro_geometry_data.{h,cpp}`. An internal native
  `mesh_set_micro_geometry(RID, const Ref<MicroGeometryData> &)` forwards this
  strong reference through the threaded server to MeshStorage, with sibling
  closure. No scene includes in shared descriptor, new public runtime bake API,
  Node access on worker threads or extra standalone GPU RID is required.
  The descriptor holds source path/content identity and CPU metadata; pages use
  logical IDs and 64-bit file ranges, decoded lengths and integrity digests.
- Preserve source array API behavior independently of selected GPU pages. An
  eligible static mesh must not also retain unnecessary full raster GPU buffers.
  On vertex/index/attribute mutation invalidate the derived generation before
  transitioning to the existing mutable path; rigid transform changes do not
  rebuild asset geometry.
- GPU stable generation-checked asset/instance/material/cluster/page identities;
  compact visible-list indices are not identities. Object-local positions and
  current/previous transforms preserve raster double split and per-view RT origin.
- Shared bounded page pool and terminal groups; per-view raster and RT cuts;
  aggregate requests across viewports. Publish refinement only when its complete
  required group and CLAS are ready. Saturation uses a legal resident coarse cut,
  records pressure and requests missing pages. Count roots, attributes, AS,
  scratch and in-flight allocations in reported budgets.
- Render graph tracks address-loaded buffers and indirect counts. Unpublish and
  retire references before reuse using actual completed GPU submissions. Content
  generations distinguish cut changes from allocation moves and drive existing
  history invalidation paths without resetting for a physical relocation alone.

## Step 1 — Derived resource, builder, import and export [independent]

Implement `scene/resources/3d/micro_geometry.{h,cpp}` and native format
loader/saver, registration in `scene/register_scene_types.cpp`, scene SCons and
`doc/classes/MicroGeometry.xml`. Extend `scene/resources/mesh.{h,cpp}` storage
property and ArrayMesh XML, `rendering_server_types.h`/the narrow storage bridge.
Implement builder integration in `modules/meshoptimizer/` and its recorded
dependency import/SCsub; use a callback from the scene importer layer to avoid
a module dependency cycle. Define error-returning construction with atomic
publication and contextual import errors.

Replace `ImporterMesh::generate_clusters()` and its CLUS v1 serialization in
`scene/resources/3d/importer_mesh.{h,cpp}`. Run after final remapping/plugins;
include scene/glTF and standalone OBJ importers under `editor/import/3d/`.
Generate `.mgdata`, register generated files, bump affected importer format
versions, and preserve external `save_to_file`/resource takeover behavior.
Close custom resource export in `editor/export/editor_export_platform.cpp`:
customization must preserve the native format and resulting dependencies rather
than forcibly producing `.res`. Add only native loader/saver dependencies needed
by this format. Internal pages need no separate raw payload exporter.

Why first: freezes asset and selected-triangle identity used by all runtime
consumers. Remove old fork blob writer/reader together. Later steps may complete
temporarily broken runtime consumers; do not add a transitional format rail.
Compile affected code and editor; inspect actual dragon import once this step's
minimal importer/resource closure is usable. No new automated unit suite.

## Step 2 — Storage, persistent scene and page lifecycle [independent]

In `storage_rd/mesh_storage.{h,cpp}` replace full static CLUS upload/readback
with immutable metadata and resident page ownership. Add focused runtime storage
files under `servers/rendering/renderer_rd/storage_rd/` if needed, registered by
its SCsub. CPU workers read/decode file pages; renderer thread owns uploads,
descriptors and publication. The mesh/resource release and mutation sites close
old generations and cancel stale requests without overwriting active assets.

Extend existing scene registration in `renderer_scene_cull.{h,cpp}` and existing
RT geometry/material/BindlessBlock authority. Maintain persistent instance,
surface/material and current/previous transform records with dirty updates.
CPU scene/editor APIs remain ingestion, not per-frame static draw selection.
Rigid MultiMesh records retain instance custom/color/previous transform data.
Keep viewport selection/origin/history distinct from shared asset storage.

Why now: both raster and AS work must consume the same stable resident data.
No competing static scene registry and no Node per cluster.

## Step 3 — RD indirect counts and GPU-produced AS work [independent]

Extend `servers/rendering/rendering_device.{h,cpp}` and
`rendering_device_graph.{h,cpp}` to route draw-count buffers into existing
driver `command_render_draw[_indexed]_indirect_count`. Preserve existing bound
draw API; any additional public method gets ClassDB/XML closure. Track count,
command and address-loaded geometry buffers through graph hazards.

Replace once-only `blas_build_from_clusters` with batched GPU descriptor/count
and destination support, and allow TLAS input generated on GPU. Extend
`rendering_device_driver.h`, Vulkan implementation and graph resource tracking;
do not merely remove `cluster_built` guard. Track CLAS storage, BLAS outputs,
TLAS instance inputs, scratch and write→AS→trace dependencies. Query actual
device limits and alignments. Shared interface siblings must compile; do not
implement unsupported D3D12/Metal RT or add legacy rendering fallbacks.

Why now: selection cannot be GPU-driven if CPU reads its cluster count/list
back to assemble each frame. Static asset CLAS is built at residency time;
unchanged instance cuts reuse their BLAS rather than rebuild needlessly.

## Step 4 — GPU selection and raster surface rendering [independent]

Add Slang compute traversal/compaction shaders under renderer RD shader ownership
and register compilation/dependencies through existing Slang/SCons tooling.
Use bounded multi-dispatch traversal, group-consistent DAG cuts, frustum culling,
per-view depth pyramid and conservative temporal HZB with current-depth recovery
for rejected work, camera cuts and near-plane cases. Overflow cannot publish
partial replacement groups. Do not use off-spec persistent work queues.

Replace eligible CPU `_fill_render_list`/`_fill_instance_data` and per-surface
draw submission in `forward_clustered/render_forward_clustered.{h,cpp}` with
GPU-selected cluster work binned by compatible material/pipeline. CPU binds bins;
GPU supplies commands/counts and geometry through indexed vertex pulling.
Preserve native Slang material evaluation, instance uniforms, material overrides,
coverage, six surface attachments, depth and motion consumed by RTXDI/DDGI/NRD/RR.
Preserve sorted blending and actual deformation routing. Wire a selected-cluster
color output here so traversal is inspectable before final UI work.

Why now: proves the asset→GPU cut→visible surface path using real materials;
CPU draws per meshlet are not an acceptable endpoint.

## Step 5 — Selected RT geometry and simple controls [independent]

Replace `RenderRaytracing::_populate_cluster_blas`, `process_surface` static
whole-surface assembly and rigid `_build_merged_mm_blas` replication with shared
resident CLAS and selected per-instance BLAS/TLAS work. Preserve real deformed,
procedural and alpha handling. Use the same traversal data with independent RT
error/multiplier; retain terminal coverage of the resident scenario offscreen.

Replace original-triangle cluster remap in
`shaders/raytracing/geometry_decode_inc.slang` and native hit payload/SBT mapping.
All coverage, light sampling, RTXDI, DDGI and existing PT consumers decode the
selected triangle consistently. Emissive leaves retain immutable sampling IDs,
matching native emitter confirmation/PDF; update material eligibility on reload.
Handle cut/content history generations and mesh removal using current viewport
history mechanisms; do not infer content stability from unchanged BLAS RID.

Register Environment RT error/multiplier accessors/defaults/ranges through
`scene/resources/environment.{h,cpp}`, RenderingServer/scene settings and XML.
Changing them affects the next selection and required history refresh, not
offline rebuilding. Preserve shader/material hot reload and current RR fixes.

## Step 6 — Debug UI and owner asset validation [independent]

Extend Viewport/RenderingServer debug enums, bindings/XML and 3D editor menu in
`editor/scene/3d/node_3d_editor_viewport.{h,cpp}`. Provide actual selected meshlet
colors for camera raster and RT representation, plus frozen selection inspection.
RT debug rays view selected RT triangles; no reflection feature is introduced.
Show selected triangle/cluster counts and page/AS memory and build counters.
Existing light-cluster debug is not the microgeometry view.

Use `rawcontent/xyzrgb_dragon.glb` (owner-provided, 202136944 bytes, one primitive,
3609600 vertices, 7219045 indexed triangles, POSITION/NORMAL/TEXCOORD_0, no
skin/animation) through the actual Godot importer. Keep the input unchanged.
Add a minimal inspectable project/scene beside it or use an isolated demo folder;
stage only owned fixture files, not pre-existing untracked inputs/caches.

Record actual import/reimport time, hierarchy/terminal counts and payload size;
verify OBJ, native resource save/load, selected-scene/customized export and PCK
page reads. Build ordinary/double editor and template; launch the source-matched
Vulkan renderer. Inspect moving camera/rigid objects/MultiMesh, seams/materials,
offscreen shadows/DDGI, emissions, retained deformation, two viewports, constrained
pool, resource reload/unload and real RT/raster debug cuts. Compare simple RT
settings on the same view and report quality/cost rather than promising a ratio.
No automated tests beyond any specifically owner-requested import check.

## Execution gates

Every implementation step has a scoped commit then fresh exact/cumulative
hostile review under repository policy. Actual executable fixture/proof changes
also get proof-auditor. Main coordinates one shared checkout, no worktrees/stash.
Do not overwrite unrelated dirty scenes/docs. Intermediate incompleteness is
recorded; final success requires the full path, not just the dragon import.
Update implementation status with actual commits, checks and unresolved issues.
