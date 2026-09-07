# Camera-following DDGI, comparison PT and DLSS RR — research

Date: 2026-09-07 UTC. Source baseline: `381c36eec7d7e0d5c9111be366a3a0ae78cdadd3`.
Owner scope: automatically created camera-following DDGI from the first working
version; optional path tracing for comparison sharing RT scene data; restore
Ray Reconstruction as a selectable alternative to NRD and expose usable DLSS
controls. Hybrid raster + RTXDI + DDGI remains the main renderer. No automated
tests were requested. No implementation, build or GPU launch was performed by
this research. Existing dirty files and game assets are preserved.

## SDK and integration choice

RTXGI-DDGI `f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6` (1.3.6) is still remote
HEAD, checked with `git ls-remote` on this date. Use this pinned DDGI source,
not RTXGI 2.x NRC/SHaRC, which implement different radiance-cache algorithms.

The earlier [SDK research](2026-09-06-1748-rtxgi-ddgi-integration-summary.md)
is historical for local integration. Its separate DXC/GLSL recommendation and
PT-only scene activation no longer describe the current renderer. Reuse SDK
probe math through the existing Slang -> SPIR-V -> RenderingDevice path.
The [Slang research](2026-09-07-1112-shader-unification-summary.md) records one
DDGI radiance blend variant compiled with `HLSL=1` and `__spirv__=1`; this is
bounded prior compiler evidence, not full DDGI execution or all variants.

Use an engine-owned DDGI effect and RD textures/pipelines. The SDK's raw Vulkan
host command recording is unnecessary: importing its shader/header closure does
not require allowing it to own descriptors, command buffers or barriers. Extend
the pinned import mechanism patterned on `misc/scripts/update_rtxdi_nrd.py`,
record licenses/provenance, and use recorded patches for any upstream changes.
`glsl_builders.py:294` embeds Slang include closures; its external include roots
and dependency tracking must include the imported DDGI closure.

## Moving probes and bounded work

The [SDK volume reference](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/docs/DDGIVolume.md)
supports camera-attached scrolling volumes. This is an internal finite working
set, not a fixed authored volume limiting world traversal. A camera-following
set of nested grids supplies near/far density without storing an entire world.

