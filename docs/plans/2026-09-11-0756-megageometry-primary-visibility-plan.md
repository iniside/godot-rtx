# Mega Geometry: raster, primary RT and hybrid visibility

Approved by the owner on 2026-09-11 ("to zaczynaj"). Source baseline
`8d15b00863`, rendering source through `6e916a19c8`. Implementation and the
bounded build/runtime comparison below are authorized. Fresh-context dispatch
succeeded on retry; the brief plan review passes at `3724291fba`.

## Context and decision

The owner requires Mega Geometry as the geometry foundation and allows NVIDIA
extensions. Compare optimized raster visibility, primary visibility through
RT/CLAS, and hybrid execution in either order. Choose from measured total frame
cost and preserved behavior. Do not assume either mesh shaders or ray tracing
wins because the API is available.

Keep the existing import/DAG, streaming, instancing, CLAS, material authority,
RTXDI, DDGI and denoising. Static DAG geometry remains the scope; preserve
existing ordinary/deformed meshes. No foliage, voxels, XR, VR, multiview, split
screen, new scene architecture, automatic quality degradation, new job system,
full path-tracer rewrite or wholesale NVRHI/RTXMG embedding. No automated tests,
proof auditors or long review loops. One brief source review and actual editor
measurements are the implementation validation.

The comparison changes **primary opaque/cutout visibility**, not lighting.
“Raster-only” here means raster primary visibility; RTXDI/DDGI still use RT.
“RT-only” means primary rays for scene surfaces, not enabling the existing full
path-traced lighting mode. Editor overlays and existing separately composed
transparent effects retain their own place in the frame.

## Known baseline

Native `demos/rtxdi_manual/microgeometry_stress/scene.escn`, 10000 instances,
saved camera, visible axes, 1 px, guide/HZB resolution 2689x1602. Last valid
paired measurements: GPU 21.1695 -> 19.1815 ms, camera raster 11.4683 -> 11.2305
ms, shadow raster 3.4178 -> 1.0920 ms, unprofiled 46 -> 54 FPS. Camera submits
785617 clusters / 67357876 triangles; HZB rejects 140481 clusters. Unique
cluster vertices total 49830642, versus 202073628 triangle corners. These are
geometry counters, not hardware invocation statistics.

10000 instances share 62 RT cuts/BLAS, with 61333 resident CLAS, 3724 pages and
zero pending pages after convergence. These existing cheaper RT cuts are not
automatically suitable for primary-camera quality. Both earlier empty-mesh
captures are invalid comparisons; the final capture fixes descriptor visibility
and legal simplified-cluster primitive identities.

## Reference evidence and overlap

Pinned references, checked 2026-09-11:

- NVIDIA `vk_lod_clusters` `22301a0cb682eec543dd851a6502c26ffdf436f5`:
  `shaders/render_raster_clusters.mesh.glsl:334-372` performs triangle tests and
  subgroup compaction; `shaders/culling.glsl:225-241` tests facing and pixel
  footprint. Lines 39-41 disable that sample implementation on EXT. This does
  not mean EXT cannot cull primitives.
- NVIDIA `RTXMG` `9f7644a854776ed896900f011367df420c6fb389`,
  `docs/ClusterLOD.md`: GPU selection, streaming and CLAS/BLAS reuse; its primary
  renderer is RT-only and derives from vk_lod_clusters. It is an algorithm and
  implementation reference, not a ready Godot renderer component.
- Pinned Slang `2026.13.1`, `source/slang/slang-emit-spirv.cpp:6931`, directly
  maps its Mesh stage to MeshEXT. Our `modules/slang/shader_compile.cpp:134`
  requests direct SPIR-V. A switch to the NV draw command cannot change this
  execution model. Do not introduce a second GLSL material backend or an
  unplanned compiler fork just to label the path NV.
- EXT supports primitive culling (`CullPrimitiveEXT`, Slang `SV_CullPrimitive`)
  and compact output. The first in-engine raster candidate adapts NVIDIA's
  algorithm to the existing Slang path. NV remains allowed; supporting its
  distinct compiler output is a separately identified cost, not assumed done.

Local authorities:

| Existing authority | Use in this plan |
| --- | --- |
| `scene_forward_clustered.slang:1059`, `evaluate_vertex` | Existing material/vertex evaluation and mesh output; add primitive visibility here. |
| `MicroGeometrySelection`, `micro_geometry_select.slang` | Keep the DAG selection, recovery and streaming owners. |
| `RenderForwardClustered::render_scene` and `_render_micro_geometry` | Select and order primary visibility producers in the current renderer. |
| `pathtracing_inc.slang:97`, `pt_write_primary`; `surface_data_inc.slang:41`, `encode_surface_buffers` | Reuse reconstruction/export for primary rays without PT bounces/lighting. |
| `RenderBufferDataForwardClustered::_ensure_rtxdi_surface`, `prepare_rtxdi_surface`, `commit_rtxdi_surface` (cpp 77/95/157) | One history and output authority for every candidate. |
| Existing `RenderRaytracing` TLAS and shared cuts | One scene acceleration owner; add primary category/quality policy here. |
| Existing WorkerThreadPool and RD graph | Fan out independent CPU preparation; retain explicit GPU dependencies. |

