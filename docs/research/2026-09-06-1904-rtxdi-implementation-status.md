# RTXDI implementation status

Owner approval: 2026-09-06, "zaczynaj".
Approved plan commit: `e02ea8c87dbad71ca5db85cbba605c47e9825477`.
Implementation task baseline: `33b555c7f225afd1e586bf749f2b485c1de260df`.
Plan: [RTXDI replacement](../plans/2026-09-06-1854-rtxdi-renderer-plan.md).

## Current stage

Step 1 landed in `89b35e1d1c287bc3e68ac289c2aede7363f08dc3`; its fresh hostile
review returned PASS on 2026-09-06. Step 2 landed in
`ee7ae4e52888eba32b1e2b62c93cb64bab4aebaa`; its first fresh review returned
REJECT for missing bindless descriptor-indexing preflight. Scoped fix
`de61204e58d62a8aee25fa3a4fee5a836a7fc0ce` adds an internal RD feature backed by
the four Vulkan descriptor-indexing bits and a compositor preflight check.
Fresh round 2 review returned PASS on 2026-09-06. The fix has a clean staged diff check but no
separate compile yet; Step 3 owns concurrent renderer edits.
Step 3 landed in `afbe198fefaa10ab4679cffa15e258106d4efdfb`; first fresh review
returned REJECT for four concrete defects: shared shader binding collisions,
missing area-light range attenuation, inconsistent environment PDF re-evaluation,
and inclusion of SKY_ONLY directionals in scene direct lights. Scoped fix
`1c31998c6f0ad703f39c879f2f1cbc9b83004f52` closes those findings; fresh round 2
review returned PASS on 2026-09-06. No additional identity/lifetime defect was
confirmed by either round.
Step 4 landed in `c52c9519259323e4795c3359605858e27a6c70af`; first fresh review
returned REJECT for missing previous-depth history, missing authoritative camera
cut invalidation, and parameter-name-based standard-material classification that
allows unsupported procedural emission. Scoped fix
`4f340213e8946d5de5a50bf1758b17b646c2e55e` adds per-set depth snapshots,
authoritative camera identity and generated-material provenance. Fresh round 2
returned REJECT: the new shared `RendererSceneRender::render_scene()` camera RID
parameters were not added to GLES3/dummy overrides. The three round-one behavioral
findings are closed. Seven focused C++ translation units passed clangd checks;
scoped diff checks passed. No full build or GPU verification was performed.
The owner authorized the remaining signature fix with "to popraw" on 2026-09-07
local time (2026-09-06 UTC). Commit
`826dc09a5a259b0f6478c364c126e911df2f79e5` adds the two camera RIDs to the GLES3
declaration/definition and dummy override. Both affected translation units passed
MSVC `/Zs` syntax compilation using root compile-database flags. Fresh bounded
review of this owner-requested correction returned PASS on 2026-09-06 UTC at
`6e539007f842b9cfd2ce845e8a99636af176e7f8`. The last reported Step 4 defect is
closed; this is not full build or runtime validation.
Step 5 resumed on owner instruction "zacznij kolejny krok" (2026-09-07 local,
2026-09-06 UTC), from `3e1d7ee4356c655634dba5d0f391d985e9dbf741` with the preserved
partial work. A fresh core-implementer uses `gpt-6-astra`, high effort, because
this step combines ReSTIR algorithm integration with GPU resource lifetime and
dispatch synchronization. Step 5 landed in
`5f9177d425f4f1d8241c554bf4816bb12f523ff1`; fresh review returned REJECT on
2026-09-06 UTC for three confirmed surface/coverage/reprojection defects listed
below. After the requested stop, the owner authorized "popraw i kontynuuj" on
2026-09-07. Corrective commit `5a532d08b36a4d262a7d372dd50f4c81b151fefa`
implements those fixes. Fresh final round 2 review returned REJECT on 2026-09-07
for shadow-facing semantics; it confirmed the three earlier findings are closed.
The owner subsequently authorized "no to popraw te cienie" on 2026-09-07.
A bounded shadow-facing correction landed in `29a810ad370a3cd3d4519040e2973f9dbabe9d01`
from `1868d9649f83ed9606b453c7b3817f6d0d745e65`; fresh bounded review returned
PASS on 2026-09-07. The remaining reported shadow-facing defect is closed at the
source/compile boundary.
Step 6 NRD/HDR began from that
corrective commit in a separate core-implementer context (`gpt-6-astra`, high
effort for GPU lifetime, pinned SPIR-V integration and frame composition), then
paused with its unwired partial source preserved. It resumed after the shadow
correction passed review and landed in `a661887676ce3c1e1f51ecb1a44bb9c2483627e7`.
Fresh Stage 6 review is pending. Step 7 real-device validation has not started.
The frame dispatch is wired in code; real-device execution and rendered output
remain unverified.

