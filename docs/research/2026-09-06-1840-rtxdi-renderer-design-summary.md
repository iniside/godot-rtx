# Concrete RTXDI renderer integration design

Research: 2026-09-06 UTC, Godot `33b555c7f225afd1e586bf749f2b485c1de260df`.
Status: proposed engineering design, not implementation or GPU proof. No build,
test, launch, SDK vendoring or source edit was performed.

## Owner scope and replacement boundary

The owner explicitly waives backward compatibility and support for platforms
without ray tracing. These instructions override repository defaults about
preserving upstream compatibility for this task. Do not invent aliases,
migrations, a non-RT fallback, or a second direct-lighting authority.

Replace the selected Forward+ scene path with raster primary visibility and
RTXDI direct lighting. DDGI follows using the same surface/light semantics.
The automatic cluster DAG and separate geometry/voxel foliage direction remain.
This work does not require deleting unrelated 2D rendering or every unused
platform backend from the repository.

The [earlier RTXDI report](2026-09-06-1823-rtxdi-integration-summary.md) remains
source evidence, but its upstream-compatibility requirement is superseded here.

## Concrete topology

Proposed first production topology, at internal pre-upscale resolution:

1. Update scene geometry/BLAS/CLAS and the viewport TLAS; prepare current and
   previous light tables and their identity remap.
2. Rasterize opaque materials into a surface buffer and depth, without lighting.
3. Prepare local/emissive/environment sampling distributions.
4. Generate initial DI samples; temporal reuse; spatial reuse; final visibility
   and shading. Use separate dispatches, SDK buffer indices and RD dependencies.
5. Denoise the diffuse/specular signals with NRD RELAX.
6. Compose linear HDR: reconstructed direct terms plus emission, with each material
   factor applied once. DDGI later supplies diffuse indirect at this authority.
7. Apply atmosphere/fog and the existing post/upscale/tonemap chain in the proper
   domain. Do not use MinimalSample's primary ray generation or tonemapping.

The first DI milestone has no claim of completed indirect lighting. Old SDFGI,
voxel-GI, lightmap, reflection-probe and ambient contributions cannot simply be
added to the DI output: their ownership and sky contribution must be explicit.
For the bounded initial direct-light result, omit those old GI contributions;
DDGI and indirect specular are later work. This is a milestone boundary, not a
proposal to drop indirect lighting from the target renderer.

## Renderer selection and AS ownership

Existing authority: `renderer_compositor_rd.cpp:377–391` selects
`RenderForwardClusteredPT` for Forward+. It silently selects Mobile below 48
textures/stage. Replace that selection with the hybrid renderer and reject
unsupported capabilities before partially initializing scene resources. The
constructor currently returns no error result, so failure propagation must be
closed through its initialization/caller contract; leaving `scene` null before
the unconditional `scene->init()` is invalid.

Follow-up source closes the startup boundary: `RendererCompositorRD::is_viable()`
currently returns `OK` and the bounded caller search found no invocation.
`RenderingServerDefault::_init()` at cpp 248–259 dereferences the constructed
compositor and its scene immediately. Public `RenderingServer::init()` is void;
`main/main.cpp:821,3604` has no renderer-error return to inspect. Therefore a
correct implementation must add an initialization-result contract and propagate
failure back to startup, including synchronization with the rendering thread.
The selected compositor must pass capability validation before being published.
This API break is justified by mandatory RT, rather than worked around with a
partially initialized renderer. Windows display-server creation already has
`Error &r_error` RD-failure paths, but enforcing policy only there would not
cover every compositor creation path.

Require Vulkan ray-query/AS support for this first backend. Vulkan reports
`SUPPORTS_RAY_QUERY` in `rendering_device_driver_vulkan.cpp:7949–7952`;
`RenderingDevice` AS creation accepts pipeline or query at cpp 306–307, 480–486.
Keep additional capabilities actually required by retained CLAS/bindless code;
rayQuery alone is not evidence that all existing geometry paths work. Do not
introduce software traversal or a D3D12 stub fallback. Keep engine startup
fallbacks in the final affected-consumer audit: compositor selection is the
verified scene authority, not proof that platform initialization never retries
another backend.

Move `RenderRaytracing` ownership from `RenderForwardClusteredPT::_setup_rt()` /
destructor into `RenderForwardClustered` lifetime. Preserve the existing
`_free_rt_viewport_state()` hook and `RenderRaytracing::free_viewport_state()`.
The old subclass must no longer choose between PT and non-RT opaque rendering.
Do not add an Environment switch to activate required RT scene infrastructure.