The TLAS is already built before camera raster (`render_forward_clustered.cpp:
2924-2968`). The current bottleneck therefore does not require a new renderer
server or a new material system to explore RT visibility.

## Common contract and candidate matrix

Every mode produces the same canonical six surface attachments and reverse-Z
depth for the same camera/jitter, then calls the same history commit and the
same RTXDI/DDGI/denoiser/composition stages. No downstream consumer guesses
which producer supplied a pixel. Current/previous transforms, stable surface
identity, material classification, emission, normal/roughness and motion must
come from the winning surface. Clear misses and newly exposed pixels explicitly.

| Candidate | Primary surface production | Required order |
| --- | --- | --- |
| R | All scene surfaces through improved existing raster | Selection -> raster -> final depth/history -> lighting |
| T | Supported scene surfaces through primary RT/CLAS | Primary-quality RT cut -> TLAS -> primary trace -> depth resolve/history -> lighting |
| H-R | Conventional geometry raster, microgeometry RT | Conventional raster -> depth-bounded micro RT -> final depth/history -> lighting |
| H-T | Microgeometry RT, conventional geometry raster | Micro RT -> depth resolve -> conventional depth-tested raster -> final history -> lighting |

Hybrid partitions are explicit and disjoint. A mesh is not rendered in both
partitions. Ray masks use internal TLAS category bits, not Godot's visibility
layers: microgeometry=1, conventional=2; primary rays use 1 or 3, existing
secondary rays continue using 0xff. Preserve the independent 32-bit instance
layer test. Apply categories at every native TLAS descriptor producer.

H-R uses conventional depth to bound ray distance and replaces a surface only
for a strictly nearer microgeometry hit. H-T initializes real depth from the
microgeometry hit before conventional drawing. Equal-depth ownership must be
consistent across the two orders; preserve ordinary depth-test semantics and
document the tie rule. Preserve sky/miss behavior and existing nonopaque pass
composition. Do not use previous-frame occlusion to permanently remove RT
geometry required by secondary rays.

Primary quality uses the existing RT group-error policy with visible error no
looser than the raster 1 px control. For the conservative comparison, disable
the extra off-screen error multiplier rather than implement a cut union.
Maintain one valid DAG cut per instance; never combine overlapping coarse and
fine triangles. Wait for both geometry and CLAS readiness. Record actual cuts,
LOD distribution and residency: equal numeric tolerance alone is not proof of
equal geometry. Account for extra BLAS/TLAS work in the RT candidate's total.

## Step 1 -> shared visibility output and measurement control [independent]

Own forward-clustered `render_forward_clustered.{h,cpp}`, `render_rtxdi.cpp`,
`render_pathtracing.cpp`, corresponding headers and current primary-surface
shader includes. Extend the existing canonical surface owner instead of adding
a competing G-buffer/history owner. Reuse the PT primary writer by extraction;
retain full PT as an existing consumer of shared reconstruction.

Extract shared reconstruction/packing into proposed
`shaders/raytracing/primary_surface_inc.slang`, used by the existing PT writer
and new primary-only raygen. Keep `surface_data_inc.slang` the encoding authority.

Close actual format differences: PT currently uses single-buffered surfaces and
RGBA16F base color while raster uses RGBA8 UNORM; the canonical raster targets
currently lack storage usage. Add only usage/capability requirements needed for
RT writes, or an explicit format-conversion stage owned by the same buffers.
Depth uses the existing float-depth-to-depth-attachment resolve pattern; do not
assume a depth attachment can be written as a storage image. Move surface commit
and history copying after the last producer/depth resolve, once per frame.

Use the existing `copy_r32f_to_depth_fb` integration near
`render_forward_clustered.cpp:3036`. Keep the downstream
`RenderRTXDI::render(const RenderRTXDISurfaceResources &, ...)` contract.

Add a temporary internal development mode selector for R/T/H-R/H-T, read once
per renderer initialization. It is an experiment control, not a serialized
Environment/ProjectSettings API or per-object authoring field. No ClassDB/XML
change is needed unless implementation reveals an actual public contract;
return to this plan before introducing one.