Step 1 evidence: pinned importer completed 159 NRD SPIR-V tasks; a temporary
native GLSL reservoir/random-sampler closure passed glslangValidator Vulkan 1.2.
This did not cover the later complete RAB/DI passes. SCons built 15 imported
host translation units and linked the editor/console executables with
`platform=windows target=editor accesskit=no d3d12=no -j16`. The disabled optional
SDKs were missing on this machine. These are compile/link results, not GPU proof.

The independent review verified pinned upstream files and the recorded RTXDI
patch, all 159 embedded SPIR-V variants and their binding offsets, and all 15
host objects in the linked archive. It found no defects within Step 1's scope.
Full DI shaders, template and real-device dispatch remain later-stage gates.

Step 2 evidence (2026-09-06): the same Windows Vulkan editor build command
completed and linked both binaries after initialization, ownership and API
replacement. Six changed XML files parsed successfully; staged diff check passed.
This does not prove startup failure behavior or GPU rendering. The excluded owner
file `rt_test_scenes/capture.gd:28,30,31` still references removed PT properties;
it was not migrated. Step 7 uses a new owned demonstration project.

Step 3 evidence (2026-09-06): scoped staged diff check passed, and clangd checks
of the changed translation units reported no compiler diagnostics. This is not
a full compile/link or shader validation. The commit replaces camera-bounded RT
population and the 64-light packing with resident scene collection, per-viewport
current/previous light snapshots and invalidating remaps. Shared sampling is in
`shaders/raytracing/rtxdi_light_sampling_inc.glsl`. It restores bindless finalization
after geometry/material/light texture registration. Real-device correctness of
stable identities, PDFs, deformation and resource lifetime remains unverified.

Step 3 fix evidence (2026-09-06): narrow SCons shader-header generation passed;
flattened closest-hit default and ray-query-shadow variants passed glslangValidator
for Vulkan 1.3, with bindings 13–16 each declared once. The shared sampling include
now declares no descriptors. A GLSL-reserved local name found by the diagnostic
was also corrected. Changed-line clangd reported zero errors; scoped diff checks
passed. These are shader/source diagnostics, not full renderer or GPU proof.

Step 4 evidence (2026-09-06): clangd parsed both changed C++ translation units
with exit zero; static assertions lock the changed InstanceData offsets; shader
conditional nesting and scoped staged/committed diff checks passed. This is not
a full surface-shader compile or GPU verification. The commit adds the six-MRT
surface variant, two-set viewport history and motion, and visible diagnostics for
unsupported materials. It removes normal-view legacy opaque lighting/GI and
blended drawing. Fog shadows and reflection capture remain for Step 6 closure.

Commit metadata correction: Step 4 fix `4f340213e8` was executed by
`core-implementer (gpt-5.6-sol)`. Its message accidentally contains literal `\n`
sequences on one physical line, so Git does not parse the intended body/trailer.
The original message still contains the truthful identity. History was preserved;
this entry records attribution and the formatting defect without claiming it was
rewritten or that code validation covers commit-message formatting.

## Resume point

Step 5 first review target: `5f9177d425f4f1d8241c554bf4816bb12f523ff1`.
Round 1 findings at that historical target:

- P1: export an actual oriented triangle geometric normal. The gate at
  `shaders/raytracing/rtxdi_application_bridge_inc.glsl:265` consumed
  interpolated shading normal from `shaders/forward_clustered/scene_forward_clustered.glsl:1369,2905`.
  Smooth shading can admit light below the actual triangle plane.
- P1: preserve material filter/repeat/coverage sampling semantics in ray visibility
  and emissive coverage. `rtxdi_application_bridge_inc.glsl:328` used linear,
  repeat and LOD 0 regardless of the standard material; a clamped raster hole can
  therefore cast an opaque shadow. Raster authority: `scene/resources/material.cpp:702,732`.
- P2: add jitter displacement to the motion passed to SDK temporal reprojection.
  `rtxdi_di.glsl:201` forwards motion from which raster removed jitter; the sampled
  previous buffer requires `(previous_jitter - current_jitter) * 0.5 * viewport_size`.
  Raster producer: `scene_forward_clustered.glsl:2896-2904`; SDK consumer:
  `Rtxdi/DI/TemporalResampling.hlsli:64`.