`RTViewportState` owns TLAS and geometry/material/motion buffers per
`RenderSceneBuffersRD`; extend this ownership rather than create another TLAS
cache. Renderer-wide geometry caches remain shared, viewport visibility and
history remain separate. Rendering-thread work uses storage RIDs, never Nodes.
All new buffers, uniform sets, pipelines and NRD state require normal, resize,
partial-initialization and teardown release paths through RD deferred lifetime.

## Off-screen geometry and light identity

`renderer_scene_cull.cpp:3419–3426` currently enables RT from
`Environment.pathtracing_enabled`; collection at 3277–3318 uses camera far-plane
AABB and visibility-range filtering. Replace this activation and collection for
the RT-only path. A far-away caster can shadow a visible receiver, so camera
visibility is not a safe RT population predicate.

Conservative first correctness domain: resident, explicitly visible scenario
geometry from `_scene_cull()`'s existing `scenario->instance_data` iteration
(2901–2937), including off-frustum and shadows-only geometry when it casts shadows.
Keep explicit visibility/shadow/layer semantics; bypass raster occlusion and
object visibility-distance cuts for RT participation. Use this same snapshot for
light extraction. This is O(resident scene) collection, not a proven dense-world
performance solution. Later bounded traversal must conservatively cover receiver
to light segments, directional casters and active probe domains; do not replace
the existing camera AABB with a larger arbitrary radius.

Replace `gather_lights()` and the 64-light, camera-scored `RT_LightData` list.
Build the new light domain alongside RT scene preparation:

- Analytic identity: full light-instance RID, including its generation. Existing
  `LightInstance::self` and base/transform fields are in `light_storage.h:106–130`.
  Packed `forward_id` mappings at cpp 995–998 are not temporal identities.
- Support directional, omni, spot and area lights from storage. Area records
  include width/height axes and projector/atlas data (`light_storage.h:139–169`,
  cpp 917–1000). Preserve their evaluation semantics instead of approximating
  every area light as a point.
- Emissive identity: instance generation, surface identity/topology generation,
  primitive index. Build metadata during `build_tlas()` surface traversal,
  cpp 2792–2897. Geometry device addresses and primitive counts already exist
  in `RT_GeometryData`; `RT_MaterialData` has emission parameters/textures.
- Maintain two light snapshots and current-to-previous/previous-to-current
  index maps. Deleted/re-topologized emitters map to invalid; never reinterpret
  their old reservoir slot as a newly streamed triangle. Transform or intensity
  changes update data without pretending the emitter was necessarily deleted.
- Textured/procedural emission requires evaluation consistent with the material
  contract; constant emission metadata alone is not sufficient. Area/PDF updates
  follow deformation. Sampling distributions must not assign zero support to a
  light that can contribute. Camera-visible emissive meshes are not the whole set.

## Surface contract and material authority

Add a dedicated `PASS_MODE_RTXDI_SURFACE` and matching shader/pipeline variant in
`render_forward_clustered.h`, `scene_shader_forward_clustered.h/.cpp` and
`shaders/forward_clustered/scene_forward_clustered.glsl`. Names here are proposed,
not existing APIs. It replaces the active opaque color draw; it is not an extra
material evaluation before running that old draw again.

Evaluate raster material code, decals, coverage, normal maps and emission once.
Export final surface parameters before any light loop. Do not define
`MODE_RENDER_DEPTH`: it suppresses decal work. Give the new mode separate guards
that suppress light/fog/GI evaluation while retaining material and decal work.
Keep actual alpha-scissor/hash discard, unlike `MODE_RENDER_MATERIAL`'s
alpha-zero substitution at shader 1353–1390: writing depth for a hole would hide
the surface behind it. The initial single-sample target does not use MSAA.
The normal/roughness prepass and late velocity draw cannot remain the sole
providers of inputs consumed earlier by RTXDI.

Proposed explicit first surface layout (subject to format capability checks):

| Target | Format | Content |
| --- | --- | --- |
| Base | RGBA8_UNORM | linear base color and coverage |
| Shading | RGBA16_SFLOAT | octahedral world shading normal, roughness, specular control |
| Emission | RGBA16_SFLOAT | HDR emission and metallic |
| Motion | RGBA16_SFLOAT | previous-minus-current pixel XY and same-surface linear-depth Z, AO |
| Geometry | R32_UINT | packed octahedral geometric normal |
| Classification | R32G32_UINT | exact material flags and receiver layer mask |
| Depth | existing depth format | device depth, reconstructed using matching projection |