Timers must separate selection, primary-quality AS updates, raster geometry,
primary tracing, surface/depth resolve, hybrid merge and remaining lighting.
Counters distinguish selected triangles, emitted raster triangles, triangle
rejection reasons, primary rays/hits/misses and unsupported materials. Use
sampled instrumentation without per-vertex global atomics; FPS measurements run
with profiling disabled. This precedes alternatives so all compare the same
output/measurement contract.

## Step 2 -> NVIDIA-derived primitive culling in raster [independent]

Own `scene_forward_clustered.slang`, `scene_forward_clustered_inc.slang`,
`scene_shader_forward_clustered.{h,cpp}`, microgeometry raster parameter and
statistics declarations in `micro_geometry_selection.{h,cpp}` and their exact
C++ consumers. Keep material evaluation and fragment behavior at their current
authority. Replace unconditional triangle emission in the mesh entry.

Evaluate each unique vertex once. Share only data required for triangle tests;
adapt facing/pixel-footprint culling and subgroup compaction from the pinned
NVIDIA sample. EXT cull flags versus compact primitive output is a bounded
implementation choice resolved by local measured cost; retain the faster valid
implementation, without two permanent culling systems. Do not read EXT output
arrays as temporary shared storage. Respect SetMeshOutputCounts ordering and
device output/shared-memory limits.

Never use a naive area<1 pixel rule. Preserve triangles crossing near/clip
planes or uncertain numerical cases; use actual viewport, jitter, winding,
mirrored transforms, cull mode and sample coverage. Two-sided materials must
not get backface rejection. Ordinary vertex draws, wireframe/meshlet debug,
shadow variants and HZB recovery/freeze retain their intended behavior.
Keep legal INVALID_ID simplified primitive identities and all actual index/
payload/lifetime checks. Evaluate a smaller workgroup against the current 128
lanes only as a fixed A/B variant, not an automatic tuning subsystem. No import
or cluster-size rebuild. Measure R before advancing to T.

## Step 3 -> primary RT using current hit-material authority [independent]

Own `render_pathtracing.{h,cpp}`, `render_raytracing.{h,cpp}`,
`render_forward_clustered.{h,cpp}`, `render_rtxdi.cpp` and their native primary/
material shader includes. Add a primary-only execution entry to the existing
RT pipeline authority. Reuse its hit groups/SBT/material evaluation and primary
surface reconstruction; no PT lighting loop or duplicated material compiler.

Concrete reuse points in `render_raytracing.cpp` are
`create_material_pipeline` (4988), `create_material_uniform_set` (5127) and
`register_raytracing_buffer_dependencies` (5099). Do not route camera rays
through `trace_material_rays`' full-screen input/result buffers. Proposed internal
operation, not an existing API:

```cpp
bool RenderRaytracing::render_primary_surface(
    const RenderDataRD *, RTViewportState *, Span<RID> outputs,
    RID depth_output, RID conventional_depth,
    uint8_t category_mask, bool preserve_on_miss);
```

`outputs` contains exactly the six canonical current surfaces. A null
`conventional_depth` disables depth bounding. Pipeline/SBT state belongs to
`RTViewportState`, including hit-program revision invalidation and retirement;
surface/depth images belong to the existing render-buffer owner. Add raygen/miss
in proposed `shaders/raytracing/primary_surface.slang`; reuse
`rt_material_hit.slang`, `rt_hit_context_inc.slang` and
`material_coverage_inc.slang`. The spatial-material compiler remains unchanged.

Write the canonical current surfaces established in Step 1. Generate camera
rays consistently with the raster projection, near/far clipping and jitter,
including the supported camera types; carry previous transforms into motion.
Close texture-footprint/derivative, opacity/cutout, face-culling and material
classification differences explicitly. Do not count missing or unsupported
surfaces as speedups. Candidate T must report an unsupported scene/material
contract instead of silently drawing an incomplete scene.

Reuse near/far ray bounds and perspective/orthographic construction from
`pathtracing.slang:15-26`, with deterministic pixel centers and existing camera
jitter, without PT random subpixel sampling. Specific parity checks are RT ray
footprint versus raster derivatives, raster roughness limiting, alpha hash/mip
selection and emission packing order. Ordinary skeletal deformation has RT
buffers already; arbitrary shader vertex/POSITION deformation is a separate
unsupported contract. Full T is ineligible for such a scene; hybrids preserve
those conventional raster meshes. Do not disguise that exclusion as a fallback.

Apply the primary quality policy through the existing RT selector/shared cuts;
preserve secondary visibility and one-cut topology. Add internal category masks
at all native TLAS producers and preserve existing ray/layer semantics. Reuse
AS ownership, dependency declarations and submission retirement. Every new
per-view scratch/image/RID belongs to the current render-buffer owner and has
matching resize, early-error and deferred-release behavior. Compare T with R
using the same lighting, not with the full-PT renderer mode.

