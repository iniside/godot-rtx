# Shader unification research

2026-09-07 11:12 UTC; source baseline `a89cd8a3b064427e29a89af0f0b6823bfac21da7`.
Read-only renderer research plus standalone compiler diagnostics; no engine edits,
engine build, rendering run, automated tests, or implementation approval.
Owner requested starting unification before DDGI after discussing growing adapters.
The owner approved implementation on 2026-09-07 with "no to zaczynaj". The
[approved plan](../plans/2026-09-07-1126-shader-unification-plan.md) landed in
`b2d04129cb`; this report records the preceding research evidence.

Fresh hostile plan review returned PASS on 2026-09-07 at the baseline above;
reviewed plan SHA-256 `7A9E7BA2B27BDDC248A0D0C2032BF76F6A23E991253CFF0550D2CBEE39F99596`.
It checked shader/RD, lifetime/threading, public contracts, build/export, scope and
old PT dependency removal, and planned real-device validation. This is a plan
verdict, not an implementation/runtime verdict.

## Recommendation and boundary

Use Slang direct SPIR-V for the migrated Forward+ spatial/surface/DI/NRD-HDR core,
with a real target emitter behind the existing Godot shader frontend. Keep public
gdshader/VisualShader authoring and existing material resource contracts. Retain
distinct canvas/UI, sky/fog/particle and other postprocess GLSL consumers in this
first scope. This is not a claim of whole-engine GLSL removal. Shader language is
selected per consumer, not by globally changing the Vulkan device compiler.

Use existing ShaderRD cache/baker and final SPIR-V reflection/container. RAB keeps
SDK callbacks/history/reservoir semantics; renderer material, BRDF/sample/PDF,
geometry decode, light evaluation and visibility belong to a common shader library.
NRD resource orchestration remains necessary; use its pinned shader header for
encoding and material factors instead of handwritten GLSL equivalents.

Complete probe-hit material evaluation, probe ray footprints and DDGI itself remain
later work. This migration makes inputs explicit and preserves current behavior;
it does not establish arbitrary custom shader, procedural, decal or transmission
support at ray hits. Retain existing unsupported-material diagnostics.

## Source map

Paths below are relative to `servers/rendering` unless otherwise indicated.

| Authority | Verified source behavior / implication |
|---|---|
| `rendering_device.cpp:232`, `shader_compile_spirv_from_source` | Public HLSL enum exists but dispatch accepts only GLSL. |
| `renderer_rd/shader_rd.cpp:280,407,1154` | Builds template variants; `compile_stages` hardcodes GLSL. Target/entrypoint/compiler identity must reach runtime and cache. |
| `renderer_rd/shader_rd.cpp:986-1065` | Existing cache identity must include compiler/version/options/target, avoiding stale reuse without deleting caches. |
| `shader_compiler.{h,cpp}` | Existing frontend emits GLSL types, declarations, IO and texture/sampler expressions plus CPU uniform metadata; token aliases are insufficient. |
| `renderer_rd/forward_clustered/scene_shader_forward_clustered.cpp:41,238,282` | Spatial `set_code`, RT support diagnostics and `set_code_rt` are material authority. |
| `renderer_rd/storage_rd/material_storage.cpp:2228` | Shader type selects material owner; canvas shares compiler machinery but has its own configuration. |
| `editor/export/shader_baker_export_plugin.cpp:373-428` (repo relative) | WorkItem captures source/dynamic buffers; target/entrypoint/options must propagate into export compile. |
| `glsl_builders.py:203`, `SConstruct:1216` (repo relative) | Source embedding/include dependencies, distinct from runtime shader compilation. |
| `renderer_rd/spirv-reflect`, `rendering_shader_container.cpp`, `drivers/vulkan/rendering_shader_container_vulkan.cpp:47` (last repo relative) | Retain downstream reflection/compression. Existing Vulkan container target reports Vulkan 1.1/SPIR-V 1.4; internal new target must be explicit and verified through actual RD path. |

Public frontend evidence: `scene/resources/shader.cpp:89,290,301`,
`doc/classes/Shader.xml`, `modules/visual_shader/visual_shader.cpp:2678,3155`.
VisualShader generates Godot shader source and calls Shader::set_code; preserving
that frontend avoids a second authoring language or new public Resource/API.

Shader paths below are under `servers/rendering/renderer_rd/shaders`:

- `raytracing/rtxdi_application_bridge_inc.glsl:274-316` mixes material conversion
  and BRDF evaluation/sample/PDF with RAB. Its camera-dependent coverage footprint
  at 360-377 must become explicit input before future probes reuse coverage.
- `raytracing/rtxdi_light_sampling_inc.glsl:33-117` duplicates geometry helpers in
  `raytracing/raytracing_hit_inc.glsl:37-129`. The latter embeds stage builtins;
  extract explicit geometry/primitive/cluster/barycentric inputs rather than include
  hit-stage code from compute. Preserve bounds/remap validity semantics.