This needs six color attachments and compatible usage/format support. Hardware
depth is not linear view depth. Existing UV velocity at shader 3074–3085 must be
converted to SDK pixel convention; depth motion needs previous position of the
same surface, not a previous depth lookup at an arbitrary pixel. Orthographic
and perspective reconstruction must be explicitly handled or unsupported modes
rejected. Half precision motion range and color quantization are quality limits
to inspect; no format is established as a measured optimum.

Keep current/previous surface sets and matrices per viewport/view, with a valid
history flag and frame index. First frame, resize, camera cut or discontinuous
surface state invalidates history. Background is explicit, never a stale
reservoir. Multiview requires independent eye histories; the first milestone may
explicitly reject multiview rather than accidentally share one.

The shared first BRDF contract follows existing `MaterialResult`: base color,
metallic, roughness, dielectric specular, emission, shading/geometric normals.
Extract the actual evaluation/sample/PDF functions to one shader authority used
by RTXDI and future probe shading; a similarly named struct alone does not unify
two different BRDFs. Custom `light()` callbacks and unsupported lobes must produce
clear material errors rather than execute the removed forward loop. This is
allowed to break old material compatibility.

Do not label thin-leaf transmission or glass as merely compatibility baggage.
The first opaque milestone does not complete them. Before claiming the complete
foliage renderer, add a shared two-sided/transmission model and the required
surface/visibility data. Alpha blending needs its own multi-layer sampling and
composition topology; one opaque reservoir per pixel cannot represent it.
Preserving a legacy forward-lit transparent pass would violate the intended
shared lighting authority. Explicit unsupported diagnostics are preferable in
the bounded milestone, with the visual feature retained as subsequent work.

## RTXDI runtime and shader build

Use the previously pinned sample `a6efab96` / runtime `f12037fa`, not unpinned
main or Donut/NVRHI. `ReSTIRDIContext::GetReservoirBufferParameters()`,
`GetBufferIndices()`, `SetFrameIndex()` and `SetResamplingMode()` own the layout
and rotation. Select `TemporalAndSpatial`, initially without checkerboarding.
The pinned host declares `c_NumReSTIRDIReservoirBuffers = 3` and computes indices
in `Source/ReSTIRDI.cpp:253–287`. Allocate three arrays using SDK pitches, with
barriers between temporal, spatial and shading accesses. Do not spatially gather
from a buffer being overwritten by neighboring invocations.

Use GLSL bridge callbacks for surface loading, light reconstruction/sampling,
target PDFs, light remapping and visibility. Final shading traces current-scene
visibility; start with stored visibility reuse disabled. Current AS for temporal
correction has a known transient-bias tradeoff. Previous AS is not part of the
first allocation contract; it can be reconsidered from actual artifacts.

There is a concrete build blocker to solve, beyond `RTXDI_GLSL` macros:
`glsl_builders.py::include_file_in_rd_header()` resolves only relative paths or
`thirdparty/` root paths, while runtime headers include `Rtxdi/...`. Add a scoped
SDK include root to the builder and explicit recursive `.h/.hlsli` dependencies
in shader `SCsub`. Its textual flattener also processes conditional includes;
include expansion/order and guards need compiler verification. Preserve upstream
SDK files; any necessary modification uses a recorded patch/import recipe.
Do not hand-edit generated headers or rewrite SDK files' include paths in place.

Register runtime host sources/dependency import in `servers/rendering/renderer_rd/SCsub`
and shader entry points under `shaders/raytracing/SCsub`. The GLSL claim remains
uncompiled; the first implementation gate must compile the selected DI closure
and RAB bridge before building the rest of the integration around it.

## Denoising choice and composition

Recommend NRD `RELAX_DIFFUSE_SPECULAR`, following FullSample, with a native RD
dispatch adapter. Use its sample-pinned NRD commit
`2cd553031abec76bc4e109f20ef40a23ef0645c7` (header version 4.17.1), not the newer
master documentation as an API contract. `NRD.h` exposes `CreateInstance`,
`GetInstanceDesc`, `SetCommonSettings`, `SetDenoiserSettings`,
`GetComputeDispatches`, `DestroyInstance`. Its returned dispatch memory belongs
to the instance and is overwritten by the next query.