## Step 4 -> both hybrid orderings [independent]

Own the same renderer orchestration, raster classification, RT primary entry
and depth resolve files. Build H-R and H-T from Steps 1-3 with the explicit
partitions above; do not add another surface format, TLAS or scene registry.
Trace only the intended category. Depth-to-ray-distance conversion must use the
actual projection rather than linearizing reverse-Z with an assumed formula.
Preserve the winning surface's complete output, motion and history identity.

The raster helper `_render_list_with_draw_list` (cpp 1254-1275) embeds HZB
build/recovery and intermediate surface commits. Preserve R's existing two-pass
recovery; do not move its first-pass HZB past the recovery it feeds. RT-produced
microgeometry needs no camera raster selection/HZB recovery: skip that work
where there is no remaining consumer. For hybrids, prevent intermediate commits
from publishing incomplete surfaces; any retained next-frame HZB consumes final
merged depth/classification and keeps the existing deformed-pixel veto.
Invalidate temporal history on mode/partition changes.
H-R may shorten primary rays using current conventional depth; H-T may
reject conventional fragments using current microgeometry depth. Neither
ordering removes secondary-ray geometry solely because it is camera-occluded.
Any extra pass/transfer/AS update is measured as part of the candidate.

## Step 5 -> native comparison and production decision [independent]

Freeze each complete candidate and use an ordinary Windows Vulkan build. Run
the existing native stress scene at the same camera, resolution, 1 px and
lighting settings, retaining the baseline binary. Also use a short camera
movement/reveal run and a bounded mixed scene in `rtxdi_manual` with overlapping
microgeometry and existing conventional/deformed material examples. Preserve
scene assets; no 10000-Node reconstruction. Check actual visible geometry and
depth/material/motion agreement before accepting performance numbers.

Measure settled stationary frame time, movement/streaming behavior and CPU
active work separately from waits. Compare total GPU, visibility and AS/merge
cost, memory, input/emitted geometry and unprofiled FPS. Recheck a close result
with a reversed A/B pair; do not launch a benchmark suite or chase noise with
unbounded repetitions. Restore editor settings after captures. A camera-only
win that moves equal or larger work into AS building/resolve is not a frame win.

Select the fastest valid candidate for the actual workload and record the
tradeoff. Keep only selected production paths; remove abandoned experimental
branches, controls and scratch allocations in the same selection change.
If one method wins static and another streaming, report that explicitly before
inventing automatic per-frame routing. The owner's roughly 9 ms target remains
an objective, not a promised consequence of this plan.

## Scheduling, integration and completion

Steps 1 -> 2 -> 3 -> 4 -> 5 are dependency ordered. Their source ownership
overlaps, so writers are sequential. Within each step, fan out independent CPU
preparation on the existing WorkerThreadPool; do not put rendering work on the
main thread or create a serial chain disguised as worker jobs. Keep RD calls
on their owning thread. GPU barriers follow real producer/consumer edges;
this plan does not assume async compute is free overlap or introduce a new
multi-queue scheduler. Shadow work retains its own dependencies.

New shader sources must be owned by the existing SCsub generation/include graph;
generated files and thirdparty files are not hand edited. Preserve upstream
public APIs and ordinary backend behavior. Only supported Vulkan hardware runs
these candidates; do not add unsupported-platform compatibility renderers.
Keep Slang's current global optimization policy unchanged in this comparison.

After implementation: successful current build, real Vulkan execution with
visible geometry, comparable quality inputs, honest measurements, one brief
source review, source commits and updated project-state/status. No automated
tests or proof-auditor gates. The plan is revised if a material/compiler issue
requires an architecture beyond these boundaries rather than hiding it behind
a fallback or silently expanding the task.

## Primary sources

Review status: independent source research completed. Fresh brief plan review
passes for exact `3724291fba` and cumulative `8d15b00863..3724291fba` after the
dispatch retry succeeded. This establishes plan coherence only; material parity,
runtime synchronization and comparative performance still require implementation
validation. No new build or runtime measurement was performed for this plan.

- https://github.com/nvpro-samples/vk_lod_clusters/tree/22301a0cb682eec543dd851a6502c26ffdf436f5
- https://github.com/NVIDIA-RTX/RTXMG/blob/9f7644a854776ed896900f011367df420c6fb389/docs/ClusterLOD.md
- https://github.com/shader-slang/slang/blob/v2026.13.1/source/slang/slang-emit-spirv.cpp
- https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_mesh_shader.html
- https://github.com/shader-slang/slang/blob/v2026.13.1/docs/user-guide/a2-01-spirv-target-specific.md
