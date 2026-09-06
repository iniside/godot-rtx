# RTXDI renderer replacement plan

Status: owner approved; implementation authorized. Date: 2026-09-06 UTC.
Baseline: `33b555c7f225afd1e586bf749f2b485c1de260df`.

Fresh hostile plan review: PASS, 2026-09-06 (`rtxdi_plan_review`, gpt-5.6-sol).
No approval-changing findings. Shader compilation and real-device behavior remain
unverified implementation gates; this verdict is not renderer validation.

## Outcome and authority

Replace the active Forward+ opaque renderer with raster visibility, RTXDI direct
lighting and NRD RELAX. The final step must build and render on a real Vulkan RT
device. Intermediate steps may fail to build or render; there is no requirement
to keep a temporary forward or PT rendering path alive. Steps are dependency and
ownership boundaries, not individually usable releases.

Owner overrides: backward compatibility and non-RT platforms are not requirements.
Remove superseded public APIs/settings without aliases or migration. Preserve
unrelated dirty work, use the current branch, and do not stage owner demo content.
The owner approved this plan with "zaczynaj" on 2026-09-06.

The target continues toward shared direct lighting for DDGI, automatic cluster
DAG, geometric near foliage and distant voxels. This plan delivers the first
RTXDI renderer, not DDGI, indirect reflections, the new geometry DAG, voxel
foliage, glass or thin-leaf transmission. Those are subsequent features, not
legacy fallbacks to retain during replacement.

## Final contract

- First supported runtime backend: Vulkan with ray queries, AS and all features
  actually required by retained bindless/CLAS code. Six compatible color targets
  are required. Fail clearly if unsupported; never downgrade Forward+ to Mobile.
- Raster primary visibility; mono-view; no MSAA. Multiple independent ordinary
  viewports are supported. Unsupported multiview/MSAA settings are rejected.
- Standard isotropic metallic/roughness PBR, geometric and shading normals,
  dielectric specular, decals, standard emission and alpha-scissor/hash coverage.
  Unsupported custom `light()`, extra lobes, blended surfaces and procedural
  emissive-light evaluation receive explicit diagnostics, not forward fallback.
  Vertex/material customization within the supported contract must produce
  consistent raster and ray-visible geometry/materials; unsupported constructs
  must be rejected rather than silently approximated.
- Analytic directional/omni/spot/area lights, standard-material emissive triangles
  and environment-map direct illumination. Preserve receiver/caster masks,
  explicit visibility and shadow flags. Baked-light suppression is removed from
  this dynamic-lighting path together with old baked-GI composition.
- One shared surface/BRDF/light-sampling/visibility authority for camera DI and
  future DDGI hits. No camera reservoir reuse is assumed for probe hits.
- No old lightmap/SDFGI/voxel-GI/probe diffuse contribution in the final DI image.
  Final light output is direct diffuse/specular plus surface emission and the
  visible sky. DDGI and reflected secondary-surface light arrive later.
- Keep volumetric fog's existing shadow-map dependency in this task. These maps
  remain only because a retained effect consumes them, not as opaque-DI fallback.
  Fog's direct-light extraction must use the same authoritative light population;
  any remaining bounded fog-light representation is explicitly separate from
  the complete RTXDI registry and must not truncate the latter.
- NRD RELAX performs DI denoising. Existing ordinary TAA/upscaling is downstream;
  PT-specific DLSS Ray Reconstruction is removed from the active path.

## Evidence and overlap

The [concrete design](../research/2026-09-06-1840-rtxdi-renderer-design-summary.md)
contains source anchors and proposed formats. The [SDK investigation](../research/2026-09-06-1823-rtxdi-integration-summary.md)
pins RTXDI sample `a6efab96` and runtime `f12037fa`; NRD is pinned at `2cd55303`
(4.17.1). Those exact full hashes in the reports are the import versions.

Existing candidates and treatment:

| Existing authority | Treatment |
| --- | --- |
| `RenderForwardClustered` | Extend and replace its active opaque topology; retain scene/material infrastructure, canvas/post integration and required fog scheduling. |
| `RenderForwardClusteredPT` | Retire selected runtime subclass, activation and PT-only controls; move useful RT ownership before removing the old path. |
| `RenderRaytracing` / `RTViewportState` | Reuse geometry/AS caches and per-viewport ownership, replace limited light packing; no second AS service. |
| `PASS_MODE_DEPTH_MATERIAL` | Baking/material-export semantics differ and skip required shading work; add an explicit surface variant rather than reuse its UV2/coverage behavior. |
| Current normal/roughness and velocity buffers | Insufficient and conditionally/late produced; replace active input production with mandatory surface export and history. |
| Raster and RT BRDF code | Consolidate supported evaluation/sampling/PDF functions; matching field names alone are insufficient. |
| SDK FullSample | Reuse algorithm/API patterns and pinned runtime; replace Donut/NVRHI callbacks with our RD/material bridge. |
| PT DLSS-RR | Guide semantics do not establish a DI denoiser contract; replace with pinned NRD RELAX integration. |