FullSample `NrdIntegration.cpp` consumes `computeShaderSPIRV`, binding offsets,
permanent/transient texture descriptions, and the dispatch list. Translate these
to RD shaders, uniform sets, textures and compute dispatches. This is necessary
to execute the chosen denoiser; an NRI/NVRHI renderer integration is unnecessary.
Prepared SPIR-V must match the pinned host library. A per-viewport NRD instance
and pools are destroyed with the corresponding render buffers.

Use the pinned NRD front-end packing/material-factor contract for noisy diffuse
and specular radiance/distance, normals, roughness, view depth and motion. Do not
feed arbitrarily packed final color into RELAX. Reapply removed material factors
exactly once after denoising, then add emission. Direct lighting is evaluated at
native internal resolution; DLSS upscaling is downstream. Existing PT-specific
DLSS-RR guide buffers are not automatically valid for this topology, so RR is not
the first DI denoiser contract.

## Contracts removed or changed

Remove obsolete PT activation as the shared RT-scene authority. If the runtime PT
path is retired by the implementation plan, remove its public properties through
`scene/resources/environment.{h,cpp}`, Environment XML, renderer environment
storage, RenderingServer declarations/bindings and scene-cull forwarding in one
change. No migration or renamed enable flag is required. PT quality controls
must not silently acquire unrelated RTXDI meanings.

The opaque direct loops and their shadow-map evaluation are replaced, not added
to. Volumetric fog is a verified remaining shadow consumer:
`_update_volumetric_fog()` at `render_forward_clustered.cpp:1357–1427` receives
positional/directional shadow data. Retain required production at 1599–1655
and bindings at 3517–3612 for that consumer in the first DI milestone. This is
visual-effect functionality, not backward compatibility. Removing that GPU cost
requires separately replacing fog visibility; opaque RTXDI alone does not do it.
Do not remove alpha casters from shadow passes. New quality controls,
if exposed, need actual defaults, binding/XML and serialization contracts;
required RT itself does not need an enable toggle.

Narrow PT removal inventory also includes `rendering_server_default.h:895`,
`renderer_scene_render.cpp:712–733`, `rendering_method.h:319–326`,
`storage/environment_storage.h:175–180,334–340` and cpp 899–938,
`rendering_server_enums.h:760`, RenderingServer XML methods 1402–1409 and
denoiser constants near 5998–6001. PT settings are registered at
`rendering_server.cpp:3829–3835` and ProjectSettings XML 3258–3282.
These are source anchors, not permission to erase unrelated scene files;
serialized uses need a bounded inventory before the implementation commit.

## What the next implementation plan must close

The executable dependency order is shader/dependency integration, shared RT
ownership/light registry, raster surface replacement, DI/history, then
NRD/composition and removal of superseded consumers. These are whole-feature
implementation tasks requiring separate implementer contexts, not inline edits.

The concrete recommendation for the first milestone is mono-view, single-sample
opaque standard PBR, including alpha-cutout raster visibility and analytic/area/
standard-emissive lighting. Unsupported custom lobes, procedural emissive light
sampling and blended surfaces must be diagnosed explicitly. Extending those
features is subsequent work, including the target thin-leaf model.

Before execution, turn this research into an implementation plan with the exact
startup error API/thread handoff and serialized-consumer inventory, and review
that plan. A fresh design-review dispatch was attempted but the runtime rejected
it with an agent thread limit. No independent review verdict is claimed.

Implementation validation, once authorized: selected shader compilation, Windows
Vulkan editor and template builds, resource loading, then real-device inspection
of analytic/area/emissive lights, moving lights/occluders, camera cuts, resize,
multiple viewports, off-screen shadows and emission/composition. Inspect GPU
timings and allocations per pass before optimizing. No automated tests are
proposed or authorized; no FPS or minimum GPU performance is established.

## Evidence

Local paths above are relative to `servers/rendering/renderer_rd` unless otherwise
specified. Research used targeted source, ClassDB/XML and shader/build reads;
root compiledb was absent. Reused prior baseline evidence where unchanged.

- [Pinned DI host rotation](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b/Source/ReSTIRDI.cpp)
- [Pinned DI host declarations](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b/Include/Rtxdi/DI/ReSTIRDI.h)
- [Sample NRD adapter](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Samples/FullSample/Source/RenderPasses/DenoisingPasses/NrdIntegration.cpp)
- [Pinned NRD API](https://github.com/NVIDIA-RTX/NRD/blob/2cd553031abec76bc4e109f20ef40a23ef0645c7/Include/NRD.h)
- [NRD overview: RELAX targets RTXDI](https://github.com/NVIDIA-RTX/NRD)