The review confirmed reservoir rotation, descriptor/BDA dependencies, full masks,
PDF measures, NRD factors and resource lifetime at the reviewed source boundary.
It checked the linked-editor log and shader artifacts, but did not run GPU code.
These findings were initially reported without implementation at the owner's
requested stop; the later "popraw i kontynuuj" instruction authorizes their fix
and continuation through the remaining approved stages.

Correction evidence, 2026-09-07 at `5a532d08b3`: the raster producer exports the
oriented deformed triangle plane separately from the shading normal. Parsed
material texture metadata selects the viewport's existing sampler palette at
bindings 33–44; shared ray/emitter alpha coverage uses projected-triangle UV
gradients. The 112-byte material layout is unchanged. Jitter displacement is
added only at the RTXDI temporal consumer, preserving non-jittered NRD motion.
The patch also excludes the surface variant from the legacy motion-output block
after actual surface shader compilation exposed that collision.

Four DI compute variants and specialized/UBERSHADER surface vertex/fragment
variants compiled for Vulkan 1.3. The surface diagnostic used an empty material
code scaffold; it is not coverage of all generated standard-material variants.
The Windows editor and console linked in 34.81 seconds using
`scons platform=windows target=editor accesskit=no d3d12=no -j16`.
Evidence artifacts: `%TEMP%/rtxdi-step5-fix-glsl/compile.log` and
`%TEMP%/rtxdi-step5-fix-build-final.log`. These are compilation results only;
real-device execution remains unverified.

Final round 2 result, 2026-09-07, frozen `5a532d08b3`, cumulative
`41bcc80c4..5a532d08b3`: REJECT for one P1 shadow-facing defect.
`shaders/raytracing/rtxdi_application_bridge_inc.glsl:406–409` uses no facing-cull
flag and confirms every covered candidate. A single-sided plane hit from its
culled side therefore blocks DI even with ordinary `SHADOW_CASTING_SETTING_ON`.
`doc/classes/GeometryInstance3D.xml:97–104` distinguishes this from `DOUBLE_SIDED`;
the retained raster shadow path honors the distinction at
`forward_clustered/render_forward_clustered.cpp:521`. The required correction
must respect material-facing shadow culling and the explicit double-sided
override for mesh and MultiMesh instances; a global cull flag alone is not enough.
The two-round review stopped the prior task. The three round-one findings are
closed; the owner's subsequent "no to popraw te cienie" authorizes a separate
bounded correction of the remaining shadow-facing defect. Commit `29a810ad37`
exports material culling and the explicit double-sided override in geometry
metadata, filters candidates from the light side, and corrects expanded MultiMesh
facing for negative local determinants. The query direction is receiver-to-light,
opposite the raster shadow view. Mesh and merged/expanded MultiMesh paths carry
the metadata; material masks and alpha coverage remain in the confirmation path.
Fresh review of this separately authorized correction returned PASS. It checked
material back/front/disabled culling, the double-sided override, owner mirror
compensation and expanded/merged MultiMesh parity against frozen source and
Khronos traversal semantics. Runtime coverage remains outstanding.

Shadow-correction evidence, 2026-09-07: four DI variants passed Vulkan 1.3 GLSL
compilation; the SCons `render_raytracing.windows.editor.x86_64.obj` target passed
in 3.69 seconds. Logs are `%TEMP%/rtxdi-shadow-facing-glsl/compile.log` and
`%TEMP%/rtxdi-shadow-facing-object-build.log`. The full editor attempt was blocked
by preserved incomplete Step 6 `nrd_effect.cpp` fields/types (`spir_v` and label
`String` versus `Span<char>`), recorded in `%TEMP%/rtxdi-shadow-facing-build.log`.
No real-device facing or rendering validation has run.

Step 6 evidence, 2026-09-07 at `a661887676`: the RD adapter owns NRD RELAX
instances, pools and dispatch resources per render buffer. Frame preparation,
denoising and HDR composition are wired before sky/post. Analytic/emissive/sky
inputs use unexposed radiance; composition applies exposure and material factors
once. Retained fog consumes the exposure-independent sky bake with its per-view
energy multiplier. Legacy main-view GI resources and active reflection/VoxelGI
capture scheduling are removed. The provisional NRD compilation errors noted
above are closed by this completed stage.

The final Windows editor/console build passed in 33.44 seconds; log:
`%TEMP%/rtxdi-step6-final-build.log`. Twenty-four DI/frame and sixteen retained
fog shader variants compiled for Vulkan 1.3 under `%TEMP%/rtxdi-step6-final-glsl`.
The exact patch is `%TEMP%/rtxdi-step6.patch`. These are source/compile results,
not real-device dispatch or visual proof. Fresh Stage 6 review is pending; no
automated tests ran.