Navigation: root compiledb is absent. Evidence uses targeted declarations,
implementations, ClassDB/XML, shaders/SCons and history. Bounded searches are
lower bounds. Before each step, inspect only the relevant source/working diff
since baseline; do not restart broad research.

## Step 1 — Dependencies and shader integration [independent]

Why first: later shader and denoiser contracts depend on exact pinned APIs.

Import RTXDI runtime and NRD through reproducible dependency/import rules with
separate licenses and notices. No hand edits to generated or upstream vendored
files. Add host sources/build ownership under `servers/rendering/renderer_rd/SCsub`
and appropriate dependency metadata. Do not import Donut/NVRHI as engine layers.

Extend `glsl_builders.py::include_file_in_rd_header` with the scoped SDK include
root needed by `Rtxdi/...`; register recursive `.h/.hlsli` dependencies in
`shaders/raytracing/SCsub`. Preserve include guards and conditional semantics;
do not create a second handwritten copy of ReSTIR math. Compile the selected
GLSL DI closure when it helps detect a blocking SDK issue early. If native GLSL
requires patches, record them in the import recipe. A change to HLSL/DXC runtime
integration instead is a plan change, not an implicit fallback.

Produce pinned NRD SPIR-V and matching host code using its build recipe; RD will
load those compute shaders. No generated binaries/caches are staged outside the
repository's recorded dependency policy.

## Step 2 — RT-only ownership, initialization and old API removal [independent]

Why now: scene preparation and view resources must have one owner before new DI
passes and history are attached.

In `renderer_compositor_rd.{h,cpp}`, select `RenderForwardClustered` for the new
Forward+ path. Replace Mobile-on-low-texture and unknown-method fallbacks with
failure. Make `_create_current()` validate the actual RD feature/format/limit
contract before allocating/publishing a compositor, returning null on failure.
Keep the tool-only dummy compositor for non-rendering engine setup; it is not a
3D fallback and supplies no rendering proof.

Proposed C++ contract: change `RenderingServer::init()` and
`RenderingServerDefault::init()` to return `Error`. `_init()` records a private
initialization result; the existing `command_queue.push_and_sync` establishes
completion before `init()` reads that result. Validate compositor construction
before dereferencing it or publishing scene/storage pointers. On failure, roll
back partial resources on their owning thread and stop/join the pump task once.
`finish()` must distinguish initialized and failed states and never finalize
uncreated resources. Cover main-thread and threaded startup.

Update declarations/overrides and both `main/main.cpp` initialization callers
(around 821 and 3604) to handle failure and route to the existing cleanup/error
path. Main/runtime device selection must not retry Mobile/GL after the mandatory
RT failure. Audit the relevant platform/backend retry callers as part of this
same contract change; do not leave a platform-specific downgrade hole.

Move `RenderRaytracing` ownership/setup and `_free_rt_viewport_state()` into
`RenderForwardClustered`; retain `RTViewportState` teardown. Delete the runtime
PT subclass/selection and PT-only raygen/output/guide consumers once useful AS,
material and bindless support has moved. Keep no alternate old lighting authority.

Remove `Environment.pathtracing_enabled`, debug mode, samples, bounces and PT
denoiser contract across Environment, RenderingServer, RenderingMethod,
RendererSceneRender/Cull, environment storage, enums, bindings and XML. The full
file inventory is in the design report. Remove PT-only SER/async-compilation
settings if their users disappear; move retained geometry-cache knobs from
`rendering/pathtracing/*` to `rendering/raytracing/*` with their existing meanings
and defaults, updating every live getter and ProjectSettings XML without aliases.

Bound serialized/bound-symbol inventory to tracked engine/demo consumers and
explicitly owned assets before deleting names. Owner's untracked `gi_demo`
contains old PT settings/scripts and is intentionally not migrated or staged.
Use a separate owned demonstration scene in the final step.

## Step 3 — Authoritative RT scene and light registry [independent]