Source inspection finds a concrete integration constraint:
[`DDGIVolumeBase::ComputeScrolling`](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/src/ddgi/DDGIVolume.cpp#L399)
advances potentially multiple cells, whereas
[`DDGIClearScrolledPlane`](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/f33e496ca31b3f0eec1c4e2cbaa8bb620e337fa6/rtxgi-sdk/shaders/ddgi/include/ProbeIndexing.hlsl#L295)
matches one physical plane per axis. Calling these unchanged is insufficient
for multiple-cell jumps and teleports. `GetDescGPU()` also packs bounded scroll
offsets; engine world travel must not accumulate unbounded offsets there.

Proposed contract: CPU cell identity uses signed 64-bit integer coordinates;
physical slots use normalized positive modulo. Update mapping and invalidate
the complete union of entering slabs for every cascade each frame, even when
its lighting update is skipped. A jump with no overlap resets the whole cascade.
Keep overlapping probes' world positions and history fixed. Recycled slots lose
irradiance, distance, relocation, classification and validity together. An
engine reset pass owns this; suppress the SDK's one-plane clear flags so fresh
results are not erased again in the blend pass. Bounded scroll offsets and a
compensated descriptor origin preserve the SDK indexing identity.

Start with whole-cascade updates to avoid patching SDK workgroup indexing for
arbitrary sparse probe batches. A configurable updates-per-frame budget bounds
ray work. Update newly exposed cascades preferentially with explicit maximum
age/fairness for all other cascades. Invalid cells are never sampled as valid
old lighting while waiting. Classification-inactive probes still receive the
SDK fixed rays required for reactivation when their cascade is updated.

Trace reads existing probe lighting, then barriers precede irradiance/distance
blend, relocation/classification and final screen sampling. Shared ray-data
scratch can be reused sequentially between equal-size cascades after its last
consumer. Do not require a full extra atlas copy merely for sequential updates.
Probe ray rotation state must stay matched to that update's ray data.

Probe interpolation and overlap blending must account for pending validity.
Blend normalized contributions from eligible cascades; never sum overlapping
volumes as independent energy. No valid probe support means zero DDGI until
initialized, not stale radiance or a silent SDFGI/ambient fallback. Expose this
state in probe diagnostics. SDK leak suppression remains an approximation.

## Current local scene and ownership

`RendererSceneCull::_scene_cull` at `servers/rendering/renderer_scene_cull.cpp:3229`
gathers resident visible meshes/MultiMeshes and local lights independently of
camera frustum/HZB/far range. Directionals join at 3681 and the lists enter
`render_scene` at 3704. This replaced the old camera AABB in `afbe198fefa`.
`RenderRaytracing::build_tlas` at `render_raytracing.cpp:2529` consumes that scene.
No DDGI volume-cull expansion or world streaming subsystem is needed here.

`RenderRaytracing::_get_or_create_viewport_state:88` keys camera state by
`RenderSceneBuffersRD*`; `_free_viewport_state_internal:111`/`free_viewport_state:148`
and Forward+ `free_data:211` already establish the release chain. DDGI follows
this ownership, not shared Environment RID ownership. Existing `_render_scene`
rejects reflection captures, MSAA and multiple views at 1806–1808; this plan does
not add stereo RT. Multiple independent mono viewports remain in scope.

`RendererViewport::viewport_set_scenario:1334` does not clear render-buffer
history and RenderDataRD has no scenario RID. Propagate scenario identity or
invalidate there to prevent DDGI/PT history crossing worlds. DDGI cell history
must not inherit every camera screen-history reset.

`build_acceleration_structures:2086`, previous motion uploads at 2668/2868/3019,
light/projector/emissive transforms at 3126/3212/3257, and DI reconstruction in
`rtxdi_application_bridge_inc.slang:29–73` currently use float world translations.
Use CPU-subtracted per-buffer RT origins before packing, relative RT camera
constants and explicit previous/current snapshot-basis conversion. Keep BLAS
local and raster SceneData/Node/physics coordinates unchanged. Ordinary float
builds cannot recover placement precision already lost on the CPU.

Opaque frame composition is `shaders/effects/rtxdi_frame.slang:191`; it currently
adds emission and DI, without SDFGI/VoxelGI/lightmaps. Add DDGI there rather than
revive the historical forward ambient branch. PRE_OPAQUE guide readiness at
`render_forward_clustered.cpp:2037–2047` must remain valid.

## Shared hit-material programs and true PT

Current shared Slang supplies SurfaceData, BRDF/PDF/sampling and geometry/coverage/
light helpers. `geometry_decode_inc.slang:164` does not yet decode complete
normals/tangents/UV2/custom attributes. `surface_shading_inc.slang:208` returns
only a partial hit, and `RenderRaytracing::process_material:1625` uploads metadata
without executing hit code. Uniform arrays reserve space but need complete
packing. This is the largest shared DDGI/PT implementation dependency.

Use existing `ShaderCompiler::TARGET_SLANG` and `_dump_slang_call:533` with an
explicit hit context, byte-offset uniform access, typed bindless textures and
ray-cone implicit LOD. Preserve explicit textureLod/Grad semantics. Native RT
stages already map in `modules/slang/shader_compile.cpp:134,183`, `ShaderRD`
assembles RT stages at `shader_rd.cpp:360`, and RD exposes `HitGroup` and
`raytracing_pipeline_create` at `rendering_device.h:1294,1300`. Implement shared
material hit programs/SBT with distinct DDGI/PT raygen consumers. Count-one,
offset-zero TLAS SBT mapping at `render_raytracing.cpp:2088` must change with the
new program mapping. RT-list BDA dependencies must use the retained current
dependency set at 3386, not the removed PT subset.

PT means true primary camera rays, including orthographic camera handling, with
its own surface/depth/motion exports. Reuse retained previous deformed positions
and motion transforms; pixel velocity is previous UV minus current UV. Raw FP32
progressive accumulation is independent of DDGI and temporal denoisers. Preview
diffuse/specular split follows first scattering, with emission separate.

Historical PT remains an algorithm reference, not a copy target: old dielectric
F0 was `0.16 * specular²`, current shared BRDF is `0.08 * specular`; old lights
predate current registries and fog was per segment. Comparison uses current
common semantics and documents primary-fog limitations. Current Forward+
`_render_scene:2092–2095` has pre/post-transparent callbacks but no transparent
draw; alpha-pass materials are explicitly unsupported at 3752–3757. Preserve
that shared diagnostic envelope rather than imply a retained alpha-blend pass.

Supported RT geometry is the existing mesh/CLAS/merged-MultiMesh/deformation
envelope. Procedural AABB intersection and particle transform expansion do not
have corresponding current RT execution. Existing unsupported material flags
at `render_forward_clustered.cpp:3754–3756` and `scene_shader_forward_clustered.cpp:238`
must remain explicit. Raster decals currently affect exported material at
`scene_forward_clustered.slang:1362`; shared hit evaluation must account for
them or explicitly identify a common excluded comparison case.

The plan includes common decal evaluation: `TextureStorage` owns world-space
DecalInstance/resource/atlas (`texture_storage.h:333–366`), but
`update_decal_buffer` (`texture_storage.cpp:4096`) packs camera-view transforms
at 4179. Extend existing scenario volume gathering (current camera-only decal
append `renderer_scene_cull.cpp:2940`) and reuse the packing/schema with an RT
origin transform. Extract the material operation at `scene_forward_clustered.slang:1393–1459`;
preserve camera fade/sort, overlapping iteration order and capacity admission.
RT initially iterates a resident decal snapshot; no new spatial subsystem/API.

## Ray Reconstruction and actual versions

Local inspected SDK is Streamline 2.10.0. `bin/sl.dlss_d.dll` is 2.10.0.0;
`nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssg.dll` are 310.5.0.0. These files
are ignored binaries, not pinned by the current source checkout. SHA256:

- SR: `22C4660249DA5B352CDE0CE4D95178C3761223DD6AC096D0B88E5C03D7A5CDD1`
- RR: `4D53F6114EDC61BD41FC293008AA818AB092BB8286830E61C98453DA92D936D0`
- SL RR: `1A65AE1656D7AAB4CB1025E14BC14A92330A3A3008A05AAFAE803C4FE508C260`

`drivers/streamline/streamline_context.cpp:327` allows OTA/downloaded plugins.
Therefore the bundled versions alone do not establish the effective model.
[NVIDIA's August 25 announcement](https://www.nvidia.com/en-us/geforce/news/gamescom-2026-dlss-4-5-ray-reconstruction-release-announcements-trailers/)
describes 4.5 RR/preset F delivery through NVIDIA App early access. The public
[2.12.0 release](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.12.0)
does not by itself prove model F availability in a redistributable package.
The untracked old local RR note's universal D/E-only statement is historical.
A version/provenance import and effective runtime check is required before
claiming the requested 4.5 behavior. No automatic driver/App changes are planned.

The [pinned RR guide](https://github.com/NVIDIA-RTX/Streamline/blob/v2.10.0/docs/ProgrammingGuideDLSS_RR.md)
defines camera HDR color, primary depth/motion, diffuse/specular reflectance,
normal/roughness, exposure and reset semantics. RR replaces the NRD/SR camera
path; probe history still belongs to DDGI. Use remodulated raw RTXDI plus DDGI
for hybrid and actual PT camera output/guides for PT.

The [2.10.0 RR implementation](https://github.com/NVIDIA-RTX/Streamline/blob/v2.10.0/source/plugins/sl.dlss_d/dlss_dEntry.cpp#L563)
requires material/normal guides but retrieves specular-hit-distance and specular
motion tags with `optional=true` at 586/592 and forwards a nullable Vulkan
resource at 922. Hybrid can therefore omit reflection tags rather than invent
distances or add a reflection subsystem. Underlying NGX acceptance and image
quality for this exact hybrid remain required GPU validation.

Current `DLSSEffect` retains RR APIs but Forward+ never enables them. Its caller
hardcodes reset false; context creation queries SR settings even for future RR;
destruction recycles an ID without feature resource free. Callback output access
and tag lifetimes require correction at their actual RD/Streamline authority.
Mode changes must remove old PT optional tags before hybrid RR evaluation.

## Public API placement

Environment stores mode/denoiser and DDGI/PT configuration; per-buffer state
owns histories. Follow `_update_sdfgi` through RenderingServer,
RenderingServerDefault, RenderingMethod, RendererSceneCull, nonvirtual
RendererSceneRender and shared RendererEnvironmentStorage. This route avoids
unnecessary new dummy/GLES3 virtual overrides. Keep upstream SDFGI and Viewport
contracts, synchronize ClassDB/XML and replace stale automatic-PT-to-RR docs.
DLSS quality stays on existing Viewport/root project scaling settings; scene
RR selection remains visible with useful availability/configuration messages.

## Evidence methods and remaining synthesis

Methods: maintained project-state and current owner direction; targeted source
and shader reads; clangd navigation by separate bounded research contexts;
historical `git show` for deleted PT; official pinned NVIDIA source. Bounded
text inventories are lower bounds, not exhaustive API/reference coverage.
Current runtime quality/performance and effective DLSS model remain unproven.
The accompanying harness plan records public contracts, source integration,
ownership, remaining compiler/model validation, and implementation order.

## Developer payload delivery follow-up, 2026-09-07

The independent read-only delivery investigation verified the official
[Streamline 2.12 release](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.12.0)
at commit `e8aaa6eaac968711fb62473d4ae8256dde20919b` and
[DLSS 310.7 release](https://github.com/NVIDIA/DLSS/releases/tag/v310.7.0)
at commit `a291cc7d2cc642a51566f3dfd5376f635cd1b284`.
Downloaded Streamline archive SHA-256:
`F5C0A3D870707DDDC3570FB4BCD3655CF48A8A68C3A9D342910CFA21B77DCF48`.
Downloaded DLSS demo archive SHA-256:
`6A66B2808976506B9BE07EEAB922914439F678A8AF2AF83AE0927EEAFA04DEDB`.

The public package supports SR 4.5 with documented L/M presets and functional
RR using D/E presets. Public RR preset F reverts to default and does not prove
RR 4.5. The extracted signed NVIDIA production SR 310.7 DLL hash is
`BE6E434A94CA32499515EB62CA0E6C274526055D568D0426E4C652DCDFB6EE6E`;
RR 310.7 DLL hash is
`F4E97624F70FBB769ACB11EBD751B512ECC9463D4BD6AEF04896D3956E6084A0`.
These establish package provenance, not the effective model during rendering.

[NVIDIA App release notes](https://www.nvidia.com/en-eu/software/nvidia-app/release-highlights/)
announce RR 4.5 through App/OTA. The investigated live OTA manifest and signed
local cache contain NGX 310.9 / Streamline 2.14, but no corresponding public
developer SDK archive or standalone redistribution provenance was established.
Do not vendor the local cache. This limits a bundled RR 4.5 claim, not restoration
of the requested RR option. The original owner separately requested missing
DLSS 4.5 and RR availability; do not silently reinterpret both as RR 4.5.

Step 7 should use a reproducible official-package importer, matching headers,
license/provenance and build/export dependency closure. The current Windows
exporter has Slang dependency copying but lacks equivalent Streamline closure.
Public 2.12 deprecates `slSetTag` in favor of `slSetTagForFrame` with
`eUseFrameBasedResourceTagging`. Verify feature lifecycle, actual loaded versions,
presets and successful NGX evaluation on the real GPU and in an exported build
without relying on an App override. No driver or App configuration was changed.

## Plan disposition

Active harness artifact:
`C:/Users/lukas/.codex/plans/2026-09-07-1635-godot-ddgi-pt-rr-plan.md`.
Fresh round 1 review rejected one scope error: a promised existing transparent
pass is absent. The corrected plan preserves current alpha diagnostics and
compositor callbacks. A new reviewer returned final round 2 PASS on 2026-09-07,
checking classes 1–9, actual source/API/SBT/lifecycle and validation requirements.
The owner subsequently approved implementation with "implementuj plan" on
2026-09-07. The [approved repository plan](../plans/2026-09-07-1635-ddgi-pt-rr-plan.md)
was committed separately as `00f73fb82f` before implementation. Execution is
tracked in [implementation status](2026-09-07-1701-ddgi-pt-rr-implementation-status.md).
