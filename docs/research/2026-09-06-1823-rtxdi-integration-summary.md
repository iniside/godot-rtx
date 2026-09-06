# RTXDI integration research

Scope update: the owner's later RT-only/no-backward-compatibility instruction
supersedes compatibility requirements below. See the
[concrete design](2026-09-06-1840-rtxdi-renderer-design-summary.md) for current scope.

Date: 2026-09-06 UTC. Godot baseline: `33b555c7f225afd1e586bf749f2b485c1de260df`.
Owner direction: RTXDI before DDGI, with shared direct lighting across visible surfaces and future probe hits. Raster primary visibility, automatic cluster DAG, no manual object LODs, geometric near foliage and distant voxel foliage remain the target.

Research only: no source changes, compilation, sample execution, or automated tests. This is an architectural recommendation and gap inventory, not an approved implementation plan.

## Pinned NVIDIA references

SDK/sample commit: [`a6efab966b7c3b272da0461578eb56ac61c7cbff`](https://github.com/NVIDIA-RTX/RTXDI/tree/a6efab966b7c3b272da0461578eb56ac61c7cbff).
Its `Libraries/Rtxdi` gitlink pins the runtime separately at [`f12037fa8e97ebc08e9e3edfd2de528ed1772a4b`](https://github.com/NVIDIA-RTX/RTXDI-Library/tree/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b).
Selected raw files were downloaded for read-only inspection to the OS temporary `codex-rtxdi-research` directory. No SDK was vendored into Godot.

The runtime carries NVIDIA's SDK license, rather than Godot's MIT license. Preserve its distinct license and notices in any future dependency integration; this report does not determine redistribution terms. [Pinned license](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b/LICENSE.txt).

## What to integrate

RTXDI supplies sampling/resampling math; the application owns scene representation, materials, rays, resources and dispatch. `ReSTIRDIContext` is the DI host entry. Current/previous light data and index translation are necessary for temporal reuse. Screen reservoirs use SDK-computed array pitches. The documentation explicitly describes buffer-less sampling for RTXGI probe hits. ReGIR adds spatial light proposals and can feed both screen ReSTIR and off-screen sampling. It does not replace shadow visibility. [Integration guide](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/Integration.md).

Recommendation: share the light registry, light samplers, PDFs, material evaluation and visibility contract. Camera pixels get screen-space temporal/spatial reuse. Future DDGI hits get world-space initial sampling, optionally ReGIR. They do not inherit camera-pixel reservoirs. This meets the owner's lighting consistency goal without requiring ReSTIR GI or PT.

## Sample code worth reusing, and its boundaries

`MinimalSample/Shaders/Render.hlsl` demonstrates initial local-light/BRDF sampling, fused spatiotemporal reuse, weighted shading, final visibility and reservoir storage. It traces primary rays and tone-maps its output directly. Those two choices do not match our raster-primary HDR pipeline. Its fused approach reads previous-frame neighbors; it is not a template for arbitrary current-frame spatial passes. [Minimal render shader](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Samples/MinimalSample/Shaders/Render.hlsl).

The minimal surface bridge returns an invalid surface for current-G-buffer queries. Its previous-surface branch reads previous depth/normals but passes current `u_GBufferDiffuseAlbedo` and `u_GBufferSpecularRough` into `RAB_GetGBufferMaterial`. That is a source-level inconsistency to investigate before copying, not a demonstrated runtime bug. Its BRDF assumes an isotropic tangent reconstruction. [Minimal surface bridge](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Samples/MinimalSample/Shaders/RtxdiApplicationBridge/RAB_Surface.hlsli).

Use FullSample's separate DI passes as the clearer frame-graph reference. `ShadeSamples.hlsl` loads a surface and reservoir, reconstructs the light sample, shades it, and emits diffuse/specular plus distance inputs for denoising; it supports compute ray queries or ray-generation dispatch. Adapt its output contract to Godot rather than adding the sample's result over existing direct light. [Full shading pass](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Samples/FullSample/Shaders/LightingPasses/DI/ShadeSamples.hlsl).

The full visibility bridge distinguishes cheap conservative visibility from final visibility including transparent/alpha-tested geometry. Temporal visibility may use previous AS; using current AS is explicitly associated with transient bias. Therefore previous TLAS/BLAS retention is a quality/memory design question, not an automatic requirement to duplicate the entire scene. Its triangle-oriented final ray query is not ready-made traversal for our future voxel representation. [Visibility bridge](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Samples/FullSample/Shaders/LightingPasses/RtxdiApplicationBridge/RAB_VisibilityTest.hlsli).

## Shader integration route

The pinned runtime already contains `RTXDI_GLSL` type, texture and subgroup macros in `Include/Rtxdi/RtxdiTypes.h`. Native GLSL inclusion with our bridge and RD descriptors is therefore the first route to evaluate. This is source evidence of intended support, not proof that the entire selected include closure compiles in this fork. [Runtime types](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b/Include/Rtxdi/RtxdiTypes.h).

NVIDIA's sample uses DXC/ShaderMake for HLSL-to-SPIR-V, Vulkan 1.2, SM 6.5 and NVRHI register offsets. This is a reference alternative if native inclusion hits a concrete blocker; Donut/NVRHI are sample infrastructure, not a proposed replacement for RenderingDevice. [Sample shader build](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Samples/MinimalSample/Shaders/CMakeLists.txt).

## History and resource contract

The bridge's temporal depth-motion component is the previous-minus-current linear depth of the same surface. Existing 2D motion textures cannot simply be passed unchanged. Current and previous material/normal/depth reconstruction must agree on coordinate space, camera jitter, instance motion and internal resolution. [Bridge specification](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/RtxdiApplicationBridge.md).

The packed DI reservoir contains six 32-bit fields (24 bytes). Two tightly packed 1920x1080 arrays alone would be 94.9 MiB; at 2560x1440, 168.75 MiB. These are arithmetic illustrations before block padding, surface history, lighting textures, PDFs, AS or denoiser resources, not selected presets. Additional separate passes may need additional arrays. [Packed layout](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b/Include/Rtxdi/RtxdiParameters.h#L136).

Recommended invalidation cases: camera cuts, resize/internal-resolution changes, viewport destruction, changed light identity, deleted emissive triangles and discontinuous geometry changes. Wind and moving occluders also require conservative limits on visibility reuse. These are integration obligations inferred from persistent sampling, not behavior verified in Godot.

## Scope recommendation

### Local integration map

Read-only local investigation used the project index, targeted source reads and bounded text searches. Root `compile_commands.json` was absent, so clangd was unavailable. Git history identifies RT introduction at `135dff3887` and light gathering at `ce5786d6c0e` as fork work. Text inventory is a lower bound; this is not a complete public API audit.

| Area | Verified source anchor | Consequence |
| --- | --- | --- |
| Raster scheduling | `forward_clustered/render_forward_clustered.cpp`: `_render_scene`, 2130, 2399–2466; `_pre_opaque_render`, 1573–1710 | Opaque materials currently shade in the forward draw. A compute DI pass needs material export before sampling and a later composition consumer. A pre-opaque hook alone does not supply those inputs. |
| Surface buffers | Same file, 2023–2038 and 2142–2156 | Normal/roughness and velocity are conditional. No complete current/previous material surface set is provided by this path. Force required production under the new renderer's ownership, independently of unrelated effects. |
| Existing material export | Same file, `_render_material`, 3093–3149; `shaders/forward_clustered/scene_forward_clustered.glsl`, 1040–1050 | Material outputs exist in a separate mode, not normal opaque attachments. Reuse its lessons; do not assume it is already a full RTXDI G-buffer. |
| Material authority | `scene_forward_clustered.glsl`, 1194–1205, 1604–1614; `shaders/raytracing/raytracing_material_eval_inc.glsl`, 4–12 | Raster material evaluation includes decals; RT has a separate `MaterialResult`. Shared BRDF semantics and surface export need explicit reconciliation. |
| Direct light and composition | `scene_forward_clustered.glsl`, 2634–2848, 3016–3059 | Directional/omni/spot/area evaluation writes direct terms before material modulation and final output. Replace these direct contributions for the new path; a texture added over finished color would retain old direct lighting. Define whether DI textures are demodulated or final radiance. |
| RT light domain | `forward_clustered/render_raytracing.cpp`, `gather_lights`, 3075–3255; `render_raytracing.h`, 143; upload at cpp 3395–3417 | Existing list is scored/truncated to 64 and contains directional/omni/spot lights. It is not a stable, complete RTXDI light registry. Area and emissive lights need coverage; temporal remapping cannot use a sorted slot as identity. |
| Existing RT shading | `shaders/raytracing/raytracing_closest_hit_common_inc.glsl`, 481–549; `raytracing_lights_inc.glsl`, 22–46, 168–250 | Material and shadow semantics are useful references, but evaluator calls are coupled to RT payloads and the limited light list. |

Paths above are relative to `servers/rendering/renderer_rd/`. Raster layer masks and baked-light handling occur at `scene_forward_clustered.glsl` 2696–2702, 2757–2763 and 2818–2823; a shared sampler must preserve the applicable upstream semantics. Sample materials are not an authority for Godot's public shader behavior.

The [earlier scene-lifecycle investigation](2026-09-06-1748-rtxgi-ddgi-integration-summary.md) also remains relevant: TLAS setup and RT instance gathering are PT-enabled today. Hybrid DI requires independent scene activation and off-screen shadow-caster coverage. No new implementation was made in this research.

Recommendation: evaluate a raster material-export stage followed by separate DI passes and HDR composition, rather than attempting to recover materials from the finished forward color. This entails a renderer change, with upstream-compatible material behavior to resolve before approval. It does not require replacing raster visibility with primary rays from MinimalSample.

Start with a complete opaque DI vertical slice: raster surface export, shared lights/material evaluation, initial sampling, temporal/spatial reuse, final visibility, denoising and HDR composition. Initial debugging can disable reuse to isolate errors, but that is not the final product path. Integrate analytic, emissive and environment light categories deliberately; unsupported categories must be explicit, never silently dropped.

A unified lighting path still needs an explicit material contract for custom light shaders, anisotropy, clearcoat, unshaded objects, transparency and thin-leaf transmission. Opaque leaf geometry removes alpha-cutout traversal but does not define light transmission. DDGI and ReGIR follow once this foundation works; neither the completed DAG nor voxel foliage should block the first integration.

Open before an implementation plan: supported material/light coverage, exact raster surface export, denoiser choice and guide format, reservoir layout/pass count, shader include compatibility, and whether temporal bias justifies previous AS retention. No local timing or GPU minimum is established.