Why now: surface sampling requires complete current/previous lights and correct
shadow geometry, including off-screen contributors.

Replace PT activation and camera-AABB-only RT collection in
`renderer_scene_cull.cpp::_scene_cull`. Collect explicitly visible, resident
scenario geometry/lights independently of raster frustum, occlusion and manual
visibility ranges, retaining valid object/shadow/layer semantics and shadows-only
casters. The initial collection is conservative, not a claim of bounded dense-
world cost. Do not restrict all RT instances using camera receiver-layer masks
when a visible receiver's shadow mask can reference another layer.

Replace `RenderRaytracing::gather_lights`, `RT_LightData` and its 64-entry cap with
current/previous complete light snapshots. Stable keys use full instance RIDs
with generation; emissive keys also include surface/topology generation and
primitive index. Build old/new index remaps; deletion and topology changes
invalidate old references rather than reassign their meaning.

Use `storage_rd/light_storage.{h,cpp}` as the analytic data authority, including
area axes/projector fields. Register standard emissive triangles during the same
`build_tlas()` surface traversal that produces geometry/material data. Derive
world area and emission from the rendered geometry/material contract, including
texture and supported deformation changes. Zero-emission/nonuniform texture
sampling must not introduce a zero-PDF region with nonzero contribution.

Create the local/emissive/environment proposal distributions and PDF evaluation
needed by the pinned initial sampler. ReGIR is not required for this plan.
Represent no lights/background explicitly. Share sampling/BRDF/PDF semantics
through one include authority used by the RAB bridge and future probe callers.

Resources remain renderer/per-viewport owned; no Node access from render workers.
Keep snapshots/geometry alive until all submissions referencing them finish.
Reallocated buffers invalidate their descriptor sets before reuse. Preserve
dynamic meshes and MultiMesh identity, not only static triangle scenes.

## Step 4 — Raster surface replacement and history [independent]

Why now: ReSTIR requires coherent current/previous surfaces before sampling.

Add `PASS_MODE_RTXDI_SURFACE` and corresponding shader/pipeline variants in
`render_forward_clustered.{h,cpp}`, `scene_shader_forward_clustered.{h,cpp}` and
`shaders/forward_clustered/scene_forward_clustered.glsl`. Replace the active opaque
color draw and its direct/GI/fog calculations with one material/decal evaluation.
Do not define `MODE_RENDER_DEPTH`, which suppresses needed decal work. Keep actual
alpha-scissor/hash discard; alpha zero with depth writes does not make a hole.

Use the six-target surface layout in the design report: base/coverage,
normal/roughness/specular, HDR emission/metallic, motion XYZ/AO, packed geometric
normal, exact classification/layer data, plus device depth. Reject unsupported
formats, MSAA and multiview. Flags are explicit integers, never truncated sort
keys presented as stable material identity.

Make motion production unconditional and early. XY is previous-minus-current in
SDK pixel units with consistent jitter; Z is linear-depth difference for the same
surface using previous geometry/transforms. Hardware depth remains device depth.
Support perspective and orthographic reconstruction. Keep per-viewport current/
previous surface resources, matrices, frame index and history validity. Camera
cuts, resize, history gaps and invalid deformation history reject temporal reuse.

Remove the active legacy opaque direct loop and old GI composition, as well as
the forward-lit blended-surface path for this explicitly opaque milestone.
Unsupported materials produce a visible diagnostic, not silent disappearance.
Do not remove masked materials from fog's retained shadow-caster passes.

## Step 5 — ReSTIR DI passes and visibility [independent]

Why now: uses the scene registry and surface/history contracts from steps 3–4.

Add the renderer-owned DI pass implementation and shaders under
`forward_clustered/` and `shaders/raytracing/`, with `SCsub` registration. Names
for new private helpers remain implementer choices; no new Node/server API.
Implement the pinned RAB surface/light/target-PDF/visibility/remap callbacks.

Use `ReSTIRDIContext` in `TemporalAndSpatial` mode, no checkerboard initially,
with `GetReservoirBufferParameters()` and `GetBufferIndices()`. Allocate its
three reservoir arrays with SDK pitches. Execute initial sampling, temporal,
spatial and final shading with explicit RD write/read dependencies; never gather
neighbors from an array concurrently overwritten by that spatial dispatch.