- `rtxdi_di.glsl` under raytracing uses candidate and committed CLAS ray queries,
  nonuniform descriptors, device addresses and 64-bit data. Ordinary ray-query
  compiler support alone is not enough.
- `raytracing/rtxdi_nrd_inc.glsl` and `effects/rtxdi_frame.glsl` share material
  factors but handwrite SDK math/normal packing. Preserve demodulation/remodulation,
  raw radiance, exposure once, and existing guide/fog ordering.

The single host light registry remains `RenderRaytracing::build_light_registry`;
do not introduce a second light table or camera reservoir ownership for DDGI.

## Dormant PT remains a live geometry dependency

In `renderer_rd/forward_clustered/render_raytracing.cpp`, `initialize:52-62` invokes
SceneShaderRaytracing initialization; `build_tlas:2541` ensures a pipeline bundle.
`is_hg_ready_in_bundle` gates procedural/MultiMesh/mesh eligibility at
2652/2750/2867. `prepare_frame:568` drains compilation and `build_tlas:3098`
finalizes custom shaders. Thus removing initialization alone can drop geometry.

Replace readiness gates with supported material/geometry classification in the
same removal step. `register_custom_shader`/`get_custom_shader_entry` consumers at
1687/1830/1833/2774/2943 still extract uniform/texture/opacity metadata. Preserve
that analysis without SBT slots or hit-group compilation. The existing extraction
seam is `_preprocess_shader`/`_finalize_uniforms_with_textures` in
`scene_shader_raytracing.cpp:435-607`. Remove pipeline bundles/workers/getters and
exclusively consumed raygen/hit templates after closing these consumers. Bounded
searches found no active external pipeline/SBT getter consumer; this is a lower
bound, with removal closure still requiring semantic/source and build checks.

## Compiler diagnostics

Installed binaries: `C:/VulkanSDK/1.4.357.0/Bin`.
Slang reports `2026.13.1-1-g84792eb15`; DXC reports `1.9.0.5399 (a107ba613)`.
These are inspected local versions, not claims about newest available releases.

| Diagnostic | Result |
|---|---|
| Slang compute RayQuery CandidateClusterID + CommittedClusterID | Compile and Vulkan 1.3 spirv-val PASS; disassembly has NV cluster capability/extension and both cluster query opcodes. |
| Same source, installed DXC cs_6_5 | Rejects intrinsics as requiring SM 6.10. Installed cs_6_10 attempt returns invalid shader module. Does not establish absence in other DXC versions. |
| Pinned RTXDI Reservoir/RandomSampler + NRD MaterialFactors/RELAX packing | Both Slang and DXC compile; both Vulkan 1.3 spirv-val PASS. Selected headers/functions, not full DI include closure. |
| RTXGI-DDGI f33e496 ProbeBlendingCS, radiance/shared memory, 128 rays, 8x8 texels | Slang compile and Vulkan 1.3 spirv-val PASS with HLSL=1 and __spirv__=1. Binding slots 0/1/2/4/5 and push constants confirmed in disassembly. Five implicit-conversion warnings remain in unchanged SDK source. |

Slang does not select this SDK's Vulkan branch automatically: without __spirv__=1
the diagnostic compiled the D3D-register branch and emitted binding-overlap
warnings. The final diagnostic explicitly selects Vulkan; do not treat syntax
success alone as ABI correctness. Definitions are SDK compile configuration,
not a handwritten replacement of SDK math. No vendored source was edited.

Files: `%TEMP%/godot-shader-unification-20260907` contains `cluster.hlsl`, `sdk.hlsl`,
SPIR-V/disassembly and `ddgi-blend-slang.log`; fetched DDGI include closure is under
its `ddgi/` directory. Slang used `-lang slang -target spirv -profile spirv_1_6`;
validation used `spirv-val --target-env vulkan1.3`. Diagnostics have no GPU dispatch
or assertions and are not automated renderer tests. Temporary files are local,
not durable runtime evidence or repository build output.

## Sources, method and limits

- [Slang native cluster-query implementation](https://github.com/shader-slang/slang/blob/master/source/slang/hlsl.meta.slang).
- [DXC SPIR-V mapping](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/SPIR-V.rst).
- [RTXDI application bridge contract](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RtxdiApplicationBridge.md).
- [Pinned DDGI blending shader](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/ProbeBlendingCS.hlsl).
- Pinned local RTXDI/NRD versions are owned by `misc/scripts/update_rtxdi_nrd.py`.

Two independent read-only agents used clang-nav against root compile_commands,
then source/declarations, ClassDB/XML, Python/SCsub/GLSL reads and scoped history.
One-shot references were sometimes TU-local; bounded inventories are lower bounds.
Parent inspected SDK/source build integration and ran compiler diagnostics. Full
shader variants, emitter completeness, CPU/GPU packing, actual RD reflection and
optimizer/container acceptance, cache/export, hot reload and runtime remain final
implementation verification requirements. No new FPS/performance claim.