Evidence date: 2026-09-06 UTC. Frozen Step 4 final review target:
`5075e8b3fc3b19213744d0e9ac9f3648ca710359`.

The last confirmed defect was the missing two camera RID parameters in
`drivers/gles3/rasterizer_scene_gles3.h:943`, its definition at `.cpp:2388`, and
`servers/rendering/dummy/rasterizer_scene_dummy.h:163` to match
`servers/rendering/renderer_scene_render.h:325`. Non-RT gameplay remains excluded;
these implementations still have to satisfy the shared C++ interface. The owner
subsequently authorized correction `826dc09a5a`; its three-line code change and
MSVC checks close the signature mismatch, confirmed by fresh independent review.

Attribution correction for `826dc09a5a`: the actual executing role/model was
`core-implementer (gpt-5.6-sol)`, selected under the adapter read at dispatch time.
Its trailer incorrectly says `Codex (GPT-6)`. This entry corrects the execution
record without rewriting history. Concurrent policy commit `8078509d21` was
preserved and is not part of the signature fix.

Step 5 implementation is committed in new `forward_clustered/render_rtxdi.{h,cpp}`,
new `shaders/raytracing/rtxdi_di.glsl`, `rtxdi_application_bridge_inc.glsl` and
`rtxdi_light_data_inc.glsl`, with narrow DI lifetime changes in
`render_raytracing.{h,cpp}` and extraction from `rtxdi_light_sampling_inc.glsl`.
The host resource/context/config/uniform-set/dispatch code is compiled and wired
after `commit_rtxdi_surface()`. Four separate RD compute lists use SDK reservoir
indices/pitches, with explicit compute BDA dependencies and per-viewport lifetime.

All four corrected native GLSL DI variants (initial, temporal, spatial, shade)
passed glslangValidator for Vulkan 1.3 using the pinned SDK. The Windows editor
and console linked with `scons platform=windows target=editor accesskit=no
d3d12=no -j16`; the final build took 42.43 seconds. The local evidence log is
`C:/Users/lukas/AppData/Local/Temp/rtxdi-step5-build-final.log`, and the four
flattened shaders/SPIR-V files are under the adjacent `rtxdi-step5-glsl/` folder.
No template build or GPU execution was performed. Additional imported GLSL
boolean fixes are recorded in
`thirdparty/rtxdi/patches/0002-glsl-di-boolean-parameters.patch` and applied to
InitialSampling, TemporalResampling and SpatialResampling; the importer applies
the recorded patch directory. No generated shader was edited. The diagnostic
`comp.spv` was removed. These temporary diagnostics are not durable runtime proof.

Initial candidate counts are 8 local, 1 infinite, 1 environment and 0 BRDF.
BRDF ray proposals are disabled because per-light caster masks do not share the
visibility domain required by that proposal's MIS support assumption. Temporal
correction uses current AS and can exhibit transient bias. Outputs are separate
RGBA16F diffuse/specular radiance and linear hit distance, demodulated with pinned
NRD material factors and sanitized/clamped for FP16. The next stages are NRD/HDR
composition and final editor/template/real-Vulkan visual validation. NRD/HDR
implementation landed in `a661887676` after the separately authorized shadow fix
passed review. Final real-device validation has not started. No automated tests
have been run.

Intermediate builds/rendering may fail by explicit owner authorization. Final
completion requires the plan's real-device rendering gate. Automated tests are
not authorized. Preserve the owner's untracked demo/game content.

## Available validation environment

Read-only device/tool discovery on 2026-09-06 via local `vulkaninfoSDK.exe` and
PowerShell: RTX 4090, NVIDIA driver 616.64, device Vulkan 1.4.351, ray query and
acceleration structure features true, maxColorAttachments 8. These feature values
do not prove shader-format compatibility or correct renderer execution.

SCons, Python 3.14, VS18 BuildTools CMake/Ninja and Vulkan SDK 1.4.357.0
glslangValidator are available. No Godot process was running at inspection.
Step 1 rebuilt the normal editor binaries, but they do not yet contain the later
RTXDI render passes. Other existing binaries remain pre-task artifacts.

## Boundaries

The approved plan's opaque standard-PBR milestone excludes DDGI, indirect
reflections, glass/thin-leaf transmission and the future geometry DAG/voxels.
Fog still requires its existing shadow maps. No backward compatibility or
non-RT gameplay fallback is to be added.