Trace current-scene final visibility with ray queries and the supported material/
coverage contract. Initially disable visibility reuse. Temporal correction uses
current AS; document its transient-bias tradeoff without allocating previous AS.
Invalidate history on cuts/resize and remap deleted lights to invalid. Produce
separate diffuse/specular radiance/distance with NRD-compatible guide semantics.

Initial candidate counts and SDK quality defaults are recorded in one renderer
configuration struct. Do not add an enable flag or public tuning API solely for
bring-up. GPU debugging may inspect intermediates without creating a second
production lighting path.

## Step 6 — NRD, HDR composition and frame closure [independent]

Why now: closes the frame only after all producer contracts exist.

Add an RD-native adapter for pinned `RELAX_DIFFUSE_SPECULAR` under `effects/`,
including its `SCsub` and shader/dependency inputs. Use `CreateInstance`,
`GetInstanceDesc`, `SetCommonSettings`, `SetDenoiserSettings` and
`GetComputeDispatches`; consume the returned dispatch descriptions before the
next call overwrites them. Load matching SPIR-V, map descriptor offsets/formats,
allocate permanent/transient pools and register every access through RD.

Own NRD instances/pools per view with teardown at render-buffer destruction;
reset on the same disocclusion/cut/resize contract as DI. Use the pinned packing,
normal/roughness and material demodulation rules. Do not hand-wave final color as
a valid denoiser input or stack DLSS-RR over a mismatched guide set.

Compose denoised DI with material factors exactly once, add emission once, and
render background sky. Do not double-add environment direct illumination through
old ambient/IBL. Preserve compatible fog/post/upscaler inputs, timestamps and
depth/velocity conventions. Reflection-probe capture paths must not invoke the
removed forward lighting loop: remove their active capture scheduling when they
have no consumer in this DI-only milestone. Non-view tool material export may
remain, without serving as an alternate gameplay renderer.

Maintain fog's required shadow passes and cluster/light resources until its own
visibility replacement is planned. Free old PT/opaque-GI history and unused
descriptors when their consumers are deleted, rather than leave stale allocations.
Audit early error, renderer destruction, viewport destruction and resize for all
resource owners. Finish ClassDB/XML/settings/build sibling closure here where
steps 2–5 deliberately left intermediate interfaces incomplete.

## Step 7 — Final build, real rendering and review [independent]

Why last: this is the sole required end-to-end rendering gate. Earlier shader
compiles are optional diagnostics, not mandatory per-stage acceptance gates.

Build Windows Vulkan editor and template_debug using repository SCons options;
compile/load the actual renderer/material/NRD shaders. No headless/dummy result
can satisfy rendering completion. Include normal and separate rendering-thread
startup inspection, with unsupported-device failure checked through its real
initialization path where a suitable device is available. Mark unavailable
hardware coverage honestly rather than fabricate it.

Create one owned manual demonstration project/scene in a new, clearly scoped
directory, without copying or rewriting the owner's dirty `gi_demo` or
`rt_test_scenes` content. Show standard PBR, a masked surface, moving/deforming
geometry, directional/point/spot/area lights, textured emission, environment light
and an off-screen shadow caster. Inspect raw DI and final denoised output, camera
motion/cuts, resize, light removal/reordering, and two independent viewports.
Confirm direct lighting/emission are not doubled and that no retired forward/PT
pass runs. Inspect fog with its retained shadow dependency and post/upscale.

Record actual binary/revision/device/internal resolution, scene, screenshots,
pass timings and allocations. Report remaining artifacts and memory costs; no
invented FPS target. Render correctness, not just compilation or nonblack output,
is the completion criterion for the supported contract. Fix final integration
defects within these stages; do not hand off a compiling-but-unrendered result.

Update project-state and implementation evidence. No automated tests are
authorized or included. Comments: default NONE. Relevant failure classes are
ClassDB/serialization, RID/GPU lifetime, shader/RD synchronization, SCons/runtime
build axes and real-device evidence. Compatibility-preservation requirements are
overridden by the owner's explicit scope.

## Dispatch, commits and approval

Step 1 → 2 → 3 → 4 → 5 → 6 → 7. Each whole step uses a separate
`core-implementer` context with model/effort selected at dispatch by shared
Model Selection, and scoped commits on the current
branch. Fresh hostile review examines the exact commit and cumulative task diff.
Review checks an intermediate step against its assigned contract, not whether
the unfinished renderer runs. No compatibility scaffolding is added to make an
intermediate commit green. Final completion requires step 7's real-device result.

Commit the approved plan separately and execute its lanes without asking again
for each stage. Intermediate build/render failures are explicitly authorized.
